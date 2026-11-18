// Requirements: UR-EXE-2, UR-EXT-3, UR-EXT-4
#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/memory_budget.hpp>
#include <slideio/processor/pipeline/operation_catalog.hpp>
#include <slideio/processor/pipeline/pipeline.hpp>

#include <pipeline/detail/pipeline_impl.hpp>
#include <pipeline/detail/run_plan.hpp>
#include <pipeline/test_support/synthetic_scene.hpp>
#include <pipeline/test_support/test_operations.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace slideio::processor;
namespace ts = slideio::processor::testing_support;

namespace {

double flat(int, int64_t, int64_t) { return 0.0; }

std::shared_ptr<ts::SyntheticSource> byteSource(int64_t width = 1000, int64_t height = 800) {
    ts::registerTestOperations();
    return std::make_shared<ts::SyntheticSource>(std::vector<ts::SyntheticLevel>{{width, height, 0, 0}},
                                                 std::vector<slideio::DataType>{slideio::DataType::DT_Byte}, flat);
}

// Three levels, 1000, 500 and 250 pixels square.
std::shared_ptr<ts::SyntheticSource> pyramid() {
    ts::registerTestOperations();
    return std::make_shared<ts::SyntheticSource>(
        std::vector<ts::SyntheticLevel>{{1000, 1000, 0, 0}, {500, 500, 0, 0}, {250, 250, 0, 0}},
        std::vector<slideio::DataType>{slideio::DataType::DT_Byte}, flat);
}

Configuration config(int32_t tile, int32_t workers = 2) {
    return ConfigurationBuilder().tileSize(tile, tile).tileSizeFromSlide(false).workerCount(workers).build();
}

OperationParameters workingSet(int64_t perPixel, int64_t perTile) {
    OperationParameters parameters;
    parameters.setInt("perPixel", perPixel).setInt("perTile", perTile);
    return parameters;
}

OperationParameters radius(int64_t r) {
    OperationParameters parameters;
    parameters.setInt("radius", r);
    return parameters;
}

int64_t tileBytesOf(const std::shared_ptr<ts::SyntheticSource>& source, int32_t tile, const PipelineSpec& spec) {
    Pipeline p(ts::makeSyntheticScene(source), config(tile), spec);
    p.validate();
    return detail::PipelineAccess::plan(p).tileBytes;
}

// Runs validate() and returns the message of the InvalidArgumentError it throws, or "" when it does not.
std::string validateMessage(const Pipeline& pipeline) {
    try {
        pipeline.validate();
    } catch (const InvalidArgumentError& e) {
        return e.what();
    }
    return "";
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

} // namespace

// An operation that overrides neither declaration declares 0 and 0, and a chain of
// such operations reserves exactly its buffers.
TEST(WorkingSet, DefaultsAreZero) {
    auto source = byteSource();
    const OperationCatalog& catalog = OperationCatalog::instance();
    const OperationPtr op = catalog.create("test.threshold", OperationParameters());
    EXPECT_EQ(op->workingSetBytesPerLevelPixel(), 0);
    EXPECT_EQ(op->workingSetBytesPerTile(), 0);
    EXPECT_EQ(catalog.workingSetBytesPerTile("test.threshold"), 0);
    PipelineSpec spec;
    spec.operation("test.threshold", OperationParameters());
    // Source 1 byte, output 1 byte, the reader's interleave buffer 1 byte.
    EXPECT_EQ(tileBytesOf(source, 256, spec), int64_t(256) * 256 * 3);
}

// The per-pixel figure is counted over the tile grown by the chain's halo.
TEST(WorkingSet, PerPixelScalesWithTheHaloedTile) {
    auto source = byteSource();
    PipelineSpec without;
    without.operation("test.boxMean", radius(3)).operation("test.workingSet", workingSet(0, 0));
    PipelineSpec with;
    with.operation("test.boxMean", radius(3)).operation("test.workingSet", workingSet(10, 0));
    EXPECT_EQ(tileBytesOf(source, 64, with) - tileBytesOf(source, 64, without), int64_t(70) * 70 * 10);
}

// Operations run one after another, so only the largest working set is counted.
TEST(WorkingSet, LargestOperationNotTheSum) {
    auto source = byteSource();
    PipelineSpec without;
    without.operation("test.workingSet", workingSet(0, 0)).operation("test.workingSet", workingSet(0, 0));
    PipelineSpec with;
    with.operation("test.workingSet", workingSet(10, 0)).operation("test.workingSet", workingSet(30, 0));
    EXPECT_EQ(tileBytesOf(source, 64, with) - tileBytesOf(source, 64, without), int64_t(64) * 64 * 30);
}

TEST(WorkingSet, PerTileAdds) {
    auto source = byteSource();
    PipelineSpec without;
    without.operation("test.workingSet", workingSet(0, 0));
    PipelineSpec with;
    with.operation("test.workingSet", workingSet(0, 1000000));
    EXPECT_EQ(tileBytesOf(source, 64, with) - tileBytesOf(source, 64, without), 1000000);
}

TEST(WorkingSet, NegativeDeclarationsCountAsZero) {
    auto source = byteSource();
    PipelineSpec zero;
    zero.operation("test.workingSet", workingSet(0, 0));
    PipelineSpec negative;
    negative.operation("test.workingSet", workingSet(-10, -1000));
    EXPECT_EQ(tileBytesOf(source, 64, negative), tileBytesOf(source, 64, zero));
}

// A ceiling that holds the tile's buffers but not the working set refuses the run
// before any pixel is read, naming the operation whose working set decides it.
TEST(WorkingSet, RefusedOverTheCeiling) {
    auto source = byteSource();
    const auto scene = ts::makeSyntheticScene(source);
    PipelineSpec buffersOnly;
    buffersOnly.operation("test.threshold", OperationParameters()).operation("test.workingSet", workingSet(0, 0));
    const int64_t buffers = tileBytesOf(source, 64, buffersOnly);

    PipelineSpec spec;
    spec.operation("test.threshold", OperationParameters()).operation("test.workingSet", workingSet(0, 1000000));
    Pipeline refused(scene, config(64), spec);
    refused.memoryBudget(makeMemoryBudget(buffers + 1000));
    const std::string message = validateMessage(refused);
    EXPECT_TRUE(contains(message, "test.workingSet")) << message;
    EXPECT_TRUE(contains(message, "above the memory ceiling")) << message;

    Pipeline fits(scene, config(64), spec);
    fits.memoryBudget(makeMemoryBudget(buffers + 1000000));
    EXPECT_NO_THROW(fits.validate());
    EXPECT_EQ(source->readCount(), 0);
}

// A whole-level chain keeps its sum of per-pixel figures and adds the largest
// per-tile figure; its one tile counts the working set like any other.
TEST(WorkingSet, WholeLevelAddsPerTile) {
    auto source = pyramid();
    const auto scene = ts::makeSyntheticScene(source);
    const int64_t area = 250 * 250;
    // Planes: source 1, test.wholeLevel's output 4, test.workingSet's output 4 bytes;
    // test.wholeLevel declares 100 bytes per level pixel.
    const int64_t wholeWithoutPerTile = area * (1 + 4 + 4 + 100);
    auto chain = [](int64_t perTile) {
        PipelineSpec spec;
        spec.analysisLevel(2);
        spec.operation("test.wholeLevel", OperationParameters()).operation("test.workingSet", workingSet(0, perTile));
        return spec;
    };

    // 10,000,000 bytes per call is test.workingSet's, the largest working set, larger
    // than test.wholeLevel's 6,250,000 at this level.
    Pipeline refused(scene, config(256), chain(10000000));
    refused.memoryBudget(makeMemoryBudget(wholeWithoutPerTile + 10000000 - 1));
    const std::string message = validateMessage(refused);
    EXPECT_TRUE(contains(message, "whole in memory")) << message;
    EXPECT_TRUE(contains(message, "test.workingSet")) << message;

    Pipeline fits(scene, config(256), chain(10000000));
    fits.memoryBudget(makeMemoryBudget(wholeWithoutPerTile + 10000000));
    EXPECT_NO_THROW(fits.validate());

    // The tile: the buffers (the three planes and the reader's interleave buffer)
    // plus the larger of test.wholeLevel's 100 bytes per pixel and the per-tile figure.
    Pipeline small(scene, config(256), chain(1000000));
    small.validate();
    EXPECT_EQ(detail::PipelineAccess::plan(small).tileBytes, area * 10 + area * 100);
    Pipeline large(scene, config(256), chain(10000000));
    large.validate();
    EXPECT_EQ(detail::PipelineAccess::plan(large).tileBytes, area * 10 + 10000000);
    EXPECT_EQ(source->readCount(), 0);
}

// A lazy read and an eager run reserve the same bytes for a tile: the plan's
// tileBytes, working set included, observed from inside apply().
TEST(WorkingSet, LazyAndEagerReserveTheSame) {
    auto source = byteSource(256, 256);
    PipelineSpec spec;
    spec.operation("test.workingSet", workingSet(10, 50000));
    Pipeline p(ts::makeSyntheticScene(source), config(64), spec);
    p.validate();
    const int64_t tileBytes = detail::PipelineAccess::plan(p).tileBytes;
    // Source 1, output 4 and the reader's interleave buffer 1 byte, plus the working set.
    ASSERT_EQ(tileBytes, int64_t(64) * 64 * (1 + 4 + 1) + int64_t(64) * 64 * 10 + 50000);
    const MemoryBudgetPtr budget = makeMemoryBudget(tileBytes);
    p.memoryBudget(budget);

    ts::WorkingSetOperation::observe(budget.get());
    p.run(nullptr, CancellationToken());
    EXPECT_EQ(ts::WorkingSetOperation::observedReservedBytes(), tileBytes) << "eager";

    ts::WorkingSetOperation::observe(budget.get());
    std::vector<float> storage(64 * 64);
    BlockPlane plane{storage.data(), DataType::Float32, 64 * 4, 0};
    BlockView block(64, 64, Rect64{64, 64, 64, 64}, 0, &plane, 1);
    p.readBlock(0, Rect64{64, 64, 64, 64}, block);
    EXPECT_EQ(ts::WorkingSetOperation::observedReservedBytes(), tileBytes) << "lazy";
    ts::WorkingSetOperation::observe(nullptr);
    EXPECT_EQ(budget->reservedBytes(), 0);
}

TEST(WorkingSet, CatalogAnswers) {
    ts::registerTestOperations();
    const OperationCatalog& catalog = OperationCatalog::instance();
    // test.workingSet's defaults: 2 bytes per pixel and 512 bytes per tile.
    EXPECT_EQ(catalog.workingSetBytesPerTile("test.workingSet"), 512);
    EXPECT_EQ(catalog.workingSetBytesPerLevelPixel("test.workingSet"), 2);
    EXPECT_THROW(catalog.workingSetBytesPerTile("test.nope"), InvalidArgumentError);
    EXPECT_THROW(catalog.workingSetBytesPerTile(nullptr), InvalidArgumentError);
    // The instance is authoritative.
    const OperationPtr op = catalog.create("test.workingSet", workingSet(7, 9));
    EXPECT_EQ(op->workingSetBytesPerLevelPixel(), 7);
    EXPECT_EQ(op->workingSetBytesPerTile(), 9);
}
