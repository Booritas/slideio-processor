// Requirements: UR-SEG-3, UR-SEG-4
#pragma once

#include <slideio/processor/core/export.hpp>
#include <slideio/processor/core/geometry.hpp>

#include <cstdint>

namespace slideio::processor::detail {

// (Exported from the data library only so the engine library, a separate DLL, calls the same
// code; not part of the public API.)

/// The library's single level-to-level-0 mapping: llround(v * scale).
/// LevelSelector, OperationContext and TileGrid all call it, so two components
/// never round one seam differently.
SLIDEIO_PROCESSOR_DATA_EXPORTS int64_t levelToLevel0(int64_t v, double scale) noexcept;

/// Maps a rectangle by its edges: x0 = map(x), x1 = map(x + width), width
/// x1 - x0, likewise y. Two rectangles sharing a level edge share the level-0
/// edge exactly.
SLIDEIO_PROCESSOR_DATA_EXPORTS Rect64 rectToLevel0(const Rect64& levelRect, double scale) noexcept;

/// The level pixel of a level-0 ordinate: the largest L with levelToLevel0(L, scale)
/// <= level0, the exact inverse of the mapping above. Integer arithmetic decides the
/// answer; a division only seeds it. False (out unchanged) when the point's level ordinate
/// would lie beyond +-2^62 (the seed guard), or a mapped product reaches 2^63.
SLIDEIO_PROCESSOR_DATA_EXPORTS bool levelOrdinateAtOrBelow(int64_t level0, double scale, int64_t& out) noexcept;

} // namespace slideio::processor::detail
