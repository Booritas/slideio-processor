// Requirements: UR-IMG-2, UR-SEG-4
#pragma once

// The store's private geometry encoding (ruling S1), covered by the store format version.
//
// Byte 0 is the encoding version, 1. Then a varint ring count; then, for each ring, a
// varint (pointCount << 1) | isHole, followed by the points. A ring's first point is
// zigzag-varint absolute x and y; each later point is zigzag-varint deltas from the
// previous one, computed in wrapping uint64_t arithmetic, so every int64_t pair
// round-trips exactly. Varints are unsigned LEB128 of at most 10 bytes.

#include <slideio/processor/core/geometry.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace slideio::processor::detail {

// Encodes every ring of `polygon`, in ring order. An empty polygon encodes as {1, 0}.
// Throws std::bad_alloc.
std::vector<uint8_t> encodePolygon(const Polygon& polygon);

// Decodes a blob written by encodePolygon into `out`. `objectName` names the owner in
// error messages (for example "object (layer 3, anchor 42)"); null reads as
// "geometry blob". Throws InternalError naming it when the blob is truncated, has
// trailing bytes, an unknown version, an over-long varint, a ring of fewer than 3
// points, or a ring Polygon::addRing refuses: a stored blob core refuses is corruption.
// `out` is unchanged on failure. Throws std::bad_alloc.
void decodePolygon(const uint8_t* data, size_t size, Polygon& out, const char* objectName = nullptr);

} // namespace slideio::processor::detail
