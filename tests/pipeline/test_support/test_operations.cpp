// Requirements: UR-EXT-1, UR-EXT-2, UR-EXT-3, UR-SEG-3, UR-SEG-4
#include <pipeline/test_support/test_operations.hpp>

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/pipeline/standard_measurements.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <string>
#include <utility>

namespace slideio::processor::testing_support {

namespace {

const unsigned char* rowOf(const BlockView& view, int32_t plane, int64_t y) {
    return static_cast<const unsigned char*>(view.planeData(plane)) + y * view.planeRowStrideBytes(plane);
}

unsigned char* mutableRowOf(const BlockView& view, int32_t plane, int64_t y) {
    return static_cast<unsigned char*>(view.planeData(plane)) + y * view.planeRowStrideBytes(plane);
}

double sampleAt(const BlockView& view, int32_t plane, int64_t x, int64_t y) {
    const unsigned char* row = rowOf(view, plane, y);
    switch (view.planeType(plane)) {
    case DataType::Byte:
        return row[x];
    case DataType::UInt16: {
        uint16_t v;
        std::memcpy(&v, row + x * 2, sizeof v);
        return v;
    }
    case DataType::Float32: {
        float v;
        std::memcpy(&v, row + x * 4, sizeof v);
        return v;
    }
    default:
        throw InvalidArgumentError("test operation: unsupported plane type");
    }
}

void requireType(const char* operation, DataType type, DataTypeSet supported) {
    if (!supported.contains(type)) {
        const std::string message = std::string(operation) + " does not support " + dataTypeName(type);
        throw UnsupportedDataTypeError(message.c_str(), type, supported);
    }
}

// Where the tile starts inside the input, which spans the halo rect.
Point64 tileOffset(const OperationContext& context) {
    return Point64{context.tileRectAtLevel().x - context.haloRectAtLevel().x,
                   context.tileRectAtLevel().y - context.haloRectAtLevel().y};
}

int64_t twiceSignedArea(const std::vector<Point64>& ring) {
    int64_t total = 0;
    for (size_t i = 0; i < ring.size(); ++i) {
        const Point64& a = ring[i];
        const Point64& b = ring[(i + 1) % ring.size()];
        total += a.x * b.y - b.x * a.y;
    }
    return total;
}

// Directions in screen coordinates (y down): east, south, west, north. Turning
// right is +1.
constexpr std::array<Point64, 4> kStep = {Point64{1, 0}, Point64{0, 1}, Point64{-1, 0}, Point64{0, -1}};

// Traces the pixel-edge rings of the pixels for which inside(x, y) holds. Every
// boundary edge keeps the component on its right, so outer rings run clockwise on
// screen and holes anticlockwise. At a vertex where the component touches itself
// diagonally the walk turns right, hugging the pixel it follows: 4-connected
// pixels, 8-connected background.
template <typename Inside>
std::vector<std::vector<Point64>> traceRings(const std::vector<Point64>& pixels, Inside inside) {
    // Keyed (y, x) so the first entry is the topmost-then-leftmost vertex.
    std::map<std::pair<int64_t, int64_t>, std::array<bool, 4>> outgoing;
    auto add = [&outgoing](int64_t x, int64_t y, int dir) { outgoing[{y, x}][static_cast<size_t>(dir)] = true; };
    for (const Point64& p : pixels) {
        if (!inside(p.x, p.y - 1)) add(p.x, p.y, 0);
        if (!inside(p.x + 1, p.y)) add(p.x + 1, p.y, 1);
        if (!inside(p.x, p.y + 1)) add(p.x + 1, p.y + 1, 2);
        if (!inside(p.x - 1, p.y)) add(p.x, p.y + 1, 3);
    }
    std::vector<std::vector<Point64>> rings;
    while (!outgoing.empty()) {
        // The topmost-then-leftmost vertex of an untraced ring is never a pinch, so
        // it has exactly one outgoing edge and the walk closes there.
        const std::pair<int64_t, int64_t> startKey = outgoing.begin()->first;
        const Point64 start{startKey.second, startKey.first};
        const std::array<bool, 4>& first = outgoing.begin()->second;
        int dir = static_cast<int>(std::find(first.begin(), first.end(), true) - first.begin());
        std::vector<Point64> ring;
        Point64 v = start;
        for (;;) {
            ring.push_back(v);
            auto it = outgoing.find({v.y, v.x});
            it->second[static_cast<size_t>(dir)] = false;
            if (std::none_of(it->second.begin(), it->second.end(), [](bool b) { return b; })) {
                outgoing.erase(it);
            }
            v = Point64{v.x + kStep[static_cast<size_t>(dir)].x, v.y + kStep[static_cast<size_t>(dir)].y};
            if (v == start) {
                break;
            }
            const std::array<bool, 4>& next = outgoing.at({v.y, v.x});
            for (int turn : {1, 0, 3}) {
                const int candidate = (dir + turn) % 4;
                if (next[static_cast<size_t>(candidate)]) {
                    dir = candidate;
                    break;
                }
            }
        }
        rings.push_back(std::move(ring));
    }
    return rings;
}

} // namespace

std::vector<Point64> normaliseRing(const std::vector<Point64>& ring) {
    std::vector<Point64> kept;
    const size_t n = ring.size();
    for (size_t i = 0; i < n; ++i) {
        const Point64& prev = ring[(i + n - 1) % n];
        const Point64& cur = ring[i];
        const Point64& next = ring[(i + 1) % n];
        const int64_t cross = (cur.x - prev.x) * (next.y - cur.y) - (cur.y - prev.y) * (next.x - cur.x);
        if (cross != 0) {
            kept.push_back(cur);
        }
    }
    const auto first = std::min_element(kept.begin(), kept.end(), [](const Point64& a, const Point64& b) {
        return a.y != b.y ? a.y < b.y : a.x < b.x;
    });
    std::rotate(kept.begin(), first, kept.end());
    return kept;
}

// --- CollectingObjectSink ---------------------------------------------------------

void CollectingObjectSink::emit(const ObjectRecord& record) {
    std::lock_guard<std::mutex> lock(mutex_);
    records_.push_back(record);
}

void CollectingObjectSink::flush() {}

int64_t CollectingObjectSink::emittedCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int64_t>(records_.size());
}

std::vector<ObjectRecord> CollectingObjectSink::records() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return records_;
}

// --- ThresholdOperation -----------------------------------------------------------

ThresholdOperation::ThresholdOperation(const OperationParameters& parameters)
    : parameters_(parameters), level_(128.0) {
    int64_t asInt = 0;
    if (!parameters_.getDouble("level", level_) && parameters_.getInt("level", asInt)) {
        level_ = static_cast<double>(asInt);
    }
}

const char* ThresholdOperation::name() const { return "test.threshold"; }
ChannelArity ThresholdOperation::arity() const { return ChannelArity::PerChannel; }
bool ThresholdOperation::requiresUniformTypes() const { return false; }
DataTypeSet ThresholdOperation::supportedTypes() const {
    return DataTypeSet().add(DataType::Byte).add(DataType::Float32);
}
Halo ThresholdOperation::halo() const { return Halo::none(); }
DataType ThresholdOperation::outputType(DataType) const { return DataType::Byte; }
const OperationParameters& ThresholdOperation::parameters() const { return parameters_; }

void ThresholdOperation::apply(OperationContext& context) {
    const BlockView& in = context.input();
    BlockView& out = context.output();
    const Point64 offset = tileOffset(context);
    for (int32_t plane = 0; plane < in.planeCount(); ++plane) {
        requireType(name(), in.planeType(plane), supportedTypes());
        for (int64_t y = 0; y < out.height(); ++y) {
            unsigned char* row = mutableRowOf(out, plane, y);
            for (int64_t x = 0; x < out.width(); ++x) {
                row[x] = sampleAt(in, plane, x + offset.x, y + offset.y) >= level_ ? 255 : 0;
            }
        }
    }
}

// --- BoxMeanOperation -------------------------------------------------------------

BoxMeanOperation::BoxMeanOperation(const OperationParameters& parameters)
    : parameters_(parameters), radius_(1) {
    int64_t radius = 1;
    parameters_.getInt("radius", radius);
    if (radius < 0 || radius > 1024) {
        throw InvalidArgumentError("test.boxMean: radius must be in [0, 1024]");
    }
    radius_ = static_cast<int32_t>(radius);
}

const char* BoxMeanOperation::name() const { return "test.boxMean"; }
ChannelArity BoxMeanOperation::arity() const { return ChannelArity::PerChannel; }
bool BoxMeanOperation::requiresUniformTypes() const { return false; }
DataTypeSet BoxMeanOperation::supportedTypes() const {
    return DataTypeSet().add(DataType::Byte).add(DataType::UInt16).add(DataType::Float32);
}
Halo BoxMeanOperation::halo() const { return Halo::uniform(radius_); }
DataType BoxMeanOperation::outputType(DataType) const { return DataType::Float32; }
const OperationParameters& BoxMeanOperation::parameters() const { return parameters_; }

void BoxMeanOperation::apply(OperationContext& context) {
    const BlockView& in = context.input();
    BlockView& out = context.output();
    const Point64 offset = tileOffset(context);
    const double window = static_cast<double>((2 * radius_ + 1) * (2 * radius_ + 1));
    for (int32_t plane = 0; plane < in.planeCount(); ++plane) {
        requireType(name(), in.planeType(plane), supportedTypes());
        for (int64_t y = 0; y < out.height(); ++y) {
            unsigned char* row = mutableRowOf(out, plane, y);
            for (int64_t x = 0; x < out.width(); ++x) {
                double sum = 0;
                for (int64_t dy = -radius_; dy <= radius_; ++dy) {
                    for (int64_t dx = -radius_; dx <= radius_; ++dx) {
                        sum += sampleAt(in, plane, x + offset.x + dx, y + offset.y + dy);
                    }
                }
                const float mean = static_cast<float>(sum / window);
                std::memcpy(row + x * 4, &mean, sizeof mean);
            }
        }
    }
}

// --- ComponentsOperation ----------------------------------------------------------

ComponentsOperation::ComponentsOperation(const OperationParameters& parameters)
    : parameters_(parameters), seams_(false) {
    parameters_.getBool("seams", seams_);
}

const char* ComponentsOperation::name() const { return "test.components"; }
ChannelArity ComponentsOperation::arity() const { return ChannelArity::Joint; }
bool ComponentsOperation::requiresUniformTypes() const { return true; }
DataTypeSet ComponentsOperation::supportedTypes() const { return DataTypeSet().add(DataType::Byte); }
Halo ComponentsOperation::halo() const { return Halo::none(); }
DataType ComponentsOperation::outputType(DataType) const { return DataType::Byte; }
const OperationParameters& ComponentsOperation::parameters() const { return parameters_; }
bool ComponentsOperation::producesObjects() const { return true; }

void ComponentsOperation::apply(OperationContext& context) {
    const BlockView& in = context.input();
    if (in.planeCount() != 1) {
        throw InvalidArgumentError("test.components: needs exactly one input plane");
    }
    requireType(name(), in.planeType(0), supportedTypes());
    BlockView& out = context.output();
    const Point64 offset = tileOffset(context);
    for (int64_t y = 0; y < out.height(); ++y) {
        std::memcpy(mutableRowOf(out, 0, y), rowOf(in, 0, y + offset.y) + offset.x,
                    static_cast<size_t>(out.width()));
    }

    // Components are found in the tile alone, never in the halo around it: every
    // component is then anchored in this tile, and one that continues past the tile
    // edge is the tile-clipped piece, which abuts its neighbour's piece exactly at
    // the seam. Coordinates below are tile-local.
    const Rect64 tile = context.tileRectAtLevel();
    const int64_t width = tile.width;
    const int64_t height = tile.height;
    if (offset.x < 0 || offset.y < 0 || offset.x + width > in.width() || offset.y + height > in.height()) {
        throw InvalidArgumentError("test.components: the tile rect must lie inside the input");
    }
    const Point64 origin{tile.x, tile.y};
    auto sample = [&in, offset](int64_t x, int64_t y) { return rowOf(in, 0, y + offset.y)[x + offset.x]; };

    std::vector<int32_t> labels(static_cast<size_t>(width * height), 0);
    auto at = [width](int64_t x, int64_t y) { return static_cast<size_t>(y * width + x); };
    int32_t nextLabel = 0;
    for (int64_t sy = 0; sy < height; ++sy) {
        for (int64_t sx = 0; sx < width; ++sx) {
            if (sample(sx, sy) == 0 || labels[at(sx, sy)] != 0) {
                continue;
            }
            const int32_t label = ++nextLabel;
            std::vector<Point64> pixels;
            std::vector<Point64> stack = {Point64{sx, sy}};
            labels[at(sx, sy)] = label;
            IntensityPartial partial{0, 0.0, 0.0, 0.0, 0.0, in.planeSourceChannel(0)};
            bool touchesEdge = false;
            while (!stack.empty()) {
                const Point64 p = stack.back();
                stack.pop_back();
                pixels.push_back(p);
                const double v = sample(p.x, p.y);
                partial.minimum = partial.count == 0 ? v : std::min(partial.minimum, v);
                partial.maximum = partial.count == 0 ? v : std::max(partial.maximum, v);
                ++partial.count;
                partial.sum += v;
                partial.sumOfSquares += v * v;
                touchesEdge = touchesEdge || p.x == 0 || p.y == 0 || p.x == width - 1 || p.y == height - 1;
                for (const Point64& step : kStep) {
                    const int64_t nx = p.x + step.x;
                    const int64_t ny = p.y + step.y;
                    if (nx >= 0 && ny >= 0 && nx < width && ny < height && labels[at(nx, ny)] == 0 &&
                        sample(nx, ny) != 0) {
                        labels[at(nx, ny)] = label;
                        stack.push_back(Point64{nx, ny});
                    }
                }
            }

            auto inside = [&](int64_t x, int64_t y) {
                return x >= 0 && y >= 0 && x < width && y < height && labels[at(x, y)] == label;
            };
            std::vector<std::vector<Point64>> outer;
            std::vector<std::vector<Point64>> holes;
            for (std::vector<Point64>& ring : traceRings(pixels, inside)) {
                for (Point64& p : ring) {
                    p = context.toLevel0(Point64{p.x + origin.x, p.y + origin.y});
                }
                std::vector<Point64> normalised = normaliseRing(ring);
                (twiceSignedArea(normalised) > 0 ? outer : holes).push_back(std::move(normalised));
            }
            if (outer.size() != 1) {
                throw InternalError("test.components: a 4-connected component must have one outer ring");
            }
            std::sort(holes.begin(), holes.end(), [](const std::vector<Point64>& a, const std::vector<Point64>& b) {
                return a.front().y != b.front().y ? a.front().y < b.front().y : a.front().x < b.front().x;
            });
            Polygon polygon;
            polygon.addRing(outer.front().data(), static_cast<int64_t>(outer.front().size()), false);
            for (const std::vector<Point64>& hole : holes) {
                polygon.addRing(hole.data(), static_cast<int64_t>(hole.size()), true);
            }

            if (seams_ && touchesEdge) {
                context.emitSeamFragment(polygon, context.targetLayer(), &partial, 1);
                continue;
            }
            ObjectSink* sink = context.objectSink();
            if (sink == nullptr) {
                throw InternalError("test.components: the run supplied no object sink");
            }
            ObjectRecord record;
            record.setGeometry(polygon);
            record.setLayer(context.targetLayer());
            applyStandardMeasurements(polygon, &partial, 1, record.measurements());
            sink->emit(record);
        }
    }
}

} // namespace slideio::processor::testing_support
