// Requirements: UR-EXE-2, UR-EXE-3, UR-EXE-4, UR-EXE-5, UR-QA-3
#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/control.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/memory_budget.hpp>
#include <slideio/processor/pipeline/executor.hpp>
#include <slideio/processor/pipeline/pipeline.hpp>

#include <pipeline/detail/pipeline_impl.hpp>
#include <pipeline/test_support/executor_support.hpp>
#include <pipeline/test_support/synthetic_scene.hpp>
#include <pipeline/test_support/test_operations.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

using namespace slideio::processor;
namespace ts = slideio::processor::testing_support;

namespace {

Configuration config(int32_t tileSide, int32_t workers) {
    return ConfigurationBuilder().tileSize(tileSide, tileSide).tileSizeFromSlide(false).workerCount(workers).build();
}

OperationParameters intParams(std::initializer_list<std::pair<const char*, int64_t>> values) {
    OperationParameters p;
    for (const auto& v : values) {
        p.setInt(v.first, v.second);
    }
    return p;
}

double texture(int, int64_t x, int64_t y) { return static_cast<double>((x * 37 + y * 91 + (x * y) % 13) % 256); }

// Separate rectangular blobs of 2..6 pixels a side, one per 13 x 11 cell; many
// straddle tile seams.
double blobs(int, int64_t x, int64_t y) {
    const int64_t cx = x / 13;
    const int64_t cy = y / 11;
    const int64_t lx = x % 13;
    const int64_t ly = y % 11;
    const int64_t w = 2 + (cx * 7 + cy * 3) % 5;
    const int64_t h = 2 + (cx * 5 + cy * 11) % 5;
    return lx >= 1 && lx < 1 + w && ly >= 1 && ly < 1 + h ? 255.0 : 0.0;
}

// An 8 x 8 grid of 64-pixel tiles.
std::shared_ptr<slideio::Scene> gridScene() {
    ts::registerTestOperations();
    ts::registerProbeOperation();
    return ts::makeSyntheticScene({{512, 512, 0, 0}}, {slideio::DataType::DT_Byte}, texture);
}

PipelineSpec probe(OperationParameters parameters) {
    PipelineSpec spec;
    spec.operation("test.probe", parameters);
    return spec;
}

// Keeps every tile it receives, by its level-0 rect, as raw bytes of every plane.
class TileKeepingRasterSink : public RasterSink {
public:
    using Key = std::tuple<int64_t, int64_t, int64_t, int64_t>;
    void writeTile(const BlockView& tile) override {
        std::vector<uint8_t> bytes;
        for (int32_t plane = 0; plane < tile.planeCount(); ++plane) {
            const int64_t rowBytes = tile.width() * dataTypeSizeBytes(tile.planeType(plane));
            for (int64_t y = 0; y < tile.height(); ++y) {
                const auto* row = static_cast<const uint8_t*>(tile.planeData(plane)) + y * tile.planeRowStrideBytes(plane);
                bytes.insert(bytes.end(), row, row + rowBytes);
            }
        }
        const Rect64 r = tile.slideRect();
        std::lock_guard<std::mutex> lock(mutex_);
        tiles_[Key{r.x, r.y, r.width, r.height}] = std::move(bytes);
        ++written_;
    }
    void flush() override {}
    int64_t tilesWritten() const override { return written_; }
    std::map<Key, std::vector<uint8_t>> tiles() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return tiles_;
    }

private:
    mutable std::mutex mutex_;
    std::map<Key, std::vector<uint8_t>> tiles_;
    std::atomic<int64_t> written_{0};
};

using Ring = std::vector<std::pair<int64_t, int64_t>>;
using ObjectKey = std::tuple<ObjectId, std::vector<std::pair<bool, Ring>>,
                             std::vector<std::tuple<std::string, double, int32_t>>>;

std::vector<ObjectKey> sortedKeys(const std::vector<ObjectRecord>& records) {
    std::vector<ObjectKey> keys;
    for (const ObjectRecord& record : records) {
        std::vector<std::pair<bool, Ring>> rings;
        const Polygon& polygon = record.geometry();
        for (int32_t r = 0; r < polygon.ringCount(); ++r) {
            std::vector<Point64> points(static_cast<size_t>(polygon.ringPointCount(r)));
            polygon.ringPoints(r, points.data(), static_cast<int64_t>(points.size()));
            Ring ring;
            for (const Point64& p : points) {
                ring.emplace_back(p.x, p.y);
            }
            rings.emplace_back(polygon.ringIsHole(r), std::move(ring));
        }
        std::vector<std::tuple<std::string, double, int32_t>> measurements;
        const MeasurementSet& m = record.measurements();
        for (int32_t i = 0; i < m.count(); ++i) {
            const Measurement measurement = m.at(i);
            measurements.emplace_back(measurement.name(), measurement.value(), static_cast<int32_t>(measurement.unit()));
        }
        keys.emplace_back(record.id(), std::move(rings), std::move(measurements));
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

struct RunResult {
    std::map<TileKeepingRasterSink::Key, std::vector<uint8_t>> tiles;
    std::vector<ObjectKey> objects;
};

// Records every progress call; flags a call that starts while another is running.
class RecordingObserver : public ProgressObserver {
public:
    void onProgress(const char* stageName, double fraction, int64_t completed, int64_t total) override {
        if (inside_.exchange(true)) {
            overlapped_ = true;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        calls_.push_back(Call{stageName, fraction, completed, total});
        inside_ = false;
    }
    struct Call {
        std::string stage;
        double fraction;
        int64_t completed;
        int64_t total;
    };
    std::vector<Call> calls_;
    std::atomic<bool> inside_{false};
    std::atomic<bool> overlapped_{false};
};

// Cancels its source once `after` tiles have completed.
class CancellingObserver : public ProgressObserver {
public:
    CancellingObserver(CancellationSource& source, int64_t after) : source_(source), after_(after) {}
    void onProgress(const char*, double, int64_t completed, int64_t) override {
        if (completed >= after_) {
            source_.cancel();
        }
    }

private:
    CancellationSource& source_;
    int64_t after_;
};

// Restores armed failures when a test leaves.
struct DisarmGuard {
    ~DisarmGuard() { ts::ProbeOperation::setFailuresArmed(true); }
};

} // namespace

TEST(TileExecutor, ResultsIdenticalAcrossWorkerCounts) {
    ts::registerTestOperations();
    ts::registerProbeOperation();
    auto scene = ts::makeSyntheticScene({{2000, 1500, 0, 0}}, {slideio::DataType::DT_Byte}, blobs);
    PipelineSpec spec;
    // A pass-through probe first, to see that tiles really overlap with several workers.
    spec.operation("test.probe", intParams({{"copy", 1}, {"sleepMs", 1}}));
    spec.operation("test.boxMean", intParams({{"radius", 1}}));
    OperationParameters threshold;
    threshold.setDouble("level", 100.0);
    spec.operation("test.threshold", threshold);
    spec.operation("test.components", OperationParameters());

    auto runWith = [&](int32_t workers) {
        TileKeepingRasterSink raster;
        ts::CollectingObjectSink objects;
        Pipeline p(scene, config(128, workers), spec);
        p.bindLayer("objects", LayerId(3));
        p.rasterSink(&raster);
        ts::ProbeOperation::resetCounters();
        p.runInto(objects, nullptr, CancellationToken());
        return RunResult{raster.tiles(), sortedKeys(objects.records())};
    };

    const RunResult reference = runWith(1);
    EXPECT_EQ(ts::ProbeOperation::maxConcurrent(), 1);
    ASSERT_EQ(reference.tiles.size(), 16u * 12u);
    ASSERT_GT(reference.objects.size(), 1000u);
    for (const int32_t workers : {2, 4, 8}) {
        const RunResult result = runWith(workers);
        EXPECT_GT(ts::ProbeOperation::maxConcurrent(), 1) << workers << " workers ran no two tiles at once";
        EXPECT_TRUE(result.tiles == reference.tiles) << workers << " workers: raster tiles differ";
        EXPECT_TRUE(result.objects == reference.objects) << workers << " workers: objects differ";
    }
}

TEST(TileExecutor, SingleWorkerIsAllowed) {
    auto scene = gridScene();
    TileKeepingRasterSink raster;
    Pipeline p(scene, config(64, 1), probe(OperationParameters()));
    p.rasterSink(&raster);
    p.run(nullptr, CancellationToken());
    EXPECT_EQ(raster.tilesWritten(), 64);

    WorkerPool one(1, makeMemoryBudget(1 << 20));
    EXPECT_EQ(one.workerCount(), 1);
    EXPECT_THROW(WorkerPool(0, makeMemoryBudget(1 << 20)), InvalidArgumentError);
    EXPECT_THROW(WorkerPool(-1, makeMemoryBudget(1 << 20)), InvalidArgumentError);
    EXPECT_THROW(WorkerPool(2, nullptr), InvalidArgumentError);
}

TEST(TileExecutor, FailureNamesTileStageAndOperation) {
    auto scene = gridScene();
    DisarmGuard guard;
    ts::ProbeOperation::setFailuresArmed(true);
    ts::FlushLog log;
    ts::LoggingObjectSink objects(log);
    ts::LoggingEdgeSink edges(log);
    ts::LoggingAnnotationSink annotations(log);
    ts::LoggingRasterSink raster(log);
    // Tile (3, 2) of 64-pixel tiles holds level pixel (3 * 64 + 5, 2 * 64 + 7).
    Pipeline p(scene, config(64, 4), probe(intParams({{"failX", 3 * 64 + 5}, {"failY", 2 * 64 + 7}})));
    p.stageName("nuclei").edgeSink(&edges).annotationSink(&annotations).rasterSink(&raster);

    try {
        p.runInto(objects, nullptr, CancellationToken());
        FAIL() << "no TileError";
    } catch (const TileError& e) {
        EXPECT_TRUE(e.hasTile());
        EXPECT_EQ(e.tileX(), 3);
        EXPECT_EQ(e.tileY(), 2);
        EXPECT_STREQ(e.stageName(), "nuclei");
        EXPECT_STREQ(e.operationName(), "test.probe");
        EXPECT_STREQ(e.cause(), "deliberate failure");
    }
    EXPECT_TRUE(log.entries().empty()) << "a sink was flushed on the failure path";

    // The pool is gone with the failed run: the same pipeline runs again.
    ts::ProbeOperation::setFailuresArmed(false);
    p.runInto(objects, nullptr, CancellationToken());
    EXPECT_GE(raster.tilesWritten(), 64);
    EXPECT_EQ(log.entries().size(), 4u);
}

TEST(TileExecutor, ProcessorErrorBecomesTileErrorAndCancelledErrorPassesThrough) {
    auto scene = gridScene();
    DisarmGuard guard;
    ts::ProbeOperation::setFailuresArmed(true);
    {
        Pipeline p(scene, config(64, 4), probe(intParams({{"failX", 0}, {"failY", 0}, {"failKind", 1}})));
        try {
            p.run(nullptr, CancellationToken());
            FAIL() << "no TileError";
        } catch (const TileError& e) {
            EXPECT_EQ(e.tileX(), 0);
            EXPECT_EQ(e.tileY(), 0);
            EXPECT_STREQ(e.stageName(), "pipeline");
            EXPECT_NE(std::string(e.cause()).find("deliberate failure"), std::string::npos);
        }
    }
    Pipeline p(scene, config(64, 4), probe(intParams({{"failX", 0}, {"failY", 0}, {"failKind", 2}})));
    EXPECT_THROW(p.run(nullptr, CancellationToken()), CancelledError);
}

TEST(TileExecutor, CancellationIsCancelledError) {
    auto scene = gridScene();
    ts::ProbeOperation::resetCounters();
    CancellationSource source;
    CancellingObserver observer(source, 5);
    ts::FlushLog log;
    ts::LoggingObjectSink objects(log);
    Pipeline p(scene, config(64, 4), probe(intParams({{"sleepMs", 2}})));
    EXPECT_THROW(p.runInto(objects, &observer, source.token()), CancelledError);
    EXPECT_LT(ts::ProbeOperation::applyCount(), 64);
    EXPECT_GE(ts::ProbeOperation::applyCount(), 5);
    EXPECT_TRUE(log.entries().empty());

    // A cancelled token stops a run before its first tile.
    ts::ProbeOperation::resetCounters();
    EXPECT_THROW(p.runInto(objects, nullptr, source.token()), CancelledError);
    EXPECT_EQ(ts::ProbeOperation::applyCount(), 0);
}

TEST(TileExecutor, ProgressReachesTotal) {
    auto scene = gridScene();
    RecordingObserver observer;
    Pipeline p(scene, config(64, 8), probe(OperationParameters()));
    p.stageName("nuclei");
    p.run(&observer, CancellationToken());

    EXPECT_FALSE(observer.overlapped_.load()) << "progress calls overlapped";
    ASSERT_EQ(observer.calls_.size(), 64u);
    for (size_t i = 0; i < observer.calls_.size(); ++i) {
        const auto& call = observer.calls_[i];
        EXPECT_EQ(call.stage, "nuclei");
        EXPECT_EQ(call.completed, static_cast<int64_t>(i + 1));
        EXPECT_EQ(call.total, 64);
        EXPECT_DOUBLE_EQ(call.fraction, static_cast<double>(i + 1) / 64.0);
    }
    EXPECT_EQ(observer.calls_.back().completed, 64);
}

TEST(TileExecutor, BudgetBoundsConcurrentTiles) {
    auto scene = gridScene();
    Pipeline p(scene, config(64, 8), probe(intParams({{"sleepMs", 5}})));
    p.validate();
    const int64_t tileBytes = detail::PipelineAccess::plan(p).tileBytes;
    ASSERT_GT(tileBytes, 0);
    p.memoryBudget(makeMemoryBudget(2 * tileBytes + tileBytes / 2));

    ts::ProbeOperation::resetCounters();
    p.run(nullptr, CancellationToken());
    EXPECT_EQ(ts::ProbeOperation::applyCount(), 64);
    EXPECT_EQ(ts::ProbeOperation::maxConcurrent(), 2);

    // Without the bound, eight workers do run eight tiles at once.
    p.memoryBudget(nullptr);
    ts::ProbeOperation::resetCounters();
    p.run(nullptr, CancellationToken());
    EXPECT_GT(ts::ProbeOperation::maxConcurrent(), 2);
}

TEST(TileExecutor, SinksFlushedInOrderOnSuccess) {
    auto scene = gridScene();
    ts::FlushLog log;
    ts::LoggingObjectSink objects(log);
    ts::LoggingEdgeSink edges(log);
    ts::LoggingAnnotationSink annotations(log);
    ts::LoggingRasterSink raster(log);
    Pipeline p(scene, config(64, 4), probe(OperationParameters()));
    p.rasterSink(&raster).annotationSink(&annotations).edgeSink(&edges);
    p.runInto(objects, nullptr, CancellationToken());
    EXPECT_EQ(log.entries(), (std::vector<std::string>{"object", "edge", "annotation", "raster"}));
    EXPECT_EQ(raster.tilesWritten(), 64);
}

TEST(TileExecutor, ExecutorReportsTilesAndPoolShutsDownTwice) {
    auto scene = gridScene();
    Pipeline p(scene, config(64, 3), probe(OperationParameters()));
    p.validate();
    WorkerPool pool(3, makeMemoryBudget(int64_t(1) << 30));
    TileExecutor executor(detail::PipelineAccess::plan(p).grid, pool, config(64, 3));
    EXPECT_EQ(executor.tilesTotal(), 64);
    EXPECT_EQ(executor.tilesCompleted(), 0);
    ts::CollectingObjectSink sink;
    executor.run(p, sink, nullptr, CancellationToken());
    EXPECT_EQ(executor.tilesCompleted(), 64);
    pool.waitIdle();
    pool.shutdown();
    pool.shutdown();
    EXPECT_THROW(executor.run(p, sink, nullptr, CancellationToken()), InvalidArgumentError);
}

// The 3 x 2 grid of 64-pixel tiles the truncated-count tests run over.
static std::shared_ptr<slideio::Scene> truncationScene() {
    ts::registerTestOperations();
    ts::registerProbeOperation();
    return ts::makeSyntheticScene({{192, 128, 0, 0}}, {slideio::DataType::DT_Byte}, texture);
}

TEST(Executor, SumsTruncatedObjects) {
    auto scene = truncationScene();
    for (const int32_t workers : {1, 4}) {
        Pipeline p(scene, config(64, workers), probe(intParams({{"truncated", 1}})));
        EXPECT_EQ(p.truncatedObjectCount(), 0) << "a fresh pipeline";
        p.run(nullptr, CancellationToken());
        EXPECT_EQ(p.truncatedObjectCount(), 6) << workers << " workers";
        // A second run reports its own count.
        p.run(nullptr, CancellationToken());
        EXPECT_EQ(p.truncatedObjectCount(), 6) << workers << " workers, second run";
    }
    Pipeline quiet(scene, config(64, 4), probe(OperationParameters()));
    quiet.run(nullptr, CancellationToken());
    EXPECT_EQ(quiet.truncatedObjectCount(), 0);
}

TEST(Executor, RefusesANegativeTruncatedCount) {
    auto scene = truncationScene();
    Pipeline p(scene, config(64, 2), probe(intParams({{"truncated", -1}})));
    // The operation's InvalidArgumentError is a ProcessorError, so it reaches the caller
    // as the run's TileError, with the operation named and the refusal as the cause.
    try {
        p.run(nullptr, CancellationToken());
        FAIL() << "no TileError";
    } catch (const TileError& e) {
        EXPECT_STREQ(e.operationName(), "test.probe");
        EXPECT_NE(std::string(e.cause()).find("countTruncatedObjects"), std::string::npos) << e.cause();
    }
    EXPECT_EQ(p.truncatedObjectCount(), 0);
}
