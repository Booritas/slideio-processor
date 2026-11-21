// Requirements: UR-SEG-3, UR-SEG-4
#include <core/detail/level_mapping.hpp>

#include <cmath>

namespace slideio::processor::detail {

int64_t levelToLevel0(int64_t v, double scale) noexcept {
    return static_cast<int64_t>(std::llround(static_cast<double>(v) * scale));
}

namespace {

// levelToLevel0, refused where llround would leave int64. 2^63 is exact in double; a
// product at or beyond it does not fit.
bool forward(int64_t levelOrdinate, double scale, int64_t& out) noexcept {
    if (!(std::fabs(static_cast<double>(levelOrdinate) * scale) < 9223372036854775808.0)) {
        return false;
    }
    out = levelToLevel0(levelOrdinate, scale);
    return true;
}

} // namespace

bool levelOrdinateAtOrBelow(int64_t level0, double scale, int64_t& out) noexcept {
    // A seed a few steps from the answer at practical magnitudes (bounded at the 2^62 guard), kept clear of int64's ends so the
    // steps cannot overflow.
    const double estimate = std::floor((static_cast<double>(level0) + 0.5) / scale);
    if (!(std::fabs(estimate) < 4611686018427387904.0)) { // 2^62
        return false;
    }
    int64_t level = static_cast<int64_t>(estimate);
    int64_t mapped = 0;
    for (;;) {
        if (!forward(level, scale, mapped)) {
            return false;
        }
        if (mapped <= level0) {
            break;
        }
        --level;
    }
    for (;;) {
        if (!forward(level + 1, scale, mapped)) {
            return false;
        }
        if (mapped > level0) {
            break;
        }
        ++level;
    }
    out = level;
    return true;
}

Rect64 rectToLevel0(const Rect64& levelRect, double scale) noexcept {
    const int64_t x0 = levelToLevel0(levelRect.x, scale);
    const int64_t y0 = levelToLevel0(levelRect.y, scale);
    const int64_t x1 = levelToLevel0(levelRect.x + levelRect.width, scale);
    const int64_t y1 = levelToLevel0(levelRect.y + levelRect.height, scale);
    return Rect64{x0, y0, x1 - x0, y1 - y0};
}

} // namespace slideio::processor::detail
