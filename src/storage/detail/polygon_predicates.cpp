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

// A signed integer of up to 256 bits, as sign and magnitude (four 64-bit limbs, least
// significant first). It holds doubled coordinates (up to 2^65), their differences and
// the cross products of those differences (under 2^133) exactly.
struct Wide {
    bool negative = false;
    uint64_t limb[4] = {0, 0, 0, 0};
};

bool isZero(const Wide& w) noexcept { return (w.limb[0] | w.limb[1] | w.limb[2] | w.limb[3]) == 0; }

Wide wideOf(int64_t v) noexcept {
    Wide w;
    w.negative = v < 0;
    w.limb[0] = v < 0 ? 0 - static_cast<uint64_t>(v) : static_cast<uint64_t>(v);
    return w;
}

int compareMagnitudes(const Wide& a, const Wide& b) noexcept {
    for (int i = 3; i >= 0; --i) {
        if (a.limb[i] != b.limb[i]) return a.limb[i] < b.limb[i] ? -1 : 1;
    }
    return 0;
}

Wide add(const Wide& a, const Wide& b) noexcept {
    Wide r;
    if (a.negative == b.negative) {
        uint64_t carry = 0;
        for (int i = 0; i < 4; ++i) {
            const uint64_t s = a.limb[i] + carry;
            const uint64_t c1 = s < carry ? 1 : 0;
            r.limb[i] = s + b.limb[i];
            carry = c1 + (r.limb[i] < s ? 1 : 0);
        }
        r.negative = a.negative;
    } else {
        const bool aLarger = compareMagnitudes(a, b) >= 0;
        const Wide& big = aLarger ? a : b;
        const Wide& small = aLarger ? b : a;
        uint64_t borrow = 0;
        for (int i = 0; i < 4; ++i) {
            const uint64_t d = big.limb[i] - small.limb[i];
            const uint64_t b1 = big.limb[i] < small.limb[i] ? 1 : 0;
            r.limb[i] = d - borrow;
            borrow = b1 + (d < borrow ? 1 : 0);
        }
        r.negative = big.negative;
    }
    if (isZero(r)) r.negative = false;
    return r;
}

Wide negate(Wide w) noexcept {
    if (!isZero(w)) w.negative = !w.negative;
    return w;
}

Wide subtract(const Wide& a, const Wide& b) noexcept { return add(a, negate(b)); }

Wide multiply(const Wide& a, const Wide& b) noexcept {
    Wide r;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; i + j < 4; ++j) {
            if (a.limb[i] == 0 || b.limb[j] == 0) continue;
            const Unsigned128 p = multiply(a.limb[i], b.limb[j]);
            // Adds p at limb i + j, propagating the carry; the operands here never overflow 256 bits.
            uint64_t carry = p.low;
            uint64_t next = p.high;
            for (int k = i + j; k < 4 && (carry != 0 || next != 0); ++k) {
                const uint64_t s = r.limb[k] + carry;
                const uint64_t overflow = s < carry ? 1 : 0;
                r.limb[k] = s;
                carry = next + overflow;
                next = carry < overflow ? 1 : 0;
            }
        }
    }
    r.negative = !isZero(r) && (a.negative != b.negative);
    return r;
}

int signOf(const Wide& w) noexcept { return isZero(w) ? 0 : (w.negative ? -1 : 1); }

// The sign of the cross product (ux, uy) x (vx, vy).
int crossSign(const Wide& ux, const Wide& uy, const Wide& vx, const Wide& vy) noexcept {
    return signOf(subtract(multiply(ux, vy), multiply(uy, vx)));
}

// A point in doubled coordinates, so the midpoint of two lattice points is exact.
struct Doubled {
    Wide x;
    Wide y;
};

Doubled doubled(Point64 p) noexcept { return {add(wideOf(p.x), wideOf(p.x)), add(wideOf(p.y), wideOf(p.y))}; }

Doubled midpointDoubled(Point64 p, Point64 q) noexcept { return {add(wideOf(p.x), wideOf(q.x)), add(wideOf(p.y), wideOf(q.y))}; }

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

// Whether the point m + eps * d, for every small enough eps > 0, lies inside a polygon given by
// its edges: the even-odd parity of the edges the open ray from m along d crosses. m is
// in doubled coordinates and d is any non-zero integer vector. An edge is counted when
// its ends lie on different sides of the ray's line (half-open: on the line counts as
// the non-positive side, a consistent infinitesimal shift) and the line it lies on meets
// the ray at t > 0. An edge through m itself meets it at t = 0 and is not counted: the
// side point lies off it.
bool insideBeside(const std::vector<Edge>& edges, const Doubled& m, const Wide& dx, const Wide& dy) {
    bool inside = false;
    for (const Edge& e : edges) {
        const Doubled a = doubled(e.a);
        const Doubled b = doubled(e.b);
        const Wide ax = subtract(a.x, m.x), ay = subtract(a.y, m.y);
        const Wide bx = subtract(b.x, m.x), by = subtract(b.y, m.y);
        if ((crossSign(dx, dy, ax, ay) > 0) == (crossSign(dx, dy, bx, by) > 0)) continue;
        // The lines meet at m + t d with t = cross(a - m, b - a) / cross(d, b - a).
        const Wide ex = subtract(bx, ax), ey = subtract(by, ay);
        const int numerator = crossSign(ax, ay, ex, ey);
        if (numerator != 0 && numerator == crossSign(dx, dy, ex, ey)) inside = !inside;
    }
    return inside;
}

// The exact containment test for touching boundaries. Every inner edge is cut at the
// outer vertices on it, and every outer edge at the inner vertices on it; with no proper
// crossing (checked first), the open pieces between cuts meet the other boundary nowhere,
// or lie along it. For each piece and each side of it, a point just beside the piece's
// midpoint that is inside inner must be inside outer.
//
// Sufficient: were some point of inner's interior outside outer's closure, the region of
// such points would be bounded by pieces of the two boundaries. A bounding inner piece has
// the region on its inner side, outside outer; a bounding outer piece has it on its outer
// exterior side, inside inner. Either way the check fails for that piece. Necessary: when
// inner lies in outer, a point just beside a piece that is inside inner is off both
// boundaries (no boundary meets the open piece transversally), so it is inside outer.
bool piecesStayInside(const Rings& inner, const Rings& outer) {
    const auto piecesHold = [&](const Edge& edge, const std::vector<Point64>& cuts) {
        std::vector<Point64> points{edge.a, edge.b};
        for (const Point64& cut : cuts) {
            if (onSegment(edge.a, edge.b, cut)) points.push_back(cut);
        }
        // Collinear lattice points order by x along a non-vertical edge, else by y.
        const bool byX = edge.a.x != edge.b.x;
        std::sort(points.begin(), points.end(),
                  [byX](Point64 p, Point64 q) { return byX ? p.x < q.x : p.y < q.y; });
        points.erase(std::unique(points.begin(), points.end()), points.end());
        // A normal of the edge, and so of every piece.
        const Wide nx = subtract(wideOf(edge.a.y), wideOf(edge.b.y));
        const Wide ny = subtract(wideOf(edge.b.x), wideOf(edge.a.x));
        for (size_t i = 0; i + 1 < points.size(); ++i) {
            const Doubled m = midpointDoubled(points[i], points[i + 1]);
            for (const bool flip : {false, true}) {
                const Wide dx = flip ? negate(nx) : nx;
                const Wide dy = flip ? negate(ny) : ny;
                if (insideBeside(inner.edges, m, dx, dy) && !insideBeside(outer.edges, m, dx, dy)) return false;
            }
        }
        return true;
    };
    for (const Edge& edge : inner.edges) {
        if (!piecesHold(edge, outer.vertices)) return false;
    }
    for (const Edge& edge : outer.edges) {
        if (!piecesHold(edge, inner.vertices)) return false;
    }
    return true;
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
    bool contact = false;  // the boundaries touch: a vertex of one lies on the other's boundary
    for (const Point64& v : ringsInner.vertices) {
        const Location where = locate(v, ringsOuter.edges);
        if (where == Location::Outside) return false;
        if (where == Location::Boundary) contact = true;
    }
    for (const Edge& p : ringsInner.edges) {
        for (const Edge& q : ringsOuter.edges) {
            if (segmentsCrossProperly(p, q)) return false;
        }
    }
    for (const Point64& v : ringsOuter.vertices) {
        const Location where = locate(v, ringsInner.edges);
        if (where == Location::Inside) return false;
        if (where == Location::Boundary) contact = true;
    }
    // Disjoint boundaries: every inner ring lies in outer's interior and no outer ring
    // crosses inner's interior, so each region of inner is inside outer. Touching ones
    // need the piece test, since inner can escape through an outer vertex or along a
    // shared edge.
    return !contact || piecesStayInside(ringsInner, ringsOuter);
}

} // namespace slideio::processor::detail
