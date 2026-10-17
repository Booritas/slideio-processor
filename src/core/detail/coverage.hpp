#pragma once

#include <slideio/processor/core/geometry.hpp>

#include <cstdint>
#include <functional>

namespace slideio::processor::detail {

/// A pixel grid in level-0 space: pixel (i, j) is the square
/// [originX + i*pixelW, originX + (i+1)*pixelW) x [originY + j*pixelH, ...).
struct PixelGrid {
    double originX, originY, pixelW, pixelH;
    int64_t width, height;
};

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
