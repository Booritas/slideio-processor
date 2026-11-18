// Requirements: UR-SEG-3, UR-SEG-4
#pragma once

#include <slideio/processor/core/geometry.hpp>

#include <cstdint>
#include <vector>

namespace slideio::processor::detail {

/// The pixel-edge rings of one labelled region.
struct TracedRegion {
    /// The region's label in the traced buffer; positive.
    int32_t label = 0;
    /// The rings, outer rings first, each listing only the vertices where it
    /// turns (a vertex where a ring touches itself appears twice). Pixel-corner
    /// coordinates relative to the buffer: pixel (x, y) spans x..x+1, y..y+1.
    std::vector<std::vector<Point64>> rings;
    /// Per ring: true for a hole.
    std::vector<bool> isHole;
    /// True when a pixel of the region lies on the buffer's first or last row
    /// or column.
    bool touchesEdge = false;
};

/// Traces every region of a label buffer: width x height int32 labels, row y
/// starting at labels + y * stride. Each positive label is one region; 0 and
/// negative labels are background. The caller labels so that each region is
/// connected under the chosen connectivity, which is what makes it one outer
/// ring. Pixels beyond the buffer are background.
///
/// Every boundary edge keeps its region on its right in y-down coordinates, so
/// outer rings have positive shoelace area (clockwise on screen) and holes
/// negative. Where a region touches itself diagonally at a vertex, the walk
/// takes the sharpest left turn with @p eightConnected (8-connected pixels:
/// one outer ring through the point twice, as the seam merger links it) and
/// the sharpest right turn otherwise (4-connected pixels: the rings stay
/// apart). Regions are returned in the order their first pixel is met in
/// row-major order; their rings in the order they are met. Throws InternalError
/// when the edges of a region do not close, which valid input never causes.
std::vector<TracedRegion> traceRegions(const int32_t* labels, int64_t width, int64_t height, int64_t stride,
                                       bool eightConnected);

} // namespace slideio::processor::detail
