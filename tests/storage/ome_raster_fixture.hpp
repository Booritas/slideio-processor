// Requirements: UR-RES-7
// Test-only: the 1500x900 Int32 raster the OME-TIFF layout tests write, shared by the storage
// test (libtiff read-back) and the masks test (SlideIO read-back). The caller compiles the
// storage detail sources in, as tests/storage and tests/masks do.
#pragma once

#include "storage/detail/pyramid_tiff_writer.hpp"
#include "storage/detail/tile_spool.hpp"

#include <slideio/processor/core/data_type.hpp>
#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/storage/raster_store.hpp>

#include <cstdint>
#include <filesystem>
#include <vector>

namespace slideio::processor::test {

constexpr int64_t kOmeWidth = 1500;
constexpr int64_t kOmeHeight = 900;

// The sample at (x, y): distinct along both axes, negative in part, so a transposed,
// shifted or sign-lost read shows.
inline int32_t omeSample(int64_t x, int64_t y) { return static_cast<int32_t>(y * 4096 + x - 1000000); }

// Spools a `width` x `height` raster of omeSample at `spoolPath` and writes it to `tif` with
// the OME-TIFF layout.
inline detail::PyramidInfo writeOmeRaster(const std::filesystem::path& tif, const std::filesystem::path& spoolPath,
                                          const detail::PyramidMetadata& metadata,
                                          int64_t width = kOmeWidth, int64_t height = kOmeHeight,
                                          RasterCompression compression = RasterCompression::Deflate) {
    const detail::RasterFormat format{DataType::Int32, 1, compression};
    std::vector<int32_t> plane(static_cast<size_t>(width * height));
    for (int64_t y = 0; y < height; ++y)
        for (int64_t x = 0; x < width; ++x) plane[static_cast<size_t>(y * width + x)] = omeSample(x, y);
    detail::TileSpool spool(spoolPath, format);
    const void* planes[] = {plane.data()};
    const int64_t strides[] = {width * static_cast<int64_t>(sizeof(int32_t))};
    spool.append(Rect64{0, 0, width, height}, width, height, planes, strides);
    spool.place(1.0, 1.0);
    return detail::writePyramidTiff(tif, spool, format, metadata, detail::PyramidLayout::OmeSubIfds);
}

} // namespace slideio::processor::test
