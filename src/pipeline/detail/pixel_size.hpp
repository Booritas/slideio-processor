// Requirements: UR-SEG-5, UR-EXT-1
#pragma once

#include <slideio/slideio/scene.hpp>

namespace slideio::processor::detail {

/// The level-0 pixel size of @p scene in micrometres: the mean of SlideIO's x and y
/// resolution (metres per pixel) times 10^6 when both are finite and positive and
/// agree within 10^-6 relative, else 0 (unknown or anisotropic). Never throws.
double levelZeroPixelSizeMicrons(const slideio::Scene& scene) noexcept;

} // namespace slideio::processor::detail
