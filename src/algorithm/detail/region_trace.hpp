// Requirements: UR-SEG-3, UR-SEG-4
#pragma once

#include <slideio/processor/core/geometry.hpp>

#include <cstdint>
#include <vector>

namespace slideio::processor::detail {

/// One traced ring: the vertices points[first .. first + count) of its
/// TracedRegions, in pixel-corner coordinates relative to the buffer (pixel
/// (x, y) spans x..x+1, y..y+1), listing only the vertices where the ring turns
/// (a vertex where a ring touches itself appears twice).
struct TracedRing {
    int64_t first = 0;
    int32_t count = 0;
    /// The index of the ring's region in TracedRegions::regions.
    int32_t region = 0;
    bool isHole = false;
};

/// One labelled region.
struct TracedRegion {
    /// The region's label in the traced buffer; positive.
    int32_t label = 0;
    /// The region's rings are rings[ringOrder[firstRing .. firstRing + ringCount)],
    /// outer rings first, each kind in the order its rings are met.
    int32_t firstRing = 0;
    int32_t ringCount = 0;
    /// True when a pixel of the region lies on the buffer's first or last row
    /// or column.
    bool touchesEdge = false;
};

/// Every region of a label buffer and their rings. Each array is allocated
/// once at its final size (points and rings at an upper bound counted before
/// tracing), so none holds an old and a new block while growing.
struct TracedRegions {
    /// In the order their first pixel is met in row-major order.
    std::vector<TracedRegion> regions;
    /// Every ring's vertices, ring after ring.
    std::vector<Point64> points;
    /// In the order the rings are met.
    std::vector<TracedRing> rings;
    /// Ring indices grouped by region; see TracedRegion::firstRing.
    std::vector<int32_t> ringOrder;
    /// For each label from 0 to the largest one, the index of its region in
    /// regions, or -1 when no pixel has it.
    std::vector<int32_t> regionOfLabel;

    /// The k-th ring of region @p region, outer rings first.
    const TracedRing& ring(int32_t region, int32_t k) const {
        return rings[static_cast<size_t>(
            ringOrder[static_cast<size_t>(regions[static_cast<size_t>(region)].firstRing + k)])];
    }
    /// The first vertex of @p ring.
    const Point64* ringPoints(const TracedRing& ring) const { return points.data() + ring.first; }
};

/// Traces every region of a label buffer: width x height int32 labels, row y
/// starting at labels + y * stride. Each positive label is one region; 0 and
/// negative labels are background. Labels are at most width * height (they are
/// numbered within the buffer, as 1..K), since regions are indexed by label.
/// The caller labels so that each region is connected under the chosen
/// connectivity, which is what makes it one outer ring. Pixels beyond the
/// buffer are background.
///
/// Every boundary edge keeps its region on its right in y-down coordinates, so
/// outer rings have positive shoelace area (clockwise on screen) and holes
/// negative. Where a region touches itself diagonally at a vertex, the walk
/// takes the sharpest left turn with @p eightConnected (8-connected pixels:
/// one outer ring through the point twice, as the seam merger links it) and
/// the sharpest right turn otherwise (4-connected pixels: the rings stay
/// apart). Throws InternalError when a label exceeds width * height or the
/// edges of a region do not close, which valid input never causes.
///
/// Memory: the result holds 16 bytes per vertex, 24 per ring (at most a
/// quarter of the vertices), 16 per region and 4 per label; tracing adds one
/// byte per pixel and 4 bytes per region, freed on return. The vertices are
/// counted exactly before tracing: at a pixel corner, a label on one or three
/// of the four pixels around it turns once, a label on two diagonal ones twice.
TracedRegions traceRegions(const int32_t* labels, int64_t width, int64_t height, int64_t stride,
                           bool eightConnected);

} // namespace slideio::processor::detail
