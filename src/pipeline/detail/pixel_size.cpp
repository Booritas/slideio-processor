// Requirements: UR-SEG-5, UR-EXT-1
#include <pipeline/detail/pixel_size.hpp>

#include <slideio/processor/core/measurement.hpp>

#include <tuple>

namespace slideio::processor::detail {

double levelZeroPixelSizeMicrons(const slideio::Scene& scene) noexcept {
    try {
        const std::tuple<double, double> resolution = scene.getResolution();
        return pixelSizeMicronsFromResolution(std::get<0>(resolution), std::get<1>(resolution));
    } catch (...) {
        return 0.0;
    }
}

} // namespace slideio::processor::detail
