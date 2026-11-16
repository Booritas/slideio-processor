// Requirements: UR-SEG-3, UR-SEG-5
#include <slideio/processor/pipeline/standard_measurements.hpp>

#include <slideio/processor/core/error.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <string>

namespace slideio::processor {

void applyStandardMeasurements(const Polygon& level0Polygon, const IntensityPartial* partials,
                               int32_t partialCount, MeasurementSet& out) {
    if (partialCount < 0) {
        throw InvalidArgumentError("applyStandardMeasurements: partialCount must not be negative");
    }
    if (partials == nullptr && partialCount > 0) {
        throw InvalidArgumentError(
            "applyStandardMeasurements: partials must not be null when partialCount is positive");
    }
    std::set<int32_t> channels;
    for (int32_t i = 0; i < partialCount; ++i) {
        if (!channels.insert(partials[i].channel).second) {
            const std::string message = "applyStandardMeasurements: channel " +
                                        std::to_string(partials[i].channel) +
                                        " has more than one partial; combine them first";
            throw InvalidArgumentError(message.c_str());
        }
    }

    // Level-0 pixels; calibrated units are the algorithm module's.
    out.set("area", level0Polygon.area(), MeasurementUnit::SquarePixels);
    out.set("perimeter", level0Polygon.perimeter(), MeasurementUnit::Pixels);
    for (int32_t i = 0; i < partialCount; ++i) {
        const IntensityPartial& p = partials[i];
        if (p.count <= 0) {
            continue;
        }
        const double n = static_cast<double>(p.count);
        const double mean = p.sum / n;
        // Population deviation; clamped because rounding can make the difference slightly negative.
        const double stddev = std::sqrt(std::max(0.0, p.sumOfSquares / n - mean * mean));
        const std::string prefix = "intensity." + std::to_string(p.channel) + ".";
        out.set((prefix + "mean").c_str(), mean, MeasurementUnit::Intensity);
        out.set((prefix + "stddev").c_str(), stddev, MeasurementUnit::Intensity);
        out.set((prefix + "min").c_str(), p.minimum, MeasurementUnit::Intensity);
        out.set((prefix + "max").c_str(), p.maximum, MeasurementUnit::Intensity);
    }
}

} // namespace slideio::processor
