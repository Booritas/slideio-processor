// Requirements: UR-STG-1
#include "workspace/support/test_slide.hpp"

#include "common/temp_path.hpp"

#include <tiffio.h>

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

// Writes the tiled RGB TIFF; `shade(x, y, px)` fills the three samples of pixel (x, y).
template <typename Shade>
std::filesystem::path writeSlide(const char* caller, const std::filesystem::path& file, int32_t width, int32_t height,
                                 Shade shade) {
    if (width <= 0 || height <= 0) {
        throw std::runtime_error(std::string(caller) + ": width and height must be positive");
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
    TIFFSetField(t, TIFFTAG_IMAGEWIDTH, static_cast<uint32_t>(width));
    TIFFSetField(t, TIFFTAG_IMAGELENGTH, static_cast<uint32_t>(height));
    TIFFSetField(t, TIFFTAG_SAMPLESPERPIXEL, kSamples);
    TIFFSetField(t, TIFFTAG_BITSPERSAMPLE, static_cast<uint16_t>(8));
    TIFFSetField(t, TIFFTAG_SAMPLEFORMAT, static_cast<uint16_t>(SAMPLEFORMAT_UINT));
    TIFFSetField(t, TIFFTAG_PHOTOMETRIC, static_cast<uint16_t>(PHOTOMETRIC_RGB));
    TIFFSetField(t, TIFFTAG_PLANARCONFIG, static_cast<uint16_t>(PLANARCONFIG_CONTIG));
    TIFFSetField(t, TIFFTAG_COMPRESSION, static_cast<uint16_t>(COMPRESSION_NONE));
    TIFFSetField(t, TIFFTAG_TILEWIDTH, kTile);
    TIFFSetField(t, TIFFTAG_TILELENGTH, kTile);

    std::vector<uint8_t> tile(static_cast<size_t>(kTile) * kTile * kSamples);
    for (uint32_t ty = 0; ty < static_cast<uint32_t>(height); ty += kTile) {
        for (uint32_t tx = 0; tx < static_cast<uint32_t>(width); tx += kTile) {
            for (uint32_t y = 0; y < kTile; ++y) {
                for (uint32_t x = 0; x < kTile; ++x) {
                    uint8_t* px = &tile[(static_cast<size_t>(y) * kTile + x) * kSamples];
                    shade(tx + x, ty + y, px);
                }
            }
            if (TIFFWriteTile(t, tile.data(), tx, ty, 0, 0) < 0) {
                throw std::runtime_error(std::string(caller) + ": libtiff cannot write a tile of " + file.u8string());
            }
        }
    }
    if (TIFFWriteDirectory(t) == 0) {
        throw std::runtime_error(std::string(caller) + ": libtiff cannot write the directory of " + file.u8string());
    }
    return file;
}

} // namespace

std::filesystem::path writeTestSlide(const std::filesystem::path& file, int32_t width, int32_t height) {
    return writeSlide("writeTestSlide", file, width, height, [](uint32_t x, uint32_t y, uint8_t* px) {
        px[0] = static_cast<uint8_t>(x & 0xFF);
        px[1] = static_cast<uint8_t>(y & 0xFF);
        px[2] = static_cast<uint8_t>((x + y) & 0xFF);
    });
}

std::filesystem::path writeBlobSlide(const std::filesystem::path& file, int32_t width, int32_t height) {
    constexpr int64_t kRadius = 20;
    return writeSlide("writeBlobSlide", file, width, height, [=](uint32_t x, uint32_t y, uint8_t* px) {
        bool inside = false;
        for (int64_t i = 1; i <= 5; i += 2) {
            for (int64_t j = 1; j <= 5; j += 2) {
                const int64_t dx = static_cast<int64_t>(x) - static_cast<int64_t>(width) * i / 6;
                const int64_t dy = static_cast<int64_t>(y) - static_cast<int64_t>(height) * j / 6;
                if (dx * dx + dy * dy <= kRadius * kRadius) inside = true;
            }
        }
        const uint8_t value = inside ? 20 : 255;
        px[0] = px[1] = px[2] = value;
    });
}

TempDirectory::TempDirectory() : path_(uniqueTempPath("workspace_test", "")) {}

TempDirectory::~TempDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
}

} // namespace slideio::processor::testing_support
