// Requirements: UR-SEG-2, UR-SEG-3, UR-SEG-4, UR-SEG-5, UR-SEG-7, UR-SEG-8, UR-ALG-6, UR-ALG-8
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

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <queue>
#include <string>
#include <utility>
#include <vector>

namespace slideio::processor {

namespace detail {

// The values WatershedParameters holds.
struct WatershedValues {
    WatershedMarkers markers = WatershedMarkers::LocalMaxima;
    std::string auxiliaryName;
    int32_t minimumMarkerDistancePx = 5;
    int32_t maximumMarkerSpacingPx = 32;
    int64_t minimumAreaPx = 0;
    MeasurementSelection measurements = MeasurementSelection::standard();
    std::string targetLayer; // empty: the run's only bound layer
    std::optional<double> minimumMarkerValue; // absent: every local maximum is a marker
    WatershedUnreached unreached = WatershedUnreached::Fail;
};

struct WatershedParametersAccess {
    // Null for moved-from parameters.
    static const WatershedValues* values(const WatershedParameters& parameters) noexcept;
};

} // namespace detail

class WatershedParameters::Impl : public detail::WatershedValues {};

namespace detail {

const WatershedValues* WatershedParametersAccess::values(const WatershedParameters& parameters) noexcept {
    return parameters.impl_.get();
}

} // namespace detail

namespace {

constexpr const char* kName = "segment.watershed";
constexpr const char* const kMarkerSpellings[] = {"LocalMaxima", "AuxiliaryRaster"};
constexpr const char* const kUnreachedSpellings[] = {"Fail", "Drop"};
// The largest spacing whose halo, 2 * spacing + 1, an int32 holds.
constexpr int64_t kMaximumSpacingPx = (std::numeric_limits<int32_t>::max() - 1) / 2;
// The same for Drop, whose halo is 4 * spacing + 1.
constexpr int64_t kMaximumDropSpacingPx = (std::numeric_limits<int32_t>::max() - 1) / 4;

// A pixel's flooding state, held where its seed goes: no marker has reached
// it, or it waits in the queue. A flooded pixel holds the index of its seed.
constexpr int32_t kUnreached = -1;
constexpr int32_t kQueued = -2;

// What one apply() call allocates beyond the input and output blocks, per
// pixel of the halo'd tile, objects emitted included: the run's tile filter
// and finishing sink handle each object inside apply(), so their copies count.
// The framework multiplies this figure by the halo'd tile, which the flood
// covers whole; what scales with the tile alone is at most as much per pixel.
// In bytes per halo'd pixel:
// - Flooding: plane 0 as doubles (8) and each pixel's seed (4); then either
//   the markers' sliding-window maxima (4 per pixel, and a window as long as
//   the longer side), freed before flooding, or the queue, one entry per foreground
//   pixel (4), reserved at its final size: 16.
// - Labelling, tracing and emission keep the seeds (4), the doubles and queue
//   freed. The basins' parts are labelled straight into the Int32 output plane
//   with a union-find parent per provisional label, at most one per tile pixel
//   (4), freed before tracing. Then, as for segment.contours, whose regions
//   likewise touch side by side (with AuxiliaryRaster markers every pixel can
//   be its own basin): the traced arrays at most 112 per tile pixel and the
//   join keys 8, with the visited flags (1) and a cursor per object (4) while
//   tracing. Many small objects add one IntensityPartial (48) per object and
//   measured plane, 144 for kMaxMeasuredPlanes (three): 4 + 120 + 144 = 268.
//   One large object adds instead its longest ring's buffer (32) and its
//   polygon growing, 110 under libstdc++'s and libc++'s doubling (see
//   segment.contours): 4 + 120 + 32 + 110 = 266.
// 288 covers both. The meter, which sees the core library's allocations too,
// measures at most 249 per halo'd pixel (MSVC) on the adversarial inputs of
// test_working_set_segment.cpp, at a spacing of 2: 268.4 per tile pixel, the
// small-object bound, on anchor rasters giving every pixel its own basin, or
// a checkerboard of basins, with three measured planes; about 120 on one
// basin with an island of another at every other pixel.
constexpr int64_t kBytesPerPixel = 288;
// What does not scale with the tile: one object being emitted, its record and
// the finishing sink's copy each holding up to 28 measurements, as for
// segment.connected_components, plus the small arrays of a tile with few
// objects.
constexpr int64_t kBytesPerTile = 16384;

[[noreturn]] void fail(const std::string& problem) {
    throw InvalidArgumentError((std::string(kName) + ": " + problem).c_str());
}

// The reach failure at level pixel (x, y), the first failing pixel of the
// tile in row order. A pixel some marker reached lies in an object longer
// than the spacing. One no marker reached is ambiguous: its object may have no
// marker at all, or one further than the halo'd context shows; the message
// names both causes and the settings that rule them out. It names parameters,
// never their values.
[[noreturn]] void failReach(bool reached, int64_t x, int64_t y, bool floorSet) {
    const std::string pixel = "level pixel (" + std::to_string(x) + ", " + std::to_string(y) + ")";
    if (reached) {
        fail("the foreground at " + pixel +
             " lies further than maximumMarkerSpacingPx from its marker; set maximumMarkerSpacingPx to at least "
             "the largest width or height of an object");
    }
    fail("no marker within maximumMarkerSpacingPx reaches the foreground at " + pixel +
         ": its object has no marker, because " +
         (floorSet ? "its peak is below minimumMarkerValue or " : "") +
         "a higher object lies within minimumMarkerDistancePx, or it is wider or higher than "
         "maximumMarkerSpacingPx; on a distance transform, open the mask first with an Ellipse element of "
         "radius at least " +
         (floorSet ? "minimumMarkerValue and " : "") +
         "minimumMarkerDistancePx + 1, and set maximumMarkerSpacingPx to at least the largest width or height of "
         "an object");
}

void checkNotMovedFrom(const void* impl) {
    if (impl == nullptr) {
        throw InvalidArgumentError("WatershedParameters: the parameters were moved from");
    }
}

// A pixel is foreground when its sample is neither 0 nor NaN.
bool foreground(double value) { return value != 0.0 && !std::isnan(value); }

int64_t loadInt64(const BlockView& view, int64_t x, int64_t y) {
    int64_t value = 0;
    std::memcpy(&value,
                static_cast<const uint8_t*>(view.planeData(0)) + y * view.planeRowStrideBytes(0) +
                    x * static_cast<int64_t>(sizeof(int64_t)),
                sizeof(value));
    return value;
}

// The flood of one halo'd context: width x height pixels, index i = y * width
// + x. Pixels are ordered by value, descending, then by index, ascending: the
// index orders pixels by row, then column, as level coordinates do, so the
// order does not depend on where the context lies.
class Flood {
public:
    Flood(const std::vector<double>& values, std::vector<int32_t>& seeds, int64_t width, int64_t height)
        : values_(values), seeds_(seeds), width_(width), height_(height) {}

    // Whether pixel a comes before pixel b; both foreground.
    bool precedes(int32_t a, int32_t b) const {
        const double va = values_[static_cast<size_t>(a)];
        const double vb = values_[static_cast<size_t>(b)];
        return va > vb || (va == vb && a < b);
    }

    bool isForeground(int64_t i) const { return foreground(values_[static_cast<size_t>(i)]); }

    // Marks as markers the foreground pixels that come before every other
    // foreground pixel within @p distance (Chebyshev), among those whose whole
    // window lies inside the context: a window reaching beyond it might hold a
    // pixel that comes first, so the pixel is not decided here.
    // A pixel below @p floor, when given, is simply not a marker; it still
    // floods and counts as foreground.
    void detectMaxima(int64_t distance, const std::optional<double>& floor) {
        if (width_ <= 2 * distance || height_ <= 2 * distance) {
            return;
        }
        // Along rows, the first pixel of each window; then along columns, the
        // first of those.
        std::vector<int32_t> rowFirst(seeds_.size(), -1);
        std::vector<int32_t> window(static_cast<size_t>(std::max(width_, height_)));
        for (int64_t y = 0; y < height_; ++y) {
            const int64_t row = y * width_;
            slidingFirst(
                width_, distance, window,
                [&](int64_t x) { return isForeground(row + x) ? static_cast<int32_t>(row + x) : -1; },
                [&](int64_t x, int32_t first) { rowFirst[static_cast<size_t>(row + x)] = first; });
        }
        for (int64_t x = distance; x < width_ - distance; ++x) {
            slidingFirst(
                height_, distance, window, [&](int64_t y) { return rowFirst[static_cast<size_t>(y * width_ + x)]; },
                [&](int64_t y, int32_t first) {
                    const int64_t i = y * width_ + x;
                    if (first == i && (!floor || values_[static_cast<size_t>(i)] >= *floor)) {
                        seeds_[static_cast<size_t>(i)] = first;
                    }
                });
        }
    }

    // Floods the foreground from the marked pixels: each pixel, taken in
    // order, joins the seed of its first flooded side neighbour.
    //
    // With @p bound (Drop mode), a flooded neighbour may claim a pixel only if
    // its seed lies within @p bound (Chebyshev) of the pixel, and the pixel
    // joins the first such neighbour. A pixel none may claim stays unflooded,
    // propagates nothing, and is taken again when a later neighbour floods.
    // This gives the order of the whole-image model (DropBounded in
    // tests/algorithm/support/flood_model.cpp): the highest-precedence pixel
    // some neighbour may claim floods next. The queue holds every such pixel,
    // plus pixels that turn out to have no claimant. Whether a neighbour may
    // claim a pixel depends only on which of its neighbours are flooded, and a
    // flooded pixel keeps its seed, so a pixel popped with no claimant cannot
    // gain one until a neighbour floods, and that neighbour queues it again
    // (it is returned to kUnreached for that). Popping it changes nothing
    // else. A pixel is queued at most once at a time, so the queue never holds
    // more entries than foreground pixels.
    // Without @p bound (Fail mode) every queued pixel floods, as it always has.
    void run(std::optional<int64_t> bound = std::nullopt) {
        int64_t waiting = 0;
        for (size_t i = 0; i < seeds_.size(); ++i) {
            waiting += seeds_[i] == kUnreached && isForeground(static_cast<int64_t>(i)) ? 1 : 0;
        }
        std::vector<int32_t> storage;
        storage.reserve(static_cast<size_t>(waiting));
        const auto later = [this](int32_t a, int32_t b) { return precedes(b, a); };
        std::priority_queue<int32_t, std::vector<int32_t>, decltype(later)> queue(later, std::move(storage));
        const auto enqueueNeighbours = [&](int64_t i) {
            forNeighbours(i, [&](int64_t n) {
                if (seeds_[static_cast<size_t>(n)] == kUnreached && isForeground(n)) {
                    seeds_[static_cast<size_t>(n)] = kQueued;
                    queue.push(static_cast<int32_t>(n));
                }
            });
        };
        for (size_t i = 0; i < seeds_.size(); ++i) {
            if (seeds_[i] >= 0) {
                enqueueNeighbours(static_cast<int64_t>(i));
            }
        }
        while (!queue.empty()) {
            const int32_t pixel = queue.top();
            queue.pop();
            int32_t first = -1;
            forNeighbours(pixel, [&](int64_t n) {
                const auto neighbour = static_cast<int32_t>(n);
                if (seeds_[static_cast<size_t>(n)] >= 0 && (first < 0 || precedes(neighbour, first)) &&
                    (!bound || chebyshev(pixel, seeds_[static_cast<size_t>(n)]) <= *bound)) {
                    first = neighbour;
                }
            });
            if (first < 0) {
                // Bounded only: no flooded neighbour's seed is near enough.
                seeds_[static_cast<size_t>(pixel)] = kUnreached;
                continue;
            }
            // Unbounded, a queued pixel has a flooded neighbour: the one that queued it.
            seeds_[static_cast<size_t>(pixel)] = seeds_[static_cast<size_t>(first)];
            enqueueNeighbours(pixel);
        }
    }

private:
    int64_t chebyshev(int64_t a, int64_t b) const {
        return std::max(std::abs(a % width_ - b % width_), std::abs(a / width_ - b / width_));
    }

    template <typename Visit>
    void forNeighbours(int64_t i, Visit visit) const {
        const int64_t x = i % width_;
        const int64_t y = i / width_;
        if (y > 0) {
            visit(i - width_);
        }
        if (x > 0) {
            visit(i - 1);
        }
        if (x + 1 < width_) {
            visit(i + 1);
        }
        if (y + 1 < height_) {
            visit(i + width_);
        }
    }

    // For each k in [distance, count - distance), passes to put(k, first) the
    // pixel among item(k - distance) .. item(k + distance) that comes first,
    // or -1 when all are -1. The candidates still able to come first are kept
    // in @p positions in order, so each item is pushed and dropped once.
    template <typename Item, typename Put>
    void slidingFirst(int64_t count, int64_t distance, std::vector<int32_t>& positions, Item item, Put put) const {
        size_t head = 0;
        size_t tail = 0;
        // The window holds positions k, not pixels: their items are read back.
        for (int64_t k = 0; k < count; ++k) {
            const int32_t candidate = item(k);
            if (candidate >= 0) {
                while (tail > head && precedes(candidate, item(positions[tail - 1]))) {
                    --tail;
                }
                positions[tail++] = static_cast<int32_t>(k);
            }
            if (k >= 2 * distance) {
                const int64_t centre = k - distance;
                while (tail > head && positions[head] < centre - distance) {
                    ++head;
                }
                put(centre, tail > head ? item(positions[head]) : -1);
            }
        }
    }

    const std::vector<double>& values_;
    std::vector<int32_t>& seeds_;
    int64_t width_;
    int64_t height_;
};

class WatershedOperation : public Operation {
public:
    explicit WatershedOperation(const detail::WatershedValues& p)
        : markers_(p.markers),
          auxiliaryName_(p.auxiliaryName),
          distance_(p.minimumMarkerDistancePx),
          floor_(p.minimumMarkerValue),
          spacing_(p.maximumMarkerSpacingPx),
          unreached_(p.unreached),
          plan_(p.measurements, static_cast<double>(p.minimumAreaPx), 0.0, true) {
        parameters_.setString("markers", kMarkerSpellings[static_cast<int32_t>(p.markers)])
            .setString("auxiliaryName", p.auxiliaryName.c_str())
            .setInt("minimumMarkerDistancePx", p.minimumMarkerDistancePx)
            .setInt("maximumMarkerSpacingPx", p.maximumMarkerSpacingPx)
            .setInt("minimumAreaPx", p.minimumAreaPx)
            .setString("measurements", detail::formatMeasurementSelection(p.measurements).c_str());
        if (p.minimumMarkerValue) {
            parameters_.setDouble("minimumMarkerValue", *p.minimumMarkerValue);
        }
        if (p.unreached == WatershedUnreached::Drop) {
            parameters_.setString("unreachedForeground", kUnreachedSpellings[static_cast<int32_t>(p.unreached)]);
        }
        if (!p.targetLayer.empty()) {
            parameters_.setString("targetLayer", p.targetLayer.c_str());
        }
    }

    const char* name() const override { return kName; }
    ChannelArity arity() const override { return ChannelArity::Joint; }
    bool requiresUniformTypes() const override { return false; }
    DataTypeSet supportedTypes() const override { return DataTypeSet::all(); }
    // A tile pixel's basin depends on markers within the spacing, and their
    // detection on pixels a further minimumMarkerDistancePx (at most the
    // spacing) away; the one more keeps the pixels just beyond in view.
    // In Drop mode a kept pixel's basin can also depend on which front reaches
    // it first along paths within 2 * spacing of it, from seeds within the
    // spacing of those paths, whose detection needs the marker window again:
    // 4 * spacing + 1 (design "Tile invariance: the gate", step 4b). That is
    // an estimate, not a proof; tests/algorithm/test_watershed_model.cpp
    // checks it, and with the old halo bounded claims were tile-dependent.
    Halo halo() const override {
        return Halo::uniform(unreached_ == WatershedUnreached::Drop ? 4 * spacing_ + 1 : 2 * spacing_ + 1);
    }
    DataType outputType(DataType) const override { return DataType::Int32; }
    // Plane 0 is flooded and at most kMaxMeasuredPlanes more are measured,
    // which is what the working set is declared for; validation calls this.
    int32_t outputPlaneCount(int32_t inputPlaneCount) const override {
        detail::checkMeasuredPlaneCount(kName, inputPlaneCount);
        return 1;
    }
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
        if (in.planeCount() < 1) {
            throw InternalError((std::string(kName) + ": the input has no plane").c_str());
        }
        detail::checkMeasuredPlaneCount(kName, in.planeCount());
        if (out.planeCount() != 1 || out.planeType(0) != DataType::Int32 || out.width() != tile.width ||
            out.height() != tile.height || out.planeRowStrideBytes(0) % 4 != 0) {
            throw InternalError((std::string(kName) + ": the output is not one Int32 plane of the tile").c_str());
        }
        if (in.width() != halo.width || in.height() != halo.height) {
            throw InternalError((std::string(kName) + ": the input does not cover the halo rect").c_str());
        }
        const BlockView* anchors = nullptr;
        if (markers_ == WatershedMarkers::AuxiliaryRaster) {
            anchors = context.auxiliary(auxiliaryName_.c_str());
            if (anchors == nullptr) {
                fail("parameter 'auxiliaryName' names no bound raster");
            }
            if (anchors->planeCount() != 1 || anchors->planeType(0) != DataType::Int64) {
                fail("parameter 'auxiliaryName' names a raster that is not one Int64 plane");
            }
            if (anchors->width() != halo.width || anchors->height() != halo.height) {
                throw InternalError(
                    (std::string(kName) + ": the auxiliary raster does not cover the halo rect").c_str());
            }
        }
        const int64_t width = halo.width;
        const int64_t height = halo.height;
        // Seeds are pixel indices within the context, held as int32.
        if (width * height >= std::numeric_limits<int32_t>::max()) {
            fail("the tile has too many pixels to label");
        }
        const int64_t offsetX = tile.x - halo.x;
        const int64_t offsetY = tile.y - halo.y;

        std::vector<int32_t> seeds(static_cast<size_t>(width * height), kUnreached);
        {
            const std::vector<double> values = readPlane0(in);
            Flood flood(values, seeds, width, height);
            if (anchors == nullptr) {
                flood.detectMaxima(distance_, floor_);
            } else {
                for (int64_t y = 0; y < height; ++y) {
                    for (int64_t x = 0; x < width; ++x) {
                        const int64_t i = y * width + x;
                        if (loadInt64(*anchors, x, y) != -1 && flood.isForeground(i)) {
                            seeds[static_cast<size_t>(i)] = static_cast<int32_t>(i);
                        }
                    }
                }
            }
            flood.run(unreached_ == WatershedUnreached::Drop ? std::optional<int64_t>(spacing_) : std::nullopt);
            // The reach: every foreground pixel of the tile was flooded from a
            // seed within the spacing. Checked before anything is emitted.
            for (int64_t y = 0; y < tile.height; ++y) {
                for (int64_t x = 0; x < tile.width; ++x) {
                    const int64_t i = (y + offsetY) * width + x + offsetX;
                    if (!flood.isForeground(i)) {
                        continue;
                    }
                    const int32_t seed = seeds[static_cast<size_t>(i)];
                    if (seed < 0 || std::max(std::abs(seed % width - (x + offsetX)),
                                             std::abs(seed / width - (y + offsetY))) > spacing_) {
                        if (unreached_ == WatershedUnreached::Fail) {
                            failReach(seed >= 0, tile.x + x, tile.y + y, floor_.has_value());
                        }
                        // Drop: the pixel takes no basin, so labelling sees background.
                        seeds[static_cast<size_t>(i)] = kUnreached;
                    }
                }
            }
        }

        // A basin's identity, the same in every tile: its marker pixel in level
        // coordinates, or its anchor value.
        const auto keyOf = [&](int32_t seed) -> int64_t {
            const int64_t x = seed % width;
            const int64_t y = seed / width;
            if (anchors != nullptr) {
                return loadInt64(*anchors, x, y);
            }
            return ((halo.y + y) << 31) | (halo.x + x);
        };
        const auto seedAt = [&](int64_t x, int64_t y) {
            return seeds[static_cast<size_t>((y + offsetY) * width + x + offsetX)];
        };
        // Each basin's 4-connected parts within the tile, labelled 1..K, as the
        // tracer requires; every pixel may start a label, since neighbouring
        // basins touch side by side.
        auto* labels = static_cast<int32_t*>(out.planeData(0));
        const int64_t stride = out.planeRowStrideBytes(0) / 4;
        detail::labelRegions(
            labels, stride, tile.width, tile.height, false, tile.width * tile.height, kName,
            [&](int64_t x, int64_t y) { return seedAt(x, y) >= 0; },
            [&](int64_t x, int64_t y, int64_t nx, int64_t ny) {
                const int32_t a = seedAt(x, y);
                const int32_t b = seedAt(nx, ny);
                return a == b || keyOf(a) == keyOf(b);
            });
        const detail::TracedRegions traced = detail::traceRegions(labels, tile.width, tile.height, stride, false);
        // A region's first outer ring starts at the top-left corner of its
        // first pixel, so that pixel gives its basin.
        std::vector<int64_t> keys(traced.regions.size());
        for (size_t r = 0; r < keys.size(); ++r) {
            const Point64 first = *traced.ringPoints(traced.ring(static_cast<int32_t>(r), 0));
            if (labels[first.y * stride + first.x] != traced.regions[r].label) {
                throw InternalError((std::string(kName) + ": a traced ring does not start at its region").c_str());
            }
            keys[r] = keyOf(seedAt(first.x, first.y));
        }
        detail::emitRegions(context, labels, stride, traced, detail::measuredPlanes(context, 1, kName), 0,
                            keys.data());
    }

private:
    // Plane 0 of the halo'd input as doubles: exact for every type but 64-bit
    // integers beyond 2^53 in magnitude.
    static std::vector<double> readPlane0(const BlockView& in) {
        const int64_t width = in.width();
        const int64_t height = in.height();
        const DataType type = in.planeType(0);
        const auto* base = static_cast<const uint8_t*>(in.planeData(0));
        const int64_t inStride = in.planeRowStrideBytes(0);
        std::vector<double> values(static_cast<size_t>(width * height));
        for (int64_t y = 0; y < height; ++y) {
            const uint8_t* row = base + y * inStride;
            for (int64_t x = 0; x < width; ++x) {
                values[static_cast<size_t>(y * width + x)] = detail::sampleAsDouble(detail::readSample(row, x, type));
            }
        }
        return values;
    }

    WatershedMarkers markers_;
    std::string auxiliaryName_;
    int64_t distance_;
    std::optional<double> floor_;
    int32_t spacing_;
    WatershedUnreached unreached_;
    detail::MeasurementPlan plan_;
    OperationParameters parameters_;
};

int32_t int32Parameter(detail::BagReader& bag, const char* key, int32_t fallback) {
    const int64_t value = bag.getInt(key, fallback);
    if (value < std::numeric_limits<int32_t>::min() || value > std::numeric_limits<int32_t>::max()) {
        bag.throwBadValue(key, "is out of range");
    }
    return static_cast<int32_t>(value);
}

OperationPtr watershedFromParameters(const OperationParameters& parameters) {
    detail::BagReader bag(kName, parameters);
    WatershedParameters p;
    p.markers(bag.getEnum("markers", WatershedMarkers::LocalMaxima, kMarkerSpellings, 2));
    p.auxiliaryName(bag.getString("auxiliaryName", ""));
    p.minimumMarkerDistancePx(int32Parameter(bag, "minimumMarkerDistancePx", 5));
    p.maximumMarkerSpacingPx(int32Parameter(bag, "maximumMarkerSpacingPx", 32));
    p.minimumAreaPx(bag.getInt("minimumAreaPx", 0));
    const char* measurements = bag.getString("measurements", nullptr);
    if (measurements != nullptr) {
        MeasurementSelection selection;
        if (!detail::parseMeasurementSelection(measurements, selection)) {
            bag.throwBadValue("measurements", "is not a comma-separated list of MeasurementKind spellings");
        }
        p.measurements(selection);
    }
    if (bag.contains("minimumMarkerValue")) {
        // Finite already: OperationParameters refuses a non-finite double.
        p.minimumMarkerValue(bag.getDouble("minimumMarkerValue", 0.0));
    }
    p.unreachedForeground(bag.getEnum("unreachedForeground", WatershedUnreached::Fail, kUnreachedSpellings, 2));
    p.targetLayer(bag.getString("targetLayer", ""));
    bag.finish();
    return makeWatershed(p);
}

} // namespace

WatershedParameters::WatershedParameters() : impl_(std::make_unique<Impl>()) {}

WatershedParameters::WatershedParameters(const WatershedParameters& other)
    : impl_(other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr) {}

WatershedParameters::WatershedParameters(WatershedParameters&& other) noexcept = default;

WatershedParameters& WatershedParameters::operator=(const WatershedParameters& other) {
    if (this != &other) {
        std::unique_ptr<Impl> copy = other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr;
        impl_ = std::move(copy);
    }
    return *this;
}

WatershedParameters& WatershedParameters::operator=(WatershedParameters&& other) noexcept = default;

WatershedParameters::~WatershedParameters() = default;

WatershedParameters& WatershedParameters::markers(WatershedMarkers markers) {
    checkNotMovedFrom(impl_.get());
    impl_->markers = markers;
    return *this;
}

WatershedParameters& WatershedParameters::auxiliaryName(const char* bindingName) {
    checkNotMovedFrom(impl_.get());
    if (bindingName == nullptr) {
        throw InvalidArgumentError("WatershedParameters: parameter 'auxiliaryName' must not be null");
    }
    impl_->auxiliaryName = bindingName;
    return *this;
}

WatershedParameters& WatershedParameters::minimumMarkerDistancePx(int32_t distancePx) {
    checkNotMovedFrom(impl_.get());
    impl_->minimumMarkerDistancePx = distancePx;
    return *this;
}

WatershedParameters& WatershedParameters::maximumMarkerSpacingPx(int32_t spacingPx) {
    checkNotMovedFrom(impl_.get());
    impl_->maximumMarkerSpacingPx = spacingPx;
    return *this;
}

WatershedParameters& WatershedParameters::minimumAreaPx(int64_t areaPx) {
    checkNotMovedFrom(impl_.get());
    impl_->minimumAreaPx = areaPx;
    return *this;
}

WatershedParameters& WatershedParameters::measurements(const MeasurementSelection& selection) {
    checkNotMovedFrom(impl_.get());
    impl_->measurements = selection;
    return *this;
}

WatershedParameters& WatershedParameters::targetLayer(const char* layerName) {
    checkNotMovedFrom(impl_.get());
    if (layerName == nullptr) {
        throw InvalidArgumentError("WatershedParameters: parameter 'targetLayer' must not be null");
    }
    impl_->targetLayer = layerName;
    return *this;
}

WatershedParameters& WatershedParameters::minimumMarkerValue(double value) {
    checkNotMovedFrom(impl_.get());
    if (!std::isfinite(value)) {
        throw InvalidArgumentError("WatershedParameters: parameter 'minimumMarkerValue' must be finite");
    }
    impl_->minimumMarkerValue = value;
    return *this;
}

WatershedParameters& WatershedParameters::unreachedForeground(WatershedUnreached behaviour) {
    checkNotMovedFrom(impl_.get());
    impl_->unreached = behaviour;
    return *this;
}

OperationPtr makeWatershed(const WatershedParameters& parameters) {
    const detail::WatershedValues* p = detail::WatershedParametersAccess::values(parameters);
    if (p == nullptr) {
        fail("parameter 'parameters' was moved from");
    }
    if (p->markers != WatershedMarkers::LocalMaxima && p->markers != WatershedMarkers::AuxiliaryRaster) {
        fail("parameter 'markers' is not a WatershedMarkers");
    }
    if (p->markers == WatershedMarkers::AuxiliaryRaster && p->auxiliaryName.empty()) {
        fail("parameter 'auxiliaryName' must not be empty with AuxiliaryRaster markers");
    }
    if (p->unreached != WatershedUnreached::Fail && p->unreached != WatershedUnreached::Drop) {
        fail("parameter 'unreachedForeground' is not a WatershedUnreached");
    }
    if (p->minimumMarkerValue && p->markers != WatershedMarkers::LocalMaxima) {
        fail("parameter 'minimumMarkerValue' applies only to LocalMaxima markers");
    }
    if (p->maximumMarkerSpacingPx < 1 || p->maximumMarkerSpacingPx > kMaximumSpacingPx) {
        fail("parameter 'maximumMarkerSpacingPx' must be from 1 to 1073741823");
    }
    if (p->unreached == WatershedUnreached::Drop && p->maximumMarkerSpacingPx > kMaximumDropSpacingPx) {
        fail("parameter 'maximumMarkerSpacingPx' must be from 1 to 536870911 with unreachedForeground Drop");
    }
    if (p->minimumMarkerDistancePx < 1) {
        fail("parameter 'minimumMarkerDistancePx' must be at least 1");
    }
    if (p->minimumMarkerDistancePx > p->maximumMarkerSpacingPx) {
        fail("parameter 'minimumMarkerDistancePx' must not exceed maximumMarkerSpacingPx");
    }
    if (p->minimumAreaPx < 0) {
        fail("parameter 'minimumAreaPx' must not be negative");
    }
    return std::make_shared<WatershedOperation>(*p);
}

namespace detail {

OperationFactoryPtr makeWatershedFactory() {
    return std::make_shared<BuiltinFactory>(
        &watershedFromParameters,
        "Analytic: touching discs whose distance transform is flooded give one basin each, with areas within 5% "
        "of the disc less half the shared lens and centroids within 1 pixel; plateau markers at their "
        "topmost-leftmost pixel; anchor-raster markers; objects identical to one tile at tile sizes 64, 100 and "
        "257; a region beyond the spacing fails at every tile size",
        "O(n log n) in the pixels of the halo'd tile for the priority flood, O(n) for marker detection and "
        "labelling, plus O(v) in the vertices of the traced rings");
}

} // namespace detail

} // namespace slideio::processor
