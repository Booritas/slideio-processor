// Requirements: UR-SEG-3, UR-STG-3, UR-EXE-5, UR-IMG-5
#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/control.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/core/measurement.hpp>
#include <slideio/processor/core/memory_budget.hpp>
#include <slideio/processor/core/object_record.hpp>
#include <slideio/processor/core/raster_mask.hpp>
#include <slideio/processor/pipeline/pipeline.hpp>
#include <slideio/processor/pipeline/tile_grid.hpp>

#include <pipeline/detail/pipeline_impl.hpp>
#include <pipeline/detail/restriction.hpp>
#include <pipeline/detail/seam_merge_failure.hpp>
#include <pipeline/test_support/disc_mask_source.hpp>
#include <pipeline/test_support/executor_support.hpp>
#include <pipeline/test_support/synthetic_scene.hpp>
#include <pipeline/test_support/test_operations.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
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

namespace {

// The objects of an unrestricted single-tile run of `components`.
std::vector<ObjectRecord> unrestrictedObjects(const std::shared_ptr<slideio::Scene>& scene) {
    ts::CollectingObjectSink all;
    Pipeline open(scene, config(static_cast<int32_t>(kSide), 1), components(false));
    open.bindLayer("objects", LayerId(3));
    open.runInto(all, nullptr, CancellationToken());
    return all.records();
}

bool insideRect(const Rect64& r, const Point64& p) {
    return p.x >= r.x && p.y >= r.y && p.x < r.x + r.width && p.y < r.y + r.height;
}

// What a restricted run stored, and the tiles it evaluated on demand.
struct StoredRun {
    std::vector<ObjectRecord> records;
    int64_t onDemand = 0;
};

StoredRun runRestrictedBy(const std::shared_ptr<slideio::Scene>& scene, const PipelineSpec& spec, int32_t workers,
                          ts::DiscMaskSource* disc = nullptr) {
    ts::CollectingObjectSink stored;
    Pipeline p(scene, config(64, workers), spec);
    if (disc != nullptr) {
        p.maskSource(disc);
    }
    p.bindLayer("objects", LayerId(3));
    p.runInto(stored, nullptr, CancellationToken());
    return StoredRun{stored.records(), detail::PipelineAccess::impl(p).counters.tilesEvaluatedOnDemand.load()};
}

std::vector<ObjectRecord> filtered(const std::vector<ObjectRecord>& records,
                                   const std::function<bool(const ObjectRecord&)>& keep) {
    std::vector<ObjectRecord> kept;
    for (const ObjectRecord& r : records) {
        if (keep(r)) {
            kept.push_back(r);
        }
    }
    return kept;
}

} // namespace

// An ROI whose edges cut blobs: every object anchored inside is stored whole, on
// all four edges, and one anchored outside is not stored, though a cut part of
// it lies inside. (The fixture's blobs are rectangles, so one crossing the left
// edge has its anchor, the top-left corner, outside.)
TEST(SeamRun, RoiEdgeObjectIsNotTruncated) {
    auto scene = blobScene();
    const Rect64 roi{70, 80, 311, 298}; // edges at 70, 80, 381, 378, inside blobs
    const std::vector<ObjectRecord> all = unrestrictedObjects(scene);
    const std::vector<ObjectRecord> expected =
        filtered(all, [&](const ObjectRecord& r) { return insideRect(roi, r.geometry().anchor()); });

    int64_t crossingRightOrBottom = 0;
    int64_t crossingLeftAnchoredOutside = 0;
    int64_t crossingTopWithCornerInside = 0;
    for (const ObjectRecord& r : all) {
        const Point64 a = r.geometry().anchor();
        const Rect64 box = r.geometry().boundingBox();
        if (insideRect(roi, a)) {
            crossingRightOrBottom +=
                box.x + box.width > roi.x + roi.width || box.y + box.height > roi.y + roi.height ? 1 : 0;
        }
        // Cut at the left edge, the part inside would start at the ROI's edge, in the anchor's row.
        crossingLeftAnchoredOutside +=
            box.x < roi.x && box.x + box.width > roi.x && a.y >= roi.y && a.y < roi.y + roi.height ? 1 : 0;
        crossingTopWithCornerInside += box.y < roi.y && box.y + box.height > roi.y && box.x >= roi.x &&
                                               box.x < roi.x + roi.width
                                           ? 1
                                           : 0;
    }
    ASSERT_GT(crossingRightOrBottom, 5) << "the fixture should have objects crossing the right or bottom edge";
    ASSERT_GT(crossingLeftAnchoredOutside, 5) << "the fixture should have objects crossing the left edge";
    ASSERT_GT(crossingTopWithCornerInside, 5) << "the fixture should have objects crossing the top edge";

    PipelineSpec spec = components(true);
    spec.restrictTo(roi);
    for (const int32_t workers : {1, 4, 8}) {
        const StoredRun run = runRestrictedBy(scene, spec, workers);
        EXPECT_EQ(sortedIds(run.records), sortedIds(expected)) << workers << " workers: the same objects are stored";
        EXPECT_TRUE(sortedKeys(run.records) == sortedKeys(expected))
            << workers << " workers: an object crossing the edge is stored cut";
        EXPECT_GT(run.onDemand, 0) << workers << " workers: tiles outside the ROI are evaluated for crossing objects";
    }
}

namespace {

// The objects of an unrestricted single-tile run of `components` at level 1.
std::vector<ObjectRecord> unrestrictedObjectsAtLevelOne(const std::shared_ptr<slideio::Scene>& scene) {
    ts::CollectingObjectSink all;
    PipelineSpec spec = components(false);
    spec.analysisLevel(1);
    Pipeline open(scene, config(static_cast<int32_t>(kSide), 1), spec);
    open.bindLayer("objects", LayerId(3));
    open.runInto(all, nullptr, CancellationToken());
    return all.records();
}

// An ROI run at level 1 (seam merging on, 64-pixel tiles) stores exactly the unrestricted objects
// whose level-0 anchor lies in the ROI, whole, at 1 and 4 workers.
void expectRoiAtLevelOneKeepsItsAnchors(int64_t level0Side, int64_t level1Side, const Rect64& roi) {
    ts::registerTestOperations();
    auto scene = ts::makeSyntheticScene({{level0Side, level0Side, 0, 0}, {level1Side, level1Side, 0, 0}},
                                        {slideio::DataType::DT_Byte}, blobs);
    const std::vector<ObjectRecord> all = unrestrictedObjectsAtLevelOne(scene);
    const std::vector<ObjectRecord> expected =
        filtered(all, [&](const ObjectRecord& r) { return insideRect(roi, r.geometry().anchor()); });

    // Objects anchored outside the ROI, in a level pixel that holds one of its edge anchors: the
    // mask sets that pixel, so only the exact test refuses them.
    const LevelSelector selector(scene);
    // The level pixel holding a level-0 point, as the restriction tests it.
    const auto levelPixel = [&](Point64 p) {
        const Rect64 pixel = selector.mapToLevel(Rect64{p.x, p.y, 1, 1}, 1);
        return Point64{pixel.x, pixel.y};
    };
    const int64_t px0 = levelPixel(Point64{roi.x, roi.y}).x;
    const int64_t px1 = levelPixel(Point64{roi.x + roi.width - 1, roi.y}).x;
    const int64_t py0 = levelPixel(Point64{roi.x, roi.y}).y;
    const int64_t py1 = levelPixel(Point64{roi.x, roi.y + roi.height - 1}).y;
    int64_t edgePixelOutside = 0;
    int64_t crossingSeam = 0;
    for (const ObjectRecord& r : all) {
        const Point64 a = r.geometry().anchor();
        const int64_t lx = levelPixel(a).x;
        const int64_t ly = levelPixel(a).y;
        edgePixelOutside += !insideRect(roi, a) && lx >= px0 && lx <= px1 && ly >= py0 && ly <= py1 ? 1 : 0;
        crossingSeam += insideRect(roi, a) && crossesSeam(r, 64, 0) ? 1 : 0;
    }
    ASSERT_GT(edgePixelOutside, 0) << "the fixture should have an object anchored in an edge level pixel, outside the ROI";
    ASSERT_GT(crossingSeam, 0) << "the fixture should have admitted objects crossing a tile seam";
    ASSERT_FALSE(expected.empty());

    PipelineSpec spec = components(true);
    spec.analysisLevel(1).restrictTo(roi);
    for (const int32_t workers : {1, 4}) {
        const StoredRun run = runRestrictedBy(scene, spec, workers);
        EXPECT_EQ(sortedIds(run.records), sortedIds(expected)) << workers << " workers: the same objects are stored";
        EXPECT_TRUE(sortedKeys(run.records) == sortedKeys(expected)) << workers << " workers: stored whole";
    }
}

} // namespace

// Scale 2.4: the ROI's edges lie inside level pixels, so the pixel holding an edge anchor can also
// hold anchors outside the ROI. The seam-merged store keeps exactly the objects anchored in the ROI.
TEST(SeamRun, RoiAtLevelOneKeepsExactlyItsAnchors) {
    expectRoiAtLevelOneKeepsItsAnchors(600, 250, Rect64{253, 188, 150, 190});
}

// Scale 2: an ROI edge inside a level pixel; anchors one level-0 column or row outside are
// refused, though they fall in the same level pixel.
TEST(SeamRun, RoiAtLevelOneOfAnIntegerScaleKeepsExactlyItsAnchors) {
    expectRoiAtLevelOneKeepsItsAnchors(512, 256, Rect64{211, 157, 200, 160});
}

// A polygon whose bounding-box edges cut blobs: the objects stored are those of
// the unrestricted run whose anchor the polygon's mask admits, whole.
TEST(SeamRun, PolygonEdgeObjectIsNotTruncated) {
    auto scene = blobScene();
    const Point64 corners[] = {{70, 70}, {380, 90}, {400, 380}, {90, 410}};
    Polygon quad;
    quad.addRing(corners, 4, false);
    const Rect64 bounds = quad.boundingBox();
    RasterMask mask(Rect64{0, 0, kSide, kSide}, 0, 1.0);
    rasteriseInto(mask, quad);

    const std::vector<ObjectRecord> all = unrestrictedObjects(scene);
    const std::vector<ObjectRecord> expected =
        filtered(all, [&](const ObjectRecord& r) { return mask.testLevel0(r.geometry().anchor()); });
    int64_t admittedCrossingBounds = 0;
    int64_t droppedCrossingIn = 0;
    for (const ObjectRecord& r : all) {
        const Rect64 box = r.geometry().boundingBox();
        const bool crossesBounds = box.x < bounds.x || box.y < bounds.y ||
                                   box.x + box.width > bounds.x + bounds.width ||
                                   box.y + box.height > bounds.y + bounds.height;
        const bool touchesBounds = box.x + box.width > bounds.x && box.y + box.height > bounds.y &&
                                   box.x < bounds.x + bounds.width && box.y < bounds.y + bounds.height;
        if (mask.testLevel0(r.geometry().anchor())) {
            admittedCrossingBounds += crossesBounds ? 1 : 0;
        } else {
            droppedCrossingIn += crossesBounds && touchesBounds ? 1 : 0;
        }
    }
    ASSERT_GT(admittedCrossingBounds, 0) << "the fixture should keep objects crossing the bounding box";
    ASSERT_GT(droppedCrossingIn, 5) << "the fixture should drop objects crossing into the bounding box";

    PipelineSpec spec = components(true);
    spec.restrictToPolygon(quad);
    for (const int32_t workers : {1, 4, 8}) {
        const StoredRun run = runRestrictedBy(scene, spec, workers);
        EXPECT_EQ(sortedIds(run.records), sortedIds(expected)) << workers << " workers: the same objects are stored";
        EXPECT_TRUE(sortedKeys(run.records) == sortedKeys(expected)) << workers << " workers: objects differ";
        EXPECT_GT(run.onDemand, 0) << workers << " workers";
    }
}

// An ROI reaching the scene's right and bottom edges has no tile beyond it, and
// objects there are stored as in the unrestricted run.
TEST(SeamRun, RoiAtTheSceneEdgeMatchesTheUnrestrictedRun) {
    auto scene = blobScene();
    const Rect64 roi{256, 256, kSide - 256, kSide - 256};
    const std::vector<ObjectRecord> all = unrestrictedObjects(scene);
    const std::vector<ObjectRecord> expected =
        filtered(all, [&](const ObjectRecord& r) { return insideRect(roi, r.geometry().anchor()); });
    ASSERT_GT(expected.size(), 100u);

    PipelineSpec spec = components(true);
    spec.restrictTo(roi);
    for (const int32_t workers : {1, 4}) {
        const StoredRun run = runRestrictedBy(scene, spec, workers);
        EXPECT_TRUE(sortedKeys(run.records) == sortedKeys(expected)) << workers << " workers";
        EXPECT_GT(run.onDemand, 0) << workers << " workers: objects cross the ROI's left and top edges";
    }
}

// An ROI and a mask source together: an object is stored when its anchor is in
// both, and whole.
TEST(SeamRun, RoiWithAMaskSourceStoresWholeObjects) {
    auto scene = blobScene();
    const Rect64 roi{70, 80, 311, 298}; // edges at 70, 80, 381, 378, inside blobs
    ts::DiscMaskSource disc(370.0, 230.0, 150.0); // reaches the right and bottom edges
    const std::vector<ObjectRecord> all = unrestrictedObjects(scene);
    const std::vector<ObjectRecord> inRoi =
        filtered(all, [&](const ObjectRecord& r) { return insideRect(roi, r.geometry().anchor()); });
    const std::vector<ObjectRecord> expected = filtered(inRoi, [&](const ObjectRecord& r) {
        const Point64 a = r.geometry().anchor();
        return disc.contains(static_cast<double>(a.x) + 0.5, static_cast<double>(a.y) + 0.5);
    });
    ASSERT_GT(expected.size(), 100u);
    int64_t crossingTheEdge = 0;
    for (const ObjectRecord& r : expected) {
        const Rect64 box = r.geometry().boundingBox();
        crossingTheEdge += box.x + box.width > roi.x + roi.width || box.y + box.height > roi.y + roi.height ? 1 : 0;
    }
    ASSERT_GT(crossingTheEdge, 5) << "the fixture should have admitted objects crossing the ROI edge";
    ASSERT_LT(expected.size(), inRoi.size()) << "the mask should exclude some objects the ROI admits";

    PipelineSpec spec = components(true);
    spec.restrictTo(roi);
    for (const int32_t workers : {1, 4}) {
        const StoredRun run = runRestrictedBy(scene, spec, workers, &disc);
        EXPECT_EQ(sortedIds(run.records), sortedIds(expected)) << workers << " workers";
        EXPECT_TRUE(sortedKeys(run.records) == sortedKeys(expected)) << workers << " workers";
        EXPECT_GT(run.onDemand, 0) << workers << " workers";
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

namespace {

// Collects objects, but fails, with a standard exception, on the first object
// cut by a seam of 64-pixel tiles: one only the seam merge produces.
class FailingOnMergedSink : public ObjectSink {
public:
    void emit(const ObjectRecord& record) override {
        if (crossesSeam(record, 64, 0)) {
            throw std::runtime_error("the store refused a merged object");
        }
        ++emitted_;
    }
    void flush() override {}
    int64_t emittedCount() const override { return emitted_; }

private:
    std::atomic<int64_t> emitted_{0};
};

std::exception_ptr thrown(const std::function<void()>& raise) {
    try {
        raise();
    } catch (...) {
        return std::current_exception();
    }
    return nullptr;
}

} // namespace

// A standard exception from the object sink while merged objects are emitted
// reaches the caller as a TileError, the original text as its cause.
TEST(SeamRun, ObjectSinkFailureWhileMergingIsTranslated) {
    FailingOnMergedSink sink;
    Pipeline p(blobScene(), config(64, 4), components(true));
    p.bindLayer("objects", LayerId(3));
    try {
        p.runInto(sink, nullptr, CancellationToken());
        FAIL() << "the sink's failure was lost";
    } catch (const TileError& e) {
        EXPECT_TRUE(e.hasTile());
        EXPECT_STREQ(e.cause(), "the store refused a merged object");
    }
}

// What the seam merge after the last tile throws reaches the caller as the run
// reports failures: a ProcessorError (a cancellation included) and
// std::bad_alloc as they are, anything else as a TileError with no tile.
TEST(SeamRun, SeamMergeFailuresAreTranslated) {
    const auto translated = [](const std::exception_ptr& failure) {
        return thrown([&] { detail::rethrowSeamMergeFailure(failure, "stage"); });
    };
    for (const std::exception_ptr& failure :
         {thrown([] { throw std::runtime_error("the store refused an object"); }),
          thrown([] { throw std::system_error(std::make_error_code(std::errc::resource_deadlock_would_occur)); }),
          thrown([] { throw 7; })}) {
        try {
            std::rethrow_exception(translated(failure));
        } catch (const TileError& e) {
            EXPECT_FALSE(e.hasTile());
            EXPECT_STREQ(e.stageName(), "stage");
            EXPECT_STRNE(e.cause(), "");
        } catch (...) {
            ADD_FAILURE() << "not translated to a TileError";
        }
    }
    EXPECT_THROW(std::rethrow_exception(translated(thrown([] { throw IoError("read back"); }))), IoError);
    EXPECT_THROW(std::rethrow_exception(translated(thrown([] { throw CancelledError("cancelled"); }))),
                 CancelledError);
    EXPECT_THROW(std::rethrow_exception(translated(thrown([] { throw InternalError("still open"); }))),
                 InternalError);
    EXPECT_THROW(std::rethrow_exception(translated(thrown([] { throw std::bad_alloc(); }))), std::bad_alloc);
}

#ifdef _WIN32
// A scratch directory whose path cannot be formed (not UTF-8) is an IoError
// naming the run's scratch directory, not a raw standard exception.
TEST(SeamRun, UnusableScratchDirectoryIsAnIoError) {
    ts::CollectingObjectSink stored;
    Pipeline p(blobScene(), config(64, 1, "\xff\xfe-not-utf8"), components(true));
    p.bindLayer("objects", LayerId(3));
    try {
        p.runInto(stored, nullptr, CancellationToken());
        FAIL() << "the run did not fail";
    } catch (const IoError& e) {
        EXPECT_NE(std::string(e.what()).find("scratch directory"), std::string::npos) << e.what();
        EXPECT_STRNE(e.cause(), "");
    } catch (const std::exception& e) {
        FAIL() << "untranslated: " << e.what();
    }
}
#endif

namespace {

// A scene of `width` x `height` Byte samples, 255 inside one of `rects` and 0 elsewhere.
std::shared_ptr<slideio::Scene> rectScene(int64_t width, int64_t height, std::vector<Rect64> rects) {
    ts::registerTestOperations();
    ts::registerProbeOperation();
    return ts::makeSyntheticScene({{width, height, 0, 0}}, {slideio::DataType::DT_Byte},
                                  [rects](int, int64_t x, int64_t y) {
                                      for (const Rect64& r : rects) {
                                          if (r.contains(Point64{x, y})) {
                                              return 255.0;
                                          }
                                      }
                                      return 0.0;
                                  });
}

// Horizontal bars three pixels high, 150 pixels long with 20-pixel gaps, every
// 11 rows, each row of bars shifted: most cross several 64-pixel tiles.
double bars(int, int64_t x, int64_t y) {
    return y % 11 >= 2 && y % 11 < 5 && (x + 37 * (y / 11)) % 170 < 150 ? 255.0 : 0.0;
}

// The objects of an unrestricted single-tile run of `spec` whose anchor's
// pixel `disc` sets: what a restricted run must store.
std::vector<ObjectKey> referenceInside(const std::shared_ptr<slideio::Scene>& scene, const PipelineSpec& spec,
                                       const ts::DiscMaskSource& disc) {
    const auto rect = scene->getRect();
    const int32_t side = std::max(std::get<2>(rect), std::get<3>(rect));
    ts::CollectingObjectSink all;
    Pipeline p(scene, config(side, 1), spec);
    p.bindLayer("objects", LayerId(3));
    p.runInto(all, nullptr, CancellationToken());
    std::vector<ObjectRecord> inside;
    for (const ObjectRecord& r : all.records()) {
        const Point64 anchor = r.geometry().anchor();
        if (disc.contains(static_cast<double>(anchor.x) + 0.5, static_cast<double>(anchor.y) + 0.5)) {
            inside.push_back(r);
        }
    }
    return sortedKeys(inside);
}

// The width of an object's outer ring, from its key; -1 for an empty key.
int64_t outerWidth(const ObjectKey& key) {
    const std::vector<std::pair<bool, Ring>>& rings = std::get<1>(key);
    if (rings.empty() || rings.front().second.empty()) {
        return -1;
    }
    const Ring& ring = rings.front().second;
    const auto byX = [](const std::pair<int64_t, int64_t>& a, const std::pair<int64_t, int64_t>& b) {
        return a.first < b.first;
    };
    return std::max_element(ring.begin(), ring.end(), byX)->first -
           std::min_element(ring.begin(), ring.end(), byX)->first;
}

// What a run restricted by a mask source stored and counted.
struct RestrictedRun {
    std::vector<ObjectKey> keys;
    int64_t onDemand = 0;
    int64_t skipped = 0;
};

RestrictedRun runRestricted(const std::shared_ptr<slideio::Scene>& scene, const PipelineSpec& spec,
                            ts::DiscMaskSource& disc, int32_t workers, RasterSink* raster = nullptr,
                            ProgressObserver* progress = nullptr) {
    ts::CollectingObjectSink stored;
    Pipeline p(scene, config(64, workers), spec);
    p.maskSource(&disc);
    p.bindLayer("objects", LayerId(3));
    if (raster != nullptr) {
        p.rasterSink(raster);
    }
    p.runInto(stored, progress, CancellationToken());
    const detail::RunCounters& counters = detail::PipelineAccess::impl(p).counters;
    return RestrictedRun{sortedKeys(stored.records()), counters.tilesEvaluatedOnDemand.load(),
                         counters.tilesSkipped.load()};
}

// The tiles of `grid` holding a pixel whose centre `disc` contains.
int64_t tilesHoldingMask(const TileGrid& grid, const ts::DiscMaskSource& disc) {
    int64_t count = 0;
    for (int64_t t = 0; t < grid.tileCount(); ++t) {
        const Rect64 r = grid.tileRect(t);
        bool any = false;
        for (int64_t y = r.y; y < r.y + r.height && !any; ++y) {
            for (int64_t x = r.x; x < r.x + r.width && !any; ++x) {
                any = disc.contains(static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5);
            }
        }
        count += any ? 1 : 0;
    }
    return count;
}

// Counts the raster tiles written.
class CountingRasterSink : public RasterSink {
public:
    void writeTile(const BlockView&) override { ++written_; }
    void flush() override {}
    int64_t tilesWritten() const override { return written_; }

private:
    std::atomic<int64_t> written_{0};
};

// Records every progress call.
class ProgressLog : public ProgressObserver {
public:
    void onProgress(const char*, double, int64_t completed, int64_t total) override {
        std::lock_guard<std::mutex> lock(mutex_);
        calls_.emplace_back(completed, total);
    }
    std::vector<std::pair<int64_t, int64_t>> calls() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return calls_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::pair<int64_t, int64_t>> calls_;
};

// A disc that sets pixels in tile 0 of 64-pixel tiles only.
constexpr double kDiscX = 20.0;
constexpr double kDiscY = 25.0;
constexpr double kDiscRadius = 30.0;

} // namespace

// An object whose anchor the mask admits, reaching into a tile the mask
// excludes, is stored whole: the excluded tile is evaluated for it.
TEST(SeamRun, RestrictedObjectCrossingIntoASkippedTileIsWhole) {
    auto scene = rectScene(256, 256, {Rect64{44, 20, 41, 10}});
    ts::DiscMaskSource disc(kDiscX, kDiscY, kDiscRadius);
    ASSERT_FALSE(disc.contains(64.5, 25.5)) << "tile 1 should hold no mask pixel";
    const std::vector<ObjectKey> reference = referenceInside(scene, components(false), disc);
    ASSERT_EQ(reference.size(), 1u);
    ASSERT_EQ(outerWidth(reference.front()), 41);

    const RestrictedRun run = runRestricted(scene, components(true), disc, 4);
    ASSERT_EQ(run.keys.size(), 1u);
    EXPECT_EQ(outerWidth(run.keys.front()), 41) << "the object was truncated at the skipped tile";
    EXPECT_TRUE(run.keys == reference) << "the object was not stored whole";
    EXPECT_EQ(run.onDemand, 1);
    EXPECT_EQ(run.skipped, 15) << "a tile evaluated on demand stays counted as skipped";
}

// An object running from inside the mask across three excluded tiles pulls in
// each of them, one after another, and no other tile.
TEST(SeamRun, RestrictedObjectSpanningSeveralSkippedTilesIsWhole) {
    auto scene = rectScene(320, 256, {Rect64{40, 20, 200, 10}});
    ts::DiscMaskSource disc(kDiscX, kDiscY, kDiscRadius);
    const std::vector<ObjectKey> reference = referenceInside(scene, components(false), disc);
    ASSERT_EQ(reference.size(), 1u);
    ASSERT_EQ(outerWidth(reference.front()), 200);

    const RestrictedRun run = runRestricted(scene, components(true), disc, 4);
    ASSERT_EQ(run.keys.size(), 1u);
    EXPECT_EQ(outerWidth(run.keys.front()), 200) << "the object was truncated at the skipped tiles";
    EXPECT_TRUE(run.keys == reference) << "the object was not stored whole";
    EXPECT_EQ(run.onDemand, 3);
}

// A restricted run with many objects crossing out of the mask stores the same
// objects, geometry and measurements, whatever the worker count.
TEST(SeamRun, RestrictedRunIsWorkerCountInvariant) {
    ts::registerTestOperations();
    auto scene = ts::makeSyntheticScene({{kSide, kSide, 0, 0}}, {slideio::DataType::DT_Byte}, bars);
    ts::DiscMaskSource disc(256.0, 256.0, 120.0);
    const std::vector<ObjectKey> reference = referenceInside(scene, components(false), disc);
    ASSERT_GT(reference.size(), 15u);

    for (const int32_t workers : {1, 4, 8}) {
        const RestrictedRun run = runRestricted(scene, components(true), disc, workers);
        EXPECT_GT(run.onDemand, 0) << workers << " workers: the fixture should cross into skipped tiles";
        EXPECT_TRUE(run.keys == reference) << workers << " workers: objects differ from the reference";
    }
}

// Only objects reaching out of the tiles the pass evaluated demand tiles: the
// objects found in a demanded tile, lying wholly in skipped tiles, pull in no
// more. Dense 20-pixel squares every 30 pixels; the disc sets pixels in the
// four tiles of columns and rows 3 and 4 only. Squares cross the seams at 192
// (into column and row 2); at 320 squares end on the seam, which still makes
// columns and row 5 expected. So the evaluated tiles' squares reach the eleven
// tiles around them, but for the corner (5,5), which a 4-connected square
// ending at the corner does not reach: (2,2), (2,3), (2,4), (3,2), (4,2) and
// (3,5), (4,5), (5,3), (5,4), and through the squares cut at both 192 and 320,
// (2,5) and (5,2). Without the rule the demands spread to 32 tiles.
TEST(SeamRun, OnlyObjectsReachingFromEvaluatedTilesDemandTiles) {
    ts::registerTestOperations();
    auto scene = ts::makeSyntheticScene({{kSide, kSide, 0, 0}}, {slideio::DataType::DT_Byte},
                                        [](int, int64_t x, int64_t y) {
                                            return x % 30 < 20 && y % 30 < 20 ? 255.0 : 0.0;
                                        });
    ts::DiscMaskSource disc(256.0, 256.0, 60.0);
    const std::vector<ObjectKey> reference = referenceInside(scene, components(false), disc);
    ASSERT_FALSE(reference.empty());

    for (const int32_t workers : {1, 4, 8}) {
        const RestrictedRun run = runRestricted(scene, components(true), disc, workers);
        EXPECT_EQ(run.skipped, 60) << workers << " workers";
        EXPECT_EQ(run.onDemand, 11) << workers << " workers";
        EXPECT_TRUE(run.keys == reference) << workers << " workers: objects differ from the reference";
    }
}

// A tile evaluated on demand writes no raster tile and reports no progress:
// the raster holds the tiles holding a mask pixel, as when skipped tiles were
// not evaluated, and progress ends at its total.
TEST(SeamRun, DemandedTilesWriteNoRasterAndReportNoProgress) {
    auto scene = rectScene(256, 256, {Rect64{40, 20, 200, 10}});
    ts::DiscMaskSource disc(kDiscX, kDiscY, kDiscRadius);
    CountingRasterSink raster;
    ProgressLog progress;
    const RestrictedRun run = runRestricted(scene, components(true), disc, 4, &raster, &progress);
    ASSERT_EQ(run.onDemand, 3);

    Pipeline p(scene, config(64, 1), components(true));
    p.bindLayer("objects", LayerId(3));
    p.validate();
    const TileGrid& grid = detail::PipelineAccess::plan(p).grid;
    EXPECT_EQ(raster.tilesWritten(), tilesHoldingMask(grid, disc));
    const std::vector<std::pair<int64_t, int64_t>> calls = progress.calls();
    ASSERT_EQ(static_cast<int64_t>(calls.size()), grid.tileCount());
    EXPECT_EQ(calls.back(), std::make_pair(grid.tileCount(), grid.tileCount()));
    for (const auto& call : calls) {
        EXPECT_LE(call.first, call.second);
    }
}

// A cancellation inside a tile evaluated on demand stops the run with a
// CancelledError, and the object waiting for that tile is not stored.
TEST(SeamRun, CancellationDuringDemandedTiles) {
    auto scene = rectScene(256, 256, {Rect64{44, 20, 41, 10}});
    ts::DiscMaskSource disc(kDiscX, kDiscY, kDiscRadius);
    DisarmGuard guard;
    ts::ProbeOperation::setFailuresArmed(true);
    PipelineSpec spec;
    OperationParameters probe;
    probe.setInt("copy", 1);
    probe.setInt("failX", 70);
    probe.setInt("failY", 25);
    probe.setInt("failKind", 2);
    spec.operation("test.probe", probe);
    spec.operation("test.threshold", OperationParameters());
    spec.operation("test.components", seams(true));

    ts::CollectingObjectSink stored;
    Pipeline p(scene, config(64, 4), spec);
    p.maskSource(&disc);
    p.bindLayer("objects", LayerId(3));
    EXPECT_THROW(p.runInto(stored, nullptr, CancellationToken()), CancelledError);
    EXPECT_EQ(stored.emittedCount(), 0) << "an object waiting for the cancelled tile was stored";
}

// Without a restriction nothing is skipped, so nothing is evaluated on demand,
// and the objects are those of the single-tile run.
TEST(SeamRun, UnrestrictedRunIsUnchanged) {
    auto scene = blobScene();
    ts::CollectingObjectSink whole;
    Pipeline single(scene, config(static_cast<int32_t>(kSide), 1), components(false));
    single.bindLayer("objects", LayerId(3));
    single.runInto(whole, nullptr, CancellationToken());

    ts::CollectingObjectSink stored;
    Pipeline p(scene, config(64, 4), components(true));
    p.bindLayer("objects", LayerId(3));
    p.runInto(stored, nullptr, CancellationToken());
    EXPECT_TRUE(sortedKeys(stored.records()) == sortedKeys(whole.records()));
    const detail::RunCounters& counters = detail::PipelineAccess::impl(p).counters;
    EXPECT_EQ(counters.tilesSkipped.load(), 0);
    EXPECT_EQ(counters.tilesEvaluatedOnDemand.load(), 0);
}
