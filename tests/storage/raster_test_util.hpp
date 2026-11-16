// Test-only: tiles that own their pixels, and a raw libtiff read of one page of a raster
// file, for the raster writer and source tests.
#pragma once

#include "storage/detail/half_float.hpp"

#include <slideio/processor/core/block_view.hpp>
#include <slideio/processor/core/data_type.hpp>
#include <slideio/processor/core/geometry.hpp>

#include <gtest/gtest.h>
#include <tiffio.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace slideio::processor {

// A BlockView together with the buffers it points into. Not copyable or movable: the view
// points into `buffers`.
struct OwnedView {
    OwnedView() = default;
    OwnedView(const OwnedView&) = delete;
    OwnedView& operator=(const OwnedView&) = delete;

    std::vector<std::vector<uint8_t>> buffers;  // one per plane, rows packed
    BlockView view;
};

// Stores `value` as one sample of `type` at `out`.
inline void storeSample(DataType type, double value, uint8_t* out) {
    const auto put = [out](auto sample) { std::memcpy(out, &sample, sizeof(sample)); };
    switch (type) {
    case DataType::Byte: put(static_cast<uint8_t>(value)); break;
    case DataType::Int8: put(static_cast<int8_t>(value)); break;
    case DataType::UInt16: put(static_cast<uint16_t>(value)); break;
    case DataType::Int16: put(static_cast<int16_t>(value)); break;
    case DataType::UInt32: put(static_cast<uint32_t>(value)); break;
    case DataType::Int32: put(static_cast<int32_t>(value)); break;
    case DataType::UInt64: put(static_cast<uint64_t>(value)); break;
    case DataType::Int64: put(static_cast<int64_t>(value)); break;
    case DataType::Float16: put(detail::floatToHalf(static_cast<float>(value))); break;
    case DataType::Float32: put(static_cast<float>(value)); break;
    case DataType::Float64: put(value); break;
    default: ADD_FAILURE() << "storeSample: not a sample type"; break;
    }
}

// A tile of `width` x `height` level pixels covering `slideRect` at `level`, with
// `planes` planes of `type`; sample (x, y) of plane p holds fill(x, y, p), x and y in
// the tile's pixels.
inline std::unique_ptr<OwnedView> makeView(int32_t level, Rect64 slideRect, int64_t width, int64_t height,
                                           DataType type, int32_t planes,
                                           const std::function<double(int64_t, int64_t, int32_t)>& fill) {
    auto owned = std::make_unique<OwnedView>();
    const int64_t sampleBytes = dataTypeSizeBytes(type);
    std::vector<BlockPlane> descriptors;
    for (int32_t p = 0; p < planes; ++p) {
        std::vector<uint8_t> buffer(static_cast<size_t>(width * height * sampleBytes));
        for (int64_t y = 0; y < height; ++y)
            for (int64_t x = 0; x < width; ++x)
                storeSample(type, fill(x, y, p), buffer.data() + (y * width + x) * sampleBytes);
        owned->buffers.push_back(std::move(buffer));
        descriptors.push_back(BlockPlane{owned->buffers.back().data(), type, width * sampleBytes, p});
    }
    owned->view = BlockView(width, height, slideRect, level, descriptors.data(), planes);
    return owned;
}

// makeView with every sample of every plane set to `fill`.
inline std::unique_ptr<OwnedView> makeView(int32_t level, Rect64 slideRect, int64_t width, int64_t height,
                                           DataType type, int32_t planes, double fill) {
    return makeView(level, slideRect, width, height, type, planes,
                    [fill](int64_t, int64_t, int32_t) { return fill; });
}

// One plane of one page of a raster file, as doubles.
struct RasterPage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<double> samples;  // row-major, width x height

    double at(uint32_t x, uint32_t y) const { return samples[static_cast<size_t>(y) * width + x]; }
};

// The sample at `in` of a TIFF sample with the given format and size, as a double.
inline double loadSample(uint16_t format, uint16_t bits, const uint8_t* in) {
    const auto get = [in](auto sample) {
        std::memcpy(&sample, in, sizeof(sample));
        return static_cast<double>(sample);
    };
    if (format == SAMPLEFORMAT_IEEEFP) {
        if (bits == 16) {
            uint16_t half = 0;
            std::memcpy(&half, in, sizeof(half));
            return detail::halfToFloat(half);
        }
        return bits == 32 ? get(float()) : get(double());
    }
    const bool isSigned = format == SAMPLEFORMAT_INT;
    switch (bits) {
    case 8: return isSigned ? get(int8_t()) : get(uint8_t());
    case 16: return isSigned ? get(int16_t()) : get(uint16_t());
    case 32: return isSigned ? get(int32_t()) : get(uint32_t());
    default: return isSigned ? get(int64_t()) : get(uint64_t());
    }
}

// Reads plane `plane` of page `page` of the raster file at `path` through libtiff. Fails
// the test and returns an empty page when the file or page cannot be read.
inline RasterPage readPage(const std::filesystem::path& path, int page, uint16_t plane) {
    RasterPage result;
#ifdef _WIN32
    TIFF* tif = TIFFOpenW(path.c_str(), "r");
#else
    TIFF* tif = TIFFOpen(path.c_str(), "r");
#endif
    if (tif == nullptr) {
        ADD_FAILURE() << "readPage: cannot open '" << path.u8string() << "'";
        return result;
    }
    if (TIFFSetDirectory(tif, static_cast<tdir_t>(page)) != 1) {
        ADD_FAILURE() << "readPage: no page " << page << " in '" << path.u8string() << "'";
        TIFFClose(tif);
        return result;
    }
    uint32_t tileWidth = 0, tileHeight = 0;
    uint16_t format = SAMPLEFORMAT_UINT, bits = 8;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &result.width);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &result.height);
    TIFFGetField(tif, TIFFTAG_TILEWIDTH, &tileWidth);
    TIFFGetField(tif, TIFFTAG_TILELENGTH, &tileHeight);
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLEFORMAT, &format);
    TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &bits);
    const size_t sampleBytes = bits / 8;
    result.samples.assign(static_cast<size_t>(result.width) * result.height, 0.0);
    std::vector<uint8_t> tile(static_cast<size_t>(TIFFTileSize(tif)));
    for (uint32_t ty = 0; ty < result.height; ty += tileHeight) {
        for (uint32_t tx = 0; tx < result.width; tx += tileWidth) {
            const tmsize_t read = TIFFReadEncodedTile(tif, TIFFComputeTile(tif, tx, ty, 0, plane), tile.data(),
                                                      static_cast<tmsize_t>(tile.size()));
            if (read != static_cast<tmsize_t>(tile.size())) {
                ADD_FAILURE() << "readPage: tile (" << tx << ", " << ty << ") of page " << page << " is short";
                TIFFClose(tif);
                return RasterPage();
            }
            for (uint32_t y = ty; y < std::min(result.height, ty + tileHeight); ++y)
                for (uint32_t x = tx; x < std::min(result.width, tx + tileWidth); ++x)
                    result.samples[static_cast<size_t>(y) * result.width + x] =
                        loadSample(format, bits, tile.data() + ((y - ty) * tileWidth + (x - tx)) * sampleBytes);
        }
    }
    TIFFClose(tif);
    return result;
}

} // namespace slideio::processor
