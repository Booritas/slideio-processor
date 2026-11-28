// Requirements: UR-SEG-8, UR-QA-2, UR-QA-3
#include <slideio/processor/algorithm/segment/segment.hpp>
#include <slideio/processor/core/data_type.hpp>

#include <algorithm/support/flood_model.hpp>
#include <algorithm/support/harness.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <vector>

using namespace slideio::processor;
namespace ts = slideio::processor::testing_support;

namespace {

int64_t packedKey(int64_t x, int64_t y) { return (y << 31) | x; }

// The distance transform of TouchingDiscsSeparated (test_watershed.cpp): two
// discs of radius 10 centred at (24, 24) and (40, 24) in a 64 x 48 image, each
// disc pixel getting the distance to the nearest pixel centre outside both
// discs, brute force over the whole image (the outside of the image counts as
// background but lies further than any disc pixel's nearest background here).
std::vector<double> touchingDiscs(int64_t width, int64_t height) {
    const auto inside = [](int64_t x, int64_t y) {
        return (x - 24) * (x - 24) + (y - 24) * (y - 24) <= 100 || (x - 40) * (x - 40) + (y - 24) * (y - 24) <= 100;
    };
    std::vector<double> distance(static_cast<size_t>(width * height), 0.0);
    for (int64_t y = 0; y < height; ++y) {
        for (int64_t x = 0; x < width; ++x) {
            if (!inside(x, y)) {
                continue;
            }
            int64_t best = std::numeric_limits<int64_t>::max();
            for (int64_t v = -1; v <= height; ++v) {
                for (int64_t u = -1; u <= width; ++u) {
                    const bool outside = u < 0 || v < 0 || u >= width || v >= height;
                    if (outside || !inside(u, v)) {
                        best = std::min(best, (u - x) * (u - x) + (v - y) * (v - y));
                    }
                }
            }
            distance[static_cast<size_t>(y * width + x)] = std::sqrt(static_cast<double>(best));
        }
    }
    return distance;
}

// The strip of test_watershed.cpp's stripPixel: rows 4 and 5 of a 150 x 10
// image, x < 150, falling from its one maximum at x = 0.
constexpr int64_t kStripWidth = 150;
constexpr int64_t kStripHeight = 10;

std::vector<double> strip() {
    std::vector<double> values(static_cast<size_t>(kStripWidth * kStripHeight), 0.0);
    for (int64_t y = 4; y < 6; ++y) {
        for (int64_t x = 0; x < kStripWidth; ++x) {
            values[static_cast<size_t>(y * kStripWidth + x)] = 200.0 - static_cast<double>(x);
        }
    }
    return values;
}

ts::ModelParameters stripParameters(ts::ModelRule rule) {
    ts::ModelParameters p;
    p.markerDistance = 2;
    p.spacing = 8;
    p.rule = rule;
    return p;
}

// Expects every strip pixel to be @p want(x) and every other pixel background.
template <typename Want>
void expectStrip(const ts::ModelResult& result, Want want, const char* label) {
    ASSERT_EQ(result.key.size(), static_cast<size_t>(kStripWidth * kStripHeight)) << label;
    for (int64_t y = 0; y < kStripHeight; ++y) {
        for (int64_t x = 0; x < kStripWidth; ++x) {
            const int64_t expected = y == 4 || y == 5 ? want(x) : ts::kModelBackground;
            ASSERT_EQ(result.key[static_cast<size_t>(y * kStripWidth + x)], expected)
                << label << " at (" << x << ", " << y << ")";
        }
    }
}

} // namespace

TEST(FloodModel, MatchesSingleTileOnTouchingDiscs) {
    constexpr int64_t kWidth = 64;
    constexpr int64_t kHeight = 48;
    const std::vector<double> dt = touchingDiscs(kWidth, kHeight);

    const ts::ModelResult model = ts::modelWatershed(dt, kWidth, kHeight, ts::ModelParameters());
    EXPECT_FALSE(model.refused);

    // The discs lie inside the image, so one direct application is the single tile.
    const OperationPtr op = makeWatershed(WatershedParameters());
    const ts::Image labels = ts::applyDirect(*op, {DataType::Float32}, {0}, kWidth, kHeight,
                                             [&](int32_t, int64_t x, int64_t y) {
                                                 return dt[static_cast<size_t>(y * kWidth + x)];
                                             });
    ASSERT_EQ(labels.types, std::vector<DataType>{DataType::Int32});

    // The same partition of the foreground: a bijection between labels and keys,
    // and background (label 0) exactly where the model has background.
    std::map<int64_t, int64_t> keyOfLabel;
    std::map<int64_t, int64_t> labelOfKey;
    for (int64_t y = 0; y < kHeight; ++y) {
        for (int64_t x = 0; x < kWidth; ++x) {
            const int64_t key = model.key[static_cast<size_t>(y * kWidth + x)];
            const auto label = static_cast<int64_t>(labels.at(0, x, y));
            ASSERT_NE(key, ts::kModelDropped) << "(" << x << ", " << y << ")";
            ASSERT_EQ(label == 0, key == ts::kModelBackground) << "(" << x << ", " << y << ")";
            if (label == 0) {
                continue;
            }
            const auto byLabel = keyOfLabel.emplace(label, key).first;
            const auto byKey = labelOfKey.emplace(key, label).first;
            ASSERT_EQ(byLabel->second, key) << "label " << label << " at (" << x << ", " << y << ")";
            ASSERT_EQ(byKey->second, label) << "key " << key << " at (" << x << ", " << y << ")";
        }
    }
    EXPECT_EQ(keyOfLabel.size(), 2u);
    EXPECT_NE(model.key[static_cast<size_t>(24 * kWidth + 24)], model.key[static_cast<size_t>(24 * kWidth + 40)]);
}

TEST(FloodModel, StripRules) {
    const std::vector<double> values = strip();
    // The one marker is (0, 4): the topmost of the two equal maxima at x = 0.
    const int64_t marker = packedKey(0, 4);
    const auto kept = [marker](int64_t x) { return x <= 8 ? marker : ts::kModelDropped; };

    EXPECT_TRUE(ts::modelWatershed(values, kStripWidth, kStripHeight, stripParameters(ts::ModelRule::Fail)).refused);

    const ts::ModelResult plain =
        ts::modelWatershed(values, kStripWidth, kStripHeight, stripParameters(ts::ModelRule::DropPlain));
    EXPECT_FALSE(plain.refused);
    expectStrip(plain, kept, "DropPlain");

    const ts::ModelResult bounded =
        ts::modelWatershed(values, kStripWidth, kStripHeight, stripParameters(ts::ModelRule::DropBounded));
    EXPECT_FALSE(bounded.refused);
    expectStrip(bounded, kept, "DropBounded");

    // No marker: a floor above every value, or anchors that are all -1.
    const auto dropped = [](int64_t) { return ts::kModelDropped; };
    for (const ts::ModelRule rule : {ts::ModelRule::Fail, ts::ModelRule::DropPlain, ts::ModelRule::DropBounded}) {
        ts::ModelParameters floored = stripParameters(rule);
        floored.floor = 1000.0;
        ts::ModelParameters unanchored = stripParameters(rule);
        unanchored.anchors.assign(values.size(), -1);
        for (const ts::ModelParameters& p : {floored, unanchored}) {
            const ts::ModelResult none = ts::modelWatershed(values, kStripWidth, kStripHeight, p);
            EXPECT_EQ(none.refused, rule == ts::ModelRule::Fail);
            expectStrip(none, dropped, "no marker");
        }
    }

    // Anchors: one basin per anchor value, its seed the anchor pixel itself.
    ts::ModelParameters anchored = stripParameters(ts::ModelRule::DropPlain);
    anchored.anchors.assign(values.size(), -1);
    anchored.anchors[static_cast<size_t>(4 * kStripWidth + 20)] = 7;
    const ts::ModelResult fromAnchor = ts::modelWatershed(values, kStripWidth, kStripHeight, anchored);
    expectStrip(fromAnchor, [](int64_t x) { return x >= 12 && x <= 28 ? int64_t{7} : ts::kModelDropped; },
                "anchor at x = 20");
}
