// Requirements: UR-RES-7
#pragma once

// Writes a spooled raster as a pyramidal BigTIFF (rulings S41, S42 and S43): tiled 512,
// PLANARCONFIG_SEPARATE with one sample per plane, page 0 at full resolution and page k
// a FILETYPE_REDUCEDIMAGE of half page k-1's size, rounded up, until both dimensions are
// at most 512. Every tile of every page is written; an all-zero tile is encoded once per
// page and its compressed bytes reused.

#include "storage/detail/tile_spool.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace slideio::processor::detail {

// What page 0's ImageDescription records (ruling S41).
struct PyramidMetadata {
    std::string name;        // the raster's name, UTF-8
    std::string description; // its description, UTF-8; may be empty
    int32_t level = 0;       // the level its tiles were written at
    double scaleX = 1.0;     // level-0 pixels per raster pixel
    double scaleY = 1.0;
};

// What writePyramidTiff wrote.
struct PyramidInfo {
    int32_t pages = 0;  // page count, page 0 included
    int64_t width = 0;  // page 0's size: the spool's extent
    int64_t height = 0;
};

// Creates or truncates `tif` and writes the spool's raster into it. Page 0 comes from the
// spool; each page k > 0 is built from page k-1, read back through a second handle on the
// same file: integer types take the top-left sample present in each 2x2 block, floating
// types the mean of the samples present, in double. The file is complete and closed on
// return. The spool must be placed (TileSpool::place); its extent is page 0's size.
// Throws InternalError for an empty or unplaced spool or a `format` whose type or plane
// count is not the spool's, IoError naming the file when libtiff or the file fails, and IoError from
// the spool. On a throw the partial file is left for the caller to delete.
PyramidInfo writePyramidTiff(const std::filesystem::path& tif, const TileSpool& spool, const RasterFormat& format,
                             const PyramidMetadata& metadata);

} // namespace slideio::processor::detail
