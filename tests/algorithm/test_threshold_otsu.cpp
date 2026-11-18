// Requirements: UR-PIX-5, UR-QA-3, UR-EXE-1, UR-ALG-6
#include <slideio/processor/algorithm/pixel/threshold.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/raster_mask.hpp>
#include <slideio/processor/pipeline/operation_catalog.hpp>
#include <slideio/processor/pipeline/pipeline_spec.hpp>

#include <algorithm/support/harness.hpp>
#include <pipeline/detail/parameters_json.hpp>
#include <pipeline/test_support/synthetic_scene.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <vector>

using namespace slideio::processor;
namespace ts = slideio::processor::testing_support;

namespace {

constexpr const char* kName = "pixel.threshold_otsu";
constexpr int64_t kWidth = 640;
constexpr int64_t kHeight = 480;

// The message of the InvalidArgumentError creating pixel.threshold_otsu from @p p throws; "" when none.
std::string rejection(const OperationParameters& p) {
    try {
        OperationCatalog::instance().create(kName, p);
    } catch (const InvalidArgumentError& e) {
        return e.what();
    }
    return "";
}

// A textured image of three populations around 40, 120 and 200, in equal shares
// towards the top left and without the middle one towards the bottom right, so
// a restriction to the top left moves the threshold.
double trimodal(int, int64_t x, int64_t y) {
    static const double centres[] = {40.0, 120.0, 200.0};
    int64_t mode = ((x / 53) + (y / 41) + (x * y / 997)) % 3;
    if (x + y > 640 && mode == 1) {
        mode = 2;
    }
    const double base = centres[mode];
    const int64_t noise = ((x * 7919 + y * 104729) ^ (x * y)) % 31 - 15;
    return std::min(255.0, std::max(0.0, base + static_cast<double>(noise)));
}

// Otsu over bin indices, written out the straightforward way: for each split k,
// sum both classes afresh. The smallest k maximising the between-class variance
// over splits with two non-empty classes; the highest non-empty bin when there is
// no such split; -2 when nothing was counted.
int64_t referenceOtsu(const std::vector<int64_t>& hist) {
    const auto n = static_cast<int64_t>(hist.size());
    int64_t total = 0;
    for (int64_t c : hist) {
        total += c;
    }
    if (total == 0) {
        return -2;
    }
    int64_t best = -1;
    double bestValue = -1.0;
    for (int64_t k = 0; k <= n - 2; ++k) {
        int64_t c0 = 0;
        int64_t c1 = 0;
        double s0 = 0.0;
        double s1 = 0.0;
        for (int64_t i = 0; i < n; ++i) {
            if (i <= k) {
                c0 += hist[static_cast<size_t>(i)];
                s0 += static_cast<double>(i) * static_cast<double>(hist[static_cast<size_t>(i)]);
            } else {
                c1 += hist[static_cast<size_t>(i)];
                s1 += static_cast<double>(i) * static_cast<double>(hist[static_cast<size_t>(i)]);
            }
        }
        if (c0 == 0 || c1 == 0) {
            continue;
        }
        const double w0 = static_cast<double>(c0) / static_cast<double>(total);
        const double w1 = static_cast<double>(c1) / static_cast<double>(total);
        const double d = s0 / static_cast<double>(c0) - s1 / static_cast<double>(c1);
        const double value = w0 * w1 * d * d;
        if (value > bestValue) {
            bestValue = value;
            best = k;
        }
    }
    if (best >= 0) {
        return best;
    }
    for (int64_t i = n - 1; i >= 0; --i) {
        if (hist[static_cast<size_t>(i)] != 0) {
            return i;
        }
    }
    return -2;
}

// The bin of a sample over [lo, hi] in n bins, as Histogram64 bins it: -1 below
// the range, n above it.
int64_t binOf(double v, double lo, double hi, int64_t n) {
    if (v < lo) {
        return -1;
    }
    if (v > hi) {
        return n;
    }
    auto index = static_cast<int64_t>(std::floor((v - lo) / (hi - lo) * static_cast<double>(n)));
    return std::min(n - 1, std::max<int64_t>(0, index));
}

PipelineSpec otsuSpec(const OperationPtr& op) {
    PipelineSpec spec;
    spec.then(op);
    return spec;
}

Polygon triangle(int64_t x0, int64_t y0, int64_t x1, int64_t y1) {
    const Point64 points[] = {{x0, y0}, {x1, y0}, {x0, y1}};
    Polygon p;
    p.addRing(points, 3, false);
    return p;
}

// Builds the in-memory histogram of a Byte image over the pixels @p admitted
// accepts, computes k* and checks every run (workers x tile sizes) against
// thresholding at k*, on the admitted pixels.
void expectStreamedEqualsInMemory(const std::shared_ptr<slideio::Scene>& scene, const PipelineSpec& spec,
                                  const std::function<double(int64_t, int64_t)>& value,
                                  const std::function<bool(int64_t, int64_t)>& admitted) {
    std::vector<int64_t> hist(256, 0);
    for (int64_t y = 0; y < kHeight; ++y) {
        for (int64_t x = 0; x < kWidth; ++x) {
            if (admitted(x, y)) {
                ++hist[static_cast<size_t>(value(x, y))];
            }
        }
    }
    const int64_t k = referenceOtsu(hist);
    ASSERT_GE(k, 0);
    for (int32_t workers : {1, 4, 8}) {
        for (int32_t tile : {64, 100, 257}) {
            SCOPED_TRACE("workers " + std::to_string(workers) + " tile " + std::to_string(tile));
            const ts::Image out = ts::runEager(scene, spec, tile, workers);
            ASSERT_EQ(out.types, std::vector<DataType>{DataType::Byte});
            int64_t mismatches = 0;
            for (int64_t y = 0; y < kHeight; ++y) {
                for (int64_t x = 0; x < kWidth; ++x) {
                    if (!admitted(x, y)) {
                        continue;
                    }
                    const double expected = static_cast<int64_t>(value(x, y)) > k ? 255.0 : 0.0;
                    if (out.at(0, x, y) != expected) {
                        ++mismatches;
                    }
                }
            }
            EXPECT_EQ(mismatches, 0);
        }
    }
}

} // namespace

TEST(ThresholdOtsu, FactoryEqualsNamePath) {
    // Without a range: the bag holds direction only.
    const OperationPtr typed = makeOtsuThreshold(ThresholdDirection::Below);
    const OperationPtr named = OperationCatalog::instance().create(kName, OperationParameters().setString("direction", "Below"));
    EXPECT_TRUE(detail::parametersEqual(typed->parameters(), named->parameters()));
    EXPECT_EQ(typed->parameters().count(), 1);
    EXPECT_STREQ(typed->parameters().getString("direction", nullptr), "Below");
    const OperationPtr defaults = OperationCatalog::instance().create(kName, OperationParameters());
    EXPECT_TRUE(detail::parametersEqual(defaults->parameters(), makeOtsuThreshold(ThresholdDirection::Above)->parameters()));

    // With a range: the bag is completed with binCount and round-trips.
    const OperationPtr ranged = OperationCatalog::instance().create(
        kName, OperationParameters().setDouble("histogramMinimum", 0.0).setDouble("histogramMaximum", 1.0));
    const OperationParameters& bag = ranged->parameters();
    EXPECT_EQ(bag.count(), 4);
    int64_t bins = 0;
    ASSERT_TRUE(bag.getInt("binCount", bins));
    EXPECT_EQ(bins, 256);
    EXPECT_STREQ(bag.getString("direction", nullptr), "Above");
    const OperationPtr replayed = OperationCatalog::instance().create(kName, bag);
    EXPECT_TRUE(detail::parametersEqual(replayed->parameters(), bag));
}

TEST(ThresholdOtsu, DeclaresItsContract) {
    const OperationPtr op = makeOtsuThreshold(ThresholdDirection::Above);
    EXPECT_STREQ(op->name(), kName);
    EXPECT_EQ(op->arity(), ChannelArity::PerChannel);
    EXPECT_TRUE(op->needsGlobalContext());
    EXPECT_TRUE(op->supportedTypes() == DataTypeSet::all());
    EXPECT_TRUE(op->halo().isEmpty());
    for (int32_t i = 0; i < 11; ++i) {
        EXPECT_EQ(op->outputType(static_cast<DataType>(i)), DataType::Byte);
    }
    EXPECT_STREQ(OperationCatalog::instance().correctnessReference(kName),
                 "Otsu N. A threshold selection method from gray-level histograms. IEEE Trans SMC 1979;9:62-66; "
                 "equals the threshold of a full in-memory histogram of the same slide");
    EXPECT_STREQ(OperationCatalog::instance().complexityNote(kName),
                 "O(n) in the number of samples, plus O(bins) once per channel; two passes over the slide");
}

TEST(ThresholdOtsu, RejectsBadParameters) {
    const std::string unknown = rejection(OperationParameters().setString("directoin", "Above"));
    EXPECT_NE(unknown.find("directoin"), std::string::npos) << unknown;
    EXPECT_NE(unknown.find(kName), std::string::npos) << unknown;

    // Half a range, a binCount without a range, an inverted or non-finite range, binCount out of bounds.
    EXPECT_FALSE(rejection(OperationParameters().setDouble("histogramMinimum", 0.0)).empty());
    EXPECT_FALSE(rejection(OperationParameters().setDouble("histogramMaximum", 1.0)).empty());
    EXPECT_FALSE(rejection(OperationParameters().setInt("binCount", 16)).empty());
    EXPECT_FALSE(rejection(OperationParameters().setDouble("histogramMinimum", 1.0).setDouble("histogramMaximum", 1.0))
                     .empty());
    // A bag cannot hold a non-finite double, but a finite range can span more than a double holds.
    EXPECT_FALSE(
        rejection(OperationParameters().setDouble("histogramMinimum", -1e308).setDouble("histogramMaximum", 1e308))
            .empty());
    const std::string tooFew =
        rejection(OperationParameters().setDouble("histogramMinimum", 0.0).setDouble("histogramMaximum", 1.0).setInt(
            "binCount", 1));
    EXPECT_NE(tooFew.find("binCount"), std::string::npos) << tooFew;
    EXPECT_FALSE(rejection(OperationParameters()
                               .setDouble("histogramMinimum", 0.0)
                               .setDouble("histogramMaximum", 1.0)
                               .setInt("binCount", 1048577))
                     .empty());
    EXPECT_TRUE(rejection(OperationParameters()
                              .setDouble("histogramMinimum", 0.0)
                              .setDouble("histogramMaximum", 1.0)
                              .setInt("binCount", 1048576))
                    .empty());
    EXPECT_FALSE(rejection(OperationParameters().setDouble("binCount", 2.0).setDouble("histogramMinimum", 0.0).setDouble(
                               "histogramMaximum", 1.0))
                     .empty());
    EXPECT_THROW(makeOtsuThreshold(static_cast<ThresholdDirection>(7)), InvalidArgumentError);
}

TEST(ThresholdOtsu, CombineRefusesAnotherKindOrShape) {
    const OperationPtr otsu = makeOtsuThreshold(ThresholdDirection::Above);
    const OperationPtr fixed = makeFixedThreshold(1.0, ThresholdDirection::Above);
    try {
        otsu->combine(*fixed);
        FAIL() << "combine accepted another kind of operation";
    } catch (const InvalidArgumentError& e) {
        EXPECT_NE(std::string(e.what()).find(kName), std::string::npos) << e.what();
    }
}

TEST(ThresholdOtsu, BimodalAnalytic) {
    const auto pixel = [](int, int64_t x, int64_t y) { return ((x / 7 + y / 5) % 2) == 0 ? 60.0 : 180.0; };
    auto scene = ts::makeSyntheticScene({{200, 150, 0, 0}}, {slideio::DataType::DT_Byte}, pixel);
    const ts::Image out = ts::runEager(scene, otsuSpec(makeOtsuThreshold(ThresholdDirection::Above)), 64, 4);
    ASSERT_EQ(out.types, std::vector<DataType>{DataType::Byte});
    int64_t mismatches = 0;
    for (int64_t y = 0; y < 150; ++y) {
        for (int64_t x = 0; x < 200; ++x) {
            if (out.at(0, x, y) != (pixel(0, x, y) == 180.0 ? 255.0 : 0.0)) {
                ++mismatches;
            }
        }
    }
    EXPECT_EQ(mismatches, 0);

    const ts::Image below = ts::runEager(scene, otsuSpec(makeOtsuThreshold(ThresholdDirection::Below)), 100, 2);
    mismatches = 0;
    for (int64_t y = 0; y < 150; ++y) {
        for (int64_t x = 0; x < 200; ++x) {
            if (below.at(0, x, y) != (pixel(0, x, y) == 60.0 ? 255.0 : 0.0)) {
                ++mismatches;
            }
        }
    }
    EXPECT_EQ(mismatches, 0);
}

TEST(ThresholdOtsu, StreamedEqualsInMemory) {
    auto scene = ts::makeSyntheticScene({{kWidth, kHeight, 0, 0}}, {slideio::DataType::DT_Byte}, trimodal);
    expectStreamedEqualsInMemory(
        scene, otsuSpec(makeOtsuThreshold(ThresholdDirection::Above)), [](int64_t x, int64_t y) { return trimodal(0, x, y); },
        [](int64_t, int64_t) { return true; });
}

TEST(ThresholdOtsu, RestrictedCountsOnlyAdmittedPixels) {
    auto scene = ts::makeSyntheticScene({{kWidth, kHeight, 0, 0}}, {slideio::DataType::DT_Byte}, trimodal);
    const Polygon polygon = triangle(30, 20, 600, 450);
    RasterMask mask(Rect64{0, 0, kWidth, kHeight}, 0, 1.0);
    rasteriseInto(mask, polygon);
    PipelineSpec spec = otsuSpec(makeOtsuThreshold(ThresholdDirection::Above));
    spec.restrictToPolygon(polygon);

    // The restriction must change the threshold, or the test proves nothing.
    std::vector<int64_t> all(256, 0);
    std::vector<int64_t> inside(256, 0);
    for (int64_t y = 0; y < kHeight; ++y) {
        for (int64_t x = 0; x < kWidth; ++x) {
            const auto v = static_cast<size_t>(trimodal(0, x, y));
            ++all[v];
            if (mask.test(Point64{x, y})) {
                ++inside[v];
            }
        }
    }
    ASSERT_NE(referenceOtsu(all), referenceOtsu(inside));

    expectStreamedEqualsInMemory(
        scene, spec, [](int64_t x, int64_t y) { return trimodal(0, x, y); },
        [&mask](int64_t x, int64_t y) { return mask.test(Point64{x, y}); });
}

TEST(ThresholdOtsu, FloatNeedsARange) {
    const auto pixel = [](int, int64_t x, int64_t y) {
        if (x == 3 && y % 17 == 0) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        return trimodal(0, x, y) / 255.0;
    };
    auto scene = ts::makeSyntheticScene({{kWidth, kHeight, 0, 0}}, {slideio::DataType::DT_Float32}, pixel);

    try {
        ts::runEager(scene, otsuSpec(makeOtsuThreshold(ThresholdDirection::Above)), 128, 2);
        FAIL() << "a Float32 run without a range succeeded";
    } catch (const TileError& e) {
        const std::string cause = e.cause();
        EXPECT_NE(cause.find(kName), std::string::npos) << cause;
        EXPECT_NE(cause.find("histogramMinimum"), std::string::npos) << cause;
        EXPECT_NE(cause.find("Float32"), std::string::npos) << cause;
    }

    const OperationPtr ranged = OperationCatalog::instance().create(
        kName,
        OperationParameters().setDouble("histogramMinimum", 0.0).setDouble("histogramMaximum", 1.0).setInt("binCount", 256));
    std::vector<int64_t> hist(256, 0);
    for (int64_t y = 0; y < kHeight; ++y) {
        for (int64_t x = 0; x < kWidth; ++x) {
            const double v = static_cast<double>(static_cast<float>(pixel(0, x, y)));
            if (!std::isnan(v)) {
                ++hist[static_cast<size_t>(binOf(v, 0.0, 1.0, 256))];
            }
        }
    }
    const int64_t k = referenceOtsu(hist);
    ASSERT_GE(k, 0);
    for (int32_t workers : {1, 4}) {
        const ts::Image out = ts::runEager(scene, otsuSpec(ranged), 100, workers);
        int64_t mismatches = 0;
        for (int64_t y = 0; y < kHeight; ++y) {
            for (int64_t x = 0; x < kWidth; ++x) {
                const double v = static_cast<double>(static_cast<float>(pixel(0, x, y)));
                const double expected = !std::isnan(v) && binOf(v, 0.0, 1.0, 256) > k ? 255.0 : 0.0;
                if (out.at(0, x, y) != expected) {
                    ++mismatches;
                }
            }
        }
        EXPECT_EQ(mismatches, 0) << "workers " << workers;
    }
}

TEST(ThresholdOtsu, OutOfRangeSamplesAreEndBins) {
    // Equal populations at 0 (below the range), 60, 140 and 250 (above it); over
    // [50, 150] in 10 bins, 60 is bin 1 and 140 bin 9, so k* is 1.
    const double values[] = {0.0, 60.0, 140.0, 250.0};
    const auto pixel = [&values](int, int64_t x, int64_t) { return values[x % 4]; };
    auto scene = ts::makeSyntheticScene({{120, 90, 0, 0}}, {slideio::DataType::DT_Byte}, pixel);
    const auto run = [&](const char* direction) {
        return ts::runEager(scene,
                            otsuSpec(OperationCatalog::instance().create(kName, OperationParameters()
                                                                                    .setString("direction", direction)
                                                                                    .setDouble("histogramMinimum", 50.0)
                                                                                    .setDouble("histogramMaximum", 150.0)
                                                                                    .setInt("binCount", 10))),
                            64, 4);
    };
    const ts::Image above = run("Above");
    const ts::Image below = run("Below");
    for (int64_t x = 0; x < 4; ++x) {
        EXPECT_EQ(above.at(0, x, 7), x >= 2 ? 255.0 : 0.0) << x;
        EXPECT_EQ(below.at(0, x, 7), x <= 1 ? 255.0 : 0.0) << x;
    }
}

TEST(ThresholdOtsu, WideIntegerTypesBinPerValue) {
    // UInt16 and Int16 without a range: one bin per value, so 1000 and 1001 separate.
    for (slideio::DataType type : {slideio::DataType::DT_UInt16, slideio::DataType::DT_Int16}) {
        const auto pixel = [](int, int64_t x, int64_t) { return x % 2 == 0 ? 1000.0 : 1001.0; };
        auto scene = ts::makeSyntheticScene({{90, 70, 0, 0}}, {type}, pixel);
        const ts::Image out = ts::runEager(scene, otsuSpec(makeOtsuThreshold(ThresholdDirection::Above)), 64, 4);
        EXPECT_EQ(out.at(0, 0, 5), 0.0);
        EXPECT_EQ(out.at(0, 1, 5), 255.0);
    }
}

TEST(ThresholdOtsu, ConstantImage) {
    auto scene = ts::makeSyntheticScene({{150, 110, 0, 0}}, {slideio::DataType::DT_Byte},
                                        [](int, int64_t, int64_t) { return 100.0; });
    const ts::Image above = ts::runEager(scene, otsuSpec(makeOtsuThreshold(ThresholdDirection::Above)), 64, 4);
    const ts::Image below = ts::runEager(scene, otsuSpec(makeOtsuThreshold(ThresholdDirection::Below)), 64, 4);
    EXPECT_TRUE(std::all_of(above.planes[0].begin(), above.planes[0].end(), [](uint8_t v) { return v == 0; }));
    EXPECT_TRUE(std::all_of(below.planes[0].begin(), below.planes[0].end(), [](uint8_t v) { return v == 255; }));
}
