// Requirements: UR-RES-3, UR-IMG-2
#pragma once

// Exact polygon predicates on int64 lattice coordinates (ruling S10), for the polygon
// filters of ObjectStore queries. Every orientation test is exact for any int64
// operands: coordinate differences are taken as sign and 64-bit magnitude, and the
// cross product compares two unsigned 64x64->128-bit products. Nothing is computed in
// floating point.
//
// A polygon's region is closed: its interior under the even-odd rule over every ring,
// plus every ring edge.

#include <slideio/processor/core/geometry.hpp>

namespace slideio::processor::detail {

// The sign of the cross product (b - a) x (c - a): 1 when c lies to the left of the
// directed line a->b (counter-clockwise with y up), -1 to the right, 0 when collinear.
// Exact for every int64 input.
int orientation(Point64 a, Point64 b, Point64 c) noexcept;

// True when the closed regions of a and b share at least one point, touching included:
// their bounding boxes meet, and either a pair of edges crosses or touches, or a vertex
// of one lies inside the other. False when either is empty.
bool polygonsIntersect(const Polygon& a, const Polygon& b);

// True when the closed region of inner lies inside or on the closed region of outer,
// exactly, for concave polygons and holes too. Fast rejections: a vertex of inner outside
// outer, an edge of inner properly crossing an edge of outer, a vertex of outer strictly
// inside inner. When the boundaries do not touch, passing those is containment. When
// they touch (a vertex of one on the other's boundary), every edge of each is cut at the
// other's vertices on it, and for each piece, a point just beside its midpoint, on either
// side, that is inside inner must be inside outer; this catches an inner escaping through
// an outer vertex or along a shared edge. Midpoints use doubled coordinates and 256-bit
// cross products, so the test is exact for any int64 input. False when either is empty.
bool polygonContainedBy(const Polygon& inner, const Polygon& outer);

} // namespace slideio::processor::detail
