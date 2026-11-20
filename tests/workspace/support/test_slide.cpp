// Requirements: UR-STG-1
#include "workspace/support/test_slide.hpp"

#include "common/temp_path.hpp"

#include <tiffio.h>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace slideio::processor::testing_support {

namespace {

constexpr uint32_t kTile = 64;
constexpr uint16_t kSamples = 3;

struct TiffCloser {
    void operator()(TIFF* tif) const noexcept { TIFFClose(tif); }
};

// Writes the tiled RGB TIFF, `levels` pages deep; `shade(x, y, px)` fills the three samples
// of the level-0 pixel (x, y).
template <typename Shade>
std::filesystem::path writeSlide(const char* caller, const std::filesystem::path& file, int32_t width, int32_t height,
                                 int32_t levels, Shade shade) {
    if (width <= 0 || height <= 0) {
        throw std::runtime_error(std::string(caller) + ": width and height must be positive");
    }
    if (levels < 1) {
        throw std::runtime_error(std::string(caller) + ": levels must be at least 1");
    }
#ifdef _WIN32
    std::unique_ptr<TIFF, TiffCloser> tif(TIFFOpenW(file.wstring().c_str(), "w"));
#else
    std::unique_ptr<TIFF, TiffCloser> tif(TIFFOpen(file.c_str(), "w"));
#endif
    if (!tif) {
        throw std::runtime_error(std::string(caller) + ": libtiff cannot create " + file.u8string());
    }
    TIFF* t = tif.get();
    std::vector<uint8_t> tile(static_cast<size_t>(kTile) * kTile * kSamples);
    // Level k is the slide halved k times; its pixel (x, y) shades as level-0 pixel (x, y) << k.
    for (int32_t level = 0; level < levels; ++level) {
        const uint32_t shift = static_cast<uint32_t>(level);
        const uint32_t levelWidth = std::max<uint32_t>(1, (static_cast<uint32_t>(width) + (1u << shift) - 1) >> shift);
        const uint32_t levelHeight = std::max<uint32_t>(1, (static_cast<uint32_t>(height) + (1u << shift) - 1) >> shift);
        if (level > 0) TIFFSetField(t, TIFFTAG_SUBFILETYPE, static_cast<uint32_t>(FILETYPE_REDUCEDIMAGE));
        // SlideIO's SVS driver, which takes the tiled pages as levels, reads the first page's description.
        if (level == 0 && levels > 1) TIFFSetField(t, TIFFTAG_IMAGEDESCRIPTION, "Aperio Image Library v1.0");
        TIFFSetField(t, TIFFTAG_IMAGEWIDTH, levelWidth);
        TIFFSetField(t, TIFFTAG_IMAGELENGTH, levelHeight);
        TIFFSetField(t, TIFFTAG_SAMPLESPERPIXEL, kSamples);
        TIFFSetField(t, TIFFTAG_BITSPERSAMPLE, static_cast<uint16_t>(8));
        TIFFSetField(t, TIFFTAG_SAMPLEFORMAT, static_cast<uint16_t>(SAMPLEFORMAT_UINT));
        TIFFSetField(t, TIFFTAG_PHOTOMETRIC, static_cast<uint16_t>(PHOTOMETRIC_RGB));
        TIFFSetField(t, TIFFTAG_PLANARCONFIG, static_cast<uint16_t>(PLANARCONFIG_CONTIG));
        TIFFSetField(t, TIFFTAG_COMPRESSION, static_cast<uint16_t>(COMPRESSION_NONE));
        TIFFSetField(t, TIFFTAG_TILEWIDTH, kTile);
        TIFFSetField(t, TIFFTAG_TILELENGTH, kTile);
        for (uint32_t ty = 0; ty < levelHeight; ty += kTile) {
            for (uint32_t tx = 0; tx < levelWidth; tx += kTile) {
                for (uint32_t y = 0; y < kTile; ++y) {
                    for (uint32_t x = 0; x < kTile; ++x) {
                        uint8_t* px = &tile[(static_cast<size_t>(y) * kTile + x) * kSamples];
                        shade((tx + x) << shift, (ty + y) << shift, px);
                    }
                }
                if (TIFFWriteTile(t, tile.data(), tx, ty, 0, 0) < 0) {
                    throw std::runtime_error(std::string(caller) + ": libtiff cannot write a tile of " +
                                             file.u8string());
                }
            }
        }
        if (TIFFWriteDirectory(t) == 0) {
            throw std::runtime_error(std::string(caller) + ": libtiff cannot write the directory of " +
                                     file.u8string());
        }
    }
    return file;
}

constexpr int64_t kRadius = 20;

// True when (x, y) lies in one of the discs of the 3 x 3 grid, centres at 1/6, 3/6 and
// 5/6 of each side; `skipMiddleColumn` leaves the 3/6 column out.
bool insideDisc(int64_t x, int64_t y, int64_t width, int64_t height, bool skipMiddleColumn) {
    for (int64_t i = 1; i <= 5; i += 2) {
        if (skipMiddleColumn && i == 3) continue;
        for (int64_t j = 1; j <= 5; j += 2) {
            const int64_t dx = x - width * i / 6;
            const int64_t dy = y - height * j / 6;
            if (dx * dx + dy * dy <= kRadius * kRadius) return true;
        }
    }
    return false;
}

} // namespace

std::filesystem::path writeTestSlide(const std::filesystem::path& file, int32_t width, int32_t height) {
    return writeSlide("writeTestSlide", file, width, height, 1, [](uint32_t x, uint32_t y, uint8_t* px) {
        px[0] = static_cast<uint8_t>(x & 0xFF);
        px[1] = static_cast<uint8_t>(y & 0xFF);
        px[2] = static_cast<uint8_t>((x + y) & 0xFF);
    });
}

std::filesystem::path writeBlobSlide(const std::filesystem::path& file, int32_t width, int32_t height,
                                     int32_t levels) {
    return writeSlide("writeBlobSlide", file, width, height, levels, [=](uint32_t x, uint32_t y, uint8_t* px) {
        const bool inside = insideDisc(x, y, width, height, false);
        px[0] = px[1] = px[2] = inside ? 20 : 255;
    });
}

std::filesystem::path writeHalfBlobSlide(const std::filesystem::path& file, int32_t width, int32_t height) {
    return writeSlide("writeHalfBlobSlide", file, width, height, 1, [=](uint32_t x, uint32_t y, uint8_t* px) {
        const bool inside = insideDisc(x, y, width, height, true);
        const bool left = static_cast<int64_t>(x) < width / 2;
        px[0] = px[1] = px[2] = inside ? 20 : (left ? 255 : 200);
    });
}

std::filesystem::path writeEdgeBlobSlide(const std::filesystem::path& file, int32_t width, int32_t height,
                                         int32_t levels) {
    return writeSlide("writeEdgeBlobSlide", file, width, height, levels, [=](uint32_t x, uint32_t y, uint8_t* px) {
        const int64_t centres[][2] = {{kEdgeX - 25, 150}, {kEdgeX + 60, 150}, {100, 450}};
        bool inside = false;
        for (const auto& c : centres) {
            const int64_t dx = static_cast<int64_t>(x) - c[0];
            const int64_t dy = static_cast<int64_t>(y) - c[1];
            inside = inside || dx * dx + dy * dy <= kRadius * kRadius;
        }
        px[0] = px[1] = px[2] = inside ? 20 : (static_cast<int64_t>(x) < kEdgeX ? 255 : 200);
    });
}

TempDirectory::TempDirectory() : path_(uniqueTempPath("workspace_test", "")) {}

TempDirectory::~TempDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
}

} // namespace slideio::processor::testing_support
