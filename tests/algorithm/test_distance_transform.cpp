// Requirements: UR-PIX-7, UR-QA-2, UR-ALG-6
#include <slideio/processor/algorithm/pixel/filter.hpp>
#include <slideio/processor/core/block_view.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/pipeline/operation_catalog.hpp>
#include <slideio/processor/pipeline/pipeline_spec.hpp>

#include <algorithm/support/harness.hpp>
#include <pipeline/detail/operation_context_state.hpp>
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

namespace {

// A mostly foreground mask with a few background pixels, so that the distances
// are long and the context is far larger than the image.
Mask sparseMask(int64_t width, int64_t height, int count, uint32_t seed) {
    Mask m{width, height, std::vector<uint8_t>(static_cast<size_t>(width * height), 1)};
    std::mt19937 rng(seed);
    for (int i = 0; i < count; ++i) {
        const int64_t x = static_cast<int64_t>(rng() % static_cast<uint32_t>(width));
        const int64_t y = static_cast<int64_t>(rng() % static_cast<uint32_t>(height));
        m.v[static_cast<size_t>(y * width + x)] = 0;
    }
    return m;
}

} // namespace

TEST(DistanceTransform, MatchesBruteForceAtALargeMaximum) {
    // OpenCV's exact path intersects parabolas in float: long distances in a
    // context of thousands of pixels must still be the exact square roots.
    for (int32_t maximum : {300, 1000}) {
        expectMatchesBruteForce(sparseMask(220, 170, 6, 20261125u + static_cast<uint32_t>(maximum)), maximum);
    }
}

TEST(DistanceTransform, TiledEqualsSingleTileAtALargeMaximum) {
    const Mask m = sparseMask(300, 260, 8, 77u);
    auto scene = ts::makeSyntheticScene({{m.width, m.height, 0, 0}}, {slideio::DataType::DT_Byte},
                                        [&m](int, int64_t x, int64_t y) { return m.at(x, y) ? 255.0 : 0.0; });
    PipelineSpec spec;
    spec.then(makeDistanceTransform(600));
    const ts::Image reference = ts::runEager(scene, spec, 4096, 1);
    ASSERT_EQ(reference.types, std::vector<DataType>{DataType::Float32});
    for (int32_t tile : {64, 100, 257}) {
        const ts::Image tiled = ts::runEager(scene, spec, tile, 4);
        EXPECT_TRUE(tiled.planes == reference.planes) << "tile " << tile;
    }
    // And the single tile is the brute force.
    for (int64_t y = 0; y < m.height; y += 7) {
        for (int64_t x = 0; x < m.width; x += 5) {
            ASSERT_EQ(static_cast<float>(reference.at(0, x, y)), bruteForce(m, x, y, 600)) << x << "," << y;
        }
    }
}

namespace {

// A background pixel, in the coordinates of the picture it lies in.
struct Point {
    int64_t x;
    int64_t y;
};

// min(H, the distance from (x, y) to the nearest of @p points), and, when
// @p width and @p height are positive, to the nearest pixel centre outside a
// width x height level. As the operation states it: the square root of the
// integer squared distance, in double, then as float.
float bruteForcePoints(const std::vector<Point>& points, int64_t x, int64_t y, int32_t maximum, int64_t width = 0,
                       int64_t height = 0) {
    int64_t best = static_cast<int64_t>(maximum) * maximum;
    if (width > 0 && height > 0) {
        const int64_t edge = std::min({x + 1, y + 1, width - x, height - y});
        best = std::min(best, edge * edge);
    }
    for (const Point& p : points) {
        const int64_t dx = x - p.x;
        const int64_t dy = y - p.y;
        best = std::min(best, dx * dx + dy * dy);
    }
    return static_cast<float>(std::sqrt(static_cast<double>(best)));
}

// Applies @p op to the tile @p tile of a context of width x height Byte pixels,
// every one foreground but @p background: the context is the tile grown by the
// operation's halo, inside the level, as the framework gives it away from the
// level's edge. Returns the tile's Float32 output.
std::vector<float> applyInContext(Operation& op, int64_t width, int64_t height, const std::vector<Point>& background,
                                  const Rect64& tile) {
    std::vector<uint8_t> context(static_cast<size_t>(width * height), uint8_t{255});
    for (const Point& p : background) {
        context[static_cast<size_t>(p.y * width + p.x)] = 0;
    }
    std::vector<float> out(static_cast<size_t>(tile.width * tile.height), -1.0f);
    const BlockPlane inPlane{context.data(), DataType::Byte, width, 0};
    const BlockPlane outPlane{out.data(), DataType::Float32, tile.width * static_cast<int64_t>(sizeof(float)), 0};
    const BlockView input(width, height, Rect64{0, 0, width, height}, 0, &inPlane, 1);
    BlockView output(tile.width, tile.height, tile, 0, &outPlane, 1);
    detail::OperationContextState state;
    state.input = &input;
    state.output = &output;
    state.level = 0;
    state.tileRectAtLevel = state.tileRect = tile;
    state.haloRectAtLevel = state.haloRect = Rect64{0, 0, width, height};
    state.levelScale = 1.0;
    state.tileIndex = 0;
    op.apply(*detail::OperationContextAccess::make(std::move(state)));
    return out;
}

} // namespace

TEST(DistanceTransform, ExactAtLongDistancesInAWideContext) {
    // The final review's case: a parabola breakpoint computed in float32 (as
    // OpenCV 4.14.0's precise mode does) falls on the wrong side of an integer
    // at x = 5289 of an 8192-wide context, choosing the background pixel at
    // column 4481 (squared distance 949889) over the one at column 6081
    // (949888). The breakpoint depends on the absolute column, so the error
    // depends on the tiling.
    constexpr int32_t kMaximum = 2048;
    constexpr int64_t kWidth = 4096 + 2 * kMaximum;
    constexpr int64_t kHeight = 1 + 2 * kMaximum;
    const Rect64 tile{kMaximum, kMaximum, 4096, 1};
    const std::vector<Point> background{{4481, kMaximum - 545}, {6081, kMaximum - 568}};
    const OperationPtr op = makeDistanceTransform(kMaximum);
    const std::vector<float> out = applyInContext(*op, kWidth, kHeight, background, tile);
    EXPECT_EQ(out[static_cast<size_t>(5289 - kMaximum)], static_cast<float>(std::sqrt(949888.0)));
    for (int64_t x = 0; x < tile.width; ++x) {
        ASSERT_EQ(out[static_cast<size_t>(x)], bruteForcePoints(background, tile.x + x, tile.y, kMaximum))
            << "x " << tile.x + x;
    }

    // Sparse background in wide contexts at the largest maximums: every
    // distance of the tile row is exact.
    for (int32_t maximum : {2048, 4096}) {
        const int64_t width = 4096 + 2 * static_cast<int64_t>(maximum);
        const int64_t height = 1 + 2 * static_cast<int64_t>(maximum);
        std::mt19937 rng(static_cast<uint32_t>(maximum) * 7919u);
        std::vector<Point> points;
        for (int i = 0; i < 60; ++i) {
            points.push_back(Point{static_cast<int64_t>(rng() % static_cast<uint32_t>(width)),
                                   static_cast<int64_t>(rng() % static_cast<uint32_t>(height))});
        }
        const Rect64 row{maximum, maximum, 4096, 1};
        const OperationPtr wide = makeDistanceTransform(maximum);
        const std::vector<float> distances = applyInContext(*wide, width, height, points, row);
        for (int64_t x = 0; x < row.width; ++x) {
            ASSERT_EQ(distances[static_cast<size_t>(x)], bruteForcePoints(points, row.x + x, row.y, maximum))
                << "maximum " << maximum << " x " << row.x + x;
        }
    }
}

TEST(DistanceTransform, TiledEqualsSingleTileAtLongDistances) {
    // A mostly foreground level thousands of pixels across, so the true
    // distances run to the maximum and the contexts are thousands wide; tiles
    // of different sizes put every pixel at different context columns.
    constexpr int64_t kWidth = 3000;
    constexpr int64_t kHeight = 2600;
    constexpr int32_t kMaximum = 1100;
    std::mt19937 rng(20261125u);
    std::vector<Point> points;
    for (int i = 0; i < 12; ++i) {
        points.push_back(Point{static_cast<int64_t>(rng() % static_cast<uint32_t>(kWidth)),
                               static_cast<int64_t>(rng() % static_cast<uint32_t>(kHeight))});
    }
    std::vector<uint8_t> mask(static_cast<size_t>(kWidth * kHeight), uint8_t{1});
    for (const Point& p : points) {
        mask[static_cast<size_t>(p.y * kWidth + p.x)] = 0;
    }
    auto scene = ts::makeSyntheticScene({{kWidth, kHeight, 0, 0}}, {slideio::DataType::DT_Byte},
                                        [&mask](int, int64_t x, int64_t y) {
                                            return mask[static_cast<size_t>(y * kWidth + x)] != 0 ? 255.0 : 0.0;
                                        });
    PipelineSpec spec;
    spec.then(makeDistanceTransform(kMaximum));
    const ts::Image reference = ts::runEager(scene, spec, 4096, 1);
    ASSERT_EQ(reference.width, kWidth);
    ASSERT_EQ(reference.height, kHeight);
    ASSERT_EQ(reference.types, std::vector<DataType>{DataType::Float32});
    for (int64_t y = 0; y < kHeight; y += 23) {
        for (int64_t x = 0; x < kWidth; x += 19) {
            ASSERT_EQ(static_cast<float>(reference.at(0, x, y)),
                      bruteForcePoints(points, x, y, kMaximum, kWidth, kHeight))
                << x << "," << y;
        }
    }
    for (int32_t tile : {1000, 1536}) {
        const ts::Image tiled = ts::runEager(scene, spec, tile, 4);
        EXPECT_TRUE(tiled.planes == reference.planes) << "tile " << tile;
    }
}
