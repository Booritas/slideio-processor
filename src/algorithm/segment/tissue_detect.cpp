// Requirements: UR-SEG-1, UR-SEG-2, UR-SEG-3, UR-SEG-4, UR-SEG-7, UR-ALG-6, UR-ALG-8
#include <slideio/processor/algorithm/segment/segment.hpp>

#include <slideio/processor/core/block_view.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/raster_mask.hpp>

#include "detail/bag.hpp"
#include "detail/builtin_factory.hpp"
#include "detail/builtin_operations.hpp"
#include "detail/measurement_finish.hpp"
#include "detail/region_emit.hpp"
#include "detail/region_label.hpp"
#include "detail/region_trace.hpp"
#include "detail/triangle_threshold.hpp"
#include "detail/working_set.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace slideio::processor {

namespace detail {

// The values TissueDetectParameters holds.
struct TissueDetectValues {
    TissueMode mode = TissueMode::Brightfield;
    std::optional<double> saturationThreshold; // unset: 0.07; refused in fluorescence mode
    std::optional<TissueThresholdMethod> thresholdMethod; // unset: Triangle in fluorescence mode
    std::optional<double> intensityThreshold; // with Fixed only
    int64_t minimumRegionAreaPx = 0;
    int32_t closingRadiusPx = 2;
    std::string targetLayer; // empty: the run's only bound layer
};

struct TissueDetectParametersAccess {
    // Null for moved-from parameters.
    static const TissueDetectValues* values(const TissueDetectParameters& parameters) noexcept;
};

} // namespace detail

class TissueDetectParameters::Impl : public detail::TissueDetectValues {};

namespace detail {

const TissueDetectValues* TissueDetectParametersAccess::values(const TissueDetectParameters& parameters) noexcept {
    return parameters.impl_.get();
}

} // namespace detail

namespace {

constexpr const char* kName = "segment.tissue_detect";
constexpr const char* kPlaneCountMessage =
    "segment.tissue_detect: needs exactly three channels, red, green and blue; for fluorescence, set mode to "
    "Fluorescence";
constexpr const char* kFluorescencePlaneCountMessage = "segment.tissue_detect: needs at least one channel";
// The largest closing radius: a disc 255 pixels across, the largest element
// pixel.morph_close takes. Distances along a row are capped at radius + 1, so
// they fit a byte.
constexpr int32_t kMaximumRadiusPx = 127;
constexpr double kDefaultSaturationThreshold = 0.07;
// Spelled as the enumerators, in their order.
constexpr const char* kModeSpellings[] = {"Brightfield", "Fluorescence"};
constexpr const char* kMethodSpellings[] = {"Triangle", "Fixed"};

// What one apply() call allocates beyond the input and output blocks, per
// pixel of the halo'd tile, objects emitted included: the run's tile filter
// and finishing sink handle each object inside apply(), so their copies count.
// - The closing: the tissue mask, turned in place into each pixel's distance
//   along its row to the nearest tissue pixel (1), and the dilated mask,
//   turned in place into the distance to the nearest pixel outside it (1);
//   both freed before labelling: 2.
// - Labelling the closed mask of the tile, which the Byte output plane holds:
//   one int32 label per tile pixel (4), kept until the objects are emitted,
//   and the union-find parents (at most one per two pixels: 2), freed before
//   tracing.
// - Tracing and emission, as for segment.connected_components with no
//   measured plane (4-connected foreground and background): many small
//   objects take the traced arrays (56) and the visited flags (1) or a cursor
//   per object (2); one large object takes the traced arrays (at most 46),
//   its longest ring's buffer (32) and its polygon growing (110 under
//   libstdc++'s and libc++'s doubling), 188.
// So at most 4 + 188 = 192 while tracing, more than the closing's 2 or
// labelling's 6. 200 covers it. The meter, which sees the core library's
// allocations too, measures at most 66 per halo'd pixel (MSVC) on the
// adversarial masks of test_working_set_segment.cpp: a solid region with a
// one-pixel hole at every other pixel, or a checkerboard, without a closing.
constexpr int64_t kBytesPerPixel = 200;
// What does not scale with the tile: one object being emitted, its record and
// the finishing sink's copy each holding at most four measurements (`area`,
// `perimeter` and both in micrometres), plus the small arrays of a tile with
// few objects. The meter measures 1.2 KiB for one small object.
constexpr int64_t kBytesPerTile = 4096;
// The triangle method's histogram: one bin per value of the widest type it
// counts, UInt16.
constexpr int64_t kMaxTriangleBins = 65536;
// What a Triangle instance holds besides its object and its histogram: the
// parameter bag's entries, the disc's half-widths and the control block it is
// made in, with the allocator's overhead on each: the meter measures 2.6 KiB
// beyond the histogram and the object at radius 2 under MSVC; the disc's
// half-widths grow by 4 bytes a radius, up to 127.
constexpr int64_t kStateBookkeepingBytes =
    4096 + (kMaximumRadiusPx + 1) * static_cast<int64_t>(sizeof(int32_t)) + detail::kAllocatorBlockOverhead;
constexpr const char* kTriangleTypeMessage =
    "segment.tissue_detect: Triangle needs Byte or UInt16 input; for Float32 set thresholdMethod to Fixed";

[[noreturn]] void fail(const std::string& problem) {
    throw InvalidArgumentError((std::string(kName) + ": " + problem).c_str());
}

void checkNotMovedFrom(const void* impl) {
    if (impl == nullptr) {
        throw InvalidArgumentError("TissueDetectParameters: the parameters were moved from");
    }
}

template <typename T>
T load(const uint8_t* row, int64_t x) {
    T value;
    std::memcpy(&value, row + x * static_cast<int64_t>(sizeof(T)), sizeof(T));
    return value;
}

// Replaces each byte of a width x height buffer by its distance along its row
// to the nearest byte equal to @p target, capped at @p cap; bytes beyond the
// row count as farther than the cap. In place: the forward pass reads each
// byte before writing it, and the backward pass reads only distances.
void distancesAlongRows(uint8_t* buffer, int64_t width, int64_t height, uint8_t target, uint8_t cap) {
    for (int64_t y = 0; y < height; ++y) {
        uint8_t* row = buffer + y * width;
        uint8_t previous = cap;
        for (int64_t x = 0; x < width; ++x) {
            previous = row[x] == target ? uint8_t{0} : static_cast<uint8_t>(std::min<int32_t>(previous + 1, cap));
            row[x] = previous;
        }
        for (int64_t x = width - 2; x >= 0; --x) {
            row[x] = static_cast<uint8_t>(std::min<int32_t>(row[x], row[x + 1] + 1));
        }
    }
}

class TissueDetectOperation : public Operation {
public:
    explicit TissueDetectOperation(const detail::TissueDetectValues& p)
        : mode_(p.mode),
          threshold_(p.saturationThreshold.value_or(kDefaultSaturationThreshold)),
          cutoff_(p.intensityThreshold.value_or(0.0)),
          // Fixed knows its cut-off now; Triangle learns it from the whole slide.
          cutoffKnown_(p.mode == TissueMode::Fluorescence && p.thresholdMethod &&
                       *p.thresholdMethod == TissueThresholdMethod::Fixed),
          triangle_(p.mode == TissueMode::Fluorescence && !cutoffKnown_),
          radius_(p.closingRadiusPx),
          plan_(MeasurementSelection().add(MeasurementKind::Area).add(MeasurementKind::Perimeter),
                static_cast<double>(p.minimumRegionAreaPx), 0.0, true) {
        // The disc: row dy of it spans dx from -halfWidth_[|dy|] to
        // halfWidth_[|dy|], the pixels with dx^2 + dy^2 <= radius^2.
        for (int32_t dy = 0; dy <= radius_; ++dy) {
            int32_t w = 0;
            while ((w + 1) * (w + 1) + dy * dy <= radius_ * radius_) {
                ++w;
            }
            halfWidth_.push_back(w);
        }
        if (p.mode == TissueMode::Fluorescence) {
            const TissueThresholdMethod method = p.thresholdMethod.value_or(TissueThresholdMethod::Triangle);
            parameters_.setString("mode", kModeSpellings[static_cast<int32_t>(p.mode)])
                .setString("thresholdMethod", kMethodSpellings[static_cast<int32_t>(method)]);
            if (p.intensityThreshold) {
                parameters_.setDouble("intensityThreshold", *p.intensityThreshold);
            }
        } else {
            parameters_.setDouble("saturationThreshold", threshold_);
        }
        parameters_.setInt("minimumRegionAreaPx", p.minimumRegionAreaPx).setInt("closingRadiusPx", p.closingRadiusPx);
        if (!p.targetLayer.empty()) {
            parameters_.setString("targetLayer", p.targetLayer.c_str());
        }
    }

    const char* name() const override { return kName; }
    ChannelArity arity() const override { return ChannelArity::Joint; }
    bool requiresUniformTypes() const override { return true; }
    DataTypeSet supportedTypes() const override {
        return DataTypeSet().add(DataType::Byte).add(DataType::UInt16).add(DataType::Float32);
    }
    // A tile pixel's closed value depends on the dilated mask within the
    // radius, and each dilated value on the mask within the radius again; the
    // one more keeps the pixels just beyond in view.
    Halo halo() const override { return Halo::uniform(2 * radius_ + 1); }
    DataType outputType(DataType) const override { return DataType::Byte; }
    // In brightfield mode the three planes are a pixel's colour; in fluorescence
    // mode each is a stain's intensity. Validation calls this.
    int32_t outputPlaneCount(int32_t inputPlaneCount) const override {
        checkPlaneCount(inputPlaneCount);
        return 1;
    }
    const OperationParameters& parameters() const override { return parameters_; }
    bool producesObjects() const override { return true; }
    bool objectsAreEightConnected() const override { return false; }
    bool finishObject(ObjectRecord& record, const ObjectFinishContext& context) const override {
        return plan_.finish(record, context);
    }
    int64_t workingSetBytesPerLevelPixel() const override { return kBytesPerPixel; }
    // Triangle: the first accumulate() call creates the histogram, 8 bytes
    // per bin, 65536 bins at most (UInt16's), and the allocator's overhead on
    // it; the figure counts twice as many, as Otsu's counts its local copy,
    // and covers the overhead twice. accumulate() allocates nothing per pixel.
    int64_t workingSetBytesPerTile() const override {
        return triangle_ ? kBytesPerTile + 2 * kMaxTriangleBins * 8 + 2 * detail::kAllocatorBlockOverhead
                         : kBytesPerTile;
    }

    // Fluorescence with Triangle counts the whole slide's channel maximum
    // first; brightfield and Fixed need nothing beyond the tile.
    bool needsGlobalContext() const override { return triangle_; }

    // What a Triangle instance holds between calls: its histogram, at most
    // 65536 bins of 8 bytes whatever it has counted or combined, the
    // allocator's overhead on it, the instance itself and its bookkeeping.
    int64_t accumulatedStateBytes(int32_t) const override {
        if (!triangle_) {
            return 0;
        }
        return kMaxTriangleBins * 8 + detail::kAllocatorBlockOverhead +
               static_cast<int64_t>(sizeof(TissueDetectOperation)) + kStateBookkeepingBytes;
    }

    // Counts the maximum over the planes of each pixel of the tile rect (not
    // the halo) that the run's mask admits, one bin per value. The tile rect
    // lies inside the level (the grid clips it), so no pixel beyond the level,
    // read as zero, is counted.
    void accumulate(const OperationContext& context) override {
        if (!triangle_) {
            return;
        }
        const BlockView& in = context.input();
        checkPlaneCount(in.planeCount());
        const DataType type = checkInputTypes(in);
        if (type != DataType::Byte && type != DataType::UInt16) {
            throw UnsupportedDataTypeError(kTriangleTypeMessage, type,
                                           DataTypeSet().add(DataType::Byte).add(DataType::UInt16));
        }
        const size_t binCount = type == DataType::Byte ? 256 : static_cast<size_t>(kMaxTriangleBins);
        if (bins_.empty()) {
            bins_.assign(binCount, 0);
        } else if (bins_.size() != binCount) {
            fail("the input type changed between tiles");
        }
        if (type == DataType::Byte) {
            countMaximum<uint8_t>(context);
        } else {
            countMaximum<uint16_t>(context);
        }
    }

    void combine(const Operation& other) override {
        const auto* o = dynamic_cast<const TissueDetectOperation*>(&other);
        if (o == nullptr) {
            fail("cannot combine with an operation of another kind");
        }
        if (o->bins_.empty()) {
            return;
        }
        if (bins_.empty()) {
            bins_ = o->bins_;
            return;
        }
        if (bins_.size() != o->bins_.size()) {
            fail("cannot combine histograms of different sizes");
        }
        for (size_t i = 0; i < bins_.size(); ++i) {
            bins_[i] += o->bins_[i];
        }
    }

    // The cut-off is the triangle bin of the histogram; tissue is above it.
    // Nothing counted, no cut-off: nothing is tissue. The bin may be -1 for a
    // histogram rising straight to its peak from bin 0, as OpenCV's, and then
    // every counted value is above it.
    void finalizeAccumulation() override {
        if (!triangle_) {
            return;
        }
        const bool counted = std::any_of(bins_.begin(), bins_.end(), [](int64_t c) { return c > 0; });
        if (counted) {
            cutoff_ = static_cast<double>(detail::triangleThresholdBin(bins_));
            cutoffKnown_ = true;
        }
    }

    void apply(OperationContext& context) override {
        const BlockView& in = context.input();
        BlockView& out = context.output();
        const Rect64 tile = context.tileRectAtLevel();
        const Rect64 halo = context.haloRectAtLevel();
        checkPlaneCount(in.planeCount());
        if (out.planeCount() != 1 || out.planeType(0) != DataType::Byte || out.width() != tile.width ||
            out.height() != tile.height) {
            throw InternalError((std::string(kName) + ": the output is not one Byte plane of the tile").c_str());
        }
        const DataType type = checkInputTypes(in);
        close(in, out, tile.x - halo.x, tile.y - halo.y, type, context.levelRectAtLevel(), halo);

        // Label the closed tissue of the tile, as the output plane holds it.
        const auto* closed = static_cast<const uint8_t*>(out.planeData(0));
        const int64_t closedStride = out.planeRowStrideBytes(0);
        std::vector<int32_t> labels(static_cast<size_t>(tile.width * tile.height));
        // A new label needs its left neighbour in the background, so a row
        // starts at most (width + 1) / 2 of them.
        const int64_t maximumLabels = tile.height * ((tile.width + 1) / 2);
        detail::labelRegions(
            labels.data(), tile.width, tile.width, tile.height, false, maximumLabels, kName,
            [&](int64_t x, int64_t y) { return closed[y * closedStride + x] != 0; },
            [](int64_t, int64_t, int64_t, int64_t) { return true; });
        const detail::TracedRegions traced =
            detail::traceRegions(labels.data(), tile.width, tile.height, tile.width, false);
        // Nothing is measured; every region joins its seam neighbours, key 0.
        detail::emitRegions(context, labels.data(), tile.width, traced, in, in.planeCount());
    }

private:
    void checkPlaneCount(int32_t planeCount) const {
        if (mode_ == TissueMode::Fluorescence) {
            if (planeCount < 1) {
                throw ChannelArityError(kFluorescencePlaneCountMessage, planeCount);
            }
        } else if (planeCount != 3) {
            throw ChannelArityError(kPlaneCountMessage, planeCount);
        }
    }

    // The planes' one type, when it is one supportedTypes() holds.
    DataType checkInputTypes(const BlockView& in) const {
        const DataType type = in.planeType(0);
        bool uniform = true;
        for (int32_t p = 1; p < in.planeCount(); ++p) {
            uniform = uniform && in.planeType(p) == type;
        }
        if (!supportedTypes().contains(type) || !uniform) {
            throw UnsupportedDataTypeError(
                (std::string(kName) + ": the input planes are not all Byte, all UInt16 or all Float32").c_str(),
                type, supportedTypes());
        }
        return type;
    }

    // Writes the closed tissue mask of the tile into @p out, 255 or 0: the
    // mask of the halo'd context @p in, dilated then eroded by the disc. The
    // tile starts at (offsetX, offsetY) of the context.
    //
    // Exact under tiling: the context is the tile grown by 2 * radius + 1,
    // with the pixels beyond the level read as zero, so background, whether
    // the run is tiled or not. A tile pixel's closed value reads the dilated
    // mask within the radius of it, at least radius + 1 from the context's
    // edge, and each of those reads the mask within the radius again, still
    // inside the context. Rows and columns beyond the context, which the
    // dilation counts as background and the erosion as dilated, are thus
    // never within reach of a tile pixel. In fluorescence mode, where a
    // negative cut-off would make a zero tissue, the pixels of @p haloRect
    // beyond @p levelRect are made background explicitly; an empty level rect
    // (a context without a run) leaves the whole context as read.
    void close(const BlockView& in, BlockView& out, int64_t offsetX, int64_t offsetY, DataType type,
               const Rect64& levelRect, const Rect64& haloRect) const {
        const int64_t width = in.width();
        const int64_t height = in.height();
        const auto cap = static_cast<uint8_t>(radius_ + 1);
        std::vector<uint8_t> mask(static_cast<size_t>(width * height));
        if (mode_ == TissueMode::Fluorescence) {
            if (!cutoffKnown_) {
                // No cut-off yet: nothing is tissue.
                std::fill(mask.begin(), mask.end(), uint8_t{0});
            } else {
                // The context's rect inside the level, in context coordinates.
                Rect64 inside{0, 0, width, height};
                if (levelRect.width > 0 && levelRect.height > 0) {
                    const int64_t x0 = std::max(levelRect.x, haloRect.x) - haloRect.x;
                    const int64_t y0 = std::max(levelRect.y, haloRect.y) - haloRect.y;
                    const int64_t x1 = std::min(levelRect.x + levelRect.width, haloRect.x + width) - haloRect.x;
                    const int64_t y1 = std::min(levelRect.y + levelRect.height, haloRect.y + height) - haloRect.y;
                    inside = Rect64{x0, y0, std::max<int64_t>(0, x1 - x0), std::max<int64_t>(0, y1 - y0)};
                }
                if (type == DataType::Byte) {
                    maximumMask<uint8_t>(in, inside, mask.data());
                } else if (type == DataType::UInt16) {
                    maximumMask<uint16_t>(in, inside, mask.data());
                } else {
                    maximumMask<float>(in, inside, mask.data());
                }
            }
        } else if (type == DataType::Byte) {
            tissueMask<uint8_t>(in, mask.data());
        } else if (type == DataType::UInt16) {
            tissueMask<uint16_t>(in, mask.data());
        } else {
            tissueMask<float>(in, mask.data());
        }
        auto* target = static_cast<uint8_t*>(out.planeData(0));
        const int64_t targetStride = out.planeRowStrideBytes(0);
        if (radius_ == 0) {
            for (int64_t y = 0; y < out.height(); ++y) {
                const uint8_t* row = mask.data() + (y + offsetY) * width + offsetX;
                for (int64_t x = 0; x < out.width(); ++x) {
                    target[y * targetStride + x] = row[x] != 0 ? 255 : 0;
                }
            }
            return;
        }
        // Dilation: a pixel is in the dilated mask when some row dy of the
        // disc around it reaches a tissue pixel, its distance along that row
        // within the row's half-width.
        distancesAlongRows(mask.data(), width, height, 1, cap);
        std::vector<uint8_t> dilated(static_cast<size_t>(width * height));
        for (int64_t y = 0; y < height; ++y) {
            const int64_t first = std::max<int64_t>(0, y - radius_);
            const int64_t last = std::min<int64_t>(height - 1, y + radius_);
            for (int64_t x = 0; x < width; ++x) {
                uint8_t reached = 0;
                for (int64_t sy = first; sy <= last && reached == 0; ++sy) {
                    const int32_t w = halfWidth_[static_cast<size_t>(sy > y ? sy - y : y - sy)];
                    reached = mask[static_cast<size_t>(sy * width + x)] <= w ? 1 : 0;
                }
                dilated[static_cast<size_t>(y * width + x)] = reached;
            }
        }
        mask = std::vector<uint8_t>();
        // Erosion, of the tile's pixels only: a pixel stays when no row of the
        // disc around it reaches a pixel outside the dilated mask.
        distancesAlongRows(dilated.data(), width, height, 0, cap);
        for (int64_t y = 0; y < out.height(); ++y) {
            const int64_t cy = y + offsetY;
            for (int64_t x = 0; x < out.width(); ++x) {
                const int64_t cx = x + offsetX;
                bool kept = true;
                for (int64_t dy = -radius_; dy <= radius_ && kept; ++dy) {
                    const int32_t w = halfWidth_[static_cast<size_t>(dy < 0 ? -dy : dy)];
                    kept = dilated[static_cast<size_t>((cy + dy) * width + cx)] > w;
                }
                target[y * targetStride + x] = kept ? 255 : 0;
            }
        }
    }

    // Writes 1 for each tissue pixel of @p in and 0 for the others into
    // @p mask, row-major over the context. A pixel is tissue when the spread of
    // its three samples over their maximum exceeds the threshold; one whose
    // maximum is not above 0 (0, or negative or NaN in Float32) is not.
    template <typename T>
    void tissueMask(const BlockView& in, uint8_t* mask) const {
        const int64_t width = in.width();
        for (int64_t y = 0; y < in.height(); ++y) {
            const uint8_t* rows[3];
            for (int32_t p = 0; p < 3; ++p) {
                rows[p] = static_cast<const uint8_t*>(in.planeData(p)) + y * in.planeRowStrideBytes(p);
            }
            for (int64_t x = 0; x < width; ++x) {
                const T a = load<T>(rows[0], x);
                const T b = load<T>(rows[1], x);
                const T c = load<T>(rows[2], x);
                const T maximum = std::max({a, b, c});
                const T minimum = std::min({a, b, c});
                const double spread = static_cast<double>(maximum - minimum);
                mask[y * width + x] = maximum > 0 && spread / static_cast<double>(maximum) > threshold_ ? 1 : 0;
            }
        }
    }

    // The maximum over the planes of pixel @p x of @p rows, NaN samples left
    // out whatever their plane; false when every sample is NaN.
    template <typename T>
    static bool pixelMaximum(const std::vector<const uint8_t*>& rows, int64_t x, double& maximum) {
        bool found = false;
        for (const uint8_t* row : rows) {
            const auto value = static_cast<double>(load<T>(row, x));
            if (!std::isnan(value) && (!found || value > maximum)) {
                maximum = value;
                found = true;
            }
        }
        return found;
    }

    // Writes 1 for each pixel of @p in inside @p inside (context coordinates)
    // whose maximum over all its planes is above the cut-off and 0 for the
    // others into @p mask, row-major over the context. NaN samples are left
    // out of the maximum; a pixel with nothing but NaN is background.
    template <typename T>
    void maximumMask(const BlockView& in, const Rect64& inside, uint8_t* mask) const {
        const int64_t width = in.width();
        const int32_t planes = in.planeCount();
        std::fill(mask, mask + width * in.height(), uint8_t{0});
        std::vector<const uint8_t*> rows(static_cast<size_t>(planes));
        for (int64_t y = inside.y; y < inside.y + inside.height; ++y) {
            for (int32_t p = 0; p < planes; ++p) {
                rows[static_cast<size_t>(p)] =
                    static_cast<const uint8_t*>(in.planeData(p)) + y * in.planeRowStrideBytes(p);
            }
            for (int64_t x = inside.x; x < inside.x + inside.width; ++x) {
                double maximum = 0.0;
                mask[y * width + x] = pixelMaximum<T>(rows, x, maximum) && maximum > cutoff_ ? 1 : 0;
            }
        }
    }

    // Adds the maximum over the planes of each pixel of the tile rect that
    // the run's mask admits to its bin.
    template <typename T>
    void countMaximum(const OperationContext& context) {
        const BlockView& in = context.input();
        const Rect64 tile = context.tileRectAtLevel();
        const Rect64 haloRect = context.haloRectAtLevel();
        const RasterMask* admitted = context.mask();
        const int64_t offsetX = tile.x - haloRect.x;
        const int64_t offsetY = tile.y - haloRect.y;
        const int32_t planes = in.planeCount();
        std::vector<const uint8_t*> rows(static_cast<size_t>(planes));
        for (int64_t y = 0; y < tile.height; ++y) {
            for (int32_t p = 0; p < planes; ++p) {
                rows[static_cast<size_t>(p)] = static_cast<const uint8_t*>(in.planeData(p)) +
                                               (y + offsetY) * in.planeRowStrideBytes(p) +
                                               offsetX * static_cast<int64_t>(sizeof(T));
            }
            for (int64_t x = 0; x < tile.width; ++x) {
                if (admitted != nullptr && !admitted->test(Point64{tile.x + x, tile.y + y})) {
                    continue;
                }
                T maximum = load<T>(rows[0], x);
                for (int32_t p = 1; p < planes; ++p) {
                    maximum = std::max(maximum, load<T>(rows[static_cast<size_t>(p)], x));
                }
                ++bins_[static_cast<size_t>(maximum)];
            }
        }
    }

    TissueMode mode_;
    double threshold_;
    // Fluorescence: tissue is above cutoff_ once cutoffKnown_.
    double cutoff_;
    bool cutoffKnown_;
    // Fluorescence with Triangle: the cut-off comes from bins_, the whole
    // slide's histogram of the channel maximum, one bin per value.
    bool triangle_;
    std::vector<int64_t> bins_;
    int32_t radius_;
    std::vector<int32_t> halfWidth_;
    detail::MeasurementPlan plan_;
    OperationParameters parameters_;
};

OperationPtr tissueDetectFromParameters(const OperationParameters& parameters) {
    detail::BagReader bag(kName, parameters);
    TissueDetectParameters p;
    p.mode(bag.getEnum("mode", TissueMode::Brightfield, kModeSpellings, 2));
    // Absent by default, and refused when it does not fit the mode, so a
    // given key is kept apart from the default.
    if (bag.contains("saturationThreshold")) {
        p.saturationThreshold(bag.getDouble("saturationThreshold", kDefaultSaturationThreshold));
    }
    if (bag.contains("thresholdMethod")) {
        p.thresholdMethod(bag.getEnum("thresholdMethod", TissueThresholdMethod::Triangle, kMethodSpellings, 2));
    }
    if (bag.contains("intensityThreshold")) {
        p.intensityThreshold(bag.getDouble("intensityThreshold", 0.0));
    }
    p.minimumRegionAreaPx(bag.getInt("minimumRegionAreaPx", 0));
    const int64_t radius = bag.getInt("closingRadiusPx", 2);
    if (radius < 0 || radius > kMaximumRadiusPx) {
        bag.throwBadValue("closingRadiusPx", "must be an integer from 0 to 127");
    }
    p.closingRadiusPx(static_cast<int32_t>(radius));
    p.targetLayer(bag.getString("targetLayer", ""));
    bag.finish();
    return makeTissueDetect(p);
}

} // namespace

TissueDetectParameters::TissueDetectParameters() : impl_(std::make_unique<Impl>()) {}

TissueDetectParameters::TissueDetectParameters(const TissueDetectParameters& other)
    : impl_(other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr) {}

TissueDetectParameters::TissueDetectParameters(TissueDetectParameters&& other) noexcept = default;

TissueDetectParameters& TissueDetectParameters::operator=(const TissueDetectParameters& other) {
    if (this != &other) {
        std::unique_ptr<Impl> copy = other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr;
        impl_ = std::move(copy);
    }
    return *this;
}

TissueDetectParameters& TissueDetectParameters::operator=(TissueDetectParameters&& other) noexcept = default;

TissueDetectParameters::~TissueDetectParameters() = default;

TissueDetectParameters& TissueDetectParameters::mode(TissueMode mode) {
    checkNotMovedFrom(impl_.get());
    impl_->mode = mode;
    return *this;
}

TissueDetectParameters& TissueDetectParameters::thresholdMethod(TissueThresholdMethod method) {
    checkNotMovedFrom(impl_.get());
    impl_->thresholdMethod = method;
    return *this;
}

TissueDetectParameters& TissueDetectParameters::intensityThreshold(double threshold) {
    checkNotMovedFrom(impl_.get());
    impl_->intensityThreshold = threshold;
    return *this;
}

TissueDetectParameters& TissueDetectParameters::saturationThreshold(double threshold) {
    checkNotMovedFrom(impl_.get());
    impl_->saturationThreshold = threshold;
    return *this;
}

TissueDetectParameters& TissueDetectParameters::minimumRegionAreaPx(int64_t areaPx) {
    checkNotMovedFrom(impl_.get());
    impl_->minimumRegionAreaPx = areaPx;
    return *this;
}

TissueDetectParameters& TissueDetectParameters::closingRadiusPx(int32_t radiusPx) {
    checkNotMovedFrom(impl_.get());
    impl_->closingRadiusPx = radiusPx;
    return *this;
}

TissueDetectParameters& TissueDetectParameters::targetLayer(const char* layerName) {
    checkNotMovedFrom(impl_.get());
    if (layerName == nullptr) {
        throw InvalidArgumentError("TissueDetectParameters: parameter 'targetLayer' must not be null");
    }
    impl_->targetLayer = layerName;
    return *this;
}

OperationPtr makeTissueDetect(const TissueDetectParameters& parameters) {
    const detail::TissueDetectValues* p = detail::TissueDetectParametersAccess::values(parameters);
    if (p == nullptr) {
        fail("parameter 'parameters' was moved from");
    }
    if (p->mode != TissueMode::Brightfield && p->mode != TissueMode::Fluorescence) {
        fail("parameter 'mode' is not an enumerator");
    }
    if (p->thresholdMethod && *p->thresholdMethod != TissueThresholdMethod::Triangle &&
        *p->thresholdMethod != TissueThresholdMethod::Fixed) {
        fail("parameter 'thresholdMethod' is not an enumerator");
    }
    if (p->mode == TissueMode::Fluorescence) {
        if (p->saturationThreshold) {
            fail("parameter 'saturationThreshold' applies to brightfield mode only");
        }
        const bool fixed = p->thresholdMethod && *p->thresholdMethod == TissueThresholdMethod::Fixed;
        if (fixed && !p->intensityThreshold) {
            fail("parameter 'intensityThreshold' is required with the Fixed threshold method");
        }
        if (!fixed && p->intensityThreshold) {
            fail("parameter 'intensityThreshold' applies to the Fixed threshold method only");
        }
        if (p->intensityThreshold && !std::isfinite(*p->intensityThreshold)) {
            fail("parameter 'intensityThreshold' must be finite");
        }
    } else {
        if (p->thresholdMethod) {
            fail("parameter 'thresholdMethod' applies to fluorescence mode only");
        }
        if (p->intensityThreshold) {
            fail("parameter 'intensityThreshold' applies to fluorescence mode only");
        }
        const double saturation = p->saturationThreshold.value_or(kDefaultSaturationThreshold);
        if (!(saturation >= 0.0 && saturation <= 1.0)) {
            fail("parameter 'saturationThreshold' must be from 0 to 1");
        }
    }
    if (p->minimumRegionAreaPx < 0) {
        fail("parameter 'minimumRegionAreaPx' must not be negative");
    }
    if (p->closingRadiusPx < 0 || p->closingRadiusPx > kMaximumRadiusPx) {
        fail("parameter 'closingRadiusPx' must be from 0 to 127");
    }
    return std::make_shared<TissueDetectOperation>(*p);
}

namespace detail {

OperationFactoryPtr makeTissueDetectFactory() {
    return std::make_shared<BuiltinFactory>(
        &tissueDetectFromParameters,
        "Analytic: pink ellipses with one-pixel holes on grey-noise glass give the ellipses' area within 1% at "
        "level 0; a 2-pixel gap is closed at radius 2 and not at 0; at analysis level 2, areas are 16 level-0 "
        "pixels per level-2 pixel and within the staircase bound of the analytic area; objects identical to one "
        "tile at tile sizes 64, 100 and 257, Byte and UInt16 alike. The labelled corpus of SDS 06 section 6.9 is "
        "outside the repository",
        "O(n * (2r + 1)) in the pixels of the halo'd tile for the saturation and the closing, r the closing "
        "radius; then labelling by union-find with path halving, nearly linear in the tile's pixels, and O(v) "
        "in the vertices of the traced rings");
}

} // namespace detail

} // namespace slideio::processor
