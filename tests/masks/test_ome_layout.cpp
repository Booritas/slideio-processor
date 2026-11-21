// Requirements: UR-RES-7
// The OME-TIFF pyramid layout (reporting plan C, decision 1) as SlideIO reads it: the file
// detail::writePyramidTiff writes with PyramidLayout::OmeSubIfds opens through SlideIO's
// OME-TIFF driver with every level. The storage detail sources are compiled into this test
// (tests/masks/CMakeLists.txt); the data library itself never links SlideIO.
#include "common/temp_path.hpp"
#include "storage/ome_raster_fixture.hpp"

#include <slideio/core/levelinfo.hpp>
#include <slideio/slideio/scene.hpp>
#include <slideio/slideio/slide.hpp>
#include <slideio/slideio/slideio.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <tuple>
#include <vector>

using namespace slideio::processor;

namespace {

// The raster and its spool in a fresh directory, all removed at scope exit. The name ends in
// .ome.tif, as the driver's file specs expect, though the test names the driver explicitly.
struct OmeFile {
    std::filesystem::path dir = uniqueTempPath("ome_layout", "");
    std::filesystem::path tif = dir / "raster.ome.tif";
    std::filesystem::path spool = dir / "raster.spool";
    OmeFile() { std::filesystem::create_directories(dir); }
    ~OmeFile() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    OmeFile(const OmeFile&) = delete;
    OmeFile& operator=(const OmeFile&) = delete;
};

} // namespace

TEST(OmeLayout, OpensInSlideIOWithAllLevels) {
    OmeFile file;
    detail::PyramidMetadata metadata;
    metadata.name = "labels";
    metadata.micronsPerPixel = 0.25;
    test::writeOmeRaster(file.tif, file.spool, metadata);

    auto slide = slideio::openSlide(file.tif.u8string(), "OMETIFF");
    ASSERT_TRUE(slide);
    ASSERT_EQ(slide->getNumScenes(), 1);
    auto scene = slide->getScene(0);
    ASSERT_TRUE(scene);
    EXPECT_EQ(scene->getName(), "labels");
    EXPECT_EQ(scene->getRect(), std::make_tuple(0, 0, 1500, 900));
    ASSERT_EQ(scene->getNumChannels(), 1);
    EXPECT_EQ(scene->getChannelDataType(0), slideio::DataType::DT_Int32);
    const auto resolution = scene->getResolution();
    EXPECT_DOUBLE_EQ(std::get<0>(resolution), 0.25e-6);
    EXPECT_DOUBLE_EQ(std::get<1>(resolution), 0.25e-6);

    ASSERT_EQ(scene->getNumZoomLevels(), 3);
    const int sizes[3][2] = {{1500, 900}, {750, 450}, {375, 225}};
    for (int level = 0; level < 3; ++level) {
        SCOPED_TRACE("level " + std::to_string(level));
        const slideio::LevelInfo* info = scene->getLevelInfo(level);
        ASSERT_NE(info, nullptr);
        EXPECT_EQ(info->getSize().width, sizes[level][0]);
        EXPECT_EQ(info->getSize().height, sizes[level][1]);
        // Each level read directly: integers take the top-left sample of each 2x2 block.
        const int width = sizes[level][0], height = sizes[level][1];
        std::vector<int32_t> pixels(static_cast<size_t>(width) * height);
        scene->readResampledLevelBlockChannels(level, std::make_tuple(0, 0, width, height),
                                               std::make_tuple(width, height), {}, pixels.data(),
                                               pixels.size() * sizeof(int32_t));
        size_t mismatches = 0;
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
                if (pixels[static_cast<size_t>(y) * width + x] !=
                    test::omeSample(static_cast<int64_t>(x) << level, static_cast<int64_t>(y) << level))
                    ++mismatches;
        EXPECT_EQ(mismatches, 0u);
    }

    // Level 0 through the scene's own block read, at full resolution.
    std::vector<int32_t> pixels(static_cast<size_t>(test::kOmeWidth * test::kOmeHeight));
    scene->readBlock(std::make_tuple(0, 0, 1500, 900), pixels.data(), pixels.size() * sizeof(int32_t));
    size_t mismatches = 0;
    for (int64_t y = 0; y < test::kOmeHeight; ++y)
        for (int64_t x = 0; x < test::kOmeWidth; ++x)
            if (pixels[static_cast<size_t>(y * test::kOmeWidth + x)] != test::omeSample(x, y)) ++mismatches;
    EXPECT_EQ(mismatches, 0u);
}

// A raster of one tile is a single level, which SlideIO reads at its own size.
TEST(OmeLayout, OneTileRasterOpensInSlideIO) {
    OmeFile file;
    test::writeOmeRaster(file.tif, file.spool, detail::PyramidMetadata{}, 300, 200);
    auto slide = slideio::openSlide(file.tif.u8string(), "OMETIFF");
    ASSERT_TRUE(slide);
    ASSERT_EQ(slide->getNumScenes(), 1);
    auto scene = slide->getScene(0);
    EXPECT_EQ(scene->getRect(), std::make_tuple(0, 0, 300, 200));
    EXPECT_EQ(scene->getNumZoomLevels(), 1);
    EXPECT_EQ(std::get<0>(scene->getResolution()), 0.0);
    std::vector<int32_t> pixels(300 * 200);
    scene->readBlock(std::make_tuple(0, 0, 300, 200), pixels.data(), pixels.size() * sizeof(int32_t));
    size_t mismatches = 0;
    for (int y = 0; y < 200; ++y)
        for (int x = 0; x < 300; ++x)
            if (pixels[static_cast<size_t>(y) * 300 + x] != test::omeSample(x, y)) ++mismatches;
    EXPECT_EQ(mismatches, 0u);
}
