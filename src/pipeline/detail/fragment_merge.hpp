// Requirements: UR-SEG-3
#pragma once

#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/pipeline/tile_grid.hpp>

#include <cstdint>
#include <vector>

namespace slideio::processor::detail {

/// A stretch of a seam fragment's boundary lying on a seam line, in level-0
/// integers. A vertical contact lies on x = line and spans y in [begin, end);
/// a horizontal one lies on y = line and spans x in [begin, end). begin < end.
struct SeamContact {
    bool vertical;  ///< True for a line x = line, false for y = line.
    int64_t line;   ///< The line's level-0 coordinate.
    int64_t begin;  ///< Start of the interval along the line.
    int64_t end;    ///< End of the interval along the line, exclusive.
};

/// The seam contacts of a fragment emitted in tile @p tileIndex of @p grid. The
/// seam lines are the level-0 edges of that tile (TileGrid::tileRect) shared
/// with a neighbouring tile; the outer boundary of the grid region is not a
/// seam. A contact is a maximal interval of the union of the ring edges lying
/// on such a line, clipped to the tile's extent along it; intervals that touch
/// are one contact. Vertical contacts come first, then horizontal ones, each
/// ordered by line and then begin. Throws InvalidArgumentError when the index
/// is out of range.
std::vector<SeamContact> seamContacts(const Polygon& polygon, const TileGrid& grid, int64_t tileIndex);

/// True when two contacts lie on the same line with the same orientation and
/// their intervals overlap with positive length. Intervals meeting at one point
/// (objects touching at a corner) do not join: merging is 4-connected.
bool contactsJoin(const SeamContact& a, const SeamContact& b);

/// The exact union of the fragments of one object, in integers. Every ring is
/// first oriented by its hole flag (outer rings to positive shoelace area in
/// y-down coordinates, clockwise on screen; holes negative), so the object is
/// always on the right of its edges. Every axis-parallel edge is split at the
/// fragment vertices lying inside it (seam lines are axis-parallel, so no other
/// edge can need it); directed edges coinciding with opposite directions cancel,
/// which removes the shared seams; the rest are linked into rings, taking at a
/// vertex with several outgoing edges the one turning most sharply right, so
/// rings that touch at a point stay apart. Positive rings are outer, negative
/// ones holes. The result is normalised by normaliseRings.
///
/// Throws InternalError when there are no fragments, when fragments overlap (a
/// directed edge survives twice), when the edges do not close into rings, when
/// a ring has zero area, or when the union has other than exactly one outer
/// ring (the fragments are not one 4-connected object). The message carries the
/// fragment and ring counts, never coordinates, which could carry clinical
/// data; this function does not know the layer, so the caller adds the layer
/// name when it reports the error. Coordinates are level-0 slide coordinates; edge-vector
/// cross products must fit int64, which holds for any coordinate below 2^31.
Polygon mergeFragments(const std::vector<const Polygon*>& fragments);

/// A ring with collinear vertices removed (the wrap-around included, each vertex
/// tested against its original neighbours), rotated to start at its
/// topmost-then-leftmost vertex; orientation is kept.
std::vector<Point64> normaliseRing(const std::vector<Point64>& ring);

/// Normalises every ring of @p polygon with normaliseRing and reorders the
/// rings: outer rings first in their order, then the holes ordered by first
/// vertex (y, then x). The form in which object operations emit polygons, so a
/// merged object and one found whole compare point for point. Throws
/// InvalidArgumentError, leaving the polygon unchanged, when a ring degenerates.
void normaliseRings(Polygon& polygon);

} // namespace slideio::processor::detail
