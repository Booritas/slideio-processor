// Requirements: UR-EXT-1, UR-EXT-2, UR-EXT-3, UR-SEG-3, UR-SEG-4
#include <pipeline/test_support/test_operations.hpp>

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/pipeline/standard_measurements.hpp>

#include <pipeline/detail/fragment_merge.hpp>

#include <algorithm>
#include <array>
#include <atomic>
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

// The engine's normalisation, so emitted fragments and merged objects share one
// implementation (the only engine-internal call the test operations make).
std::vector<Point64> normaliseRing(const std::vector<Point64>& ring) { return detail::normaliseRing(ring); }

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
    : parameters_(parameters), radius_(1), declaredHalo_(1) {
    int64_t radius = 1;
    parameters_.getInt("radius", radius);
    if (radius < 0 || radius > 1024) {
        throw InvalidArgumentError("test.boxMean: radius must be in [0, 1024]");
    }
    radius_ = static_cast<int32_t>(radius);
    int64_t declared = radius;
    parameters_.getInt("declaredHalo", declared);
    if (declared < 0 || declared > 1024) {
        throw InvalidArgumentError("test.boxMean: declaredHalo must be in [0, 1024]");
    }
    declaredHalo_ = static_cast<int32_t>(declared);
}

const char* BoxMeanOperation::name() const { return "test.boxMean"; }
ChannelArity BoxMeanOperation::arity() const { return ChannelArity::PerChannel; }
bool BoxMeanOperation::requiresUniformTypes() const { return false; }
DataTypeSet BoxMeanOperation::supportedTypes() const {
    return DataTypeSet().add(DataType::Byte).add(DataType::UInt16).add(DataType::Float32);
}
Halo BoxMeanOperation::halo() const { return Halo::uniform(declaredHalo_); }
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
                    const int64_t sy = y + offset.y + dy;
                    for (int64_t dx = -radius_; dx <= radius_; ++dx) {
                        const int64_t sx = x + offset.x + dx;
                        // Only an under-declared halo reaches outside the input.
                        if (sx >= 0 && sy >= 0 && sx < in.width() && sy < in.height()) {
                            sum += sampleAt(in, plane, sx, sy);
                        }
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
    : parameters_(parameters),
      seams_(false),
      haloScan_(false),
      halo_(0),
      rotateRings_(false),
      finishMinArea_(0.0),
      finishTag_(false) {
    parameters_.getBool("seams", seams_);
    parameters_.getBool("haloScan", haloScan_);
    parameters_.getBool("rotateRings", rotateRings_);
    parameters_.getDouble("finishMinArea", finishMinArea_);
    parameters_.getBool("finishTag", finishTag_);
    int64_t halo = 0;
    parameters_.getInt("halo", halo);
    if (halo < 0 || halo > 1024) {
        throw InvalidArgumentError("test.components: halo must be in [0, 1024]");
    }
    halo_ = static_cast<int32_t>(halo);
}

const char* ComponentsOperation::name() const { return "test.components"; }
ChannelArity ComponentsOperation::arity() const { return ChannelArity::Joint; }
bool ComponentsOperation::requiresUniformTypes() const { return false; }
DataTypeSet ComponentsOperation::supportedTypes() const {
    return DataTypeSet().add(DataType::Byte).add(DataType::Float32);
}
Halo ComponentsOperation::halo() const { return Halo::uniform(halo_); }
DataType ComponentsOperation::outputType(DataType) const { return DataType::Byte; }
const OperationParameters& ComponentsOperation::parameters() const { return parameters_; }
bool ComponentsOperation::producesObjects() const { return true; }
int32_t ComponentsOperation::outputPlaneCount(int32_t) const { return 1; }

bool ComponentsOperation::finishObject(ObjectRecord& record, const ObjectFinishContext&) const {
    Measurement area;
    if (record.measurements().find("area", area) && area.value() < finishMinArea_) {
        return false;
    }
    if (finishTag_) {
        record.measurements().set("finished", 1.0, MeasurementUnit::Dimensionless);
    }
    return true;
}

void ComponentsOperation::apply(OperationContext& context) {
    const BlockView& in = context.input();
    if (in.planeCount() != 1 && in.planeCount() != 2) {
        throw InvalidArgumentError("test.components: needs one input plane (the mask) or two (the mask, then the "
                                   "intensity)");
    }
    // The mask is Byte in both forms; Float32 is admitted only as the intensity plane.
    requireType(name(), in.planeType(0), DataTypeSet().add(DataType::Byte));
    const int32_t intensityPlane = in.planeCount() - 1;
    requireType(name(), in.planeType(intensityPlane), supportedTypes());
    BlockView& out = context.output();
    Point64 offset = tileOffset(context);

    // Components are found in the tile alone, never in the halo around it: every
    // component is then anchored in this tile, and one that continues past the tile
    // edge is the tile-clipped piece, which abuts its neighbour's piece exactly at
    // the seam. With "haloScan" they are found in the whole input instead.
    // Coordinates below are local to the scanned rect.
    const Rect64 tile = context.tileRectAtLevel();
    if (offset.x < 0 || offset.y < 0 || offset.x + tile.width > in.width() || offset.y + tile.height > in.height()) {
        throw InvalidArgumentError("test.components: the tile rect must lie inside the input");
    }
    if (out.width() != tile.width || out.height() != tile.height || out.planeCount() != 1 ||
        out.planeType(0) != DataType::Byte) {
        throw InvalidArgumentError("test.components: the output must be one Byte plane the size of the tile");
    }
    for (int64_t y = 0; y < tile.height; ++y) {
        std::memcpy(mutableRowOf(out, 0, y), rowOf(in, 0, y + offset.y) + offset.x,
                    static_cast<size_t>(tile.width));
    }

    const Rect64 scanned = haloScan_ ? context.haloRectAtLevel() : tile;
    const int64_t width = scanned.width;
    const int64_t height = scanned.height;
    const Point64 origin{scanned.x, scanned.y};
    if (haloScan_) {
        offset = Point64{0, 0};
    }
    auto sample = [&in, offset](int64_t x, int64_t y) { return rowOf(in, 0, y + offset.y)[x + offset.x]; };
    auto intensity = [&in, offset, intensityPlane](int64_t x, int64_t y) {
        return sampleAt(in, intensityPlane, x + offset.x, y + offset.y);
    };

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
            IntensityPartial partial{0, 0.0, 0.0, 0.0, 0.0, in.planeSourceChannel(intensityPlane)};
            bool touchesEdge = false;
            while (!stack.empty()) {
                const Point64 p = stack.back();
                stack.pop_back();
                pixels.push_back(p);
                const double v = intensity(p.x, p.y);
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
            if (rotateRings_) {
                // Start one pixel along the first edge: a vertex the normal form drops.
                const std::vector<Point64>& ring = outer.front();
                const Point64 a = ring[0];
                const Point64 b = ring[1];
                const Point64 step{(b.x > a.x) - (b.x < a.x), (b.y > a.y) - (b.y < a.y)};
                std::vector<Point64> rotated{Point64{a.x + step.x, a.y + step.y}};
                rotated.insert(rotated.end(), ring.begin() + 1, ring.end());
                rotated.push_back(a);
                if (rotated[0].x == b.x && rotated[0].y == b.y) {
                    rotated.erase(rotated.begin() + 1);
                }
                Polygon moved;
                moved.addRing(rotated.data(), static_cast<int64_t>(rotated.size()), false);
                for (const std::vector<Point64>& hole : holes) {
                    moved.addRing(hole.data(), static_cast<int64_t>(hole.size()), true);
                }
                polygon = std::move(moved);
            }
            ObjectRecord record;
            record.setGeometry(polygon);
            record.setLayer(context.targetLayer());
            applyStandardMeasurements(polygon, &partial, 1, record.measurements());
            sink->emit(record);
        }
    }
}

// --- Tile copies shared by the operations below ------------------------------------

namespace {

// Copies the tile part of every plane of the input into the output plane of the
// same index, converting each sample to Float32.
void copyTileAsFloat(OperationContext& context, int32_t planes) {
    const BlockView& in = context.input();
    BlockView& out = context.output();
    const Point64 offset = tileOffset(context);
    for (int32_t plane = 0; plane < planes; ++plane) {
        for (int64_t y = 0; y < out.height(); ++y) {
            unsigned char* row = mutableRowOf(out, plane, y);
            for (int64_t x = 0; x < out.width(); ++x) {
                const float v = static_cast<float>(sampleAt(in, plane, x + offset.x, y + offset.y));
                std::memcpy(row + x * 4, &v, sizeof v);
            }
        }
    }
}

} // namespace

// --- WholeLevelOperation ----------------------------------------------------------

WholeLevelOperation::WholeLevelOperation(const OperationParameters& parameters) : parameters_(parameters) {}

const char* WholeLevelOperation::name() const { return "test.wholeLevel"; }
ChannelArity WholeLevelOperation::arity() const { return ChannelArity::PerChannel; }
bool WholeLevelOperation::requiresUniformTypes() const { return false; }
DataTypeSet WholeLevelOperation::supportedTypes() const {
    return DataTypeSet().add(DataType::Byte).add(DataType::Float32);
}
Halo WholeLevelOperation::halo() const { return Halo::none(); }
DataType WholeLevelOperation::outputType(DataType) const { return DataType::Float32; }
const OperationParameters& WholeLevelOperation::parameters() const { return parameters_; }
TileRequirement WholeLevelOperation::tileRequirement() const { return TileRequirement::WholeLevel; }
int64_t WholeLevelOperation::workingSetBytesPerLevelPixel() const { return 100; }

void WholeLevelOperation::apply(OperationContext& context) {
    for (int32_t plane = 0; plane < context.input().planeCount(); ++plane) {
        requireType(name(), context.input().planeType(plane), supportedTypes());
    }
    copyTileAsFloat(context, context.input().planeCount());
}

// --- JointUniformOperation --------------------------------------------------------

JointUniformOperation::JointUniformOperation(const OperationParameters& parameters) : parameters_(parameters) {}

const char* JointUniformOperation::name() const { return "test.jointUniform"; }
ChannelArity JointUniformOperation::arity() const { return ChannelArity::Joint; }
bool JointUniformOperation::requiresUniformTypes() const { return true; }
DataTypeSet JointUniformOperation::supportedTypes() const {
    return DataTypeSet().add(DataType::Byte).add(DataType::Float32);
}
Halo JointUniformOperation::halo() const { return Halo::none(); }
DataType JointUniformOperation::outputType(DataType inputType) const { return inputType; }
const OperationParameters& JointUniformOperation::parameters() const { return parameters_; }
int32_t JointUniformOperation::outputPlaneCount(int32_t) const { return 1; }

void JointUniformOperation::apply(OperationContext& context) {
    const BlockView& in = context.input();
    BlockView& out = context.output();
    requireType(name(), in.planeType(0), supportedTypes());
    if (out.planeType(0) != in.planeType(0)) {
        throw InvalidArgumentError("test.jointUniform: the output must have the input's type");
    }
    const Point64 offset = tileOffset(context);
    const int64_t sampleBytes = dataTypeSizeBytes(in.planeType(0));
    for (int64_t y = 0; y < out.height(); ++y) {
        std::memcpy(mutableRowOf(out, 0, y), rowOf(in, 0, y + offset.y) + offset.x * sampleBytes,
                    static_cast<size_t>(out.width() * sampleBytes));
    }
}

// --- FloatOnlyOperation -----------------------------------------------------------

FloatOnlyOperation::FloatOnlyOperation(const OperationParameters& parameters) : parameters_(parameters) {}

const char* FloatOnlyOperation::name() const { return "test.floatOnly"; }
ChannelArity FloatOnlyOperation::arity() const { return ChannelArity::PerChannel; }
bool FloatOnlyOperation::requiresUniformTypes() const { return false; }
DataTypeSet FloatOnlyOperation::supportedTypes() const { return DataTypeSet().add(DataType::Float32); }
Halo FloatOnlyOperation::halo() const { return Halo::none(); }
DataType FloatOnlyOperation::outputType(DataType) const { return DataType::Float32; }
const OperationParameters& FloatOnlyOperation::parameters() const { return parameters_; }

void FloatOnlyOperation::apply(OperationContext& context) {
    for (int32_t plane = 0; plane < context.input().planeCount(); ++plane) {
        requireType(name(), context.input().planeType(plane), supportedTypes());
    }
    copyTileAsFloat(context, context.input().planeCount());
}

// --- WorkingSetOperation ----------------------------------------------------------

namespace {

std::atomic<const MemoryBudget*> observedBudget{nullptr};
std::atomic<int64_t> observedReserved{0};

} // namespace

WorkingSetOperation::WorkingSetOperation(const OperationParameters& parameters)
    : parameters_(parameters), perPixel_(2), perTile_(512) {
    parameters_.getInt("perPixel", perPixel_);
    parameters_.getInt("perTile", perTile_);
}

const char* WorkingSetOperation::name() const { return "test.workingSet"; }
ChannelArity WorkingSetOperation::arity() const { return ChannelArity::PerChannel; }
bool WorkingSetOperation::requiresUniformTypes() const { return false; }
DataTypeSet WorkingSetOperation::supportedTypes() const {
    return DataTypeSet().add(DataType::Byte).add(DataType::Float32);
}
Halo WorkingSetOperation::halo() const { return Halo::none(); }
DataType WorkingSetOperation::outputType(DataType) const { return DataType::Float32; }
const OperationParameters& WorkingSetOperation::parameters() const { return parameters_; }
int64_t WorkingSetOperation::workingSetBytesPerLevelPixel() const { return perPixel_; }
int64_t WorkingSetOperation::workingSetBytesPerTile() const { return perTile_; }

void WorkingSetOperation::observe(const MemoryBudget* budget) {
    observedReserved.store(0);
    observedBudget.store(budget);
}

int64_t WorkingSetOperation::observedReservedBytes() { return observedReserved.load(); }

void WorkingSetOperation::apply(OperationContext& context) {
    if (const MemoryBudget* budget = observedBudget.load()) {
        const int64_t reserved = budget->reservedBytes();
        int64_t seen = observedReserved.load();
        while (reserved > seen && !observedReserved.compare_exchange_weak(seen, reserved)) {
        }
    }
    for (int32_t plane = 0; plane < context.input().planeCount(); ++plane) {
        requireType(name(), context.input().planeType(plane), supportedTypes());
    }
    copyTileAsFloat(context, context.input().planeCount());
}

// --- Factories --------------------------------------------------------------------

PrototypeFactory::PrototypeFactory(Maker maker, const char* correctnessReference, const char* complexityNote)
    : maker_(maker),
      prototype_(maker(OperationParameters())),
      correctnessReference_(correctnessReference),
      complexityNote_(complexityNote) {}

OperationPtr PrototypeFactory::create(const OperationParameters& parameters) const { return maker_(parameters); }
const char* PrototypeFactory::name() const { return prototype_->name(); }
ChannelArity PrototypeFactory::arity() const { return prototype_->arity(); }
bool PrototypeFactory::requiresUniformTypes() const { return prototype_->requiresUniformTypes(); }
DataTypeSet PrototypeFactory::supportedTypes() const { return prototype_->supportedTypes(); }
Halo PrototypeFactory::halo() const { return prototype_->halo(); }
const char* PrototypeFactory::correctnessReference() const { return correctnessReference_; }
const char* PrototypeFactory::complexityNote() const { return complexityNote_; }
TileRequirement PrototypeFactory::tileRequirement() const { return prototype_->tileRequirement(); }
int64_t PrototypeFactory::workingSetBytesPerLevelPixel() const { return prototype_->workingSetBytesPerLevelPixel(); }
bool PrototypeFactory::producesObjects() const { return prototype_->producesObjects(); }
int32_t PrototypeFactory::outputPlaneCount(int32_t inputPlaneCount) const {
    return prototype_->outputPlaneCount(inputPlaneCount);
}
int64_t PrototypeFactory::workingSetBytesPerTile() const { return prototype_->workingSetBytesPerTile(); }
int64_t PrototypeFactory::accumulatedStateBytes(int32_t planeCount) const {
    return prototype_->accumulatedStateBytes(planeCount);
}

OperationFactoryPtr makeThresholdFactory() {
    return std::make_shared<PrototypeFactory>(
        [](const OperationParameters& p) -> OperationPtr { return std::make_shared<ThresholdOperation>(p); },
        "Analytic: out = 255 where in >= level, else 0", "O(n) in the number of samples");
}

OperationFactoryPtr makeBoxMeanFactory() {
    return std::make_shared<PrototypeFactory>(
        [](const OperationParameters& p) -> OperationPtr { return std::make_shared<BoxMeanOperation>(p); },
        "Analytic: the arithmetic mean of the (2r+1)^2 window", "O(n r^2) in the number of samples");
}

OperationFactoryPtr makeComponentsFactory() {
    return std::make_shared<PrototypeFactory>(
        [](const OperationParameters& p) -> OperationPtr { return std::make_shared<ComponentsOperation>(p); },
        "Definition: 4-connected components of the non-zero mask samples", "O(n) in the number of samples");
}

OperationFactoryPtr makeWholeLevelFactory() {
    return std::make_shared<PrototypeFactory>(
        [](const OperationParameters& p) -> OperationPtr { return std::make_shared<WholeLevelOperation>(p); },
        "Analytic: out = in, as Float32, over the whole level", "O(n) in the number of samples");
}

OperationFactoryPtr makeJointUniformFactory() {
    return std::make_shared<PrototypeFactory>(
        [](const OperationParameters& p) -> OperationPtr { return std::make_shared<JointUniformOperation>(p); },
        "Analytic: out = plane 0 of in", "O(n) in the number of samples");
}

OperationFactoryPtr makeFloatOnlyFactory() {
    return std::make_shared<PrototypeFactory>(
        [](const OperationParameters& p) -> OperationPtr { return std::make_shared<FloatOnlyOperation>(p); },
        "Analytic: out = in", "O(n) in the number of samples");
}

OperationFactoryPtr makeWorkingSetFactory() {
    return std::make_shared<PrototypeFactory>(
        [](const OperationParameters& p) -> OperationPtr { return std::make_shared<WorkingSetOperation>(p); },
        "Analytic: out = in, as Float32", "O(n) in the number of samples");
}

void registerTestOperations() {
    OperationCatalog& catalog = OperationCatalog::instance();
    for (const OperationFactoryPtr& factory :
         {makeThresholdFactory(), makeBoxMeanFactory(), makeComponentsFactory(), makeWholeLevelFactory(),
          makeJointUniformFactory(), makeFloatOnlyFactory(), makeWorkingSetFactory()}) {
        if (!catalog.isRegistered(factory->name())) {
            catalog.registerFactory(factory);
        }
    }
}

} // namespace slideio::processor::testing_support
