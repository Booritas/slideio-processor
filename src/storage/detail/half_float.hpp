// Requirements: UR-RES-7
#pragma once

// IEEE 754 binary16 ("half") conversions for the Float16 pyramid mean (ruling S43). C++17
// has no half type, so a Float16 sample travels as its 16 raw bits.

#include <cstdint>

namespace slideio::processor::detail {

// The float value of the half whose bits are `bits`. Exact: every half, subnormals,
// infinities and NaNs included, is representable as a float. A NaN keeps its sign and
// the high bits of its payload.
float halfToFloat(uint16_t bits) noexcept;

// The bits of the half nearest to `value`, ties to even. Values beyond the largest finite
// half (65504) round to infinity as IEEE 754 does; values too small for a subnormal round
// to a signed zero. A NaN stays a NaN (quiet, sign kept).
uint16_t floatToHalf(float value) noexcept;

} // namespace slideio::processor::detail
