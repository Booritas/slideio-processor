// Requirements: UR-SEG-8, UR-SEG-3, UR-PIX-7, UR-PIX-8
#include <slideio/processor/algorithm/pixel/filter.hpp>
#include <slideio/processor/algorithm/pixel/threshold.hpp>
#include <slideio/processor/algorithm/segment/segment.hpp>
#include <slideio/processor/core/measurement.hpp>
#include <slideio/processor/core/object_record.hpp>
#include <slideio/processor/pipeline/pipeline_spec.hpp>

#include <algorithm/support/harness.hpp>
#include <pipeline/test_support/synthetic_scene.hpp>

#include <slideio/slideio/scene.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using namespace slideio::processor;
namespace ts = slideio::processor::testing_support;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int64_t kWidth = 320;
constexpr int64_t kHeight = 280;
constexpr int64_t kRadius = 9;

struct Centre {
    int64_t x;
    int64_t y;
};

double measurement(const ObjectRecord& record, const char* name) {
    Measurement m;
    return record.measurements().find(name, m) ? m.value() : std::numeric_limits<double>::quiet_NaN();
}

bool has(const ObjectRecord& record, const char* name) {
    Measurement m;
    return record.measurements().find(name, m);
}

std::vector<Point64> ringOf(const Polygon& polygon, int32_t ring) {
    std::vector<Point64> points(static_cast<size_t>(polygon.ringPointCount(ring)));
    polygon.ringPoints(ring, points.data(), static_cast<int64_t>(points.size()));
    return points;
}

bool sameGeometry(const Polygon& a, const Polygon& b) {
    if (a.ringCount() != b.ringCount()) {
        return false;
    }
    for (int32_t r = 0; r < a.ringCount(); ++r) {
        if (a.ringIsHole(r) != b.ringIsHole(r)) {
            return false;
        }
        const std::vector<Point64> pa = ringOf(a, r);
        const std::vector<Point64> pb = ringOf(b, r);
        if (pa.size() != pb.size() || !std::equal(pa.begin(), pa.end(), pb.begin())) {
            return false;
        }
    }
    return true;
}

// Expects @p objects to equal @p reference object by object: ids, layers,
// rings point for point and every measurement exactly.
void expectSameObjects(const std::vector<ObjectRecord>& reference, const std::vector<ObjectRecord>& objects,
                       const std::string& label) {
    ASSERT_EQ(objects.size(), reference.size()) << label;
    for (size_t i = 0; i < reference.size(); ++i) {
        const ObjectRecord& want = reference[i];
        const ObjectRecord& got = objects[i];
        ASSERT_TRUE(got.id() == want.id()) << label << ", object " << i;
        EXPECT_EQ(got.layer(), want.layer()) << label << ", object " << i;
        EXPECT_TRUE(sameGeometry(got.geometry(), want.geometry())) << label << ", object " << i;
        ASSERT_EQ(got.measurements().count(), want.measurements().count()) << label << ", object " << i;
        for (int32_t k = 0; k < want.measurements().count(); ++k) {
            const Measurement m = want.measurements().at(k);
            ASSERT_TRUE(has(got, m.name())) << label << ", object " << i << ": " << m.name();
            EXPECT_EQ(measurement(got, m.name()), m.value()) << label << ", object " << i << ": " << m.name();
        }
    }
}

// Six pairs of touching discs, three columns by two rows, alternating
// horizontal and vertical pairs, centres 14 apart. Tile seams of 64, 100 and
// 257 pixels cut through several of them.
std::vector<Centre> discCentres() {
    std::vector<Centre> centres;
    for (int64_t row = 0; row < 2; ++row) {
        for (int64_t column = 0; column < 3; ++column) {
            const int64_t i = row * 3 + column;
            const int64_t cx = 60 + 105 * column - 15 * (column / 2) + (i * 3) % 7 - 3;
            const int64_t cy = 70 + 130 * row + (i * 5) % 7 - 3;
            if (i % 2 == 0) {
                centres.push_back({cx - 7, cy});
                centres.push_back({cx + 7, cy});
            } else {
                centres.push_back({cx, cy - 7});
                centres.push_back({cx, cy + 7});
            }
        }
    }
    return centres;
}

bool inDisc(const Centre& c, int64_t x, int64_t y) {
    const int64_t dx = x - c.x;
    const int64_t dy = y - c.y;
    return dx * dx + dy * dy <= kRadius * kRadius;
}

bool inPaleCentre(const Centre& c, int64_t x, int64_t y) {
    return std::abs(x - c.x) <= 1 && std::abs(y - c.y) <= 1;
}

bool inAnyDisc(const std::vector<Centre>& centres, int64_t x, int64_t y) {
    return std::any_of(centres.begin(), centres.end(), [x, y](const Centre& c) { return inDisc(c, x, y); });
}

// 0 in any disc's 3 x 3 pale centre (tested first, so all 12 discs have one,
// even where a neighbour covers it), else 200 inside a disc, else 0.
double ringNucleus(const std::vector<Centre>& centres, int64_t x, int64_t y) {
    if (std::any_of(centres.begin(), centres.end(), [x, y](const Centre& c) { return inPaleCentre(c, x, y); })) {
        return 0.0;
    }
    return inAnyDisc(centres, x, y) ? 200.0 : 0.0;
}

} // namespace

TEST(NucleiChain, TouchingRingNucleiSeparatedAtThreeTileSizes) {
    const std::vector<Centre> centres = discCentres();
    ASSERT_EQ(centres.size(), 12u);
    const auto scene = ts::makeSyntheticScene(
        {{kWidth, kHeight, 0, 0}}, {slideio::DataType::DT_Byte},
        [centres](int, int64_t x, int64_t y) { return ringNucleus(centres, x, y); });

    PipelineSpec spec;
    spec.then(makeFixedThreshold(100.0, ThresholdDirection::Above))
        .then(makeFillHoles(8))
        .then(makeDistanceTransform(12))
        .then(makeWatershed(WatershedParameters()
                                .minimumMarkerDistancePx(5)
                                .maximumMarkerSpacingPx(16)
                                .minimumMarkerValue(3.0)));

    const std::vector<ObjectRecord> reference = ts::runEagerObjects(scene, spec, 4096, 4);
    ASSERT_EQ(reference.size(), 12u);
    const double expectedArea = kPi * static_cast<double>(kRadius * kRadius);
    std::vector<bool> claimed(centres.size(), false);
    for (size_t i = 0; i < reference.size(); ++i) {
        const ObjectRecord& object = reference[i];
        EXPECT_NEAR(measurement(object, "area"), expectedArea, 0.1 * expectedArea) << "object " << i;
        // The centroid is in pixel-corner coordinates: pixel (x, y) spans [x, x + 1).
        const double cx = measurement(object, "centroid.x");
        const double cy = measurement(object, "centroid.y");
        int matches = 0;
        for (size_t d = 0; d < centres.size(); ++d) {
            if (std::abs(cx - (static_cast<double>(centres[d].x) + 0.5)) <= 1.5 &&
                std::abs(cy - (static_cast<double>(centres[d].y) + 0.5)) <= 1.5) {
                ++matches;
                EXPECT_FALSE(claimed[d]) << "disc " << d << " matched twice";
                claimed[d] = true;
            }
        }
        EXPECT_EQ(matches, 1) << "object " << i << " centroid " << cx << ", " << cy;
    }
    EXPECT_TRUE(std::all_of(claimed.begin(), claimed.end(), [](bool b) { return b; }));

    // fill_holes matters: every object is a single ring (no hole ring), and the
    // areas add up to exactly the pixels of the union of the 12 filled discs.
    int64_t unionPixels = 0;
    for (int64_t y = 0; y < kHeight; ++y) {
        for (int64_t x = 0; x < kWidth; ++x) {
            unionPixels += inAnyDisc(centres, x, y) ? 1 : 0;
        }
    }
    double totalArea = 0.0;
    for (const ObjectRecord& object : reference) {
        EXPECT_EQ(object.geometry().ringCount(), 1);
        totalArea += measurement(object, "area");
    }
    EXPECT_EQ(totalArea, static_cast<double>(unionPixels));

    for (const int32_t tile : {64, 100, 257}) {
        const auto cut = std::count_if(reference.begin(), reference.end(), [tile](const ObjectRecord& r) {
            const Rect64 b = r.boundingBox();
            return b.x / tile != (b.x + b.width - 1) / tile || b.y / tile != (b.y + b.height - 1) / tile;
        });
        ASSERT_GE(cut, 1) << tile << "-pixel seams should cut an object";
        expectSameObjects(reference, ts::runEagerObjects(scene, spec, tile, 4), std::to_string(tile) + "-pixel tiles");
    }
}
