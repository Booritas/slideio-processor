// Requirements: UR-SEG-5, UR-EXT-1
#include <pipeline/detail/pixel_size.hpp>

#include <algorithm>
#include <cmath>
#include <tuple>

namespace slideio::processor::detail {

double levelZeroPixelSizeMicrons(const slideio::Scene& scene) noexcept {
    try {
        const std::tuple<double, double> resolution = scene.getResolution();
        const double x = std::get<0>(resolution);
        const double y = std::get<1>(resolution);
        if (!std::isfinite(x) || !std::isfinite(y) || !(x > 0.0) || !(y > 0.0)) {
            return 0.0;
        }
        if (std::fabs(x - y) > 1e-6 * std::max(x, y)) {
            return 0.0;
        }
        return 0.5 * (x + y) * 1e6;
    } catch (...) {
        return 0.0;
    }
}

} // namespace slideio::processor::detail
