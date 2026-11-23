// Requirements: UR-RES-7, UR-BLD-5
// Drives detail::TiffHandle, the libtiff wrapper behind raster I/O (rulings S40, S41, S52);
// its source is compiled into the test (it is not exported).
#include "storage/detail/tiff_io.hpp"

#include "common/temp_path.hpp"

#include <slideio/processor/core/data_type.hpp>
#include <slideio/processor/core/error.hpp>

#include <gtest/gtest.h>
#include <tiffio.h>
#include <zlib.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace slideio::processor;

namespace {

constexpr uint32_t kTile = 512;

// The UTF-8 form of a path, as the wrapper takes it.
std::string utf8(const std::filesystem::path& path) { return path.u8string(); }

// A tile of UInt16 samples whose values depend on the tile, the plane and the position.
std::vector<uint16_t> patternTile(uint32_t x, uint32_t y, uint16_t plane) {
    std::vector<uint16_t> tile(kTile * kTile);
    for (size_t i = 0; i < tile.size(); ++i)
        tile[i] = static_cast<uint16_t>(i * 7 + x * 3 + y * 5 + plane * 1000);
    return tile;
}

// Writes a 600x300 BigTIFF, tiled 512, 2 separate UInt16 planes, Deflate, every tile
// filled with patternTile.
void writeSample(const std::string& path) {
    detail::TiffHandle handle = detail::TiffHandle::openWrite(path);
    TIFF* tif = handle.get();
    ASSERT_NE(tif, nullptr);
    const uint16_t extra[] = {EXTRASAMPLE_UNSPECIFIED};
    ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, 600u), 1);
    ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGELENGTH, 300u), 1);
    ASSERT_EQ(TIFFSetField(tif, TIFFTAG_TILEWIDTH, kTile), 1);
    ASSERT_EQ(TIFFSetField(tif, TIFFTAG_TILELENGTH, kTile), 1);
    ASSERT_EQ(TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 16), 1);
    ASSERT_EQ(TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_UINT), 1);
    ASSERT_EQ(TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 2), 1);
    ASSERT_EQ(TIFFSetField(tif, TIFFTAG_EXTRASAMPLES, 1, extra), 1);
    ASSERT_EQ(TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK), 1);
    ASSERT_EQ(TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_SEPARATE), 1);
    ASSERT_EQ(TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE), 1);
    for (uint16_t plane = 0; plane < 2; ++plane) {
        for (uint32_t x : {0u, kTile}) {
            std::vector<uint16_t> tile = patternTile(x, 0, plane);
            const tmsize_t bytes = static_cast<tmsize_t>(tile.size() * sizeof(uint16_t));
            ASSERT_EQ(TIFFWriteEncodedTile(tif, TIFFComputeTile(tif, x, 0, 0, plane), tile.data(), bytes), bytes);
        }
    }
    handle.close();
    EXPECT_EQ(handle.get(), nullptr);
}

} // namespace

TEST(TiffIo, WritesAndReadsATiledBigTiff) {
    const std::filesystem::path path = uniqueTempPath(u8"ü_日本", ".tif");
    TempFileGuard guard(path);
    writeSample(utf8(path));
    if (HasFatalFailure()) return;
    ASSERT_TRUE(std::filesystem::exists(path));

    detail::TiffHandle handle = detail::TiffHandle::openRead(utf8(path));
    TIFF* tif = handle.get();
    ASSERT_NE(tif, nullptr);
    EXPECT_NE(TIFFIsBigTIFF(tif), 0);
    EXPECT_NE(TIFFIsTiled(tif), 0);
    uint32_t width = 0, height = 0;
    uint16_t samples = 0, planar = 0;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &width);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &height);
    TIFFGetField(tif, TIFFTAG_SAMPLESPERPIXEL, &samples);
    TIFFGetField(tif, TIFFTAG_PLANARCONFIG, &planar);
    EXPECT_EQ(width, 600u);
    EXPECT_EQ(height, 300u);
    EXPECT_EQ(samples, 2);
    EXPECT_EQ(planar, PLANARCONFIG_SEPARATE);
    for (uint16_t plane = 0; plane < 2; ++plane) {
        for (uint32_t x : {0u, kTile}) {
            const std::vector<uint16_t> expected = patternTile(x, 0, plane);
            std::vector<uint16_t> actual(expected.size());
            const tmsize_t bytes = static_cast<tmsize_t>(actual.size() * sizeof(uint16_t));
            ASSERT_EQ(TIFFReadEncodedTile(tif, TIFFComputeTile(tif, x, 0, 0, plane), actual.data(), bytes), bytes)
                << "plane " << plane << ", x " << x;
            EXPECT_EQ(actual, expected) << "plane " << plane << ", x " << x;
        }
    }
    handle.close();
}

TEST(TiffIo, ErrorsBecomeIoErrorWithLibtiffText) {
    const std::filesystem::path path = uniqueTempPath("not_a_tiff", ".tif");
    TempFileGuard guard(path);
    writeFile(path, "hello");

    testing::internal::CaptureStderr();
    bool thrown = false;
    try {
        detail::TiffHandle::openRead(utf8(path));
    } catch (const IoError& e) {
        thrown = true;
        // The raster file is named by its role and its path; libtiff, given the path as its
        // module name, prefixes its own text with it.
        const std::string expected = "TIFFClientOpen: libtiff failed on the raster file (" + utf8(path) + "); cause: ";
        EXPECT_EQ(std::string(e.what()).rfind(expected, 0), 0u) << e.what();
        ASSERT_NE(e.cause(), nullptr);
        EXPECT_STRNE(e.cause(), "");
        EXPECT_EQ(std::string(e.cause()).rfind(utf8(path) + ": ", 0), 0u) << e.cause();
    }
    const std::string err = testing::internal::GetCapturedStderr();
    EXPECT_TRUE(thrown);
    EXPECT_EQ(err, "");
}

TEST(TiffIo, MissingFileIsIoErrorNamingTheRasterFile) {
    const std::filesystem::path path = uniqueTempPath("missing", ".tif"); // never created
    try {
        detail::TiffHandle::openRead(utf8(path));
        FAIL() << "no IoError";
    } catch (const IoError& e) {
        const std::string expected = "TIFFOpen: cannot open the raster file (" + utf8(path) + "); cause: ";
        EXPECT_EQ(std::string(e.what()).rfind(expected, 0), 0u) << e.what();
    }
}

TEST(TiffIo, FailNamesTheOperationAndClearsTheCollectedText) {
    const std::filesystem::path path = uniqueTempPath("fail", ".tif");
    TempFileGuard guard(path);
    writeSample(utf8(path));
    if (HasFatalFailure()) return;

    detail::TiffHandle handle = detail::TiffHandle::openRead(utf8(path));
    std::vector<uint16_t> buffer(kTile * kTile);
    testing::internal::CaptureStderr();
    // Tile 99 does not exist: libtiff reports an error and returns -1.
    ASSERT_EQ(TIFFReadEncodedTile(handle.get(), 99, buffer.data(), -1), -1);
    try {
        handle.fail("TIFFReadEncodedTile");
        FAIL() << "no IoError";
    } catch (const IoError& e) {
        const std::string expected = "TIFFReadEncodedTile: libtiff failed on the raster file (" + utf8(path) + "); cause: ";
        EXPECT_EQ(std::string(e.what()).rfind(expected, 0), 0u) << e.what();
        ASSERT_NE(e.cause(), nullptr);
        EXPECT_STRNE(e.cause(), "");
    }
    // The text was consumed: a second failure without a new libtiff error carries none.
    try {
        handle.fail("again");
        FAIL() << "no IoError";
    } catch (const IoError& e) {
        EXPECT_TRUE(e.cause() == nullptr || std::strlen(e.cause()) == 0) << e.cause();
    }
    EXPECT_EQ(testing::internal::GetCapturedStderr(), "");
    handle.close();
}

TEST(TiffIo, HandlesOnManyThreadsKeepTheirOwnErrors) {
    const std::filesystem::path valid = uniqueTempPath("valid", ".tif");
    TempFileGuard validGuard(valid);
    writeSample(utf8(valid));
    if (HasFatalFailure()) return;

    constexpr int kThreads = 8;
    constexpr int kIterations = 40;
    std::vector<std::filesystem::path> garbage;
    std::vector<std::unique_ptr<TempFileGuard>> guards;
    for (int i = 0; i < kThreads / 2; ++i) {
        garbage.push_back(uniqueTempPath("garbage", ".tif"));
        guards.push_back(std::make_unique<TempFileGuard>(garbage.back()));
        // Two equal bytes lead the file, so libtiff's "bad magic number" text is this file's own:
        // 65 + i, twice, is 0x4141, 0x4242, ... The error names the file by its role and its own path.
        writeFile(garbage.back(), std::string(2, static_cast<char>(65 + i)) + " garbage");
    }

    std::atomic<int> validFailures{0};
    std::atomic<int> garbageMisreports{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kIterations; ++i) {
                if (t % 2 == 0) {
                    try {
                        detail::TiffHandle handle = detail::TiffHandle::openRead(utf8(valid));
                        std::vector<uint16_t> tile(kTile * kTile);
                        const tmsize_t bytes = static_cast<tmsize_t>(tile.size() * sizeof(uint16_t));
                        if (TIFFReadEncodedTile(handle.get(), 0, tile.data(), bytes) != bytes ||
                            tile != patternTile(0, 0, 0))
                            ++validFailures;
                        handle.close();
                    } catch (...) {
                        ++validFailures;
                    }
                } else {
                    const size_t ownIndex = static_cast<size_t>(t / 2);
                    const std::string own = utf8(garbage[ownIndex]);
                    const auto magicText = [](size_t index) {
                        return "bad magic number " + std::to_string((65 + index) * 257) + " ";
                    };
                    try {
                        detail::TiffHandle::openRead(own);
                        ++garbageMisreports;
                    } catch (const IoError& e) {
                        const std::string what = e.what();
                        // The error names the raster file by its role and its own path, and carries this
                        // handle's own libtiff text: its file's magic number, and no other file's.
                        const std::string expected = "TIFFClientOpen: libtiff failed on the raster file (" + own + ")";
                        if (what.rfind(expected, 0) != 0 ||
                            what.find(magicText(ownIndex)) == std::string::npos)
                            ++garbageMisreports;
                        for (size_t other = 0; other < garbage.size(); ++other)
                            if (other != ownIndex && (what.find(magicText(other)) != std::string::npos ||
                                                      what.find(utf8(garbage[other])) != std::string::npos))
                                ++garbageMisreports;
                    } catch (...) {
                        ++garbageMisreports;
                    }
                }
            }
        });
    }
    for (auto& thread : threads) thread.join();
    EXPECT_EQ(validFailures.load(), 0);
    EXPECT_EQ(garbageMisreports.load(), 0);
}

// The raster spool compresses with zlib directly, so its headers must reach storage code,
// not only libtiff's link line.
TEST(TiffIo, ZlibHeadersAreAvailable) {
    ASSERT_NE(zlibVersion(), nullptr);
    EXPECT_EQ(zlibVersion()[0], ZLIB_VERSION[0]);
}

TEST(TiffIo, SampleFormatMapping) {
    using detail::tiffBitsPerSample;
    using detail::tiffSampleFormat;
    EXPECT_EQ(tiffSampleFormat(DataType::Int8), SAMPLEFORMAT_INT);
    EXPECT_EQ(tiffBitsPerSample(DataType::Int8), 8);
    EXPECT_EQ(tiffSampleFormat(DataType::UInt64), SAMPLEFORMAT_UINT);
    EXPECT_EQ(tiffBitsPerSample(DataType::UInt64), 64);
    EXPECT_EQ(tiffSampleFormat(DataType::Float16), SAMPLEFORMAT_IEEEFP);
    EXPECT_EQ(tiffBitsPerSample(DataType::Float16), 16);
    // The rest of S41's table.
    EXPECT_EQ(tiffSampleFormat(DataType::Byte), SAMPLEFORMAT_UINT);
    EXPECT_EQ(tiffSampleFormat(DataType::UInt16), SAMPLEFORMAT_UINT);
    EXPECT_EQ(tiffSampleFormat(DataType::UInt32), SAMPLEFORMAT_UINT);
    EXPECT_EQ(tiffSampleFormat(DataType::Int16), SAMPLEFORMAT_INT);
    EXPECT_EQ(tiffSampleFormat(DataType::Int32), SAMPLEFORMAT_INT);
    EXPECT_EQ(tiffSampleFormat(DataType::Int64), SAMPLEFORMAT_INT);
    EXPECT_EQ(tiffSampleFormat(DataType::Float32), SAMPLEFORMAT_IEEEFP);
    EXPECT_EQ(tiffSampleFormat(DataType::Float64), SAMPLEFORMAT_IEEEFP);
    EXPECT_EQ(tiffBitsPerSample(DataType::Byte), 8);
    EXPECT_EQ(tiffBitsPerSample(DataType::Int32), 32);
    EXPECT_EQ(tiffBitsPerSample(DataType::Float64), 64);
    EXPECT_THROW(tiffSampleFormat(DataType::Unknown), InternalError);
    EXPECT_THROW(tiffBitsPerSample(static_cast<DataType>(99)), InternalError);
}
