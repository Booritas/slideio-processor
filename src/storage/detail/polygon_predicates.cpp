#include "storage/detail/polygon_predicates.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
#endif

namespace slideio::processor::detail {

namespace {

// A signed value of up to 65 bits: a sign and a 64-bit magnitude.
struct Signed65 {
    bool negative;
    uint64_t magnitude;
};

// a - b, exactly: |a - b| < 2^64 for any int64 a and b.
Signed65 difference(int64_t a, int64_t b) noexcept {
    if (a >= b) return {false, static_cast<uint64_t>(a) - static_cast<uint64_t>(b)};
    return {true, static_cast<uint64_t>(b) - static_cast<uint64_t>(a)};
}

struct Unsigned128 {
    uint64_t high;
    uint64_t low;
};

// The full 128-bit product of two 64-bit magnitudes.
Unsigned128 multiply(uint64_t a, uint64_t b) noexcept {
#if defined(_MSC_VER) && defined(_M_X64)
    Unsigned128 product;
    product.low = _umul128(a, b, &product.high);
    return product;
#elif defined(__SIZEOF_INT128__)
    const unsigned __int128 product = static_cast<unsigned __int128>(a) * b;
    return {static_cast<uint64_t>(product >> 64), static_cast<uint64_t>(product)};
#else
    // Schoolbook multiplication on 32-bit halves.
    const uint64_t aLow = a & 0xFFFFFFFFu, aHigh = a >> 32;
    const uint64_t bLow = b & 0xFFFFFFFFu, bHigh = b >> 32;
    const uint64_t lowLow = aLow * bLow;
    const uint64_t highLow = aHigh * bLow;
    const uint64_t lowHigh = aLow * bHigh;
    const uint64_t highHigh = aHigh * bHigh;
    const uint64_t middle = (lowLow >> 32) + (highLow & 0xFFFFFFFFu) + (lowHigh & 0xFFFFFFFFu);
    return {highHigh + (highLow >> 32) + (lowHigh >> 32) + (middle >> 32), (middle << 32) | (lowLow & 0xFFFFFFFFu)};
#endif
}

int compare(Unsigned128 a, Unsigned128 b) noexcept {
    if (a.high != b.high) return a.high < b.high ? -1 : 1;
    if (a.low != b.low) return a.low < b.low ? -1 : 1;
    return 0;
}

// The sign of p * q - r * s, exactly.
int signOfDifferenceOfProducts(Signed65 p, Signed65 q, Signed65 r, Signed65 s) noexcept {
    const Unsigned128 left = multiply(p.magnitude, q.magnitude);
    const Unsigned128 right = multiply(r.magnitude, s.magnitude);
    const bool leftZero = left.high == 0 && left.low == 0;
    const bool rightZero = right.high == 0 && right.low == 0;
    const bool leftNegative = !leftZero && (p.negative != q.negative);
    const bool rightNegative = !rightZero && (r.negative != s.negative);
    if (leftNegative != rightNegative) return leftNegative ? -1 : 1;
    const int magnitudes = compare(left, right);
    return leftNegative ? -magnitudes : magnitudes;
}

struct Edge {
    Point64 a;
    Point64 b;
};

struct Rings {
    std::vector<Edge> edges;
    std::vector<Point64> vertices;
    std::vector<Point64> firstVertices;  // one per ring
};

Rings ringsOf(const Polygon& polygon) {
    Rings rings;
    std::vector<Point64> points;
    for (int32_t ring = 0; ring < polygon.ringCount(); ++ring) {
        const int64_t count = polygon.ringPointCount(ring);
        points.resize(static_cast<size_t>(count));
        polygon.ringPoints(ring, points.data(), count);
        for (size_t i = 0; i < points.size(); ++i) {
            rings.edges.push_back({points[i], points[(i + 1) % points.size()]});
        }
        rings.vertices.insert(rings.vertices.end(), points.begin(), points.end());
        rings.firstVertices.push_back(points.front());
    }
    return rings;
}

// True when p lies on the closed segment a-b.
bool onSegment(Point64 a, Point64 b, Point64 p) noexcept {
    return orientation(a, b, p) == 0 && std::min(a.x, b.x) <= p.x && p.x <= std::max(a.x, b.x) &&
           std::min(a.y, b.y) <= p.y && p.y <= std::max(a.y, b.y);
}

// True when the closed segments share a point, touching and collinear overlap included.
bool segmentsMeet(const Edge& p, const Edge& q) noexcept {
    if (std::max(p.a.x, p.b.x) < std::min(q.a.x, q.b.x) || std::max(q.a.x, q.b.x) < std::min(p.a.x, p.b.x) ||
        std::max(p.a.y, p.b.y) < std::min(q.a.y, q.b.y) || std::max(q.a.y, q.b.y) < std::min(p.a.y, p.b.y)) {
        return false;
    }
    const int o1 = orientation(p.a, p.b, q.a);
    const int o2 = orientation(p.a, p.b, q.b);
    const int o3 = orientation(q.a, q.b, p.a);
    const int o4 = orientation(q.a, q.b, p.b);
    if (o1 != o2 && o3 != o4) return true;
    return (o1 == 0 && onSegment(p.a, p.b, q.a)) || (o2 == 0 && onSegment(p.a, p.b, q.b)) ||
           (o3 == 0 && onSegment(q.a, q.b, p.a)) || (o4 == 0 && onSegment(q.a, q.b, p.b));
}

// True when the segments cross at one point interior to both.
bool segmentsCrossProperly(const Edge& p, const Edge& q) noexcept {
    return orientation(p.a, p.b, q.a) * orientation(p.a, p.b, q.b) < 0 &&
           orientation(q.a, q.b, p.a) * orientation(q.a, q.b, p.b) < 0;
}

enum class Location { Outside, Boundary, Inside };

// Locates the lattice point p against a polygon's closed region, by the even-odd rule.
Location locate(Point64 p, const std::vector<Edge>& edges) noexcept {
    bool inside = false;
    for (const Edge& e : edges) {
        if (onSegment(e.a, e.b, p)) return Location::Boundary;
        if ((e.a.y > p.y) != (e.b.y > p.y)) {
            // The edge spans the horizontal line through p; it crosses the ray towards +x
            // when p lies on the edge's left for an upward edge, its right for a downward one.
            const int side = orientation(e.a, e.b, p);
            if (e.b.y > e.a.y ? side > 0 : side < 0) inside = !inside;
        }
    }
    return inside ? Location::Inside : Location::Outside;
}

// Closed bounding boxes share a point.
bool boxesMeet(const Rect64& a, const Rect64& b) noexcept {
    return a.x <= b.right() && b.x <= a.right() && a.y <= b.bottom() && b.y <= a.bottom();
}

} // namespace

int orientation(Point64 a, Point64 b, Point64 c) noexcept {
    return signOfDifferenceOfProducts(difference(b.x, a.x), difference(c.y, a.y), difference(b.y, a.y),
                                      difference(c.x, a.x));
}

bool polygonsIntersect(const Polygon& a, const Polygon& b) {
    if (a.isEmpty() || b.isEmpty() || !boxesMeet(a.boundingBox(), b.boundingBox())) return false;
    const Rings ringsA = ringsOf(a);
    const Rings ringsB = ringsOf(b);
    for (const Edge& p : ringsA.edges) {
        for (const Edge& q : ringsB.edges) {
            if (segmentsMeet(p, q)) return true;
        }
    }
    // No boundaries meet, so each ring lies wholly inside or wholly outside the other
    // polygon: one vertex per ring decides.
    for (const Point64& v : ringsA.firstVertices) {
        if (locate(v, ringsB.edges) != Location::Outside) return true;
    }
    for (const Point64& v : ringsB.firstVertices) {
        if (locate(v, ringsA.edges) != Location::Outside) return true;
    }
    return false;
}

bool polygonContainedBy(const Polygon& inner, const Polygon& outer) {
    if (inner.isEmpty() || outer.isEmpty()) return false;
    const Rect64 innerBox = inner.boundingBox();
    const Rect64 outerBox = outer.boundingBox();
    if (innerBox.x < outerBox.x || innerBox.y < outerBox.y || innerBox.right() > outerBox.right() ||
        innerBox.bottom() > outerBox.bottom()) {
        return false;
    }
    const Rings ringsInner = ringsOf(inner);
    const Rings ringsOuter = ringsOf(outer);
    for (const Point64& v : ringsInner.vertices) {
        if (locate(v, ringsOuter.edges) == Location::Outside) return false;
    }
    for (const Edge& p : ringsInner.edges) {
        for (const Edge& q : ringsOuter.edges) {
            if (segmentsCrossProperly(p, q)) return false;
        }
    }
    for (const Point64& v : ringsOuter.vertices) {
        if (locate(v, ringsInner.edges) == Location::Inside) return false;
    }
    return true;
}

} // namespace slideio::processor::detail
