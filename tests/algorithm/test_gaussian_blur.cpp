// Requirements: UR-PIX-3, UR-QA-2, UR-IMG-11, UR-ALG-6
#include <slideio/processor/algorithm/pixel/filter.hpp>
#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/pipeline/operation_catalog.hpp>
#include <slideio/processor/pipeline/pipeline.hpp>
#include <slideio/processor/pipeline/pipeline_spec.hpp>

#include <algorithm/support/harness.hpp>
#include <pipeline/detail/parameters_json.hpp>
#include <pipeline/test_support/synthetic_scene.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <random>
#include <string>
#include <vector>

using namespace slideio::processor;
namespace ts = slideio::processor::testing_support;

namespace {

constexpr const char* kName = "pixel.gaussian_blur";

// The message of the InvalidArgumentError creating the operation from @p p throws; "" when none.
std::string rejection(const OperationParameters& p) {
    try {
        OperationCatalog::instance().create(kName, p);
    } catch (const InvalidArgumentError& e) {
        return e.what();
    }
    return "";
}

// The message of the InvalidArgumentError the factory throws; "" when none.
std::string factoryRejection(double sigma, int32_t kernelSize) {
    try {
        makeGaussianBlur(sigma, kernelSize);
    } catch (const InvalidArgumentError& e) {
        return e.what();
    }
    return "";
}

// The 1-D kernel the operation defines: exp(-d^2 / 2 sigma^2) normalised to sum 1.
std::vector<double> kernel(double sigma, int32_t kernelSize) {
    const int32_t size = kernelSize == 0 ? 2 * static_cast<int32_t>(std::ceil(3.0 * sigma)) + 1 : kernelSize;
    const int32_t r = size / 2;
    std::vector<double> w(static_cast<size_t>(size));
    double sum = 0.0;
    for (int32_t d = -r; d <= r; ++d) {
        w[static_cast<size_t>(d + r)] = std::exp(-static_cast<double>(d) * d / (2.0 * sigma * sigma));
        sum += w[static_cast<size_t>(d + r)];
    }
    for (double& v : w) {
        v /= sum;
    }
    return w;
}

// A direct per-pixel computation with zero padding: for each window row top to
// bottom its weighted sum left to right, then those sums weighted top to bottom.
double naiveOutput(const std::function<double(int64_t, int64_t)>& v, int64_t width, int64_t height, int64_t x,
                   int64_t y, const std::vector<double>& w) {
    const int64_t size = static_cast<int64_t>(w.size());
    const int64_t r = size / 2;
    const auto at = [&](int64_t px, int64_t py) {
        return px < 0 || py < 0 || px >= width || py >= height ? 0.0 : v(px, py);
    };
    double sum = 0.0;
    for (int64_t dy = 0; dy < size; ++dy) {
        double rowSum = 0.0;
        for (int64_t dx = 0; dx < size; ++dx) {
            rowSum += w[static_cast<size_t>(dx)] * at(x + dx - r, y + dy - r);
        }
        sum += w[static_cast<size_t>(dy)] * rowSum;
    }
    return sum;
}

// Whether a one-operation pipeline over a 1-channel synthetic scene of @p type validates.
bool pipelineValidates(DataType type) {
    const auto slideioType = static_cast<slideio::DataType>(dataTypeToSlideio(type));
    auto scene = ts::makeSyntheticScene({{16, 16, 0, 0}}, {slideioType}, [](int, int64_t, int64_t) { return 0.0; });
    PipelineSpec spec;
    spec.then(makeGaussianBlur(1.0, 0));
    const Configuration config = ConfigurationBuilder().tileSize(16, 16).tileSizeFromSlide(false).workerCount(1).build();
    try {
        Pipeline(scene, config, spec).validate();
        return true;
    } catch (const UnsupportedDataTypeError&) {
        return false;
    }
}

} // namespace

TEST(GaussianBlur, FactoryEqualsNamePath) {
    OperationParameters p;
    p.setDouble("sigma", 2.0).setInt("kernelSize", 0);
    const OperationPtr byName = OperationCatalog::instance().create(kName, p);
    const OperationPtr byFactory = makeGaussianBlur(2.0, 0);
    EXPECT_TRUE(detail::parametersEqual(byFactory->parameters(), byName->parameters()));
    EXPECT_EQ(byFactory->parameters().count(), 2);

    // Defaults fill the bag: sigma 1.0, kernelSize 0.
    const OperationPtr byDefault = OperationCatalog::instance().create(kName, OperationParameters());
    EXPECT_EQ(byDefault->parameters().count(), 2);
    EXPECT_TRUE(detail::parametersEqual(byDefault->parameters(), makeGaussianBlur(1.0, 0)->parameters()));
    // A whole number is accepted for sigma.
    EXPECT_TRUE(detail::parametersEqual(
        OperationCatalog::instance().create(kName, OperationParameters().setInt("sigma", 2))->parameters(),
        byFactory->parameters()));

    // The declared contract: kernel 2 * ceil(3 * 2) + 1 = 13, halo 6.
    EXPECT_STREQ(byFactory->name(), kName);
    EXPECT_EQ(byFactory->arity(), ChannelArity::PerChannel);
    EXPECT_FALSE(byFactory->requiresUniformTypes());
    EXPECT_FALSE(byFactory->producesObjects());
    for (const OperationPtr& op : {byFactory, byName}) {
        const Halo halo = op->halo();
        EXPECT_EQ(halo.left, 6);
        EXPECT_EQ(halo.top, 6);
        EXPECT_EQ(halo.right, 6);
        EXPECT_EQ(halo.bottom, 6);
    }
    EXPECT_EQ(makeGaussianBlur(2.0, 5)->halo().left, 2);
    EXPECT_EQ(makeGaussianBlur(0.5, 1)->halo().left, 0);
    EXPECT_STREQ(OperationCatalog::instance().correctnessReference(kName),
                 "Analytic: a normalised Gaussian kernel's response to constant, linear and impulse images");
    EXPECT_STREQ(OperationCatalog::instance().complexityNote(kName), "O(n * kernelSize) in the number of samples");
}

TEST(GaussianBlur, RejectsBadParameters) {
    for (double sigma : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
        const std::string message = factoryRejection(sigma, 0);
        EXPECT_NE(message.find("'sigma'"), std::string::npos) << message;
        EXPECT_NE(message.find(kName), std::string::npos) << message;
        if (std::isfinite(sigma)) {
            const std::string byName = rejection(OperationParameters().setDouble("sigma", sigma));
            EXPECT_NE(byName.find("'sigma'"), std::string::npos) << byName;
        }
    }
    EXPECT_EQ(factoryRejection(-1.0, 0).find("-1"), std::string::npos);
    for (int32_t kernelSize : {2, -1, 1027}) {
        const std::string message = factoryRejection(1.0, kernelSize);
        EXPECT_NE(message.find("'kernelSize'"), std::string::npos) << kernelSize << ": " << message;
        EXPECT_NE(message.find(kName), std::string::npos) << message;
        if (kernelSize != 2) { // "2" is a digit of the stated bound
            EXPECT_EQ(message.find(std::to_string(kernelSize)), std::string::npos) << message;
        }
        const std::string byName = rejection(OperationParameters().setInt("kernelSize", kernelSize));
        EXPECT_NE(byName.find("'kernelSize'"), std::string::npos) << byName;
        if (kernelSize != 2) {
            EXPECT_EQ(byName.find(std::to_string(kernelSize)), std::string::npos) << byName;
        }
    }
    EXPECT_TRUE(factoryRejection(1.0, 1).empty());
    EXPECT_TRUE(factoryRejection(1.0, 1025).empty());
    // A sigma whose derived kernel would exceed 1025 is refused, naming sigma.
    EXPECT_NE(factoryRejection(1000.0, 0).find("'sigma'"), std::string::npos);
    EXPECT_TRUE(factoryRejection(1000.0, 1025).empty());

    // By name: unknown key and wrong type.
    const std::string unknown = rejection(OperationParameters().setInt("kernelsize", 3));
    EXPECT_NE(unknown.find("kernelsize"), std::string::npos) << unknown;
    EXPECT_FALSE(rejection(OperationParameters().setDouble("kernelSize", 3.0)).empty());
    EXPECT_FALSE(rejection(OperationParameters().setString("sigma", "2")).empty());
}

TEST(GaussianBlur, AnalyticResponses) {
    constexpr int64_t kSize = 64;
    // A constant image is unchanged away from the edges.
    const OperationPtr op = makeGaussianBlur(2.0, 0);
    const ts::Image constant =
        ts::applyDirect(*op, {DataType::Float32}, {0}, kSize, kSize, [](int32_t, int64_t, int64_t) { return 100.0; });
    ASSERT_EQ(constant.types, std::vector<DataType>{DataType::Float32});
    for (int64_t y = 6; y < kSize - 6; ++y) {
        for (int64_t x = 6; x < kSize - 6; ++x) {
            ASSERT_EQ(constant.at(0, x, y), 100.0) << x << "," << y;
        }
    }

    // A symmetric kernel reproduces a linear ramp.
    const ts::Image ramp = ts::applyDirect(*op, {DataType::Float64}, {0}, kSize, kSize,
                                           [](int32_t, int64_t x, int64_t) { return 3.0 * static_cast<double>(x); });
    ASSERT_EQ(ramp.types, std::vector<DataType>{DataType::Float64});
    for (int64_t y = 6; y < kSize - 6; ++y) {
        for (int64_t x = 6; x < kSize - 6; ++x) {
            ASSERT_NEAR(ramp.at(0, x, y), 3.0 * static_cast<double>(x), 1e-9) << x << "," << y;
        }
    }

    // An impulse reproduces the 2-D kernel, the outer product of the 1-D one.
    for (const auto& [sigma, kernelSize] : {std::pair<double, int32_t>{1.5, 0}, {3.0, 7}, {0.8, 1}, {2.0, 0}}) {
        const OperationPtr blur = makeGaussianBlur(sigma, kernelSize);
        const std::vector<double> w = kernel(sigma, kernelSize);
        const int64_t r = static_cast<int64_t>(w.size()) / 2;
        constexpr int64_t kCentre = 20;
        const ts::Image impulse =
            ts::applyDirect(*blur, {DataType::Float64}, {0}, 41, 41, [](int32_t, int64_t x, int64_t y) {
                return x == kCentre && y == kCentre ? 1.0 : 0.0;
            });
        for (int64_t y = 0; y < 41; ++y) {
            for (int64_t x = 0; x < 41; ++x) {
                const int64_t dx = x - kCentre;
                const int64_t dy = y - kCentre;
                const double expected = std::abs(dx) > r || std::abs(dy) > r
                                            ? 0.0
                                            : w[static_cast<size_t>(dx + r)] * w[static_cast<size_t>(dy + r)];
                ASSERT_NEAR(impulse.at(0, x, y), expected, 1e-15) << sigma << " " << x << "," << y;
            }
        }
    }
}

TEST(GaussianBlur, KernelSizeOneIsIdentity) {
    const OperationPtr op = makeGaussianBlur(0.7, 1);
    const ts::Image out = ts::applyDirect(*op, {DataType::Int16}, {0}, 13, 9, [](int32_t, int64_t x, int64_t y) {
        return static_cast<double>(x * 37 - y * 101);
    });
    ASSERT_EQ(out.types, std::vector<DataType>{DataType::Float32});
    for (int64_t y = 0; y < 9; ++y) {
        for (int64_t x = 0; x < 13; ++x) {
            EXPECT_EQ(out.at(0, x, y), static_cast<double>(x * 37 - y * 101)) << x << "," << y;
        }
    }
}

TEST(GaussianBlur, RefusesSigmaBelowTheFloor) {
    // A sigma so small that its square underflows would make every weight
    // 0 / 0; it is refused, naming sigma, and the floor itself is accepted.
    for (int32_t kernelSize : {0, 1, 3}) {
        const std::string message = factoryRejection(1e-300, kernelSize);
        EXPECT_NE(message.find("'sigma'"), std::string::npos) << kernelSize << ": " << message;
        EXPECT_NE(message.find(kName), std::string::npos) << message;
        EXPECT_EQ(message.find("300"), std::string::npos) << message;
    }
    EXPECT_NE(factoryRejection(9.9e-7, 3).find("'sigma'"), std::string::npos);
    const std::string byName = rejection(OperationParameters().setDouble("sigma", 1e-300));
    EXPECT_NE(byName.find("'sigma'"), std::string::npos) << byName;

    // At the floor every off-centre weight is 0, so the result is the input.
    const OperationPtr op = makeGaussianBlur(1e-6, 3);
    const ts::Image out = ts::applyDirect(*op, {DataType::Int16}, {0}, 13, 9, [](int32_t, int64_t x, int64_t y) {
        return static_cast<double>(x * 37 - y * 101);
    });
    for (int64_t y = 0; y < 9; ++y) {
        for (int64_t x = 0; x < 13; ++x) {
            EXPECT_EQ(out.at(0, x, y), static_cast<double>(x * 37 - y * 101)) << x << "," << y;
        }
    }
}

TEST(GaussianBlur, MatchesDirectComputation) {
    constexpr int64_t kWidth = 97;
    constexpr int64_t kHeight = 83;
    std::mt19937 rng(20261118u);
    std::uniform_real_distribution<double> dist(-50.0, 100.0);
    std::vector<double> samples(static_cast<size_t>(kWidth * kHeight));
    for (double& s : samples) {
        s = dist(rng);
    }
    const auto value = [&](int64_t x, int64_t y) { return samples[static_cast<size_t>(y * kWidth + x)]; };
    const OperationPtr op = makeGaussianBlur(1.3, 9);
    const std::vector<double> w = kernel(1.3, 9);
    const ts::Image out = ts::applyDirect(*op, {DataType::Float64}, {0}, kWidth, kHeight,
                                          [&](int32_t, int64_t x, int64_t y) { return value(x, y); });
    for (int64_t y = 0; y < kHeight; ++y) {
        for (int64_t x = 0; x < kWidth; ++x) {
            ASSERT_EQ(out.at(0, x, y), naiveOutput(value, kWidth, kHeight, x, y, w)) << x << "," << y;
        }
    }
}

TEST(GaussianBlur, OutputTypes) {
    const OperationPtr op = makeGaussianBlur(1.0, 3);
    for (int32_t i = 0; i < 11; ++i) {
        const auto type = static_cast<DataType>(i);
        EXPECT_EQ(op->supportedTypes().contains(type), type != DataType::Int64 && type != DataType::UInt64)
            << dataTypeName(type);
    }
    for (DataType type : {DataType::Byte, DataType::Int16, DataType::Float32}) {
        EXPECT_EQ(op->outputType(type), DataType::Float32) << dataTypeName(type);
        const ts::Image out =
            ts::applyDirect(*op, {type}, {0}, 8, 8, [](int32_t, int64_t x, int64_t) { return static_cast<double>(x); });
        EXPECT_EQ(out.types, std::vector<DataType>{DataType::Float32}) << dataTypeName(type);
    }
    EXPECT_EQ(op->outputType(DataType::Float64), DataType::Float64);
    const ts::Image f64 = ts::applyDirect(*op, {DataType::Float64}, {0}, 8, 8,
                                          [](int32_t, int64_t x, int64_t) { return static_cast<double>(x); });
    EXPECT_EQ(f64.types, std::vector<DataType>{DataType::Float64});

    EXPECT_TRUE(pipelineValidates(DataType::UInt16));
    EXPECT_FALSE(pipelineValidates(DataType::Int64));
}

TEST(GaussianBlur, TiledEqualsSingleTile) {
    const auto pattern = [](int, int64_t x, int64_t y) {
        return static_cast<double>((x * 7 + y * 13 + (x * y) % 11) % 256) * 3.25;
    };
    for (slideio::DataType type : {slideio::DataType::DT_Float32, slideio::DataType::DT_UInt16}) {
        SCOPED_TRACE(static_cast<int>(type));
        auto scene = ts::makeSyntheticScene({{300, 260, 0, 0}}, {type}, pattern);
        PipelineSpec spec;
        spec.then(makeGaussianBlur(2.5, 0));
        const ts::Image reference = ts::runEager(scene, spec, 4096, 1);
        ASSERT_EQ(reference.width, 300);
        ASSERT_EQ(reference.height, 260);
        ASSERT_EQ(reference.types, std::vector<DataType>{DataType::Float32});
        for (int32_t tile : {64, 100, 257}) {
            const ts::Image tiled = ts::runEager(scene, spec, tile, 4);
            EXPECT_TRUE(tiled.planes == reference.planes) << "tile " << tile;
        }
    }
}
