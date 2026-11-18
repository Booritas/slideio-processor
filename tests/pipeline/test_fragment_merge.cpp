// Requirements: UR-SEG-3
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/pipeline/tile_grid.hpp>

#include <pipeline/detail/fragment_merge.hpp>
#include <pipeline/test_support/synthetic_scene.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace slideio::processor;
namespace ts = slideio::processor::testing_support;

namespace {

std::vector<Point64> ringOf(const Polygon& polygon, int32_t ring) {
    std::vector<Point64> points(static_cast<size_t>(polygon.ringPointCount(ring)));
    polygon.ringPoints(ring, points.data(), static_cast<int64_t>(points.size()));
    return points;
}

Polygon polygonOf(const std::vector<std::vector<Point64>>& rings) {
    Polygon polygon;
    for (size_t i = 0; i < rings.size(); ++i) {
        polygon.addRing(rings[i].data(), static_cast<int64_t>(rings[i].size()), i != 0);
    }
    return polygon;
}

// A rectangle as one ring, clockwise on screen (y down).
Polygon rectangle(int64_t x, int64_t y, int64_t w, int64_t h) {
    return polygonOf({{{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}}});
}

Polygon merge(const std::vector<const Polygon*>& fragments) { return detail::mergeFragments(fragments); }

std::shared_ptr<slideio::Scene> sceneOf(std::vector<ts::SyntheticLevel> levels) {
    return ts::makeSyntheticScene(std::move(levels), std::vector<slideio::DataType>{slideio::DataType::DT_Byte},
                                  [](int, int64_t, int64_t) { return 0.0; });
}

void expectRing(const Polygon& polygon, int32_t ring, const std::vector<Point64>& expected) {
    const std::vector<Point64> actual = ringOf(polygon, ring);
    ASSERT_EQ(actual.size(), expected.size()) << "ring " << ring;
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(actual[i].x, expected[i].x) << "ring " << ring << " vertex " << i;
        EXPECT_EQ(actual[i].y, expected[i].y) << "ring " << ring << " vertex " << i;
    }
}

} // namespace

TEST(FragmentMerge, TwoHalvesOfARectangle) {
    // The left half is given anticlockwise: merging orients every ring by its flag first.
    const Polygon left = polygonOf({{{0, 0}, {0, 4}, {6, 4}, {6, 0}}});
    const Polygon right = rectangle(6, 0, 4, 4);
    const Polygon merged = merge({&left, &right});
    ASSERT_EQ(merged.ringCount(), 1);
    expectRing(merged, 0, {{0, 0}, {10, 0}, {10, 4}, {0, 4}});
}

TEST(FragmentMerge, FourQuartersAroundACorner) {
    const Polygon a = rectangle(0, 0, 5, 5);
    const Polygon b = rectangle(5, 0, 5, 5);
    const Polygon c = rectangle(0, 5, 5, 5);
    const Polygon d = rectangle(5, 5, 5, 5);
    const Polygon merged = merge({&d, &b, &a, &c});
    ASSERT_EQ(merged.ringCount(), 1);
    expectRing(merged, 0, {{0, 0}, {10, 0}, {10, 10}, {0, 10}});
}

TEST(FragmentMerge, AnnulusAcrossFourSeamsHasAHole) {
    // A 12x12 square with a 4x4 hole at (4,4), cut by x = 6 and y = 6: four L-shapes.
    const Polygon tl = polygonOf({{{0, 0}, {6, 0}, {6, 4}, {4, 4}, {4, 6}, {0, 6}}});
    const Polygon tr = polygonOf({{{6, 0}, {12, 0}, {12, 6}, {8, 6}, {8, 4}, {6, 4}}});
    const Polygon bl = polygonOf({{{0, 6}, {4, 6}, {4, 8}, {6, 8}, {6, 12}, {0, 12}}});
    const Polygon br = polygonOf({{{8, 6}, {12, 6}, {12, 12}, {6, 12}, {6, 8}, {8, 8}}});
    const Polygon merged = merge({&tl, &tr, &bl, &br});
    ASSERT_EQ(merged.ringCount(), 2);
    EXPECT_FALSE(merged.ringIsHole(0));
    EXPECT_TRUE(merged.ringIsHole(1));
    expectRing(merged, 0, {{0, 0}, {12, 0}, {12, 12}, {0, 12}});
    expectRing(merged, 1, {{4, 4}, {4, 8}, {8, 8}, {8, 4}});

    const Polygon unsplit = polygonOf({{{0, 0}, {12, 0}, {12, 12}, {0, 12}}, {{4, 4}, {4, 8}, {8, 8}, {8, 4}}});
    EXPECT_EQ(merged.area(), unsplit.area());
    EXPECT_EQ(merged.area(), 128.0);
}

TEST(FragmentMerge, UShapeJoinsThroughTwoSeamsIntoOneRing) {
    // Arms [1,3] and [7,9] from y = 0 to 8 on a base y in [6,8], cut by x = 5 and y = 5:
    // the arms' tops are separate fragments that join only through the bottom row.
    const Polygon topLeft = rectangle(1, 0, 2, 5);
    const Polygon topRight = rectangle(7, 0, 2, 5);
    const Polygon bottomLeft = polygonOf({{{1, 5}, {3, 5}, {3, 6}, {5, 6}, {5, 8}, {1, 8}}});
    const Polygon bottomRight = polygonOf({{{7, 5}, {9, 5}, {9, 8}, {5, 8}, {5, 6}, {7, 6}}});

    // In a 2x2 grid of 5x5 tiles each arm top touches the bottom row through y = 5.
    const std::shared_ptr<slideio::Scene> scene = sceneOf({{10, 10, 0, 0}});
    const LevelSelector selector(scene);
    const TileGrid grid(Rect64{0, 0, 10, 10}, 0, 5, 5, selector);
    const std::vector<detail::SeamContact> tl = detail::seamContacts(topLeft, grid, 0);
    const std::vector<detail::SeamContact> bl = detail::seamContacts(bottomLeft, grid, 2);
    ASSERT_EQ(tl.size(), 1u);
    EXPECT_FALSE(tl[0].vertical);
    EXPECT_EQ(tl[0].line, 5);
    EXPECT_EQ(tl[0].begin, 1);
    EXPECT_EQ(tl[0].end, 3);
    bool joined = false;
    for (const detail::SeamContact& c : bl) {
        joined = joined || detail::contactsJoin(tl[0], c);
    }
    EXPECT_TRUE(joined);
    EXPECT_FALSE(detail::contactsJoin(tl[0], detail::seamContacts(topRight, grid, 1).front()));

    const Polygon merged = merge({&topRight, &bottomLeft, &topLeft, &bottomRight});
    ASSERT_EQ(merged.ringCount(), 1);
    expectRing(merged, 0, {{1, 0}, {3, 0}, {3, 6}, {7, 6}, {7, 0}, {9, 0}, {9, 8}, {1, 8}});
}

TEST(FragmentMerge, SelfTouchingObjectKeepsItsPinch) {
    // A 4x4 block missing pixels (0,0) and (1,1), cut at x = 2: the missing pixel
    // (1,1) meets the background diagonally at vertex (1,1), so the boundary runs
    // into it through that vertex, turning right there as ComponentsOperation does.
    const Polygon nub = rectangle(1, 0, 1, 1);
    const Polygon ell = polygonOf({{{0, 1}, {1, 1}, {1, 2}, {2, 2}, {2, 4}, {0, 4}}});
    const Polygon right = rectangle(2, 0, 2, 4);
    const Polygon merged = merge({&right, &ell, &nub});
    ASSERT_EQ(merged.ringCount(), 1);
    expectRing(merged, 0, {{1, 0}, {4, 0}, {4, 4}, {0, 4}, {0, 1}, {1, 1}, {1, 2}, {2, 2}, {2, 1}, {1, 1}});
    EXPECT_EQ(merged.area(), 14.0);
}

TEST(FragmentMerge, CornerTouchDoesNotJoin) {
    EXPECT_FALSE(detail::contactsJoin({true, 5, 0, 3}, {true, 5, 3, 6}));
    EXPECT_TRUE(detail::contactsJoin({true, 5, 0, 3}, {true, 5, 2, 6}));
    EXPECT_FALSE(detail::contactsJoin({true, 5, 0, 3}, {true, 6, 0, 3}));
    EXPECT_FALSE(detail::contactsJoin({true, 5, 0, 3}, {false, 5, 0, 3}));

    // Two squares meeting diagonally at (5,5) in a 2x2 grid of 5x5 tiles.
    const std::shared_ptr<slideio::Scene> scene = sceneOf({{10, 10, 0, 0}});
    const LevelSelector selector(scene);
    const TileGrid grid(Rect64{0, 0, 10, 10}, 0, 5, 5, selector);
    const Polygon a = rectangle(0, 0, 5, 5);
    const Polygon d = rectangle(5, 5, 5, 5);
    for (const detail::SeamContact& ca : detail::seamContacts(a, grid, 0)) {
        for (const detail::SeamContact& cd : detail::seamContacts(d, grid, 3)) {
            EXPECT_FALSE(detail::contactsJoin(ca, cd));
        }
    }
    // Merged anyway, they are two objects: more than one outer ring.
    EXPECT_THROW(merge({&a, &d}), InternalError);
}

TEST(FragmentMerge, SeamContactsLieOnInteriorTileEdges) {
    const std::shared_ptr<slideio::Scene> scene = sceneOf({{10, 10, 0, 0}});
    const LevelSelector selector(scene);
    const TileGrid grid(Rect64{0, 0, 10, 10}, 0, 5, 5, selector);
    // Filling tile 0: the region's outer boundary is not a seam; collinear
    // vertices on the seam give one maximal contact.
    const Polygon whole = polygonOf({{{0, 0}, {5, 0}, {5, 2}, {5, 5}, {3, 5}, {0, 5}}});
    const std::vector<detail::SeamContact> contacts = detail::seamContacts(whole, grid, 0);
    ASSERT_EQ(contacts.size(), 2u);
    EXPECT_TRUE(contacts[0].vertical);
    EXPECT_EQ(contacts[0].line, 5);
    EXPECT_EQ(contacts[0].begin, 0);
    EXPECT_EQ(contacts[0].end, 5);
    EXPECT_FALSE(contacts[1].vertical);
    EXPECT_EQ(contacts[1].line, 5);
    EXPECT_EQ(contacts[1].begin, 0);
    EXPECT_EQ(contacts[1].end, 5);
    // Inside the tile, away from its edges: no contact.
    EXPECT_TRUE(detail::seamContacts(rectangle(1, 1, 2, 2), grid, 0).empty());
}

TEST(FragmentMerge, OutputIsNormalised) {
    // Each half has a hole; the left one starts mid-edge and has a collinear vertex.
    const Polygon left = polygonOf({{{6, 10}, {0, 10}, {0, 5}, {0, 0}, {6, 0}}, {{1, 6}, {1, 8}, {3, 8}, {3, 6}}});
    const Polygon right = polygonOf({{{6, 0}, {12, 0}, {12, 10}, {6, 10}}, {{8, 1}, {8, 3}, {10, 3}, {10, 1}}});
    const Polygon merged = merge({&left, &right});
    ASSERT_EQ(merged.ringCount(), 3);
    expectRing(merged, 0, {{0, 0}, {12, 0}, {12, 10}, {0, 10}});
    // Holes ordered by first vertex, y then x.
    expectRing(merged, 1, {{8, 1}, {8, 3}, {10, 3}, {10, 1}});
    expectRing(merged, 2, {{1, 6}, {1, 8}, {3, 8}, {3, 6}});
    for (int32_t r = 0; r < merged.ringCount(); ++r) {
        const std::vector<Point64> ring = ringOf(merged, r);
        const size_t n = ring.size();
        for (size_t i = 0; i < n; ++i) {
            const Point64& p = ring[(i + n - 1) % n];
            const Point64& c = ring[i];
            const Point64& q = ring[(i + 1) % n];
            EXPECT_NE((c.x - p.x) * (q.y - c.y) - (c.y - p.y) * (q.x - c.x), 0) << "ring " << r << " vertex " << i;
            EXPECT_TRUE(ring[0].y < c.y || (ring[0].y == c.y && ring[0].x <= c.x));
        }
    }

    // normaliseRings on its own: rings normalised, holes reordered.
    Polygon raw = polygonOf({{{5, 5}, {0, 5}, {0, 0}, {3, 0}, {5, 0}}, {{3, 3}, {4, 3}, {4, 4}, {3, 4}},
                             {{1, 1}, {2, 1}, {2, 2}, {1, 2}}});
    detail::normaliseRings(raw);
    ASSERT_EQ(raw.ringCount(), 3);
    expectRing(raw, 0, {{0, 0}, {5, 0}, {5, 5}, {0, 5}});
    expectRing(raw, 1, {{1, 1}, {2, 1}, {2, 2}, {1, 2}});
    expectRing(raw, 2, {{3, 3}, {4, 3}, {4, 4}, {3, 4}});
}

TEST(FragmentMerge, OpenEdgesAreInternalError) {
    // A gap of one pixel: nothing cancels, two outer rings.
    const Polygon left = rectangle(0, 0, 6, 4);
    const Polygon gapped = rectangle(7, 0, 3, 4);
    EXPECT_THROW(merge({&left, &gapped}), InternalError);
    // Overlapping by one pixel: the shared top and bottom stretches run twice.
    const Polygon overlapping = rectangle(5, 0, 5, 4);
    EXPECT_THROW(merge({&left, &overlapping}), InternalError);
    EXPECT_THROW(merge({}), InternalError);
}

TEST(FragmentMerge, ErrorsCarryCountsNotCoordinates) {
    // Distinctive coordinates, so any of them leaking into the message is visible.
    const Polygon left = rectangle(91357, 24683, 6, 4);
    const Polygon gapped = rectangle(91364, 24683, 3, 4);
    try {
        merge({&left, &gapped});
        FAIL() << "expected InternalError";
    } catch (const InternalError& e) {
        const std::string what = e.what();
        EXPECT_NE(what.find("2 outer rings"), std::string::npos) << what;
        EXPECT_NE(what.find("2 fragments, 2 rings"), std::string::npos) << what;
        for (const char* coordinate : {"91357", "91363", "91364", "91367", "24683", "24687"}) {
            EXPECT_EQ(what.find(coordinate), std::string::npos) << what;
        }
    }
}

TEST(FragmentMerge, NonDyadicSeamsCoincideExactly) {
    // Level 1 of an 8001-wide pyramid with a 2000-wide level 1: scale 4.0005.
    const std::shared_ptr<slideio::Scene> scene = sceneOf({{8001, 6001, 0, 0}, {2000, 1500, 0, 0}});
    const LevelSelector selector(scene);
    const TileGrid grid(Rect64{0, 0, 2000, 1500}, 1, 301, 301, selector);
    // The seam between columns 0 and 1 is level x = 301: 301 * 4.0005 = 1204.15.
    const int64_t seam = grid.tileRect(0).right();
    ASSERT_EQ(seam, grid.tileRect(1).x);
    ASSERT_EQ(seam, 1204);

    // Level pixels [290, 301) and [301, 312) of rows [10, 20), mapped to level 0 the
    // way an operation maps its rings.
    const auto levelRect = [](int64_t x, int64_t y, int64_t w, int64_t h) { return rectangle(x, y, w, h); };
    const Polygon left = selector.mapToLevel0(levelRect(290, 10, 11, 10), 1);
    const Polygon right = selector.mapToLevel0(levelRect(301, 10, 11, 10), 1);

    const std::vector<detail::SeamContact> lc = detail::seamContacts(left, grid, 0);
    const std::vector<detail::SeamContact> rc = detail::seamContacts(right, grid, 1);
    ASSERT_EQ(lc.size(), 1u);
    ASSERT_EQ(rc.size(), 1u);
    EXPECT_EQ(lc[0].line, seam);
    EXPECT_TRUE(detail::contactsJoin(lc[0], rc[0]));

    const Polygon merged = merge({&left, &right});
    const Polygon whole = selector.mapToLevel0(levelRect(290, 10, 22, 10), 1);
    ASSERT_EQ(merged.ringCount(), 1);
    expectRing(merged, 0, ringOf(whole, 0));
}
