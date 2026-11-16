// Requirements: UR-PIX-5, UR-IMG-12
#pragma once

#include <cstdint>

namespace slideio::processor::detail {

/// Converts an IEEE 754 binary16 bit pattern to double, exactly. Subnormals,
/// signed zeros, infinities and NaN are all handled. `core` has no public
/// conversion and the engine must not reach into storage's detail (ruling P14).
double halfToDouble(uint16_t bits) noexcept;

} // namespace slideio::processor::detail
