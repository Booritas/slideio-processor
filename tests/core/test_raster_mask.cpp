// Requirements: UR-IMG-5, UR-STG-3, UR-QA-2, UR-QA-3
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/core/raster_mask.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <utility>
#include <vector>

using namespace slideio::processor;

namespace {

std::vector<std::string> dump(const RasterMask& m) {
    const Rect64 r = m.regionAtLevel();
    std::vector<std::string> rows;
    for (int64_t y = 0; y < r.height; ++y) {
        std::string s;
        for (int64_t x = 0; x < r.width; ++x) {
            s += m.test({r.x + x, r.y + y}) ? '#' : '.';
        }
        rows.push_back(s);
    }
    return rows;
}

// True when segment a-b plausibly passes through the open unit square of pixel (x, y):
// the segment's line strictly separates two corners and its bounding box overlaps the
// open square. Over-approximates slightly, which is the safe direction for this check.
bool segmentTouchesPixel(Point64 a, Point64 b, int64_t x, int64_t y) {
    if (std::max(a.x, b.x) <= x || std::min(a.x, b.x) >= x + 1 || std::max(a.y, b.y) <= y ||
        std::min(a.y, b.y) >= y + 1) {
        return false;
    }
    bool pos = false, neg = false;
    for (int64_t cy = y; cy <= y + 1; ++cy) {
        for (int64_t cx = x; cx <= x + 1; ++cx) {
            const int64_t s = (b.x - a.x) * (cy - a.y) - (b.y - a.y) * (cx - a.x);
            pos = pos || s > 0;
            neg = neg || s < 0;
        }
    }
    return pos && neg;
}

// Area of the part of a simple ring inside the axis-aligned box [x0,x1]x[y0,y1]
// (Sutherland-Hodgman clip, then the shoelace formula).
double clippedArea(std::vector<std::pair<double, double>> poly, double x0, double y0, double x1, double y1) {
    auto clip = [&poly](auto inside, auto cross) {
        std::vector<std::pair<double, double>> out;
        for (size_t i = 0; i < poly.size(); ++i) {
            const auto a = poly[i], b = poly[(i + 1) % poly.size()];
            const bool ia = inside(a), ib = inside(b);
            if (ia) out.push_back(a);
            if (ia != ib) out.push_back(cross(a, b));
        }
        poly = out;
    };
    auto atX = [](double x) { return [x](std::pair<double, double> a, std::pair<double, double> b) {
        return std::make_pair(x, a.second + (x - a.first) * (b.second - a.second) / (b.first - a.first)); }; };
    auto atY = [](double y) { return [y](std::pair<double, double> a, std::pair<double, double> b) {
        return std::make_pair(a.first + (y - a.second) * (b.first - a.first) / (b.second - a.second), y); }; };
    clip([x0](auto p) { return p.first >= x0; }, atX(x0));
    clip([x1](auto p) { return p.first <= x1; }, atX(x1));
    clip([y0](auto p) { return p.second >= y0; }, atY(y0));
    clip([y1](auto p) { return p.second <= y1; }, atY(y1));
    double a = 0;
    for (size_t i = 0; i < poly.size(); ++i) {
        const auto p = poly[i], q = poly[(i + 1) % poly.size()];
        a += p.first * q.second - q.first * p.second;
    }
    return std::fabs(a) / 2;
}

} // namespace

TEST(RasterMask, ConstructionAndCoordinates) {
    RasterMask m(Rect64{10, 20, 4, 3}, 1, 2.0);
    EXPECT_TRUE(m.isEmpty()); m.row(1)[2] = 255;
    EXPECT_TRUE(m.test({12, 21})); EXPECT_FALSE(m.test({0, 0}));
    EXPECT_TRUE(m.testLevel0({24, 42})); EXPECT_TRUE(m.testLevel0({25, 43})); EXPECT_FALSE(m.testLevel0({26, 42}));
    EXPECT_EQ(m.setPixelCount(), 1); EXPECT_THROW(m.row(3), InvalidArgumentError);
    EXPECT_THROW(RasterMask(Rect64{0,0,0,1}, 0, 1.0), InvalidArgumentError);
    EXPECT_THROW(RasterMask(Rect64{0,0,1,1}, 0, std::nan("")), InvalidArgumentError);
    EXPECT_THROW(RasterMask(Rect64{0,0,1,1}, -1, 1.0), InvalidArgumentError);
}

TEST(RasterMask, AccessorsNullCopyMove) {
    RasterMask n; EXPECT_TRUE(n.isNull()); EXPECT_TRUE(n.isEmpty()); EXPECT_EQ(n.setPixelCount(), 0);
    EXPECT_FALSE(n.test({0, 0})); EXPECT_FALSE(n.testLevel0({0, 0}));
    EXPECT_THROW(n.row(0), InvalidArgumentError);
    Point64 v[] = {{0,0},{1,0},{1,1},{0,1}}; Polygon p; p.addRing(v, 4, false);
    EXPECT_THROW(rasteriseInto(n, p), InvalidArgumentError);
    EXPECT_THROW(RasterMask(Rect64{0,0,1,1}, 0, 0.0), InvalidArgumentError);
    EXPECT_THROW(RasterMask(Rect64{0,0,1,1}, 0, -2.0), InvalidArgumentError);
    EXPECT_THROW(RasterMask(Rect64{0,0,1,1}, 0, HUGE_VAL), InvalidArgumentError);
    EXPECT_THROW(RasterMask(Rect64{0,0,1,-1}, 0, 1.0), InvalidArgumentError);

    RasterMask m(Rect64{-3, 5, 2, 2}, 2, 4.0);
    EXPECT_FALSE(m.isNull()); EXPECT_EQ(m.regionAtLevel(), (Rect64{-3, 5, 2, 2}));
    EXPECT_EQ(m.level(), 2); EXPECT_EQ(m.levelScale(), 4.0);
    m.row(0)[0] = 255; EXPECT_TRUE(m.test({-3, 5}));
    EXPECT_TRUE(m.testLevel0({-12, 20})); EXPECT_TRUE(m.testLevel0({-9, 23}));
    EXPECT_FALSE(m.testLevel0({-13, 20}));                            // floor(-13/4) = -4, outside
    const RasterMask& cm = m; EXPECT_EQ(cm.row(0)[0], 255);
    RasterMask c(m); c.row(1)[1] = 255; EXPECT_EQ(m.setPixelCount(), 1); EXPECT_EQ(c.setPixelCount(), 2);
    RasterMask a; a = c; EXPECT_EQ(a.setPixelCount(), 2); a = n; EXPECT_TRUE(a.isNull());
    RasterMask mv(std::move(c)); EXPECT_EQ(mv.setPixelCount(), 2); EXPECT_TRUE(c.isNull());
    RasterMask ma; ma = std::move(mv); EXPECT_EQ(ma.level(), 2); EXPECT_EQ(ma.setPixelCount(), 2);
}

// Ruling (controller): conformance is superset + exact for rectilinear integer polygons.
// The brief's IntegerPolygonMatchesContainsExactly is replaced by the next two tests.
TEST(RasterMask, CoverageIsSupersetOfContains) {
    Point64 v[] = {{1,1},{7,2},{5,6},{2,5}}; Polygon p; p.addRing(v, 4, false);
    RasterMask m(Rect64{0,0,8,8}, 0, 1.0); rasteriseInto(m, p);
    int extra = 0;
    for (int64_t y = 0; y < 8; ++y) for (int64_t x = 0; x < 8; ++x) {
        if (p.contains({x, y})) { EXPECT_TRUE(m.test({x, y})) << x << "," << y; continue; }
        if (!m.test({x, y})) continue;
        ++extra;
        bool onBoundary = false;
        for (int k = 0; k < 4; ++k) onBoundary = onBoundary || segmentTouchesPixel(v[k], v[(k + 1) % 4], x, y);
        EXPECT_TRUE(onBoundary) << "covered but not on the boundary: " << x << "," << y;
    }
    EXPECT_GT(extra, 0);
}

TEST(RasterMask, AxisAlignedIntegerPolygonMatchesContainsExactly) {
    Point64 l[] = {{1,1},{7,1},{7,4},{4,4},{4,7},{1,7}}, h[] = {{2,2},{3,2},{3,3},{2,3}};
    Polygon p; p.addRing(l, 6, false); p.addRing(h, 4, true);
    RasterMask m(Rect64{0,0,8,8}, 0, 1.0); rasteriseInto(m, p);
    for (int64_t y = 0; y < 8; ++y) for (int64_t x = 0; x < 8; ++x)
        EXPECT_EQ(m.test({x, y}), p.contains({x, y})) << x << "," << y;
    EXPECT_FALSE(m.test({2, 2})); EXPECT_EQ(m.setPixelCount(), 6 * 3 + 3 * 3 - 1);
}

TEST(RasterMask, ConservativeAtCoarseLevel) {
    Point64 v[] = {{5,5},{11,5},{11,11},{5,11}}; Polygon p; p.addRing(v, 4, false);
    RasterMask m(Rect64{0,0,4,4}, 2, 4.0); rasteriseInto(m, p);      // level-0 [5,11) -> pixels 1..2
    EXPECT_EQ(dump(m), (std::vector<std::string>{"....", ".##.", ".##.", "...."}));
    RasterMask e(Rect64{0,0,4,4}, 2, 4.0);                            // edge on pixel boundary only
    Point64 w[] = {{4,4},{8,4},{8,8},{4,8}}; Polygon q; q.addRing(w, 4, false); rasteriseInto(e, q);
    EXPECT_EQ(dump(e), (std::vector<std::string>{"....", ".#..", "....", "...."}));
}

TEST(RasterMask, HoleClearsOnlyFullyCoveredPixels) {
    Point64 o[] = {{0,0},{16,0},{16,16},{0,16}}, h[] = {{5,5},{11,5},{11,11},{5,11}};
    Polygon p; p.addRing(o, 4, false); p.addRing(h, 4, true);
    RasterMask m(Rect64{0,0,4,4}, 2, 4.0); rasteriseInto(m, p);       // hole covers no pixel fully
    EXPECT_EQ(dump(m), (std::vector<std::string>{"####", "####", "####", "####"}));
    Point64 h2[] = {{4,4},{12,4},{12,12},{4,12}}; Polygon r; r.addRing(o, 4, false); r.addRing(h2, 4, true);
    RasterMask n(Rect64{0,0,4,4}, 2, 4.0); rasteriseInto(n, r);
    EXPECT_EQ(dump(n), (std::vector<std::string>{"####", "#..#", "#..#", "####"}));
}

TEST(RasterMask, SeamAgreementAcrossTiles) {                          // Review Focus 2
    Point64 v[] = {{3,1},{61,9},{40,58},{9,35}}; Polygon p; p.addRing(v, 4, false);
    for (double scale : {1.0, 4.0}) {
        int64_t n = scale == 1.0 ? 64 : 16;
        RasterMask whole(Rect64{0,0,n,n}, 0, scale), left(Rect64{0,0,n/2,n}, 0, scale),
                   right(Rect64{n/2,0,n - n/2,n}, 0, scale);
        rasteriseInto(whole, p); rasteriseInto(left, p); rasteriseInto(right, p);
        for (int64_t y = 0; y < n; ++y) for (int64_t x = 0; x < n; ++x)
            EXPECT_EQ(whole.test({x,y}), x < n/2 ? left.test({x,y}) : right.test({x,y})) << scale << ":" << x << "," << y;
    }
}

TEST(RasterMask, SeamAgreementAtNonDyadicScales) {
    // Ruling (controller): absolute level mapping. At scale 3 the vertex (3,30) lies on a lattice
    // line, and edges (58,4)-(40,40) and (40,40)-(4,58) pass exactly through lattice corners
    // between their vertices; 4.0001 is SDS 01 1.17's drifting pyramid. Tiles split at a
    // non-aligned column and row must agree with the whole region at every pixel.
    Point64 v[] = {{1,1},{58,4},{40,40},{4,58},{3,30}}; Polygon p; p.addRing(v, 5, false);
    for (double scale : {3.0, 4.0001}) {
        const int64_t n = static_cast<int64_t>(std::ceil(64.0 / scale)) + 2, o = -1;
        const int64_t sx = n / 2 + 1, sy = n / 3 + 1;
        RasterMask whole(Rect64{o, o, n, n}, 1, scale);
        rasteriseInto(whole, p);
        const Rect64 parts[] = {{o, o, sx, sy}, {o + sx, o, n - sx, sy},
                                {o, o + sy, sx, n - sy}, {o + sx, o + sy, n - sx, n - sy}};
        int64_t setInParts = 0;
        for (const Rect64& r : parts) {
            RasterMask t(r, 1, scale); rasteriseInto(t, p); setInParts += t.setPixelCount();
            for (int64_t y = r.y; y < r.bottom(); ++y) for (int64_t x = r.x; x < r.right(); ++x)
                EXPECT_EQ(t.test({x, y}), whole.test({x, y})) << scale << ":" << x << "," << y;
        }
        EXPECT_EQ(setInParts, whole.setPixelCount());
        EXPECT_GT(whole.setPixelCount(), 0); EXPECT_LT(whole.setPixelCount(), n * n);
    }
}

TEST(RasterMask, AdditiveAndClear) {
    RasterMask m(Rect64{0,0,4,4}, 0, 1.0);
    Point64 a[] = {{0,0},{1,0},{1,1},{0,1}}, b[] = {{3,3},{4,3},{4,4},{3,4}};
    Polygon p, q; p.addRing(a, 4, false); q.addRing(b, 4, false);
    rasteriseInto(m, p); rasteriseInto(m, q); EXPECT_EQ(m.setPixelCount(), 2);
    m.clear(); EXPECT_TRUE(m.isEmpty());
}

TEST(RasterMask, PolygonOutsideRegionSetsNothing) {
    Point64 v[] = {{100,100},{110,100},{110,110},{100,110}}; Polygon p; p.addRing(v, 4, false);
    RasterMask m(Rect64{0,0,8,8}, 0, 1.0); rasteriseInto(m, p); EXPECT_TRUE(m.isEmpty());
    Point64 w[] = {{-20,0},{0,0},{0,8},{-20,8}}; Polygon q; q.addRing(w, 4, false);  // abuts the left edge
    rasteriseInto(m, q); EXPECT_TRUE(m.isEmpty());
    Polygon empty; rasteriseInto(m, empty); EXPECT_TRUE(m.isEmpty());
}

TEST(RasterMask, PolygonLargerThanRegionFillsIt) {
    Point64 v[] = {{-50,-50},{50,-50},{50,50},{-50,50}}; Polygon p; p.addRing(v, 4, false);
    RasterMask m(Rect64{10,10,8,8}, 0, 1.0); rasteriseInto(m, p); EXPECT_EQ(m.setPixelCount(), 64);
    Point64 d[] = {{0,-1000},{1000,0},{0,1000},{-1000,0}}; Polygon q; q.addRing(d, 4, false);  // diamond
    RasterMask c(Rect64{-4,-4,8,8}, 1, 2.0); rasteriseInto(c, q); EXPECT_EQ(c.setPixelCount(), 64);
}

TEST(RasterMask, TriangleAtScaleTwoPixelByPixel) {
    // Level-0 (0,0),(8,2),(2,8) -> level-1 pixel units A(0,0), B(4,1), C(1,4).
    // AB marks row 0 cells 0..3; BC (u+v=5) marks (3,1),(2,2),(1,3); CA (v=4u) marks cell 0 of
    // every row. Centres: row 0 in [0.125,2), row 1 in [0.375,3.5), row 2 in [0.625,2.5),
    // row 3 in [0.875,1.5) -- that last one holds no centre.
    Point64 v[] = {{0,0},{8,2},{2,8}}; Polygon p; p.addRing(v, 3, false);
    RasterMask m(Rect64{0,0,4,4}, 1, 2.0); rasteriseInto(m, p);
    EXPECT_EQ(dump(m), (std::vector<std::string>{"####", "####", "###.", "##.."}));
}

TEST(RasterMask, DiagonalThroughLatticeCornersMarksNoSideCells) {
    // Hypotenuse u+v=4 passes exactly through corners (3,1),(2,2),(1,3).
    Point64 v[] = {{0,0},{8,0},{0,8}}; Polygon p; p.addRing(v, 3, false);
    RasterMask m(Rect64{0,0,4,4}, 1, 2.0); rasteriseInto(m, p);
    EXPECT_EQ(dump(m), (std::vector<std::string>{"####", "###.", "##..", "#..."}));
    RasterMask t(Rect64{1,1,4,4}, 1, 2.0); rasteriseInto(t, p);       // same answer from another origin
    for (int64_t y = 1; y < 4; ++y) for (int64_t x = 1; x < 4; ++x) EXPECT_EQ(t.test({x, y}), m.test({x, y}));
}

TEST(RasterMask, RandomStarPolygonsMatchAreaReference) {
    // Star-shaped rings with vertices sorted by angle are simple, so the pixel's overlap
    // with the even-odd region is the clipped ring's area. Covered must equal area > 0.
    std::mt19937 rng(20261017u);
    const double kPi = 3.14159265358979323846;
    for (int iter = 0; iter < 60; ++iter) {
        const double scale = iter % 3 == 0 ? 1.0 : iter % 3 == 1 ? 2.0 : 4.0;
        const int n = 3 + static_cast<int>(rng() % 9);
        std::vector<double> angles;
        for (int k = 0; k < n; ++k) angles.push_back(2 * kPi * (rng() % 100000) / 100000.0);
        std::sort(angles.begin(), angles.end());
        std::vector<Point64> v;
        std::vector<std::pair<double, double>> ref;
        for (double a : angles) {
            const double r = 4 + static_cast<double>(rng() % 28);
            Point64 pt{32 + static_cast<int64_t>(std::lround(r * std::cos(a))),
                       32 + static_cast<int64_t>(std::lround(r * std::sin(a)))};
            if (!v.empty() && v.back() == pt) continue;
            v.push_back(pt);
            ref.emplace_back(static_cast<double>(pt.x) / scale, static_cast<double>(pt.y) / scale);
        }
        if (v.size() < 3) continue;
        Polygon p; p.addRing(v.data(), static_cast<int64_t>(v.size()), false);
        const int64_t g = static_cast<int64_t>(64 / scale);
        const int64_t ox = static_cast<int64_t>(rng() % 5), oy = static_cast<int64_t>(rng() % 5);
        RasterMask m(Rect64{ox, oy, g - ox, g - oy}, 0, scale); rasteriseInto(m, p);
        for (int64_t y = oy; y < g; ++y) for (int64_t x = ox; x < g; ++x) {
            const double area = clippedArea(ref, double(x), double(y), double(x + 1), double(y + 1));
            EXPECT_EQ(m.test({x, y}), area > 1e-9) << iter << ": " << x << "," << y << " area " << area;
        }
    }
}

TEST(RasterMask, RandomSeamAgreementAtNonDyadicScales) {
    // Random rings whose edges run along slopes +-1 and +-1/2 from random integer vertices, so
    // many of them pass exactly through lattice corners and centres between vertices. Every
    // tile split must agree with the whole region at scales 3 and 4.0001.
    std::mt19937 rng(4242u);
    int64_t disagreements = 0;
    int tried = 0;
    for (int iter = 0; iter < 200; ++iter) {
        const double scale = iter % 2 == 0 ? 3.0 : 4.0001;
        std::vector<Point64> v;
        Point64 cur{static_cast<int64_t>(rng() % 20) + 20, static_cast<int64_t>(rng() % 20) + 20};
        const int n = 3 + static_cast<int>(rng() % 6);
        for (int k = 0; k < n; ++k) {
            v.push_back(cur);
            const int64_t d = 1 + static_cast<int64_t>(rng() % 12);
            const int64_t dx = (rng() % 2 ? d : -d), dy = (rng() % 3 == 0 ? dx / 2 : (rng() % 2 ? d : -d));
            cur = Point64{cur.x + dx, cur.y + dy};
        }
        Polygon p;
        try { p.addRing(v.data(), static_cast<int64_t>(v.size()), false); }
        catch (const InvalidArgumentError&) { continue; }                // fewer than 3 distinct points
        ++tried;
        const int64_t g = 24, o = static_cast<int64_t>(rng() % 3);
        RasterMask whole(Rect64{o, o, g, g}, 1, scale); rasteriseInto(whole, p);
        const int64_t sx = 1 + static_cast<int64_t>(rng() % (g - 1)), sy = 1 + static_cast<int64_t>(rng() % (g - 1));
        const Rect64 parts[] = {{o, o, sx, sy}, {o + sx, o, g - sx, sy},
                                {o, o + sy, sx, g - sy}, {o + sx, o + sy, g - sx, g - sy}};
        for (const Rect64& r : parts) {
            RasterMask t(r, 1, scale); rasteriseInto(t, p);
            for (int64_t y = r.y; y < r.bottom(); ++y) for (int64_t x = r.x; x < r.right(); ++x)
                if (t.test({x, y}) != whole.test({x, y})) ++disagreements;
        }
    }
    EXPECT_EQ(disagreements, 0);
    EXPECT_GT(tried, 150);
}
