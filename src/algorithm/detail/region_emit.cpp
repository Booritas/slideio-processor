// Requirements: UR-SEG-3, UR-SEG-4, UR-SEG-5, UR-IMG-2
#include "detail/region_emit.hpp"

#include "detail/sample_codec.hpp"

#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/core/object_record.hpp>
#include <slideio/processor/pipeline/standard_measurements.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace slideio::processor::detail {

namespace {

void addSample(IntensityPartial& partial, double v) {
    if (std::isnan(v)) {
        return;
    }
    partial.minimum = partial.count == 0 ? v : std::min(partial.minimum, v);
    partial.maximum = partial.count == 0 ? v : std::max(partial.maximum, v);
    ++partial.count;
    partial.sum += v;
    partial.sumOfSquares += v * v;
}

} // namespace

void emitRegions(OperationContext& context, const int32_t* labels, int64_t stride, const TracedRegions& traced,
                 const BlockView& input, int32_t firstIntensityPlane) {
    const std::vector<TracedRegion>& regions = traced.regions;
    if (regions.empty()) {
        return;
    }
    const Rect64 tile = context.tileRectAtLevel();
    const Rect64 halo = context.haloRectAtLevel();
    const int64_t offsetX = tile.x - halo.x;
    const int64_t offsetY = tile.y - halo.y;
    const int32_t first = std::max<int32_t>(firstIntensityPlane, 0);
    const int32_t planes = std::max<int32_t>(input.planeCount() - first, 0);

    // partials[region * planes + plane], in the order of the input's planes.
    std::vector<IntensityPartial> partials(regions.size() * static_cast<size_t>(planes));
    for (size_t r = 0; r < regions.size(); ++r) {
        for (int32_t p = 0; p < planes; ++p) {
            partials[r * static_cast<size_t>(planes) + static_cast<size_t>(p)] =
                IntensityPartial{0, 0.0, 0.0, 0.0, 0.0, input.planeSourceChannel(first + p)};
        }
    }
    if (planes > 0) {
        const std::vector<int32_t>& regionOfLabel = traced.regionOfLabel;
        for (int64_t y = 0; y < tile.height; ++y) {
            const int32_t* row = labels + y * stride;
            for (int64_t x = 0; x < tile.width; ++x) {
                const int32_t label = row[x];
                if (label <= 0 || static_cast<size_t>(label) >= regionOfLabel.size()) {
                    continue;
                }
                const int32_t region = regionOfLabel[static_cast<size_t>(label)];
                if (region < 0) {
                    continue;
                }
                IntensityPartial* own = &partials[static_cast<size_t>(region) * static_cast<size_t>(planes)];
                for (int32_t p = 0; p < planes; ++p) {
                    const int32_t plane = first + p;
                    const auto* inputRow = static_cast<const uint8_t*>(input.planeData(plane)) +
                                           (y + offsetY) * input.planeRowStrideBytes(plane);
                    addSample(own[p], sampleAsDouble(readSample(inputRow, x + offsetX, input.planeType(plane))));
                }
            }
        }
    }

    // One buffer, as long as the longest ring, maps every ring in turn.
    int32_t longest = 0;
    for (const TracedRing& ring : traced.rings) {
        longest = std::max(longest, ring.count);
    }
    std::vector<Point64> mapped(static_cast<size_t>(longest));
    ObjectSink* sink = context.objectSink();
    for (size_t r = 0; r < regions.size(); ++r) {
        const TracedRegion& region = regions[r];
        const IntensityPartial* own = planes > 0 ? &partials[r * static_cast<size_t>(planes)] : nullptr;
        if (!region.touchesEdge && sink == nullptr) {
            continue;
        }
        ObjectRecord record;
        {
            Polygon polygon;
            for (int32_t k = 0; k < region.ringCount; ++k) {
                const TracedRing& ring = traced.ring(static_cast<int32_t>(r), k);
                const Point64* points = traced.ringPoints(ring);
                for (int32_t i = 0; i < ring.count; ++i) {
                    mapped[static_cast<size_t>(i)] =
                        context.toLevel0(Point64{points[i].x + tile.x, points[i].y + tile.y});
                }
                polygon.addRing(mapped.data(), ring.count, ring.isHole);
            }
            if (region.touchesEdge) {
                context.emitSeamFragment(polygon, context.targetLayer(), own, planes);
                continue;
            }
            record.setGeometry(polygon);
        }
        record.setLayer(context.targetLayer());
        applyStandardMeasurements(record.geometry(), own, planes, record.measurements());
        sink->emit(record);
    }
}

} // namespace slideio::processor::detail
