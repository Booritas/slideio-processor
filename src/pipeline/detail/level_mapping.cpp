// Requirements: UR-SEG-3, UR-SEG-4
#include <pipeline/detail/level_mapping.hpp>

#include <cmath>

namespace slideio::processor::detail {

int64_t levelToLevel0(int64_t v, double scale) noexcept {
    return static_cast<int64_t>(std::llround(static_cast<double>(v) * scale));
}

Rect64 rectToLevel0(const Rect64& levelRect, double scale) noexcept {
    const int64_t x0 = levelToLevel0(levelRect.x, scale);
    const int64_t y0 = levelToLevel0(levelRect.y, scale);
    const int64_t x1 = levelToLevel0(levelRect.x + levelRect.width, scale);
    const int64_t y1 = levelToLevel0(levelRect.y + levelRect.height, scale);
    return Rect64{x0, y0, x1 - x0, y1 - y0};
}

} // namespace slideio::processor::detail
