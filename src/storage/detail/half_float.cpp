#include "storage/detail/half_float.hpp"

#include <cmath>
#include <cstring>

namespace slideio::processor::detail {

float halfToFloat(uint16_t bits) noexcept {
    const uint32_t sign = static_cast<uint32_t>(bits & 0x8000u) << 16;
    const uint32_t exponent = (bits >> 10) & 0x1fu;
    const uint32_t mantissa = bits & 0x3ffu;
    if (exponent == 0) {
        // Zero or subnormal: mantissa * 2^-24, exact in a float.
        const float magnitude = std::ldexp(static_cast<float>(mantissa), -24);
        return sign != 0 ? -magnitude : magnitude;
    }
    uint32_t result = 0;
    if (exponent == 0x1f)
        result = sign | 0x7f800000u | (mantissa << 13); // infinity or NaN
    else
        result = sign | ((exponent + 112u) << 23) | (mantissa << 13); // rebias 15 -> 127
    float value = 0.0f;
    std::memcpy(&value, &result, sizeof value);
    return value;
}

uint16_t floatToHalf(float value) noexcept {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    const auto sign = static_cast<uint16_t>((bits >> 16) & 0x8000u);
    const uint32_t magnitude = bits & 0x7fffffffu;
    if (magnitude >= 0x7f800000u) {
        if (magnitude == 0x7f800000u) return static_cast<uint16_t>(sign | 0x7c00u);
        // NaN: keep the payload's high bits and make it quiet, so it never becomes infinity.
        return static_cast<uint16_t>(sign | 0x7e00u | ((magnitude >> 13) & 0x3ffu));
    }
    // 65520 is halfway between the largest half (65504) and 65536; the tie goes to the even
    // neighbour, infinity, as everything above it does.
    if (magnitude >= 0x477ff000u) return static_cast<uint16_t>(sign | 0x7c00u);
    if (magnitude < 0x38800000u) {
        // Below the smallest normal half (2^-14): a subnormal in units of 2^-24. Up to 2^-25
        // inclusive (the tie with zero) rounds to zero.
        if (magnitude <= 0x33000000u) return sign;
        const uint32_t mantissa = (magnitude & 0x7fffffu) | 0x800000u;
        const uint32_t shift = 126u - (magnitude >> 23); // 14..24
        uint32_t units = mantissa >> shift;
        const uint32_t rest = mantissa & ((1u << shift) - 1u);
        const uint32_t halfway = 1u << (shift - 1u);
        if (rest > halfway || (rest == halfway && (units & 1u) != 0)) ++units; // may carry into 2^-14
        return static_cast<uint16_t>(sign | units);
    }
    uint32_t half = (((magnitude >> 23) - 112u) << 10) | ((magnitude & 0x7fffffu) >> 13);
    const uint32_t rest = magnitude & 0x1fffu;
    if (rest > 0x1000u || (rest == 0x1000u && (half & 1u) != 0)) ++half; // a carry moves to the next exponent
    return static_cast<uint16_t>(sign | half);
}

} // namespace slideio::processor::detail
