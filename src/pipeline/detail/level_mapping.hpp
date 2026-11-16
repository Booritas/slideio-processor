// Requirements: UR-SEG-3, UR-SEG-4
#pragma once

#include <slideio/processor/core/geometry.hpp>

#include <cstdint>

namespace slideio::processor::detail {

/// The library's single level-to-level-0 mapping: llround(v * scale).
/// LevelSelector, OperationContext and TileGrid all call it, so two components
/// never round one seam differently.
int64_t levelToLevel0(int64_t v, double scale) noexcept;

/// Maps a rectangle by its edges: x0 = map(x), x1 = map(x + width), width
/// x1 - x0, likewise y. Two rectangles sharing a level edge share the level-0
/// edge exactly.
Rect64 rectToLevel0(const Rect64& levelRect, double scale) noexcept;

} // namespace slideio::processor::detail
