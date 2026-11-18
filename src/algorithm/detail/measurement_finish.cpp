// Requirements: UR-SEG-4, UR-SEG-5, UR-SEG-6, UR-SEG-7
#include "detail/measurement_finish.hpp"

#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/core/measurement.hpp>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace slideio::processor::detail {

namespace {

std::vector<Point64> pointsOf(const Polygon& polygon, int32_t ring) {
    std::vector<Point64> points(static_cast<size_t>(polygon.ringPointCount(ring)));
    polygon.ringPoints(ring, points.data(), static_cast<int64_t>(points.size()));
    return points;
}

// The area-weighted centroid, outer rings positive and holes negative whatever
// their orientation; the mean of ring 0's vertices when the area is 0, as
// Polygon::centroid() does.
void centroidOf(const Polygon& polygon, double& cx, double& cy) {
    double twiceArea = 0.0;
    double sumX = 0.0;
    double sumY = 0.0;
    for (int32_t ring = 0; ring < polygon.ringCount(); ++ring) {
        const std::vector<Point64> points = pointsOf(polygon, ring);
        double a = 0.0;
        double x = 0.0;
        double y = 0.0;
        for (size_t i = 0; i < points.size(); ++i) {
            const double x0 = static_cast<double>(points[i].x);
            const double y0 = static_cast<double>(points[i].y);
            const double x1 = static_cast<double>(points[(i + 1) % points.size()].x);
            const double y1 = static_cast<double>(points[(i + 1) % points.size()].y);
            const double cross = x0 * y1 - x1 * y0;
            a += cross;
            x += (x0 + x1) * cross;
            y += (y0 + y1) * cross;
        }
        if ((a < 0.0) != polygon.ringIsHole(ring)) {
            a = -a;
            x = -x;
            y = -y;
        }
        twiceArea += a;
        sumX += x;
        sumY += y;
    }
    if (twiceArea != 0.0) {
        cx = sumX / (3.0 * twiceArea);
        cy = sumY / (3.0 * twiceArea);
        return;
    }
    cx = 0.0;
    cy = 0.0;
    if (polygon.ringCount() == 0) {
        return;
    }
    const std::vector<Point64> points = pointsOf(polygon, 0);
    for (const Point64& p : points) {
        cx += static_cast<double>(p.x);
        cy += static_cast<double>(p.y);
    }
    cx /= static_cast<double>(points.size());
    cy /= static_cast<double>(points.size());
}

// Sets a geometric measurement in pixels and, when the scene is calibrated, in
// micrometres; or removes both when its kind is not selected.
void setGeometric(MeasurementSet& set, bool selected, const char* name, double value, double scale,
                  MeasurementUnit unit, const char* calibratedSuffix, MeasurementUnit calibratedUnit) {
    const std::string calibrated = std::string(name) + calibratedSuffix;
    if (!selected) {
        set.remove(name);
        set.remove(calibrated.c_str());
        return;
    }
    set.set(name, value, unit);
    if (scale > 0.0) {
        set.set(calibrated.c_str(), value * scale, calibratedUnit);
    } else {
        set.remove(calibrated.c_str());
    }
}

bool endsWith(const std::string& text, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return text.size() >= n && text.compare(text.size() - n, n, suffix) == 0;
}

} // namespace

MeasurementPlan::MeasurementPlan(const MeasurementSelection& selection, double minimumAreaPx,
                                 double maximumAreaPx, bool keepHoles)
    : area_(selection.contains(MeasurementKind::Area)),
      perimeter_(selection.contains(MeasurementKind::Perimeter)),
      centroid_(selection.contains(MeasurementKind::Centroid)),
      boundingBox_(selection.contains(MeasurementKind::BoundingBox)),
      mean_(selection.contains(MeasurementKind::IntensityMean)),
      stddev_(selection.contains(MeasurementKind::IntensityStdDev)),
      minimum_(selection.contains(MeasurementKind::IntensityMin)),
      maximum_(selection.contains(MeasurementKind::IntensityMax)), minimumAreaPx_(minimumAreaPx),
      maximumAreaPx_(maximumAreaPx), keepHoles_(keepHoles) {}

bool MeasurementPlan::finish(ObjectRecord& record, const ObjectFinishContext& context) const {
    MeasurementSet& set = record.measurements();

    if (!keepHoles_) {
        const Polygon& geometry = record.geometry();
        bool hasHole = false;
        for (int32_t ring = 0; ring < geometry.ringCount(); ++ring) {
            hasHole = hasHole || geometry.ringIsHole(ring);
        }
        if (hasHole) {
            Polygon outer;
            for (int32_t ring = 0; ring < geometry.ringCount(); ++ring) {
                if (!geometry.ringIsHole(ring)) {
                    const std::vector<Point64> points = pointsOf(geometry, ring);
                    outer.addRing(points.data(), static_cast<int64_t>(points.size()), false);
                }
            }
            record.setGeometry(outer);
            set.set("area", outer.area(), MeasurementUnit::SquarePixels);
            set.set("perimeter", outer.perimeter(), MeasurementUnit::Pixels);
        }
    }
    const Polygon& geometry = record.geometry();

    Measurement found;
    const double area = set.find("area", found) ? found.value() : geometry.area();
    if ((minimumAreaPx_ > 0.0 && area < minimumAreaPx_) || (maximumAreaPx_ > 0.0 && area > maximumAreaPx_)) {
        return false;
    }
    const double perimeter = set.find("perimeter", found) ? found.value() : geometry.perimeter();

    const double s = std::isfinite(context.pixelSizeMicrons) && context.pixelSizeMicrons > 0.0
                         ? context.pixelSizeMicrons
                         : 0.0;
    setGeometric(set, area_, "area", area, s * s, MeasurementUnit::SquarePixels, ".um2",
                 MeasurementUnit::SquareMicrometres);
    setGeometric(set, perimeter_, "perimeter", perimeter, s, MeasurementUnit::Pixels, ".um",
                 MeasurementUnit::Micrometres);

    double cx = 0.0;
    double cy = 0.0;
    if (centroid_) {
        centroidOf(geometry, cx, cy);
    }
    setGeometric(set, centroid_, "centroid.x", cx, s, MeasurementUnit::Pixels, ".um", MeasurementUnit::Micrometres);
    setGeometric(set, centroid_, "centroid.y", cy, s, MeasurementUnit::Pixels, ".um", MeasurementUnit::Micrometres);

    const Rect64 box = geometry.boundingBox();
    setGeometric(set, boundingBox_, "boundingBox.x", static_cast<double>(box.x), s, MeasurementUnit::Pixels, ".um",
                 MeasurementUnit::Micrometres);
    setGeometric(set, boundingBox_, "boundingBox.y", static_cast<double>(box.y), s, MeasurementUnit::Pixels, ".um",
                 MeasurementUnit::Micrometres);
    setGeometric(set, boundingBox_, "boundingBox.width", static_cast<double>(box.width), s,
                 MeasurementUnit::Pixels, ".um", MeasurementUnit::Micrometres);
    setGeometric(set, boundingBox_, "boundingBox.height", static_cast<double>(box.height), s,
                 MeasurementUnit::Pixels, ".um", MeasurementUnit::Micrometres);

    // Intensity names are per channel, so the unselected ones are found by name.
    std::vector<std::string> unselected;
    for (int32_t i = 0; i < set.count(); ++i) {
        const std::string name = set.at(i).name();
        if (name.compare(0, 10, "intensity.") != 0) {
            continue;
        }
        if ((!mean_ && endsWith(name, ".mean")) || (!stddev_ && endsWith(name, ".stddev")) ||
            (!minimum_ && endsWith(name, ".min")) || (!maximum_ && endsWith(name, ".max"))) {
            unselected.push_back(name);
        }
    }
    for (const std::string& name : unselected) {
        set.remove(name.c_str());
    }
    return true;
}

} // namespace slideio::processor::detail
