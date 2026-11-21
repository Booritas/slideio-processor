// Requirements: UR-IMG-4, UR-EXE-6, UR-EXT-3, UR-SEG-3, UR-QA-2, UR-QA-3, UR-IMG-5, UR-IMG-1
#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/pipeline/tile_grid.hpp>

#include <core/detail/level_mapping.hpp>
#include <pipeline/detail/level_table.hpp>
#include <pipeline/test_support/synthetic_scene.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

using namespace slideio::processor;
namespace ts = slideio::processor::testing_support;

namespace {

std::shared_ptr<slideio::Scene> sceneOf(std::vector<ts::SyntheticLevel> levels) {
    return ts::makeSyntheticScene(std::move(levels), std::vector<slideio::DataType>{slideio::DataType::DT_Byte},
                                  [](int, int64_t, int64_t) { return 0.0; });
}

// Factor 8001 / 2000 = 4.0005: a level-0 tile boundary lands part-way through a level pixel.
std::shared_ptr<slideio::Scene> nonDyadic() {
    return sceneOf({{8001, 6001, 0, 0}, {2000, 1500, 0, 0}, {500, 375, 0, 0}});
}

std::shared_ptr<slideio::Scene> dyadic() {
    return sceneOf({{4096, 4096, 0, 0}, {1024, 1024, 0, 0}, {256, 256, 0, 0}});
}

void expectRect(const Rect64& r, int64_t x, int64_t y, int64_t w, int64_t h) {
    EXPECT_EQ(r.x, x);
    EXPECT_EQ(r.y, y);
    EXPECT_EQ(r.width, w);
    EXPECT_EQ(r.height, h);
}

bool sameRect(const Rect64& a, const Rect64& b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

} // namespace

TEST(LevelSelector, ScaleOfANonDyadicPyramid) {
    const LevelSelector s(nonDyadic());
    EXPECT_EQ(s.scaleOfLevel(0), 1.0);
    EXPECT_EQ(s.scaleOfLevel(1), 8001.0 / 2000);
    EXPECT_EQ(s.scaleOfLevel(2), 8001.0 / 500);
}

TEST(LevelSelector, LevelForScaleNeverDecodesLevelZeroForACoarseRequest) {
    const LevelSelector s(dyadic());
    EXPECT_EQ(s.levelForScale(1.0 / 16), 2);
    EXPECT_EQ(s.levelForScale(1.0 / 64), 2);
    EXPECT_EQ(s.levelForScale(0.25), 1);
    EXPECT_EQ(s.levelForScale(0.3), 0);
    EXPECT_EQ(s.levelForScale(1.0), 0);
    // A fraction a hair above 1/4 from rounding elsewhere still selects level 1.
    EXPECT_EQ(s.levelForScale(0.25 * (1 + 1e-12)), 1);
}

TEST(LevelSelector, LevelForSize) {
    const LevelSelector s(dyadic());
    EXPECT_EQ(s.levelForSize(256, 256), 2);
    EXPECT_EQ(s.levelForSize(300, 10), 1);
    EXPECT_EQ(s.levelForSize(5000, 1), 0);
}

TEST(LevelSelector, MapToLevelRoundsOutward) {
    const LevelSelector s(nonDyadic());
    // Level-0 pixels 1..4 fall in level pixels 0 and 1: x0 = floor(1 / 4.0005), x1 = ceil(5 / 4.0005).
    expectRect(s.mapToLevel(Rect64{1, 1, 4, 4}, 1), 0, 0, 2, 2);
    expectRect(s.mapToLevel(Rect64{1, 1, 4, 4}, 0), 1, 1, 4, 4);
    // Every level-0 pixel of the input is covered after mapping back.
    const Rect64 roi{1237, 911, 3001, 77};
    const Rect64 back = s.mapToLevel0(s.mapToLevel(roi, 1), 1);
    EXPECT_LE(back.x, roi.x);
    EXPECT_LE(back.y, roi.y);
    EXPECT_GE(back.right(), roi.right());
    EXPECT_GE(back.bottom(), roi.bottom());
}

TEST(LevelSelector, MapToLevel0IsTheSingleMapping) {
    const LevelSelector s(nonDyadic());
    const double scale = 8001.0 / 2000;
    const Rect64 r{300, 200, 300, 200};
    EXPECT_TRUE(sameRect(s.mapToLevel0(r, 1), detail::rectToLevel0(r, scale)));
    // 300 * 4.0005 = 1200.15 and 600 * 4.0005 = 2400.3: the edges map, then the width follows.
    expectRect(s.mapToLevel0(r, 1), 1200, 800, 1200, 800);

    const Point64 p = s.mapToLevel0(Point64{7, 3}, 1);
    EXPECT_EQ(p.x, detail::levelToLevel0(7, scale));
    EXPECT_EQ(p.y, detail::levelToLevel0(3, scale));

    const Point64 outer[] = {{0, 0}, {10, 0}, {10, 10}, {0, 10}};
    const Point64 hole[] = {{2, 2}, {2, 5}, {5, 5}, {5, 2}};
    Polygon poly;
    poly.addRing(outer, 4, false);
    poly.addRing(hole, 4, true);
    const Polygon mapped = s.mapToLevel0(poly, 1);
    ASSERT_EQ(mapped.ringCount(), 2);
    EXPECT_FALSE(mapped.ringIsHole(0));
    EXPECT_TRUE(mapped.ringIsHole(1));
    ASSERT_EQ(mapped.ringPointCount(1), 4);
    Point64 got[4];
    mapped.ringPoints(1, got, 4);
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(got[i].x, detail::levelToLevel0(hole[i].x, scale));
        EXPECT_EQ(got[i].y, detail::levelToLevel0(hole[i].y, scale));
    }
}

TEST(LevelSelector, SceneWithoutLevelsHasOne) {
    // makeScene refuses metadata without levels, so the rule is pinned where the
    // selector reads the scene: zero reported levels become one, the scene rect.
    const std::vector<detail::LevelDims> table =
        detail::levelTable(0, 640, 480, [](int32_t) -> detail::LevelDims {
            ADD_FAILURE() << "no level may be queried when none is reported";
            return detail::LevelDims{0, 0, 0, 0};
        });
    ASSERT_EQ(table.size(), 1u);
    EXPECT_EQ(table[0].width, 640);
    EXPECT_EQ(table[0].height, 480);
    EXPECT_EQ(table[0].tileWidth, 0);
    EXPECT_EQ(table[0].tileHeight, 0);
    EXPECT_EQ(detail::scaleOfLevel(table, 0), 1.0);
}

TEST(LevelSelector, Validation) {
    EXPECT_THROW(LevelSelector(nullptr), InvalidArgumentError);
    const LevelSelector s(dyadic());
    EXPECT_THROW(s.scaleOfLevel(3), InvalidArgumentError);
    EXPECT_THROW(s.scaleOfLevel(-1), InvalidArgumentError);
    EXPECT_THROW(s.mapToLevel(Rect64{0, 0, 1, 1}, 3), InvalidArgumentError);
    EXPECT_THROW(s.mapToLevel0(Rect64{0, 0, 1, 1}, 3), InvalidArgumentError);
    EXPECT_THROW(s.mapToLevel0(Polygon(), -1), InvalidArgumentError);
    EXPECT_THROW(s.levelForScale(0.0), InvalidArgumentError);
    EXPECT_THROW(s.levelForScale(-0.5), InvalidArgumentError);
    EXPECT_THROW(s.levelForScale(std::numeric_limits<double>::quiet_NaN()), InvalidArgumentError);
    EXPECT_THROW(s.levelForScale(std::numeric_limits<double>::infinity()), InvalidArgumentError);
    EXPECT_THROW(s.levelForSize(0, 10), InvalidArgumentError);
    EXPECT_THROW(s.levelForSize(10, -1), InvalidArgumentError);
    // x + width past INT64_MAX is reported, not wrapped.
    const int64_t big = std::numeric_limits<int64_t>::max();
    EXPECT_NO_THROW(s.mapToLevel(Rect64{0, 0, big / 2, 1}, 2));
    EXPECT_THROW(s.mapToLevel(Rect64{big, 0, big, 1}, 0), InvalidArgumentError);
    EXPECT_THROW(s.mapToLevel(Rect64{0, big - 1, 1, 1}, 0), InvalidArgumentError);
    EXPECT_THROW(s.mapToLevel(Rect64{0, 0, -1, 1}, 1), InvalidArgumentError);
    EXPECT_THROW(s.mapToLevel(Rect64{0, 0, 1, -1}, 1), InvalidArgumentError);
}

TEST(LevelSelector, MapToLevelInvertsTheSingleMappingOnEveryPlatform) {
    // s = 100000 / 33333 rounds below the true ratio, so 100000 / s is a hair above
    // 33333: ceil gives 33334 in exact or extended precision and 33333 in double,
    // which made the old long-double formula differ between MSVC and Linux. The
    // inverse is now defined through llround(L * s), which maps 33333 to exactly
    // 100000 (33333 * s is within 4e-12 of it), so the full width maps to 33333
    // everywhere, and the round trip lands on the level-0 extent itself.
    const LevelSelector s(sceneOf({{100000, 100000, 0, 0}, {33333, 33333, 0, 0}}));
    const double scale = 100000.0 / 33333;
    ASSERT_EQ(s.scaleOfLevel(1), scale);
    ASSERT_EQ(detail::levelToLevel0(33333, scale), 100000);
    ASSERT_EQ(detail::levelToLevel0(33332, scale), 99997);
    expectRect(s.mapToLevel(Rect64{0, 0, 100000, 100000}, 1), 0, 0, 33333, 33333);
    expectRect(s.mapToLevel0(s.mapToLevel(Rect64{0, 0, 100000, 100000}, 1), 1), 0, 0, 100000, 100000);
    // An interior edge: 99998 lies in level pixel 33332 = [99997, 100000).
    expectRect(s.mapToLevel(Rect64{99998, 99997, 1, 3}, 1), 33332, 33332, 1, 1);

    // The defining property, ordinate by ordinate: x0 is the last level pixel
    // starting at or before x, x1 the first one starting at or after x + width.
    for (int64_t x = 99000; x < 100000; x += 7) {
        for (int64_t width : {int64_t{0}, int64_t{1}, int64_t{2}, int64_t{3}, int64_t{10}}) {
            const Rect64 m = s.mapToLevel(Rect64{x, 0, width, 1}, 1);
            const int64_t x1 = m.x + m.width;
            EXPECT_LE(detail::levelToLevel0(m.x, scale), x);
            EXPECT_GT(detail::levelToLevel0(m.x + 1, scale), x);
            EXPECT_GE(detail::levelToLevel0(x1, scale), x + width);
            if (width > 0) {
                EXPECT_LT(detail::levelToLevel0(x1 - 1, scale), x + width);
            }
        }
    }
}

TEST(LevelSelector, CopiesAreIndependentValues) {
    LevelSelector a(nonDyadic());
    const LevelSelector b(a);
    LevelSelector c(dyadic());
    c = b;
    EXPECT_EQ(c.scaleOfLevel(1), 8001.0 / 2000);
    LevelSelector d(std::move(a));
    EXPECT_EQ(d.scaleOfLevel(2), 8001.0 / 500);
}

TEST(TileGrid, AdjacentTilesShareLevel0EdgesExactly) {
    const LevelSelector s(nonDyadic());
    const Rect64 region{0, 0, 2000, 1500};
    const TileGrid g(region, 1, 300, 200, s);
    EXPECT_EQ(g.level(), 1);
    ASSERT_EQ(g.columns(), 7);
    ASSERT_EQ(g.rows(), 8);
    ASSERT_EQ(g.tileCount(), 56);

    int64_t area = 0;
    for (int64_t r = 0; r < g.rows(); ++r) {
        for (int64_t c = 0; c < g.columns(); ++c) {
            const Rect64 t = g.tileRect(g.tileIndex(c, r));
            EXPECT_TRUE(sameRect(t, detail::rectToLevel0(g.tileRectAtLevel(g.tileIndex(c, r)), 8001.0 / 2000)));
            area += t.area();
            if (c + 1 < g.columns()) {
                EXPECT_EQ(t.right(), g.tileRect(g.tileIndex(c + 1, r)).x) << c << "," << r;
            }
            if (r + 1 < g.rows()) {
                EXPECT_EQ(t.bottom(), g.tileRect(g.tileIndex(c, r + 1)).y) << c << "," << r;
            }
        }
    }
    const Rect64 whole = s.mapToLevel0(region, 1);
    expectRect(whole, 0, 0, 8001, 6001);
    EXPECT_EQ(area, whole.area());
    // Column 1 spans level 300..600, which is 1200.15..2400.3 at level 0.
    expectRect(g.tileRect(1), 1200, 0, 1200, 800);
    EXPECT_EQ(g.tileRect(g.tileCount() - 1).right(), 8001);
    EXPECT_EQ(g.tileRect(g.tileCount() - 1).bottom(), 6001);
}

TEST(TileGrid, RegionOffsetIsTheOrigin) {
    const LevelSelector s(dyadic());
    const TileGrid g(Rect64{100, 50, 300, 300}, 1, 128, 128, s);
    EXPECT_EQ(g.columns(), 3);
    EXPECT_EQ(g.rows(), 3);
    expectRect(g.tileRectAtLevel(0), 100, 50, 128, 128);
    expectRect(g.tileRectAtLevel(g.tileIndex(2, 1)), 356, 178, 44, 128);
    expectRect(g.tileRect(0), 400, 200, 512, 512);
}

TEST(TileGrid, EdgeTilesAreClipped) {
    const LevelSelector s(dyadic());
    const TileGrid g(Rect64{0, 0, 1000, 700}, 0, 512, 512, s);
    EXPECT_EQ(g.columns(), 2);
    EXPECT_EQ(g.rows(), 2);
    EXPECT_EQ(g.tileCount(), 4);
    expectRect(g.tileRectAtLevel(0), 0, 0, 512, 512);
    expectRect(g.tileRectAtLevel(3), 512, 512, 488, 188);
    EXPECT_EQ(g.tileIndex(1, 1), 3);
}

TEST(TileGrid, IsEdgeTile) {
    const LevelSelector s(dyadic());
    const TileGrid g(Rect64{0, 0, 300, 300}, 0, 100, 100, s);
    for (int64_t i = 0; i < g.tileCount(); ++i) {
        EXPECT_EQ(g.isEdgeTile(i), i != 4) << i;
    }
    const TileGrid one(Rect64{0, 0, 10, 10}, 0, 100, 100, s);
    EXPECT_TRUE(one.isEdgeTile(0));
}

TEST(TileGrid, HaloIsNotClipped) {
    const LevelSelector s(nonDyadic());
    const TileGrid g(Rect64{0, 0, 1000, 700}, 0, 512, 512, s);
    expectRect(g.tileRectAtLevelWithHalo(0, Halo::uniform(5)), -5, -5, 522, 522);
    expectRect(g.tileRectAtLevelWithHalo(3, Halo{1, 2, 3, 4}), 511, 510, 492, 194);
    expectRect(g.tileRectAtLevelWithHalo(3, Halo::none()), 512, 512, 488, 188);

    const TileGrid g1(Rect64{0, 0, 2000, 1500}, 1, 300, 200, s);
    const Rect64 withHalo = g1.tileRectAtLevelWithHalo(1, Halo::uniform(5));
    expectRect(withHalo, 295, -5, 310, 210);
    EXPECT_TRUE(sameRect(g1.tileRectWithHalo(1, Halo::uniform(5)), detail::rectToLevel0(withHalo, 8001.0 / 2000)));
    EXPECT_THROW(g1.tileRectAtLevelWithHalo(0, Halo{-1, 0, 0, 0}), InvalidArgumentError);
}

TEST(TileGrid, Int64Scale) {
    const LevelSelector s(sceneOf({{4000000, 3000000, 0, 0}}));
    const TileGrid g(Rect64{0, 0, 4000000, 3000000}, 0, 512, 512, s);
    EXPECT_EQ(g.columns(), 7813);
    EXPECT_EQ(g.rows(), 5860);
    EXPECT_EQ(g.tileCount(), int64_t{7813} * 5860);
    const int64_t last = g.tileCount() - 1;
    EXPECT_EQ(g.tileIndex(7812, 5859), last);
    expectRect(g.tileRectAtLevel(last), int64_t{7812} * 512, int64_t{5859} * 512, 256, 192);
    expectRect(g.tileRect(last), int64_t{7812} * 512, int64_t{5859} * 512, 256, 192);
    EXPECT_TRUE(g.isEdgeTile(last));

    // A region far beyond 32-bit coordinates lays out without overflow.
    const TileGrid huge(Rect64{0, 0, int64_t{1} << 40, int64_t{1} << 20}, 0, 1, 1, s);
    EXPECT_EQ(huge.columns(), int64_t{1} << 40);
    EXPECT_EQ(huge.tileCount(), int64_t{1} << 60);
    expectRect(huge.tileRectAtLevel(huge.tileIndex((int64_t{1} << 40) - 1, 3)), (int64_t{1} << 40) - 1, 3, 1, 1);
    // A grid whose tile count would not fit int64 is refused rather than wrapped.
    EXPECT_THROW(TileGrid(Rect64{0, 0, int64_t{1} << 40, int64_t{1} << 40}, 0, 1, 1, s), InvalidArgumentError);
}

TEST(TileGrid, Validation) {
    const LevelSelector s(dyadic());
    const Rect64 region{0, 0, 1000, 700};
    EXPECT_THROW(TileGrid(region, 0, 0, 512, s), InvalidArgumentError);
    EXPECT_THROW(TileGrid(region, 0, 512, 0, s), InvalidArgumentError);
    EXPECT_THROW(TileGrid(region, 0, -4, 512, s), InvalidArgumentError);
    EXPECT_THROW(TileGrid(region, 3, 512, 512, s), InvalidArgumentError);
    EXPECT_THROW(TileGrid(region, -1, 512, 512, s), InvalidArgumentError);

    const TileGrid g(region, 0, 512, 512, s);
    EXPECT_THROW(g.tileRectAtLevel(-1), InvalidArgumentError);
    EXPECT_THROW(g.tileRectAtLevel(4), InvalidArgumentError);
    EXPECT_THROW(g.tileRect(4), InvalidArgumentError);
    EXPECT_THROW(g.tileRectAtLevelWithHalo(4, Halo::none()), InvalidArgumentError);
    EXPECT_THROW(g.tileRectWithHalo(-1, Halo::none()), InvalidArgumentError);
    EXPECT_THROW(g.isEdgeTile(4), InvalidArgumentError);
    EXPECT_THROW(g.tileIndex(2, 0), InvalidArgumentError);
    EXPECT_THROW(g.tileIndex(0, 2), InvalidArgumentError);
    EXPECT_THROW(g.tileIndex(-1, 0), InvalidArgumentError);

    const TileGrid empty(Rect64{0, 0, 0, 700}, 0, 512, 512, s);
    EXPECT_EQ(empty.tileCount(), 0);
    EXPECT_THROW(empty.tileRectAtLevel(0), InvalidArgumentError);
    const TileGrid negative(Rect64{10, 10, -5, 700}, 0, 512, 512, s);
    EXPECT_EQ(negative.tileCount(), 0);
}

TEST(TileGrid, CopiesAreIndependentValues) {
    const LevelSelector s(dyadic());
    TileGrid a(Rect64{0, 0, 1000, 700}, 0, 512, 512, s);
    TileGrid b(Rect64{0, 0, 10, 10}, 1, 5, 5, s);
    b = a;
    EXPECT_EQ(b.tileCount(), 4);
    EXPECT_EQ(b.level(), 0);
    const TileGrid c(std::move(a));
    expectRect(c.tileRectAtLevel(3), 512, 512, 488, 188);
}

TEST(DefaultTileSize, NativeOrConfigured) {
    const auto scene = sceneOf({{4096, 4096, 256, 256}, {1024, 1024, 0, 0}, {256, 256, 128, 0}});
    const Configuration native = ConfigurationBuilder().tileSize(1000, 700).tileSizeFromSlide(true).build();
    const Configuration configured = ConfigurationBuilder().tileSize(1000, 700).tileSizeFromSlide(false).build();
    int32_t w = 0;
    int32_t h = 0;

    defaultTileSize(scene, 0, native, w, h);
    EXPECT_EQ(w, 256);
    EXPECT_EQ(h, 256);

    defaultTileSize(scene, 0, configured, w, h);
    EXPECT_EQ(w, 1000);
    EXPECT_EQ(h, 700);

    // No native tiling on level 1, and only half of one on level 2.
    defaultTileSize(scene, 1, native, w, h);
    EXPECT_EQ(w, 1000);
    EXPECT_EQ(h, 700);
    defaultTileSize(scene, 2, native, w, h);
    EXPECT_EQ(w, 1000);
    EXPECT_EQ(h, 700);

    EXPECT_THROW(defaultTileSize(scene, 3, native, w, h), InvalidArgumentError);
    EXPECT_THROW(defaultTileSize(scene, -1, native, w, h), InvalidArgumentError);
    EXPECT_THROW(defaultTileSize(nullptr, 0, native, w, h), InvalidArgumentError);
}
