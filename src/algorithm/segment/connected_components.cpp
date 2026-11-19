// Requirements: UR-SEG-2, UR-SEG-3, UR-SEG-4, UR-SEG-5, UR-SEG-6, UR-SEG-7, UR-ALG-6, UR-ALG-8
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
#include "detail/sample_codec.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <string>

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
// pixel of the halo'd tile, objects emitted included: the run's tile filter
// and finishing sink handle each object inside apply(), so their copies count.
// The labels are written straight into the Int32 output plane, so labelling
// takes only the union-find parents (one int32 per provisional label, at most
// one per two pixels: 2 bytes per pixel), freed before tracing. Tracing and
// emission allocate each array once, at a size counted beforehand. For N tile
// pixels: at most K = N / 2 objects (no two foreground pixels side by side
// belong to different ones), at most V = 2 N vertices (a pixel corner has at
// most two foreground pixels, diagonal, turning there once each or one object
// turning there twice) and at most V / 4 rings. In bytes per pixel:
// - the traced arrays: the label index (4 per label, 2), the regions (16 per
//   object, 8), the vertices (16 each, 32), the rings (24 each, 12) and their
//   order (4 each, 2): 56 at most; while tracing, also the visited flags (1)
//   or a cursor per object (2).
// - Many small objects (a 4-connected checkerboard): one IntensityPartial (48)
//   per object and measured plane, 24 per plane, 72 for kMaxMeasuredPlanes
//   (three): 56 + 72 = 128. Each object's own polygon, record and finished copy
//   are emitted and freed one at a time: kBytesPerTile.
// - One large object: the traced arrays (at most 46, with few regions), the
//   buffer its longest ring is mapped in (16 per vertex, at most 32) and its
//   polygon (16 per vertex and 9 per ring, at most 36.5) while it is built ring
//   by ring, its points vector growing: old and new blocks together are at
//   most 2.5 times the final size under MSVC's 1.5x growth and 3 times under
//   libstdc++'s and libc++'s doubling, 110. Afterwards the polygon's slack and
//   the record's exact copy (36.5), then the record and the finishing sink's
//   one copy of it (36.5 each), stay below that. The tile filter passes an
//   object whose rings are already normalised, as the tracer leaves them, on
//   without copying it. So at most 46 + 32 + 110 = 188.
// 192 covers both. The meter, which sees the core library's allocations too,
// measures at most 126 (MSVC) on the adversarial masks of
// test_working_set_segment.cpp: 126 on a 4-connected checkerboard with three
// planes and on an 8-connected checkerboard kept inside the tile, one object
// with a hole at every other pixel.
constexpr int64_t kBytesPerPixel = 192;
// What does not scale with the tile: one object being emitted, its record and
// the finishing sink's copy each holding up to 28 measurements (8 geometric and
// 12 intensity names, and 8 in micrometres) at a 48-byte entry with vector
// growth and a 32-byte name, about 10 KiB for the two; plus the small arrays
// of a tile with few objects. The meter measures 4.6 KiB for one object with
// three measured planes.
constexpr int64_t kBytesPerTile = 16384;

[[noreturn]] void fail(const std::string& problem) {
    throw InvalidArgumentError((std::string(kName) + ": " + problem).c_str());
}

void checkNotMovedFrom(const void* impl) {
    if (impl == nullptr) {
        throw InvalidArgumentError("ConnectedComponentsParameters: the parameters were moved from");
    }
}

template <typename T>
T load(const uint8_t* row, int64_t x) {
    T value;
    std::memcpy(&value, row + x * static_cast<int64_t>(sizeof(T)), sizeof(T));
    return value;
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
    // Plane 0 is segmented and at most kMaxMeasuredPlanes more are measured,
    // which is what the working set is declared for; validation calls this.
    int32_t outputPlaneCount(int32_t inputPlaneCount) const override {
        detail::checkMeasuredPlaneCount(kName, inputPlaneCount);
        return 1;
    }
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
        detail::checkMeasuredPlaneCount(kName, in.planeCount());
        if (out.planeCount() != 1 || out.planeType(0) != DataType::Int32 || out.width() != tile.width ||
            out.height() != tile.height || out.planeRowStrideBytes(0) % 4 != 0) {
            throw InternalError((std::string(kName) + ": the output is not one Int32 plane of the tile").c_str());
        }
        // The labels are the output plane itself.
        auto* labels = static_cast<int32_t*>(out.planeData(0));
        const int64_t stride = out.planeRowStrideBytes(0) / 4;
        label(in, labels, stride, tile.width, tile.height, tile.x - halo.x, tile.y - halo.y);
        const detail::TracedRegions traced =
            detail::traceRegions(labels, tile.width, tile.height, stride, eightConnected_);
        detail::emitRegions(context, labels, stride, traced, detail::measuredPlanes(context, 1, kName), 0);
    }

private:
    void label(const BlockView& in, int32_t* labels, int64_t stride, int64_t width, int64_t height,
               int64_t offsetX, int64_t offsetY) const {
        const auto* base = static_cast<const uint8_t*>(in.planeData(0));
        const int64_t inStride = in.planeRowStrideBytes(0);
        // A new label needs its left neighbour in the background, so a row
        // starts at most (width + 1) / 2 of them.
        const int64_t maximumLabels = height * ((width + 1) / 2);
        const auto run = [&](auto sampleForeground) {
            detail::labelRegions(
                labels, stride, width, height, eightConnected_, maximumLabels, kName,
                [&](int64_t x, int64_t y) { return sampleForeground(base + (y + offsetY) * inStride, x + offsetX); },
                [](int64_t, int64_t, int64_t, int64_t) { return true; });
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
