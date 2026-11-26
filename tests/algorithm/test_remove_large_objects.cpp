// Requirements: UR-PIX-9, UR-QA-2, UR-QA-3, UR-ALG-6
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
#include <utility>
#include <vector>

using namespace slideio::processor;
namespace ts = slideio::processor::testing_support;

namespace {

constexpr const char* kName = "pixel.remove_large_objects";

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
std::string factoryRejection(int32_t size) {
    try {
        makeRemoveLargeObjects(size);
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
};

// The definition, by a flood fill over the whole image: 8-connected foreground
// components whose bounding box is at most maxSize wide and at most maxSize high.
// The result is 255 for the foreground of those, 0 elsewhere.
std::vector<uint8_t> bruteForce(const Mask& m, int64_t maxSize) {
    std::vector<uint8_t> out(m.v.size(), 0);
    std::vector<uint8_t> seen(m.v.size(), 0);
    for (int64_t sy = 0; sy < m.height; ++sy) {
        for (int64_t sx = 0; sx < m.width; ++sx) {
            if (!m.at(sx, sy) || seen[static_cast<size_t>(sy * m.width + sx)]) {
                continue;
            }
            std::vector<std::pair<int64_t, int64_t>> stack{{sx, sy}};
            std::vector<std::pair<int64_t, int64_t>> members;
            seen[static_cast<size_t>(sy * m.width + sx)] = 1;
            int64_t minX = sx;
            int64_t maxX = sx;
            int64_t minY = sy;
            int64_t maxY = sy;
            while (!stack.empty()) {
                const auto [x, y] = stack.back();
                stack.pop_back();
                members.emplace_back(x, y);
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
                minY = std::min(minY, y);
                maxY = std::max(maxY, y);
                for (int64_t ny = y - 1; ny <= y + 1; ++ny) {
                    for (int64_t nx = x - 1; nx <= x + 1; ++nx) {
                        if (nx < 0 || ny < 0 || nx >= m.width || ny >= m.height || !m.at(nx, ny) ||
                            seen[static_cast<size_t>(ny * m.width + nx)]) {
                            continue;
                        }
                        seen[static_cast<size_t>(ny * m.width + nx)] = 1;
                        stack.emplace_back(nx, ny);
                    }
                }
            }
            if (maxX - minX + 1 <= maxSize && maxY - minY + 1 <= maxSize) {
                for (const auto& [x, y] : members) {
                    out[static_cast<size_t>(y * m.width + x)] = 255;
                }
            }
        }
    }
    return out;
}

ts::Image applyMask(const Mask& m, int32_t size, DataType type = DataType::Byte) {
    const OperationPtr op = makeRemoveLargeObjects(size);
    return ts::applyDirect(*op, {type}, {0}, m.width, m.height,
                           [&m](int32_t, int64_t x, int64_t y) { return m.at(x, y) ? 1.0 : 0.0; });
}

void expectMatchesBruteForce(const Mask& m, int32_t size) {
    const ts::Image out = applyMask(m, size);
    ASSERT_EQ(out.types, std::vector<DataType>{DataType::Byte});
    const std::vector<uint8_t> expected = bruteForce(m, size);
    for (int64_t y = 0; y < m.height; ++y) {
        for (int64_t x = 0; x < m.width; ++x) {
            ASSERT_EQ(out.at(0, x, y), static_cast<double>(expected[static_cast<size_t>(y * m.width + x)]))
                << "size " << size << " at " << x << "," << y;
        }
    }
}

} // namespace

TEST(RemoveLargeObjects, FactoryEqualsNamePath) {
    OperationParameters p;
    p.setInt("maximumObjectSizePx", 7);
    const OperationPtr byName = OperationCatalog::instance().create(kName, p);
    const OperationPtr byFactory = makeRemoveLargeObjects(7);
    EXPECT_TRUE(detail::parametersEqual(byFactory->parameters(), byName->parameters()));
    EXPECT_EQ(byFactory->parameters().count(), 1);
    EXPECT_EQ(byName->halo().left, 7);
    EXPECT_EQ(byFactory->halo().bottom, 7);

    Mask m(30, 20);
    m.fill(2, 2, 5, 5, true);
    m.fill(12, 3, 9, 4, true);
    const auto pattern = [&m](int32_t, int64_t x, int64_t y) { return m.at(x, y) ? 1.0 : 0.0; };
    const ts::Image a = ts::applyDirect(*byFactory, {DataType::Byte}, {0}, m.width, m.height, pattern);
    const ts::Image b = ts::applyDirect(*byName, {DataType::Byte}, {0}, m.width, m.height, pattern);
    EXPECT_TRUE(a.planes == b.planes);

    // The default is 32.
    const OperationPtr byDefault = OperationCatalog::instance().create(kName, OperationParameters());
    EXPECT_EQ(byDefault->parameters().count(), 1);
    int64_t size = 0;
    EXPECT_TRUE(byDefault->parameters().getInt("maximumObjectSizePx", size));
    EXPECT_EQ(size, 32);
    EXPECT_TRUE(detail::parametersEqual(byDefault->parameters(), makeRemoveLargeObjects(32)->parameters()));
    EXPECT_EQ(byDefault->halo().left, 32);

    for (const OperationPtr& op : {byFactory, byName}) {
        EXPECT_STREQ(op->name(), kName);
        EXPECT_EQ(op->arity(), ChannelArity::PerChannel);
        EXPECT_FALSE(op->requiresUniformTypes());
        EXPECT_FALSE(op->producesObjects());
        EXPECT_TRUE(op->supportedTypes().contains(DataType::Float64));
        EXPECT_EQ(op->outputType(DataType::Float32), DataType::Byte);
    }
    EXPECT_STREQ(OperationCatalog::instance().correctnessReference(kName),
                 "Published definition: 8-connected foreground components within the declared size; checked "
                 "against a brute-force flood fill");
    EXPECT_STREQ(OperationCatalog::instance().complexityNote(kName),
                 "O(n) in the number of samples of the halo'd tile");
}

TEST(RemoveLargeObjects, RejectsBadParameters) {
    for (int32_t size : {0, 4097}) {
        const std::string message = factoryRejection(size);
        EXPECT_NE(message.find("'maximumObjectSizePx'"), std::string::npos) << size << ": " << message;
        EXPECT_NE(message.find(kName), std::string::npos) << message;
        if (size != 0) { // "0" is a digit of the stated bound
            EXPECT_EQ(message.find(std::to_string(size)), std::string::npos) << message;
        }
        const std::string byName = rejection(OperationParameters().setInt("maximumObjectSizePx", size));
        EXPECT_NE(byName.find("'maximumObjectSizePx'"), std::string::npos) << byName;
        EXPECT_NE(byName.find(kName), std::string::npos) << byName;
        if (size != 0) {
            EXPECT_EQ(byName.find(std::to_string(size)), std::string::npos) << byName;
        }
    }
    EXPECT_TRUE(factoryRejection(1).empty());
    EXPECT_TRUE(factoryRejection(4096).empty());
    const std::string negative = factoryRejection(-5);
    EXPECT_NE(negative.find("'maximumObjectSizePx'"), std::string::npos) << negative;
    EXPECT_EQ(negative.find("-5"), std::string::npos) << negative;
    const std::string negativeByName = rejection(OperationParameters().setInt("maximumObjectSizePx", -5));
    EXPECT_NE(negativeByName.find("'maximumObjectSizePx'"), std::string::npos) << negativeByName;
    EXPECT_EQ(negativeByName.find("-5"), std::string::npos) << negativeByName;

    // Wrong types and an unknown key.
    const std::string wrongType = rejection(OperationParameters().setDouble("maximumObjectSizePx", 8.0));
    EXPECT_NE(wrongType.find("maximumObjectSizePx"), std::string::npos) << wrongType;
    EXPECT_NE(wrongType.find(kName), std::string::npos) << wrongType;
    EXPECT_FALSE(rejection(OperationParameters().setString("maximumObjectSizePx", "8")).empty());
    const std::string unknown = rejection(OperationParameters().setInt("maximumObject", 8));
    EXPECT_NE(unknown.find("maximumObject"), std::string::npos) << unknown;
}

TEST(RemoveLargeObjects, BoundIsInclusive) {
    // With the bound 5: a 5x5 square and a 1x5 bar are kept; 6x5, 5x6 and a 1x6 bar are removed.
    Mask m(60, 20);
    m.fill(3, 3, 5, 5, true);  // kept
    m.fill(12, 3, 6, 5, true); // removed: too wide
    m.fill(22, 3, 5, 6, true); // removed: too high
    m.fill(31, 3, 1, 5, true); // kept: a bar
    m.fill(36, 3, 1, 6, true); // removed: a longer bar
    m.fill(41, 3, 5, 1, true); // kept: a horizontal bar
    m.fill(49, 3, 6, 1, true); // removed: a longer horizontal bar
    const ts::Image out = applyMask(m, 5);
    EXPECT_EQ(out.at(0, 5, 5), 255.0);
    EXPECT_EQ(out.at(0, 3, 3), 255.0);
    EXPECT_EQ(out.at(0, 14, 5), 0.0);
    EXPECT_EQ(out.at(0, 24, 5), 0.0);
    EXPECT_EQ(out.at(0, 31, 5), 255.0);
    EXPECT_EQ(out.at(0, 36, 5), 0.0);
    EXPECT_EQ(out.at(0, 43, 3), 255.0);
    EXPECT_EQ(out.at(0, 52, 3), 0.0);
    EXPECT_EQ(out.at(0, 30, 15), 0.0); // plain background
    expectMatchesBruteForce(m, 5);
}

TEST(RemoveLargeObjects, DiagonalChainIsOneObject) {
    // Six pixels on a diagonal are one 6x6 object: removed at 5.
    Mask chain(20, 20);
    for (int64_t i = 0; i < 6; ++i) {
        chain.set(4 + i, 4 + i, true);
    }
    const ts::Image removed = applyMask(chain, 5);
    for (int64_t i = 0; i < 6; ++i) {
        ASSERT_EQ(removed.at(0, 4 + i, 4 + i), 0.0) << i;
    }
    expectMatchesBruteForce(chain, 5);
    // With one gap it is two objects, of 3x3 and 2x2: kept.
    Mask gap = chain;
    gap.set(7, 7, false);
    const ts::Image kept = applyMask(gap, 5);
    for (int64_t i = 0; i < 6; ++i) {
        ASSERT_EQ(kept.at(0, 4 + i, 4 + i), i == 3 ? 0.0 : 255.0) << i;
    }
    expectMatchesBruteForce(gap, 5);
}

TEST(RemoveLargeObjects, SmallObjectAtLevelEdgeKept) {
    Mask m(30, 20);
    m.fill(0, 0, 3, 3, true);   // in the corner, kept
    m.fill(10, 0, 10, 3, true); // along the top edge, wider than the bound
    const ts::Image out = applyMask(m, 5);
    EXPECT_EQ(out.at(0, 0, 0), 255.0);
    EXPECT_EQ(out.at(0, 2, 2), 255.0);
    EXPECT_EQ(out.at(0, 10, 0), 0.0);
    EXPECT_EQ(out.at(0, 19, 2), 0.0);
    // At the far corner too.
    Mask far(30, 20);
    far.fill(27, 17, 3, 3, true);
    EXPECT_EQ(applyMask(far, 5).at(0, 29, 19), 255.0);
    expectMatchesBruteForce(m, 5);
    expectMatchesBruteForce(far, 5);
}

TEST(RemoveLargeObjects, NaNIsBackground) {
    // NaN separates two 4x4 squares that would otherwise form an 8x4 object.
    const OperationPtr op = makeRemoveLargeObjects(5);
    const ts::Image out = ts::applyDirect(*op, {DataType::Float32}, {0}, 16, 10, [](int32_t, int64_t x, int64_t y) {
        if (y < 3 || y > 6 || x < 3 || x > 10) {
            return 0.0;
        }
        return x == 7 ? std::numeric_limits<double>::quiet_NaN() : 1.0;
    });
    ASSERT_EQ(out.types, std::vector<DataType>{DataType::Byte});
    EXPECT_EQ(out.at(0, 3, 3), 255.0);
    EXPECT_EQ(out.at(0, 6, 6), 255.0);
    EXPECT_EQ(out.at(0, 7, 4), 0.0);
    EXPECT_EQ(out.at(0, 8, 3), 255.0);
    EXPECT_EQ(out.at(0, 10, 6), 255.0);
}

TEST(RemoveLargeObjects, OutputIsByte255) {
    Mask m(30, 12);
    m.fill(2, 2, 3, 3, true);
    m.fill(10, 2, 9, 3, true);
    const ts::Image reference = applyMask(m, 4);
    EXPECT_EQ(reference.at(0, 3, 3), 255.0);
    EXPECT_EQ(reference.at(0, 12, 3), 0.0);
    for (DataType type : {DataType::UInt16, DataType::Float32}) {
        SCOPED_TRACE(dataTypeName(type));
        const ts::Image out = applyMask(m, 4, type);
        ASSERT_EQ(out.types, std::vector<DataType>{DataType::Byte});
        EXPECT_TRUE(out.planes == reference.planes);
    }
}

TEST(RemoveLargeObjects, MatchesBruteForce) {
    std::mt19937 rng(20261126u);
    for (double density : {0.55, 0.30}) {
        std::bernoulli_distribution foreground(density);
        Mask m(60, 50);
        for (uint8_t& v : m.v) {
            v = foreground(rng) ? 1 : 0;
        }
        for (int32_t size : {2, 4, 8}) {
            SCOPED_TRACE(density);
            expectMatchesBruteForce(m, size);
        }
    }
}

TEST(RemoveLargeObjects, TiledEqualsSingleTile) {
    // Rectangles of 20 to 39 pixels a side on a grid whose period divides no tile size (some are within
    // the bound, some above it), small specks, and one blob of 120x90.
    constexpr int64_t kWidth = 300;
    constexpr int64_t kHeight = 260;
    constexpr int32_t kSize = 32;
    Mask m(kWidth, kHeight);
    int64_t index = 0;
    for (int64_t y = 4; y + 40 < kHeight; y += 47) {
        for (int64_t x = 3; x + 40 < kWidth; x += 51) {
            ++index;
            m.fill(x, y, 20 + (index * 7) % 20, 20 + (index * 11) % 20, true);
        }
    }
    for (int64_t x = 60; x < kWidth - 4; x += 63) {
        m.fill(x, 120, 3, 3, true);
    }
    m.fill(140, 100, 120, 90, true);
    const auto pattern = [&m](int, int64_t x, int64_t y) { return m.at(x, y) ? 200.0 : 0.0; };
    const std::vector<uint8_t> expected = bruteForce(m, kSize);
    const auto kept = std::count(expected.begin(), expected.end(), uint8_t{255});
    const auto foregroundCount = std::count(m.v.begin(), m.v.end(), uint8_t{1});
    EXPECT_GT(kept, 0);
    EXPECT_LT(kept, foregroundCount); // some objects are removed
    for (slideio::DataType type : {slideio::DataType::DT_Byte, slideio::DataType::DT_Float32}) {
        SCOPED_TRACE(static_cast<int>(type));
        auto scene = ts::makeSyntheticScene({{kWidth, kHeight, 0, 0}}, {type}, pattern);
        PipelineSpec spec;
        spec.then(makeRemoveLargeObjects(kSize));
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

TEST(RemoveLargeObjects, RefusesAContextTooLargeToLabel) {
#ifndef NDEBUG
    GTEST_SKIP() << "a debug BlockView reads the last byte of the view, which this test does not allocate";
#else
    // A context of 2^31 pixels: the check comes before any pixel is read, so
    // one byte backs the view (a release BlockView reads nothing).
    constexpr int64_t kWidth = int64_t(1) << 31;
    uint8_t sample = 255;
    uint8_t result = 0;
    const BlockPlane inPlane{&sample, DataType::Byte, kWidth, 0};
    const BlockPlane outPlane{&result, DataType::Byte, 1, 0};
    const BlockView input(kWidth, 1, Rect64{0, 0, kWidth, 1}, 0, &inPlane, 1);
    BlockView output(1, 1, Rect64{0, 0, 1, 1}, 0, &outPlane, 1);
    detail::OperationContextState state;
    state.input = &input;
    state.output = &output;
    state.tileRectAtLevel = state.tileRect = Rect64{0, 0, 1, 1};
    state.haloRectAtLevel = state.haloRect = Rect64{0, 0, kWidth, 1};
    state.tileIndex = 0;
    const OperationPtr op = makeRemoveLargeObjects(1);
    std::string message;
    try {
        op->apply(*detail::OperationContextAccess::make(std::move(state)));
    } catch (const InvalidArgumentError& e) {
        message = e.what();
    }
    EXPECT_NE(message.find("pixel.remove_large_objects"), std::string::npos) << message;
    EXPECT_NE(message.find("2147483648 pixels"), std::string::npos) << message;
    EXPECT_NE(message.find("smaller tile size"), std::string::npos) << message;
#endif
}
