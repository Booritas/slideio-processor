// Requirements: UR-SEG-4, UR-SEG-5, UR-SEG-6, UR-SEG-7
#pragma once

#include <slideio/processor/algorithm/measure/measure.hpp>
#include <slideio/processor/core/object_record.hpp>
#include <slideio/processor/pipeline/operation.hpp>

namespace slideio::processor::detail {

/// What a segmenter does to each of its objects in Operation::finishObject():
/// hole rules, area filters and the selected measurements, applied the same
/// way to an object found whole in one tile and to one merged across seams.
/// Immutable once built, so finish() may run concurrently.
class MeasurementPlan {
public:
    /// @p minimumAreaPx and @p maximumAreaPx bound the object's final area in
    /// square level-0 pixels, inclusive; 0 leaves that side unbounded. With
    /// @p keepHoles false, hole rings are removed. The caller has checked the
    /// values.
    MeasurementPlan(const MeasurementSelection& selection, double minimumAreaPx, double maximumAreaPx,
                    bool keepHoles);

    /// Finishes @p record, whose rings are normalised and whose standard
    /// measurements (`area`, `perimeter`, `intensity.<c>.*`) are applied:
    /// - without holes kept, removes the hole rings and recomputes `area` and
    ///   `perimeter` from the outer rings (intensity stays over the object's
    ///   own pixels);
    /// - returns false, discarding the object, when its area is outside the
    ///   bounds;
    /// - sets `centroid.x`, `centroid.y` (area-weighted, unrounded, by the
    ///   shoelace formula in double) and `boundingBox.x`, `.y`, `.width`,
    ///   `.height` when selected, in level-0 pixels;
    /// - when context.pixelSizeMicrons is positive, adds each selected
    ///   geometric measurement in micrometres as `<name>.um`, and `area.um2`;
    /// - removes every measurement of a kind not selected.
    bool finish(ObjectRecord& record, const ObjectFinishContext& context) const;

private:
    bool area_;
    bool perimeter_;
    bool centroid_;
    bool boundingBox_;
    bool mean_;
    bool stddev_;
    bool minimum_;
    bool maximum_;
    double minimumAreaPx_;
    double maximumAreaPx_;
    bool keepHoles_;
};

} // namespace slideio::processor::detail
