// Requirements: UR-SEG-3, UR-SEG-4
#include "detail/region_trace.hpp"

#include <slideio/processor/core/error.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>

namespace slideio::processor::detail {

namespace {

// Directions in y-down coordinates: east, south, west, north. Turning right is
// +1. An edge leaving a vertex in direction d is side d of the pixel on its
// right (0 top, 1 right, 2 bottom, 3 left), so each edge belongs to one pixel.
constexpr std::array<int64_t, 4> kDx = {1, 0, -1, 0};
constexpr std::array<int64_t, 4> kDy = {0, 1, 0, -1};
// The pixel on the right of the edge leaving vertex (x, y) in direction d, as
// an offset from the vertex.
constexpr std::array<int64_t, 4> kRightX = {0, -1, -1, 0};
constexpr std::array<int64_t, 4> kRightY = {0, 0, -1, -1};
// The vertex side d of a pixel starts from, as an offset from the pixel.
constexpr std::array<int64_t, 4> kStartX = {0, 1, 1, 0};
constexpr std::array<int64_t, 4> kStartY = {0, 0, 1, 1};

bool vertexBefore(const Point64& a, const Point64& b) { return a.y != b.y ? a.y < b.y : a.x < b.x; }

[[noreturn]] void notClosed() {
    throw InternalError("traceRegions: a region's edges do not close into a ring");
}

[[noreturn]] void miscounted() {
    throw InternalError("traceRegions: the rings have more vertices than were counted");
}

class Tracer {
public:
    Tracer(const int32_t* labels, int64_t width, int64_t height, int64_t stride, bool eightConnected,
           TracedRegions& out)
        : labels_(labels), width_(width), height_(height), stride_(stride),
          // Left, straight, right with 8-connected pixels; right, straight, left without.
          turns_(eightConnected ? std::array<int, 3>{3, 0, 1} : std::array<int, 3>{1, 0, 3}), out_(out) {}

    void trace() {
        indexRegions();
        reserveVertices();
        visited_.assign(static_cast<size_t>(width_ * height_), uint8_t{0});
        for (int64_t y = 0; y < height_; ++y) {
            for (int64_t x = 0; x < width_; ++x) {
                const int32_t label = at(x, y);
                if (label <= 0) {
                    continue;
                }
                for (int side = 0; side < 4; ++side) {
                    const int64_t vx = x + kStartX[static_cast<size_t>(side)];
                    const int64_t vy = y + kStartY[static_cast<size_t>(side)];
                    if ((visited(x, y) & (1u << side)) == 0 && edgeLabel(vx, vy, side) == label) {
                        walk(vx, vy, side, label);
                    }
                }
            }
        }
        std::vector<uint8_t>().swap(visited_);
        orderRings();
    }

private:
    int32_t at(int64_t x, int64_t y) const {
        if (x < 0 || y < 0 || x >= width_ || y >= height_) {
            return 0;
        }
        return labels_[y * stride_ + x];
    }

    uint8_t& visited(int64_t x, int64_t y) { return visited_[static_cast<size_t>(y * width_ + x)]; }

    // Indexes the regions by label, in the order their first pixel is met.
    void indexRegions() {
        int32_t largest = 0;
        for (int64_t y = 0; y < height_; ++y) {
            for (int64_t x = 0; x < width_; ++x) {
                largest = std::max(largest, at(x, y));
            }
        }
        if (largest > width_ * height_) {
            throw InternalError("traceRegions: a label exceeds the buffer's pixel count");
        }
        out_.regionOfLabel.assign(static_cast<size_t>(largest) + 1, -1);
        int32_t count = 0;
        for (int64_t y = 0; y < height_; ++y) {
            for (int64_t x = 0; x < width_; ++x) {
                const int32_t label = at(x, y);
                if (label > 0 && out_.regionOfLabel[static_cast<size_t>(label)] < 0) {
                    out_.regionOfLabel[static_cast<size_t>(label)] = count++;
                }
            }
        }
        out_.regions.resize(static_cast<size_t>(count));
        for (int64_t y = 0; y < height_; ++y) {
            for (int64_t x = 0; x < width_; ++x) {
                const int32_t label = at(x, y);
                if (label <= 0) {
                    continue;
                }
                TracedRegion& region = out_.regions[static_cast<size_t>(out_.regionOfLabel[static_cast<size_t>(label)])];
                region.label = label;
                region.touchesEdge =
                    region.touchesEdge || x == 0 || y == 0 || x == width_ - 1 || y == height_ - 1;
            }
        }
    }

    // Counts the vertices every ring will have, from the 2 x 2 pixels around
    // each pixel corner: a label on one or three of them turns there once, a
    // label on two diagonal ones twice (two rings, or one ring passing twice),
    // and a label on two side by side or on all four not at all. Every ring
    // has at least four vertices, which bounds the rings.
    void reserveVertices() {
        int64_t vertices = 0;
        for (int64_t y = 0; y <= height_; ++y) {
            for (int64_t x = 0; x <= width_; ++x) {
                const std::array<int32_t, 4> around = {at(x - 1, y - 1), at(x, y - 1), at(x - 1, y), at(x, y)};
                for (size_t i = 0; i < 4; ++i) {
                    const int32_t label = around[i];
                    bool seen = label <= 0;
                    for (size_t j = 0; j < i && !seen; ++j) {
                        seen = around[j] == label;
                    }
                    if (seen) {
                        continue;
                    }
                    const bool topLeft = around[0] == label;
                    const bool topRight = around[1] == label;
                    const bool bottomLeft = around[2] == label;
                    const bool bottomRight = around[3] == label;
                    const int n = int(topLeft) + int(topRight) + int(bottomLeft) + int(bottomRight);
                    if (n == 1 || n == 3) {
                        vertices += 1;
                    } else if (n == 2 && topLeft == bottomRight) {
                        vertices += 2;
                    }
                }
            }
        }
        if (vertices > std::numeric_limits<int32_t>::max()) {
            throw InternalError("traceRegions: the rings have too many vertices");
        }
        out_.points.reserve(static_cast<size_t>(vertices));
        out_.rings.reserve(static_cast<size_t>(vertices / 4));
    }

    // The label of the region owning the edge leaving vertex (x, y) in
    // direction d, or 0 when there is no such edge.
    int32_t edgeLabel(int64_t x, int64_t y, int d) const {
        const int64_t px = x + kRightX[static_cast<size_t>(d)];
        const int64_t py = y + kRightY[static_cast<size_t>(d)];
        const int32_t label = at(px, py);
        if (label <= 0) {
            return 0;
        }
        // Across side d lies the neighbour in direction d - 1 (top for east, ...).
        const size_t across = static_cast<size_t>((d + 3) % 4);
        return at(px + kDx[across], py + kDy[across]) == label ? 0 : label;
    }

    // Follows the ring through the edge leaving (startX, startY) in direction
    // startDir until that edge comes round again, appending its vertices to the
    // shared points. The successor of an edge depends only on the labels, so a
    // start at a pinch closes correctly.
    void walk(int64_t startX, int64_t startY, int startDir, int32_t label) {
        std::vector<Point64>& points = out_.points;
        const size_t first = points.size();
        int64_t x = startX;
        int64_t y = startY;
        int dir = startDir;
        // Every edge is walked at most once, so a longer walk means the edges do not close.
        const int64_t limit = 4 * width_ * height_ + 4;
        for (int64_t steps = 0;; ++steps) {
            if (steps > limit) {
                notClosed();
            }
            const int64_t px = x + kRightX[static_cast<size_t>(dir)];
            const int64_t py = y + kRightY[static_cast<size_t>(dir)];
            visited(px, py) = static_cast<uint8_t>(visited(px, py) | (1u << dir));
            x += kDx[static_cast<size_t>(dir)];
            y += kDy[static_cast<size_t>(dir)];
            int next = -1;
            for (int turn : turns_) {
                const int candidate = (dir + turn) % 4;
                if (edgeLabel(x, y, candidate) == label) {
                    next = candidate;
                    break;
                }
            }
            if (next < 0) {
                notClosed();
            }
            if (next != dir) {
                // The vertices were counted beforehand, so this never reallocates.
                if (points.size() == points.capacity()) {
                    miscounted();
                }
                points.push_back(Point64{x, y});
            }
            if (x == startX && y == startY && next == startDir) {
                break;
            }
            dir = next;
        }
        // Start at the first topmost-then-leftmost vertex, the form the run
        // normalises rings to, so an object goes on without being rebuilt.
        const auto begin = points.begin() + static_cast<std::ptrdiff_t>(first);
        std::rotate(begin, std::min_element(begin, points.end(), vertexBefore), points.end());
        int64_t twiceArea = 0;
        const size_t n = points.size() - first;
        for (size_t i = 0; i < n; ++i) {
            const Point64& a = points[first + i];
            const Point64& b = points[first + (i + 1) % n];
            twiceArea += a.x * b.y - b.x * a.y;
        }
        if (out_.rings.size() == out_.rings.capacity()) {
            miscounted();
        }
        TracedRing ring;
        ring.first = static_cast<int64_t>(first);
        ring.count = static_cast<int32_t>(n);
        ring.region = out_.regionOfLabel[static_cast<size_t>(label)];
        ring.isHole = twiceArea <= 0;
        out_.rings.push_back(ring);
        ++out_.regions[static_cast<size_t>(ring.region)].ringCount;
    }

    // Groups the rings by region: outer rings first, in the order they are met,
    // then holes ordered by first vertex (y, then x), ties in the order met; the
    // order the run normalises rings to.
    void orderRings() {
        int32_t next = 0;
        for (TracedRegion& region : out_.regions) {
            region.firstRing = next;
            next += region.ringCount;
        }
        out_.ringOrder.assign(out_.rings.size(), 0);
        std::vector<int32_t> placed(out_.regions.size(), 0);
        for (const bool holes : {false, true}) {
            for (size_t r = 0; r < out_.rings.size(); ++r) {
                const TracedRing& ring = out_.rings[r];
                if (ring.isHole != holes) {
                    continue;
                }
                const auto region = static_cast<size_t>(ring.region);
                out_.ringOrder[static_cast<size_t>(out_.regions[region].firstRing + placed[region]++)] =
                    static_cast<int32_t>(r);
            }
        }
        const auto holeBefore = [this](int32_t a, int32_t b) {
            const Point64& pa = out_.points[static_cast<size_t>(out_.rings[static_cast<size_t>(a)].first)];
            const Point64& pb = out_.points[static_cast<size_t>(out_.rings[static_cast<size_t>(b)].first)];
            return vertexBefore(pa, pb) || (pa == pb && a < b);
        };
        for (const TracedRegion& region : out_.regions) {
            const auto begin = out_.ringOrder.begin() + region.firstRing;
            const auto end = begin + region.ringCount;
            const auto holes = std::find_if(begin, end, [this](int32_t ring) {
                return out_.rings[static_cast<size_t>(ring)].isHole;
            });
            std::sort(holes, end, holeBefore);
        }
    }

    const int32_t* labels_;
    int64_t width_;
    int64_t height_;
    int64_t stride_;
    std::array<int, 3> turns_;
    TracedRegions& out_;
    std::vector<uint8_t> visited_; // per pixel, bit d set once its side d has been walked
};

} // namespace

TracedRegions traceRegions(const int32_t* labels, int64_t width, int64_t height, int64_t stride,
                           bool eightConnected) {
    TracedRegions out;
    if (width <= 0 || height <= 0) {
        return out;
    }
    Tracer(labels, width, height, stride, eightConnected, out).trace();
    return out;
}

} // namespace slideio::processor::detail
