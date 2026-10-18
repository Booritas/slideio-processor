#pragma once

#include <slideio/processor/core/block_view.hpp>
#include <slideio/processor/core/geometry.hpp>

#include <cstdint>
#include <functional>

namespace slideio::processor::detail {

/// The smallest level scale (level-0 pixels per level pixel) accepted by
/// RasterMask and gridOfView. One constant for both, because the kernel's
/// NaN-free arithmetic relies on every entry point rejecting the same scales.
inline constexpr double kMinLevelScale = 1e-6;

/// A window of `width` x `height` pixels onto the pixel lattice of one level.
///
/// Level pixel (I, J), in ABSOLUTE level coordinates, is the level-0 square
/// [I*pixelW, (I+1)*pixelW) x [J*pixelH, (J+1)*pixelH). The window holds the
/// level pixels originLevelX <= I < originLevelX + width (and likewise for J);
/// grid pixel (i, j) is level pixel (originLevelX + i, originLevelY + j).
///
/// The origin is an integer on purpose: the kernel maps vertices as x / pixelW
/// with no origin term, so the rounding is identical for every window and two
/// windows sharing a seam agree on it at any scale. Callers supply:
/// - RasterMask: originLevel = region.x / region.y, pixelW = pixelH = levelScale.
/// - BlockView (Task 12): pixelW = slideRect.width / width (likewise H), and
///   originLevelX = llround(slideRect.x / pixelW) (likewise Y).
struct PixelGrid {
    int64_t originLevelX, originLevelY;
    double pixelW, pixelH;
    int64_t width, height;
};

/// The grid a BlockView is rasterised on: pixel size slideRect/size, origin
/// llround(slideRect.x / pixelW). Shared by rasteriseInto(BlockView...) and
/// the layer raster source so a query can cover exactly what is rasterised.
///
/// The origin is ROUNDED: when slideRect.x / pixelW (or y / pixelH) is not an
/// integer, this grid is offset from the true level grid by up to half a level
/// pixel. The kernel's conservative guarantee holds on this rounded grid, not
/// on the true one. Not exported.
/// Throws InvalidArgumentError for a null view or an unusable scale or origin.
PixelGrid gridOfView(const BlockView& view);

/// The conservative even-odd coverage kernel (SDS 01 1.17, plan ruling R10).
///
/// Calls row(y, x0, x1) for each maximal run [x0, x1) of covered pixels in row
/// y, rows ascending and runs ascending within a row. A pixel is covered when
/// a polygon edge passes through its open interior, or when its centre is
/// inside by even-odd with the same tie rule as Polygon::contains. Every run is
/// computed before the first call, so a throw (std::bad_alloc) leaves the
/// caller's target untouched. Not exported.
void forEachCoveredSpan(const Polygon& polygon, const PixelGrid& grid,
                        const std::function<void(int64_t y, int64_t x0, int64_t x1)>& row);

} // namespace slideio::processor::detail
