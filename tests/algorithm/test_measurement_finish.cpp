// Requirements: UR-SEG-4, UR-SEG-5, UR-SEG-6, UR-SEG-7
#include <slideio/processor/algorithm/measure/measure.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/measurement.hpp>
#include <slideio/processor/core/object_record.hpp>
#include <slideio/processor/pipeline/operation.hpp>
#include <slideio/processor/pipeline/standard_measurements.hpp>

#include <detail/measurement_finish.hpp>
#include <pipeline/test_support/synthetic_scene.hpp>

#include <slideio/slideio/scene.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace slideio::processor;
namespace ts = slideio::processor::testing_support;

namespace {

const std::vector<std::string> kGeometricNames = {"area",          "perimeter",     "centroid.x",
                                                  "centroid.y",    "boundingBox.x", "boundingBox.y",
                                                  "boundingBox.width", "boundingBox.height"};
const std::vector<std::string> kIntensityNames = {"intensity.3.mean", "intensity.3.stddev", "intensity.3.min",
                                                  "intensity.3.max"};

void addBox(Polygon& polygon, int64_t x, int64_t y, int64_t w, int64_t h, bool hole) {
    // Outer rings clockwise on screen (positive), holes the other way.
    std::vector<Point64> ring = {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}};
    if (hole) {
        std::swap(ring[1], ring[3]);
    }
    polygon.addRing(ring.data(), static_cast<int64_t>(ring.size()), hole);
}

// A record as the framework hands it to finishObject(): the polygon with the
// standard measurements over it and one intensity partial on channel 3.
ObjectRecord recordOf(const Polygon& polygon) {
    ObjectRecord record;
    record.setGeometry(polygon);
    record.setLayer(LayerId(1));
    const IntensityPartial partial{4, 20.0, 110.0, 2.0, 8.0, 3};
    applyStandardMeasurements(polygon, &partial, 1, record.measurements());
    return record;
}

ObjectRecord rectangleRecord() {
    Polygon polygon;
    addBox(polygon, 10, 20, 2, 4, false);
    return recordOf(polygon);
}

ObjectRecord squareWithHoleRecord() {
    Polygon polygon;
    addBox(polygon, 0, 0, 6, 6, false);
    addBox(polygon, 2, 2, 2, 2, true);
    return recordOf(polygon);
}

bool has(const ObjectRecord& record, const std::string& name) {
    Measurement m;
    return record.measurements().find(name.c_str(), m);
}

double value(const ObjectRecord& record, const std::string& name) {
    Measurement m;
    return record.measurements().find(name.c_str(), m) ? m.value() : std::numeric_limits<double>::quiet_NaN();
}

MeasurementUnit unit(const ObjectRecord& record, const std::string& name) {
    Measurement m;
    record.measurements().find(name.c_str(), m);
    return m.unit();
}

const ObjectFinishContext kUncalibrated{0.0, 0};
const ObjectFinishContext kHalfMicron{0.5, 0};

std::shared_ptr<slideio::Scene> sceneWith(double resolutionX, double resolutionY) {
    auto source = std::make_shared<ts::SyntheticSource>(
        std::vector<ts::SyntheticLevel>{{64, 48, 0, 0}}, std::vector<slideio::DataType>{slideio::DataType::DT_Byte},
        [](int, int64_t, int64_t) { return 0.0; });
    source->setResolution(resolutionX, resolutionY);
    return ts::makeSyntheticScene(source);
}

} // namespace

TEST(MeasurementFinish, CalibratedNamesAtHalfMicron) {
    const detail::MeasurementPlan plan(MeasurementSelection::standard(), 0.0, 0.0, true);
    ObjectRecord record = rectangleRecord();
    ASSERT_TRUE(plan.finish(record, kHalfMicron));
    EXPECT_EQ(value(record, "area"), 8.0);
    EXPECT_EQ(value(record, "area.um2"), 2.0);
    EXPECT_EQ(unit(record, "area.um2"), MeasurementUnit::SquareMicrometres);
    EXPECT_EQ(value(record, "perimeter"), 12.0);
    EXPECT_EQ(value(record, "perimeter.um"), 6.0);
    EXPECT_EQ(unit(record, "perimeter.um"), MeasurementUnit::Micrometres);
    EXPECT_EQ(value(record, "centroid.x.um"), 5.5);
    EXPECT_EQ(value(record, "centroid.y.um"), 11.0);
    EXPECT_EQ(value(record, "boundingBox.width.um"), 1.0);
    EXPECT_EQ(value(record, "boundingBox.height.um"), 2.0);
    EXPECT_EQ(value(record, "boundingBox.x.um"), 5.0);
    EXPECT_EQ(value(record, "boundingBox.y.um"), 10.0);
}

TEST(MeasurementFinish, NoCalibratedNamesWhenThePixelSizeIsUnknown) {
    const detail::MeasurementPlan plan(MeasurementSelection::standard(), 0.0, 0.0, true);
    ObjectRecord record = rectangleRecord();
    ASSERT_TRUE(plan.finish(record, kUncalibrated));
    for (int32_t i = 0; i < record.measurements().count(); ++i) {
        const std::string name = record.measurements().at(i).name();
        EXPECT_EQ(name.find(".um"), std::string::npos) << name;
    }
    for (const std::string& name : kGeometricNames) {
        Measurement m;
        ASSERT_TRUE(record.measurements().find(name.c_str(), m)) << name;
        EXPECT_FALSE(m.isCalibrated()) << name;
    }
    for (const std::string& name : kIntensityNames) {
        EXPECT_TRUE(has(record, name)) << name;
    }
}

TEST(MeasurementFinish, CentroidAndBoundingBoxExact) {
    const detail::MeasurementPlan plan(MeasurementSelection::standard(), 0.0, 0.0, true);
    ObjectRecord record = rectangleRecord();
    ASSERT_TRUE(plan.finish(record, kUncalibrated));
    EXPECT_EQ(value(record, "centroid.x"), 11.0);
    EXPECT_EQ(value(record, "centroid.y"), 22.0);
    EXPECT_EQ(unit(record, "centroid.x"), MeasurementUnit::Pixels);
    EXPECT_EQ(value(record, "boundingBox.x"), 10.0);
    EXPECT_EQ(value(record, "boundingBox.y"), 20.0);
    EXPECT_EQ(value(record, "boundingBox.width"), 2.0);
    EXPECT_EQ(value(record, "boundingBox.height"), 4.0);
}

TEST(MeasurementFinish, CentroidExactFarFromTheOrigin) {
    // A long object at level-0 coordinates of a large slide: summed in slide
    // coordinates, the shoelace products leave double's exact integer range
    // and the centroid drifts by about 1e-10 pixels.
    Polygon polygon;
    addBox(polygon, 200000, 300000, 10001, 100001, false);
    ObjectRecord record = recordOf(polygon);
    const detail::MeasurementPlan plan(MeasurementSelection::geometryOnly(), 0.0, 0.0, true);
    ASSERT_TRUE(plan.finish(record, kUncalibrated));
    EXPECT_EQ(value(record, "centroid.x"), 205000.5);
    EXPECT_EQ(value(record, "centroid.y"), 350000.5);
}

TEST(MeasurementFinish, CentroidIsUnroundedAndHolesWeighNegatively) {
    // An L of three unit pixels: (0,0), (1,0), (0,1). Centroid (5/6, 5/6), which
    // Polygon::centroid() would round to (1, 1).
    Polygon polygon;
    const std::vector<Point64> ring = {{0, 0}, {2, 0}, {2, 1}, {1, 1}, {1, 2}, {0, 2}};
    polygon.addRing(ring.data(), static_cast<int64_t>(ring.size()), false);
    ObjectRecord record = recordOf(polygon);
    const detail::MeasurementPlan plan(MeasurementSelection::geometryOnly(), 0.0, 0.0, true);
    ASSERT_TRUE(plan.finish(record, kUncalibrated));
    EXPECT_DOUBLE_EQ(value(record, "centroid.x"), 5.0 / 6.0);
    EXPECT_DOUBLE_EQ(value(record, "centroid.y"), 5.0 / 6.0);

    // A 6 x 6 square with a 2 x 2 hole off-centre at (3, 1): the hole pulls the
    // centroid away from it. (36 * 3 - 4 * 4) / 32 = 2.875; (36 * 3 - 4 * 2) / 32 = 3.125.
    Polygon holed;
    addBox(holed, 0, 0, 6, 6, false);
    addBox(holed, 3, 1, 2, 2, true);
    ObjectRecord holedRecord = recordOf(holed);
    ASSERT_TRUE(plan.finish(holedRecord, kUncalibrated));
    EXPECT_EQ(value(holedRecord, "centroid.x"), 2.875);
    EXPECT_EQ(value(holedRecord, "centroid.y"), 3.125);
}

TEST(MeasurementFinish, UnselectedNamesAreRemoved) {
    MeasurementSelection selection;
    selection.add(MeasurementKind::Area).add(MeasurementKind::IntensityMean);
    const detail::MeasurementPlan plan(selection, 0.0, 0.0, true);
    ObjectRecord record = rectangleRecord();
    ASSERT_TRUE(plan.finish(record, kHalfMicron));
    EXPECT_TRUE(has(record, "area"));
    EXPECT_TRUE(has(record, "area.um2"));
    EXPECT_TRUE(has(record, "intensity.3.mean"));
    EXPECT_EQ(value(record, "intensity.3.mean"), 5.0);
    for (const std::string& name : {"perimeter", "perimeter.um", "centroid.x", "centroid.y.um", "boundingBox.x",
                                    "boundingBox.height.um", "intensity.3.stddev", "intensity.3.min",
                                    "intensity.3.max"}) {
        EXPECT_FALSE(has(record, name)) << name;
    }
    EXPECT_EQ(record.measurements().count(), 3);
}

TEST(MeasurementFinish, EmptySelectionKeepsNoMeasurement) {
    const detail::MeasurementPlan plan(MeasurementSelection(), 0.0, 0.0, true);
    ObjectRecord record = rectangleRecord();
    ASSERT_TRUE(plan.finish(record, kHalfMicron));
    EXPECT_EQ(record.measurements().count(), 0);
}

TEST(MeasurementFinish, AreaBoundsDiscard) {
    // The rectangle's area is 8.
    struct Case {
        double minimum;
        double maximum;
        bool kept;
    };
    for (const Case& c : {Case{9.0, 0.0, false}, Case{0.0, 7.0, false}, Case{8.0, 8.0, true},
                          Case{0.0, 0.0, true}, Case{7.5, 0.0, true}, Case{0.0, 8.5, true}}) {
        const detail::MeasurementPlan plan(MeasurementSelection::standard(), c.minimum, c.maximum, true);
        ObjectRecord record = rectangleRecord();
        EXPECT_EQ(plan.finish(record, kUncalibrated), c.kept) << c.minimum << " " << c.maximum;
    }
}

TEST(MeasurementFinish, AreaBoundsIgnoreTheSelection) {
    // Area need not be selected to be filtered on.
    const detail::MeasurementPlan plan(MeasurementSelection(), 9.0, 0.0, true);
    ObjectRecord record = rectangleRecord();
    EXPECT_FALSE(plan.finish(record, kUncalibrated));
}

TEST(MeasurementFinish, DroppingHolesRecomputesAreaAndPerimeter) {
    {
        const detail::MeasurementPlan plan(MeasurementSelection::standard(), 0.0, 0.0, true);
        ObjectRecord record = squareWithHoleRecord();
        ASSERT_TRUE(plan.finish(record, kUncalibrated));
        EXPECT_EQ(record.geometry().ringCount(), 2);
        EXPECT_EQ(value(record, "area"), 32.0);
        EXPECT_EQ(value(record, "perimeter"), 32.0);
    }
    {
        const detail::MeasurementPlan plan(MeasurementSelection::standard(), 0.0, 0.0, false);
        ObjectRecord record = squareWithHoleRecord();
        const Point64 anchor = record.geometry().anchor();
        ASSERT_TRUE(plan.finish(record, kHalfMicron));
        EXPECT_EQ(record.geometry().ringCount(), 1);
        EXPECT_FALSE(record.geometry().ringIsHole(0));
        EXPECT_EQ(record.geometry().anchor(), anchor);
        EXPECT_EQ(value(record, "area"), 36.0);
        EXPECT_EQ(value(record, "perimeter"), 24.0);
        EXPECT_EQ(value(record, "area.um2"), 9.0);
        EXPECT_EQ(value(record, "centroid.x"), 3.0);
        // Intensity stays over the object's own pixels.
        EXPECT_EQ(value(record, "intensity.3.mean"), 5.0);
    }
}

TEST(MeasurementFinish, AreaFilterSeesTheAreaWithoutHoles) {
    // 32 with the hole, 36 without: a minimum of 34 keeps it only once the hole is gone.
    const detail::MeasurementPlan keeping(MeasurementSelection::standard(), 34.0, 0.0, true);
    ObjectRecord withHole = squareWithHoleRecord();
    EXPECT_FALSE(keeping.finish(withHole, kUncalibrated));
    const detail::MeasurementPlan dropping(MeasurementSelection::standard(), 34.0, 0.0, false);
    ObjectRecord withoutHole = squareWithHoleRecord();
    EXPECT_TRUE(dropping.finish(withoutHole, kUncalibrated));
}

TEST(MeasurementSelectionTest, FactoriesAndCounts) {
    const MeasurementSelection empty;
    EXPECT_EQ(empty.count(), 0);
    EXPECT_FALSE(empty.contains(MeasurementKind::Area));

    const MeasurementSelection all = MeasurementSelection::standard();
    EXPECT_EQ(all.count(), 8);
    for (int32_t k = 0; k < 8; ++k) {
        EXPECT_TRUE(all.contains(static_cast<MeasurementKind>(k))) << k;
    }
    EXPECT_FALSE(all.contains(static_cast<MeasurementKind>(8)));
    EXPECT_FALSE(all.contains(static_cast<MeasurementKind>(-1)));

    const MeasurementSelection geometry = MeasurementSelection::geometryOnly();
    EXPECT_EQ(geometry.count(), 4);
    EXPECT_TRUE(geometry.contains(MeasurementKind::Area));
    EXPECT_TRUE(geometry.contains(MeasurementKind::Perimeter));
    EXPECT_TRUE(geometry.contains(MeasurementKind::Centroid));
    EXPECT_TRUE(geometry.contains(MeasurementKind::BoundingBox));
    EXPECT_FALSE(geometry.contains(MeasurementKind::IntensityMean));

    MeasurementSelection some;
    some.add(MeasurementKind::Centroid).add(MeasurementKind::Centroid).add(MeasurementKind::IntensityMax);
    EXPECT_EQ(some.count(), 2);
    EXPECT_EQ(some.addAll().count(), 8);
}

TEST(MeasurementSelectionTest, AddRefusesAValueThatIsNotAKind) {
    MeasurementSelection selection;
    selection.add(MeasurementKind::Area);
    EXPECT_THROW(selection.add(static_cast<MeasurementKind>(8)), InvalidArgumentError);
    EXPECT_THROW(selection.add(static_cast<MeasurementKind>(-1)), InvalidArgumentError);
    EXPECT_EQ(selection.count(), 1);
}

TEST(MeasurementSelectionTest, CopyAndMove) {
    MeasurementSelection original;
    original.add(MeasurementKind::Perimeter);
    MeasurementSelection copy(original);
    copy.add(MeasurementKind::Area);
    EXPECT_EQ(original.count(), 1);
    EXPECT_EQ(copy.count(), 2);

    MeasurementSelection assigned;
    assigned = copy;
    EXPECT_EQ(assigned.count(), 2);
    assigned = assigned; // self-assignment keeps the selection
    EXPECT_EQ(assigned.count(), 2);

    MeasurementSelection moved(std::move(copy));
    EXPECT_EQ(moved.count(), 2);
    EXPECT_EQ(copy.count(), 0); // NOLINT(bugprone-use-after-move): moved-from is empty
    MeasurementSelection moveAssigned;
    moveAssigned = std::move(moved);
    EXPECT_TRUE(moveAssigned.contains(MeasurementKind::Area));
    EXPECT_EQ(moved.count(), 0); // NOLINT(bugprone-use-after-move)
    moved.add(MeasurementKind::IntensityMin);
    EXPECT_EQ(moved.count(), 1);
}

TEST(SceneCalibration, NullSceneIsUncalibrated) {
    EXPECT_FALSE(sceneIsCalibrated(nullptr));
    EXPECT_EQ(scenePixelSizeMicrons(nullptr), 0.0);
}

TEST(SceneCalibration, SameRuleAsTheRun) {
    // The resolutions tests/pipeline/test_pixel_size.cpp checks the run's rule with.
    EXPECT_DOUBLE_EQ(scenePixelSizeMicrons(sceneWith(0.25e-6, 0.25e-6)), 0.25);
    EXPECT_TRUE(sceneIsCalibrated(sceneWith(0.25e-6, 0.25e-6)));
    EXPECT_EQ(scenePixelSizeMicrons(sceneWith(0.0, 0.0)), 0.0);
    EXPECT_FALSE(sceneIsCalibrated(sceneWith(0.0, 0.0)));
    EXPECT_EQ(scenePixelSizeMicrons(sceneWith(0.25e-6, 0.5e-6)), 0.0);
    EXPECT_FALSE(sceneIsCalibrated(sceneWith(0.25e-6, 0.5e-6)));
    EXPECT_EQ(scenePixelSizeMicrons(sceneWith(0.25e-6, 0.0)), 0.0);
    EXPECT_EQ(scenePixelSizeMicrons(sceneWith(-0.25e-6, -0.25e-6)), 0.0);
    EXPECT_NEAR(scenePixelSizeMicrons(sceneWith(0.25e-6, 0.25e-6 * (1.0 + 5e-7))), 0.25 * (1.0 + 2.5e-7), 1e-12);
    EXPECT_EQ(scenePixelSizeMicrons(sceneWith(0.25e-6, 0.25e-6 * (1.0 + 5e-6))), 0.0);
}
