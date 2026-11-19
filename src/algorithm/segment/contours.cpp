// Requirements: UR-SEG-2, UR-SEG-3, UR-SEG-4, UR-ALG-6, UR-ALG-8
#include <slideio/processor/algorithm/segment/segment.hpp>

#include <slideio/processor/core/block_view.hpp>
#include <slideio/processor/core/error.hpp>

#include "detail/bag.hpp"
#include "detail/builtin_factory.hpp"
#include "detail/builtin_operations.hpp"
#include "detail/measurement_finish.hpp"
#include "detail/region_emit.hpp"
#include "detail/region_label.hpp"
#include "detail/region_trace.hpp"

#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace slideio::processor {

namespace detail {

// The values ContourParameters holds.
struct ContourValues {
    bool includeHoles = true;
    std::string targetLayer; // empty: the run's only bound layer
};

struct ContourParametersAccess {
    // Null for moved-from parameters.
    static const ContourValues* values(const ContourParameters& parameters) noexcept;
};

} // namespace detail

class ContourParameters::Impl : public detail::ContourValues {};

namespace detail {

const ContourValues* ContourParametersAccess::values(const ContourParameters& parameters) noexcept {
    return parameters.impl_.get();
}

} // namespace detail

namespace {

constexpr const char* kName = "segment.contours";

// What one apply() call allocates beyond the input and output blocks, per
// pixel of the halo'd tile, objects emitted included: the run's tile filter
// and finishing sink handle each object inside apply(), so their copies count.
// The labels are written straight into the Int32 output plane, so labelling
// takes only the union-find parents (one int32 per provisional label, at most
// one per pixel: 4 bytes per pixel), freed before tracing. Tracing and
// emission allocate each array once, at a size counted beforehand. Unlike
// foreground and background, regions of different values touch side by side:
// for N tile pixels there are at most K = N objects (a checkerboard of four
// values, every pixel its own object), at most V = 4 N vertices (a pixel
// corner has four pixels around it, each turning there at most once, or two
// diagonal ones of one region turning there twice) and at most V / 4 = N
// rings. One region alone turns at most twice at a corner, so its rings have
// at most 2 N vertices and N / 2 rings. In bytes per pixel:
// - the traced arrays: the label index (4 per label, 4), the regions (16 per
//   object, 16), the vertices (16 each, 64), the rings (24 each, 24) and their
//   order (4 each, 4): 112 at most; while tracing, also the visited flags (1)
//   and a cursor per object (4). Then the join keys, 8 per object: 120.
// - Many small objects: nothing is measured, so no intensity partials; each
//   object's own polygon, record and finished copy are emitted and freed one
//   at a time: kBytesPerTile.
// - One large object: the traced arrays and keys (at most 120), the buffer its
//   longest ring is mapped in (16 per vertex, at most 32) and its polygon (16
//   per vertex and 9 per ring, at most 36.5) while it is built ring by ring,
//   its points vector growing: old and new blocks together are at most 2.5
//   times the final size under MSVC's 1.5x growth and 3 times under
//   libstdc++'s and libc++'s doubling, 110. Afterwards the record's copies stay
//   below that, as for segment.connected_components. So at most
//   120 + 32 + 110 = 262.
// 264 covers both. The meter, which sees the core library's allocations too,
// measures at most 118 (MSVC) on the adversarial label rasters of
// test_working_set_segment.cpp: 118 on a checkerboard of four values, every
// pixel its own object, reaching the tile's edge (120 per tile pixel, the
// traced arrays and keys at their bound); about 79 on one region with an island of
// another value at every other pixel.
constexpr int64_t kBytesPerPixel = 264;
// What does not scale with the tile: one object being emitted, its record and
// the finishing sink's copy each holding up to four measurements (area and
// perimeter, and both in micrometres), plus the small arrays of a tile with
// few objects. The meter measures 1.1 KiB for one object.
constexpr int64_t kBytesPerTile = 4096;

[[noreturn]] void fail(const std::string& problem) {
    throw InvalidArgumentError((std::string(kName) + ": " + problem).c_str());
}

void checkNotMovedFrom(const void* impl) {
    if (impl == nullptr) {
        throw InvalidArgumentError("ContourParameters: the parameters were moved from");
    }
}

DataTypeSet contourTypes() {
    return DataTypeSet()
        .add(DataType::Byte)
        .add(DataType::Int8)
        .add(DataType::UInt16)
        .add(DataType::Int16)
        .add(DataType::UInt32)
        .add(DataType::Int32)
        .add(DataType::Float32);
}

template <typename T>
T load(const uint8_t* row, int64_t x) {
    T value;
    std::memcpy(&value, row + x * static_cast<int64_t>(sizeof(T)), sizeof(T));
    return value;
}

// A pixel is in a region when its sample is neither 0 nor NaN; -0 compares
// equal to 0.
template <typename T>
bool foregroundSample(T value) {
    if constexpr (std::is_floating_point_v<T>) {
        return value != 0 && !std::isnan(value);
    } else {
        return value != 0;
    }
}

// The join key of a region of value @p value, the same in every tile: the
// value itself for an integer type, its bit pattern for Float32 (distinct
// values, 0 and NaN aside, have distinct patterns).
template <typename T>
int64_t joinKeyOf(T value) {
    if constexpr (std::is_same_v<T, float>) {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return static_cast<int64_t>(bits);
    } else {
        return static_cast<int64_t>(value);
    }
}

class ContourOperation : public Operation {
public:
    explicit ContourOperation(const detail::ContourValues& p)
        : plan_(MeasurementSelection().add(MeasurementKind::Area).add(MeasurementKind::Perimeter), 0.0, 0.0,
                p.includeHoles) {
        parameters_.setBool("includeHoles", p.includeHoles);
        if (!p.targetLayer.empty()) {
            parameters_.setString("targetLayer", p.targetLayer.c_str());
        }
    }

    const char* name() const override { return kName; }
    ChannelArity arity() const override { return ChannelArity::PerChannel; }
    bool requiresUniformTypes() const override { return false; }
    DataTypeSet supportedTypes() const override { return contourTypes(); }
    Halo halo() const override { return Halo::uniform(1); }
    DataType outputType(DataType) const override { return DataType::Int32; }
    const OperationParameters& parameters() const override { return parameters_; }
    bool producesObjects() const override { return true; }
    bool objectsAreEightConnected() const override { return false; }
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
        if (in.planeCount() != 1) {
            throw InternalError((std::string(kName) + ": the input is not one plane").c_str());
        }
        if (out.planeCount() != 1 || out.planeType(0) != DataType::Int32 || out.width() != tile.width ||
            out.height() != tile.height || out.planeRowStrideBytes(0) % 4 != 0) {
            throw InternalError((std::string(kName) + ": the output is not one Int32 plane of the tile").c_str());
        }
        switch (in.planeType(0)) {
        case DataType::Byte: trace<uint8_t>(context, tile, halo); return;
        case DataType::Int8: trace<int8_t>(context, tile, halo); return;
        case DataType::UInt16: trace<uint16_t>(context, tile, halo); return;
        case DataType::Int16: trace<int16_t>(context, tile, halo); return;
        case DataType::UInt32: trace<uint32_t>(context, tile, halo); return;
        case DataType::Int32: trace<int32_t>(context, tile, halo); return;
        case DataType::Float32: trace<float>(context, tile, halo); return;
        default: break;
        }
        throw UnsupportedDataTypeError((std::string(kName) + ": the input plane has an unsupported type").c_str(),
                                       in.planeType(0), supportedTypes());
    }

private:
    // Labels each region of equal value as one connected region 1..K within
    // the tile rect, as the tracer requires, traces them and emits each keyed
    // by its value.
    template <typename T>
    void trace(OperationContext& context, const Rect64& tile, const Rect64& halo) const {
        const BlockView& in = context.input();
        BlockView& out = context.output();
        // The labels are the output plane itself.
        auto* labels = static_cast<int32_t*>(out.planeData(0));
        const int64_t stride = out.planeRowStrideBytes(0) / 4;
        const auto* base = static_cast<const uint8_t*>(in.planeData(0));
        const int64_t inStride = in.planeRowStrideBytes(0);
        const int64_t offsetX = tile.x - halo.x;
        const int64_t offsetY = tile.y - halo.y;
        const auto sample = [&](int64_t x, int64_t y) {
            return load<T>(base + (y + offsetY) * inStride, x + offsetX);
        };
        // Every pixel may start a label: its left and upper neighbours may
        // both hold other values.
        detail::labelRegions(
            labels, stride, tile.width, tile.height, false, tile.width * tile.height, kName,
            [&](int64_t x, int64_t y) { return foregroundSample(sample(x, y)); },
            [&](int64_t x, int64_t y, int64_t nx, int64_t ny) { return sample(x, y) == sample(nx, ny); });
        const detail::TracedRegions traced = detail::traceRegions(labels, tile.width, tile.height, stride, false);
        // A region's first outer ring starts at the top-left corner of its
        // first pixel, so that pixel gives its value.
        std::vector<int64_t> keys(traced.regions.size());
        for (size_t r = 0; r < keys.size(); ++r) {
            const Point64 first = *traced.ringPoints(traced.ring(static_cast<int32_t>(r), 0));
            if (labels[first.y * stride + first.x] != traced.regions[r].label) {
                throw InternalError((std::string(kName) + ": a traced ring does not start at its region").c_str());
            }
            keys[r] = joinKeyOf(sample(first.x, first.y));
        }
        detail::emitRegions(context, labels, stride, traced, in, 1, keys.data());
    }

    detail::MeasurementPlan plan_;
    OperationParameters parameters_;
};

OperationPtr contoursFromParameters(const OperationParameters& parameters) {
    detail::BagReader bag(kName, parameters);
    ContourParameters p;
    p.includeHoles(bag.getBool("includeHoles", true));
    p.targetLayer(bag.getString("targetLayer", ""));
    bag.finish();
    return makeContourExtraction(p);
}

} // namespace

ContourParameters::ContourParameters() : impl_(std::make_unique<Impl>()) {}

ContourParameters::ContourParameters(const ContourParameters& other)
    : impl_(other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr) {}

ContourParameters::ContourParameters(ContourParameters&& other) noexcept = default;

ContourParameters& ContourParameters::operator=(const ContourParameters& other) {
    if (this != &other) {
        std::unique_ptr<Impl> copy = other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr;
        impl_ = std::move(copy);
    }
    return *this;
}

ContourParameters& ContourParameters::operator=(ContourParameters&& other) noexcept = default;

ContourParameters::~ContourParameters() = default;

ContourParameters& ContourParameters::includeHoles(bool include) {
    checkNotMovedFrom(impl_.get());
    impl_->includeHoles = include;
    return *this;
}

ContourParameters& ContourParameters::targetLayer(const char* layerName) {
    checkNotMovedFrom(impl_.get());
    if (layerName == nullptr) {
        throw InvalidArgumentError("ContourParameters: parameter 'targetLayer' must not be null");
    }
    impl_->targetLayer = layerName;
    return *this;
}

OperationPtr makeContourExtraction(const ContourParameters& parameters) {
    const detail::ContourValues* p = detail::ContourParametersAccess::values(parameters);
    if (p == nullptr) {
        fail("parameter 'parameters' was moved from");
    }
    return std::make_shared<ContourOperation>(*p);
}

namespace detail {

OperationFactoryPtr makeContoursFactory() {
    return std::make_shared<BuiltinFactory>(
        &contoursFromParameters,
        "Analytic: label rasters of touching regions, holes filled by other labels or by background, and "
        "Float32 labels one ulp apart give exact counts, shared rings, areas and perimeters; objects identical "
        "to one tile at tile sizes 64, 100 and 257 with regions of different values touching across seams",
        "O(n) in the number of pixels, plus O(v) in the vertices of the traced rings; union-find with path "
        "halving, nearly linear");
}

} // namespace detail

} // namespace slideio::processor
