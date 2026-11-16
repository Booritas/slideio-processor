// Requirements: UR-PIX-5, UR-IMG-12
#include <pipeline/detail/half.hpp>

#include <cmath>
#include <limits>

namespace slideio::processor::detail {

double halfToDouble(uint16_t bits) noexcept {
    const bool negative = (bits & 0x8000u) != 0;
    const int exponent = (bits >> 10) & 0x1F;
    const int mantissa = bits & 0x3FF;
    double magnitude;
    if (exponent == 0) {
        // Zero and subnormals: mantissa * 2^-24, exact in double.
        magnitude = std::ldexp(static_cast<double>(mantissa), -24);
    } else if (exponent == 31) {
        magnitude = mantissa == 0 ? std::numeric_limits<double>::infinity()
                                  : std::numeric_limits<double>::quiet_NaN();
    } else {
        magnitude = std::ldexp(static_cast<double>(mantissa | 0x400), exponent - 25);
    }
    return negative ? -magnitude : magnitude;
}

} // namespace slideio::processor::detail
