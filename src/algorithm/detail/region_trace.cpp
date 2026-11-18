// Requirements: UR-SEG-3, UR-SEG-4
#include "detail/region_trace.hpp"

#include <slideio/processor/core/error.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <unordered_map>

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

class Tracer {
public:
    Tracer(const int32_t* labels, int64_t width, int64_t height, int64_t stride, bool eightConnected)
        : labels_(labels), width_(width), height_(height), stride_(stride),
          // Left, straight, right with 8-connected pixels; right, straight, left without.
          turns_(eightConnected ? std::array<int, 3>{3, 0, 1} : std::array<int, 3>{1, 0, 3}),
          visited_(static_cast<size_t>(width * height), uint8_t{0}) {}

    std::vector<TracedRegion> trace() {
        std::vector<TracedRegion> regions;
        std::unordered_map<int32_t, size_t> indexOf;
        int32_t lastLabel = 0;
        size_t lastIndex = 0;
        for (int64_t y = 0; y < height_; ++y) {
            for (int64_t x = 0; x < width_; ++x) {
                const int32_t label = at(x, y);
                if (label <= 0) {
                    continue;
                }
                if (label != lastLabel || regions.empty()) {
                    const auto found = indexOf.find(label);
                    if (found == indexOf.end()) {
                        lastIndex = regions.size();
                        indexOf.emplace(label, lastIndex);
                        regions.emplace_back();
                        regions.back().label = label;
                    } else {
                        lastIndex = found->second;
                    }
                    lastLabel = label;
                }
                TracedRegion& region = regions[lastIndex];
                region.touchesEdge =
                    region.touchesEdge || x == 0 || y == 0 || x == width_ - 1 || y == height_ - 1;
                for (int side = 0; side < 4; ++side) {
                    const int64_t vx = x + kStartX[static_cast<size_t>(side)];
                    const int64_t vy = y + kStartY[static_cast<size_t>(side)];
                    if ((visited(x, y) & (1u << side)) == 0 && edgeLabel(vx, vy, side) == label) {
                        region.rings.push_back(walk(vx, vy, side, label));
                    }
                }
            }
        }
        for (TracedRegion& region : regions) {
            orderRings(region);
        }
        return regions;
    }

private:
    int32_t at(int64_t x, int64_t y) const {
        if (x < 0 || y < 0 || x >= width_ || y >= height_) {
            return 0;
        }
        return labels_[y * stride_ + x];
    }

    uint8_t& visited(int64_t x, int64_t y) { return visited_[static_cast<size_t>(y * width_ + x)]; }

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
    // startDir until that edge comes round again. The successor of an edge
    // depends only on the labels, so a start at a pinch closes correctly.
    std::vector<Point64> walk(int64_t startX, int64_t startY, int startDir, int32_t label) {
        std::vector<Point64> ring;
        int64_t x = startX;
        int64_t y = startY;
        int dir = startDir;
        // Every edge is walked at most once, so a longer walk means the edges do not close.
        const int64_t limit = 4 * width_ * height_ + 4;
        for (int64_t steps = 0;; ++steps) {
            if (steps > limit) {
                throw InternalError("traceRegions: a region's edges do not close into a ring");
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
                throw InternalError("traceRegions: a region's edges do not close into a ring");
            }
            if (next != dir) {
                ring.push_back(Point64{x, y});
            }
            if (x == startX && y == startY && next == startDir) {
                return ring;
            }
            dir = next;
        }
    }

    // Marks each ring's hole flag by its orientation and puts outer rings first,
    // keeping the order within each kind.
    static void orderRings(TracedRegion& region) {
        std::vector<std::vector<Point64>> outer;
        std::vector<std::vector<Point64>> holes;
        for (std::vector<Point64>& ring : region.rings) {
            int64_t twiceArea = 0;
            for (size_t i = 0; i < ring.size(); ++i) {
                const Point64& a = ring[i];
                const Point64& b = ring[(i + 1) % ring.size()];
                twiceArea += a.x * b.y - b.x * a.y;
            }
            (twiceArea > 0 ? outer : holes).push_back(std::move(ring));
        }
        region.rings = std::move(outer);
        region.isHole.assign(region.rings.size(), false);
        for (std::vector<Point64>& hole : holes) {
            region.rings.push_back(std::move(hole));
            region.isHole.push_back(true);
        }
    }

    const int32_t* labels_;
    int64_t width_;
    int64_t height_;
    int64_t stride_;
    std::array<int, 3> turns_;
    std::vector<uint8_t> visited_; // per pixel, bit d set once its side d has been walked
};

} // namespace

std::vector<TracedRegion> traceRegions(const int32_t* labels, int64_t width, int64_t height, int64_t stride,
                                       bool eightConnected) {
    if (width <= 0 || height <= 0) {
        return {};
    }
    return Tracer(labels, width, height, stride, eightConnected).trace();
}

} // namespace slideio::processor::detail
