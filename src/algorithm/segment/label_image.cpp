// Requirements: UR-SEG-3, UR-SEG-4, UR-SEG-5, UR-EXT-2, UR-EXT-4
#include <slideio/processor/algorithm/segment/label_image.hpp>

#include <slideio/processor/algorithm/measure/measure.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/geometry.hpp>

#include "detail/measurement_finish.hpp"
#include "detail/region_emit.hpp"
#include "detail/region_label.hpp"
#include "detail/region_trace.hpp"
#include "detail/working_set.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace slideio::processor {

namespace {

// What one apply() call allocates beyond the input and output blocks, per pixel
// of the halo rect, objects emitted included. Labelling: the predicted plane
// (4), the region labels (4) and the union-find parents (4, one per provisional
// label, at most one per pixel), the parents freed after labelling and the
// predicted plane with them. Tracing and emission then hold the region labels
// (4), a keep flag per region (at most 1) and what segment.contours declares
// for its traced arrays, mapping buffer and largest polygon: 264 per pixel,
// labels and predictions alike being regions of different values touching side
// by side, and 8 of the 264 its join keys, which this operation does not make.
// So at most max(12, 4 + 1 + 264) = 269; 272 covers it.
constexpr int64_t kBytesPerPixel = 272;
// What does not scale with the tile: one object being emitted, its record and
// the finishing sink's copy each holding up to four measurements (area and
// perimeter, and both calibrated), as segment.contours declares, plus the
// allocator's overhead on each buffer live at the peak: the region labels, the
// keep flags, the five traced arrays, the mapping buffer, and the polygon's.
constexpr int64_t kBytesPerTile = 4096 + 16 * detail::kAllocatorBlockOverhead;

[[noreturn]] void internal(const char* operation, const char* problem) {
    throw InternalError((std::string(operation) + ": " + problem).c_str());
}

} // namespace

LabelImageOperation::LabelImageOperation() noexcept = default;

LabelImageOperation::~LabelImageOperation() = default;

ChannelArity LabelImageOperation::arity() const { return ChannelArity::Joint; }

bool LabelImageOperation::requiresUniformTypes() const { return false; }

DataTypeSet LabelImageOperation::supportedTypes() const { return DataTypeSet::all(); }

DataType LabelImageOperation::outputType(DataType) const { return DataType::Int32; }

int32_t LabelImageOperation::outputPlaneCount(int32_t) const { return 1; }

bool LabelImageOperation::producesObjects() const { return true; }

int64_t LabelImageOperation::workingSetBytesPerLevelPixel() const { return kBytesPerPixel; }

int64_t LabelImageOperation::workingSetBytesPerTile() const { return kBytesPerTile; }

bool LabelImageOperation::finishObject(ObjectRecord& record, const ObjectFinishContext& context) const {
    const detail::MeasurementPlan plan(MeasurementSelection().add(MeasurementKind::Area).add(MeasurementKind::Perimeter),
                                       0.0, 0.0, true);
    return plan.finish(record, context);
}

void LabelImageOperation::apply(OperationContext& context) {
    const char* operation = name();
    const Rect64 tile = context.tileRectAtLevel();
    const Rect64 halo = context.haloRectAtLevel();
    const Rect64 level = context.levelRectAtLevel();
    BlockView& out = context.output();
    if (out.planeCount() != 1 || out.planeType(0) != DataType::Int32 || out.width() != tile.width ||
        out.height() != tile.height || out.planeRowStrideBytes(0) % 4 != 0) {
        internal(operation, "the output is not one Int32 plane of the tile");
    }
    if (!(halo.intersected(tile) == tile)) {
        internal(operation, "the halo rect does not contain the tile");
    }
    const int64_t width = halo.width;
    const int64_t height = halo.height;
    if (width * height >= std::numeric_limits<int32_t>::max()) {
        throw InvalidArgumentError((std::string(operation) + ": the tile has too many pixels to label").c_str());
    }

    // 1. The model's labels over the halo rect, cleared beyond the level.
    std::vector<int32_t> predicted(static_cast<size_t>(width * height), 0);
    {
        Rect64 slideRect = context.haloRect();
        if (slideRect.isEmpty()) {
            slideRect = halo;
        }
        const BlockPlane plane{predicted.data(), DataType::Int32, width * 4, 0};
        BlockView labels(width, height, slideRect, context.level(), &plane, 1);
        predictLabels(context, labels);
    }
    if (!level.isEmpty()) {
        for (int64_t y = 0; y < height; ++y) {
            int32_t* row = predicted.data() + y * width;
            const int64_t levelY = halo.y + y;
            if (levelY < level.y || levelY >= level.bottom()) {
                std::fill(row, row + width, 0);
                continue;
            }
            const int64_t first = std::clamp<int64_t>(level.x - halo.x, 0, width);
            const int64_t end = std::clamp<int64_t>(level.right() - halo.x, 0, width);
            std::fill(row, row + first, 0);
            std::fill(row + end, row + width, 0);
        }
    }

    // 8. The output plane: the labels over the tile rect.
    {
        auto* base = static_cast<uint8_t*>(out.planeData(0));
        const int64_t outStride = out.planeRowStrideBytes(0);
        for (int64_t y = 0; y < tile.height; ++y) {
            const int32_t* from = predicted.data() + (tile.y - halo.y + y) * width + (tile.x - halo.x);
            std::memcpy(base + y * outStride, from, static_cast<size_t>(tile.width) * 4);
        }
    }

    // 2. Each maximal 4-connected set of one non-zero value is one region; the
    // predictions are freed once labelled.
    std::vector<int32_t> regionLabels(static_cast<size_t>(width * height));
    {
        const int32_t* p = predicted.data();
        detail::labelRegions(
            regionLabels.data(), width, width, height, false, width * height, operation,
            [p, width](int64_t x, int64_t y) { return p[y * width + x] != 0; },
            [p, width](int64_t x, int64_t y, int64_t nx, int64_t ny) {
                return p[y * width + x] == p[ny * width + nx];
            });
        std::vector<int32_t>().swap(predicted);
    }

    // 3. Their rings, in halo-buffer coordinates.
    const detail::TracedRegions traced = detail::traceRegions(regionLabels.data(), width, height, width, false);

    // 4-6. A region is the tile's when its anchor, the first vertex of its first
    // outer ring, lies in the tile rect; it is cut off when it reaches a side of
    // the halo rect inside the level.
    const bool known = !level.isEmpty();
    const bool leftInside = !known || halo.x > level.x;
    const bool topInside = !known || halo.y > level.y;
    const bool rightInside = !known || halo.right() < level.right();
    const bool bottomInside = !known || halo.bottom() < level.bottom();
    std::vector<uint8_t> chosen(traced.regions.size(), 0);
    int64_t truncated = 0;
    for (size_t r = 0; r < traced.regions.size(); ++r) {
        const detail::TracedRegion& region = traced.regions[r];
        const Point64 anchor = *traced.ringPoints(traced.ring(static_cast<int32_t>(r), 0));
        if (!tile.contains(Point64{anchor.x + halo.x, anchor.y + halo.y})) {
            continue;
        }
        bool cut = false;
        if (region.touchesEdge) {
            // A pixel on the buffer's edge puts an outer-ring vertex on that side.
            for (int32_t k = 0; k < region.ringCount && !cut; ++k) {
                const detail::TracedRing& ring = traced.ring(static_cast<int32_t>(r), k);
                if (ring.isHole) {
                    continue;
                }
                const Point64* points = traced.ringPoints(ring);
                for (int32_t i = 0; i < ring.count && !cut; ++i) {
                    cut = (leftInside && points[i].x == 0) || (rightInside && points[i].x == width) ||
                          (topInside && points[i].y == 0) || (bottomInside && points[i].y == height);
                }
            }
        }
        if (cut) {
            ++truncated;
            continue;
        }
        chosen[r] = 1;
    }
    if (truncated > 0) {
        context.countTruncatedObjects(truncated);
    }

    // 7. The others, whole, through the object sink.
    detail::emitWholeRegions(context, regionLabels.data(), width, halo, traced, chosen.data(), BlockView(), 0);
}

} // namespace slideio::processor
