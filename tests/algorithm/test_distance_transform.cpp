// Requirements: UR-PIX-7, UR-QA-2, UR-ALG-6
#include <slideio/processor/algorithm/pixel/filter.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/pipeline/operation_catalog.hpp>
#include <slideio/processor/pipeline/pipeline_spec.hpp>

#include <algorithm/support/harness.hpp>
#include <pipeline/detail/parameters_json.hpp>
#include <pipeline/test_support/synthetic_scene.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <vector>

using namespace slideio::processor;
namespace ts = slideio::processor::testing_support;

namespace {

constexpr const char* kName = "pixel.distance_transform";

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
std::string factoryRejection(int32_t maximum) {
    try {
        makeDistanceTransform(maximum);
    } catch (const InvalidArgumentError& e) {
        return e.what();
    }
    return "";
}

// A binary mask of width x height; true is foreground.
struct Mask {
    int64_t width;
    int64_t height;
    std::vector<uint8_t> v;
    bool at(int64_t x, int64_t y) const { return v[static_cast<size_t>(y * width + x)] != 0; }
};

// The definition, directly: min(H, the distance to the nearest background pixel
// centre), the outside of the image being background. 0 for a background pixel.
float bruteForce(const Mask& m, int64_t x, int64_t y, int32_t maximum) {
    if (!m.at(x, y)) {
        return 0.0f;
    }
    // The nearest outside pixel centre is on an axis.
    double best = static_cast<double>(maximum);
    best = std::min(best, static_cast<double>(std::min({x + 1, y + 1, m.width - x, m.height - y})));
    for (int64_t by = 0; by < m.height; ++by) {
        for (int64_t bx = 0; bx < m.width; ++bx) {
            if (!m.at(bx, by)) {
                const double dx = static_cast<double>(x - bx);
                const double dy = static_cast<double>(y - by);
                best = std::min(best, std::sqrt(dx * dx + dy * dy));
            }
        }
    }
    return static_cast<float>(best);
}

ts::Image applyMask(const Mask& m, int32_t maximum, DataType type = DataType::Byte) {
    const OperationPtr op = makeDistanceTransform(maximum);
    return ts::applyDirect(*op, {type}, {0}, m.width, m.height,
                           [&m](int32_t, int64_t x, int64_t y) { return m.at(x, y) ? 1.0 : 0.0; });
}

void expectMatchesBruteForce(const Mask& m, int32_t maximum) {
    const ts::Image out = applyMask(m, maximum);
    ASSERT_EQ(out.types, std::vector<DataType>{DataType::Float32});
    for (int64_t y = 0; y < m.height; ++y) {
        for (int64_t x = 0; x < m.width; ++x) {
            ASSERT_EQ(static_cast<float>(out.at(0, x, y)), bruteForce(m, x, y, maximum))
                << "maximum " << maximum << " at " << x << "," << y;
        }
    }
}

} // namespace

TEST(DistanceTransform, FactoryEqualsNamePath) {
    OperationParameters p;
    p.setInt("maximumDistancePx", 7);
    const OperationPtr byName = OperationCatalog::instance().create(kName, p);
    const OperationPtr byFactory = makeDistanceTransform(7);
    EXPECT_TRUE(detail::parametersEqual(byFactory->parameters(), byName->parameters()));
    EXPECT_EQ(byFactory->parameters().count(), 1);

    // The default fills the bag: maximumDistancePx 32.
    const OperationPtr byDefault = OperationCatalog::instance().create(kName, OperationParameters());
    EXPECT_EQ(byDefault->parameters().count(), 1);
    int64_t defaultMaximum = 0;
    EXPECT_TRUE(byDefault->parameters().getInt("maximumDistancePx", defaultMaximum));
    EXPECT_EQ(defaultMaximum, 32);
    EXPECT_TRUE(detail::parametersEqual(byDefault->parameters(), makeDistanceTransform(32)->parameters()));
    EXPECT_EQ(byDefault->halo().left, 32);

    for (const OperationPtr& op : {byFactory, byName}) {
        EXPECT_STREQ(op->name(), kName);
        EXPECT_EQ(op->arity(), ChannelArity::PerChannel);
        EXPECT_FALSE(op->requiresUniformTypes());
        EXPECT_FALSE(op->producesObjects());
    }
    EXPECT_STREQ(OperationCatalog::instance().correctnessReference(kName),
                 "Published definition: exact Euclidean distance (Felzenszwalb and Huttenlocher); checked against "
                 "brute force");
    EXPECT_STREQ(OperationCatalog::instance().complexityNote(kName),
                 "O(n) in the number of samples of the halo'd tile");
}

TEST(DistanceTransform, RejectsBadMaximum) {
    for (int32_t maximum : {0, 4097}) {
        const std::string message = factoryRejection(maximum);
        EXPECT_NE(message.find("'maximumDistancePx'"), std::string::npos) << maximum << ": " << message;
        EXPECT_NE(message.find(kName), std::string::npos) << message;
        if (maximum != 0) { // "0" is a digit of the stated bound
            EXPECT_EQ(message.find(std::to_string(maximum)), std::string::npos) << message;
        }
        const std::string byName = rejection(OperationParameters().setInt("maximumDistancePx", maximum));
        EXPECT_NE(byName.find("'maximumDistancePx'"), std::string::npos) << byName;
        EXPECT_NE(byName.find(kName), std::string::npos) << byName;
        if (maximum != 0) {
            EXPECT_EQ(byName.find(std::to_string(maximum)), std::string::npos) << byName;
        }
    }
    EXPECT_TRUE(factoryRejection(1).empty());
    EXPECT_TRUE(factoryRejection(4096).empty());
    EXPECT_FALSE(factoryRejection(-5).empty());

    // By name: a Double value, an unknown key, a String.
    const std::string wrongType = rejection(OperationParameters().setDouble("maximumDistancePx", 8.0));
    EXPECT_NE(wrongType.find("maximumDistancePx"), std::string::npos) << wrongType;
    EXPECT_NE(wrongType.find(kName), std::string::npos) << wrongType;
    const std::string unknown = rejection(OperationParameters().setInt("maximumDistance", 8));
    EXPECT_NE(unknown.find("maximumDistance"), std::string::npos) << unknown;
    EXPECT_FALSE(rejection(OperationParameters().setString("maximumDistancePx", "8")).empty());
}

TEST(DistanceTransform, MatchesBruteForce) {
    std::mt19937 rng(20261125u);
    std::bernoulli_distribution foreground(0.7);
    Mask m{60, 50, std::vector<uint8_t>(60 * 50)};
    for (uint8_t& v : m.v) {
        v = foreground(rng) ? 1 : 0;
    }
    for (int32_t maximum : {3, 8, 32}) {
        expectMatchesBruteForce(m, maximum);
    }
}

TEST(DistanceTransform, DiscCentreIsItsRadius) {
    Mask m{41, 41, std::vector<uint8_t>(41 * 41)};
    for (int64_t y = 0; y < 41; ++y) {
        for (int64_t x = 0; x < 41; ++x) {
            const int64_t dx = x - 20;
            const int64_t dy = y - 20;
            m.v[static_cast<size_t>(y * 41 + x)] = dx * dx + dy * dy <= 100 ? 1 : 0;
        }
    }
    const ts::Image out = applyMask(m, 32);
    EXPECT_EQ(static_cast<float>(out.at(0, 20, 20)), bruteForce(m, 20, 20, 32));
    // The nearest background pixel is at (10, 1) from the centre: sqrt(101), just past the radius.
    EXPECT_EQ(static_cast<float>(out.at(0, 20, 20)), static_cast<float>(std::sqrt(101.0)));
    for (int64_t y = 0; y < 41; ++y) {
        for (int64_t x = 0; x < 41; ++x) {
            if (!m.at(x, y)) {
                ASSERT_EQ(out.at(0, x, y), 0.0) << x << "," << y;
            }
        }
    }
}

TEST(DistanceTransform, LevelEdgeIsBackground) {
    constexpr int64_t kWidth = 20;
    constexpr int64_t kHeight = 12;
    Mask m{kWidth, kHeight, std::vector<uint8_t>(kWidth * kHeight, 1)};
    const ts::Image out = applyMask(m, 32);
    for (int64_t y = 0; y < kHeight; ++y) {
        for (int64_t x = 0; x < kWidth; ++x) {
            const double expected = static_cast<double>(std::min({x + 1, y + 1, kWidth - x, kHeight - y}));
            ASSERT_EQ(out.at(0, x, y), expected) << x << "," << y;
        }
    }
}

TEST(DistanceTransform, NaNIsBackgroundNegativeIsForeground) {
    constexpr int64_t kSize = 9;
    const OperationPtr op = makeDistanceTransform(32);
    const ts::Image out = ts::applyDirect(*op, {DataType::Float32}, {0}, kSize, kSize, [](int32_t, int64_t x, int64_t y) {
        return x == 4 && y == 4 ? std::numeric_limits<double>::quiet_NaN() : -1.0;
    });
    ASSERT_EQ(out.types, std::vector<DataType>{DataType::Float32});
    EXPECT_EQ(out.at(0, 4, 4), 0.0);
    for (const auto& [x, y] : {std::pair<int64_t, int64_t>{3, 4}, {5, 4}, {4, 3}, {4, 5}}) {
        EXPECT_EQ(out.at(0, x, y), 1.0) << x << "," << y;
    }
    const double diagonal = static_cast<double>(std::sqrt(2.0f));
    for (const auto& [x, y] : {std::pair<int64_t, int64_t>{3, 3}, {5, 3}, {3, 5}, {5, 5}}) {
        EXPECT_EQ(out.at(0, x, y), diagonal) << x << "," << y;
    }
}

TEST(DistanceTransform, AllForegroundGivesTheMaximum) {
    constexpr int64_t kWidth = 300;
    constexpr int64_t kHeight = 260;
    constexpr int32_t kMaximum = 8;
    auto scene = ts::makeSyntheticScene({{kWidth, kHeight, 0, 0}}, {slideio::DataType::DT_Byte},
                                        [](int, int64_t, int64_t) { return 255.0; });
    PipelineSpec spec;
    spec.then(makeDistanceTransform(kMaximum));
    const ts::Image out = ts::runEager(scene, spec, 64, 4);
    ASSERT_EQ(out.width, kWidth);
    ASSERT_EQ(out.height, kHeight);
    ASSERT_EQ(out.types, std::vector<DataType>{DataType::Float32});
    for (int64_t y = 0; y < kHeight; ++y) {
        for (int64_t x = 0; x < kWidth; ++x) {
            const int64_t edge = std::min({x + 1, y + 1, kWidth - x, kHeight - y});
            const double expected = static_cast<double>(std::min<int64_t>(edge, kMaximum));
            ASSERT_EQ(out.at(0, x, y), expected) << x << "," << y;
        }
    }
}

TEST(DistanceTransform, OutputTypeIsFloat32) {
    const OperationPtr op = makeDistanceTransform(4);
    for (DataType type : {DataType::Byte, DataType::UInt16, DataType::Int32, DataType::Float64}) {
        SCOPED_TRACE(dataTypeName(type));
        EXPECT_TRUE(op->supportedTypes().contains(type));
        EXPECT_EQ(op->outputType(type), DataType::Float32);
        const ts::Image out = ts::applyDirect(*op, {type}, {0}, 9, 9, [](int32_t, int64_t x, int64_t y) {
            return x == 4 && y == 4 ? 0.0 : 5.0;
        });
        ASSERT_EQ(out.types, std::vector<DataType>{DataType::Float32});
        EXPECT_EQ(out.at(0, 4, 4), 0.0);
        EXPECT_EQ(out.at(0, 4, 3), 1.0);
        EXPECT_EQ(out.at(0, 1, 1), 2.0); // two from the level edge, the centre is further
    }
}

TEST(DistanceTransform, HaloIsTheMaximum) {
    const OperationPtr op = makeDistanceTransform(5);
    const Halo halo = op->halo();
    EXPECT_EQ(halo.left, 5);
    EXPECT_EQ(halo.top, 5);
    EXPECT_EQ(halo.right, 5);
    EXPECT_EQ(halo.bottom, 5);
}

TEST(DistanceTransform, TiledEqualsSingleTile) {
    // Blobs of foreground with a sparse speckle of background and foreground in them.
    const auto pattern = [](int, int64_t x, int64_t y) {
        const bool blob = ((x / 9 + y / 7) % 3) != 0;
        const bool speckle = (x * 31 + y * 17) % 53 == 0;
        return blob && !speckle ? 200.0 : 0.0;
    };
    for (slideio::DataType type : {slideio::DataType::DT_Byte, slideio::DataType::DT_Float32}) {
        SCOPED_TRACE(static_cast<int>(type));
        auto scene = ts::makeSyntheticScene({{300, 260, 0, 0}}, {type}, pattern);
        PipelineSpec spec;
        spec.then(makeDistanceTransform(12));
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
