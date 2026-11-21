// Requirements: UR-RES-9
#pragma once

// Internal: well-known text for a polygon. Not installed.

#include <string>

namespace slideio::processor {
class Polygon;
}

namespace slideio::processor::detail {

/**
 * Appends a polygon as WKT in level-0 integer coordinates, rings closed by repeating
 * the first point: POLYGON((outer),(hole),...) for one outer ring,
 * MULTIPOLYGON(((outer),(hole),...),...) for several. A hole joins the first outer
 * ring, in ring order, whose ring alone contains the hole's first vertex (even-odd);
 * one inside none joins the nearest outer ring before it. An empty polygon appends
 * "POLYGON EMPTY".
 */
void appendWkt(std::string& out, const Polygon& polygon);

} // namespace slideio::processor::detail
