// Requirements: UR-SEG-5, UR-SEG-6, UR-EXT-1
#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/control.hpp>
#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/core/measurement.hpp>
#include <slideio/processor/core/object_record.hpp>
#include <slideio/processor/pipeline/pipeline.hpp>

#include <pipeline/detail/pixel_size.hpp>
#include <pipeline/test_support/synthetic_scene.hpp>
#include <pipeline/test_support/test_operations.hpp>

#include <slideio/slideio/scene.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <tuple>
#include <vector>

using namespace slideio::processor;
namespace ts = slideio::processor::testing_support;

namespace {

const LayerId kLayer{3};

double pixelFunction(int, int64_t x, int64_t y) {
    return (x >= 10 && x < 30 && y >= 10 && y < 30) ? 255.0 : 0.0;
}

std::shared_ptr<slideio::Scene> sceneWith(double resolutionX, double resolutionY) {
    ts::registerTestOperations();
    auto source = std::make_shared<ts::SyntheticSource>(
        std::vector<ts::SyntheticLevel>{{128, 96, 0, 0}, {64, 48, 0, 0}, {32, 24, 0, 0}},
        std::vector<slideio::DataType>{slideio::DataType::DT_Byte}, pixelFunction);
    source->setResolution(resolutionX, resolutionY);
    return ts::makeSyntheticScene(source);
}

Configuration config() {
    return ConfigurationBuilder().tileSize(32, 32).tileSizeFromSlide(false).workerCount(2).build();
}

PipelineSpec probeSpec(int32_t level) {
    PipelineSpec spec;
    spec.operation("test.pixelSizeProbe", OperationParameters());
    if (level >= 0) {
        spec.analysisLevel(level);
    }
    return spec;
}

// The pixel sizes every apply() of an eager run saw.
std::vector<double> eagerSizes(const std::shared_ptr<slideio::Scene>& scene, int32_t level = -1) {
    ts::PixelSizeProbeOperation::reset();
    Pipeline p(scene, config(), probeSpec(level));
    p.run(nullptr, CancellationToken());
    return ts::PixelSizeProbeOperation::seenPixelSizes();
}

// The pixel sizes every apply() of a lazy read of the whole level 0 saw.
std::vector<double> lazySizes(const std::shared_ptr<slideio::Scene>& scene, int32_t level = -1) {
    ts::PixelSizeProbeOperation::reset();
    Pipeline p(scene, config(), probeSpec(level));
    const std::shared_ptr<slideio::Scene> lazy = p.asScene();
    const std::tuple<int, int, int, int> rect = lazy->getRect();
    const int width = std::get<2>(rect);
    const int height = std::get<3>(rect);
    std::vector<uint8_t> buffer(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    lazy->readBlock(std::make_tuple(0, 0, width, height), buffer.data(), buffer.size());
    return ts::PixelSizeProbeOperation::seenPixelSizes();
}

::testing::AssertionResult allEqual(const std::vector<double>& sizes, double expected) {
    if (sizes.empty()) {
        return ::testing::AssertionFailure() << "no apply() call was seen";
    }
    for (size_t i = 0; i < sizes.size(); ++i) {
        if (sizes[i] != expected) {
            return ::testing::AssertionFailure() << "call " << i << " of " << sizes.size() << " saw "
                                                 << sizes[i] << ", expected " << expected;
        }
    }
    return ::testing::AssertionSuccess();
}

} // namespace

TEST(PixelSize, PixelSizeFromResolution) {
    const auto scene = sceneWith(0.25e-6, 0.25e-6);
    EXPECT_DOUBLE_EQ(detail::levelZeroPixelSizeMicrons(*scene), 0.25);
    EXPECT_TRUE(allEqual(eagerSizes(scene), 0.25));
    EXPECT_TRUE(allEqual(lazySizes(scene), 0.25));
}

TEST(PixelSize, PixelSizeZeroWhenUnknown) {
    const auto scene = sceneWith(0.0, 0.0);
    EXPECT_EQ(detail::levelZeroPixelSizeMicrons(*scene), 0.0);
    EXPECT_TRUE(allEqual(eagerSizes(scene), 0.0));
    EXPECT_TRUE(allEqual(lazySizes(scene), 0.0));
}

TEST(PixelSize, PixelSizeZeroWhenAnisotropic) {
    const auto scene = sceneWith(0.25e-6, 0.5e-6);
    EXPECT_EQ(detail::levelZeroPixelSizeMicrons(*scene), 0.0);
    EXPECT_TRUE(allEqual(eagerSizes(scene), 0.0));
    EXPECT_TRUE(allEqual(lazySizes(scene), 0.0));
}

TEST(PixelSize, PixelSizeZeroWhenOneAxisIsMissingOrNegative) {
    EXPECT_EQ(detail::levelZeroPixelSizeMicrons(*sceneWith(0.25e-6, 0.0)), 0.0);
    EXPECT_EQ(detail::levelZeroPixelSizeMicrons(*sceneWith(-0.25e-6, -0.25e-6)), 0.0);
}

TEST(PixelSize, PixelSizeIsTheMeanOfAgreeingAxes) {
    // Within one part in a million: the mean.
    const auto scene = sceneWith(0.25e-6, 0.25e-6 * (1.0 + 5e-7));
    EXPECT_NEAR(detail::levelZeroPixelSizeMicrons(*scene), 0.25 * (1.0 + 2.5e-7), 1e-12);
    // Beyond it: anisotropic.
    EXPECT_EQ(detail::levelZeroPixelSizeMicrons(*sceneWith(0.25e-6, 0.25e-6 * (1.0 + 5e-6))), 0.0);
}

TEST(PixelSize, PixelSizeIsLevelZeroAtEveryLevel) {
    const auto scene = sceneWith(0.25e-6, 0.25e-6);
    for (int32_t level = 0; level <= 2; ++level) {
        EXPECT_TRUE(allEqual(eagerSizes(scene, level), 0.25)) << "eager, level " << level;
        EXPECT_TRUE(allEqual(lazySizes(scene, level), 0.25)) << "lazy, level " << level;
    }
}

TEST(PixelSize, FinishContextCarriesPixelSizeAndLevel) {
    ts::registerTestOperations();
    for (int32_t level = 0; level <= 1; ++level) {
        const auto scene = sceneWith(0.25e-6, 0.25e-6);
        OperationParameters parameters;
        parameters.setBool("finishContext", true);
        PipelineSpec spec;
        spec.analysisLevel(level).operation("test.components", parameters);
        Pipeline p(scene, config(), spec);
        p.bindLayer("objects", kLayer);
        ts::CollectingObjectSink sink;
        p.runInto(sink, nullptr, CancellationToken());
        const std::vector<ObjectRecord> records = sink.records();
        ASSERT_FALSE(records.empty()) << "level " << level;
        for (const ObjectRecord& record : records) {
            Measurement size;
            Measurement seenLevel;
            ASSERT_TRUE(record.measurements().find("finishPixelSize", size));
            ASSERT_TRUE(record.measurements().find("finishLevel", seenLevel));
            EXPECT_EQ(size.value(), 0.25);
            EXPECT_EQ(seenLevel.value(), static_cast<double>(level));
        }
    }
}
