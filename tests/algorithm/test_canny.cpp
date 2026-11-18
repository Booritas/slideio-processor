// Requirements: UR-PIX-3, UR-QA-2, UR-ALG-6, UR-ALG-8
#include <slideio/processor/algorithm/pixel/filter.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/pipeline/operation_catalog.hpp>
#include <slideio/processor/pipeline/pipeline_spec.hpp>

#include <algorithm/support/harness.hpp>
#include <pipeline/detail/parameters_json.hpp>
#include <pipeline/test_support/synthetic_scene.hpp>

#include <gtest/gtest.h>

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace slideio::processor;
namespace ts = slideio::processor::testing_support;

namespace {

constexpr const char* kName = "pixel.canny";
constexpr int32_t kReach = 32; // the hysteresis reach the halo allows beyond the gradient's own

// The message of the InvalidArgumentError creating the operation by name throws; "" when none.
std::string rejection(const OperationParameters& p) {
    try {
        OperationCatalog::instance().create(kName, p);
    } catch (const InvalidArgumentError& e) {
        return e.what();
    }
    return "";
}

// The message of the InvalidArgumentError the factory throws; "" when none.
std::string factoryRejection(double low, double high, int32_t aperture) {
    try {
        makeCanny(low, high, aperture);
    } catch (const InvalidArgumentError& e) {
        return e.what();
    }
    return "";
}

// A Byte scene of one level whose pixels are those of @p image (CV_8U).
std::shared_ptr<slideio::Scene> sceneOf(const cv::Mat& image) {
    const cv::Mat copy = image.clone();
    return ts::makeSyntheticScene({{copy.cols, copy.rows, 0, 0}}, {slideio::DataType::DT_Byte},
                                  [copy](int, int64_t x, int64_t y) {
                                      return static_cast<double>(copy.at<uint8_t>(static_cast<int>(y),
                                                                                  static_cast<int>(x)));
                                  });
}

// The reference: cv::Canny on @p image padded with zeros by @p halo on every
// side, as the framework reads pixels beyond the level, cropped to the image.
cv::Mat referenceCanny(const cv::Mat& image, int32_t halo, double low, double high, int32_t aperture) {
    cv::Mat padded;
    cv::copyMakeBorder(image, padded, halo, halo, halo, halo, cv::BORDER_CONSTANT, cv::Scalar(0));
    cv::Mat edges;
    cv::Canny(padded, edges, low, high, aperture, false);
    return edges(cv::Rect(halo, halo, image.cols, image.rows)).clone();
}

// The result plane of a Byte run as a CV_8U image.
cv::Mat matOf(const ts::Image& out) {
    EXPECT_EQ(out.types, std::vector<DataType>{DataType::Byte});
    cv::Mat m(static_cast<int>(out.height), static_cast<int>(out.width), CV_8U);
    std::memcpy(m.data, out.planes[0].data(), out.planes[0].size());
    return m;
}

int64_t differingPixels(const cv::Mat& a, const cv::Mat& b) {
    cv::Mat diff;
    cv::compare(a, b, diff, cv::CMP_NE);
    return cv::countNonZero(diff);
}

} // namespace

TEST(Canny, FactoryEqualsNamePath) {
    OperationParameters p;
    p.setDouble("lowThreshold", 20.0).setDouble("highThreshold", 60.0).setInt("apertureSize", 5);
    const OperationPtr byName = OperationCatalog::instance().create(kName, p);
    const OperationPtr byFactory = makeCanny(20.0, 60.0, 5);
    EXPECT_TRUE(detail::parametersEqual(byFactory->parameters(), byName->parameters()));
    EXPECT_EQ(byFactory->parameters().count(), 3);

    // The defaults fill the bag: 50, 150 and 3.
    const OperationPtr byDefault = OperationCatalog::instance().create(kName, OperationParameters());
    EXPECT_EQ(byDefault->parameters().count(), 3);
    EXPECT_TRUE(detail::parametersEqual(byDefault->parameters(), makeCanny(50.0, 150.0, 3)->parameters()));

    // Whole numbers are accepted for the thresholds.
    const OperationPtr byInts = OperationCatalog::instance().create(
        kName, OperationParameters().setInt("lowThreshold", 20).setInt("highThreshold", 60).setInt("apertureSize", 5));
    EXPECT_TRUE(detail::parametersEqual(byInts->parameters(), byFactory->parameters()));

    EXPECT_STREQ(byFactory->name(), kName);
    EXPECT_EQ(byFactory->arity(), ChannelArity::PerChannel);
    EXPECT_FALSE(byFactory->requiresUniformTypes());
    EXPECT_FALSE(byFactory->producesObjects());
    for (int32_t i = 0; i < 11; ++i) {
        const auto type = static_cast<DataType>(i);
        EXPECT_EQ(byFactory->supportedTypes().contains(type), type == DataType::Byte) << dataTypeName(type);
    }
    EXPECT_EQ(byFactory->outputType(DataType::Byte), DataType::Byte);
    EXPECT_STREQ(OperationCatalog::instance().correctnessReference(kName),
                 "Canny J. A computational approach to edge detection. IEEE TPAMI 1986;8:679-698; equals OpenCV "
                 "Canny on one tile");
}

TEST(Canny, RejectsBadParameters) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    struct Case {
        double low;
        double high;
        int32_t aperture;
        const char* key;
    };
    const Case cases[] = {
        {80.0, 70.0, 3, "'lowThreshold'"},  // low above high
        {-5.0, 70.0, 3, "'lowThreshold'"},  // negative
        {-9.0, -5.0, 3, "'lowThreshold'"},  // both negative
        {nan, 70.0, 3, "'lowThreshold'"},   // NaN
        {10.0, nan, 3, "'highThreshold'"},  // NaN
        {10.0, inf, 3, "'highThreshold'"},  // not finite
        {10.0, 70.0, 4, "'apertureSize'"},  // even
        {10.0, 70.0, 9, "'apertureSize'"},  // beyond OpenCV's 7
        {10.0, 70.0, 1, "'apertureSize'"},  // below 3
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.key);
        const std::string message = factoryRejection(c.low, c.high, c.aperture);
        EXPECT_NE(message.find(c.key), std::string::npos) << message;
        EXPECT_NE(message.find(kName), std::string::npos) << message;
        // A parameter bag cannot hold a value that is not finite, so those
        // cases reach the factory only.
        std::string byName;
        if (std::isfinite(c.low) && std::isfinite(c.high)) {
            byName = rejection(OperationParameters()
                                   .setDouble("lowThreshold", c.low)
                                   .setDouble("highThreshold", c.high)
                                   .setInt("apertureSize", c.aperture));
            EXPECT_NE(byName.find(c.key), std::string::npos) << byName;
        }
        // No value is named.
        for (const std::string& m : {message, byName}) {
            for (const char* value : {"80", "70", "-5", "-9", "nan", "inf", "9"}) {
                EXPECT_EQ(m.find(value), std::string::npos) << m;
            }
        }
    }
    EXPECT_TRUE(factoryRejection(0.0, 0.0, 3).empty());
    EXPECT_TRUE(factoryRejection(40.0, 40.0, 7).empty());

    // By name: unknown key and wrong type.
    const std::string unknown = rejection(OperationParameters().setDouble("lowthreshold", 3.0));
    EXPECT_NE(unknown.find("lowthreshold"), std::string::npos) << unknown;
    EXPECT_FALSE(rejection(OperationParameters().setString("highThreshold", "100")).empty());
    EXPECT_FALSE(rejection(OperationParameters().setDouble("apertureSize", 3.5)).empty());
}

TEST(Canny, HaloIsTheDeclaredBound) {
    // apertureSize / 2 + 1 for the gradient and the suppression, plus the reach.
    const std::pair<int32_t, int64_t> expected[] = {{3, 34}, {5, 35}, {7, 36}};
    for (const auto& e : expected) {
        const Halo halo = makeCanny(50.0, 150.0, e.first)->halo();
        EXPECT_EQ(halo.left, e.second) << e.first;
        EXPECT_EQ(halo.top, e.second) << e.first;
        EXPECT_EQ(halo.right, e.second) << e.first;
        EXPECT_EQ(halo.bottom, e.second) << e.first;
    }
}

TEST(Canny, MatchesOpenCVOnOneTile) {
    // Filled circles and rectangles of several grey levels over a gentle ramp,
    // so that strong, weak and sub-threshold gradients all occur.
    cv::Mat image(160, 200, CV_8U);
    for (int y = 0; y < image.rows; ++y) {
        for (int x = 0; x < image.cols; ++x) {
            image.at<uint8_t>(y, x) = static_cast<uint8_t>((x + y) / 8 + ((x * 7 + y * 3) % 5));
        }
    }
    cv::rectangle(image, cv::Rect(10, 12, 50, 40), cv::Scalar(200), cv::FILLED);
    cv::rectangle(image, cv::Rect(120, 5, 70, 30), cv::Scalar(70), cv::FILLED);
    cv::rectangle(image, cv::Rect(0, 120, 60, 40), cv::Scalar(150), cv::FILLED); // touches the level's edge
    cv::circle(image, cv::Point(100, 90), 30, cv::Scalar(240), cv::FILLED);
    cv::circle(image, cv::Point(160, 120), 22, cv::Scalar(45), cv::FILLED);
    cv::circle(image, cv::Point(40, 80), 12, cv::Scalar(110), cv::FILLED);
    cv::circle(image, cv::Point(195, 60), 15, cv::Scalar(180), cv::FILLED); // cut by the level's edge
    const auto scene = sceneOf(image);

    struct Case {
        double low;
        double high;
        int32_t aperture;
    };
    for (const Case& c : {Case{50.0, 150.0, 3}, Case{20.5, 90.0, 3}, Case{200.0, 600.0, 5},
                          Case{1000.0, 4000.0, 7}}) {
        SCOPED_TRACE("aperture " + std::to_string(c.aperture));
        PipelineSpec spec;
        const OperationPtr op = makeCanny(c.low, c.high, c.aperture);
        spec.then(op);
        const cv::Mat result = matOf(ts::runEager(scene, spec, 4096, 1));
        const cv::Mat reference = referenceCanny(image, static_cast<int32_t>(op->halo().left), c.low, c.high,
                                                 c.aperture);
        ASSERT_EQ(result.size(), reference.size());
        EXPECT_EQ(differingPixels(result, reference), 0);
        EXPECT_GT(cv::countNonZero(reference), 100); // the image does have edges
    }
}

TEST(Canny, ThresholdsBeyondEveryMagnitudeGiveNoEdges) {
    // A threshold above every reachable gradient magnitude, however large,
    // marks nothing: no pixel can exceed it, so none is strong, and with no
    // strong pixel hysteresis keeps no weak one either.
    cv::Mat image = cv::Mat::zeros(64, 72, CV_8U);
    cv::rectangle(image, cv::Rect(10, 12, 30, 25), cv::Scalar(255), cv::FILLED);
    cv::circle(image, cv::Point(52, 40), 12, cv::Scalar(120), cv::FILLED);
    const auto scene = sceneOf(image);
    struct Case {
        double low;
        double high;
    };
    for (int32_t aperture : {3, 5, 7}) {
        for (const Case& c : {Case{1e12, 1e12}, Case{0.0, 1e12}, Case{50.0, 1e12}, Case{3e9, 3e9}}) {
            SCOPED_TRACE("aperture " + std::to_string(aperture) + " low " + std::to_string(c.low));
            PipelineSpec spec;
            spec.then(makeCanny(c.low, c.high, aperture));
            EXPECT_EQ(cv::countNonZero(matOf(ts::runEager(scene, spec, 4096, 1))), 0);
        }
        // The same image does have edges at ordinary thresholds.
        PipelineSpec spec;
        spec.then(makeCanny(50.0, 150.0, aperture));
        EXPECT_GT(cv::countNonZero(matOf(ts::runEager(scene, spec, 4096, 1))), 0);
    }
    // The parameters keep the values given, not the ones OpenCV is called with.
    double stored = 0.0;
    ASSERT_TRUE(makeCanny(0.0, 1e12, 3)->parameters().getDouble("highThreshold", stored));
    EXPECT_EQ(stored, 1e12);
}

TEST(Canny, ShortEdgesTileExactly) {
    // Filled discs of radius 4 to 10 at grey level 200 on 0, centres 40 apart.
    // Every non-zero 3x3 Sobel component of a 0/200 image is a multiple of 200,
    // so every non-zero L1 magnitude is at least 200, above the high threshold
    // of 150: every edge is strong, hysteresis has no chain to follow, and the
    // tiled result must equal the single-tile one exactly.
    cv::Mat image = cv::Mat::zeros(260, 300, CV_8U);
    int radius = 4;
    for (int cy = 20; cy < 260; cy += 40) {
        for (int cx = 20; cx < 300; cx += 40) {
            cv::circle(image, cv::Point(cx, cy), radius, cv::Scalar(200), cv::FILLED);
            radius = radius == 10 ? 4 : radius + 1;
        }
    }
    const auto scene = sceneOf(image);
    PipelineSpec spec;
    spec.then(makeCanny(50.0, 150.0, 3));
    const ts::Image reference = ts::runEager(scene, spec, 4096, 1);
    ASSERT_EQ(reference.width, 300);
    ASSERT_EQ(reference.height, 260);
    EXPECT_GT(cv::countNonZero(matOf(reference)), 500);
    for (int32_t tile : {64, 100}) {
        const ts::Image tiled = ts::runEager(scene, spec, tile, 4);
        EXPECT_TRUE(tiled.planes == reference.planes) << "tile " << tile;
    }
}

TEST(Canny, LongWeakChainIsTheDeclaredApproximation) {
    // A 256 x 80 image, zero except the band of rows [40, 60): grey level 100
    // for x in [10, 20) (the strong seed) and 20 for x in [20, 140) (the weak
    // chain). With apertureSize 3 the L1 Sobel magnitude across a step of
    // height h is 4h: 400 on the seed's edges and 320 where 100 meets 20, both
    // above the high threshold of 150, and 80 along the weak band's edges,
    // between the low threshold of 50 and 150. No pixel right of x = 21 is
    // strong (the weak band's corners reach at most 6 * 20 = 120). These bands
    // are verified below rather than assumed.
    constexpr double kLow = 50.0;
    constexpr double kHigh = 150.0;
    cv::Mat image = cv::Mat::zeros(80, 256, CV_8U);
    image(cv::Range(40, 60), cv::Range(10, 20)).setTo(100);
    image(cv::Range(40, 60), cv::Range(20, 140)).setTo(20);

    // The L1 magnitude as cv::Canny computes it, on the zero-padded image.
    cv::Mat padded;
    cv::copyMakeBorder(image, padded, 1, 1, 1, 1, cv::BORDER_CONSTANT, cv::Scalar(0));
    cv::Mat dx;
    cv::Mat dy;
    cv::Sobel(padded, dx, CV_16S, 1, 0, 3);
    cv::Sobel(padded, dy, CV_16S, 0, 1, 3);
    cv::Mat sum;
    cv::add(cv::abs(dx), cv::abs(dy), sum, cv::noArray(), CV_32S);
    const cv::Mat magnitude = sum(cv::Rect(1, 1, image.cols, image.rows));
    std::vector<cv::Point> strong;
    for (int y = 0; y < image.rows; ++y) {
        for (int x = 0; x < image.cols; ++x) {
            const int m = magnitude.at<int32_t>(y, x);
            if (m > kHigh) {
                ASSERT_LE(x, 21) << "a strong pixel away from the seed at " << x << "," << y;
                strong.push_back({x, y});
            }
        }
    }
    ASSERT_FALSE(strong.empty());
    for (int x = 22; x < 138; ++x) { // the weak band's top edge is weak, not strong
        const int m = std::max(magnitude.at<int32_t>(39, x), magnitude.at<int32_t>(40, x));
        ASSERT_GT(m, kLow) << x;
        ASSERT_LE(m, kHigh) << x;
    }

    const auto scene = sceneOf(image);
    PipelineSpec spec;
    spec.then(makeCanny(kLow, kHigh, 3));
    const cv::Mat single = matOf(ts::runEager(scene, spec, 4096, 1));
    const cv::Mat tiled = matOf(ts::runEager(scene, spec, 64, 4));

    // One tile keeps the whole chain: its top edge is marked from the seed to
    // the band's right end.
    for (int x = 10; x < 139; ++x) {
        EXPECT_TRUE(single.at<uint8_t>(39, x) == 255 || single.at<uint8_t>(40, x) == 255) << x;
    }

    // Tile 64 drops the chain's far part, and nothing within the reach of the
    // seed. In this geometry tiling only drops pixels; in general it may also
    // keep a chain the single tile drops, from a strong pixel the replicated
    // border makes at a tile's halo edge.
    ASSERT_GT(differingPixels(single, tiled), 0);
    int differing = 0;
    for (int y = 0; y < image.rows; ++y) {
        for (int x = 0; x < image.cols; ++x) {
            if (single.at<uint8_t>(y, x) == tiled.at<uint8_t>(y, x)) {
                continue;
            }
            ++differing;
            // Here only dropped, never added.
            EXPECT_EQ(tiled.at<uint8_t>(y, x), 0) << x << "," << y;
            int nearest = std::numeric_limits<int>::max();
            for (const cv::Point& s : strong) {
                nearest = std::min(nearest, std::max(std::abs(s.x - x), std::abs(s.y - y)));
            }
            EXPECT_GT(nearest, kReach) << x << "," << y;
        }
    }
    EXPECT_GT(differing, 0);
}

TEST(Canny, ComplexityNoteStatesTheBound) {
    EXPECT_STREQ(OperationCatalog::instance().complexityNote(kName),
                 "O(n) in the number of pixels; equals single-tile Canny wherever every weak edge chain reaches a "
                 "strong edge within 32 pixels, and may drop or keep weak edges beyond that reach");
}
