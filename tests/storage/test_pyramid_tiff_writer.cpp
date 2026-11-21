// Requirements: UR-RES-7
// Drives detail::TileSpool, detail::writePyramidTiff and the half-float helpers (rulings
// S41, S42, S43 and S46); their sources are compiled into the test (they are not
// exported). Every tag and pixel of a written file is checked through a raw libtiff read.
#include "storage/detail/half_float.hpp"
#include "storage/detail/pyramid_tiff_writer.hpp"
#include "storage/detail/tiff_io.hpp"
#include "storage/detail/tile_spool.hpp"

#include "common/temp_path.hpp"
#include "storage/ome_raster_fixture.hpp"

#include <slideio/processor/core/data_type.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/storage/raster_store.hpp>

#include <gtest/gtest.h>
#include <rapidjson/document.h>
#include <tiffio.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace slideio::processor;

namespace {

constexpr int64_t kTile = 512;

std::string utf8(const std::filesystem::path& path) { return path.u8string(); }

// Spools a block whose planes are given as row-major vectors of width x height samples.
template <typename T>
void appendBlock(detail::TileSpool& spool, int64_t x, int64_t y, int64_t width, int64_t height,
                 const std::vector<std::vector<T>>& planes) {
    std::vector<const void*> data;
    std::vector<int64_t> strides;
    for (const std::vector<T>& plane : planes) {
        data.push_back(plane.data());
        strides.push_back(width * static_cast<int64_t>(sizeof(T)));
    }
    spool.append(Rect64{x, y, width, height}, width, height, data.data(), strides.data());
}

// A file with its spool beside it, both deleted at scope exit.
struct TempRaster {
    std::filesystem::path tif = uniqueTempPath("pyramid", ".tif");
    std::filesystem::path spoolPath = uniqueTempPath("pyramid", ".spool");
    TempFileGuard tifGuard{tif};
    TempFileGuard spoolGuard{spoolPath};
};

// Checks that every tile of the current directory has bytes.
void expectEveryTilePresent(TIFF* tif, int page) {
    uint64_t* counts = nullptr;
    ASSERT_EQ(TIFFGetField(tif, TIFFTAG_TILEBYTECOUNTS, &counts), 1) << "page " << page;
    const uint32_t tiles = TIFFNumberOfTiles(tif);
    ASSERT_GT(tiles, 0u);
    for (uint32_t i = 0; i < tiles; ++i) EXPECT_GT(counts[i], 0u) << "page " << page << ", tile " << i;
}

// One plane of one page, read tile by tile through libtiff: width x height samples.
template <typename T>
std::vector<T> readPlane(TIFF* tif, int page, uint16_t plane, uint32_t* widthOut = nullptr,
                         uint32_t* heightOut = nullptr) {
    std::vector<T> result;
    EXPECT_EQ(TIFFSetDirectory(tif, static_cast<tdir_t>(page)), 1) << "page " << page;
    uint32_t width = 0, height = 0;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &width);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &height);
    if (widthOut != nullptr) *widthOut = width;
    if (heightOut != nullptr) *heightOut = height;
    result.assign(static_cast<size_t>(width) * height, T());
    std::vector<T> tile(static_cast<size_t>(kTile * kTile));
    const tmsize_t bytes = static_cast<tmsize_t>(tile.size() * sizeof(T));
    for (uint32_t ty = 0; ty < height; ty += kTile) {
        for (uint32_t tx = 0; tx < width; tx += kTile) {
            const tmsize_t read = TIFFReadEncodedTile(tif, TIFFComputeTile(tif, tx, ty, 0, plane), tile.data(), bytes);
            EXPECT_EQ(read, bytes) << "page " << page << ", tile (" << tx << ", " << ty << ")";
            for (uint32_t y = ty; y < std::min<uint32_t>(height, ty + kTile); ++y)
                for (uint32_t x = tx; x < std::min<uint32_t>(width, tx + kTile); ++x)
                    result[static_cast<size_t>(y) * width + x] = tile[(y - ty) * kTile + (x - tx)];
        }
    }
    return result;
}

// One sample of one page, read through the tile that holds it.
template <typename T>
T sampleAt(TIFF* tif, int page, uint16_t plane, uint32_t x, uint32_t y) {
    EXPECT_EQ(TIFFSetDirectory(tif, static_cast<tdir_t>(page)), 1) << "page " << page;
    std::vector<T> tile(static_cast<size_t>(kTile * kTile));
    const tmsize_t bytes = static_cast<tmsize_t>(tile.size() * sizeof(T));
    EXPECT_EQ(TIFFReadEncodedTile(tif, TIFFComputeTile(tif, x, y, 0, plane), tile.data(), bytes), bytes);
    return tile[(y % kTile) * kTile + (x % kTile)];
}

// The value of a normal or subnormal half, decoded independently of detail::halfToFloat.
double decodeHalf(uint16_t bits) {
    const int exponent = (bits >> 10) & 0x1f;
    const int mantissa = bits & 0x3ff;
    const double magnitude = exponent == 0 ? std::ldexp(mantissa, -24) : std::ldexp(1024 + mantissa, exponent - 25);
    return (bits & 0x8000) ? -magnitude : magnitude;
}

// The spacing of halves around the positive normal `value`.
double halfUlp(double value) {
    int exponent = 0;
    std::frexp(value, &exponent); // value = f * 2^exponent, f in [0.5, 1)
    return std::ldexp(1.0, exponent - 11);
}

} // namespace

TEST(PyramidTiffWriter, LayoutTagsMatchS41) {
    constexpr int64_t width = 1025, height = 777;
    constexpr int planes = 3;
    TempRaster files;
    const detail::RasterFormat format{DataType::UInt16, planes, RasterCompression::Deflate};
    const auto value = [](int p, int64_t x, int64_t y) {
        return static_cast<uint16_t>((x * 7 + y * 13 + p * 1000 + 1) & 0xffff);
    };
    {
        detail::TileSpool spool(files.spoolPath, format);
        // Two row bands, so page 0 is composed from more than one block.
        for (const auto& band : {std::pair<int64_t, int64_t>{0, 400}, std::pair<int64_t, int64_t>{400, 377}}) {
            std::vector<std::vector<uint16_t>> data(planes, std::vector<uint16_t>(width * band.second));
            for (int p = 0; p < planes; ++p)
                for (int64_t y = 0; y < band.second; ++y)
                    for (int64_t x = 0; x < width; ++x) data[p][y * width + x] = value(p, x, band.first + y);
            appendBlock(spool, 0, band.first, width, band.second, data);
        }
        const detail::PyramidMetadata metadata{"nuclei", "a \"quoted\" description", 2, 4.0, 4.5, 0.0};
        spool.place(1.0, 1.0);
        const detail::PyramidInfo info = detail::writePyramidTiff(files.tif, spool, format, metadata);
        EXPECT_EQ(info.pages, 3);
        EXPECT_EQ(info.width, width);
        EXPECT_EQ(info.height, height);
    }

    detail::TiffHandle handle = detail::TiffHandle::openRead(utf8(files.tif));
    TIFF* tif = handle.get();
    ASSERT_NE(tif, nullptr);
    EXPECT_NE(TIFFIsBigTIFF(tif), 0);
    ASSERT_EQ(TIFFNumberOfDirectories(tif), 3u);
    const uint32_t sizes[3][2] = {{1025, 777}, {513, 389}, {257, 195}};
    for (int page = 0; page < 3; ++page) {
        SCOPED_TRACE("page " + std::to_string(page));
        ASSERT_EQ(TIFFSetDirectory(tif, static_cast<tdir_t>(page)), 1);
        uint32_t w = 0, h = 0, tileW = 0, tileH = 0, subfile = 0;
        uint16_t planar = 0, samples = 0, bits = 0, sampleFormat = 0, photometric = 0, compression = 0,
                 predictor = 0, extraCount = 0;
        uint16_t* extra = nullptr;
        TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w);
        TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);
        EXPECT_EQ(w, sizes[page][0]);
        EXPECT_EQ(h, sizes[page][1]);
        EXPECT_NE(TIFFIsTiled(tif), 0);
        TIFFGetField(tif, TIFFTAG_TILEWIDTH, &tileW);
        TIFFGetField(tif, TIFFTAG_TILELENGTH, &tileH);
        EXPECT_EQ(tileW, 512u);
        EXPECT_EQ(tileH, 512u);
        TIFFGetField(tif, TIFFTAG_SUBFILETYPE, &subfile);
        EXPECT_EQ(subfile, page == 0 ? 0u : static_cast<uint32_t>(FILETYPE_REDUCEDIMAGE));
        TIFFGetField(tif, TIFFTAG_PLANARCONFIG, &planar);
        EXPECT_EQ(planar, PLANARCONFIG_SEPARATE);
        TIFFGetField(tif, TIFFTAG_SAMPLESPERPIXEL, &samples);
        EXPECT_EQ(samples, planes);
        ASSERT_EQ(TIFFGetField(tif, TIFFTAG_EXTRASAMPLES, &extraCount, &extra), 1);
        ASSERT_EQ(extraCount, 2);
        EXPECT_EQ(extra[0], EXTRASAMPLE_UNSPECIFIED);
        EXPECT_EQ(extra[1], EXTRASAMPLE_UNSPECIFIED);
        TIFFGetField(tif, TIFFTAG_BITSPERSAMPLE, &bits);
        EXPECT_EQ(bits, 16);
        TIFFGetField(tif, TIFFTAG_SAMPLEFORMAT, &sampleFormat);
        EXPECT_EQ(sampleFormat, SAMPLEFORMAT_UINT);
        TIFFGetField(tif, TIFFTAG_PHOTOMETRIC, &photometric);
        EXPECT_EQ(photometric, PHOTOMETRIC_MINISBLACK);
        TIFFGetField(tif, TIFFTAG_COMPRESSION, &compression);
        EXPECT_EQ(compression, COMPRESSION_ADOBE_DEFLATE);
        TIFFGetField(tif, TIFFTAG_PREDICTOR, &predictor);
        EXPECT_EQ(predictor, PREDICTOR_HORIZONTAL);
        char* description = nullptr;
        if (page == 0) {
            ASSERT_EQ(TIFFGetField(tif, TIFFTAG_IMAGEDESCRIPTION, &description), 1);
            rapidjson::Document doc;
            doc.Parse(description);
            ASSERT_FALSE(doc.HasParseError()) << description;
            ASSERT_TRUE(doc.IsObject());
            EXPECT_EQ(doc["slideioProcessorRaster"].GetInt(), 1);
            EXPECT_STREQ(doc["name"].GetString(), "nuclei");
            EXPECT_STREQ(doc["description"].GetString(), "a \"quoted\" description");
            EXPECT_STREQ(doc["dataType"].GetString(), "UInt16");
            EXPECT_EQ(doc["level"].GetInt(), 2);
            EXPECT_EQ(doc["scaleX"].GetDouble(), 4.0);
            EXPECT_EQ(doc["scaleY"].GetDouble(), 4.5);
        } else {
            EXPECT_EQ(TIFFGetField(tif, TIFFTAG_IMAGEDESCRIPTION, &description), 0);
        }
        expectEveryTilePresent(tif, page);
        // Integers take the top-left sample, so page k holds page 0's sample (x·2^k, y·2^k).
        for (uint16_t p = 0; p < planes; ++p) {
            const std::vector<uint16_t> actual = readPlane<uint16_t>(tif, page, p);
            ASSERT_EQ(actual.size(), static_cast<size_t>(sizes[page][0]) * sizes[page][1]);
            size_t mismatches = 0;
            for (uint32_t y = 0; y < sizes[page][1]; ++y)
                for (uint32_t x = 0; x < sizes[page][0]; ++x)
                    if (actual[static_cast<size_t>(y) * sizes[page][0] + x] !=
                        value(p, static_cast<int64_t>(x) << page, static_cast<int64_t>(y) << page))
                        ++mismatches;
            EXPECT_EQ(mismatches, 0u) << "plane " << p;
        }
    }
}

TEST(PyramidTiffWriter, CompressionAndPredictorMapping) {
    struct Case {
        DataType type;
        RasterCompression compression;
        uint16_t tiffCompression;
        int predictor; // -1: no Predictor tag
    };
    const Case cases[] = {
        {DataType::UInt16, RasterCompression::None, COMPRESSION_NONE, -1},
        {DataType::Float32, RasterCompression::Lzw, COMPRESSION_LZW, PREDICTOR_FLOATINGPOINT},
        {DataType::Float16, RasterCompression::Deflate, COMPRESSION_ADOBE_DEFLATE, PREDICTOR_NONE},
        {DataType::Int8, RasterCompression::Lzw, COMPRESSION_LZW, PREDICTOR_HORIZONTAL},
        {DataType::Float64, RasterCompression::Deflate, COMPRESSION_ADOBE_DEFLATE, PREDICTOR_FLOATINGPOINT},
    };
    constexpr int64_t width = 600, height = 300;
    for (const Case& c : cases) {
        SCOPED_TRACE(std::string(dataTypeName(c.type)) + " compression " +
                     std::to_string(static_cast<int>(c.compression)));
        TempRaster files;
        const detail::RasterFormat format{c.type, 2, c.compression};
        const size_t sampleBytes = static_cast<size_t>(dataTypeSizeBytes(c.type));
        std::vector<std::vector<uint8_t>> data(2, std::vector<uint8_t>(width * height * sampleBytes));
        for (size_t p = 0; p < 2; ++p)
            for (size_t i = 0; i < data[p].size(); ++i) data[p][i] = static_cast<uint8_t>((i * 37 + i / 977 + p) & 0x3f);
        {
            detail::TileSpool spool(files.spoolPath, format);
            const void* planes[] = {data[0].data(), data[1].data()};
            const int64_t strides[] = {width * static_cast<int64_t>(sampleBytes), width * static_cast<int64_t>(sampleBytes)};
            spool.append(Rect64{0, 0, width, height}, width, height, planes, strides);
            spool.place(1.0, 1.0);
            detail::writePyramidTiff(files.tif, spool, format, {"r", "", 0, 1.0, 1.0, 0.0});
        }
        detail::TiffHandle handle = detail::TiffHandle::openRead(utf8(files.tif));
        TIFF* tif = handle.get();
        ASSERT_NE(tif, nullptr);
        uint16_t compression = 0, predictor = 0, sampleFormat = 0, bits = 0;
        TIFFGetField(tif, TIFFTAG_COMPRESSION, &compression);
        EXPECT_EQ(compression, c.tiffCompression);
        if (c.predictor < 0) {
            EXPECT_EQ(TIFFGetField(tif, TIFFTAG_PREDICTOR, &predictor), 0);
        } else {
            ASSERT_EQ(TIFFGetField(tif, TIFFTAG_PREDICTOR, &predictor), 1);
            EXPECT_EQ(predictor, c.predictor);
        }
        TIFFGetField(tif, TIFFTAG_SAMPLEFORMAT, &sampleFormat);
        EXPECT_EQ(sampleFormat, detail::tiffSampleFormat(c.type));
        TIFFGetField(tif, TIFFTAG_BITSPERSAMPLE, &bits);
        EXPECT_EQ(bits, sampleBytes * 8);
        // Page 0 round-trips byte for byte.
        for (uint16_t p = 0; p < 2; ++p) {
            std::vector<uint8_t> tile(static_cast<size_t>(kTile * kTile) * sampleBytes);
            for (int64_t tx = 0; tx < width; tx += kTile) {
                const tmsize_t bytes = static_cast<tmsize_t>(tile.size());
                ASSERT_EQ(TIFFReadEncodedTile(tif, TIFFComputeTile(tif, static_cast<uint32_t>(tx), 0, 0, p),
                                              tile.data(), bytes),
                          bytes);
                size_t mismatches = 0;
                for (int64_t y = 0; y < height; ++y)
                    for (int64_t x = tx; x < std::min(width, tx + kTile); ++x)
                        if (std::memcmp(&tile[((y * kTile) + (x - tx)) * sampleBytes],
                                        &data[p][(y * width + x) * sampleBytes], sampleBytes) != 0)
                            ++mismatches;
                EXPECT_EQ(mismatches, 0u) << "plane " << p << ", tile x " << tx;
            }
        }
    }
}

// A 3x3 patch in the bottom-right corner of a 2051x2051 raster: pages 2051, 1026, 513 and
// 257, and page 1's 2x2 block (1024..1025, 1024..1025) sees the patch with its right
// column and bottom row alone in their edge blocks.
TEST(PyramidTiffWriter, IntegersTakeTopLeftFloatsTakeTheMean) {
    constexpr int64_t extent = 2051, patch = 2048;
    const auto writePatch = [&](const TempRaster& files, DataType type, const void* values) {
        const detail::RasterFormat format{type, 1, RasterCompression::Deflate};
        detail::TileSpool spool(files.spoolPath, format);
        const void* planes[] = {values};
        const int64_t strides[] = {3 * dataTypeSizeBytes(type)};
        spool.append(Rect64{patch, patch, 3, 3}, 3, 3, planes, strides);
        spool.place(1.0, 1.0);
        const detail::PyramidInfo info = detail::writePyramidTiff(files.tif, spool, format, {"r", "", 0, 1.0, 1.0, 0.0});
        EXPECT_EQ(info.pages, 4);
        EXPECT_EQ(info.width, extent);
        EXPECT_EQ(info.height, extent);
    };

    {
        SCOPED_TRACE("Int64");
        TempRaster files;
        const int64_t anchor = (int64_t(1) << 62) + 1;
        const int64_t values[9] = {anchor, -2, 3, -4, 5, -6, 7, -8, (int64_t(1) << 61) + 9};
        writePatch(files, DataType::Int64, values);
        detail::TiffHandle handle = detail::TiffHandle::openRead(utf8(files.tif));
        TIFF* tif = handle.get();
        ASSERT_NE(tif, nullptr);
        EXPECT_EQ(sampleAt<int64_t>(tif, 0, 0, 2048, 2048), anchor);
        EXPECT_EQ(sampleAt<int64_t>(tif, 1, 0, 1024, 1024), anchor);
        EXPECT_EQ(sampleAt<int64_t>(tif, 1, 0, 1025, 1024), values[2]);
        EXPECT_EQ(sampleAt<int64_t>(tif, 1, 0, 1024, 1025), values[6]);
        EXPECT_EQ(sampleAt<int64_t>(tif, 1, 0, 1025, 1025), values[8]);
        EXPECT_EQ(sampleAt<int64_t>(tif, 2, 0, 512, 512), anchor);
        EXPECT_EQ(sampleAt<int64_t>(tif, 3, 0, 256, 256), anchor);
        EXPECT_EQ(sampleAt<int64_t>(tif, 3, 0, 255, 256), 0);
    }
    {
        SCOPED_TRACE("Float32");
        TempRaster files;
        float values[9];
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 3; ++x) values[y * 3 + x] = 1.25f + 10.0f * x + 100.0f * y;
        writePatch(files, DataType::Float32, values);
        detail::TiffHandle handle = detail::TiffHandle::openRead(utf8(files.tif));
        TIFF* tif = handle.get();
        ASSERT_NE(tif, nullptr);
        const auto mean = [](std::initializer_list<float> samples) {
            double sum = 0;
            for (float s : samples) sum += s;
            return static_cast<float>(sum / samples.size());
        };
        EXPECT_EQ(sampleAt<float>(tif, 1, 0, 1024, 1024), mean({values[0], values[1], values[3], values[4]}));
        EXPECT_EQ(sampleAt<float>(tif, 1, 0, 1025, 1024), mean({values[2], values[5]}));
        EXPECT_EQ(sampleAt<float>(tif, 1, 0, 1024, 1025), mean({values[6], values[7]}));
        EXPECT_EQ(sampleAt<float>(tif, 1, 0, 1025, 1025), values[8]);
        // The block left of the patch holds zeros, which are samples like any other.
        EXPECT_EQ(sampleAt<float>(tif, 1, 0, 1023, 1024), 0.0f);
    }
    {
        SCOPED_TRACE("Float16");
        TempRaster files;
        // 1.0, 1.5, 1+2^-10, 2.0, 3.0, 1+2^-9, 0.5, 0.75, 1.25
        const uint16_t values[9] = {0x3C00, 0x3E00, 0x3C01, 0x4000, 0x4200, 0x3C02, 0x3800, 0x3A00, 0x3D00};
        writePatch(files, DataType::Float16, values);
        detail::TiffHandle handle = detail::TiffHandle::openRead(utf8(files.tif));
        TIFF* tif = handle.get();
        ASSERT_NE(tif, nullptr);
        const auto expectMean = [&](uint32_t x, uint32_t y, std::initializer_list<uint16_t> samples) {
            double sum = 0;
            for (uint16_t s : samples) sum += decodeHalf(s);
            const double expected = static_cast<float>(sum / samples.size());
            const double actual = decodeHalf(sampleAt<uint16_t>(tif, 1, 0, x, y));
            EXPECT_LE(std::fabs(actual - expected), halfUlp(expected)) << "(" << x << ", " << y << ")";
        };
        expectMean(1024, 1024, {values[0], values[1], values[3], values[4]});
        expectMean(1025, 1024, {values[2], values[5]}); // a tie between two halves
        expectMean(1024, 1025, {values[6], values[7]});
        EXPECT_EQ(sampleAt<uint16_t>(tif, 1, 0, 1025, 1025), values[8]);
    }
    {
        SCOPED_TRACE("Float16 mean rounded once");
        TempRaster files;
        // 2, 2, 2^-9, 2^-24: the mean 1 + 2^-11 + 2^-26 is just above the tie between 1 and
        // 1 + 2^-10. Through a float it would become the tie itself and round to 1.
        const uint16_t values[9] = {0x4000, 0x4000, 0x3C00, 0x1800, 0x0001, 0x3C00, 0x3C00, 0x3C00, 0x3C00};
        writePatch(files, DataType::Float16, values);
        detail::TiffHandle handle = detail::TiffHandle::openRead(utf8(files.tif));
        TIFF* tif = handle.get();
        ASSERT_NE(tif, nullptr);
        EXPECT_EQ(sampleAt<uint16_t>(tif, 1, 0, 1024, 1024), 0x3C01);
    }
}

TEST(PyramidTiffWriter, TinyAndThinExtents) {
    for (const RasterCompression compression :
         {RasterCompression::None, RasterCompression::Lzw, RasterCompression::Deflate}) {
        SCOPED_TRACE("compression " + std::to_string(static_cast<int>(compression)));
        const detail::RasterFormat format{DataType::UInt16, 2, compression};
        {
            SCOPED_TRACE("1x1");
            TempRaster files;
            {
                detail::TileSpool spool(files.spoolPath, format);
                appendBlock<uint16_t>(spool, 0, 0, 1, 1, {{7}, {9}});
                spool.place(1.0, 1.0);
                const detail::PyramidInfo info = detail::writePyramidTiff(files.tif, spool, format, {"r", "", 0, 1.0, 1.0, 0.0});
                EXPECT_EQ(info.pages, 1);
                EXPECT_EQ(info.width, 1);
                EXPECT_EQ(info.height, 1);
            }
            detail::TiffHandle handle = detail::TiffHandle::openRead(utf8(files.tif));
            TIFF* tif = handle.get();
            ASSERT_NE(tif, nullptr);
            EXPECT_EQ(TIFFNumberOfDirectories(tif), 1u);
            uint32_t w = 0, h = 0;
            EXPECT_EQ(readPlane<uint16_t>(tif, 0, 0, &w, &h), std::vector<uint16_t>{7});
            EXPECT_EQ(readPlane<uint16_t>(tif, 0, 1), std::vector<uint16_t>{9});
            EXPECT_EQ(w, 1u);
            EXPECT_EQ(h, 1u);
            expectEveryTilePresent(tif, 0);
        }
        {
            SCOPED_TRACE("513x1");
            TempRaster files;
            {
                detail::TileSpool spool(files.spoolPath, format);
                // Only the last pixel is written: the first tile is never touched.
                appendBlock<uint16_t>(spool, 512, 0, 1, 1, {{300}, {301}});
                spool.place(1.0, 1.0);
                const detail::PyramidInfo info = detail::writePyramidTiff(files.tif, spool, format, {"r", "", 0, 1.0, 1.0, 0.0});
                EXPECT_EQ(info.pages, 2);
                EXPECT_EQ(info.width, 513);
                EXPECT_EQ(info.height, 1);
            }
            detail::TiffHandle handle = detail::TiffHandle::openRead(utf8(files.tif));
            TIFF* tif = handle.get();
            ASSERT_NE(tif, nullptr);
            ASSERT_EQ(TIFFNumberOfDirectories(tif), 2u);
            const uint32_t widths[] = {513, 257};
            for (int page = 0; page < 2; ++page) {
                for (uint16_t p = 0; p < 2; ++p) {
                    uint32_t w = 0, h = 0;
                    const std::vector<uint16_t> actual = readPlane<uint16_t>(tif, page, p, &w, &h);
                    EXPECT_EQ(w, widths[page]);
                    EXPECT_EQ(h, 1u);
                    std::vector<uint16_t> expected(widths[page], 0);
                    expected.back() = static_cast<uint16_t>(300 + p);
                    EXPECT_EQ(actual, expected) << "page " << page << ", plane " << p;
                }
                expectEveryTilePresent(tif, page);
            }
        }
    }
}

namespace {

// FNV-1a over a whole file: a golden fingerprint that needs no hashing library.
uint64_t fileFingerprint(const std::filesystem::path& path) {
    const std::string bytes = readFile(path);
    uint64_t hash = 0xcbf29ce484222325ull;
    for (const char c : bytes) {
        hash ^= static_cast<uint8_t>(c);
        hash *= 0x100000001b3ull;
    }
    return hash;
}

// A 700x600 spool, two pages, whose samples are a function of their coordinates, written with
// the default layout.
template <typename T>
void writeGoldenRaster(const TempRaster& files, DataType type, RasterCompression compression) {
    constexpr int64_t width = 700, height = 600;
    const detail::RasterFormat format{type, 1, compression};
    std::vector<T> plane(static_cast<size_t>(width * height));
    for (int64_t y = 0; y < height; ++y)
        for (int64_t x = 0; x < width; ++x)
            plane[static_cast<size_t>(y * width + x)] = static_cast<T>((x * 31 + y * 17) % 1000 - 300);
    detail::TileSpool spool(files.spoolPath, format);
    appendBlock<T>(spool, 0, 0, width, height, {plane});
    spool.place(1.0, 1.0);
    detail::writePyramidTiff(files.tif, spool, format, {"golden", "", 0, 1.0, 1.0, 0.0});
}

} // namespace

// The default layout's bytes, captured from the writer before the OME-TIFF layout was added:
// RasterRegistry's files (store format v2) must not change. None and LZW only: both are
// libtiff's own code, so the bytes do not depend on the zlib build.
TEST(StorageRaster, SequentialLayoutUnchanged) {
    {
        SCOPED_TRACE("Int32, None");
        TempRaster files;
        writeGoldenRaster<int32_t>(files, DataType::Int32, RasterCompression::None);
        EXPECT_EQ(std::filesystem::file_size(files.tif), 5243614u);
        EXPECT_EQ(fileFingerprint(files.tif), 15712276326121411604ull);
    }
    {
        SCOPED_TRACE("Float32, LZW");
        TempRaster files;
        writeGoldenRaster<float>(files, DataType::Float32, RasterCompression::Lzw);
        EXPECT_EQ(std::filesystem::file_size(files.tif), 401140u);
        EXPECT_EQ(fileFingerprint(files.tif), 15631184123081310431ull);
    }
}

namespace {

// One plane of the current directory (a page or a SubIFD): plane `plane`'s one strip of a
// striped directory, read whole.
template <typename T>
std::vector<T> readStripPlane(TIFF* tif, uint32_t width, uint32_t height, uint16_t plane) {
    std::vector<T> result(static_cast<size_t>(width) * height);
    const tmsize_t bytes = static_cast<tmsize_t>(result.size() * sizeof(T));
    EXPECT_EQ(TIFFReadEncodedStrip(tif, TIFFComputeStrip(tif, 0, plane), result.data(), bytes), bytes);
    return result;
}

// One plane of the current directory (a page or a SubIFD), read tile by tile, or as its one
// strip when it is striped.
std::vector<int32_t> readCurrentInt32Plane(TIFF* tif, uint32_t width, uint32_t height) {
    if (TIFFIsTiled(tif) == 0) return readStripPlane<int32_t>(tif, width, height, 0);
    std::vector<int32_t> result(static_cast<size_t>(width) * height);
    std::vector<int32_t> tile(static_cast<size_t>(kTile * kTile));
    const tmsize_t bytes = static_cast<tmsize_t>(tile.size() * sizeof(int32_t));
    for (uint32_t ty = 0; ty < height; ty += kTile)
        for (uint32_t tx = 0; tx < width; tx += kTile) {
            EXPECT_EQ(TIFFReadEncodedTile(tif, TIFFComputeTile(tif, tx, ty, 0, 0), tile.data(), bytes), bytes);
            for (uint32_t y = ty; y < std::min<uint32_t>(height, ty + kTile); ++y)
                for (uint32_t x = tx; x < std::min<uint32_t>(width, tx + kTile); ++x)
                    result[static_cast<size_t>(y) * width + x] = tile[(y - ty) * kTile + (x - tx)];
        }
    return result;
}

// The current directory's size, tiling, planes and type. A level that fits in one tile is
// one strip per plane instead (SDS 03 §3.8: SlideIO's OME-TIFF driver stretches a one-tile
// level's tile).
void expectOmeLevel(TIFF* tif, uint32_t width, uint32_t height, bool reduced) {
    uint32_t w = 0, h = 0, tileW = 0, tileH = 0, subfile = 0, rowsPerStrip = 0;
    uint16_t planar = 0, samples = 0, bits = 0, sampleFormat = 0, compression = 0;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);
    EXPECT_EQ(w, width);
    EXPECT_EQ(h, height);
    const bool oneTile = width <= 512 && height <= 512;
    if (oneTile) {
        EXPECT_EQ(TIFFIsTiled(tif), 0);
        TIFFGetField(tif, TIFFTAG_ROWSPERSTRIP, &rowsPerStrip);
        EXPECT_EQ(rowsPerStrip, height);
        EXPECT_EQ(TIFFNumberOfStrips(tif), 1u);
    } else {
        EXPECT_NE(TIFFIsTiled(tif), 0);
        TIFFGetField(tif, TIFFTAG_TILEWIDTH, &tileW);
        TIFFGetField(tif, TIFFTAG_TILELENGTH, &tileH);
        EXPECT_EQ(tileW, 512u);
        EXPECT_EQ(tileH, 512u);
    }
    TIFFGetField(tif, TIFFTAG_SUBFILETYPE, &subfile);
    EXPECT_EQ(subfile, reduced ? static_cast<uint32_t>(FILETYPE_REDUCEDIMAGE) : 0u);
    TIFFGetField(tif, TIFFTAG_PLANARCONFIG, &planar);
    EXPECT_EQ(planar, PLANARCONFIG_SEPARATE);
    TIFFGetField(tif, TIFFTAG_SAMPLESPERPIXEL, &samples);
    EXPECT_EQ(samples, 1);
    TIFFGetField(tif, TIFFTAG_BITSPERSAMPLE, &bits);
    EXPECT_EQ(bits, 32);
    TIFFGetField(tif, TIFFTAG_SAMPLEFORMAT, &sampleFormat);
    EXPECT_EQ(sampleFormat, SAMPLEFORMAT_INT);
    TIFFGetField(tif, TIFFTAG_COMPRESSION, &compression);
    EXPECT_EQ(compression, COMPRESSION_ADOBE_DEFLATE);
    if (!oneTile) expectEveryTilePresent(tif, reduced ? 1 : 0);
}

} // namespace

// Decision 1 of reporting plan C: one top-level IFD at full resolution, its reduced levels as
// SubIFDs, and minimal OME-XML in its ImageDescription.
TEST(PyramidTiffWriter, OmeLayoutUsesSubIfds) {
    TempRaster files;
    detail::PyramidMetadata metadata;
    // Markup is escaped; whitespace other than a space is kept as a character reference, which
    // attribute normalisation would otherwise turn into a space; any other control character,
    // which XML 1.0 cannot carry at all, is dropped.
    metadata.name = "mask <&\"'>\t\n\r\x01";
    metadata.micronsPerPixel = 0.25;
    const detail::PyramidInfo info = test::writeOmeRaster(files.tif, files.spoolPath, metadata);
    EXPECT_EQ(info.pages, 3);
    EXPECT_EQ(info.width, test::kOmeWidth);
    EXPECT_EQ(info.height, test::kOmeHeight);

    detail::TiffHandle handle = detail::TiffHandle::openRead(utf8(files.tif));
    TIFF* tif = handle.get();
    ASSERT_NE(tif, nullptr);
    EXPECT_NE(TIFFIsBigTIFF(tif), 0);
    ASSERT_EQ(TIFFNumberOfDirectories(tif), 1u);
    ASSERT_EQ(TIFFSetDirectory(tif, 0), 1);
    {
        SCOPED_TRACE("level 0");
        expectOmeLevel(tif, 1500, 900, false);
        const std::vector<int32_t> actual = readCurrentInt32Plane(tif, 1500, 900);
        size_t mismatches = 0;
        for (uint32_t y = 0; y < 900; ++y)
            for (uint32_t x = 0; x < 1500; ++x)
                if (actual[static_cast<size_t>(y) * 1500 + x] != test::omeSample(x, y)) ++mismatches;
        EXPECT_EQ(mismatches, 0u);
    }

    char* text = nullptr;
    ASSERT_EQ(TIFFGetField(tif, TIFFTAG_IMAGEDESCRIPTION, &text), 1);
    const std::string description = text;
    for (const char* expected :
         {"<OME ", "<Image ID=\"Image:0\" Name=\"mask &lt;&amp;&quot;&apos;&gt;&#9;&#10;&#13;\"", "DimensionOrder=\"XYCZT\"",
          "Type=\"int32\"", "SizeX=\"1500\"", "SizeY=\"900\"", "SizeC=\"1\"", "SizeZ=\"1\"", "SizeT=\"1\"",
          "Interleaved=\"false\"", "PhysicalSizeX=\"0.25\"", "PhysicalSizeXUnit=\"\xC2\xB5m\"",
          "PhysicalSizeY=\"0.25\"", "PhysicalSizeYUnit=\"\xC2\xB5m\"", "<TiffData IFD=\"0\" PlaneCount=\"1\"/>"})
        EXPECT_NE(description.find(expected), std::string::npos) << expected << " in " << description;
    // No path, file name or other identifier: nothing of the file's location, and no UUID.
    EXPECT_EQ(description.find(files.tif.filename().u8string()), std::string::npos) << description;
    EXPECT_EQ(description.find(files.tif.parent_path().filename().u8string()), std::string::npos) << description;
    EXPECT_EQ(description.find("UUID"), std::string::npos) << description;
    EXPECT_EQ(description.find("FileName"), std::string::npos) << description;

    uint16_t count = 0;
    uint64_t* offsetsRaw = nullptr;
    ASSERT_EQ(TIFFGetField(tif, TIFFTAG_SUBIFD, &count, &offsetsRaw), 1);
    ASSERT_EQ(count, 2);
    const std::vector<uint64_t> offsets(offsetsRaw, offsetsRaw + count);
    const uint32_t sizes[2][2] = {{750, 450}, {375, 225}};
    for (int level = 1; level <= 2; ++level) {
        SCOPED_TRACE("level " + std::to_string(level));
        ASSERT_NE(offsets[level - 1], 0u);
        ASSERT_EQ(TIFFSetSubDirectory(tif, offsets[level - 1]), 1);
        expectOmeLevel(tif, sizes[level - 1][0], sizes[level - 1][1], true);
        EXPECT_EQ(TIFFGetField(tif, TIFFTAG_IMAGEDESCRIPTION, &text), 0);
        // Integers take the top-left sample, as in the default layout.
        const std::vector<int32_t> actual = readCurrentInt32Plane(tif, sizes[level - 1][0], sizes[level - 1][1]);
        size_t mismatches = 0;
        for (uint32_t y = 0; y < sizes[level - 1][1]; ++y)
            for (uint32_t x = 0; x < sizes[level - 1][0]; ++x)
                if (actual[static_cast<size_t>(y) * sizes[level - 1][0] + x] !=
                    test::omeSample(static_cast<int64_t>(x) << level, static_cast<int64_t>(y) << level))
                    ++mismatches;
        EXPECT_EQ(mismatches, 0u);
    }
}

// Without a resolution the OME-XML has no physical size, and an empty name is written empty.
// Two writes of the same spool are byte-identical (UR-QA-3).
TEST(PyramidTiffWriter, OmeLayoutWithoutResolutionIsReproducible) {
    TempRaster first, second;
    test::writeOmeRaster(first.tif, first.spoolPath, detail::PyramidMetadata{});
    test::writeOmeRaster(second.tif, second.spoolPath, detail::PyramidMetadata{});
    EXPECT_EQ(readFile(first.tif), readFile(second.tif));

    detail::TiffHandle handle = detail::TiffHandle::openRead(utf8(first.tif));
    TIFF* tif = handle.get();
    ASSERT_NE(tif, nullptr);
    char* text = nullptr;
    ASSERT_EQ(TIFFGetField(tif, TIFFTAG_IMAGEDESCRIPTION, &text), 1);
    const std::string description = text;
    EXPECT_NE(description.find("<Image ID=\"Image:0\" Name=\"\""), std::string::npos) << description;
    EXPECT_EQ(description.find("PhysicalSize"), std::string::npos) << description;
}

// A single-tile raster has no reduced level, so no SubIFD tag, and is one strip per plane (an
// all-zero plane included); and OME-XML has no Float16.
TEST(PyramidTiffWriter, OmeLayoutSmallRasterAndFloat16) {
    {
        TempRaster files;
        const detail::RasterFormat format{DataType::Float32, 2, RasterCompression::Lzw};
        {
            detail::TileSpool spool(files.spoolPath, format);
            appendBlock<float>(spool, 0, 0, 3, 2, {{1.5f, -2.0f, 3.0f, 4.0f, 5.0f, 6.0f}, {0, 0, 0, 0, 0, 0}});
            spool.place(1.0, 1.0);
            const detail::PyramidInfo info =
                detail::writePyramidTiff(files.tif, spool, format, {}, detail::PyramidLayout::OmeSubIfds);
            EXPECT_EQ(info.pages, 1);
        }
        detail::TiffHandle handle = detail::TiffHandle::openRead(utf8(files.tif));
        TIFF* tif = handle.get();
        ASSERT_NE(tif, nullptr);
        EXPECT_EQ(TIFFNumberOfDirectories(tif), 1u);
        uint16_t count = 0;
        uint64_t* offsets = nullptr;
        EXPECT_EQ(TIFFGetField(tif, TIFFTAG_SUBIFD, &count, &offsets), 0);
        char* text = nullptr;
        ASSERT_EQ(TIFFGetField(tif, TIFFTAG_IMAGEDESCRIPTION, &text), 1);
        const std::string description = text;
        EXPECT_NE(description.find("Type=\"float\""), std::string::npos) << description;
        EXPECT_NE(description.find("SizeC=\"2\""), std::string::npos) << description;
        EXPECT_EQ(TIFFIsTiled(tif), 0);
        EXPECT_EQ(TIFFNumberOfStrips(tif), 2u);
        EXPECT_EQ(readStripPlane<float>(tif, 3, 2, 0), (std::vector<float>{1.5f, -2.0f, 3.0f, 4.0f, 5.0f, 6.0f}));
        EXPECT_EQ(readStripPlane<float>(tif, 3, 2, 1), (std::vector<float>(6, 0.0f)));
    }
    {
        TempRaster files;
        const detail::RasterFormat format{DataType::Float16, 1, RasterCompression::None};
        detail::TileSpool spool(files.spoolPath, format);
        appendBlock<uint16_t>(spool, 0, 0, 1, 1, {{0x3C00}});
        spool.place(1.0, 1.0);
        EXPECT_THROW(detail::writePyramidTiff(files.tif, spool, format, {}, detail::PyramidLayout::OmeSubIfds),
                     UnsupportedDataTypeError);
        EXPECT_FALSE(std::filesystem::exists(files.tif));
    }
}

TEST(TileSpool, FragmentsComposeAcrossGridLines) {
    constexpr int64_t ox = 300, oy = 700, size = 1000;
    constexpr int planes = 2;
    const std::filesystem::path path = uniqueTempPath("spool", ".spool");
    TempFileGuard guard(path);
    detail::TileSpool spool(path, {DataType::UInt16, planes, RasterCompression::Deflate});

    // The reference image over the extent, built as the appends go.
    const int64_t extentW = ox + size, extentH = oy + size;
    std::vector<std::vector<uint16_t>> expected(planes, std::vector<uint16_t>(extentW * extentH, 0));

    // A block with a row stride wider than its row, so the stride is honoured.
    const auto append = [&](int64_t x0, int64_t y0, int64_t w, int64_t h, auto value) {
        const int64_t stride = w + 3;
        std::vector<std::vector<uint16_t>> data(planes, std::vector<uint16_t>(stride * h, 0xEEEE));
        for (int p = 0; p < planes; ++p)
            for (int64_t y = 0; y < h; ++y)
                for (int64_t x = 0; x < w; ++x) {
                    const uint16_t v = value(p, x0 + x, y0 + y);
                    data[p][y * stride + x] = v;
                    expected[p][(y0 + y) * extentW + x0 + x] = v;
                }
        const void* ptrs[] = {data[0].data(), data[1].data()};
        const int64_t strides[] = {stride * 2, stride * 2};
        spool.append(Rect64{x0, y0, w, h}, w, h, ptrs, strides);
    };
    append(ox, oy, size, size, [](int p, int64_t x, int64_t y) {
        return static_cast<uint16_t>((x * 3 + y * 5 + p * 7000 + 1) & 0xffff);
    });
    // A later block straddling the column-1 grid line overwrites the first one.
    append(480, 1000, 100, 100, [](int p, int64_t, int64_t) { return static_cast<uint16_t>(0xABCD + p); });

    spool.place(1.0, 1.0);
    EXPECT_EQ(spool.extentWidth(), 1300);
    EXPECT_EQ(spool.extentHeight(), 1700);

    std::vector<std::pair<int32_t, int32_t>> visited;
    size_t mismatches = 0;
    spool.forEachGridTile([&](int32_t gx, int32_t gy, const std::vector<std::vector<uint8_t>>& tile) {
        visited.emplace_back(gx, gy);
        ASSERT_EQ(tile.size(), static_cast<size_t>(planes));
        for (int p = 0; p < planes; ++p) {
            ASSERT_EQ(tile[p].size(), static_cast<size_t>(kTile * kTile * 2));
            const auto* samples = reinterpret_cast<const uint16_t*>(tile[p].data());
            for (int64_t y = 0; y < kTile; ++y)
                for (int64_t x = 0; x < kTile; ++x) {
                    const int64_t ax = gx * kTile + x, ay = gy * kTile + y;
                    const uint16_t want = ax < extentW && ay < extentH ? expected[p][ay * extentW + ax] : 0;
                    if (samples[y * kTile + x] != want) ++mismatches;
                }
        }
    });
    EXPECT_EQ(mismatches, 0u);
    const std::vector<std::pair<int32_t, int32_t>> order = {{0, 1}, {1, 1}, {2, 1}, {0, 2}, {1, 2},
                                                            {2, 2}, {0, 3}, {1, 3}, {2, 3}};
    EXPECT_EQ(visited, order);

    spool.remove();
    EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(TileSpool, ConcurrentAppendsAreAllKept) {
    constexpr int threads = 8, perThread = 50, block = 100, across = 20;
    constexpr int64_t ox = 37, oy = 59;
    const std::filesystem::path path = uniqueTempPath("spool_mt", ".spool");
    TempFileGuard guard(path);
    detail::TileSpool spool(path, {DataType::UInt32, 1, RasterCompression::Deflate});
    const auto value = [](int64_t x, int64_t y) { return static_cast<uint32_t>(y * 4096 + x + 1); };

    std::vector<std::thread> workers;
    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&, t] {
            for (int i = 0; i < perThread; ++i) {
                const int index = t * perThread + i;
                const int64_t x0 = ox + (index % across) * block, y0 = oy + (index / across) * block;
                std::vector<std::vector<uint32_t>> data(1, std::vector<uint32_t>(block * block));
                for (int64_t y = 0; y < block; ++y)
                    for (int64_t x = 0; x < block; ++x) data[0][y * block + x] = value(x0 + x, y0 + y);
                appendBlock(spool, x0, y0, block, block, data);
            }
        });
    }
    for (std::thread& worker : workers) worker.join();

    const int64_t extentW = ox + across * block, extentH = oy + (threads * perThread / across) * block;
    spool.place(1.0, 1.0);
    EXPECT_EQ(spool.extentWidth(), extentW);
    EXPECT_EQ(spool.extentHeight(), extentH);
    size_t mismatches = 0, tiles = 0;
    spool.forEachGridTile([&](int32_t gx, int32_t gy, const std::vector<std::vector<uint8_t>>& tile) {
        ++tiles;
        const auto* samples = reinterpret_cast<const uint32_t*>(tile[0].data());
        for (int64_t y = 0; y < kTile; ++y)
            for (int64_t x = 0; x < kTile; ++x) {
                const int64_t ax = gx * kTile + x, ay = gy * kTile + y;
                const bool inside = ax >= ox && ax < extentW && ay >= oy && ay < extentH;
                if (samples[y * kTile + x] != (inside ? value(ax, ay) : 0u)) ++mismatches;
            }
    });
    EXPECT_EQ(mismatches, 0u);
    EXPECT_EQ(tiles, 20u); // 2037 x 2059 reaches grid columns 0..3 and rows 0..4
    spool.remove();
}

// The spool cuts nothing at append time: place() maps every call's slideRect to level
// pixels at the scale it is given, rounding to the nearest pixel (half away from zero),
// and a call that straddles grid lines reaches every tile it touches.
TEST(TileSpool, PlaceMapsSlideRectsAtTheScale) {
    const std::filesystem::path path = uniqueTempPath("spool_place", ".spool");
    TempFileGuard guard(path);
    detail::TileSpool spool(path, {DataType::Int32, 1, RasterCompression::Deflate});
    constexpr int64_t w = 600, h = 300;
    std::vector<int32_t> data(w * h);
    for (int64_t i = 0; i < w * h; ++i) data[i] = static_cast<int32_t>(i + 1);
    const void* planes[] = {data.data()};
    const int64_t strides[] = {w * 4};
    // At scale 4: origin (1601 / 4, 2002 / 4) = (400.25, 500.5), which rounds to (400, 501).
    spool.append(Rect64{1601, 2002, w * 4, h * 4}, w, h, planes, strides);

    EXPECT_EQ(spool.extentWidth(), 0);  // nothing is placed yet
    EXPECT_THROW(spool.forEachGridTile([](int32_t, int32_t, const std::vector<std::vector<uint8_t>>&) {}),
                 InternalError);
    EXPECT_THROW(spool.place(0.0, 4.0), InternalError);
    size_t calls = 0;
    spool.forEachCall([&](const Rect64& rect, int64_t width, int64_t height) {
        ++calls;
        EXPECT_EQ(rect.x, 1601);
        EXPECT_EQ(rect.width, w * 4);
        EXPECT_EQ(width, w);
        EXPECT_EQ(height, h);
    });
    EXPECT_EQ(calls, 1u);

    spool.place(4.0, 4.0);
    EXPECT_EQ(spool.extentWidth(), 400 + w);
    EXPECT_EQ(spool.extentHeight(), 501 + h);
    std::vector<std::pair<int32_t, int32_t>> visited;
    size_t mismatches = 0;
    spool.forEachGridTile([&](int32_t gx, int32_t gy, const std::vector<std::vector<uint8_t>>& tile) {
        visited.emplace_back(gx, gy);
        const auto* samples = reinterpret_cast<const int32_t*>(tile[0].data());
        for (int64_t y = 0; y < kTile; ++y)
            for (int64_t x = 0; x < kTile; ++x) {
                const int64_t cx = gx * kTile + x - 400, cy = gy * kTile + y - 501;
                const int32_t want = cx >= 0 && cx < w && cy >= 0 && cy < h ? data[cy * w + cx] : 0;
                if (samples[y * kTile + x] != want) ++mismatches;
            }
    });
    EXPECT_EQ(mismatches, 0u);
    const std::vector<std::pair<int32_t, int32_t>> order = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    EXPECT_EQ(visited, order);

    // Placed again at another scale: the origin moves with it. Negative origins refuse.
    spool.place(2.0, 2.0);
    EXPECT_EQ(spool.extentWidth(), 801 + w);
    EXPECT_EQ(spool.extentHeight(), 1001 + h);
    spool.append(Rect64{-8, 0, 4, 4}, 2, 2, planes, strides);
    EXPECT_THROW(spool.place(4.0, 4.0), InternalError);
    spool.remove();
}

// A call is spooled as 512 x 512 pieces on its own grid, and a grid tile decodes only the
// pieces it meets: with unaligned origins each piece meets at most 4 tiles, so the pieces
// decoded stay within 4 x the pieces stored, and two overlapping calls still compose with
// the later one on top.
TEST(TileSpool, DecodesEachPieceAtMostFourTimes) {
    constexpr int planes = 2;
    const std::filesystem::path path = uniqueTempPath("spool_pieces", ".spool");
    TempFileGuard guard(path);
    detail::TileSpool spool(path, {DataType::Byte, planes, RasterCompression::Deflate});
    struct Call {
        int64_t x, y, w, h;
        int salt;
    };
    // The first spans 3 x 3 grid tiles and 3 x 3 pieces; the second, appended later, lies
    // over it across both grid lines and covers 2 x 2 pieces.
    const Call calls[] = {{100, 200, 1300, 1250, 0}, {400, 450, 700, 600, 1}};
    const auto value = [](const Call& c, int p, int64_t x, int64_t y) {
        return static_cast<uint8_t>((x * 7 + y * 13 + p * 101 + c.salt * 59) & 0xff);
    };
    for (const Call& c : calls) {
        std::vector<std::vector<uint8_t>> data(planes, std::vector<uint8_t>(c.w * c.h));
        for (int p = 0; p < planes; ++p)
            for (int64_t y = 0; y < c.h; ++y)
                for (int64_t x = 0; x < c.w; ++x) data[p][y * c.w + x] = value(c, p, x, y);
        appendBlock(spool, c.x, c.y, c.w, c.h, data);
    }
    EXPECT_EQ(spool.piecesStored(), (9 + 4) * planes);
    spool.place(1.0, 1.0);

    std::vector<std::pair<int32_t, int32_t>> visited;
    size_t mismatches = 0;
    spool.forEachGridTile([&](int32_t gx, int32_t gy, const std::vector<std::vector<uint8_t>>& tile) {
        visited.emplace_back(gx, gy);
        for (int p = 0; p < planes; ++p)
            for (int64_t y = 0; y < kTile; ++y)
                for (int64_t x = 0; x < kTile; ++x) {
                    const int64_t ax = gx * kTile + x, ay = gy * kTile + y;
                    uint8_t want = 0;
                    for (const Call& c : calls)
                        if (ax >= c.x && ax < c.x + c.w && ay >= c.y && ay < c.y + c.h)
                            want = value(c, p, ax - c.x, ay - c.y);
                    if (tile[p][y * kTile + x] != want) ++mismatches;
                }
    });
    EXPECT_EQ(mismatches, 0u);
    EXPECT_EQ(visited.size(), 9u);
    EXPECT_GT(spool.piecesDecoded(), 0);
    EXPECT_LE(spool.piecesDecoded(), 4 * spool.piecesStored());
    spool.remove();
}

TEST(HalfFloat, RoundTripsEveryHalfAndRoundsToNearestEven) {
    size_t mismatches = 0;
    for (uint32_t bits = 0; bits <= 0xffff; ++bits) {
        const auto half = static_cast<uint16_t>(bits);
        const bool nan = (half & 0x7C00) == 0x7C00 && (half & 0x03ff) != 0;
        const float value = detail::halfToFloat(half);
        if (nan) {
            if (!std::isnan(value) || (detail::floatToHalf(value) & 0x7C00) != 0x7C00 ||
                (detail::floatToHalf(value) & 0x03ff) == 0)
                ++mismatches;
            continue;
        }
        if ((half & 0x7C00) == 0x7C00) {
            if (!std::isinf(value) || std::signbit(value) != ((half & 0x8000) != 0)) ++mismatches;
        } else if (static_cast<double>(value) != decodeHalf(half)) {
            ++mismatches;
        }
        if (detail::floatToHalf(value) != half) ++mismatches;
    }
    EXPECT_EQ(mismatches, 0u);

    EXPECT_EQ(detail::floatToHalf(1.0f + std::ldexp(1.0f, -11)), 0x3C00);       // tie, to even
    EXPECT_EQ(detail::floatToHalf(1.0f + 3 * std::ldexp(1.0f, -11)), 0x3C02);   // tie, to even
    EXPECT_EQ(detail::floatToHalf(1.0f + std::ldexp(1.0f, -11) * 1.01f), 0x3C01); // above the tie
    EXPECT_EQ(detail::floatToHalf(65519.0f), 0x7BFF);                            // below the tie
    EXPECT_EQ(detail::floatToHalf(65520.0f), 0x7C00);                            // tie, to infinity
    EXPECT_EQ(detail::floatToHalf(-1e10f), 0xFC00);
    EXPECT_EQ(detail::floatToHalf(std::ldexp(1.0f, -25)), 0x0000);               // tie, to even zero
    EXPECT_EQ(detail::floatToHalf(std::ldexp(1.5f, -25)), 0x0001);
    EXPECT_EQ(detail::floatToHalf(std::ldexp(3.0f, -25)), 0x0002);               // tie, to even
    EXPECT_EQ(detail::floatToHalf(-std::ldexp(1.0f, -30)), 0x8000);
    EXPECT_EQ(detail::floatToHalf(std::ldexp(1023.5f, -24)), 0x0400);           // to the smallest normal
}

TEST(HalfFloat, DoubleToHalfRoundsOnce) {
    // The reviewer's mean of 2, 2, 2^-9 and 2^-24: through a float it lands on the tie.
    const double mean = 1.0 + std::ldexp(1.0, -11) + std::ldexp(1.0, -26);
    EXPECT_EQ(detail::floatToHalf(static_cast<float>(mean)), 0x3C00);
    EXPECT_EQ(detail::doubleToHalf(mean), 0x3C01);

    const double tiny = std::ldexp(1.0, -40);
    EXPECT_EQ(detail::doubleToHalf(1.0 + std::ldexp(1.0, -11)), 0x3C00);            // tie, to even
    EXPECT_EQ(detail::doubleToHalf(1.0 + std::ldexp(1.0, -11) + tiny), 0x3C01);     // above the tie
    EXPECT_EQ(detail::doubleToHalf(1.0 + std::ldexp(1.0, -11) - tiny), 0x3C00);     // below the tie
    EXPECT_EQ(detail::doubleToHalf(1.0 + 3 * std::ldexp(1.0, -11)), 0x3C02);        // tie, to even
    EXPECT_EQ(detail::doubleToHalf(1.0 + 3 * std::ldexp(1.0, -11) - tiny), 0x3C01); // below the tie
    EXPECT_EQ(detail::doubleToHalf(2.0 - std::ldexp(1.0, -12)), 0x4000);            // tie, carries the exponent

    // Subnormals, in units of 2^-24.
    EXPECT_EQ(detail::doubleToHalf(std::ldexp(1.0, -25)), 0x0000);                  // tie, to even zero
    EXPECT_EQ(detail::doubleToHalf(std::ldexp(1.0, -25) + std::ldexp(1.0, -70)), 0x0001);
    EXPECT_EQ(detail::doubleToHalf(std::ldexp(1.0, -24)), 0x0001);
    EXPECT_EQ(detail::doubleToHalf(std::ldexp(3.0, -25)), 0x0002);                  // tie, to even
    EXPECT_EQ(detail::doubleToHalf(std::ldexp(5.0, -25)), 0x0002);                  // tie, to even
    EXPECT_EQ(detail::doubleToHalf(std::ldexp(1023.0, -24)), 0x03FF);               // largest subnormal
    EXPECT_EQ(detail::doubleToHalf(std::ldexp(1023.5, -24) - std::ldexp(1.0, -60)), 0x03FF);
    EXPECT_EQ(detail::doubleToHalf(std::ldexp(1023.5, -24)), 0x0400);               // to the smallest normal
    EXPECT_EQ(detail::doubleToHalf(std::ldexp(1.0, -14)), 0x0400);
    EXPECT_EQ(detail::doubleToHalf(std::ldexp(1.0, -1074)), 0x0000);                // a double subnormal
    EXPECT_EQ(detail::doubleToHalf(-std::ldexp(1.0, -30)), 0x8000);

    // The largest finite half and overflow.
    EXPECT_EQ(detail::doubleToHalf(65504.0), 0x7BFF);
    EXPECT_EQ(detail::doubleToHalf(65520.0 - std::ldexp(1.0, -30)), 0x7BFF);        // below the tie
    EXPECT_EQ(detail::doubleToHalf(65520.0), 0x7C00);                               // tie, to infinity
    EXPECT_EQ(detail::doubleToHalf(-65520.0), 0xFC00);
    EXPECT_EQ(detail::doubleToHalf(1e300), 0x7C00);
    EXPECT_EQ(detail::doubleToHalf(std::numeric_limits<double>::infinity()), 0x7C00);
    EXPECT_EQ(detail::doubleToHalf(-std::numeric_limits<double>::infinity()), 0xFC00);

    // Zeros keep their sign; a NaN stays a quiet NaN with its sign, even a signalling one whose
    // payload lies only in bits a half drops.
    EXPECT_EQ(detail::doubleToHalf(0.0), 0x0000);
    EXPECT_EQ(detail::doubleToHalf(-0.0), 0x8000);
    const double quiet = std::numeric_limits<double>::quiet_NaN();
    EXPECT_EQ(detail::doubleToHalf(quiet) & 0x7E00, 0x7E00);
    EXPECT_EQ(detail::doubleToHalf(quiet) & 0x8000, 0);
    EXPECT_EQ(detail::doubleToHalf(-quiet) & 0xFE00, 0xFE00);
    const uint64_t signallingBits = 0xFFF0000000000001ull;
    double signalling = 0;
    std::memcpy(&signalling, &signallingBits, sizeof signalling);
    EXPECT_EQ(detail::doubleToHalf(signalling), 0xFE00);

    // A float widens to a double exactly, so there is nothing to round twice and the two
    // conversions agree on every float: each upper 18-bit pattern with the low 13 bits that
    // decide normal rounding (zero, the tie and their neighbours), for both signs, and then
    // random patterns for the subnormals, which round on higher bits.
    size_t mismatches = 0;
    const auto agree = [&](uint32_t floatBits) {
        float value = 0;
        std::memcpy(&value, &floatBits, sizeof value);
        if (detail::doubleToHalf(static_cast<double>(value)) != detail::floatToHalf(value)) ++mismatches;
    };
    const uint32_t lows[] = {0x0000, 0x0001, 0x0800, 0x0fff, 0x1000, 0x1001, 0x17ff, 0x1fff};
    for (uint32_t high = 0; high < (1u << 18); ++high)
        for (const uint32_t low : lows) {
            agree((high << 13) | low);
            agree(0x80000000u | (high << 13) | low);
        }
    std::mt19937 random(43);
    for (int i = 0; i < 1 << 22; ++i) agree(static_cast<uint32_t>(random()));
    EXPECT_EQ(mismatches, 0u);
}
