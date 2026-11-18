// Requirements: UR-SEG-3, UR-SEG-4, UR-SEG-5, UR-IMG-2
#include "detail/region_emit.hpp"

#include "detail/sample_codec.hpp"

#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/core/object_record.hpp>
#include <slideio/processor/pipeline/standard_measurements.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <unordered_map>

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

void emitRegions(OperationContext& context, const int32_t* labels, int64_t stride,
                 const std::vector<TracedRegion>& regions, const BlockView& input,
                 int32_t firstIntensityPlane) {
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
        std::unordered_map<int32_t, size_t> indexOf;
        for (size_t r = 0; r < regions.size(); ++r) {
            indexOf.emplace(regions[r].label, r);
        }
        for (int64_t y = 0; y < tile.height; ++y) {
            const int32_t* row = labels + y * stride;
            int32_t lastLabel = 0;
            size_t lastIndex = regions.size();
            for (int64_t x = 0; x < tile.width; ++x) {
                const int32_t label = row[x];
                if (label <= 0) {
                    continue;
                }
                if (label != lastLabel) {
                    const auto found = indexOf.find(label);
                    lastIndex = found == indexOf.end() ? regions.size() : found->second;
                    lastLabel = label;
                }
                if (lastIndex == regions.size()) {
                    continue;
                }
                for (int32_t p = 0; p < planes; ++p) {
                    const int32_t plane = first + p;
                    const auto* inputRow = static_cast<const uint8_t*>(input.planeData(plane)) +
                                           (y + offsetY) * input.planeRowStrideBytes(plane);
                    const double v = sampleAsDouble(readSample(inputRow, x + offsetX, input.planeType(plane)));
                    addSample(partials[lastIndex * static_cast<size_t>(planes) + static_cast<size_t>(p)], v);
                }
            }
        }
    }

    for (size_t r = 0; r < regions.size(); ++r) {
        const TracedRegion& region = regions[r];
        Polygon levelPolygon;
        for (size_t ring = 0; ring < region.rings.size(); ++ring) {
            std::vector<Point64> points = region.rings[ring];
            for (Point64& point : points) {
                point = Point64{point.x + tile.x, point.y + tile.y};
            }
            levelPolygon.addRing(points.data(), static_cast<int64_t>(points.size()), region.isHole[ring]);
        }
        const Polygon polygon = context.toLevel0(levelPolygon);
        const IntensityPartial* own = planes > 0 ? &partials[r * static_cast<size_t>(planes)] : nullptr;
        if (region.touchesEdge) {
            context.emitSeamFragment(polygon, context.targetLayer(), own, planes);
            continue;
        }
        ObjectSink* sink = context.objectSink();
        if (sink == nullptr) {
            continue;
        }
        ObjectRecord record;
        record.setGeometry(polygon);
        record.setLayer(context.targetLayer());
        applyStandardMeasurements(polygon, own, planes, record.measurements());
        sink->emit(record);
    }
}

} // namespace slideio::processor::detail
