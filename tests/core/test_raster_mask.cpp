// Requirements: UR-IMG-5, UR-STG-3, UR-QA-2, UR-QA-3
#include <slideio/processor/core/block_view.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/core/raster_mask.hpp>

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

TEST(RasterMask, ConstructorRejectsOverflowAndTinyScale) {
    const int64_t big = std::numeric_limits<int64_t>::max();
    EXPECT_THROW(RasterMask(Rect64{big - 1, 0, 4, 4}, 0, 1.0), InvalidArgumentError);
    EXPECT_THROW(RasterMask(Rect64{0, big - 1, 4, 4}, 0, 1.0), InvalidArgumentError);
    EXPECT_THROW(RasterMask(Rect64{0, 0, 4, 4}, 0, 1e-7), InvalidArgumentError);
    EXPECT_NO_THROW(RasterMask(Rect64{0, 0, 4, 4}, 0, 1e-6));
}

TEST(RasterMask, HugeCoordinatesSpanningRegionFillIt) {
    const int64_t k = 3000000000LL;
    Point64 sq[] = {{-k, -k}, {k, -k}, {k, k}, {-k, k}}, dia[] = {{-k, 0}, {0, -k}, {k, 0}, {0, k}};
    for (double scale : {1.0, 2.0}) {
        Polygon a, b; a.addRing(sq, 4, false); b.addRing(dia, 4, false);
        RasterMask m(Rect64{-4, -4, 8, 8}, 0, scale); rasteriseInto(m, a);
        EXPECT_EQ(m.setPixelCount(), 64) << scale;
        RasterMask d(Rect64{-4, -4, 8, 8}, 0, scale); rasteriseInto(d, b);
        EXPECT_EQ(d.setPixelCount(), 64) << scale;
        EXPECT_TRUE(d.test({-4, -4})); EXPECT_TRUE(d.test({3, 3}));
    }
}

TEST(RasterMask, HugeCoordinatesFarOutsideSetNothing) {
    const int64_t k = 3000000000LL, off = 2 * k;                      // spans [k, 3k], region near 0
    Point64 sq[] = {{off - k, off - k}, {off + k, off - k}, {off + k, off + k}, {off - k, off + k}};
    Point64 dia[] = {{off - k, off}, {off, off - k}, {off + k, off}, {off, off + k}};
    Point64 neg[] = {{-off - k, -off - k}, {-off + k, -off - k}, {-off + k, -off + k}, {-off - k, -off + k}};
    for (const Point64* v : {sq, dia, neg}) {
        Polygon p; p.addRing(v, 4, false);
        RasterMask m(Rect64{-4, -4, 8, 8}, 0, 1.0); rasteriseInto(m, p);
        EXPECT_TRUE(m.isEmpty());
        EXPECT_FALSE(m.test({0, 0})); EXPECT_FALSE(m.test({-4, 3}));
    }
}

TEST(RasterMask, HorizontalEdgeOnPixelCentreLine) {
    // Level-0 (1,1),(7,1),(7,5),(1,5) at scale 2 is the level rectangle u in [0.5,3.5], v in
    // [0.5,2.5]: the top edge lies on row 0's centre line and the bottom edge on row 2's.
    Point64 v[] = {{1,1},{7,1},{7,5},{1,5}}; Polygon p; p.addRing(v, 4, false);
    RasterMask m(Rect64{0,0,5,4}, 1, 2.0); rasteriseInto(m, p);
    EXPECT_EQ(dump(m), (std::vector<std::string>{"####.", "####.", "####.", "....."}));
    // Boundary rule: both horizontal edges cross their rows' open interior, so row 2 is covered
    // although its centre line (the bottom edge) is not inside by the tie rule; row 3 is untouched.
    for (int64_t x = 0; x < 5; ++x) EXPECT_FALSE(m.test({x, 3}));
    // Every level pixel that contains the centre of a level-0 pixel inside the polygon is set.
    for (int64_t y = 0; y < 8; ++y) for (int64_t x = 0; x < 10; ++x)
        if (p.contains({x, y})) EXPECT_TRUE(m.test({x / 2, y / 2})) << x << "," << y;
    // The same shape at scale 1 with integer edges: exact against contains.
    Point64 w[] = {{1,1},{4,1},{4,3},{1,3}}; Polygon q; q.addRing(w, 4, false);
    RasterMask n(Rect64{0,0,5,4}, 0, 1.0); rasteriseInto(n, q);
    for (int64_t y = 0; y < 4; ++y) for (int64_t x = 0; x < 5; ++x)
        EXPECT_EQ(n.test({x, y}), q.contains({x, y})) << x << "," << y;
}

TEST(RasterMask, OverlappingOuterRingsCancelUnderEvenOdd) {
    // Level units at scale 2: A = [0,6]^2, B = [2.5,8.5]^2; the overlap [2.5,6]^2 cancels.
    Point64 a[] = {{0,0},{12,0},{12,12},{0,12}}, b[] = {{5,5},{17,5},{17,17},{5,17}};
    Polygon p; p.addRing(a, 4, false); p.addRing(b, 4, false);
    RasterMask m(Rect64{0,0,10,10}, 1, 2.0); rasteriseInto(m, p);
    EXPECT_EQ(dump(m), (std::vector<std::string>{
        "######....", "######....", "#########.", "###...###.", "###...###.", "###...###.",
        "..#######.", "..#######.", "..#######.", ".........."}));
    EXPECT_FALSE(m.test({4, 4}));                                     // fully inside both rings
    EXPECT_TRUE(m.test({2, 4}));                                      // B's left edge crosses it
}

TEST(DilateMask, SquareSeparableWithinRegion) {
    RasterMask m(Rect64{0,0,5,5}, 0, 1.0); m.row(2)[2] = 255; dilateMask(m, 1);
    EXPECT_EQ(dump(m), (std::vector<std::string>{".....", ".###.", ".###.", ".###.", "....."}));
    RasterMask c(Rect64{0,0,3,3}, 0, 1.0); c.row(0)[0] = 255; dilateMask(c, 5);
    EXPECT_EQ(c.setPixelCount(), 9); dilateMask(c, 0);
    EXPECT_THROW(dilateMask(c, -1), InvalidArgumentError);
}

TEST(DilateMask, EdgesRadiusTwoAndNull) {
    RasterMask m(Rect64{-2, 3, 7, 4}, 0, 1.0); m.row(0)[0] = 255; m.row(3)[6] = 255; dilateMask(m, 2);
    EXPECT_EQ(dump(m), (std::vector<std::string>{"###....", "###.###", "###.###", "....###"}));
    RasterMask e(Rect64{0,0,4,4}, 0, 1.0); dilateMask(e, 3); EXPECT_TRUE(e.isEmpty());
    RasterMask big(Rect64{0,0,3,3}, 0, 1.0); big.row(1)[1] = 255; dilateMask(big, 2147483647);
    EXPECT_EQ(big.setPixelCount(), 9);
    RasterMask n; EXPECT_THROW(dilateMask(n, 1), InvalidArgumentError);
}

TEST(RasteriseBlockView, WritesValueAtViewScale) {
    std::vector<int64_t> buf(4 * 4, 0);
    BlockPlane pl{buf.data(), DataType::Int64, 4 * 8, 0};
    BlockView v(4, 4, Rect64{0, 0, 16, 16}, 2, &pl, 1);
    Point64 s[] = {{5,5},{11,5},{11,11},{5,11}}; Polygon p; p.addRing(s, 4, false);
    rasteriseInto(v, 0, p, (int64_t(1) << 40));
    EXPECT_EQ(buf[1 * 4 + 1], int64_t(1) << 40); EXPECT_EQ(buf[2 * 4 + 2], int64_t(1) << 40);
    EXPECT_EQ(buf[0], 0); EXPECT_EQ(buf[3 * 4 + 3], 0);
}

TEST(RasteriseBlockView, AbsoluteOriginAndUntouchedPixels) {
    // View at level-0 x in [30,42), y in [60,72), 4x4 pixels: scale 3, origin level pixel (10,20).
    std::vector<int32_t> buf(4 * 4, 7);
    BlockPlane pl{buf.data(), DataType::Int32, 4 * 4, 0};
    BlockView v(4, 4, Rect64{30, 60, 12, 12}, 1, &pl, 1);
    Point64 s[] = {{33,63},{39,63},{39,69},{33,69}}; Polygon p; p.addRing(s, 4, false);   // pixels 1..2
    rasteriseInto(v, 0, p, -5);
    for (int j = 0; j < 4; ++j) for (int i = 0; i < 4; ++i)
        EXPECT_EQ(buf[j * 4 + i], (i >= 1 && i <= 2 && j >= 1 && j <= 2) ? -5 : 7) << i << "," << j;
}

TEST(RasteriseBlockView, ValueMustFitType) {
    uint8_t b[4]; BlockPlane pb{b, DataType::Byte, 2, 0}; BlockView vb(2, 2, Rect64{0,0,2,2}, 0, &pb, 1);
    Point64 s[] = {{0,0},{2,0},{2,2},{0,2}}; Polygon p; p.addRing(s, 4, false);
    EXPECT_THROW(rasteriseInto(vb, 0, p, 256), InvalidArgumentError);
    EXPECT_THROW(rasteriseInto(vb, 1, p, 1), InvalidArgumentError);
    uint16_t h[4]; BlockPlane ph{h, DataType::Float16, 4, 0}; BlockView vh(2, 2, Rect64{0,0,2,2}, 0, &ph, 1);
    rasteriseInto(vh, 0, p, 3); EXPECT_EQ(h[0], 0x4200);               // 3.0 in IEEE half
    EXPECT_THROW(rasteriseInto(vh, 0, p, 2049), InvalidArgumentError);
}

TEST(RasteriseBlockView, TypeRangesAndFloatEncodings) {
    Point64 s[] = {{0,0},{2,0},{2,2},{0,2}}; Polygon p; p.addRing(s, 4, false);
    int8_t i8[4]; BlockPlane p8{i8, DataType::Int8, 2, 0}; BlockView v8(2, 2, Rect64{0,0,2,2}, 0, &p8, 1);
    rasteriseInto(v8, 0, p, -128); EXPECT_EQ(i8[3], -128);
    EXPECT_THROW(rasteriseInto(v8, 0, p, -129), InvalidArgumentError);
    EXPECT_THROW(rasteriseInto(v8, 0, p, 128), InvalidArgumentError);
    uint64_t u64[4]; BlockPlane pu{u64, DataType::UInt64, 16, 0}; BlockView vu(2, 2, Rect64{0,0,2,2}, 0, &pu, 1);
    rasteriseInto(vu, 0, p, std::numeric_limits<int64_t>::max());
    EXPECT_EQ(u64[0], uint64_t(std::numeric_limits<int64_t>::max()));
    EXPECT_THROW(rasteriseInto(vu, 0, p, -1), InvalidArgumentError);
    uint32_t u32[4]; BlockPlane p32{u32, DataType::UInt32, 8, 0}; BlockView v32(2, 2, Rect64{0,0,2,2}, 0, &p32, 1);
    rasteriseInto(v32, 0, p, 4294967295LL); EXPECT_EQ(u32[1], 4294967295u);
    EXPECT_THROW(rasteriseInto(v32, 0, p, 4294967296LL), InvalidArgumentError);
    float f[4]; BlockPlane pf{f, DataType::Float32, 8, 0}; BlockView vf(2, 2, Rect64{0,0,2,2}, 0, &pf, 1);
    rasteriseInto(vf, 0, p, 16777216); EXPECT_EQ(f[2], 16777216.0f);
    EXPECT_THROW(rasteriseInto(vf, 0, p, 16777217), InvalidArgumentError);
    double d[4]; BlockPlane pd{d, DataType::Float64, 16, 0}; BlockView vd(2, 2, Rect64{0,0,2,2}, 0, &pd, 1);
    rasteriseInto(vd, 0, p, -(int64_t(1) << 53)); EXPECT_EQ(d[3], -9007199254740992.0);
    EXPECT_THROW(rasteriseInto(vd, 0, p, (int64_t(1) << 53) + 1), InvalidArgumentError);
    uint16_t h[4]; BlockPlane ph{h, DataType::Float16, 4, 0}; BlockView vh(2, 2, Rect64{0,0,2,2}, 0, &ph, 1);
    rasteriseInto(vh, 0, p, -2048); EXPECT_EQ(h[0], 0xE800);          // -2048 = -2^11
    rasteriseInto(vh, 0, p, 1); EXPECT_EQ(h[1], 0x3C00);
    rasteriseInto(vh, 0, p, 0); EXPECT_EQ(h[2], 0x0000);
    rasteriseInto(vh, 0, p, 1025); EXPECT_EQ(h[3], 0x6401);
    BlockView nullView; EXPECT_THROW(rasteriseInto(nullView, 0, p, 1), InvalidArgumentError);
    EXPECT_THROW(rasteriseInto(v8, -1, p, 1), InvalidArgumentError);
}

TEST(RasteriseBlockView, PlanesAndStrideAndTinyScale) {
    // Two planes, padded rows: only the chosen plane changes, padding bytes stay.
    std::vector<uint8_t> a(3 * 8, 9), b(3 * 8, 9);
    BlockPlane pl[] = {{a.data(), DataType::Byte, 8, 0}, {b.data(), DataType::Byte, 8, 1}};
    BlockView v(3, 3, Rect64{0, 0, 3, 3}, 0, pl, 2);
    Point64 s[] = {{1,1},{2,1},{2,2},{1,2}}; Polygon p; p.addRing(s, 4, false);
    rasteriseInto(v, 1, p, 200);
    for (size_t k = 0; k < a.size(); ++k) EXPECT_EQ(a[k], 9);
    for (size_t y = 0; y < 3; ++y) for (size_t x = 0; x < 8; ++x)
        EXPECT_EQ(b[y * 8 + x], (x == 1 && y == 1) ? 200 : 9) << x << "," << y;
    std::vector<uint8_t> big(2000000, 0); BlockPlane pt{big.data(), DataType::Byte, 2000000, 0};
    BlockView tiny(2000000, 1, Rect64{0, 0, 1, 1}, 0, &pt, 1);        // scale 5e-7 < 1e-6
    EXPECT_THROW(rasteriseInto(tiny, 0, p, 1), InvalidArgumentError);
    EXPECT_TRUE(std::all_of(big.begin(), big.end(), [](uint8_t b) { return b == 0; }));
}

TEST(RasteriseBlockView, MatchesMaskOverloadAtScaleOne) {
    Point64 v[] = {{1,1},{13,3},{9,12},{2,10}}, h[] = {{5,5},{7,5},{7,7},{5,7}};
    Polygon p; p.addRing(v, 4, false); p.addRing(h, 4, true);
    RasterMask m(Rect64{0, 0, 16, 16}, 0, 1.0); rasteriseInto(m, p);
    std::vector<uint8_t> buf(16 * 16, 0);
    BlockPlane pl{buf.data(), DataType::Byte, 16, 0};
    BlockView view(16, 16, Rect64{0, 0, 16, 16}, 0, &pl, 1);
    rasteriseInto(view, 0, p, 255);
    for (int64_t y = 0; y < 16; ++y) for (int64_t x = 0; x < 16; ++x)
        EXPECT_EQ(buf[static_cast<size_t>(y * 16 + x)] != 0, m.test({x, y})) << x << "," << y;
    EXPECT_GT(m.setPixelCount(), 0);
}

TEST(RasteriseBlockView, OriginRoundsToNearestLevelPixel) {
    // slideRect.x = 31 at scale 3: origin llround(31 / 3) = 10, so view pixel i is level pixel 10 + i.
    std::vector<int32_t> buf(4 * 4, 0);
    BlockPlane pl{buf.data(), DataType::Int32, 4 * 4, 0};
    BlockView v(4, 4, Rect64{31, 60, 12, 12}, 1, &pl, 1);
    Point64 s[] = {{36,63},{42,63},{42,69},{36,69}}; Polygon p; p.addRing(s, 4, false);  // level x 12..14, y 21..22
    rasteriseInto(v, 0, p, 9);
    for (int j = 0; j < 4; ++j) for (int i = 0; i < 4; ++i)
        EXPECT_EQ(buf[j * 4 + i], (i >= 2 && i <= 3 && j >= 1 && j <= 2) ? 9 : 0) << i << "," << j;
}

TEST(RasterMask, MoveAssignLeavesTheSourceNullNeverSwaps) {
    RasterMask src(Rect64{0, 0, 2, 2}, 1, 2.0);
    src.row(0)[0] = 255;
    RasterMask dst(Rect64{5, 5, 3, 3}, 0, 1.0); // non-null target: a swap would hand it to src
    dst = std::move(src);
    EXPECT_EQ(dst.regionAtLevel(), (Rect64{0, 0, 2, 2}));
    EXPECT_EQ(dst.setPixelCount(), 1);
    EXPECT_TRUE(src.isNull());
    EXPECT_EQ(src.regionAtLevel(), (Rect64{0, 0, 0, 0}));
}

TEST(DilateMask, MatchesBruteForceWhetherOrNotTheRowRingWraps) {
    // Heights below, at and above 2r + 2 exercise the ring of horizontal rows.
    uint32_t seed = 12345;
    auto next = [&] { seed = seed * 1664525u + 1013904223u; return seed >> 24; };
    for (int32_t r : {1, 2, 3}) {
        for (int64_t h : {int64_t{1}, int64_t{2} * r + 1, int64_t{2} * r + 2, int64_t{2} * r + 3, int64_t{17}}) {
            const int64_t w = 9;
            RasterMask m(Rect64{0, 0, w, h}, 0, 1.0);
            for (int64_t y = 0; y < h; ++y)
                for (int64_t x = 0; x < w; ++x) m.row(y)[x] = next() < 20 ? 255 : 0;
            std::vector<std::vector<bool>> expect(static_cast<size_t>(h), std::vector<bool>(static_cast<size_t>(w)));
            for (int64_t y = 0; y < h; ++y)
                for (int64_t x = 0; x < w; ++x)
                    for (int64_t j = std::max<int64_t>(0, y - r); j <= std::min(h - 1, y + r); ++j)
                        for (int64_t i = std::max<int64_t>(0, x - r); i <= std::min(w - 1, x + r); ++i)
                            if (m.row(j)[i]) expect[size_t(y)][size_t(x)] = true;
            dilateMask(m, r);
            for (int64_t y = 0; y < h; ++y)
                for (int64_t x = 0; x < w; ++x)
                    ASSERT_EQ(m.row(y)[x] != 0, expect[size_t(y)][size_t(x)]) << "r=" << r << " h=" << h << " at " << x << "," << y;
        }
    }
}

TEST(RasterMask, UnitSquareAtTwoToThe53IsCovered) { // issue #4
    const int64_t x = int64_t(1) << 53;
    Point64 v[] = {{x, 0}, {x + 1, 0}, {x + 1, 1}, {x, 1}};
    Polygon p; p.addRing(v, 4, false);
    RasterMask m(Rect64{x, 0, 1, 1}, 0, 1.0); rasteriseInto(m, p);
    EXPECT_EQ(m.setPixelCount(), 1);
    RasterMask around(Rect64{x - 2, -2, 5, 5}, 0, 1.0); rasteriseInto(around, p);
    EXPECT_EQ(around.setPixelCount(), 1);
    EXPECT_TRUE(around.test({x, 0}));
    std::vector<uint8_t> buf(3 * 3, 0);
    BlockPlane pl{buf.data(), DataType::Byte, 3, 0};
    BlockView view(3, 3, Rect64{x - 1, -1, 3, 3}, 0, &pl, 1);
    rasteriseInto(view, 0, p, 7);
    EXPECT_EQ(buf, (std::vector<uint8_t>{0, 0, 0, 0, 7, 0, 0, 0, 0}));
}

TEST(RasterMask, LargeOffsetsMatchTheOriginAtIntegerScales) {
    // Shifting a polygon by a multiple of the scale shifts its mask by whole level pixels, so
    // far above 2^53 the mask must equal the one near the origin, pixel for pixel.
    Point64 v[] = {{1,1},{58,4},{40,40},{4,58},{3,30}};
    const int64_t d = int64_t(3) * 4 << 57;                            // ~1.7e18, a multiple of 3 and 4
    std::vector<Point64> shifted;
    for (const Point64& q : v) shifted.push_back({q.x + d, q.y + d});
    Polygon nearP, farP;
    nearP.addRing(v, 5, false); farP.addRing(shifted.data(), 5, false);
    for (double scale : {1.0, 3.0, 4.0}) {
        const int64_t n = static_cast<int64_t>(std::ceil(64.0 / scale)) + 2;
        const int64_t s = d / static_cast<int64_t>(scale);
        RasterMask a(Rect64{-1, -1, n, n}, 1, scale), b(Rect64{s - 1, s - 1, n, n}, 1, scale);
        rasteriseInto(a, nearP); rasteriseInto(b, farP);
        EXPECT_GT(a.setPixelCount(), 0);
        for (int64_t y = 0; y < n; ++y) for (int64_t x = 0; x < n; ++x)
            ASSERT_EQ(a.row(y)[x], b.row(y)[x]) << scale << ":" << x << "," << y;
    }
}

TEST(RasterMask, SeamAgreementFarFromTheOrigin) {
    // The seam guarantee must survive the polygon-local frame at a non-dyadic scale.
    Point64 v[] = {{1,1},{58,4},{40,40},{4,58},{3,30}};
    const int64_t d = (int64_t(1) << 60) + 12345;
    std::vector<Point64> shifted;
    for (const Point64& q : v) shifted.push_back({q.x + d, q.y + d});
    Polygon p; p.addRing(shifted.data(), 5, false);
    for (double scale : {3.0, 4.0001}) {
        // d / scale in double is off by up to ~200 level pixels here, so the region carries a
        // 256-pixel margin on each side to be sure it holds the whole ring.
        const int64_t n = static_cast<int64_t>(std::ceil(64.0 / scale)) + 512;
        const int64_t o = static_cast<int64_t>(std::floor(static_cast<double>(d) / scale)) - 256;
        const int64_t sx = n / 2 + 1, sy = n / 3 + 1;
        RasterMask whole(Rect64{o, o, n, n}, 1, scale); rasteriseInto(whole, p);
        const Rect64 parts[] = {{o, o, sx, sy}, {o + sx, o, n - sx, sy},
                                {o, o + sy, sx, n - sy}, {o + sx, o + sy, n - sx, n - sy}};
        int64_t setInParts = 0;
        for (const Rect64& r : parts) {
            RasterMask t(r, 1, scale); rasteriseInto(t, p); setInParts += t.setPixelCount();
            for (int64_t y = r.y; y < r.bottom(); ++y) for (int64_t x = r.x; x < r.right(); ++x)
                ASSERT_EQ(t.test({x, y}), whole.test({x, y})) << scale << ":" << x << "," << y;
        }
        EXPECT_EQ(setInParts, whole.setPixelCount());
        EXPECT_GT(whole.setPixelCount(), 100);                          // the ring is ~2900 px^2
    }
}
