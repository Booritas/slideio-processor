// Requirements: UR-PIX-8, UR-QA-2, UR-ALG-6
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
#include <utility>
#include <vector>

using namespace slideio::processor;
namespace ts = slideio::processor::testing_support;

namespace {

constexpr const char* kName = "pixel.fill_holes";

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
std::string factoryRejection(int32_t size, int64_t area = 0) {
    try {
        makeFillHoles(size, area);
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
    Mask(int64_t w, int64_t h) : width(w), height(h), v(static_cast<size_t>(w * h), 0) {}
    bool at(int64_t x, int64_t y) const { return v[static_cast<size_t>(y * width + x)] != 0; }
    void set(int64_t x, int64_t y, bool on) { v[static_cast<size_t>(y * width + x)] = on ? 1 : 0; }
    void fill(int64_t x0, int64_t y0, int64_t w, int64_t h, bool on) {
        for (int64_t y = y0; y < y0 + h; ++y) {
            for (int64_t x = x0; x < x0 + w; ++x) {
                set(x, y, on);
            }
        }
    }
    // A one-pixel-thick rectangular ring of foreground around a hole of holeW x holeH, its top left
    // corner at (x0, y0).
    void ring(int64_t x0, int64_t y0, int64_t holeW, int64_t holeH) {
        fill(x0, y0, holeW + 2, holeH + 2, true);
        fill(x0 + 1, y0 + 1, holeW, holeH, false);
    }
};

// The definition, by a flood fill over the whole image: 4-connected background
// components that do not touch the image border, with a bounding box of at most
// maxSize on each side and an area of at most maxArea (0: any). The result is
// 255 for foreground and filled holes, 0 elsewhere.
std::vector<uint8_t> bruteForce(const Mask& m, int64_t maxSize, int64_t maxArea) {
    std::vector<uint8_t> out(m.v.size(), 0);
    std::vector<uint8_t> seen(m.v.size(), 0);
    for (size_t i = 0; i < m.v.size(); ++i) {
        out[i] = m.v[i] != 0 ? 255 : 0;
    }
    for (int64_t sy = 0; sy < m.height; ++sy) {
        for (int64_t sx = 0; sx < m.width; ++sx) {
            if (m.at(sx, sy) || seen[static_cast<size_t>(sy * m.width + sx)]) {
                continue;
            }
            std::vector<std::pair<int64_t, int64_t>> stack{{sx, sy}};
            std::vector<std::pair<int64_t, int64_t>> members;
            seen[static_cast<size_t>(sy * m.width + sx)] = 1;
            int64_t minX = sx;
            int64_t maxX = sx;
            int64_t minY = sy;
            int64_t maxY = sy;
            bool touches = false;
            while (!stack.empty()) {
                const auto [x, y] = stack.back();
                stack.pop_back();
                members.emplace_back(x, y);
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
                minY = std::min(minY, y);
                maxY = std::max(maxY, y);
                if (x == 0 || y == 0 || x == m.width - 1 || y == m.height - 1) {
                    touches = true;
                }
                const int64_t dx[4] = {1, -1, 0, 0};
                const int64_t dy[4] = {0, 0, 1, -1};
                for (int k = 0; k < 4; ++k) {
                    const int64_t nx = x + dx[k];
                    const int64_t ny = y + dy[k];
                    if (nx < 0 || ny < 0 || nx >= m.width || ny >= m.height || m.at(nx, ny) ||
                        seen[static_cast<size_t>(ny * m.width + nx)]) {
                        continue;
                    }
                    seen[static_cast<size_t>(ny * m.width + nx)] = 1;
                    stack.emplace_back(nx, ny);
                }
            }
            const bool hole = !touches && maxX - minX + 1 <= maxSize && maxY - minY + 1 <= maxSize &&
                              (maxArea == 0 || static_cast<int64_t>(members.size()) <= maxArea);
            if (hole) {
                for (const auto& [x, y] : members) {
                    out[static_cast<size_t>(y * m.width + x)] = 255;
                }
            }
        }
    }
    return out;
}

ts::Image applyMask(const Mask& m, int32_t size, int64_t area = 0, DataType type = DataType::Byte) {
    const OperationPtr op = makeFillHoles(size, area);
    return ts::applyDirect(*op, {type}, {0}, m.width, m.height,
                           [&m](int32_t, int64_t x, int64_t y) { return m.at(x, y) ? 1.0 : 0.0; });
}

void expectMatchesBruteForce(const Mask& m, int32_t size, int64_t area) {
    const ts::Image out = applyMask(m, size, area);
    ASSERT_EQ(out.types, std::vector<DataType>{DataType::Byte});
    const std::vector<uint8_t> expected = bruteForce(m, size, area);
    for (int64_t y = 0; y < m.height; ++y) {
        for (int64_t x = 0; x < m.width; ++x) {
            ASSERT_EQ(out.at(0, x, y), static_cast<double>(expected[static_cast<size_t>(y * m.width + x)]))
                << "size " << size << " area " << area << " at " << x << "," << y;
        }
    }
}

} // namespace

TEST(FillHoles, FactoryEqualsNamePath) {
    OperationParameters p;
    p.setInt("maximumHoleSizePx", 9).setInt("maximumHoleAreaPx", 20);
    const OperationPtr byName = OperationCatalog::instance().create(kName, p);
    const OperationPtr byFactory = makeFillHoles(9, 20);
    EXPECT_TRUE(detail::parametersEqual(byFactory->parameters(), byName->parameters()));
    EXPECT_EQ(byFactory->parameters().count(), 2);

    // The default holds the size, 32, and no area: the key stays absent.
    const OperationPtr byDefault = OperationCatalog::instance().create(kName, OperationParameters());
    EXPECT_EQ(byDefault->parameters().count(), 1);
    int64_t size = 0;
    EXPECT_TRUE(byDefault->parameters().getInt("maximumHoleSizePx", size));
    EXPECT_EQ(size, 32);
    int64_t area = 0;
    EXPECT_FALSE(byDefault->parameters().getInt("maximumHoleAreaPx", area));
    EXPECT_TRUE(detail::parametersEqual(byDefault->parameters(), makeFillHoles(32)->parameters()));
    EXPECT_EQ(makeFillHoles(32, 0)->parameters().count(), 1);
    EXPECT_EQ(byDefault->halo().left, 32);
    EXPECT_EQ(byFactory->halo().bottom, 9);

    for (const OperationPtr& op : {byFactory, byName}) {
        EXPECT_STREQ(op->name(), kName);
        EXPECT_EQ(op->arity(), ChannelArity::PerChannel);
        EXPECT_FALSE(op->requiresUniformTypes());
        EXPECT_FALSE(op->producesObjects());
        EXPECT_TRUE(op->supportedTypes().contains(DataType::Float64));
        EXPECT_EQ(op->outputType(DataType::Float32), DataType::Byte);
    }
    EXPECT_STREQ(OperationCatalog::instance().correctnessReference(kName),
                 "Published definition: background components enclosed within the declared size; checked against a "
                 "brute-force flood fill");
    EXPECT_STREQ(OperationCatalog::instance().complexityNote(kName),
                 "O(n) in the number of samples of the halo'd tile");
}

TEST(FillHoles, RejectsBadParameters) {
    for (int32_t size : {0, 4097}) {
        const std::string message = factoryRejection(size);
        EXPECT_NE(message.find("'maximumHoleSizePx'"), std::string::npos) << size << ": " << message;
        EXPECT_NE(message.find(kName), std::string::npos) << message;
        if (size != 0) { // "0" is a digit of the stated bound
            EXPECT_EQ(message.find(std::to_string(size)), std::string::npos) << message;
        }
        const std::string byName = rejection(OperationParameters().setInt("maximumHoleSizePx", size));
        EXPECT_NE(byName.find("'maximumHoleSizePx'"), std::string::npos) << byName;
        EXPECT_NE(byName.find(kName), std::string::npos) << byName;
        if (size != 0) {
            EXPECT_EQ(byName.find(std::to_string(size)), std::string::npos) << byName;
        }
    }
    EXPECT_TRUE(factoryRejection(1).empty());
    EXPECT_TRUE(factoryRejection(4096).empty());
    EXPECT_FALSE(factoryRejection(-5).empty());

    // An area of 0 given by name is below the bound; by the factory it is absent. A negative one throws.
    const std::string zeroArea = rejection(OperationParameters().setInt("maximumHoleAreaPx", 0));
    EXPECT_NE(zeroArea.find("'maximumHoleAreaPx'"), std::string::npos) << zeroArea;
    EXPECT_NE(zeroArea.find(kName), std::string::npos) << zeroArea;
    const std::string negativeByName = rejection(OperationParameters().setInt("maximumHoleAreaPx", -7));
    EXPECT_NE(negativeByName.find("'maximumHoleAreaPx'"), std::string::npos) << negativeByName;
    EXPECT_EQ(negativeByName.find("-7"), std::string::npos) << negativeByName;
    const std::string negative = factoryRejection(8, -7);
    EXPECT_NE(negative.find("'maximumHoleAreaPx'"), std::string::npos) << negative;
    EXPECT_NE(negative.find(kName), std::string::npos) << negative;
    EXPECT_EQ(negative.find("-7"), std::string::npos) << negative;
    EXPECT_TRUE(rejection(OperationParameters().setInt("maximumHoleAreaPx", 1)).empty());

    // Wrong types and an unknown key.
    for (const char* key : {"maximumHoleSizePx", "maximumHoleAreaPx"}) {
        const std::string wrongType = rejection(OperationParameters().setDouble(key, 8.0));
        EXPECT_NE(wrongType.find(key), std::string::npos) << wrongType;
        EXPECT_NE(wrongType.find(kName), std::string::npos) << wrongType;
        EXPECT_FALSE(rejection(OperationParameters().setString(key, "8")).empty());
    }
    const std::string unknown = rejection(OperationParameters().setInt("maximumHole", 8));
    EXPECT_NE(unknown.find("maximumHole"), std::string::npos) << unknown;
}

TEST(FillHoles, BoundIsInclusive) {
    // Holes of 5x5 and 6x6 inside one-thick rings, with the bound 5.
    Mask m(40, 20);
    m.ring(2, 2, 5, 5);
    m.ring(15, 2, 6, 6);
    const ts::Image out = applyMask(m, 5);
    EXPECT_EQ(out.at(0, 4, 4), 255.0);  // the 5x5 hole, filled
    EXPECT_EQ(out.at(0, 2, 2), 255.0);  // its wall
    EXPECT_EQ(out.at(0, 18, 5), 0.0);   // the 6x6 hole, not filled
    EXPECT_EQ(out.at(0, 30, 10), 0.0);  // plain background
    expectMatchesBruteForce(m, 5, 0);
    expectMatchesBruteForce(m, 6, 0);
}

TEST(FillHoles, AreaLimit) {
    Mask m(20, 20);
    m.ring(3, 3, 4, 4);
    EXPECT_EQ(applyMask(m, 10, 16).at(0, 5, 5), 255.0);
    const ts::Image notFilled = applyMask(m, 10, 15);
    EXPECT_EQ(notFilled.at(0, 5, 5), 0.0);
    EXPECT_EQ(notFilled.at(0, 3, 3), 255.0); // the wall is foreground
    EXPECT_EQ(applyMask(m, 10).at(0, 5, 5), 255.0);
}

TEST(FillHoles, RegionOpenToLevelEdgeStaysOpen) {
    // A U shape whose opening is on the level's left edge: the background inside reaches the edge.
    Mask m(30, 20);
    m.fill(0, 5, 12, 1, true);
    m.fill(0, 11, 12, 1, true);
    m.fill(11, 5, 1, 7, true);
    const ts::Image out = applyMask(m, 4);
    for (int64_t y = 6; y < 11; ++y) {
        for (int64_t x = 0; x < 11; ++x) {
            ASSERT_EQ(out.at(0, x, y), 0.0) << x << "," << y;
        }
    }
    // Even with a bound far above its size.
    const ts::Image large = applyMask(m, 100);
    EXPECT_EQ(large.at(0, 5, 8), 0.0);
    // Closed by a wall at its left end, the same shape is a hole and is filled.
    Mask closed(30, 20);
    closed.fill(1, 5, 11, 1, true);
    closed.fill(1, 11, 11, 1, true);
    closed.fill(11, 5, 1, 7, true);
    closed.fill(1, 5, 1, 7, true);
    EXPECT_EQ(applyMask(closed, 10).at(0, 5, 8), 255.0);
}

TEST(FillHoles, DiagonalGapIsAHole) {
    // A ring of 7x7 whose corner pixel (3,3) is background: the outside reaches it, and the inside
    // pixel (4,4) is its diagonal neighbour only. The wall pixels (4,3) and (3,4) meet diagonally, so
    // the foreground is 8-connected and the inside is a 4-connected hole.
    Mask m(20, 20);
    m.ring(3, 3, 5, 5);
    m.set(3, 3, false);
    const ts::Image out = applyMask(m, 8);
    EXPECT_EQ(out.at(0, 4, 4), 255.0);
    EXPECT_EQ(out.at(0, 7, 7), 255.0);
    EXPECT_EQ(out.at(0, 3, 3), 0.0); // the outside stays background
    EXPECT_EQ(out.at(0, 0, 0), 0.0);
    expectMatchesBruteForce(m, 8, 0);
}

TEST(FillHoles, NestedRings) {
    // A hole of 19x19 holding an island of 11x11 holding a hole of 9x9.
    Mask m(30, 30);
    m.fill(2, 2, 21, 21, true);
    m.fill(3, 3, 19, 19, false);
    m.fill(7, 7, 11, 11, true);
    m.fill(8, 8, 9, 9, false);
    const ts::Image out = applyMask(m, 20);
    for (int64_t y = 2; y < 23; ++y) {
        for (int64_t x = 2; x < 23; ++x) {
            ASSERT_EQ(out.at(0, x, y), 255.0) << x << "," << y;
        }
    }
    EXPECT_EQ(out.at(0, 0, 0), 0.0);
    expectMatchesBruteForce(m, 20, 0);
    // With a bound of 10 only the inner hole is filled.
    const ts::Image inner = applyMask(m, 10);
    EXPECT_EQ(inner.at(0, 12, 12), 255.0);
    EXPECT_EQ(inner.at(0, 4, 4), 0.0);
    expectMatchesBruteForce(m, 10, 0);
}

TEST(FillHoles, NaNIsBackground) {
    const OperationPtr op = makeFillHoles(8);
    const ts::Image out = ts::applyDirect(*op, {DataType::Float32}, {0}, 9, 9, [](int32_t, int64_t x, int64_t y) {
        if (x == 4 && y == 4) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        return x >= 2 && x <= 6 && y >= 2 && y <= 6 ? 1.0 : 0.0;
    });
    ASSERT_EQ(out.types, std::vector<DataType>{DataType::Byte});
    EXPECT_EQ(out.at(0, 4, 4), 255.0);
    EXPECT_EQ(out.at(0, 3, 3), 255.0);
    EXPECT_EQ(out.at(0, 0, 0), 0.0);
}

TEST(FillHoles, OutputIsByte255) {
    const OperationPtr op = makeFillHoles(4);
    for (DataType type : {DataType::UInt16, DataType::Float32}) {
        SCOPED_TRACE(dataTypeName(type));
        const ts::Image out = ts::applyDirect(*op, {type}, {0}, 9, 9, [](int32_t, int64_t x, int64_t y) {
            if (x == 4 && y == 4) {
                return 0.0;
            }
            return x >= 3 && x <= 5 && y >= 3 && y <= 5 ? 1234.0 : 0.0;
        });
        ASSERT_EQ(out.types, std::vector<DataType>{DataType::Byte});
        EXPECT_EQ(out.at(0, 3, 3), 255.0);
        EXPECT_EQ(out.at(0, 4, 4), 255.0);
        EXPECT_EQ(out.at(0, 1, 1), 0.0);
    }
}

TEST(FillHoles, MatchesBruteForce) {
    std::mt19937 rng(20261125u);
    std::bernoulli_distribution foreground(0.55);
    Mask m(60, 50);
    for (uint8_t& v : m.v) {
        v = foreground(rng) ? 1 : 0;
    }
    for (int32_t size : {2, 4, 8}) {
        for (int64_t area : {int64_t{0}, int64_t{6}}) {
            expectMatchesBruteForce(m, size, area);
        }
    }
}

TEST(FillHoles, TiledEqualsSingleTile) {
    // Ring blobs with holes of 3 to 14 pixels on a grid whose period divides no tile size, plus a speckle.
    constexpr int64_t kWidth = 300;
    constexpr int64_t kHeight = 260;
    Mask m(kWidth, kHeight);
    int64_t index = 0;
    for (int64_t y = 4; y + 20 < kHeight; y += 23) {
        for (int64_t x = 3; x + 20 < kWidth; x += 27) {
            ++index;
            m.ring(x, y, 3 + (index % 12), 3 + ((index * 5) % 12));
        }
    }
    std::mt19937 rng(7u);
    std::bernoulli_distribution speckle(0.02);
    for (int64_t y = 0; y < kHeight; ++y) {
        for (int64_t x = 0; x < kWidth; ++x) {
            if (speckle(rng)) {
                m.set(x, y, true);
            }
        }
    }
    const auto pattern = [&m](int, int64_t x, int64_t y) { return m.at(x, y) ? 200.0 : 0.0; };
    const std::vector<uint8_t> expected = bruteForce(m, 10, 0);
    for (slideio::DataType type : {slideio::DataType::DT_Byte, slideio::DataType::DT_Float32}) {
        SCOPED_TRACE(static_cast<int>(type));
        auto scene = ts::makeSyntheticScene({{kWidth, kHeight, 0, 0}}, {type}, pattern);
        PipelineSpec spec;
        spec.then(makeFillHoles(10));
        const ts::Image reference = ts::runEager(scene, spec, 4096, 1);
        ASSERT_EQ(reference.width, kWidth);
        ASSERT_EQ(reference.height, kHeight);
        ASSERT_EQ(reference.types, std::vector<DataType>{DataType::Byte});
        // The single-tile run is the definition.
        for (int64_t y = 0; y < kHeight; ++y) {
            for (int64_t x = 0; x < kWidth; ++x) {
                ASSERT_EQ(reference.at(0, x, y), static_cast<double>(expected[static_cast<size_t>(y * kWidth + x)]))
                    << x << "," << y;
            }
        }
        for (int32_t tile : {64, 100, 257}) {
            const ts::Image tiled = ts::runEager(scene, spec, tile, 4);
            EXPECT_TRUE(tiled.planes == reference.planes) << "tile " << tile;
        }
    }
}
