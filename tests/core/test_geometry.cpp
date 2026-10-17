// Requirements: UR-IMG-2, UR-SEG-4, UR-SEG-5
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/geometry.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <utility>
#include <vector>

using namespace slideio::processor;

namespace {

Polygon square(int64_t x, int64_t y, int64_t s, bool hole = false, Polygon p = {}) {
    Point64 v[] = {{x, y}, {x + s, y}, {x + s, y + s}, {x, y + s}};
    p.addRing(v, 4, hole);
    return p;
}

} // namespace

TEST(Rect64, HalfOpenAlgebra) {
    Rect64 a{0, 0, 10, 10}, b{10, 0, 5, 5};
    EXPECT_FALSE(a.intersects(b));
    EXPECT_TRUE(a.contains({9, 9}));
    EXPECT_FALSE(a.contains({10, 0}));
    EXPECT_EQ(a.intersected(Rect64{5, 5, 10, 10}), (Rect64{5, 5, 5, 5}));
    EXPECT_EQ(a.united(b), (Rect64{0, 0, 15, 10}));
    EXPECT_EQ(a.united(Rect64{}), a);
    EXPECT_EQ((Rect64{0, 0, 100000, 80000}).area(), 8000000000LL);
}

TEST(Rect64, EmptyAndDisjointEdgeCases) {
    EXPECT_TRUE((Rect64{0, 0, 0, 5}).isEmpty());
    EXPECT_TRUE((Rect64{0, 0, 5, -1}).isEmpty());
    EXPECT_FALSE((Rect64{0, 0, 1, 1}).isEmpty());
    EXPECT_FALSE((Rect64{0, 0, 10, 10}).intersects(Rect64{2, 2, 0, 0}));
    EXPECT_EQ((Rect64{0, 0, 10, 10}).intersected(Rect64{20, 20, 5, 5}), (Rect64{0, 0, 0, 0}));
    EXPECT_EQ(Rect64{}.united(Rect64{}), (Rect64{0, 0, 0, 0}));
    EXPECT_EQ(Rect64{}.united(Rect64{3, 4, 5, 6}), (Rect64{3, 4, 5, 6}));
    EXPECT_EQ((Rect64{1, 2, 3, 4}).right(), 4);
    EXPECT_EQ((Rect64{1, 2, 3, 4}).bottom(), 6);
}

TEST(Polygon, RingsAndBuffers) {
    Point64 v[] = {{0, 0}, {4, 0}, {4, 4}, {0, 4}, {0, 0}};
    Polygon p;
    p.addRing(v, 5, false);
    EXPECT_EQ(p.ringPointCount(0), 4); // closing point dropped
    Point64 buf[2];
    EXPECT_EQ(p.ringPoints(0, buf, 2), 4);
    EXPECT_EQ(buf[1], (Point64{4, 0}));
    EXPECT_EQ(p.ringPoints(0, nullptr, 0), 4);
    EXPECT_THROW(p.ringIsHole(1), InvalidArgumentError);
    EXPECT_THROW(p.ringIsHole(-1), InvalidArgumentError);
    EXPECT_THROW(p.ringPointCount(1), InvalidArgumentError);
    EXPECT_THROW(p.ringPoints(1, nullptr, 0), InvalidArgumentError);
    Polygon q;
    EXPECT_THROW(q.addRing(v, 4, true), InvalidArgumentError);
    EXPECT_THROW(q.addRing(v, 2, false), InvalidArgumentError);
    EXPECT_THROW(q.addRing(nullptr, 4, false), InvalidArgumentError);
    Point64 closedTwo[] = {{0, 0}, {1, 1}, {0, 0}};
    EXPECT_THROW(q.addRing(closedTwo, 3, false), InvalidArgumentError);
    EXPECT_TRUE(q.isEmpty());
}

TEST(Polygon, ClearEmptiesThePolygon) {
    Polygon p = square(0, 0, 3);
    EXPECT_FALSE(p.isEmpty());
    p.clear();
    EXPECT_TRUE(p.isEmpty());
    EXPECT_EQ(p.ringCount(), 0);
}

TEST(Polygon, AreaWithHolesAndTwoOuters) {
    Polygon p = square(0, 0, 10);
    p = square(2, 2, 2, true, p);
    p = square(20, 0, 5, false, p);
    EXPECT_DOUBLE_EQ(p.area(), 100 - 4 + 25);
    EXPECT_DOUBLE_EQ(p.perimeter(), 40 + 8 + 20);
    EXPECT_EQ(p.boundingBox(), (Rect64{0, 0, 25, 10}));
}

TEST(Polygon, EvenOddMembershipIgnoresWhichOuterAHoleSitsIn) {
    Polygon p = square(0, 0, 10);
    p = square(20, 0, 10, false, p);
    p = square(22, 2, 2, true, p);
    EXPECT_TRUE(p.contains({5, 5}));
    EXPECT_FALSE(p.contains({22, 2}));
    EXPECT_TRUE(p.contains({25, 5}));
    EXPECT_FALSE(p.contains({15, 5}));
    EXPECT_TRUE(square(0, 0, 1).contains({0, 0}));
    EXPECT_FALSE(square(0, 0, 1).contains({1, 0}));
}

TEST(Polygon, AnchorIsTopmostThenLeftmost) {
    Point64 v[] = {{5, 2}, {9, 2}, {7, 0}, {3, 0}, {1, 4}};
    Polygon p;
    p.addRing(v, 5, false);
    EXPECT_EQ(p.anchor(), (Point64{3, 0}));
}

TEST(Polygon, CentroidAndDegenerate) {
    EXPECT_EQ(square(10, 10, 4).centroid(), (Point64{12, 12}));
    Point64 line[] = {{0, 0}, {4, 0}, {8, 0}};
    Polygon d;
    d.addRing(line, 3, false);
    EXPECT_DOUBLE_EQ(d.area(), 0.0);
    EXPECT_EQ(d.centroid(), (Point64{4, 0}));
    EXPECT_EQ(Polygon().anchor(), (Point64{0, 0}));
    EXPECT_EQ(Polygon().centroid(), (Point64{0, 0}));
    EXPECT_EQ(Polygon().boundingBox(), (Rect64{0, 0, 0, 0}));
    EXPECT_TRUE(Polygon().isEmpty());
}

TEST(Polygon, CentroidWeightsHolesNegatively) {
    // 10x10 outer with a 4x4 hole in its right half: centroid shifts left of 5.
    Polygon p = square(0, 0, 10);
    p = square(5, 3, 4, true, p);
    // (100*5 - 16*7) / 84 = 4.619..
    EXPECT_EQ(p.centroid(), (Point64{5, 5}));
    Polygon q = square(0, 0, 100);
    q = square(50, 40, 40, true, q);
    // (10000*50 - 1600*70) / 8400 = 46.19..
    EXPECT_EQ(q.centroid(), (Point64{46, 48}));
}

TEST(Polygon, LargeCoordinatesDoNotOverflow) { // UR-IMG-2
    Polygon p = square(3000000000LL, 3000000000LL, 100000);
    EXPECT_DOUBLE_EQ(p.area(), 1e10);
    EXPECT_EQ(p.centroid(), (Point64{3000050000LL, 3000050000LL}));
}

TEST(Polygon, CopyAndMovePreserveRings) {
    Polygon p = square(0, 0, 3);
    Polygon c(p);
    Polygon m(std::move(p));
    EXPECT_EQ(c.ringCount(), 1);
    EXPECT_EQ(m.ringPointCount(0), 4);
    Polygon a;
    a = c;
    EXPECT_EQ(a.ringCount(), 1);
    Polygon b;
    b = std::move(a);
    EXPECT_EQ(b.ringPointCount(0), 4);
}

namespace {
Polygon ring(std::initializer_list<Point64> pts, bool hole = false, Polygon p = {}) {
    std::vector<Point64> v(pts);
    p.addRing(v.data(), static_cast<int64_t>(v.size()), hole);
    return p;
}
} // namespace

TEST(Polygon, AnchorWhenAHoleHoldsTheSmallestY) {
    Polygon p = ring({{0, 5}, {10, 5}, {10, 15}, {0, 15}});
    p = ring({{3, 2}, {6, 2}, {6, 8}, {3, 8}}, true, p);
    EXPECT_EQ(p.anchor(), (Point64{3, 2}));
}

TEST(Polygon, AnchorWhenASecondOuterRingHoldsTheSmallestY) {
    Polygon p = square(0, 5, 10);
    p = ring({{20, 1}, {30, 1}, {30, 9}, {20, 9}}, false, p);
    EXPECT_EQ(p.anchor(), (Point64{20, 1}));
}

TEST(Polygon, AnchorTieOnYAcrossRingsTakesSmallestX) {
    Polygon p = ring({{7, 0}, {12, 0}, {12, 9}, {7, 9}});
    p = ring({{2, 0}, {4, 0}, {4, 3}, {2, 3}}, true, p);
    EXPECT_EQ(p.anchor(), (Point64{2, 0}));
    Polygon q = ring({{2, 0}, {4, 0}, {4, 3}, {2, 3}});
    q = ring({{7, 0}, {12, 0}, {12, 9}, {7, 9}}, false, q);
    EXPECT_EQ(q.anchor(), (Point64{2, 0}));
}

TEST(Polygon, ContainsAcrossASlantedEdge) { // pixel centres, R8
    Polygon t = ring({{0, 0}, {10, 0}, {0, 10}}); // diagonal x + y = 10
    EXPECT_TRUE(t.contains({4, 4}));  // centre sum 9
    EXPECT_TRUE(t.contains({3, 5}));
    EXPECT_TRUE(t.contains({0, 8}));
    EXPECT_FALSE(t.contains({5, 5})); // centre sum 11
    EXPECT_FALSE(t.contains({6, 4}));
    EXPECT_FALSE(t.contains({1, 9}));
    EXPECT_FALSE(t.contains({-1, 0}));
    Polygon d = ring({{5, 0}, {10, 5}, {5, 10}, {0, 5}});
    EXPECT_TRUE(d.contains({3, 3}));
    EXPECT_TRUE(d.contains({2, 3}));
    EXPECT_TRUE(d.contains({5, 5}));
    EXPECT_FALSE(d.contains({1, 2}));
    EXPECT_FALSE(d.contains({0, 0}));
    EXPECT_FALSE(d.contains({8, 8}));
    EXPECT_DOUBLE_EQ(d.area(), 50.0);
}

TEST(Polygon, HoleWindingDoesNotMatter) {
    Polygon same = ring({{0, 0}, {10, 0}, {10, 10}, {0, 10}});
    same = ring({{1, 1}, {7, 1}, {7, 7}, {1, 7}}, true, same);
    Polygon opposite = ring({{0, 0}, {10, 0}, {10, 10}, {0, 10}});
    opposite = ring({{1, 1}, {1, 7}, {7, 7}, {7, 1}}, true, opposite);
    EXPECT_DOUBLE_EQ(opposite.area(), 64.0);
    EXPECT_DOUBLE_EQ(opposite.area(), same.area());
    EXPECT_EQ(opposite.centroid(), same.centroid());
    EXPECT_EQ(opposite.centroid(), (Point64{6, 6}));
    for (int64_t y = -1; y <= 10; ++y) {
        for (int64_t x = -1; x <= 10; ++x) {
            EXPECT_EQ(opposite.contains({x, y}), same.contains({x, y})) << x << "," << y;
        }
    }
    EXPECT_FALSE(opposite.contains({3, 3}));
    EXPECT_TRUE(opposite.contains({8, 8}));
}
