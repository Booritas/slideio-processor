// Requirements: UR-PIX-5, UR-QA-3, UR-EXE-1
#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/control.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/memory_budget.hpp>
#include <slideio/processor/pipeline/executor.hpp>
#include <slideio/processor/pipeline/pipeline.hpp>

#include <pipeline/detail/ordered_fold.hpp>
#include <pipeline/detail/pipeline_impl.hpp>
#include <pipeline/test_support/disc_mask_source.hpp>
#include <pipeline/test_support/executor_support.hpp>
#include <pipeline/test_support/global_ops.hpp>
#include <pipeline/test_support/synthetic_scene.hpp>
#include <pipeline/test_support/test_operations.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

using namespace slideio::processor;
namespace ts = slideio::processor::testing_support;

namespace {

constexpr int64_t kSide = 512;
constexpr int32_t kTile = 64;

// Two populations, dark and bright, with a texture inside each: a histogram
// with a clear Otsu split that a slip in any bin would move.
double bimodal(int, int64_t x, int64_t y) {
    const int64_t texture = (x * 37 + y * 91 + (x * y) % 13) % 50;
    return ((x / 23 + y / 41) % 3 == 0) ? 170.0 + static_cast<double>(texture) : 30.0 + static_cast<double>(texture);
}

// A 1 at the top-left pixel of every tile, 0 elsewhere: every tile sums to 1.
double onePerTile(int, int64_t x, int64_t y) { return x % kTile == 0 && y % kTile == 0 ? 1.0 : 0.0; }

std::shared_ptr<slideio::Scene> scene(ts::PixelFunction pixel = bimodal) {
    ts::registerTestOperations();
    ts::registerGlobalOperations();
    ts::registerProbeOperation();
    return ts::makeSyntheticScene({{kSide, kSide, 0, 0}}, {slideio::DataType::DT_Byte}, std::move(pixel));
}

Configuration config(int32_t workers) {
    return ConfigurationBuilder().tileSize(kTile, kTile).tileSizeFromSlide(false).workerCount(workers).build();
}

OperationParameters offset(int64_t value) {
    OperationParameters p;
    p.setInt("offset", value);
    return p;
}

template <typename T>
std::shared_ptr<T> runInstance(const Pipeline& p, size_t position) {
    auto op = std::dynamic_pointer_cast<T>(detail::PipelineAccess::plan(p).operations.at(position));
    if (!op) {
        throw InternalError("the run's operation has an unexpected type");
    }
    return op;
}

uint64_t bitsOf(double v) {
    uint64_t bits;
    std::memcpy(&bits, &v, sizeof bits);
    return bits;
}

// The exact sample sum of a level-0 rect.
double sampleSum(const Rect64& r, const ts::PixelFunction& pixel = bimodal) {
    double s = 0.0;
    for (int64_t y = r.y; y < r.y + r.height; ++y) {
        for (int64_t x = r.x; x < r.x + r.width; ++x) {
            s += pixel(0, x, y);
        }
    }
    return s;
}

class RecordingObserver : public ProgressObserver {
public:
    void onProgress(const char*, double, int64_t completed, int64_t total) override {
        std::lock_guard<std::mutex> lock(mutex_);
        calls.emplace_back(completed, total);
    }
    std::mutex mutex_;
    std::vector<std::pair<int64_t, int64_t>> calls;
};

class CountingRasterSink : public RasterSink {
public:
    void writeTile(const BlockView&) override { ++tiles_; }
    void flush() override {}
    int64_t tilesWritten() const override { return tiles_; }

private:
    std::atomic<int64_t> tiles_{0};
};

} // namespace

namespace {

// The canonical fold by hand: the run's instance starts at zero and adds each
// tile's partial in ascending tile index.
double ascendingFold(const TileGrid& grid, const ts::PixelFunction& pixel) {
    double sum = 0.0;
    for (int64_t i = 0; i < grid.tileCount(); ++i) {
        sum += ts::FloatSumOperation::tilePartial(grid.tileRectAtLevel(i), sampleSum(grid.tileRect(i), pixel), true);
    }
    return sum;
}

} // namespace

// UR-QA-3: an order-sensitive float accumulator gives bitwise the same result for
// any worker count, because partials combine in ascending tile index.
TEST(GlobalContext, FloatAccumulationIsWorkerCountInvariant) {
    // Deterministic part. Every tile sums to 1 and tile 0 carries 2^53: in ascending
    // order each later 1 is rounded away (2^53 + 1 ties to 2^53), whereas any order
    // combining two or more other tiles before tile 0 ends at 2^53 + 2 or above.
    // With more than one worker, tile 0's accumulate is held until every other
    // tile's has finished, so its partial certainly arrives late: a fold that
    // combined in arrival order would fail on every run.
    {
        auto s = scene(onePerTile);
        PipelineSpec referenceSpec;
        referenceSpec.operation("test.floatSum", offset(1));
        Pipeline reference(s, config(1), referenceSpec);
        reference.validate();
        const TileGrid& grid = detail::PipelineAccess::plan(reference).grid;
        const int64_t tiles = grid.tileCount();
        const double expected = ascendingFold(grid, onePerTile);
        ASSERT_EQ(expected, 9007199254740992.0);
        double tileZeroLast = 0.0;
        for (int64_t i = 1; i < tiles; ++i) {
            tileZeroLast += ts::FloatSumOperation::tilePartial(grid.tileRectAtLevel(i), 1.0, true);
        }
        tileZeroLast += ts::FloatSumOperation::tilePartial(grid.tileRectAtLevel(0), 1.0, true);
        ASSERT_NE(bitsOf(tileZeroLast), bitsOf(expected)) << "the fixture must tell the orders apart";

        for (int32_t workers : {1, 2, 4, 8}) {
            OperationParameters parameters = offset(1);
            parameters.setInt("holdFirstTile", workers > 1 ? tiles - 1 : 0);
            PipelineSpec spec;
            spec.operation("test.floatSum", parameters);
            Pipeline p(s, config(workers), spec);
            ts::FloatSumOperation::resetHold();
            p.run(nullptr, CancellationToken());
            const auto op = runInstance<ts::FloatSumOperation>(p, 0);
            EXPECT_TRUE(op->finalised());
            EXPECT_EQ(op->count(), kSide * kSide) << workers << " workers";
            EXPECT_EQ(bitsOf(op->sum()), bitsOf(expected)) << workers << " workers, tile 0 held";
        }
    }

    // A textured scene, no hold: still bitwise equal across worker counts.
    auto s = scene();
    PipelineSpec spec;
    spec.operation("test.floatSum", offset(1));
    Pipeline reference(s, config(1), spec);
    reference.validate();
    const double expected = ascendingFold(detail::PipelineAccess::plan(reference).grid, bimodal);

    for (int32_t workers : {1, 2, 4, 8}) {
        Pipeline p(s, config(workers), spec);
        p.run(nullptr, CancellationToken());
        const auto op = runInstance<ts::FloatSumOperation>(p, 0);
        EXPECT_TRUE(op->finalised());
        EXPECT_EQ(op->count(), kSide * kSide) << workers << " workers";
        EXPECT_EQ(bitsOf(op->sum()), bitsOf(expected)) << workers << " workers";

        // A second run accumulates afresh rather than on top of the first.
        p.run(nullptr, CancellationToken());
        const auto again = runInstance<ts::FloatSumOperation>(p, 0);
        EXPECT_EQ(again->count(), kSide * kSide) << workers << " workers, second run";
        EXPECT_EQ(bitsOf(again->sum()), bitsOf(expected)) << workers << " workers, second run";
    }
}

// UR-PIX-5: the streamed Otsu threshold equals the one of a full in-memory
// histogram of the level.
TEST(GlobalContext, StreamedOtsuEqualsInMemoryOtsu) {
    auto s = scene();
    std::vector<int64_t> bins(256, 0);
    for (int64_t y = 0; y < kSide; ++y) {
        for (int64_t x = 0; x < kSide; ++x) {
            ++bins[static_cast<size_t>(bimodal(0, x, y))];
        }
    }
    const int32_t inMemory = ts::OtsuOperation::otsuThreshold(bins);
    ASSERT_GT(inMemory, 60);
    ASSERT_LT(inMemory, 190);

    PipelineSpec spec;
    spec.operation("test.otsu", OperationParameters());
    Pipeline p(s, config(4), spec);
    p.run(nullptr, CancellationToken());
    const auto op = runInstance<ts::OtsuOperation>(p, 0);
    EXPECT_EQ(op->histogram().totalCount(), kSide * kSide);
    for (int32_t i = 0; i < 256; ++i) {
        ASSERT_EQ(op->histogram().bin(i), bins[static_cast<size_t>(i)]) << "bin " << i;
    }
    EXPECT_EQ(op->threshold(), inMemory);
}

// Two global-context operations make three passes; progress counts every
// tile of every pass, and pass 2 accumulates over the finalised first operation.
TEST(GlobalContext, PassesAndProgress) {
    auto s = scene();
    PipelineSpec spec;
    spec.operation("test.otsu", OperationParameters());
    spec.operation("test.floatSum", offset(0));
    Pipeline p(s, config(4), spec);
    CountingRasterSink raster;
    p.rasterSink(&raster);
    p.validate();
    const int64_t tiles = detail::PipelineAccess::plan(p).grid.tileCount();
    ASSERT_EQ(tiles, 64);

    WorkerPool pool(4, makeMemoryBudget(int64_t(1) << 30));
    TileExecutor executor(detail::PipelineAccess::plan(p).grid, pool, config(4));
    EXPECT_EQ(executor.tilesTotal(), tiles) << "before a run: the grid's tile count";
    RecordingObserver observer;
    ts::CollectingObjectSink sink;
    executor.run(p, sink, &observer, CancellationToken());
    pool.shutdown();

    EXPECT_EQ(executor.tilesTotal(), 3 * tiles);
    EXPECT_EQ(executor.tilesCompleted(), 3 * tiles);
    ASSERT_EQ(static_cast<int64_t>(observer.calls.size()), 3 * tiles);
    for (size_t i = 0; i < observer.calls.size(); ++i) {
        EXPECT_EQ(observer.calls[i].first, static_cast<int64_t>(i) + 1);
        EXPECT_EQ(observer.calls[i].second, 3 * tiles);
    }
    EXPECT_EQ(raster.tilesWritten(), tiles) << "only the final pass writes to the sinks";

    const auto otsu = runInstance<ts::OtsuOperation>(p, 0);
    const auto sum = runInstance<ts::FloatSumOperation>(p, 1);
    int64_t above = 0;
    for (int64_t y = 0; y < kSide; ++y) {
        for (int64_t x = 0; x < kSide; ++x) {
            above += bimodal(0, x, y) > otsu->threshold() ? 1 : 0;
        }
    }
    ASSERT_GT(above, 0);
    EXPECT_EQ(sum->count(), kSide * kSide);
    EXPECT_EQ(sum->sum(), 255.0 * static_cast<double>(above)) << "pass 2 sees the finalised threshold's output";

    // The same through Pipeline::run.
    RecordingObserver again;
    p.run(&again, CancellationToken());
    ASSERT_FALSE(again.calls.empty());
    EXPECT_EQ(again.calls.back(), std::make_pair(3 * tiles, 3 * tiles));
}

// Results arriving out of order are held until their predecessors arrive.
TEST(GlobalContext, OrderedFoldHoldsOutOfOrderResults) {
    std::vector<int> combined;
    detail::OrderedFold<int> fold(4, [&combined](int& v) { combined.push_back(v); });
    fold.submit(3, 3);
    EXPECT_TRUE(combined.empty());
    EXPECT_EQ(fold.heldCount(), 1);
    fold.submit(1, 1);
    EXPECT_TRUE(combined.empty());
    EXPECT_EQ(fold.heldCount(), 2);
    fold.submit(0, 0);
    EXPECT_EQ(combined, (std::vector<int>{0, 1}));
    EXPECT_EQ(fold.nextIndex(), 2);
    EXPECT_FALSE(fold.complete());
    fold.submit(2, 2);
    EXPECT_EQ(combined, (std::vector<int>{0, 1, 2, 3}));
    EXPECT_TRUE(fold.complete());
    EXPECT_EQ(fold.heldCount(), 0);

    // A skipped tile counts as arrived and combines nothing.
    std::vector<int> partial;
    detail::OrderedFold<int> skipping(3, [&partial](int& v) { partial.push_back(v); });
    skipping.submit(2, 20);
    skipping.skip(1);
    EXPECT_TRUE(partial.empty());
    skipping.submit(0, 0);
    EXPECT_EQ(partial, (std::vector<int>{0, 20}));
    EXPECT_TRUE(skipping.complete());

    // An index out of range or arriving twice is refused.
    detail::OrderedFold<int> strict(2, [](int&) {});
    strict.submit(1, 1);
    EXPECT_THROW(strict.submit(1, 1), InvalidArgumentError);
    EXPECT_THROW(strict.submit(2, 2), InvalidArgumentError);
    EXPECT_THROW(strict.skip(-1), InvalidArgumentError);
    strict.submit(0, 0);
    EXPECT_THROW(strict.submit(0, 0), InvalidArgumentError);
}

// Tiles the restriction skips count as arrived, so the fold completes.
TEST(GlobalContext, SkippedTilesDoNotStallTheFold) {
    auto s = scene();
    ts::DiscMaskSource disc(150.0, 170.0, 70.0);
    PipelineSpec spec;
    spec.operation("test.floatSum", offset(0));
    Pipeline p(s, config(4), spec);
    p.maskSource(&disc);
    p.validate();
    const TileGrid& grid = detail::PipelineAccess::plan(p).grid;
    const int64_t tiles = grid.tileCount();

    // Independent expectation: a tile is evaluated iff a pixel centre lies in the disc.
    double expected = 0.0;
    int64_t expectedCount = 0;
    int64_t skipped = 0;
    for (int64_t i = 0; i < tiles; ++i) {
        const Rect64 r = grid.tileRect(i);
        bool any = false;
        for (int64_t y = r.y; y < r.y + r.height && !any; ++y) {
            for (int64_t x = r.x; x < r.x + r.width && !any; ++x) {
                any = disc.contains(static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5);
            }
        }
        if (any) {
            expected += sampleSum(r);
            expectedCount += r.width * r.height;
        } else {
            ++skipped;
        }
    }
    ASSERT_GT(skipped, tiles / 2) << "the fixture should skip most tiles";
    ASSERT_GT(expectedCount, 0);

    RecordingObserver observer;
    p.run(&observer, CancellationToken());
    const auto op = runInstance<ts::FloatSumOperation>(p, 0);
    EXPECT_TRUE(op->finalised());
    EXPECT_EQ(op->count(), expectedCount);
    EXPECT_EQ(op->sum(), expected);
    ASSERT_FALSE(observer.calls.empty());
    EXPECT_EQ(observer.calls.back(), std::make_pair(2 * tiles, 2 * tiles));
    EXPECT_EQ(detail::PipelineAccess::impl(p).counters.tilesSkipped.load(), skipped)
        << "skipped tiles are counted once, in the final pass";
}

namespace {

// Cancels its source once `after` tiles have completed.
class CancellingObserver : public ProgressObserver {
public:
    CancellingObserver(CancellationSource& source, int64_t after) : source_(source), after_(after) {}
    void onProgress(const char*, double, int64_t completed, int64_t) override {
        maxCompleted_ = std::max(maxCompleted_.load(), completed);
        if (completed >= after_) {
            source_.cancel();
        }
    }
    int64_t maxCompleted() const { return maxCompleted_; }

private:
    CancellationSource& source_;
    int64_t after_;
    std::atomic<int64_t> maxCompleted_{0};
};

} // namespace

// A tile failing in an accumulation pass fails the run as a TileError; the final
// pass never runs, so no sink is written or flushed.
TEST(GlobalContext, AccumulationPassFailureStopsTheRun) {
    auto s = scene();
    ts::ProbeOperation::setFailuresArmed(true);
    OperationParameters probe;
    probe.setInt("failX", 100);
    probe.setInt("failY", 100);
    probe.setInt("copy", 1);
    PipelineSpec spec;
    spec.operation("test.probe", probe);
    spec.operation("test.floatSum", offset(0));
    Pipeline p(s, config(4), spec);
    ts::FlushLog log;
    ts::LoggingObjectSink objects(log);
    ts::LoggingRasterSink raster(log);
    p.rasterSink(&raster);
    try {
        p.runInto(objects, nullptr, CancellationToken());
        FAIL() << "the run should fail";
    } catch (const TileError& e) {
        EXPECT_TRUE(e.hasTile());
        EXPECT_EQ(e.tileX(), 1);
        EXPECT_EQ(e.tileY(), 1);
        ASSERT_NE(e.operationName(), nullptr);
        EXPECT_STREQ(e.operationName(), "test.probe");
    }
    EXPECT_EQ(raster.tilesWritten(), 0);
    EXPECT_TRUE(log.entries().empty());
}

// finalizeAccumulation() throwing fails the run as a TileError naming no tile.
TEST(GlobalContext, FinalizeFailureIsTileErrorWithoutTile) {
    auto s = scene();
    OperationParameters parameters = offset(0);
    parameters.setInt("failFinalize", 1);
    PipelineSpec spec;
    spec.operation("test.floatSum", parameters);
    Pipeline p(s, config(4), spec);
    ts::FlushLog log;
    ts::LoggingObjectSink objects(log);
    ts::LoggingRasterSink raster(log);
    p.rasterSink(&raster);
    try {
        p.runInto(objects, nullptr, CancellationToken());
        FAIL() << "the run should fail";
    } catch (const TileError& e) {
        EXPECT_FALSE(e.hasTile());
        ASSERT_NE(e.operationName(), nullptr);
        EXPECT_STREQ(e.operationName(), "test.floatSum");
    }
    EXPECT_EQ(raster.tilesWritten(), 0);
    EXPECT_TRUE(log.entries().empty());
}

// Cancelling during an accumulation pass ends the run with CancelledError before
// the final pass.
TEST(GlobalContext, CancellationInAccumulationPass) {
    auto s = scene();
    PipelineSpec spec;
    spec.operation("test.floatSum", offset(0));
    Pipeline p(s, config(4), spec);
    ts::FlushLog log;
    ts::LoggingObjectSink objects(log);
    ts::LoggingRasterSink raster(log);
    p.rasterSink(&raster);
    CancellationSource source;
    CancellingObserver observer(source, 3);
    EXPECT_THROW(p.runInto(objects, &observer, source.token()), CancelledError);
    EXPECT_LT(observer.maxCompleted(), 64) << "the accumulation pass did not finish";
    EXPECT_EQ(raster.tilesWritten(), 0);
    EXPECT_TRUE(log.entries().empty());
}
