// Requirements: UR-SEG-3, UR-SEG-5
#pragma once

/**
 * @file standard_measurements.hpp
 * @brief applyStandardMeasurements: the one formula for an object's geometric
 * and intensity measurements.
 */

#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/core/measurement.hpp>
#include <slideio/processor/pipeline/export.hpp>
#include <slideio/processor/pipeline/operation.hpp>

#include <cstdint>

namespace slideio::processor {

/**
 * @brief Sets an object's standard measurements from its level-0 polygon and
 * its per-channel intensity partials.
 *
 * The single formula for objects finished inside one tile and for objects
 * merged from seam fragments, so the two carry identical measurements
 * (UR-SEG-3). Sets:
 * - `area` = Polygon::area(), in MeasurementUnit::SquarePixels;
 * - `perimeter` = Polygon::perimeter(), in MeasurementUnit::Pixels;
 * - for each partial with a positive count, in MeasurementUnit::Intensity:
 *   `intensity.<channel>.mean` = sum / count,
 *   `intensity.<channel>.stddev` = sqrt(max(0, sumOfSquares / count - mean^2))
 *   (population), `intensity.<channel>.min` and `intensity.<channel>.max`.
 *
 * Lengths and areas are level-0 pixels; calibrated units are added by the
 * algorithm module. A partial with count 0 contributes nothing. Existing
 * measurements of other names in @p out are kept; those of the same name are
 * replaced.
 * @param level0Polygon The object's geometry, in level-0 coordinates.
 * @param partials One partial per channel, already combined across fragments;
 * read during this call only. May be null when @p partialCount is 0.
 * @param partialCount Number of partials; not negative.
 * @param out Receives the measurements.
 * @throws InvalidArgumentError When @p partialCount is negative, when
 * @p partials is null while @p partialCount is positive, or when two partials
 * name the same channel; @p out is then unchanged.
 * @throws std::bad_alloc When memory is exhausted.
 * @par Thread-safety
 * Reentrant; @p out is ThreadCompatible.
 * @par Requirements
 * UR-SEG-3, UR-SEG-5
 */
SLIDEIO_PROCESSOR_ENGINE_EXPORTS void applyStandardMeasurements(const Polygon& level0Polygon,
                                                               const IntensityPartial* partials,
                                                               int32_t partialCount,
                                                               MeasurementSet& out);

} // namespace slideio::processor
