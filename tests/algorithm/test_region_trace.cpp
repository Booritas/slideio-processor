// Requirements: UR-SEG-3, UR-SEG-4, UR-SEG-5, UR-IMG-2, UR-QA-2
#include <slideio/processor/algorithm/measure/measure.hpp>
#include <slideio/processor/core/measurement.hpp>
#include <slideio/processor/core/object_record.hpp>
#include <slideio/processor/pipeline/operation.hpp>
#include <slideio/processor/pipeline/pipeline_spec.hpp>

#include <algorithm/support/harness.hpp>
#include <detail/measurement_finish.hpp>
#include <detail/region_emit.hpp>
#include <detail/region_trace.hpp>
#include <pipeline/test_support/synthetic_scene.hpp>
#include <pipeline/test_support/test_operations.hpp>

#include <slideio/slideio/scene.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using namespace slideio::processor;
namespace ts = slideio::processor::testing_support;

namespace {

// A label buffer drawn from rows of characters: '.' is 0, a digit its value.
struct Labels {
    int64_t width = 0;
    int64_t height = 0;
    std::vector<int32_t> values;
};

Labels labelsOf(const std::vector<std::string>& rows) {
    Labels labels;
    labels.height = static_cast<int64_t>(rows.size());
    labels.width = static_cast<int64_t>(rows.front().size());
    for (const std::string& row : rows) {
        for (char c : row) {
            labels.values.push_back(c == '.' ? 0 : c - '0');
        }
    }
    return labels;
}

std::vector<detail::TracedRegion> trace(const Labels& labels, bool eightConnected) {
    return detail::traceRegions(labels.values.data(), labels.width, labels.height, labels.width, eightConnected);
}

int64_t twiceSignedArea(const std::vector<Point64>& ring) {
    int64_t total = 0;
    for (size_t i = 0; i < ring.size(); ++i) {
        const Point64& a = ring[i];
        const Point64& b = ring[(i + 1) % ring.size()];
        total += a.x * b.y - b.x * a.y;
    }
    return total;
}

int64_t perimeterOf(const std::vector<Point64>& ring) {
    int64_t total = 0;
    for (size_t i = 0; i < ring.size(); ++i) {
        const Point64& a = ring[i];
        const Point64& b = ring[(i + 1) % ring.size()];
        total += std::abs(b.x - a.x) + std::abs(b.y - a.y);
    }
    return total;
}

double measurement(const ObjectRecord& record, const char* name) {
    Measurement m;
    return record.measurements().find(name, m) ? m.value() : std::numeric_limits<double>::quiet_NaN();
}

bool has(const ObjectRecord& record, const char* name) {
    Measurement m;
    return record.measurements().find(name, m);
}

} // namespace

TEST(RegionTrace, SquareIsOneRing) {
    const Labels labels = labelsOf({".....", ".111.", ".111.", ".111.", "....."});
    for (bool eight : {false, true}) {
        const std::vector<detail::TracedRegion> regions = trace(labels, eight);
        ASSERT_EQ(regions.size(), 1u);
        const detail::TracedRegion& region = regions[0];
        EXPECT_EQ(region.label, 1);
        ASSERT_EQ(region.rings.size(), 1u);
        ASSERT_EQ(region.isHole.size(), 1u);
        EXPECT_FALSE(region.isHole[0]);
        EXPECT_FALSE(region.touchesEdge);
        // Only the corners are vertices; area 9 and perimeter 12, outer positive.
        EXPECT_EQ(region.rings[0].size(), 4u);
        EXPECT_EQ(twiceSignedArea(region.rings[0]), 18);
        EXPECT_EQ(perimeterOf(region.rings[0]), 12);
        for (const Point64& p : region.rings[0]) {
            EXPECT_TRUE((p.x == 1 || p.x == 4) && (p.y == 1 || p.y == 4)) << p.x << "," << p.y;
        }
    }
}

TEST(RegionTrace, HoleIsASecondNegativeRing) {
    const Labels labels = labelsOf({".......", ".11111.", ".11111.", ".11.11.", ".11111.", ".11111.", "......."});
    const std::vector<detail::TracedRegion> regions = trace(labels, true);
    ASSERT_EQ(regions.size(), 1u);
    const detail::TracedRegion& region = regions[0];
    ASSERT_EQ(region.rings.size(), 2u);
    ASSERT_EQ(region.isHole.size(), 2u);
    EXPECT_FALSE(region.isHole[0]);
    EXPECT_TRUE(region.isHole[1]);
    EXPECT_EQ(twiceSignedArea(region.rings[0]), 50);
    EXPECT_EQ(twiceSignedArea(region.rings[1]), -2);
}

TEST(RegionTrace, DiagonalPairFourConnectedIsTwoObjects) {
    // Labelled 4-connected: the two pixels are two regions, each its own ring.
    const Labels labels = labelsOf({"....", ".1..", "..2.", "...."});
    const std::vector<detail::TracedRegion> regions = trace(labels, false);
    ASSERT_EQ(regions.size(), 2u);
    for (const detail::TracedRegion& region : regions) {
        ASSERT_EQ(region.rings.size(), 1u);
        EXPECT_EQ(twiceSignedArea(region.rings[0]), 2);
        EXPECT_EQ(region.rings[0].size(), 4u);
    }
    EXPECT_EQ(regions[0].label, 1);
    EXPECT_EQ(regions[1].label, 2);
}

TEST(RegionTrace, DiagonalPairEightConnectedIsOneRingThroughThePinch) {
    const Labels labels = labelsOf({"....", ".1..", "..1.", "...."});
    const std::vector<detail::TracedRegion> regions = trace(labels, true);
    ASSERT_EQ(regions.size(), 1u);
    ASSERT_EQ(regions[0].rings.size(), 1u);
    const std::vector<Point64>& ring = regions[0].rings[0];
    EXPECT_FALSE(regions[0].isHole[0]);
    EXPECT_EQ(twiceSignedArea(ring), 4);
    EXPECT_EQ(perimeterOf(ring), 8);
    // The left turn at the pinch passes through (2, 2) twice.
    ASSERT_EQ(ring.size(), 8u);
    EXPECT_EQ(std::count(ring.begin(), ring.end(), Point64{2, 2}), 2);
}

TEST(RegionTrace, FourConnectedTracingKeepsDiagonalRingsApart) {
    // One label touching itself only diagonally, traced 4-connected: the right
    // turn at the pinch keeps the pixels' rings apart, as the merger does.
    const Labels labels = labelsOf({"....", ".1..", "..1.", "...."});
    const std::vector<detail::TracedRegion> regions = trace(labels, false);
    ASSERT_EQ(regions.size(), 1u);
    ASSERT_EQ(regions[0].rings.size(), 2u);
    EXPECT_EQ(twiceSignedArea(regions[0].rings[0]), 2);
    EXPECT_EQ(twiceSignedArea(regions[0].rings[1]), 2);
}

TEST(RegionTrace, EightConnectedHolesTouchingAtAPointStayApart) {
    // Two background pixels meeting diagonally inside a region: with 8-connected
    // pixels the background is 4-connected, so they are two holes.
    const Labels labels = labelsOf({"111111", "111111", "11.111", "111.11", "111111", "111111"});
    const std::vector<detail::TracedRegion> regions = trace(labels, true);
    ASSERT_EQ(regions.size(), 1u);
    ASSERT_EQ(regions[0].rings.size(), 3u);
    EXPECT_FALSE(regions[0].isHole[0]);
    EXPECT_TRUE(regions[0].isHole[1]);
    EXPECT_TRUE(regions[0].isHole[2]);
    EXPECT_EQ(twiceSignedArea(regions[0].rings[1]), -2);
    EXPECT_EQ(twiceSignedArea(regions[0].rings[2]), -2);
}

TEST(RegionTrace, EdgeTouchingFlags) {
    const Labels labels = labelsOf({"1.....", "......", "..2...", "......", ".....3", "....4."});
    const std::vector<detail::TracedRegion> regions = trace(labels, false);
    ASSERT_EQ(regions.size(), 4u);
    EXPECT_TRUE(regions[0].touchesEdge);  // top-left corner
    EXPECT_FALSE(regions[1].touchesEdge); // interior
    EXPECT_TRUE(regions[2].touchesEdge);  // right column
    EXPECT_TRUE(regions[3].touchesEdge);  // bottom row
}

TEST(RegionTrace, TouchingLabelsShareTheirEdge) {
    const Labels labels = labelsOf({"......", ".1122.", ".1122.", "......"});
    const std::vector<detail::TracedRegion> regions = trace(labels, false);
    ASSERT_EQ(regions.size(), 2u);
    ASSERT_EQ(regions[0].rings.size(), 1u);
    ASSERT_EQ(regions[1].rings.size(), 1u);
    EXPECT_EQ(twiceSignedArea(regions[0].rings[0]), 8);
    EXPECT_EQ(twiceSignedArea(regions[1].rings[0]), 8);
    // The shared edge x = 3 is on both rings.
    for (const detail::TracedRegion& region : regions) {
        EXPECT_EQ(std::count_if(region.rings[0].begin(), region.rings[0].end(),
                                [](const Point64& p) { return p.x == 3; }),
                  2);
    }
}

TEST(RegionTrace, LabelInsideAHoleIsItsOwnRegion) {
    const Labels labels = labelsOf({"11111", "1...1", "1.2.1", "1...1", "11111"});
    const std::vector<detail::TracedRegion> regions = trace(labels, false);
    ASSERT_EQ(regions.size(), 2u);
    EXPECT_EQ(regions[0].label, 1);
    ASSERT_EQ(regions[0].rings.size(), 2u);
    EXPECT_TRUE(regions[0].isHole[1]);
    EXPECT_EQ(twiceSignedArea(regions[0].rings[1]), -18);
    EXPECT_EQ(regions[1].label, 2);
    ASSERT_EQ(regions[1].rings.size(), 1u);
    EXPECT_FALSE(regions[1].touchesEdge);
}

TEST(RegionTrace, StrideAndNegativeLabels) {
    // A 3 x 2 buffer inside rows of 5; the extra columns and negative labels are not traced.
    const std::vector<int32_t> values = {1, 1, -1, 9, 9, 1, 0, -4, 9, 9};
    const std::vector<detail::TracedRegion> regions = detail::traceRegions(values.data(), 3, 2, 5, false);
    ASSERT_EQ(regions.size(), 1u);
    ASSERT_EQ(regions[0].rings.size(), 1u);
    EXPECT_EQ(twiceSignedArea(regions[0].rings[0]), 6);
    EXPECT_EQ(regions[0].rings[0].size(), 6u);
}

// --- Emission ------------------------------------------------------------------

namespace {

// "test.regions": Joint, halo 1. Plane 0 is a mask labelled 4-connected (or
// 8-connected) over the tile rect; planes 1.. are measured. Finishes its
// objects with the standard selection.
class RegionsOperation : public Operation {
public:
    explicit RegionsOperation(bool eightConnected = false)
        : eightConnected_(eightConnected), plan_(MeasurementSelection::standard(), 0.0, 0.0, true) {}
    const char* name() const override { return "test.regions"; }
    ChannelArity arity() const override { return ChannelArity::Joint; }
    bool requiresUniformTypes() const override { return false; }
    DataTypeSet supportedTypes() const override { return DataTypeSet().add(DataType::Byte).add(DataType::Float32); }
    Halo halo() const override { return Halo::uniform(1); }
    DataType outputType(DataType) const override { return DataType::Int32; }
    int32_t outputPlaneCount(int32_t) const override { return 1; }
    bool producesObjects() const override { return true; }
    bool objectsAreEightConnected() const override { return eightConnected_; }
    const OperationParameters& parameters() const override { return parameters_; }
    bool finishObject(ObjectRecord& record, const ObjectFinishContext& context) const override {
        return plan_.finish(record, context);
    }

    void apply(OperationContext& context) override {
        const BlockView& in = context.input();
        const Rect64 tile = context.tileRectAtLevel();
        const Rect64 halo = context.haloRectAtLevel();
        const int64_t width = tile.width;
        const int64_t height = tile.height;
        auto foreground = [&](int64_t x, int64_t y) {
            const auto* row = static_cast<const uint8_t*>(in.planeData(0)) +
                              (y + tile.y - halo.y) * in.planeRowStrideBytes(0);
            return row[x + tile.x - halo.x] != 0;
        };
        std::vector<int32_t> labels(static_cast<size_t>(width * height), 0);
        int32_t next = 0;
        for (int64_t y = 0; y < height; ++y) {
            for (int64_t x = 0; x < width; ++x) {
                if (!foreground(x, y) || labels[static_cast<size_t>(y * width + x)] != 0) {
                    continue;
                }
                const int32_t label = ++next;
                std::vector<Point64> stack{Point64{x, y}};
                labels[static_cast<size_t>(y * width + x)] = label;
                while (!stack.empty()) {
                    const Point64 p = stack.back();
                    stack.pop_back();
                    const Point64 steps[] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {-1, 1}, {1, -1}, {-1, -1}};
                    for (size_t k = 0; k < (eightConnected_ ? 8u : 4u); ++k) {
                        const Point64 s = steps[k];
                        const int64_t nx = p.x + s.x;
                        const int64_t ny = p.y + s.y;
                        if (nx >= 0 && ny >= 0 && nx < width && ny < height && foreground(nx, ny) &&
                            labels[static_cast<size_t>(ny * width + nx)] == 0) {
                            labels[static_cast<size_t>(ny * width + nx)] = label;
                            stack.push_back(Point64{nx, ny});
                        }
                    }
                }
            }
        }
        const std::vector<detail::TracedRegion> regions =
            detail::traceRegions(labels.data(), width, height, width, eightConnected_);
        detail::emitRegions(context, labels.data(), width, regions, in, 1);
    }

private:
    bool eightConnected_;
    OperationParameters parameters_;
    detail::MeasurementPlan plan_;
};

} // namespace

TEST(RegionEmit, InteriorRegionIsAFinishedObjectWithPartials) {
    RegionsOperation op;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    // Object A: pixels x 2..4, y 2..3, intensity x + 10 y except a NaN at (3, 2).
    // Object B: x 8..9 at y 5, touching the tile's right edge.
    auto pixel = [nan](int32_t plane, int64_t x, int64_t y) {
        const bool a = x >= 2 && x <= 4 && y >= 2 && y <= 3;
        const bool b = x >= 8 && y == 5;
        if (plane == 0) {
            return (a || b) ? 1.0 : 0.0;
        }
        if (x == 3 && y == 2) {
            return nan;
        }
        return static_cast<double>(x + 10 * y);
    };
    const ts::DirectObjects result =
        ts::applyDirectObjects(op, {DataType::Byte, DataType::Float32}, {0, 7}, 10, 8, pixel, 0.5);

    ASSERT_EQ(result.objects.size(), 1u);
    const ObjectRecord& a = result.objects[0];
    EXPECT_EQ(a.layer(), ts::objectsLayer());
    EXPECT_EQ(a.geometry().anchor(), (Point64{2, 2}));
    EXPECT_EQ(measurement(a, "area"), 6.0);
    EXPECT_EQ(measurement(a, "perimeter"), 10.0);
    EXPECT_EQ(measurement(a, "area.um2"), 1.5);
    EXPECT_EQ(measurement(a, "centroid.x"), 3.5);
    EXPECT_EQ(measurement(a, "centroid.y"), 3.0);
    // Five finite samples: 22, 24, 32, 33, 34.
    EXPECT_DOUBLE_EQ(measurement(a, "intensity.7.mean"), (22.0 + 24.0 + 32.0 + 33.0 + 34.0) / 5.0);
    EXPECT_EQ(measurement(a, "intensity.7.min"), 22.0);
    EXPECT_EQ(measurement(a, "intensity.7.max"), 34.0);
    EXPECT_FALSE(has(a, "intensity.0.mean"));

    ASSERT_EQ(result.fragments.size(), 1u);
    EXPECT_EQ(result.fragments[0].boundingBox(), (Rect64{8, 5, 2, 1}));
}

TEST(RegionEmit, NoIntensityPlanesMeansNoIntensityMeasurements) {
    RegionsOperation op;
    auto pixel = [](int32_t, int64_t x, int64_t y) { return (x >= 1 && x <= 2 && y >= 1 && y <= 2) ? 1.0 : 0.0; };
    const ts::DirectObjects result = ts::applyDirectObjects(op, {DataType::Byte}, {0}, 6, 6, pixel);
    ASSERT_EQ(result.objects.size(), 1u);
    EXPECT_EQ(measurement(result.objects[0], "area"), 4.0);
    EXPECT_FALSE(has(result.objects[0], "area.um2"));
    for (int32_t i = 0; i < result.objects[0].measurements().count(); ++i) {
        EXPECT_NE(std::string(result.objects[0].measurements().at(i).name()).rfind("intensity.", 0), 0u);
    }
    EXPECT_TRUE(result.fragments.empty());
}

TEST(RegionEmit, RunEagerObjectsCollectsMergedObjectsById) {
    ts::registerTestOperations();
    // Two squares, one across the seams of 32-pixel tiles.
    auto pixelFunction = [](int, int64_t x, int64_t y) {
        const bool first = x >= 20 && x < 50 && y >= 20 && y < 50;
        const bool second = x >= 70 && x < 80 && y >= 5 && y < 15;
        return (first || second) ? 255.0 : 0.0;
    };
    auto source = std::make_shared<ts::SyntheticSource>(std::vector<ts::SyntheticLevel>{{128, 96, 0, 0}},
                                                        std::vector<slideio::DataType>{slideio::DataType::DT_Byte},
                                                        pixelFunction);
    const std::shared_ptr<slideio::Scene> scene = ts::makeSyntheticScene(source);
    PipelineSpec spec;
    OperationParameters seams;
    seams.setBool("seams", true);
    spec.operation("test.components", seams);
    for (int32_t tileSize : {32, 128}) {
        const std::vector<ObjectRecord> records = ts::runEagerObjects(scene, spec, tileSize, 2);
        ASSERT_EQ(records.size(), 2u) << tileSize;
        EXPECT_TRUE(records[0].id() < records[1].id());
        // Ids order by anchor (y first): the second square is the higher one.
        EXPECT_EQ(records[0].geometry().anchor(), (Point64{70, 5}));
        EXPECT_EQ(measurement(records[0], "area"), 100.0);
        EXPECT_EQ(measurement(records[1], "area"), 900.0);
        EXPECT_EQ(records[1].layer(), ts::objectsLayer());
    }
}

TEST(RegionEmit, RingsMatchTheReferenceTracerPointForPoint) {
    // A random mask with a clear border, traced by test.components (the rings the
    // seam merger's linking is checked against) and by the shared tracer: the same
    // objects, ring for ring, 4- and 8-connected.
    uint32_t state = 12345u;
    std::vector<uint8_t> mask(48 * 40, 0);
    for (int64_t y = 1; y < 39; ++y) {
        for (int64_t x = 1; x < 47; ++x) {
            state = state * 1664525u + 1013904223u;
            mask[static_cast<size_t>(y * 48 + x)] = (state >> 24) < 120u ? 1 : 0;
        }
    }
    auto pixel = [&mask](int32_t, int64_t x, int64_t y) { return mask[static_cast<size_t>(y * 48 + x)] * 255.0; };
    for (bool eight : {false, true}) {
        OperationParameters parameters;
        parameters.setBool("eightConnected", eight);
        ts::ComponentsOperation reference(parameters);
        RegionsOperation shared(eight);
        const ts::DirectObjects expected = ts::applyDirectObjects(reference, {DataType::Byte}, {0}, 48, 40, pixel);
        const ts::DirectObjects actual = ts::applyDirectObjects(shared, {DataType::Byte}, {0}, 48, 40, pixel);
        ASSERT_GT(expected.objects.size(), 20u) << eight;
        ASSERT_EQ(actual.objects.size(), expected.objects.size()) << eight;
        EXPECT_TRUE(actual.fragments.empty());
        bool sawPinchedRing = false;
        for (size_t i = 0; i < expected.objects.size(); ++i) {
            const Polygon& e = expected.objects[i].geometry();
            const Polygon& a = actual.objects[i].geometry();
            ASSERT_EQ(a.ringCount(), e.ringCount()) << eight << " object " << i;
            for (int32_t ring = 0; ring < e.ringCount(); ++ring) {
                std::vector<Point64> ep(static_cast<size_t>(e.ringPointCount(ring)));
                std::vector<Point64> ap(static_cast<size_t>(a.ringPointCount(ring)));
                e.ringPoints(ring, ep.data(), static_cast<int64_t>(ep.size()));
                a.ringPoints(ring, ap.data(), static_cast<int64_t>(ap.size()));
                EXPECT_EQ(a.ringIsHole(ring), e.ringIsHole(ring)) << eight << " object " << i;
                EXPECT_TRUE(ap == ep) << eight << " object " << i << " ring " << ring;
                for (const Point64& p : ep) {
                    sawPinchedRing = sawPinchedRing || std::count(ep.begin(), ep.end(), p) > 1;
                }
            }
            EXPECT_EQ(measurement(actual.objects[i], "area"), measurement(expected.objects[i], "area"));
            EXPECT_EQ(measurement(actual.objects[i], "perimeter"), measurement(expected.objects[i], "perimeter"));
        }
        // The pattern exercises the pinch rule in both modes.
        EXPECT_TRUE(sawPinchedRing) << eight;
    }
}
