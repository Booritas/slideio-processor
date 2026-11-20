// Requirements: UR-STG-1
// Test-only: a slide file SlideIO opens, and a temporary directory that removes itself.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace slideio::processor::testing_support {

// Writes an 8-bit, 3-sample (RGB), tiled TIFF of width x height to `file`, through
// libtiff, and returns `file`. Pixel values are a deterministic gradient. Throws
// std::runtime_error when libtiff cannot write the file.
std::filesystem::path writeTestSlide(const std::filesystem::path& file, int32_t width, int32_t height);

// Writes the same TIFF form as writeTestSlide: a white slide with nine dark discs of
// radius 20 on a 3x3 grid, their centres at 1/6, 3/6 and 5/6 of each side. Colour
// conversion to grey, an inverted fixed threshold and connected components find
// exactly nine objects in it, given a slide large enough to hold the discs apart.
// `levels` > 1 appends reduced-resolution pages, each half the size of the one before, and an
// Aperio-style description; name the file `*.svs` and the SVS driver exposes the pages as
// pyramid levels (any other extension opens one level).
std::filesystem::path writeBlobSlide(const std::filesystem::path& file, int32_t width, int32_t height,
                                     int32_t levels = 1);

// A blob slide whose left half (x < width / 2) is white and whose right half is light grey
// (200), with the nine discs but the middle column of them: three dark discs on the left, three
// on the right. Thresholding above 230 selects the left half alone (its discs are holes, so
// extractHoles = false), and below 128 selects all six discs.
std::filesystem::path writeHalfBlobSlide(const std::filesystem::path& file, int32_t width, int32_t height);

// The column where the white region of writeEdgeBlobSlide ends and the grey begins.
// Deliberately not a multiple of 4, so a level-2 analysis puts the region's edge past it, and chosen so
// that disc D lies inside one 64 px tile of the slide: an object that straddles a tile seam beside the
// mask edge can lose the fragment whose anchor falls outside the mask.
constexpr int64_t kEdgeX = 290;

// A slide with a white region (x < kEdgeX) beside a grey one (200), and four dark discs of
// radius 20: A centred 25 px inside the edge (150 down), B 60 px outside it, C deep
// inside the white region at (100, 450), and D centred 3 px outside the edge (300 down), in the band
// a level-2 analysis dilates the region by. Thresholding above 230 selects the white region
// (disc A is a hole in it, so extractHoles = false); below 128 selects the discs. `levels` and the
// `*.svs` naming are as for writeBlobSlide.
std::filesystem::path writeEdgeBlobSlide(const std::filesystem::path& file, int32_t width, int32_t height,
                                         int32_t levels = 1);

// A unique path in the temp directory, not created; remove_all on destruction, so a
// failing test leaves nothing behind.
class TempDirectory {
public:
    TempDirectory();
    ~TempDirectory();
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    const std::filesystem::path& path() const noexcept { return path_; }
    // The path as UTF-8, as the library's `const char*` arguments take it.
    std::string utf8() const { return path_.u8string(); }

private:
    std::filesystem::path path_;
};

} // namespace slideio::processor::testing_support
