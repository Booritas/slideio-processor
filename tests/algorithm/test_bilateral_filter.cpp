// Requirements: UR-PIX-3, UR-QA-2, UR-ALG-6
#include <slideio/processor/algorithm/pixel/filter.hpp>
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
#include <tuple>
#include <vector>

using namespace slideio::processor;
namespace ts = slideio::processor::testing_support;

namespace {

constexpr const char* kName = "pixel.bilateral_filter";

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
std::string factoryRejection(int32_t diameter, double sigmaColour, double sigmaSpace) {
    try {
        makeBilateralFilter(diameter, sigmaColour, sigmaSpace);
    } catch (const InvalidArgumentError& e) {
        return e.what();
    }
    return "";
}

// A direct per-pixel computation of the definition with zero padding: over the
// disc dx^2 + dy^2 <= r^2, row by row (dy ascending), left to right (dx
// ascending), in double, skipping NaN neighbours; a NaN centre gives NaN. The
// expressions are shaped exactly as the operation's own, so that, with no
// multiply-add contraction on either side, the sums agree bit for bit.
double directOutput(const std::function<double(int64_t, int64_t)>& v, int64_t width, int64_t height, int64_t x,
                    int64_t y, int32_t diameter, double sigmaColour, double sigmaSpace) {
    const auto at = [&](int64_t px, int64_t py) {
        return px < 0 || py < 0 || px >= width || py >= height ? 0.0 : v(px, py);
    };
    const double c = at(x, y);
    if (std::isnan(c)) {
        return c;
    }
    const int64_t r = diameter / 2;
    const double spaceDenominator = 2.0 * sigmaSpace * sigmaSpace;
    const double colourDenominator = 2.0 * sigmaColour * sigmaColour;
    double sum = 0.0;
    double norm = 0.0;
    for (int64_t dy = -r; dy <= r; ++dy) {
        for (int64_t dx = -r; dx <= r; ++dx) {
            if (dx * dx + dy * dy > r * r) {
                continue;
            }
            const double s = at(x + dx, y + dy);
            if (std::isnan(s)) {
                continue;
            }
            const double spatial = std::exp(-static_cast<double>(dx * dx + dy * dy) / spaceDenominator);
            const double d = s - c;
            const double weight = spatial * std::exp(-(d * d) / colourDenominator);
            sum += weight * s;
            norm += weight;
        }
    }
    return sum / norm;
}

// A deterministic value in [0, 1) for (x, y).
double hash01(int64_t x, int64_t y) {
    uint64_t h = static_cast<uint64_t>(x) * 0x9E3779B97F4A7C15ull ^ static_cast<uint64_t>(y) * 0xC2B2AE3D27D4EB4Full;
    h ^= h >> 31;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 29;
    return static_cast<double>(h >> 11) / 9007199254740992.0;
}

} // namespace

TEST(BilateralFilter, FactoryEqualsNamePath) {
    OperationParameters p;
    p.setInt("diameter", 7).setDouble("sigmaColour", 12.5).setDouble("sigmaSpace", 3.0);
    const OperationPtr byName = OperationCatalog::instance().create(kName, p);
    const OperationPtr byFactory = makeBilateralFilter(7, 12.5, 3.0);
    EXPECT_TRUE(detail::parametersEqual(byFactory->parameters(), byName->parameters()));
    EXPECT_EQ(byFactory->parameters().count(), 3);

    // Defaults fill the bag: diameter 5, sigmaColour 25.0, sigmaSpace 5.0.
    const OperationPtr byDefault = OperationCatalog::instance().create(kName, OperationParameters());
    EXPECT_EQ(byDefault->parameters().count(), 3);
    EXPECT_TRUE(detail::parametersEqual(byDefault->parameters(), makeBilateralFilter(5, 25.0, 5.0)->parameters()));
    // Whole numbers are accepted for the sigmas.
    EXPECT_TRUE(detail::parametersEqual(
        OperationCatalog::instance()
            .create(kName, OperationParameters().setInt("diameter", 7).setInt("sigmaColour", 25).setInt("sigmaSpace", 3))
            ->parameters(),
        makeBilateralFilter(7, 25.0, 3.0)->parameters()));

    EXPECT_STREQ(byFactory->name(), kName);
    EXPECT_EQ(byFactory->arity(), ChannelArity::PerChannel);
    EXPECT_FALSE(byFactory->requiresUniformTypes());
    EXPECT_FALSE(byFactory->producesObjects());
    for (const OperationPtr& op : {byFactory, byName}) {
        const Halo halo = op->halo();
        EXPECT_EQ(halo.left, 3);
        EXPECT_EQ(halo.top, 3);
        EXPECT_EQ(halo.right, 3);
        EXPECT_EQ(halo.bottom, 3);
    }
    EXPECT_EQ(byDefault->halo().left, 2);
    for (int32_t i = 0; i < 11; ++i) {
        const auto type = static_cast<DataType>(i);
        EXPECT_EQ(byFactory->supportedTypes().contains(type), type != DataType::Int64 && type != DataType::UInt64)
            << dataTypeName(type);
    }
    EXPECT_EQ(byFactory->outputType(DataType::Byte), DataType::Float32);
    EXPECT_EQ(byFactory->outputType(DataType::Float32), DataType::Float32);
    EXPECT_EQ(byFactory->outputType(DataType::Float64), DataType::Float64);
    EXPECT_STREQ(OperationCatalog::instance().correctnessReference(kName),
                 "Tomasi C, Manduchi R. Bilateral filtering for gray and color images. ICCV 1998; checked against a "
                 "direct per-pixel computation");
    EXPECT_STREQ(OperationCatalog::instance().complexityNote(kName), "O(n * diameter^2) in the number of samples");
}

TEST(BilateralFilter, RejectsBadParameters) {
    for (int32_t diameter : {1, 2, 4, 257, -3, 0}) {
        const std::string message = factoryRejection(diameter, 25.0, 5.0);
        EXPECT_NE(message.find("'diameter'"), std::string::npos) << diameter << ": " << message;
        EXPECT_NE(message.find(kName), std::string::npos) << message;
        const std::string byName = rejection(OperationParameters().setInt("diameter", diameter));
        EXPECT_NE(byName.find("'diameter'"), std::string::npos) << diameter << ": " << byName;
    }
    EXPECT_EQ(factoryRejection(257, 25.0, 5.0).find("257"), std::string::npos);
    EXPECT_EQ(factoryRejection(-3, 25.0, 5.0).find("-3"), std::string::npos);
    EXPECT_TRUE(factoryRejection(3, 25.0, 5.0).empty());
    EXPECT_TRUE(factoryRejection(255, 25.0, 5.0).empty());
    // By name, a diameter beyond int32 is refused before any conversion.
    EXPECT_NE(rejection(OperationParameters().setInt("diameter", int64_t{1} << 33)).find("'diameter'"),
              std::string::npos);

    for (double sigma : {0.0, -1.5, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
        const std::string colour = factoryRejection(5, sigma, 5.0);
        EXPECT_NE(colour.find("'sigmaColour'"), std::string::npos) << colour;
        EXPECT_NE(colour.find(kName), std::string::npos) << colour;
        const std::string space = factoryRejection(5, 25.0, sigma);
        EXPECT_NE(space.find("'sigmaSpace'"), std::string::npos) << space;
        if (std::isfinite(sigma)) {
            EXPECT_NE(rejection(OperationParameters().setDouble("sigmaColour", sigma)).find("'sigmaColour'"),
                      std::string::npos);
            EXPECT_NE(rejection(OperationParameters().setDouble("sigmaSpace", sigma)).find("'sigmaSpace'"),
                      std::string::npos);
        }
    }
    EXPECT_EQ(factoryRejection(5, -1.5, 5.0).find("1.5"), std::string::npos);

    // By name: unknown key and wrong type.
    const std::string unknown = rejection(OperationParameters().setInt("Diameter", 5));
    EXPECT_NE(unknown.find("Diameter"), std::string::npos) << unknown;
    EXPECT_FALSE(rejection(OperationParameters().setDouble("diameter", 5.0)).empty());
    EXPECT_FALSE(rejection(OperationParameters().setString("sigmaSpace", "5")).empty());
}

TEST(BilateralFilter, MatchesDefinition) {
    constexpr int64_t kWidth = 41;
    constexpr int64_t kHeight = 37;
    std::mt19937 rng(20261118u);
    std::uniform_real_distribution<double> dist(-50.0, 150.0);
    // Float32 samples, so the values as read are the ones the direct loop uses.
    std::vector<double> samples(static_cast<size_t>(kWidth * kHeight));
    for (double& s : samples) {
        s = static_cast<double>(static_cast<float>(dist(rng)));
    }
    const auto value = [&](int64_t x, int64_t y) { return samples[static_cast<size_t>(y * kWidth + x)]; };
    for (const auto& [diameter, sigmaColour, sigmaSpace] :
         {std::tuple<int32_t, double, double>{5, 25.0, 5.0}, {7, 40.0, 2.0}, {9, 10.0, 3.5}, {3, 100.0, 1.0}}) {
        SCOPED_TRACE(std::to_string(diameter));
        const OperationPtr op = makeBilateralFilter(diameter, sigmaColour, sigmaSpace);
        const ts::Image out = ts::applyDirect(*op, {DataType::Float32}, {0}, kWidth, kHeight,
                                              [&](int32_t, int64_t x, int64_t y) { return value(x, y); });
        ASSERT_EQ(out.types, std::vector<DataType>{DataType::Float32});
        const ts::Image out64 = ts::applyDirect(*op, {DataType::Float64}, {0}, kWidth, kHeight,
                                                [&](int32_t, int64_t x, int64_t y) { return value(x, y); });
        ASSERT_EQ(out64.types, std::vector<DataType>{DataType::Float64});
        for (int64_t y = 0; y < kHeight; ++y) {
            for (int64_t x = 0; x < kWidth; ++x) {
                const double expected = directOutput(value, kWidth, kHeight, x, y, diameter, sigmaColour, sigmaSpace);
                ASSERT_EQ(out.at(0, x, y), static_cast<double>(static_cast<float>(expected))) << x << "," << y;
                ASSERT_EQ(out64.at(0, x, y), expected) << x << "," << y;
            }
        }
    }
}

TEST(BilateralFilter, PreservesAStepEdge) {
    constexpr int64_t kWidth = 40;
    constexpr int64_t kHeight = 20;
    const OperationPtr op = makeBilateralFilter(9, 10.0, 5.0);
    for (DataType type : {DataType::Byte, DataType::Float32}) {
        const ts::Image out = ts::applyDirect(*op, {type}, {0}, kWidth, kHeight,
                                              [](int32_t, int64_t x, int64_t) { return x < 20 ? 0.0 : 200.0; });
        ASSERT_EQ(out.types, std::vector<DataType>{DataType::Float32});
        // Away from the image's border (which reads as zero beyond the level),
        // the columns either side of the edge keep their values.
        for (int64_t y = 4; y < kHeight - 4; ++y) {
            for (int64_t x = 4; x < kWidth - 4; ++x) {
                EXPECT_NEAR(out.at(0, x, y), x < 20 ? 0.0 : 200.0, 1e-6) << dataTypeName(type) << " " << x << "," << y;
            }
        }
    }
}

TEST(BilateralFilter, NanCentreAndNeighbour) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const OperationPtr op = makeBilateralFilter(5, 25.0, 5.0);
    for (DataType type : {DataType::Float32, DataType::Float64}) {
        // A constant 7 with one NaN at (6, 4): only that pixel is NaN, and its
        // neighbours, which skip it, stay 7, to rounding, away from the border.
        const ts::Image out = ts::applyDirect(*op, {type}, {0}, 13, 9, [&](int32_t, int64_t x, int64_t y) {
            return x == 6 && y == 4 ? nan : 7.0;
        });
        for (int64_t y = 0; y < 9; ++y) {
            for (int64_t x = 0; x < 13; ++x) {
                const double v = out.at(0, x, y);
                if (x == 6 && y == 4) {
                    EXPECT_TRUE(std::isnan(v)) << dataTypeName(type);
                } else {
                    EXPECT_FALSE(std::isnan(v)) << dataTypeName(type) << " " << x << "," << y;
                    if (x >= 2 && x < 11 && y >= 2 && y < 7) {
                        EXPECT_NEAR(v, 7.0, 1e-12) << dataTypeName(type) << " " << x << "," << y;
                    }
                }
            }
        }
    }

    // A neighbour that is NaN contributes nothing: the result equals the
    // definition with that neighbour left out of both sums.
    std::mt19937 rng(7u);
    std::uniform_real_distribution<double> dist(0.0, 100.0);
    std::vector<double> samples(15 * 11);
    for (double& s : samples) {
        s = static_cast<double>(static_cast<float>(dist(rng)));
    }
    samples[5 * 15 + 7] = nan;
    samples[2 * 15 + 3] = nan;
    const auto value = [&](int64_t x, int64_t y) { return samples[static_cast<size_t>(y * 15 + x)]; };
    const ts::Image out = ts::applyDirect(*op, {DataType::Float64}, {0}, 15, 11,
                                          [&](int32_t, int64_t x, int64_t y) { return value(x, y); });
    for (int64_t y = 0; y < 11; ++y) {
        for (int64_t x = 0; x < 15; ++x) {
            const double expected = directOutput(value, 15, 11, x, y, 5, 25.0, 5.0);
            if (std::isnan(value(x, y))) {
                EXPECT_TRUE(std::isnan(out.at(0, x, y))) << x << "," << y;
            } else {
                EXPECT_EQ(out.at(0, x, y), expected) << x << "," << y;
            }
        }
    }
}

TEST(BilateralFilter, TiledEqualsSingleTile) {
    const auto pattern = [](int, int64_t x, int64_t y) {
        return static_cast<double>((x * 7 + y * 13 + (x * y) % 11) % 256);
    };
    const auto fractional = [](int, int64_t x, int64_t y) {
        const double u = hash01(x, y);
        return u < 0.02 ? std::numeric_limits<double>::quiet_NaN() : (u - 0.3) * 250.0;
    };
    const OperationPtr op = makeBilateralFilter(9, 30.0, 4.0);
    for (slideio::DataType type : {slideio::DataType::DT_Byte, slideio::DataType::DT_Float32}) {
        SCOPED_TRACE(static_cast<int>(type));
        auto scene = ts::makeSyntheticScene({{300, 260, 0, 0}}, {type},
                                            type == slideio::DataType::DT_Byte ? pattern : fractional);
        PipelineSpec spec;
        spec.then(op);
        const ts::Image reference = ts::runEager(scene, spec, 4096, 1);
        ASSERT_EQ(reference.width, 300);
        ASSERT_EQ(reference.height, 260);
        ASSERT_EQ(reference.types, std::vector<DataType>{DataType::Float32});
        for (int32_t tile : {64, 100, 257}) {
            const ts::Image tiled = ts::runEager(scene, spec, tile, 4);
            EXPECT_TRUE(tiled.planes == reference.planes) << "tile " << tile;
        }
    }

    // The same content shifted by s columns gives the same samples, bit for
    // bit, wherever the disc does not reach the image's edge: a pixel's result
    // does not depend on its column within the input.
    constexpr int64_t kWidth = 61;
    constexpr int64_t kHeight = 19;
    const int64_t reach = op->halo().left;
    for (DataType type : {DataType::Float32, DataType::Float64}) {
        const ts::Image base = ts::applyDirect(*op, {type}, {0}, kWidth, kHeight,
                                               [&](int32_t p, int64_t x, int64_t y) { return fractional(p, x, y); });
        const size_t size = static_cast<size_t>(dataTypeSizeBytes(type));
        for (int64_t s = 1; s <= 7; ++s) {
            const ts::Image shifted =
                ts::applyDirect(*op, {type}, {0}, kWidth, kHeight,
                                [&](int32_t p, int64_t x, int64_t y) { return fractional(p, x + s, y); });
            for (int64_t y = reach; y < kHeight - reach; ++y) {
                for (int64_t x = reach; x + s + reach < kWidth; ++x) {
                    const size_t a = static_cast<size_t>(y * kWidth + x) * size;
                    const size_t b = static_cast<size_t>(y * kWidth + x + s) * size;
                    ASSERT_EQ(std::memcmp(shifted.planes[0].data() + a, base.planes[0].data() + b, size), 0)
                        << dataTypeName(type) << " shift " << s << " at " << x << "," << y;
                }
            }
        }
    }
}
