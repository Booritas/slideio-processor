// Requirements: UR-SEG-3
#include <pipeline/detail/fragment_merge.hpp>

#include <slideio/processor/core/error.hpp>

#include <algorithm>
#include <cstddef>
#include <map>
#include <string>
#include <tuple>
#include <utility>

namespace slideio::processor::detail {

namespace {

// Vertices are keyed (y, x), so the first key of an ordered map is the
// topmost-then-leftmost vertex.
using VertexKey = std::pair<int64_t, int64_t>;
using EdgeKey = std::tuple<int64_t, int64_t, int64_t, int64_t>; // from (y, x), to (y, x)

VertexKey keyOf(Point64 p) { return {p.y, p.x}; }
Point64 pointOf(const VertexKey& k) { return Point64{k.second, k.first}; }

std::vector<Point64> pointsOf(const Polygon& polygon, int32_t ring) {
    std::vector<Point64> points(static_cast<size_t>(polygon.ringPointCount(ring)));
    polygon.ringPoints(ring, points.data(), static_cast<int64_t>(points.size()));
    return points;
}

// Twice the shoelace area in y-down coordinates: positive for a ring running
// clockwise on screen. Taken relative to the first vertex to keep the terms small.
int64_t twiceSignedArea(const std::vector<Point64>& ring) {
    int64_t total = 0;
    const Point64 o = ring.front();
    for (size_t i = 1; i + 1 < ring.size(); ++i) {
        const int64_t ax = ring[i].x - o.x;
        const int64_t ay = ring[i].y - o.y;
        const int64_t bx = ring[i + 1].x - o.x;
        const int64_t by = ring[i + 1].y - o.y;
        total += ax * by - bx * ay;
    }
    return total;
}

int64_t cross(Point64 a, Point64 b) { return a.x * b.y - a.y * b.x; }
int64_t dot(Point64 a, Point64 b) { return a.x * b.x + a.y * b.y; }

// Where an outgoing direction d ranks among the turns available after arriving
// along `in`, sharpest right turn first. Directions are swept from the reversed
// incoming direction anticlockwise on screen (y down, where a positive cross
// product turns clockwise): first the right turns, then straight on, then the
// left turns, and last the U-turn back along the incoming edge.
int sweepHalf(Point64 back, Point64 d) {
    const int64_t c = cross(back, d);
    if (c < 0) return 0;
    if (c > 0) return 2;
    return dot(back, d) < 0 ? 1 : 3;
}

// True when d1 comes before d2 in the sweep from `back`.
bool turnsSharperRight(Point64 back, Point64 d1, Point64 d2) {
    const int h1 = sweepHalf(back, d1);
    const int h2 = sweepHalf(back, d2);
    if (h1 != h2) {
        return h1 < h2;
    }
    // Within one half the sweep spans less than a half-turn: d1 is met first
    // when d2 lies anticlockwise (on screen) of it.
    return cross(d1, d2) < 0;
}

// True when d1 is the preferred turn over d2 after arriving along `-back`: the
// sharper right turn, or with `left` the sharper left one. A left turn is a
// right turn of the mirror image (y negated).
bool preferredTurn(Point64 back, Point64 d1, Point64 d2, bool left) {
    if (!left) {
        return turnsSharperRight(back, d1, d2);
    }
    const auto mirror = [](Point64 p) { return Point64{p.x, -p.y}; };
    return turnsSharperRight(mirror(back), mirror(d1), mirror(d2));
}

// Errors carry counts only: an object's coordinates could carry clinical data.
[[noreturn]] void fail(const std::string& what, const std::vector<const Polygon*>& fragments) {
    int64_t rings = 0;
    for (const Polygon* fragment : fragments) {
        rings += fragment == nullptr ? 0 : fragment->ringCount();
    }
    throw InternalError(("seam merge: " + what + " (" + std::to_string(fragments.size()) + " fragments, " +
                         std::to_string(rings) + " rings)")
                            .c_str());
}

// Appends the pieces of edge a -> b split at the vertices in `along` (the
// sorted coordinates of fragment vertices on the edge's line) strictly inside it.
void appendSplit(Point64 a, Point64 b, const std::map<int64_t, std::vector<int64_t>>& vertexLines, bool horizontal,
                 std::vector<std::pair<Point64, Point64>>& out) {
    const int64_t line = horizontal ? a.y : a.x;
    const int64_t from = horizontal ? a.x : a.y;
    const int64_t to = horizontal ? b.x : b.y;
    const auto at = [&](int64_t v) { return horizontal ? Point64{v, line} : Point64{line, v}; };
    const std::vector<int64_t>& along = vertexLines.at(line);
    Point64 previous = a;
    if (from < to) {
        for (auto it = std::upper_bound(along.begin(), along.end(), from); it != along.end() && *it < to; ++it) {
            out.emplace_back(previous, at(*it));
            previous = at(*it);
        }
    } else {
        auto it = std::lower_bound(along.begin(), along.end(), from);
        while (it != along.begin()) {
            --it;
            if (*it <= to) {
                break;
            }
            out.emplace_back(previous, at(*it));
            previous = at(*it);
        }
    }
    out.emplace_back(previous, b);
}

} // namespace

std::vector<SeamContact> seamContacts(const Polygon& polygon, const TileGrid& grid, int64_t tileIndex) {
    const Rect64 tile = grid.tileRect(tileIndex);
    const int64_t column = tileIndex % grid.columns();
    const int64_t row = tileIndex / grid.columns();

    // Intervals per seam line, vertical lines (x = line) first.
    std::map<std::pair<bool, int64_t>, std::vector<std::pair<int64_t, int64_t>>> lines;
    if (column > 0) lines[{false, tile.x}];
    if (column + 1 < grid.columns()) lines[{false, tile.right()}];
    if (row > 0) lines[{true, tile.y}];
    if (row + 1 < grid.rows()) lines[{true, tile.bottom()}];

    for (int32_t ring = 0; ring < polygon.ringCount(); ++ring) {
        const std::vector<Point64> points = pointsOf(polygon, ring);
        for (size_t i = 0; i < points.size(); ++i) {
            const Point64 a = points[i];
            const Point64 b = points[(i + 1) % points.size()];
            if (a.x == b.x && a.y != b.y) {
                auto it = lines.find({false, a.x});
                const int64_t lo = std::max(std::min(a.y, b.y), tile.y);
                const int64_t hi = std::min(std::max(a.y, b.y), tile.bottom());
                if (it != lines.end() && lo < hi) it->second.emplace_back(lo, hi);
            } else if (a.y == b.y && a.x != b.x) {
                auto it = lines.find({true, a.y});
                const int64_t lo = std::max(std::min(a.x, b.x), tile.x);
                const int64_t hi = std::min(std::max(a.x, b.x), tile.right());
                if (it != lines.end() && lo < hi) it->second.emplace_back(lo, hi);
            }
        }
    }

    std::vector<SeamContact> contacts;
    for (auto& [line, intervals] : lines) {
        std::sort(intervals.begin(), intervals.end());
        for (const std::pair<int64_t, int64_t>& interval : intervals) {
            SeamContact* last = contacts.empty() ? nullptr : &contacts.back();
            const bool vertical = !line.first;
            if (last != nullptr && last->vertical == vertical && last->line == line.second &&
                interval.first <= last->end) {
                last->end = std::max(last->end, interval.second);
            } else {
                contacts.push_back(SeamContact{vertical, line.second, interval.first, interval.second});
            }
        }
    }
    return contacts;
}

bool contactsJoin(const SeamContact& a, const SeamContact& b) {
    return a.vertical == b.vertical && a.line == b.line && std::min(a.end, b.end) > std::max(a.begin, b.begin);
}

std::vector<Point64> seamPoints(const Polygon& polygon, const TileGrid& grid, int64_t tileIndex) {
    const Rect64 tile = grid.tileRect(tileIndex);
    const int64_t column = tileIndex % grid.columns();
    const int64_t row = tileIndex / grid.columns();
    const auto onSeam = [&](Point64 p) {
        const bool alongX = p.x >= tile.x && p.x <= tile.right();
        const bool alongY = p.y >= tile.y && p.y <= tile.bottom();
        return (alongY && ((p.x == tile.x && column > 0) || (p.x == tile.right() && column + 1 < grid.columns()))) ||
               (alongX && ((p.y == tile.y && row > 0) || (p.y == tile.bottom() && row + 1 < grid.rows())));
    };
    std::vector<VertexKey> keys;
    for (int32_t ring = 0; ring < polygon.ringCount(); ++ring) {
        for (const Point64& p : pointsOf(polygon, ring)) {
            if (onSeam(p)) {
                keys.push_back(keyOf(p));
            }
        }
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    std::vector<Point64> points;
    points.reserve(keys.size());
    for (const VertexKey& key : keys) {
        points.push_back(pointOf(key));
    }
    return points;
}

Polygon mergeFragments(const std::vector<const Polygon*>& fragments, bool eightConnected) {
    // Every ring, oriented so the object lies on the right of each edge.
    std::vector<std::vector<Point64>> rings;
    for (const Polygon* fragment : fragments) {
        if (fragment == nullptr) {
            fail("a fragment is null", fragments);
        }
        for (int32_t ring = 0; ring < fragment->ringCount(); ++ring) {
            std::vector<Point64> points = pointsOf(*fragment, ring);
            const int64_t area = twiceSignedArea(points);
            if (fragment->ringIsHole(ring) ? area > 0 : area < 0) {
                std::reverse(points.begin(), points.end());
            }
            rings.push_back(std::move(points));
        }
    }
    if (rings.empty()) {
        fail("a component has no fragment rings", fragments);
    }

    // Fragment vertices by horizontal line (y -> xs) and by vertical line (x -> ys).
    std::map<int64_t, std::vector<int64_t>> byRow;
    std::map<int64_t, std::vector<int64_t>> byColumn;
    for (const std::vector<Point64>& ring : rings) {
        for (const Point64& p : ring) {
            byRow[p.y].push_back(p.x);
            byColumn[p.x].push_back(p.y);
        }
    }
    for (auto* lines : {&byRow, &byColumn}) {
        for (auto& entry : *lines) {
            std::sort(entry.second.begin(), entry.second.end());
            entry.second.erase(std::unique(entry.second.begin(), entry.second.end()), entry.second.end());
        }
    }

    // Split, then cancel each directed edge against an opposite twin.
    std::map<EdgeKey, int64_t> edges;
    std::vector<std::pair<Point64, Point64>> pieces;
    for (const std::vector<Point64>& ring : rings) {
        for (size_t i = 0; i < ring.size(); ++i) {
            const Point64 a = ring[i];
            const Point64 b = ring[(i + 1) % ring.size()];
            pieces.clear();
            if (a == b) {
                continue;
            }
            if (a.y == b.y) {
                appendSplit(a, b, byRow, true, pieces);
            } else if (a.x == b.x) {
                appendSplit(a, b, byColumn, false, pieces);
            } else {
                pieces.emplace_back(a, b);
            }
            for (const auto& [from, to] : pieces) {
                const auto twin = edges.find(EdgeKey{to.y, to.x, from.y, from.x});
                if (twin != edges.end()) {
                    if (--twin->second == 0) {
                        edges.erase(twin);
                    }
                } else {
                    ++edges[EdgeKey{from.y, from.x, to.y, to.x}];
                }
            }
        }
    }

    std::map<VertexKey, std::vector<Point64>> outgoing;
    for (const auto& [edge, count] : edges) {
        if (count != 1) {
            fail("fragments overlap", fragments);
        }
        outgoing[{std::get<0>(edge), std::get<1>(edge)}].push_back(Point64{std::get<3>(edge), std::get<2>(edge)});
    }

    // Link the surviving edges into rings, starting each at the topmost-then-
    // leftmost remaining vertex and taking the preferred turn at each vertex.
    // A ring closes when, back at its start, its first edge is the preferred turn.
    std::vector<std::vector<Point64>> outer;
    std::vector<std::vector<Point64>> holes;
    while (!outgoing.empty()) {
        const auto startEntry = outgoing.begin();
        const Point64 start = pointOf(startEntry->first);
        std::vector<Point64>& firstChoices = startEntry->second;
        const Point64 firstTo = firstChoices.front();
        firstChoices.erase(firstChoices.begin());
        if (firstChoices.empty()) {
            outgoing.erase(startEntry);
        }
        const Point64 firstDirection{firstTo.x - start.x, firstTo.y - start.y};

        std::vector<Point64> ring = {start};
        Point64 v = firstTo;
        Point64 in = firstDirection;
        for (;;) {
            const Point64 back{-in.x, -in.y};
            const auto here = outgoing.find(keyOf(v));
            const bool canClose = v == start;
            bool chosen = false;
            size_t best = 0;
            Point64 bestDirection{0, 0};
            if (here != outgoing.end()) {
                for (size_t i = 0; i < here->second.size(); ++i) {
                    const Point64 d{here->second[i].x - v.x, here->second[i].y - v.y};
                    if (!chosen || preferredTurn(back, d, bestDirection, eightConnected)) {
                        chosen = true;
                        best = i;
                        bestDirection = d;
                    }
                }
            }
            if (canClose && (!chosen || preferredTurn(back, firstDirection, bestDirection, eightConnected))) {
                break;
            }
            if (!chosen) {
                fail("the fragment edges do not close into rings", fragments);
            }
            ring.push_back(v);
            const Point64 next = here->second[best];
            here->second.erase(here->second.begin() + static_cast<std::ptrdiff_t>(best));
            if (here->second.empty()) {
                outgoing.erase(here);
            }
            in = bestDirection;
            v = next;
        }

        const int64_t area = ring.size() < 3 ? 0 : twiceSignedArea(ring);
        if (area == 0) {
            fail("a merged ring has zero area", fragments);
        }
        (area > 0 ? outer : holes).push_back(std::move(ring));
    }
    if (outer.size() != 1) {
        fail("the fragments form " + std::to_string(outer.size()) + " outer rings instead of one", fragments);
    }

    Polygon merged;
    merged.addRing(outer.front().data(), static_cast<int64_t>(outer.front().size()), false);
    for (const std::vector<Point64>& hole : holes) {
        merged.addRing(hole.data(), static_cast<int64_t>(hole.size()), true);
    }
    normaliseRings(merged);
    return merged;
}

std::vector<Point64> normaliseRing(const std::vector<Point64>& ring) {
    std::vector<Point64> kept;
    const size_t n = ring.size();
    for (size_t i = 0; i < n; ++i) {
        const Point64& prev = ring[(i + n - 1) % n];
        const Point64& cur = ring[i];
        const Point64& next = ring[(i + 1) % n];
        const int64_t turn = (cur.x - prev.x) * (next.y - cur.y) - (cur.y - prev.y) * (next.x - cur.x);
        if (turn != 0) {
            kept.push_back(cur);
        }
    }
    const auto first = std::min_element(kept.begin(), kept.end(), [](const Point64& a, const Point64& b) {
        return a.y != b.y ? a.y < b.y : a.x < b.x;
    });
    std::rotate(kept.begin(), first, kept.end());
    return kept;
}

namespace {

bool vertexBefore(const Point64& a, const Point64& b) { return a.y != b.y ? a.y < b.y : a.x < b.x; }

int64_t turnAt(const Point64& prev, const Point64& cur, const Point64& next) {
    return (cur.x - prev.x) * (next.y - cur.y) - (cur.y - prev.y) * (next.x - cur.x);
}

// Whether a ring is as normaliseRing() leaves it: a turn at every vertex and
// its first vertex the first topmost-then-leftmost one.
bool ringIsNormal(const Point64* ring, size_t n) {
    if (n < 3) {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        if (turnAt(ring[(i + n - 1) % n], ring[i], ring[(i + 1) % n]) == 0) {
            return false;
        }
        if (i > 0 && vertexBefore(ring[i], ring[0])) {
            return false;
        }
    }
    return true;
}

} // namespace

bool ringsAreNormalised(const Polygon& polygon) {
    std::vector<Point64> ring;
    bool sawHole = false;
    Point64 lastHole{0, 0};
    for (int32_t r = 0; r < polygon.ringCount(); ++r) {
        ring.resize(static_cast<size_t>(polygon.ringPointCount(r)));
        polygon.ringPoints(r, ring.data(), static_cast<int64_t>(ring.size()));
        if (!ringIsNormal(ring.data(), ring.size())) {
            return false;
        }
        if (!polygon.ringIsHole(r)) {
            if (sawHole) {
                return false;
            }
            continue;
        }
        if (sawHole && vertexBefore(ring.front(), lastHole)) {
            return false;
        }
        sawHole = true;
        lastHole = ring.front();
    }
    return true;
}

void normaliseRings(Polygon& polygon) {
    if (ringsAreNormalised(polygon)) {
        return;
    }
    // Every ring normalised into one buffer, then added outer rings first and
    // holes ordered by first vertex, as normaliseRing() and a stable sort would.
    struct Span {
        size_t first;
        size_t count;
        bool isHole;
    };
    int64_t total = 0;
    for (int32_t r = 0; r < polygon.ringCount(); ++r) {
        total += polygon.ringPointCount(r);
    }
    std::vector<Point64> points;
    points.reserve(static_cast<size_t>(total));
    std::vector<Span> spans;
    spans.reserve(static_cast<size_t>(polygon.ringCount()));
    std::vector<Point64> ring;
    for (int32_t r = 0; r < polygon.ringCount(); ++r) {
        ring.resize(static_cast<size_t>(polygon.ringPointCount(r)));
        polygon.ringPoints(r, ring.data(), static_cast<int64_t>(ring.size()));
        const size_t first = points.size();
        const size_t n = ring.size();
        for (size_t i = 0; i < n; ++i) {
            if (turnAt(ring[(i + n - 1) % n], ring[i], ring[(i + 1) % n]) != 0) {
                points.push_back(ring[i]);
            }
        }
        const auto begin = points.begin() + static_cast<std::ptrdiff_t>(first);
        std::rotate(begin, std::min_element(begin, points.end(), vertexBefore), points.end());
        spans.push_back(Span{first, points.size() - first, polygon.ringIsHole(r)});
    }
    std::vector<Point64>().swap(ring);
    std::stable_partition(spans.begin(), spans.end(), [](const Span& s) { return !s.isHole; });
    const auto holes = std::find_if(spans.begin(), spans.end(), [](const Span& s) { return s.isHole; });
    std::stable_sort(holes, spans.end(), [&points](const Span& a, const Span& b) {
        const Point64 pa = a.count == 0 ? Point64{0, 0} : points[a.first];
        const Point64 pb = b.count == 0 ? Point64{0, 0} : points[b.first];
        return vertexBefore(pa, pb);
    });
    Polygon normalised;
    for (const Span& s : spans) {
        normalised.addRing(points.data() + s.first, static_cast<int64_t>(s.count), s.isHole);
    }
    polygon = std::move(normalised);
}

} // namespace slideio::processor::detail
