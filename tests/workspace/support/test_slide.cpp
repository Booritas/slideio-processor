// Requirements: UR-STG-1
#include "workspace/support/test_slide.hpp"

#include "common/temp_path.hpp"

#include <tiffio.h>

#include <memory>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace slideio::processor::testing_support {

namespace {

constexpr uint32_t kTile = 64;
constexpr uint16_t kSamples = 3;

struct TiffCloser {
    void operator()(TIFF* tif) const noexcept { TIFFClose(tif); }
};

} // namespace

std::filesystem::path writeTestSlide(const std::filesystem::path& file, int32_t width, int32_t height) {
    if (width <= 0 || height <= 0) {
        throw std::runtime_error("writeTestSlide: width and height must be positive");
    }
#ifdef _WIN32
    std::unique_ptr<TIFF, TiffCloser> tif(TIFFOpenW(file.wstring().c_str(), "w"));
#else
    std::unique_ptr<TIFF, TiffCloser> tif(TIFFOpen(file.c_str(), "w"));
#endif
    if (!tif) {
        throw std::runtime_error("writeTestSlide: libtiff cannot create " + file.u8string());
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
                    px[0] = static_cast<uint8_t>((tx + x) & 0xFF);
                    px[1] = static_cast<uint8_t>((ty + y) & 0xFF);
                    px[2] = static_cast<uint8_t>((tx + x + ty + y) & 0xFF);
                }
            }
            if (TIFFWriteTile(t, tile.data(), tx, ty, 0, 0) < 0) {
                throw std::runtime_error("writeTestSlide: libtiff cannot write a tile of " + file.u8string());
            }
        }
    }
    if (TIFFWriteDirectory(t) == 0) {
        throw std::runtime_error("writeTestSlide: libtiff cannot write the directory of " + file.u8string());
    }
    return file;
}

TempDirectory::TempDirectory() : path_(uniqueTempPath("workspace_test", "")) {}

TempDirectory::~TempDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
}

} // namespace slideio::processor::testing_support
