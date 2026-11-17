// Requirements: UR-SEG-3, UR-STG-3, UR-EXE-5
#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/control.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/core/measurement.hpp>
#include <slideio/processor/core/memory_budget.hpp>
#include <slideio/processor/core/object_record.hpp>
#include <slideio/processor/core/raster_mask.hpp>
#include <slideio/processor/pipeline/pipeline.hpp>

#include <pipeline/detail/pipeline_impl.hpp>
#include <pipeline/detail/restriction.hpp>
#include <pipeline/test_support/disc_mask_source.hpp>
#include <pipeline/test_support/executor_support.hpp>
#include <pipeline/test_support/synthetic_scene.hpp>
#include <pipeline/test_support/test_operations.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

using namespace slideio::processor;
namespace ts = slideio::processor::testing_support;
namespace fs = std::filesystem;

namespace {

constexpr int64_t kSide = 512;

// Separate rectangular blobs of 2..6 pixels a side, one per 13 x 11 cell; many
// straddle the seams of 64- and 100-pixel tiles.
double blobs(int, int64_t x, int64_t y) {
    const int64_t cx = x / 13;
    const int64_t cy = y / 11;
    const int64_t w = 2 + (cx * 7 + cy * 3) % 5;
    const int64_t h = 2 + (cx * 5 + cy * 11) % 5;
    return x % 13 >= 1 && x % 13 < 1 + w && y % 11 >= 1 && y % 11 < 1 + h ? 255.0 : 0.0;
}

double texture(int, int64_t x, int64_t y) { return static_cast<double>((x * 37 + y * 91 + (x * y) % 13) % 256); }

std::shared_ptr<slideio::Scene> blobScene() {
    ts::registerTestOperations();
    ts::registerProbeOperation();
    return ts::makeSyntheticScene({{kSide, kSide, 0, 0}}, {slideio::DataType::DT_Byte}, blobs);
}

// A fresh, empty directory under the temporary directory, removed with the object.
class TestDirectory {
public:
    TestDirectory() {
        std::random_device device;
        path_ = fs::temp_directory_path() /
                ("seam-run-test-" + std::to_string(device()) + "-" + std::to_string(device()));
        fs::create_directories(path_);
    }
    ~TestDirectory() {
        std::error_code error;
        fs::remove_all(path_, error);
    }
    TestDirectory(const TestDirectory&) = delete;
    TestDirectory& operator=(const TestDirectory&) = delete;

    std::string utf8() const { return path_.u8string(); }
    // The names of the entries directly inside it.
    std::vector<std::string> entries() const {
        std::vector<std::string> names;
        std::error_code error;
        for (fs::directory_iterator it(path_, error), end; !error && it != end; it.increment(error)) {
            names.push_back(it->path().filename().u8string());
        }
        return names;
    }

private:
    fs::path path_;
};

Configuration config(int32_t tileSide, int32_t workers, const std::string& scratch = std::string()) {
    return ConfigurationBuilder()
        .tileSize(tileSide, tileSide)
        .tileSizeFromSlide(false)
        .workerCount(workers)
        .scratchDirectory(scratch.c_str())
        .build();
}

OperationParameters seams(bool on) {
    OperationParameters p;
    p.setBool("seams", on);
    return p;
}

PipelineSpec components(bool withSeams) {
    PipelineSpec spec;
    spec.operation("test.components", seams(withSeams));
    return spec;
}

using Ring = std::vector<std::pair<int64_t, int64_t>>;
using ObjectKey = std::tuple<ObjectId, std::vector<std::pair<bool, Ring>>,
                             std::vector<std::tuple<std::string, double, int32_t>>>;

// Every record as its id, rings and measurements, sorted.
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
            measurements.emplace_back(measurement.name(), measurement.value(),
                                      static_cast<int32_t>(measurement.unit()));
        }
        keys.emplace_back(record.id(), std::move(rings), std::move(measurements));
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

std::vector<ObjectId> sortedIds(const std::vector<ObjectRecord>& records) {
    std::vector<ObjectId> ids;
    for (const ObjectRecord& r : records) {
        ids.push_back(r.id());
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

// True when the object's box is cut by a seam of `tile`-pixel tiles starting at `origin`.
bool crossesSeam(const ObjectRecord& record, int64_t tile, int64_t origin) {
    const Rect64 box = record.geometry().boundingBox();
    const auto cut = [&](int64_t from, int64_t extent) {
        const int64_t firstSeam = origin + ((from - origin) / tile + 1) * tile;
        return firstSeam < from + extent;
    };
    return cut(box.x, box.width) || cut(box.y, box.height);
}

// A raster sink that notes, at every tile, whether the scratch directory holds a
// per-run directory, and remembers each one's name.
class ScratchWatchingRasterSink : public RasterSink {
public:
    explicit ScratchWatchingRasterSink(const TestDirectory& scratch) : scratch_(scratch) {}
    void writeTile(const BlockView&) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const std::string& name : scratch_.entries()) {
            if (name.rfind("pipeline-", 0) == 0) {
                seen_.push_back(name);
            }
        }
        ++written_;
    }
    void flush() override {}
    int64_t tilesWritten() const override { return written_; }
    std::vector<std::string> seen() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return seen_;
    }

private:
    const TestDirectory& scratch_;
    mutable std::mutex mutex_;
    std::vector<std::string> seen_;
    std::atomic<int64_t> written_{0};
};

bool isRunDirectoryName(const std::string& name) {
    const std::string prefix = "pipeline-";
    if (name.size() != prefix.size() + 32 || name.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }
    return std::all_of(name.begin() + static_cast<std::ptrdiff_t>(prefix.size()), name.end(),
                       [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

struct DisarmGuard {
    ~DisarmGuard() { ts::ProbeOperation::setFailuresArmed(true); }
};

} // namespace

// Objects cut by tile seams are merged during the run: the store gets each object
// once, with the polygon, identifier and measurements of the single-tile run.
TEST(SeamRun, ObjectsAcrossSeamsAreStoredOnce) {
    auto scene = blobScene();
    ts::CollectingObjectSink whole;
    Pipeline single(scene, config(static_cast<int32_t>(kSide), 1), components(false));
    single.bindLayer("objects", LayerId(3));
    single.runInto(whole, nullptr, CancellationToken());
    const std::vector<ObjectKey> reference = sortedKeys(whole.records());
    ASSERT_GT(reference.size(), 1000u);

    for (const int32_t tile : {64, 100}) {
        int64_t crossing = 0;
        for (const ObjectRecord& r : whole.records()) {
            crossing += crossesSeam(r, tile, 0) ? 1 : 0;
        }
        ASSERT_GT(crossing, 30) << "the fixture should cut many objects at " << tile << "-pixel seams";
        for (const int32_t workers : {1, 4}) {
            ts::CollectingObjectSink stored;
            Pipeline p(scene, config(tile, workers), components(true));
            p.bindLayer("objects", LayerId(3));
            p.runInto(stored, nullptr, CancellationToken());
            EXPECT_EQ(stored.records().size(), reference.size()) << tile << "-pixel tiles, " << workers << " workers";
            EXPECT_TRUE(sortedKeys(stored.records()) == reference)
                << tile << "-pixel tiles, " << workers << " workers: objects differ from the single-tile run";
        }
    }
}

// The restriction tests a merged object by its anchor: one whose anchor lies
// outside the polygon is not stored, though its fragments came from tiles inside.
TEST(SeamRun, RestrictionAppliesToMergedObjects) {
    auto scene = blobScene();
    // The bounding box starts on a tile corner, so the restricted grid's seams are the unrestricted run's.
    const Point64 corners[] = {{64, 64}, {448, 64}, {64, 448}};
    Polygon triangle;
    triangle.addRing(corners, 3, false);

    ts::CollectingObjectSink all;
    Pipeline open(scene, config(static_cast<int32_t>(kSide), 1), components(false));
    open.bindLayer("objects", LayerId(3));
    open.runInto(all, nullptr, CancellationToken());

    PipelineSpec spec = components(true);
    spec.restrictToPolygon(triangle);
    ts::CollectingObjectSink stored;
    Pipeline restricted(scene, config(64, 4), spec);
    restricted.bindLayer("objects", LayerId(3));
    restricted.runInto(stored, nullptr, CancellationToken());

    RasterMask mask(Rect64{0, 0, kSide, kSide}, 0, 1.0);
    rasteriseInto(mask, triangle);
    std::vector<ObjectId> expected;
    int64_t keptAcrossSeams = 0;
    int64_t droppedAcrossSeams = 0;
    for (const ObjectRecord& r : all.records()) {
        const Rect64 box = r.geometry().boundingBox();
        const bool inGrid = box.x >= 64 && box.y >= 64 && box.x + box.width <= 448 && box.y + box.height <= 448;
        if (mask.testLevel0(r.geometry().anchor())) {
            expected.push_back(r.id());
            keptAcrossSeams += inGrid && crossesSeam(r, 64, 64) ? 1 : 0;
        } else {
            droppedAcrossSeams += inGrid && crossesSeam(r, 64, 64) ? 1 : 0;
        }
    }
    std::sort(expected.begin(), expected.end());
    ASSERT_GT(keptAcrossSeams, 10) << "the fixture should keep merged objects";
    ASSERT_GT(droppedAcrossSeams, 10) << "the fixture should drop merged objects lying in the grid";
    EXPECT_EQ(sortedIds(stored.records()), expected);
    for (const ObjectRecord& r : stored.records()) {
        EXPECT_TRUE(mask.testLevel0(r.geometry().anchor())) << "an object with its anchor outside was stored";
    }
}

// A run with an object-producing chain works in a directory of its own under the
// configured scratch directory, and removes it after the run: on success, after a
// tile fails, and after cancellation.
TEST(SeamRun, ScratchDirectoryRemovedAfterRun) {
    auto scene = blobScene();
    TestDirectory scratch;

    {
        ScratchWatchingRasterSink raster(scratch);
        ts::CollectingObjectSink stored;
        Pipeline p(scene, config(64, 4, scratch.utf8()), components(true));
        p.bindLayer("objects", LayerId(3));
        p.rasterSink(&raster);
        p.runInto(stored, nullptr, CancellationToken());
        const std::vector<std::string> seen = raster.seen();
        ASSERT_FALSE(seen.empty()) << "no per-run directory existed during the run";
        EXPECT_TRUE(std::all_of(seen.begin(), seen.end(), [&](const std::string& n) { return n == seen.front(); }))
            << "one run, one directory";
        EXPECT_TRUE(isRunDirectoryName(seen.front())) << seen.front();
        EXPECT_TRUE(scratch.entries().empty()) << "the per-run directory survived a successful run";
    }

    // A tile fails in the probe before the components; then a probe cancels.
    DisarmGuard guard;
    ts::ProbeOperation::setFailuresArmed(true);
    for (const int64_t kind : {0, 2}) {
        PipelineSpec spec;
        OperationParameters probe;
        probe.setInt("copy", 1);
        probe.setInt("failX", 3 * 64 + 5);
        probe.setInt("failY", 2 * 64 + 7);
        probe.setInt("failKind", kind);
        spec.operation("test.probe", probe);
        spec.operation("test.threshold", OperationParameters());
        spec.operation("test.components", seams(true));
        ScratchWatchingRasterSink raster(scratch);
        ts::CollectingObjectSink stored;
        Pipeline p(scene, config(64, 4, scratch.utf8()), spec);
        p.bindLayer("objects", LayerId(3));
        p.rasterSink(&raster);
        if (kind == 0) {
            EXPECT_THROW(p.runInto(stored, nullptr, CancellationToken()), TileError);
        } else {
            EXPECT_THROW(p.runInto(stored, nullptr, CancellationToken()), CancelledError);
        }
        EXPECT_FALSE(raster.seen().empty()) << "failure kind " << kind << ": no per-run directory during the run";
        EXPECT_TRUE(scratch.entries().empty()) << "failure kind " << kind << ": the per-run directory survived";
    }
}

// A chain that produces no objects has no merger and creates no scratch directory.
TEST(SeamRun, NoMergerForRasterOnlyChains) {
    auto scene = blobScene();
    TestDirectory scratch;
    ScratchWatchingRasterSink raster(scratch);
    PipelineSpec spec;
    OperationParameters radius;
    radius.setInt("radius", 1);
    spec.operation("test.boxMean", radius);
    Pipeline p(scene, config(64, 4, scratch.utf8()), spec);
    p.rasterSink(&raster);
    p.run(nullptr, CancellationToken());
    EXPECT_EQ(raster.tilesWritten(), 64);
    EXPECT_TRUE(raster.seen().empty()) << "a raster-only run created a scratch directory";
    EXPECT_TRUE(scratch.entries().empty());
}

// The restriction's cached tile masks are released as their neighbourhoods
// complete, so the cache holds only the masks of tiles some neighbour of which is
// still running; a mask built again after its tile settled is released at the next
// completion.
TEST(SeamRun, SettledTileMasksAreReleased) {
    ts::registerTestOperations();
    auto scene = ts::makeSyntheticScene({{kSide, kSide, 0, 0}}, {slideio::DataType::DT_Byte}, texture);
    ts::DiscMaskSource disc(250.0, 230.0, 400.0);
    PipelineSpec spec;
    spec.operation("test.components", OperationParameters());
    Pipeline p(scene, config(64, 4), spec);
    p.maskSource(&disc);
    p.bindLayer("objects", LayerId(3));
    p.validate();
    auto& impl = detail::PipelineAccess::impl(p);
    const detail::RunPlan& plan = detail::PipelineAccess::plan(p);
    const TileGrid& grid = plan.grid;
    ASSERT_EQ(grid.columns(), 8);
    ASSERT_EQ(grid.rows(), 8);
    const detail::RestrictionFilter filter(plan, impl.spec, *impl.selector, impl.maskSource);
    detail::TileMaskRelease release(filter, grid);

    // One anchor in every tile caches every tile's mask.
    for (int64_t t = 0; t < grid.tileCount(); ++t) {
        const Rect64 r = grid.tileRect(t);
        EXPECT_TRUE(release.admits(Point64{r.x + 1, r.y + 1}));
    }
    EXPECT_EQ(filter.cachedTileMaskCount(), 64);

    // Completing the first two rows settles row 0 only: row 1 still has neighbours in row 2.
    for (int64_t t = 0; t < 16; ++t) {
        release.tileCompleted(t);
    }
    EXPECT_EQ(filter.cachedTileMaskCount(), 64 - 8);

    // An anchor in a settled tile builds its mask again; the next completion drops it.
    EXPECT_TRUE(release.admits(Point64{1, 1}));
    EXPECT_EQ(filter.cachedTileMaskCount(), 64 - 8 + 1);
    release.tileCompleted(16);
    EXPECT_EQ(filter.cachedTileMaskCount(), 64 - 8);

    // Completing the rest, in reverse order, releases everything.
    for (int64_t t = grid.tileCount() - 1; t > 16; --t) {
        release.tileCompleted(t);
    }
    EXPECT_EQ(filter.cachedTileMaskCount(), 0);
}

namespace {

// What a run on another thread reports back; shared with that thread, which may
// outlive the test if the run hangs.
struct BackgroundRun {
    std::mutex mutex;
    std::condition_variable done;
    bool finished = false;
    std::string error;
    int64_t stored = 0;
};

// Runs a seam-emitting components chain with a memory ceiling of `tilesHeld`
// tiles' bytes and `workers` workers, on a thread of its own; false when it did
// not finish within the bound.
bool runsWithinBound(int32_t workers, int64_t tilesHeld, int64_t& stored, std::string& error) {
    auto scene = blobScene();
    auto pipeline = std::make_shared<Pipeline>(scene, config(64, workers), components(true));
    pipeline->bindLayer("objects", LayerId(3));
    pipeline->validate();
    const int64_t tileBytes = detail::PipelineAccess::plan(*pipeline).tileBytes;
    pipeline->memoryBudget(makeMemoryBudget(tilesHeld * tileBytes));
    pipeline->validate();
    EXPECT_EQ(detail::PipelineAccess::plan(*pipeline).tileBytes, tileBytes);

    auto cancel = std::make_shared<CancellationSource>();
    auto run = std::make_shared<BackgroundRun>();
    std::thread([pipeline, cancel, run] {
        std::string failure;
        auto sink = std::make_shared<ts::CollectingObjectSink>();
        try {
            pipeline->runInto(*sink, nullptr, cancel->token());
        } catch (const std::exception& e) {
            failure = e.what();
        }
        std::lock_guard<std::mutex> lock(run->mutex);
        run->error = failure;
        run->stored = sink->emittedCount();
        run->finished = true;
        run->done.notify_all();
    }).detach();

    std::unique_lock<std::mutex> lock(run->mutex);
    const bool finished = run->done.wait_for(lock, std::chrono::seconds(30), [&] { return run->finished; });
    if (!finished) {
        cancel->cancel();  // Leaves the hung thread to the process's exit.
        return false;
    }
    stored = run->stored;
    error = run->error;
    return true;
}

} // namespace

// A memory ceiling of exactly the tiles the workers hold leaves no room for seam
// fragments: they go to the scratch file instead of waiting for memory that only
// the waiting workers themselves could give back.
TEST(SeamRun, TightBudgetDoesNotWaitForMemory) {
    ts::CollectingObjectSink roomy;
    {
        Pipeline p(blobScene(), config(64, 4), components(true));
        p.bindLayer("objects", LayerId(3));
        p.runInto(roomy, nullptr, CancellationToken());
    }
    for (const int32_t workers : {1, 4}) {
        int64_t stored = 0;
        std::string error;
        ASSERT_TRUE(runsWithinBound(workers, workers, stored, error))
            << workers << " workers: the run did not finish within 30 s";
        EXPECT_EQ(error, "") << workers << " workers";
        EXPECT_EQ(stored, roomy.emittedCount()) << workers << " workers";
    }
}
