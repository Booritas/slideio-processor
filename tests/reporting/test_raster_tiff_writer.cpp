// Requirements: UR-RES-7, UR-QA-3
#include <slideio/processor/reporting/raster_tiff_writer.hpp>

#include "reporting/export_fixture.hpp"

#include <slideio/processor/core/block_view.hpp>
#include <slideio/processor/core/error.hpp>

#include <gtest/gtest.h>
#include <tiffio.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace slideio::processor {
namespace {

using export_test::OutputPath;
using export_test::RecordingObserver;

// A sample distinct along both level-0 axes and negative in part, so a transposed, shifted
// or sign-lost read shows.
int32_t sampleAt(int64_t x, int64_t y) { return static_cast<int32_t>(y * 4096 + x - 1000000); }

// An Int32 source whose value is a function of the pixel's level-0 coordinates.
class CoordinateSource : public TileRasterSource {
public:
    explicit CoordinateSource(DataType type = DataType::Int32) : type_(type) {}
    void fillTile(BlockView& out) override {
        ++tiles;
        if (type_ != DataType::Int32) return;  // zeros: only the type matters
        const Rect64 rect = out.slideRect();
        const double sx = static_cast<double>(rect.width) / static_cast<double>(out.width());
        const double sy = static_cast<double>(rect.height) / static_cast<double>(out.height());
        for (int64_t j = 0; j < out.height(); ++j) {
            auto* row = reinterpret_cast<int32_t*>(static_cast<uint8_t*>(out.planeData(0)) +
                                                   j * out.planeRowStrideBytes(0));
            for (int64_t i = 0; i < out.width(); ++i)
                row[i] = sampleAt(rect.x + std::llround(static_cast<double>(i) * sx),
                                  rect.y + std::llround(static_cast<double>(j) * sy));
        }
    }
    DataType planeType() const override { return type_; }
    int32_t planeCount() const override { return 1; }
    int64_t tiles = 0;

private:
    DataType type_;
};

struct Page {
    int64_t width = 0;
    int64_t height = 0;
    uint16_t compression = 0;
    std::vector<int32_t> samples;
};

// Reads the current directory's plane 0, tiled or striped.
Page readPage(TIFF* tif) {
    Page page;
    uint32_t w = 0, h = 0;
    EXPECT_EQ(TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w), 1);
    EXPECT_EQ(TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h), 1);
    TIFFGetField(tif, TIFFTAG_COMPRESSION, &page.compression);
    page.width = w;
    page.height = h;
    page.samples.assign(static_cast<size_t>(w) * h, 0);
    if (TIFFIsTiled(tif)) {
        uint32_t tw = 0, th = 0;
        TIFFGetField(tif, TIFFTAG_TILEWIDTH, &tw);
        TIFFGetField(tif, TIFFTAG_TILELENGTH, &th);
        std::vector<int32_t> tile(static_cast<size_t>(tw) * th);
        for (uint32_t y = 0; y < h; y += th) {
            for (uint32_t x = 0; x < w; x += tw) {
                EXPECT_GE(TIFFReadTile(tif, tile.data(), x, y, 0, 0), 0);
                for (uint32_t j = 0; j < th && y + j < h; ++j)
                    for (uint32_t i = 0; i < tw && x + i < w; ++i)
                        page.samples[static_cast<size_t>(y + j) * w + x + i] = tile[static_cast<size_t>(j) * tw + i];
            }
        }
    } else {
        for (uint32_t y = 0; y < h; ++y)
            EXPECT_EQ(TIFFReadScanline(tif, &page.samples[static_cast<size_t>(y) * w], y, 0), 1);
    }
    return page;
}

// Page 0 followed by its SubIFD pages.
std::vector<Page> readPages(const std::filesystem::path& path) {
    std::vector<Page> pages;
    TIFF* tif = TIFFOpen(path.string().c_str(), "r");
    EXPECT_NE(tif, nullptr);
    if (tif == nullptr) return pages;
    pages.push_back(readPage(tif));
    uint16_t count = 0;
    void* raw = nullptr;
    if (TIFFGetField(tif, TIFFTAG_SUBIFD, &count, &raw) == 1) {
        const std::vector<uint64_t> offsets(static_cast<const uint64_t*>(raw), static_cast<const uint64_t*>(raw) + count);
        for (const uint64_t offset : offsets) {
            EXPECT_EQ(TIFFSetSubDirectory(tif, offset), 1);
            pages.push_back(readPage(tif));
        }
    }
    TIFFClose(tif);
    return pages;
}

std::filesystem::path spoolOf(const OutputPath& out) {
    std::filesystem::path p = out.partial();
    p += ".spool";
    return p;
}

class RasterTiffWriterTest : public ::testing::Test {
protected:
    OutputPath out{"rastertiff", ".ome.tif"};
    CoordinateSource source;
    CancellationToken never;
    const Rect64 region{100, 200, 1500, 900};

    void expectNoFiles() {
        EXPECT_FALSE(std::filesystem::exists(out.path()));
        EXPECT_FALSE(std::filesystem::exists(out.partial()));
        EXPECT_FALSE(std::filesystem::exists(spoolOf(out)));
    }
};

TEST_F(RasterTiffWriterTest, WritesTheRegionAtTheScale) {
    for (const double scale : {1.0, 2.0}) {
        RasterTiffWriter writer(out.utf8().c_str());
        const ExportResult result = writer.write(source, region, scale, nullptr, never);
        EXPECT_FALSE(result.wasCancelled());
        EXPECT_EQ(result.bytesWritten(), static_cast<int64_t>(std::filesystem::file_size(out.path())));
        EXPECT_FALSE(std::filesystem::exists(out.partial()));
        EXPECT_FALSE(std::filesystem::exists(spoolOf(out)));

        const int64_t w = static_cast<int64_t>(1500 / scale);
        const int64_t h = static_cast<int64_t>(900 / scale);
        const std::vector<Page> pages = readPages(out.path());
        ASSERT_GE(pages.size(), 2u);
        ASSERT_EQ(pages[0].width, w);
        ASSERT_EQ(pages[0].height, h);
        EXPECT_EQ(pages[1].width, (w + 1) / 2);
        EXPECT_EQ(pages[1].height, (h + 1) / 2);
        for (int64_t y = 0; y < h; ++y)
            for (int64_t x = 0; x < w; ++x) {
                const int32_t expected = sampleAt(region.x + static_cast<int64_t>(x * scale),
                                                  region.y + static_cast<int64_t>(y * scale));
                ASSERT_EQ(pages[0].samples[static_cast<size_t>(y * w + x)], expected) << x << "," << y;
            }
        // A reduced level takes the top-left sample of each 2 x 2 block.
        for (int64_t y = 0; y < pages[1].height; ++y)
            for (int64_t x = 0; x < pages[1].width; ++x)
                ASSERT_EQ(pages[1].samples[static_cast<size_t>(y * pages[1].width + x)],
                          pages[0].samples[static_cast<size_t>(2 * y * w + 2 * x)]);
    }
}

// Records every tile's slideRect, in call order.
class SpySource : public CoordinateSource {
public:
    void fillTile(BlockView& view) override {
        rects.push_back(view.slideRect());
        sizes.push_back({view.width(), view.height()});
        CoordinateSource::fillTile(view);
    }
    std::vector<Rect64> rects;
    std::vector<std::pair<int64_t, int64_t>> sizes;
};

TEST_F(RasterTiffWriterTest, NonIntegerScaleTilesTheRegionContiguously) {
    const Rect64 odd{7, 13, 1301, 777};
    for (const double scale : {1.5, 2.5}) {
        SpySource spy;
        RasterTiffWriter writer(out.utf8().c_str());
        writer.write(spy, odd, scale, nullptr, never);
        const int64_t w = static_cast<int64_t>(std::ceil(odd.width / scale));
        const int64_t h = static_cast<int64_t>(std::ceil(odd.height / scale));
        const std::vector<Page> pages = readPages(out.path());
        ASSERT_FALSE(pages.empty());
        EXPECT_EQ(pages[0].width, w);
        EXPECT_EQ(pages[0].height, h);
        const int64_t tilesX = (w + 511) / 512;
        const int64_t tilesY = (h + 511) / 512;
        ASSERT_EQ(static_cast<int64_t>(spy.rects.size()), tilesX * tilesY);
        for (int64_t ty = 0; ty < tilesY; ++ty)
            for (int64_t tx = 0; tx < tilesX; ++tx) {
                const size_t k = static_cast<size_t>(ty * tilesX + tx);
                const Rect64& r = spy.rects[k];
                if (tx == 0) EXPECT_EQ(r.x, odd.x); else EXPECT_EQ(r.x, spy.rects[k - 1].right());
                if (ty == 0) EXPECT_EQ(r.y, odd.y); else EXPECT_EQ(r.y, spy.rects[k - static_cast<size_t>(tilesX)].bottom());
                EXPECT_EQ(spy.sizes[k].first, std::min<int64_t>(512, w - tx * 512));
                EXPECT_EQ(spy.sizes[k].second, std::min<int64_t>(512, h - ty * 512));
            }
        // The tiles cover the region, and overshoot it by less than a scale and a pixel.
        const Rect64& last = spy.rects.back();
        EXPECT_GE(last.right(), odd.right());
        EXPECT_LT(last.right(), odd.right() + static_cast<int64_t>(scale) + 2);
        EXPECT_GE(last.bottom(), odd.bottom());
        EXPECT_LT(last.bottom(), odd.bottom() + static_cast<int64_t>(scale) + 2);
        // Samples follow the level-0 coordinate of each output pixel.
        for (int64_t y = 0; y < h; y += 37)
            for (int64_t x = 0; x < w; x += 41) {
                const int32_t expected = sampleAt(odd.x + std::llround(static_cast<double>(x) * scale),
                                                  odd.y + std::llround(static_cast<double>(y) * scale));
                ASSERT_EQ(pages[0].samples[static_cast<size_t>(y * w + x)], expected) << x << "," << y;
            }
    }
}

TEST_F(RasterTiffWriterTest, ExactQuotientDoesNotOvershootAPixel) {
    // 1100 / 1.1 is 1000 exactly, but the double quotient can round above it.
    RasterTiffWriter writer(out.utf8().c_str());
    writer.write(source, Rect64{0, 0, 1100, 600}, 1.1, nullptr, never);
    const std::vector<Page> pages = readPages(out.path());
    ASSERT_FALSE(pages.empty());
    EXPECT_EQ(pages[0].width, 1000);
    EXPECT_EQ(pages[0].height, 546);
}

TEST_F(RasterTiffWriterTest, CompressionsRoundTrip) {
    std::vector<std::vector<int32_t>> samples;
    const std::pair<RasterCompression, uint16_t> cases[] = {{RasterCompression::None, COMPRESSION_NONE},
                                                            {RasterCompression::Lzw, COMPRESSION_LZW},
                                                            {RasterCompression::Deflate, COMPRESSION_ADOBE_DEFLATE}};
    for (const auto& [compression, tag] : cases) {
        RasterTiffWriter writer(out.utf8().c_str());
        writer.compression(compression).write(source, region, 1.0, nullptr, never);
        const std::vector<Page> pages = readPages(out.path());
        ASSERT_FALSE(pages.empty());
        EXPECT_EQ(pages[0].compression, tag);
        samples.push_back(pages[0].samples);
    }
    EXPECT_EQ(samples[0], samples[1]);
    EXPECT_EQ(samples[0], samples[2]);
}

TEST_F(RasterTiffWriterTest, ResolutionWritesPhysicalSize) {
    {
        RasterTiffWriter writer(out.utf8().c_str());
        writer.write(source, region, 1.0, nullptr, never);
    }
    std::string text = export_test::readText(out.path());
    EXPECT_NE(text.find("OME"), std::string::npos);
    EXPECT_EQ(text.find("PhysicalSizeX"), std::string::npos);
    {
        RasterTiffWriter writer(out.utf8().c_str());
        writer.resolution(0.25).name("tissue").write(source, region, 1.0, nullptr, never);
    }
    text = export_test::readText(out.path());
    EXPECT_NE(text.find("PhysicalSizeX=\"0.25\""), std::string::npos);
    EXPECT_NE(text.find("PhysicalSizeY=\"0.25\""), std::string::npos);
    EXPECT_NE(text.find("Name=\"tissue\""), std::string::npos);
    // No path or file name is written.
    EXPECT_EQ(text.find(out.path().stem().stem().u8string()), std::string::npos);
}

TEST_F(RasterTiffWriterTest, CancelledWriteLeavesNoFile) {
    CancellationSource cancel;
    export_test::CancellingObserver observer(cancel, 2);
    RasterTiffWriter writer(out.utf8().c_str());
    const ExportResult result = writer.write(source, region, 1.0, &observer, cancel.token());
    EXPECT_TRUE(result.wasCancelled());
    EXPECT_EQ(result.objectsWritten(), 2);
    EXPECT_EQ(result.bytesWritten(), 0);
    EXPECT_EQ(source.tiles, 2);
    expectNoFiles();
}

TEST_F(RasterTiffWriterTest, CancelledWriteKeepsAnExistingFile) {
    export_test::writeText(out.path(), "keep");
    CancellationSource cancel;
    cancel.cancel();
    RasterTiffWriter writer(out.utf8().c_str());
    const ExportResult result = writer.write(source, region, 1.0, nullptr, cancel.token());
    EXPECT_TRUE(result.wasCancelled());
    EXPECT_EQ(result.objectsWritten(), 0);
    EXPECT_EQ(source.tiles, 0);
    EXPECT_EQ(export_test::readText(out.path()), "keep");
    EXPECT_FALSE(std::filesystem::exists(out.partial()));
    EXPECT_FALSE(std::filesystem::exists(spoolOf(out)));
}

TEST_F(RasterTiffWriterTest, ReportsProgressPerTile) {
    RecordingObserver observer;
    RasterTiffWriter writer(out.utf8().c_str());
    const ExportResult result = writer.write(source, region, 1.0, &observer, never);
    EXPECT_EQ(result.objectsWritten(), 6);  // 1500 x 900 is 3 x 2 tiles of 512
    ASSERT_EQ(observer.calls.size(), 6u);
    for (size_t i = 0; i < observer.calls.size(); ++i) {
        EXPECT_EQ(observer.calls[i].stage, "raster export");
        EXPECT_EQ(observer.calls[i].completed, static_cast<int64_t>(i + 1));
        EXPECT_EQ(observer.calls[i].total, 6);
        EXPECT_DOUBLE_EQ(observer.calls[i].fraction, static_cast<double>(i + 1) / 6.0);
    }
}

TEST_F(RasterTiffWriterTest, RefusesBadArguments) {
    RasterTiffWriter writer(out.utf8().c_str());
    EXPECT_THROW(writer.write(source, Rect64{0, 0, 0, 10}, 1.0, nullptr, never), InvalidArgumentError);
    EXPECT_THROW(writer.write(source, Rect64{0, 0, 10, -1}, 1.0, nullptr, never), InvalidArgumentError);
    EXPECT_THROW(writer.write(source, region, 0.5, nullptr, never), InvalidArgumentError);
    EXPECT_THROW(writer.write(source, region, std::nan(""), nullptr, never), InvalidArgumentError);
    EXPECT_THROW(writer.resolution(-0.1), InvalidArgumentError);
    EXPECT_THROW(writer.resolution(std::nan("")), InvalidArgumentError);
    EXPECT_THROW(writer.resolution(std::numeric_limits<double>::infinity()), InvalidArgumentError);
    EXPECT_THROW(RasterTiffWriter(nullptr), InvalidArgumentError);
    EXPECT_THROW(RasterTiffWriter(""), InvalidArgumentError);
    EXPECT_EQ(source.tiles, 0);
    expectNoFiles();
}

TEST_F(RasterTiffWriterTest, RefusesANameThatIsNotUtf8) {
    RasterTiffWriter writer(out.utf8().c_str());
    writer.name("kept");
    for (const char* bad : {"\x80", "a\xC0\xAF", "\xED\xA0\x80", "\xE2\x82", "\xF4\x90\x80\x80", "\xFF"}) {
        EXPECT_THROW(writer.name(bad), InvalidArgumentError) << "accepted an invalid name";
    }
    expectNoFiles();
    writer.write(source, region, 1.0, nullptr, never);
    EXPECT_NE(export_test::readText(out.path()).find("Name=\"kept\""), std::string::npos);  // unchanged

    // Well-formed UTF-8 beyond ASCII is kept as written.
    const char* const utf8 = "Ki67 \xC2\xB5m \xE6\x97\xA5 \xF0\x9F\x94\xAC";
    RasterTiffWriter other(out.utf8().c_str());
    other.name(utf8).write(source, region, 1.0, nullptr, never);
    EXPECT_NE(export_test::readText(out.path()).find(std::string("Name=\"") + utf8 + "\""), std::string::npos);
}

TEST_F(RasterTiffWriterTest, RefusesTypesTheOmeLayoutCannotHold) {
    for (const DataType type : {DataType::Float16, DataType::UInt32, DataType::UInt64, DataType::Int64}) {
        CoordinateSource unsupported(type);
        RasterTiffWriter writer(out.utf8().c_str());
        EXPECT_THROW(writer.write(unsupported, region, 1.0, nullptr, never), UnsupportedDataTypeError);
        EXPECT_EQ(unsupported.tiles, 0);
        expectNoFiles();
    }
    CoordinateSource bytes(DataType::Byte);
    RasterTiffWriter writer(out.utf8().c_str());
    EXPECT_NO_THROW(writer.write(bytes, Rect64{0, 0, 40, 30}, 1.0, nullptr, never));
}

TEST_F(RasterTiffWriterTest, AFailingSourceLeavesNoFile) {
    class Failing : public CoordinateSource {
    public:
        void fillTile(BlockView& view) override {
            if (tiles == 1) throw IoError("fill failed");
            CoordinateSource::fillTile(view);
        }
    } failing;
    export_test::writeText(out.path(), "keep");
    RasterTiffWriter writer(out.utf8().c_str());
    EXPECT_THROW(writer.write(failing, region, 1.0, nullptr, never), IoError);
    EXPECT_EQ(export_test::readText(out.path()), "keep");
    EXPECT_FALSE(std::filesystem::exists(out.partial()));
    EXPECT_FALSE(std::filesystem::exists(spoolOf(out)));
}

TEST_F(RasterTiffWriterTest, WriteIsDeterministic) {
    OutputPath other{"rastertiff2", ".ome.tif"};
    for (const double scale : {1.0, 3.0}) {
        RasterTiffWriter first(out.utf8().c_str());
        first.resolution(0.5).name("a").write(source, region, scale, nullptr, never);
        RasterTiffWriter second(other.utf8().c_str());
        second.resolution(0.5).name("a").write(source, region, scale, nullptr, never);
        EXPECT_EQ(export_test::readText(out.path()), export_test::readText(other.path()));
    }
}

} // namespace
} // namespace slideio::processor
