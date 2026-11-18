// Requirements: UR-SEG-2, UR-SEG-3, UR-SEG-4, UR-SEG-5, UR-SEG-7, UR-ALG-6, UR-ALG-8
#include <slideio/processor/algorithm/segment/segment.hpp>

#include <slideio/processor/core/block_view.hpp>
#include <slideio/processor/core/error.hpp>

#include "detail/bag.hpp"
#include "detail/builtin_factory.hpp"
#include "detail/builtin_operations.hpp"
#include "detail/measurement_finish.hpp"
#include "detail/region_emit.hpp"
#include "detail/region_trace.hpp"
#include "detail/sample_codec.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace slideio::processor {

namespace detail {

// The values ConnectedComponentsParameters holds.
struct ConnectedComponentsValues {
    Connectivity connectivity = Connectivity::EightConnected;
    int64_t minimumAreaPx = 0;
    int64_t maximumAreaPx = 0;
    bool extractHoles = true;
    MeasurementSelection measurements = MeasurementSelection::standard();
    std::string targetLayer; // empty: the run's only bound layer
};

struct ConnectedComponentsParametersAccess {
    // Null for moved-from parameters.
    static const ConnectedComponentsValues* values(const ConnectedComponentsParameters& parameters) noexcept;
};

} // namespace detail

class ConnectedComponentsParameters::Impl : public detail::ConnectedComponentsValues {};

namespace detail {

const ConnectedComponentsValues* ConnectedComponentsParametersAccess::values(
    const ConnectedComponentsParameters& parameters) noexcept {
    return parameters.impl_.get();
}

} // namespace detail

namespace {

constexpr const char* kName = "segment.connected_components";
constexpr const char* const kConnectivitySpellings[] = {"FourConnected", "EightConnected"};

// What one apply() call allocates beyond the input and output blocks, per
// pixel of the halo'd tile. The labels are written straight into the Int32
// output plane, so labelling takes only the union-find parents: one int32 per
// provisional label, at most one per two pixels, about 2 bytes per pixel,
// freed before tracing. The peak comes after, and is reached with the most
// objects, one per two pixels (a 4-connected checkerboard), R = N / 2 for N
// tile pixels. Per object, in bytes, under MSVC's and libstdc++'s containers:
// - while tracing: its TracedRegion (at most 80) in a vector caught mid-growth,
//   old and new blocks live, at most 3 x 80 = 240; the tracer's label index, a
//   hash-table node and buckets through a rehash, at most 80; its ring (4
//   vertices in a block of 64), the ring list (24) and the hole flag (8), 96;
//   so 416 R + N (the visited flags) <= 209 per pixel;
// - while emitting: the TracedRegion with growth slack, at most 160; its ring,
//   96; the emitter's label index, 80; one IntensityPartial (48) per measured
//   plane P; so (336 + 48 P) R = 168 + 24 P per pixel, 240 with three planes.
// One region with many vertices costs less: each copy of its rings (traced,
// level and level-0 polygons, record) holds 16 bytes per vertex, and its
// vertices are at most its boundary edges, about two per pixel. 256 covers both
// phases with up to three measured planes; each further plane may add up to 24
// bytes per pixel on a checkerboard. The meter measures at most 200 (MSVC,
// three planes) on the adversarial masks of test_working_set_segment.cpp.
constexpr int64_t kBytesPerPixel = 256;
// Allocations that do not scale with the tile: hash-table bucket arrays at
// their initial size and the per-object record, polygon and measurement set
// of the object being emitted when it is small.
constexpr int64_t kBytesPerTile = 4096;

[[noreturn]] void fail(const std::string& problem) {
    throw InvalidArgumentError((std::string(kName) + ": " + problem).c_str());
}

void checkNotMovedFrom(const void* impl) {
    if (impl == nullptr) {
        throw InvalidArgumentError("ConnectedComponentsParameters: the parameters were moved from");
    }
}

// Union-find over provisional labels 1..: the root of a set is its smallest
// label, so it is the label its first pixel received in row-major order.
class Parents {
public:
    explicit Parents(size_t capacity) : parent_(capacity + 1, 0) {}

    int32_t make() {
        const int32_t label = ++count_;
        parent_[static_cast<size_t>(label)] = label;
        return label;
    }

    int32_t find(int32_t label) {
        while (parent_[static_cast<size_t>(label)] != label) {
            const int32_t up = parent_[static_cast<size_t>(label)];
            parent_[static_cast<size_t>(label)] = parent_[static_cast<size_t>(up)];
            label = up;
        }
        return label;
    }

    // Joins the sets of a and b; returns the root of the joined set.
    int32_t unite(int32_t a, int32_t b) {
        const int32_t ra = find(a);
        const int32_t rb = find(b);
        if (ra == rb) {
            return ra;
        }
        if (ra < rb) {
            parent_[static_cast<size_t>(rb)] = ra;
            return ra;
        }
        parent_[static_cast<size_t>(ra)] = rb;
        return rb;
    }

    // Replaces every entry by its set's final label, numbered 1.. in the order
    // of the roots. Every non-root entry points to a smaller label, so
    // ascending order resolves a label after the one it points to; the final
    // labels are held negated until then.
    void resolve() {
        int32_t next = 0;
        for (int32_t label = 1; label <= count_; ++label) {
            const int32_t up = parent_[static_cast<size_t>(label)];
            parent_[static_cast<size_t>(label)] = up == label ? -(++next) : parent_[static_cast<size_t>(up)];
        }
        for (int32_t label = 1; label <= count_; ++label) {
            parent_[static_cast<size_t>(label)] = -parent_[static_cast<size_t>(label)];
        }
    }

    int32_t finalLabel(int32_t label) const { return parent_[static_cast<size_t>(label)]; }

private:
    std::vector<int32_t> parent_;
    int32_t count_ = 0;
};

template <typename T>
T load(const uint8_t* row, int64_t x) {
    T value;
    std::memcpy(&value, row + x * static_cast<int64_t>(sizeof(T)), sizeof(T));
    return value;
}

// Labels the foreground of plane 0 over the tile rect, two-pass union-find:
// writes each pixel's final label (1.. in the order regions are first met row
// by row, 0 for background) into labels, row y at labels + y * stride.
template <typename Foreground>
void labelTile(int32_t* labels, int64_t stride, int64_t width, int64_t height, bool eightConnected,
               Foreground foreground) {
    // A new label needs its left neighbour in the background, so a row starts
    // at most (width + 1) / 2 of them.
    const int64_t capacity = height * ((width + 1) / 2);
    if (capacity >= std::numeric_limits<int32_t>::max()) {
        fail("the tile has too many pixels to label");
    }
    Parents parents(static_cast<size_t>(capacity));
    for (int64_t y = 0; y < height; ++y) {
        int32_t* row = labels + y * stride;
        const int32_t* above = y > 0 ? labels + (y - 1) * stride : nullptr;
        for (int64_t x = 0; x < width; ++x) {
            if (!foreground(x, y)) {
                row[x] = 0;
                continue;
            }
            int32_t label = x > 0 ? row[x - 1] : 0;
            const auto join = [&](int32_t neighbour) {
                if (neighbour > 0) {
                    label = label == 0 ? parents.find(neighbour) : parents.unite(label, neighbour);
                }
            };
            if (above != nullptr) {
                join(above[x]);
                if (eightConnected) {
                    join(x > 0 ? above[x - 1] : 0);
                    join(x + 1 < width ? above[x + 1] : 0);
                }
            }
            row[x] = label == 0 ? parents.make() : label;
        }
    }
    parents.resolve();
    for (int64_t y = 0; y < height; ++y) {
        int32_t* row = labels + y * stride;
        for (int64_t x = 0; x < width; ++x) {
            row[x] = row[x] == 0 ? 0 : parents.finalLabel(row[x]);
        }
    }
}

// A pixel is foreground when its sample is neither 0 nor NaN.
template <typename T>
bool foregroundSample(T value) {
    if constexpr (std::numeric_limits<T>::is_integer) {
        return value != 0;
    } else {
        return value != 0 && !std::isnan(value);
    }
}

class ConnectedComponentsOperation : public Operation {
public:
    explicit ConnectedComponentsOperation(const detail::ConnectedComponentsValues& p)
        : eightConnected_(p.connectivity == Connectivity::EightConnected),
          plan_(p.measurements, static_cast<double>(p.minimumAreaPx), static_cast<double>(p.maximumAreaPx),
                p.extractHoles) {
        parameters_.setString("connectivity", kConnectivitySpellings[static_cast<int32_t>(p.connectivity)])
            .setInt("minimumAreaPx", p.minimumAreaPx)
            .setInt("maximumAreaPx", p.maximumAreaPx)
            .setBool("extractHoles", p.extractHoles)
            .setString("measurements", detail::formatMeasurementSelection(p.measurements).c_str());
        if (!p.targetLayer.empty()) {
            parameters_.setString("targetLayer", p.targetLayer.c_str());
        }
    }

    const char* name() const override { return kName; }
    ChannelArity arity() const override { return ChannelArity::Joint; }
    bool requiresUniformTypes() const override { return false; }
    DataTypeSet supportedTypes() const override { return DataTypeSet::all(); }
    Halo halo() const override { return Halo::uniform(1); }
    DataType outputType(DataType) const override { return DataType::Int32; }
    int32_t outputPlaneCount(int32_t) const override { return 1; }
    const OperationParameters& parameters() const override { return parameters_; }
    bool producesObjects() const override { return true; }
    bool objectsAreEightConnected() const override { return eightConnected_; }
    bool finishObject(ObjectRecord& record, const ObjectFinishContext& context) const override {
        return plan_.finish(record, context);
    }
    int64_t workingSetBytesPerLevelPixel() const override { return kBytesPerPixel; }
    int64_t workingSetBytesPerTile() const override { return kBytesPerTile; }

    void apply(OperationContext& context) override {
        const BlockView& in = context.input();
        BlockView& out = context.output();
        const Rect64 tile = context.tileRectAtLevel();
        const Rect64 halo = context.haloRectAtLevel();
        if (in.planeCount() < 1) {
            throw InternalError((std::string(kName) + ": the input has no plane").c_str());
        }
        if (out.planeCount() != 1 || out.planeType(0) != DataType::Int32 || out.width() != tile.width ||
            out.height() != tile.height || out.planeRowStrideBytes(0) % 4 != 0) {
            throw InternalError((std::string(kName) + ": the output is not one Int32 plane of the tile").c_str());
        }
        // The labels are the output plane itself.
        auto* labels = static_cast<int32_t*>(out.planeData(0));
        const int64_t stride = out.planeRowStrideBytes(0) / 4;
        label(in, labels, stride, tile.width, tile.height, tile.x - halo.x, tile.y - halo.y);
        const std::vector<detail::TracedRegion> regions =
            detail::traceRegions(labels, tile.width, tile.height, stride, eightConnected_);
        detail::emitRegions(context, labels, stride, regions, in, 1);
    }

private:
    void label(const BlockView& in, int32_t* labels, int64_t stride, int64_t width, int64_t height,
               int64_t offsetX, int64_t offsetY) const {
        const auto* base = static_cast<const uint8_t*>(in.planeData(0));
        const int64_t inStride = in.planeRowStrideBytes(0);
        const auto run = [&](auto sampleForeground) {
            labelTile(labels, stride, width, height, eightConnected_, [&](int64_t x, int64_t y) {
                return sampleForeground(base + (y + offsetY) * inStride, x + offsetX);
            });
        };
        const auto typed = [&](auto zero) {
            using T = decltype(zero);
            run([](const uint8_t* row, int64_t x) { return foregroundSample(load<T>(row, x)); });
        };
        switch (in.planeType(0)) {
        case DataType::Byte: typed(uint8_t{}); return;
        case DataType::Int8: typed(int8_t{}); return;
        case DataType::UInt16: typed(uint16_t{}); return;
        case DataType::Int16: typed(int16_t{}); return;
        case DataType::UInt32: typed(uint32_t{}); return;
        case DataType::Int32: typed(int32_t{}); return;
        case DataType::Int64: typed(int64_t{}); return;
        case DataType::UInt64: typed(uint64_t{}); return;
        case DataType::Float32: typed(float{}); return;
        case DataType::Float64: typed(double{}); return;
        case DataType::Float16:
            run([](const uint8_t* row, int64_t x) {
                return foregroundSample(detail::decodeBinary16(load<uint16_t>(row, x)));
            });
            return;
        default: break;
        }
        throw UnsupportedDataTypeError((std::string(kName) + ": input plane 0 has an unsupported type").c_str(),
                                       in.planeType(0), supportedTypes());
    }

    bool eightConnected_;
    detail::MeasurementPlan plan_;
    OperationParameters parameters_;
};

OperationPtr connectedComponentsFromParameters(const OperationParameters& parameters) {
    detail::BagReader bag(kName, parameters);
    ConnectedComponentsParameters p;
    p.connectivity(bag.getEnum("connectivity", Connectivity::EightConnected, kConnectivitySpellings, 2));
    p.minimumAreaPx(bag.getInt("minimumAreaPx", 0));
    p.maximumAreaPx(bag.getInt("maximumAreaPx", 0));
    p.extractHoles(bag.getBool("extractHoles", true));
    const char* measurements = bag.getString("measurements", nullptr);
    if (measurements != nullptr) {
        MeasurementSelection selection;
        if (!detail::parseMeasurementSelection(measurements, selection)) {
            bag.throwBadValue("measurements", "is not a comma-separated list of MeasurementKind spellings");
        }
        p.measurements(selection);
    }
    p.targetLayer(bag.getString("targetLayer", ""));
    bag.finish();
    return makeConnectedComponents(p);
}

} // namespace

ConnectedComponentsParameters::ConnectedComponentsParameters() : impl_(std::make_unique<Impl>()) {}

ConnectedComponentsParameters::ConnectedComponentsParameters(const ConnectedComponentsParameters& other)
    : impl_(other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr) {}

ConnectedComponentsParameters::ConnectedComponentsParameters(ConnectedComponentsParameters&& other) noexcept =
    default;

ConnectedComponentsParameters& ConnectedComponentsParameters::operator=(const ConnectedComponentsParameters& other) {
    if (this != &other) {
        std::unique_ptr<Impl> copy = other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr;
        impl_ = std::move(copy);
    }
    return *this;
}

ConnectedComponentsParameters&
ConnectedComponentsParameters::operator=(ConnectedComponentsParameters&& other) noexcept = default;

ConnectedComponentsParameters::~ConnectedComponentsParameters() = default;

ConnectedComponentsParameters& ConnectedComponentsParameters::connectivity(Connectivity connectivity) {
    checkNotMovedFrom(impl_.get());
    impl_->connectivity = connectivity;
    return *this;
}

ConnectedComponentsParameters& ConnectedComponentsParameters::minimumAreaPx(int64_t areaPx) {
    checkNotMovedFrom(impl_.get());
    impl_->minimumAreaPx = areaPx;
    return *this;
}

ConnectedComponentsParameters& ConnectedComponentsParameters::maximumAreaPx(int64_t areaPx) {
    checkNotMovedFrom(impl_.get());
    impl_->maximumAreaPx = areaPx;
    return *this;
}

ConnectedComponentsParameters& ConnectedComponentsParameters::extractHoles(bool extract) {
    checkNotMovedFrom(impl_.get());
    impl_->extractHoles = extract;
    return *this;
}

ConnectedComponentsParameters& ConnectedComponentsParameters::measurements(const MeasurementSelection& selection) {
    checkNotMovedFrom(impl_.get());
    impl_->measurements = selection;
    return *this;
}

ConnectedComponentsParameters& ConnectedComponentsParameters::targetLayer(const char* layerName) {
    checkNotMovedFrom(impl_.get());
    if (layerName == nullptr) {
        throw InvalidArgumentError("ConnectedComponentsParameters: parameter 'targetLayer' must not be null");
    }
    impl_->targetLayer = layerName;
    return *this;
}

OperationPtr makeConnectedComponents(const ConnectedComponentsParameters& parameters) {
    const detail::ConnectedComponentsValues* p = detail::ConnectedComponentsParametersAccess::values(parameters);
    if (p == nullptr) {
        fail("parameter 'parameters' was moved from");
    }
    if (p->connectivity != Connectivity::FourConnected && p->connectivity != Connectivity::EightConnected) {
        fail("parameter 'connectivity' is not a Connectivity");
    }
    if (p->minimumAreaPx < 0) {
        fail("parameter 'minimumAreaPx' must not be negative");
    }
    if (p->maximumAreaPx < 0) {
        fail("parameter 'maximumAreaPx' must not be negative");
    }
    if (p->maximumAreaPx != 0 && p->maximumAreaPx < p->minimumAreaPx) {
        fail("parameter 'maximumAreaPx' must be 0 or at least minimumAreaPx");
    }
    return std::make_shared<ConnectedComponentsOperation>(*p);
}

namespace detail {

OperationFactoryPtr makeConnectedComponentsFactory() {
    return std::make_shared<BuiltinFactory>(
        &connectedComponentsFromParameters,
        "Analytic: known rectangles, rings with holes and diagonal chains give exact counts, rings, areas, "
        "perimeters, centroids and intensity statistics; objects identical to one tile at tile sizes 64, 100 "
        "and 257, intensity bit for bit for integer samples and within 1e-12 relative for Float32",
        "O(n) in the number of pixels, plus O(v) in the vertices of the traced rings; union-find with path "
        "halving, nearly linear");
}

} // namespace detail

} // namespace slideio::processor
