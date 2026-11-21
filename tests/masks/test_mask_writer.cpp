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

    EXPECT_EQ(asInt32(masks_test::readAll(*reopened, 4)), asInt32(masks_test::readAll(*scene, 4)));
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

    std::vector<int32_t> level1(static_cast<size_t>(500 * 400), -1);
    reopened->readResampledLevelBlockChannels(1, std::make_tuple(0, 0, 500, 400), std::make_tuple(500, 400), {0},
                                              level1.data(), level1.size() * sizeof(int32_t));
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
