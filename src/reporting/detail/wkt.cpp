// Requirements: UR-RES-9
#include "reporting/detail/wkt.hpp"

#include <slideio/processor/core/geometry.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace slideio::processor::detail {
namespace {

using Ring = std::vector<Point64>;

Ring ringOf(const Polygon& polygon, int32_t ring) {
    Ring points(static_cast<size_t>(polygon.ringPointCount(ring)));
    polygon.ringPoints(ring, points.data(), static_cast<int64_t>(points.size()));
    return points;
}

// Even-odd test of p against one ring alone. Coordinates are below 2^32, so the
// crossing abscissa is compared in double; a vertex exactly on the ring is a
// degenerate hole and may fall either way.
bool ringContains(const Ring& ring, Point64 p) {
    bool inside = false;
    const size_t n = ring.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const Point64 a = ring[i];
        const Point64 b = ring[j];
        if ((a.y > p.y) != (b.y > p.y)) {
            const double x = static_cast<double>(a.x) +
                             static_cast<double>(p.y - a.y) * static_cast<double>(b.x - a.x) /
                                 static_cast<double>(b.y - a.y);
            if (static_cast<double>(p.x) < x) inside = !inside;
        }
    }
    return inside;
}

void appendRing(std::string& out, const Ring& ring) {
    out += '(';
    for (size_t i = 0; i <= ring.size(); ++i) {
        const Point64 p = ring[i % ring.size()];
        if (i > 0) out += ',';
        out += std::to_string(p.x);
        out += ' ';
        out += std::to_string(p.y);
    }
    out += ')';
}

} // namespace

void appendWkt(std::string& out, const Polygon& polygon) {
    const int32_t count = polygon.ringCount();
    if (count == 0) {
        out += "POLYGON EMPTY";
        return;
    }
    std::vector<Ring> rings;
    std::vector<int32_t> outers;
    rings.reserve(static_cast<size_t>(count));
    for (int32_t r = 0; r < count; ++r) {
        rings.push_back(ringOf(polygon, r));
        if (!polygon.ringIsHole(r)) outers.push_back(r);
    }
    // holesOf[k]: the holes of outers[k], in ring order.
    std::vector<std::vector<int32_t>> holesOf(outers.size());
    for (int32_t r = 0; r < count; ++r) {
        if (!polygon.ringIsHole(r)) continue;
        const Point64 first = rings[static_cast<size_t>(r)].front();
        size_t owner = outers.size();
        for (size_t k = 0; k < outers.size(); ++k) {
            if (ringContains(rings[static_cast<size_t>(outers[k])], first)) {
                owner = k;
                break;
            }
        }
        if (owner == outers.size()) {
            // Inside no outer ring: the nearest outer ring before it (ring 0 is outer).
            owner = 0;
            for (size_t k = 0; k < outers.size() && outers[k] < r; ++k) owner = k;
        }
        holesOf[owner].push_back(r);
    }

    auto appendPart = [&](size_t k) {
        out += '(';
        appendRing(out, rings[static_cast<size_t>(outers[k])]);
        for (const int32_t hole : holesOf[k]) {
            out += ',';
            appendRing(out, rings[static_cast<size_t>(hole)]);
        }
        out += ')';
    };
    if (outers.size() == 1) {
        out += "POLYGON";
        appendPart(0);
        return;
    }
    out += "MULTIPOLYGON(";
    for (size_t k = 0; k < outers.size(); ++k) {
        if (k > 0) out += ',';
        appendPart(k);
    }
    out += ')';
}

} // namespace slideio::processor::detail
