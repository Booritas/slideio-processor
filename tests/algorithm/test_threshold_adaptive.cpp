// Requirements: UR-PIX-5, UR-QA-2, UR-ALG-6
#include <slideio/processor/algorithm/pixel/threshold.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/pipeline/operation_catalog.hpp>
#include <slideio/processor/pipeline/pipeline_spec.hpp>

#include <algorithm/support/harness.hpp>
#include <pipeline/detail/parameters_json.hpp>
#include <pipeline/test_support/synthetic_scene.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <random>
#include <string>
#include <vector>

using namespace slideio::processor;
namespace ts = slideio::processor::testing_support;

namespace {

constexpr const char* kName = "pixel.threshold_adaptive";

// The message of the InvalidArgumentError creating the operation from @p p throws; "" when none.
std::string rejection(const OperationParameters& p) {
    try {
        OperationCatalog::instance().create(kName, p);
    } catch (const InvalidArgumentError& e) {
        return e.what();
    }
    return "";
}

// The window weights, as the operation defines them: all 1 for Mean (the sum is
// divided by blockSize^2 at the end), exp(-d^2 / 2 sigma^2) normalised to sum 1
// for Gaussian.
std::vector<double> weights(AdaptiveMethod method, int32_t blockSize) {
    const int32_t r = blockSize / 2;
    std::vector<double> w(static_cast<size_t>(blockSize), 1.0);
    if (method == AdaptiveMethod::Gaussian) {
        const double sigma = 0.3 * ((blockSize - 1) * 0.5 - 1.0) + 0.8;
        double sum = 0.0;
        for (int32_t d = -r; d <= r; ++d) {
            w[static_cast<size_t>(d + r)] = std::exp(-static_cast<double>(d) * d / (2.0 * sigma * sigma));
            sum += w[static_cast<size_t>(d + r)];
        }
        for (double& v : w) {
            v /= sum;
        }
    }
    return w;
}

// A direct per-pixel computation with zero padding: for each window row top to
// bottom the weighted row sum left to right, then those sums top to bottom.
double naiveOutput(const std::function<double(int64_t, int64_t)>& v, int64_t width, int64_t height, int64_t x,
                   int64_t y, AdaptiveMethod method, int32_t blockSize, double offset, ThresholdDirection direction) {
    const std::vector<double> w = weights(method, blockSize);
    const int32_t r = blockSize / 2;
    const auto at = [&](int64_t px, int64_t py) {
        return px < 0 || py < 0 || px >= width || py >= height ? 0.0 : v(px, py);
    };
    double sum = 0.0;
    for (int32_t dy = 0; dy < blockSize; ++dy) {
        double rowSum = 0.0;
        for (int32_t dx = 0; dx < blockSize; ++dx) {
            rowSum += w[static_cast<size_t>(dx)] * at(x + dx - r, y + dy - r);
        }
        sum += w[static_cast<size_t>(dy)] * rowSum;
    }
    const double mean = method == AdaptiveMethod::Mean ? sum / (static_cast<double>(blockSize) * blockSize) : sum;
    const double t = mean - offset;
    const double s = v(x, y);
    const bool foreground = direction == ThresholdDirection::Above ? s > t : s < t;
    return foreground ? 255.0 : 0.0;
}

} // namespace

TEST(ThresholdAdaptive, FactoryEqualsNamePath) {
    OperationParameters p;
    p.setString("method", "Gaussian").setInt("blockSize", 7).setDouble("offset", -1.5).setString("direction", "Below");
    EXPECT_TRUE(detail::parametersEqual(
        makeAdaptiveThreshold(AdaptiveMethod::Gaussian, 7, -1.5, ThresholdDirection::Below)->parameters(),
        OperationCatalog::instance().create(kName, p)->parameters()));

    // Defaults fill the bag: Mean, 11, 2.0, Above.
    const OperationPtr byDefault = OperationCatalog::instance().create(kName, OperationParameters());
    EXPECT_EQ(byDefault->parameters().count(), 4);
    EXPECT_TRUE(detail::parametersEqual(
        byDefault->parameters(),
        makeAdaptiveThreshold(AdaptiveMethod::Mean, 11, 2.0, ThresholdDirection::Above)->parameters()));
    EXPECT_STREQ(byDefault->parameters().getString("method", nullptr), "Mean");

    // The declared contract.
    const OperationPtr op = makeAdaptiveThreshold(AdaptiveMethod::Mean, 15, 0.0, ThresholdDirection::Above);
    EXPECT_STREQ(op->name(), kName);
    EXPECT_EQ(op->arity(), ChannelArity::PerChannel);
    EXPECT_FALSE(op->requiresUniformTypes());
    EXPECT_FALSE(op->producesObjects());
    const Halo halo = op->halo();
    EXPECT_EQ(halo.left, 7);
    EXPECT_EQ(halo.top, 7);
    EXPECT_EQ(halo.right, 7);
    EXPECT_EQ(halo.bottom, 7);
    for (int32_t i = 0; i < 11; ++i) {
        const auto type = static_cast<DataType>(i);
        EXPECT_EQ(op->supportedTypes().contains(type), type != DataType::Int64 && type != DataType::UInt64)
            << dataTypeName(type);
        EXPECT_EQ(op->outputType(type), DataType::Byte);
    }
    EXPECT_STREQ(OperationCatalog::instance().correctnessReference(kName),
                 "Analytic: the threshold at each pixel is its window mean (or Gaussian-weighted mean) minus the "
                 "offset, compared with a direct per-pixel computation");
    EXPECT_STREQ(OperationCatalog::instance().complexityNote(kName), "O(n * blockSize) in the number of samples");
}

TEST(ThresholdAdaptive, RejectsEvenOrSmallBlock) {
    for (int32_t blockSize : {2, 4, 1027, 1, -3, 0}) {
        EXPECT_THROW(makeAdaptiveThreshold(AdaptiveMethod::Mean, blockSize, 0.0, ThresholdDirection::Above),
                     InvalidArgumentError)
            << blockSize;
        const std::string message = rejection(OperationParameters().setInt("blockSize", blockSize));
        EXPECT_NE(message.find("blockSize"), std::string::npos) << blockSize << ": " << message;
        EXPECT_NE(message.find(kName), std::string::npos) << message;
    }
    EXPECT_NO_THROW(makeAdaptiveThreshold(AdaptiveMethod::Mean, 3, 0.0, ThresholdDirection::Above));
    EXPECT_NO_THROW(makeAdaptiveThreshold(AdaptiveMethod::Mean, 1025, 0.0, ThresholdDirection::Above));

    EXPECT_THROW(makeAdaptiveThreshold(AdaptiveMethod::Mean, 5, std::numeric_limits<double>::quiet_NaN(),
                                       ThresholdDirection::Above),
                 InvalidArgumentError);
    EXPECT_THROW(makeAdaptiveThreshold(AdaptiveMethod::Mean, 5, std::numeric_limits<double>::infinity(),
                                       ThresholdDirection::Above),
                 InvalidArgumentError);
    EXPECT_THROW(makeAdaptiveThreshold(static_cast<AdaptiveMethod>(5), 5, 0.0, ThresholdDirection::Above),
                 InvalidArgumentError);
    EXPECT_THROW(makeAdaptiveThreshold(AdaptiveMethod::Mean, 5, 0.0, static_cast<ThresholdDirection>(5)),
                 InvalidArgumentError);

    // By name: unknown key, wrong type, bad spelling; never the value in the message.
    const std::string unknown = rejection(OperationParameters().setInt("blocksize", 5));
    EXPECT_NE(unknown.find("blocksize"), std::string::npos) << unknown;
    const std::string method = rejection(OperationParameters().setString("method", "Median"));
    EXPECT_NE(method.find("method"), std::string::npos) << method;
    EXPECT_EQ(method.find("Median"), std::string::npos) << method;
    EXPECT_FALSE(rejection(OperationParameters().setDouble("blockSize", 5.0)).empty());
    EXPECT_FALSE(rejection(OperationParameters().setInt("offset", 2)).empty());
}

TEST(ThresholdAdaptive, StepEdgeAnalytic) {
    // Every row is the same, so the window mean depends on x alone away from the
    // top and bottom; rows 10 to 29 are tested.
    const OperationPtr above = makeAdaptiveThreshold(AdaptiveMethod::Mean, 5, 0.0, ThresholdDirection::Above);
    const auto step = [](int32_t, int64_t x, int64_t) { return x < 50 ? 0.0 : 200.0; };
    const ts::Image out = ts::applyDirect(*above, {DataType::Byte}, {0}, 100, 40, step);
    ASSERT_EQ(out.types, std::vector<DataType>{DataType::Byte});
    for (int64_t y = 10; y < 30; ++y) {
        EXPECT_EQ(out.at(0, 50, y), 255.0) << y; // 200 > 120
        EXPECT_EQ(out.at(0, 49, y), 0.0) << y;   // 0 < 80
        EXPECT_EQ(out.at(0, 10, y), 0.0) << y;   // 0 > 0 is false
        EXPECT_EQ(out.at(0, 51, y), 255.0) << y; // 200 > 160
        EXPECT_EQ(out.at(0, 52, y), 0.0) << y;   // 200 > 200 is false
    }
    // Below: x = 49 is foreground (0 < 80), x = 50 is not (200 < 120 is false).
    const OperationPtr below = makeAdaptiveThreshold(AdaptiveMethod::Mean, 5, 0.0, ThresholdDirection::Below);
    const ts::Image belowOut = ts::applyDirect(*below, {DataType::Byte}, {0}, 100, 40, step);
    EXPECT_EQ(belowOut.at(0, 49, 20), 255.0);
    EXPECT_EQ(belowOut.at(0, 50, 20), 0.0);
    EXPECT_EQ(belowOut.at(0, 10, 20), 0.0);
}

TEST(ThresholdAdaptive, NanInWindowIsBackground) {
    const OperationPtr above = makeAdaptiveThreshold(AdaptiveMethod::Mean, 3, 0.0, ThresholdDirection::Above);
    const OperationPtr below = makeAdaptiveThreshold(AdaptiveMethod::Mean, 3, 0.0, ThresholdDirection::Below);
    // A ramp, with NaN at (5, 5): its 3 x 3 neighbourhood is background both ways.
    const auto pixel = [](int32_t, int64_t x, int64_t y) {
        return x == 5 && y == 5 ? std::numeric_limits<double>::quiet_NaN() : static_cast<double>((x * 37 + y * 11) % 17);
    };
    const ts::Image a = ts::applyDirect(*above, {DataType::Float32}, {0}, 12, 12, pixel);
    const ts::Image b = ts::applyDirect(*below, {DataType::Float32}, {0}, 12, 12, pixel);
    for (int64_t y = 4; y <= 6; ++y) {
        for (int64_t x = 4; x <= 6; ++x) {
            EXPECT_EQ(a.at(0, x, y), 0.0) << x << "," << y;
            EXPECT_EQ(b.at(0, x, y), 0.0) << x << "," << y;
        }
    }
    // Outside it, every pixel is on one side or the other unless it equals its threshold.
    int32_t decided = 0;
    for (int64_t y = 0; y < 12; ++y) {
        for (int64_t x = 0; x < 12; ++x) {
            if (std::abs(x - 5) > 1 || std::abs(y - 5) > 1) {
                decided += (a.at(0, x, y) == 255.0 || b.at(0, x, y) == 255.0) ? 1 : 0;
            }
        }
    }
    EXPECT_GT(decided, 100);
}

TEST(ThresholdAdaptive, MatchesDirectComputation) {
    constexpr int64_t kWidth = 97;
    constexpr int64_t kHeight = 83;
    std::mt19937 rng(20261118u);
    std::uniform_real_distribution<float> dist(0.0f, 100.0f);
    std::vector<float> samples(static_cast<size_t>(kWidth * kHeight));
    for (float& s : samples) {
        s = dist(rng);
    }
    const auto value = [&](int64_t x, int64_t y) { return static_cast<double>(samples[static_cast<size_t>(y * kWidth + x)]); };
    for (ThresholdDirection direction : {ThresholdDirection::Above, ThresholdDirection::Below}) {
        const OperationPtr op = makeAdaptiveThreshold(AdaptiveMethod::Gaussian, 7, 1.25, direction);
        const ts::Image out = ts::applyDirect(*op, {DataType::Float32}, {0}, kWidth, kHeight,
                                              [&](int32_t, int64_t x, int64_t y) { return value(x, y); });
        int64_t foreground = 0;
        for (int64_t y = 0; y < kHeight; ++y) {
            for (int64_t x = 0; x < kWidth; ++x) {
                const double expected = naiveOutput(value, kWidth, kHeight, x, y, AdaptiveMethod::Gaussian, 7, 1.25,
                                                    direction);
                ASSERT_EQ(out.at(0, x, y), expected) << x << "," << y;
                foreground += expected == 255.0 ? 1 : 0;
            }
        }
        EXPECT_GT(foreground, kWidth * kHeight / 10);
        EXPECT_LT(foreground, kWidth * kHeight * 9 / 10);
    }
}

TEST(ThresholdAdaptive, TiledEqualsSingleTile) {
    const auto pattern = [](int, int64_t x, int64_t y) {
        return static_cast<double>((x * 7 + y * 13 + (x * y) % 11) % 256) * 3.25;
    };
    for (slideio::DataType type : {slideio::DataType::DT_Float32, slideio::DataType::DT_UInt16}) {
        SCOPED_TRACE(static_cast<int>(type));
        auto scene = ts::makeSyntheticScene({{300, 260, 0, 0}}, {type}, pattern);
        for (AdaptiveMethod method : {AdaptiveMethod::Mean, AdaptiveMethod::Gaussian}) {
            PipelineSpec spec;
            spec.then(makeAdaptiveThreshold(method, 15, 1.0, ThresholdDirection::Above));
            const ts::Image reference = ts::runEager(scene, spec, 4096, 1);
            ASSERT_EQ(reference.width, 300);
            ASSERT_EQ(reference.height, 260);
            ASSERT_EQ(reference.types, std::vector<DataType>{DataType::Byte});
            // The single tile agrees with the direct computation, edges included.
            const auto value = [&](int64_t x, int64_t y) {
                const double v = pattern(0, x, y);
                return type == slideio::DataType::DT_Float32 ? static_cast<double>(static_cast<float>(v))
                                                             : std::floor(v);
            };
            for (int64_t y : {0, 3, 6, 7, 100, 252, 253, 259}) {
                for (int64_t x : {0, 2, 6, 7, 150, 292, 293, 299}) {
                    EXPECT_EQ(reference.at(0, x, y),
                              naiveOutput(value, 300, 260, x, y, method, 15, 1.0, ThresholdDirection::Above))
                        << x << "," << y;
                }
            }
            for (int32_t tile : {64, 100, 257}) {
                const ts::Image tiled = ts::runEager(scene, spec, tile, 4);
                EXPECT_TRUE(tiled.planes == reference.planes) << "tile " << tile;
            }
        }
    }
}
