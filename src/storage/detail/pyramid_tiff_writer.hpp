// Requirements: UR-RES-7
#pragma once

// Writes a spooled raster as a pyramidal BigTIFF (rulings S41, S42 and S43): tiled 512,
// PLANARCONFIG_SEPARATE with one sample per plane, page 0 at full resolution and page k
// a FILETYPE_REDUCEDIMAGE of half page k-1's size, rounded up, until both dimensions are
// at most 512. Every tile of every page is written; an all-zero tile is encoded once per
// page and its compressed bytes reused. Where the reduced pages go and what page 0's
// ImageDescription holds depend on the layout (SDS 03 §3.8).

#include "storage/detail/tile_spool.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace slideio::processor::detail {

// Where the reduced pages go, and what page 0's ImageDescription holds.
enum class PyramidLayout {
    // RasterRegistry's files (store format v2, ruling S41): page k is top-level IFD k, and
    // the description is the JSON object of S41. Its bytes never change.
    Sequential,
    // OME-TIFF, for exported rasters (reporting plan C, decision 1): page 0 is the only
    // top-level IFD, pages 1.. are its SubIFDs, and the description is minimal OME-XML.
    // A page that fits in one tile is one strip per plane, so SlideIO reads it at its own
    // size (SDS 03 §3.8).
    OmeSubIfds,
};

// What page 0's ImageDescription records (ruling S41). The Sequential layout writes every
// field but micronsPerPixel; OmeSubIfds writes name and micronsPerPixel only.
struct PyramidMetadata {
    std::string name;        // the raster's name, UTF-8
    std::string description; // its description, UTF-8; may be empty
    int32_t level = 0;       // the level its tiles were written at
    double scaleX = 1.0;     // level-0 pixels per raster pixel
    double scaleY = 1.0;
    double micronsPerPixel = 0.0; // page 0's pixel size in µm; 0 for unknown
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
// count is not the spool's, IoError naming the file by its role and its path when libtiff or the file fails, and IoError from
// the spool. On a throw the partial file is left for the caller to delete.
//
// With PyramidLayout::OmeSubIfds, page 0's ImageDescription is OME-XML naming the image
// `metadata.name` (possibly empty) and, when `metadata.micronsPerPixel` > 0, its physical
// pixel size; nothing else of `metadata` is written, and never a path. The layout takes
// Byte, Int8, UInt16, Int16, Int32, Float32 and Float64, the types SlideIO's OME-TIFF
// driver reads; any other type throws UnsupportedDataTypeError naming those, and a
// micronsPerPixel that is negative or not finite InvalidArgumentError, both before the
// file is created.
PyramidInfo writePyramidTiff(const std::filesystem::path& tif, const TileSpool& spool, const RasterFormat& format,
                             const PyramidMetadata& metadata, PyramidLayout layout = PyramidLayout::Sequential);

} // namespace slideio::processor::detail
