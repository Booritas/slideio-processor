// Requirements: UR-RES-7, UR-QA-3
// MaskWriter (reporting plan C, decision 4): any SlideIO scene written through
// RasterTiffWriter as a lossless OME-TIFF that SlideIO reopens with every level.
#include <slideio/processor/masks/mask_writer.hpp>
#include <slideio/processor/masks/masks.hpp>

#include "masks/mask_fixture.hpp"

#include <pipeline/test_support/synthetic_scene.hpp>
#include <pipeline/test_support/test_operations.hpp>

#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/pipeline/operation_parameters.hpp>
#include <slideio/processor/pipeline/pipeline.hpp>
#include <slideio/processor/pipeline/pipeline_spec.hpp>

#include <slideio/core/levelinfo.hpp>
#include <slideio/slideio/scene.hpp>
#include <slideio/slideio/slide.hpp>
#include <slideio/slideio/slideio.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using namespace slideio::processor;
using Masks = masks_test::MaskStoreTest;
using masks_test::OutputDir;
namespace ts = slideio::processor::testing_support;

namespace {

std::shared_ptr<slideio::Scene> reopenFile(const OutputDir& out) {
    std::shared_ptr<slideio::Slide> slide = slideio::openSlide(out.utf8(), "OMETIFF");
    if (!slide || slide->getNumScenes() != 1) return nullptr;
    return slide->getScene(0);
}

std::vector<int32_t> asInt32(const std::vector<uint8_t>& bytes) {
    std::vector<int32_t> values(bytes.size() / sizeof(int32_t));
    std::memcpy(values.data(), bytes.data(), values.size() * sizeof(int32_t));
    return values;
}

// A pipeline over a 700 x 600 Byte texture whose box mean is Float32.
struct PipelineScene {
    std::shared_ptr<ts::SyntheticSource> source;
    std::unique_ptr<Pipeline> pipeline;
    std::shared_ptr<slideio::Scene> scene;

    explicit PipelineScene(double metresPerPixel = 0.0) {
        ts::registerTestOperations();
        source = std::make_shared<ts::SyntheticSource>(
            std::vector<ts::SyntheticLevel>{{700, 600, 0, 0}},
            std::vector<slideio::DataType>{slideio::DataType::DT_Byte},
            [](int, int64_t x, int64_t y) { return static_cast<double>((x * 37 + y * 91 + (x * y) % 13) % 256); });
        if (metresPerPixel > 0.0) source->setResolution(metresPerPixel, metresPerPixel);
        OperationParameters radius;
        radius.setInt("radius", 2);
        PipelineSpec spec;
        spec.operation("test.boxMean", radius);
        const Configuration config =
            ConfigurationBuilder().tileSize(128, 128).tileSizeFromSlide(false).workerCount(2).build();
        pipeline = std::make_unique<Pipeline>(ts::makeSyntheticScene(source), config, spec);
        scene = pipeline->asScene();
    }
};

// Level 1 (500 x 400) of a reopened 1000 x 800 Int32 mask, read through SlideIO.
std::vector<int32_t> readLevel1(slideio::Scene& scene) {
    std::vector<int32_t> level1(static_cast<size_t>(500 * 400), -1);
    scene.readResampledLevelBlockChannels(1, std::make_tuple(0, 0, 500, 400), std::make_tuple(500, 400), {0},
                                          level1.data(), level1.size() * sizeof(int32_t));
    return level1;
}

// Storage's integer rule: level-1 pixel (x, y) is level-0 pixel (2x, 2y), the top-left of
// its 2 x 2 block. Also requires the fixture to have blocks spanning two values, without
// which a mean would pass as well.
void expectTopLeftSampling(const std::vector<int32_t>& level0, const std::vector<int32_t>& level1) {
    constexpr int w0 = 1000, w1 = 500, h1 = 400;
    int64_t mixedBlocks = 0, mismatches = 0;
    for (int y = 0; y < h1; ++y) {
        for (int x = 0; x < w1; ++x) {
            const auto at0 = [&](int dx, int dy) { return level0[static_cast<size_t>((2 * y + dy) * w0 + 2 * x + dx)]; };
            const int32_t topLeft = at0(0, 0);
            if (at0(1, 0) != topLeft || at0(0, 1) != topLeft || at0(1, 1) != topLeft) ++mixedBlocks;
            if (level1[static_cast<size_t>(y * w1 + x)] != topLeft) ++mismatches;
        }
    }
    EXPECT_GT(mixedBlocks, 0) << "the fixture must have 2 x 2 blocks spanning two labels";
    EXPECT_EQ(mismatches, 0) << "level 1 is not the top-left sample of level 0";
}

std::string fileText(const OutputDir& out) {
    std::ifstream in(out.tif, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

TEST_F(Masks, WrittenLabelMaskReopensInSlideIO) {
    std::shared_ptr<slideio::Scene> scene = rasteriseLabels(store(), all(), Rect64{0, 0, kWidth, kHeight});
    OutputDir out;
    const ExportResult result = MaskWriter(out.utf8().c_str()).write(scene, nullptr, CancellationToken());
    EXPECT_FALSE(result.wasCancelled());
    EXPECT_EQ(result.objectsWritten(), 4);  // 2 x 2 tiles of 512
    EXPECT_EQ(static_cast<int64_t>(std::filesystem::file_size(out.tif)), result.bytesWritten());

    std::shared_ptr<slideio::Scene> reopened = reopenFile(out);
    ASSERT_TRUE(reopened);
    EXPECT_EQ(reopened->getRect(), std::make_tuple(0, 0, kWidth, kHeight));
    ASSERT_EQ(reopened->getNumChannels(), 1);
    EXPECT_EQ(reopened->getChannelDataType(0), slideio::DataType::DT_Int32);
    // 1000 x 800, then halved (rounded up) until both sides are at most 512.
    ASSERT_EQ(reopened->getNumZoomLevels(), 2);
    EXPECT_EQ(reopened->getLevelInfo(0)->getSize(), slideio::Size(kWidth, kHeight));
    EXPECT_EQ(reopened->getLevelInfo(1)->getSize(), slideio::Size(500, 400));
    // No resolution: the label scene has none.
    EXPECT_EQ(fileText(out).find("PhysicalSizeX"), std::string::npos);

    const std::vector<int32_t> level0 = asInt32(masks_test::readAll(*reopened, 4));
    EXPECT_EQ(level0, asInt32(masks_test::readAll(*scene, 4)));
    expectTopLeftSampling(level0, readLevel1(*reopened));
}

TEST_F(Masks, ReducedLevelsKeepLabelValues) {
    std::shared_ptr<slideio::Scene> scene = rasteriseLabels(store(), all(), Rect64{0, 0, kWidth, kHeight});
    OutputDir out;
    MaskWriter(out.utf8().c_str()).write(scene, nullptr, CancellationToken());
    std::shared_ptr<slideio::Scene> reopened = reopenFile(out);
    ASSERT_TRUE(reopened);
    ASSERT_EQ(reopened->getNumZoomLevels(), 2);

    const std::vector<int32_t> level0 = asInt32(masks_test::readAll(*reopened, 4));
    const std::set<int32_t> values(level0.begin(), level0.end());
    EXPECT_EQ(values, (std::set<int32_t>{0, 3, 5, 7, 9}));

    const std::vector<int32_t> level1 = readLevel1(*reopened);
    // Membership alone cannot catch a mean (mean(3, 7) = 5); the exact rule can.
    expectTopLeftSampling(level0, level1);
    std::set<int32_t> reduced;
    for (int32_t value : level1) {
        EXPECT_EQ(values.count(value), 1u) << "a blended label value";
        reduced.insert(value);
    }
    EXPECT_GT(reduced.size(), 1u);
}

TEST_F(Masks, WritesAPipelineScene) {
    PipelineScene pipeline;
    ASSERT_EQ(pipeline.scene->getChannelDataType(0), slideio::DataType::DT_Float32);
    OutputDir out;
    MaskWriter(out.utf8().c_str()).compression(RasterCompression::Lzw).write(pipeline.scene, nullptr,
                                                                              CancellationToken());
    std::shared_ptr<slideio::Scene> reopened = reopenFile(out);
    ASSERT_TRUE(reopened);
    EXPECT_EQ(reopened->getRect(), std::make_tuple(0, 0, 700, 600));
    ASSERT_EQ(reopened->getNumChannels(), 1);
    EXPECT_EQ(reopened->getChannelDataType(0), slideio::DataType::DT_Float32);
    // Compared as bytes: lossless means bit-identical floats.
    EXPECT_EQ(masks_test::readAll(*reopened, 4), masks_test::readAll(*pipeline.scene, 4));
}

TEST_F(Masks, RefusesMixedChannelTypes) {
    std::shared_ptr<slideio::Scene> mixed = ts::makeSyntheticScene(
        {{64, 64, 0, 0}}, {slideio::DataType::DT_Byte, slideio::DataType::DT_UInt16},
        [](int, int64_t, int64_t) { return 1.0; });
    OutputDir out;
    EXPECT_THROW(MaskWriter(out.utf8().c_str()).write(mixed, nullptr, CancellationToken()), InvalidArgumentError);
    EXPECT_THROW(MaskWriter(out.utf8().c_str()).write(nullptr, nullptr, CancellationToken()), InvalidArgumentError);
    EXPECT_TRUE(out.empty());  // no file, no partial, no spool

    // A type outside the OME-TIFF layout is refused before any file, too.
    std::shared_ptr<slideio::Scene> wide = ts::makeSyntheticScene(
        {{64, 64, 0, 0}}, {slideio::DataType::DT_Int64}, [](int, int64_t, int64_t) { return 1.0; });
    EXPECT_THROW(MaskWriter(out.utf8().c_str()).write(wide, nullptr, CancellationToken()), UnsupportedDataTypeError);
    EXPECT_TRUE(out.empty());

    EXPECT_THROW(MaskWriter(""), InvalidArgumentError);
    EXPECT_THROW(MaskWriter(out.utf8().c_str()).resolution(-1.0), InvalidArgumentError);
    EXPECT_THROW(MaskWriter(out.utf8().c_str()).name("a\xC0\xAF"), InvalidArgumentError);  // not UTF-8
    EXPECT_TRUE(out.empty());
}

TEST_F(Masks, ResolutionDefaultsToTheScenes) {
    PipelineScene pipeline(0.5e-6);
    const double metres = std::get<0>(pipeline.scene->getResolution());
    ASSERT_GT(metres, 0.0);
    {
        OutputDir out;
        MaskWriter(out.utf8().c_str()).write(pipeline.scene, nullptr, CancellationToken());
        EXPECT_NE(fileText(out).find("PhysicalSizeX=\"0.5\""), std::string::npos);
        std::shared_ptr<slideio::Scene> reopened = reopenFile(out);
        ASSERT_TRUE(reopened);
        EXPECT_DOUBLE_EQ(std::get<0>(reopened->getResolution()), metres);
    }
    {
        // resolution() replaces the scene's own; 0 writes none.
        OutputDir out;
        MaskWriter(out.utf8().c_str()).resolution(0.25).write(pipeline.scene, nullptr, CancellationToken());
        EXPECT_NE(fileText(out).find("PhysicalSizeX=\"0.25\""), std::string::npos);
        OutputDir none;
        MaskWriter(none.utf8().c_str()).resolution(0.0).write(pipeline.scene, nullptr, CancellationToken());
        EXPECT_EQ(fileText(none).find("PhysicalSizeX"), std::string::npos);
    }
}

TEST_F(Masks, WriteIsDeterministic) {
    // UR-QA-3: two writes of the same scene with the same settings are byte-identical.
    std::shared_ptr<slideio::Scene> labels = rasteriseLabels(store(), all(), Rect64{0, 0, kWidth, kHeight});
    OutputDir once, twice;
    MaskWriter(once.utf8().c_str()).name("labels").resolution(0.5).write(labels, nullptr, CancellationToken());
    MaskWriter(twice.utf8().c_str()).name("labels").resolution(0.5).write(labels, nullptr, CancellationToken());
    const std::string bytes = fileText(once);
    EXPECT_FALSE(bytes.empty());
    EXPECT_EQ(bytes, fileText(twice));

    PipelineScene pipeline;
    OutputDir third, fourth;
    MaskWriter(third.utf8().c_str()).write(pipeline.scene, nullptr, CancellationToken());
    MaskWriter(fourth.utf8().c_str()).write(pipeline.scene, nullptr, CancellationToken());
    EXPECT_EQ(fileText(third), fileText(fourth));
}

TEST_F(Masks, WritesAMultiChannelScene) {
    // Two Byte channels become two planes of one IFD; the OME-XML says so with a Channel
    // element (Bio-Formats expects it), and SlideIO still opens both channels.
    std::shared_ptr<slideio::Scene> twoChannels = ts::makeSyntheticScene(
        {{600, 520, 0, 0}}, {slideio::DataType::DT_Byte, slideio::DataType::DT_Byte},
        [](int channel, int64_t x, int64_t y) { return static_cast<double>((x * 7 + y * 3 + channel * 101) % 256); });
    OutputDir out;
    MaskWriter(out.utf8().c_str()).write(twoChannels, nullptr, CancellationToken());
    EXPECT_NE(fileText(out).find("<Channel ID=\"Channel:0:0\" SamplesPerPixel=\"2\"/>"), std::string::npos);

    std::shared_ptr<slideio::Scene> reopened = reopenFile(out);
    ASSERT_TRUE(reopened);
    EXPECT_EQ(reopened->getRect(), std::make_tuple(0, 0, 600, 520));
    ASSERT_EQ(reopened->getNumChannels(), 2);
    EXPECT_EQ(reopened->getChannelDataType(1), slideio::DataType::DT_Byte);
    EXPECT_EQ(reopened->getNumZoomLevels(), 2);  // 600 x 520, then 300 x 260
    EXPECT_EQ(masks_test::readAll(*reopened, 1), masks_test::readAll(*twoChannels, 1));

    // A one-plane file has no Channel element.
    std::shared_ptr<slideio::Scene> labels = rasteriseLabels(store(), all(), Rect64{0, 0, 300, 200});
    OutputDir single;
    MaskWriter(single.utf8().c_str()).write(labels, nullptr, CancellationToken());
    EXPECT_EQ(fileText(single).find("<Channel"), std::string::npos);
}
