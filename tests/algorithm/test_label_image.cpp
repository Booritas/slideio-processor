// Requirements: UR-SEG-3, UR-SEG-4, UR-SEG-5, UR-EXT-3, UR-EXT-4, UR-EXE-2, UR-QA-3
#include <slideio/processor/algorithm/segment/label_image.hpp>

#include <algorithm/support/disks_operation.hpp>
#include <algorithm/support/harness.hpp>
#include <pipeline/test_support/synthetic_scene.hpp>
#include <pipeline/test_support/test_operations.hpp>
#include <workspace/support/test_slide.hpp>

#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/control.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/measurement.hpp>
#include <slideio/processor/core/object_query.hpp>
#include <slideio/processor/core/object_record.hpp>
#include <slideio/processor/pipeline/operation_catalog.hpp>
#include <slideio/processor/pipeline/pipeline.hpp>
#include <slideio/processor/pipeline/pipeline_spec.hpp>
#include <slideio/processor/storage/object_store.hpp>
#include <slideio/processor/storage/store.hpp>
#include <slideio/processor/workspace/layer_lifecycle.hpp>
#include <slideio/processor/workspace/stage_definition.hpp>
#include <slideio/processor/workspace/stage_registry.hpp>
#include <slideio/processor/workspace/stage_runner.hpp>
#include <slideio/processor/workspace/workspace.hpp>

#include <pipeline/detail/operation_context_state.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace slideio::processor;
namespace ts = slideio::processor::testing_support;
namespace fs = std::filesystem;

namespace {

constexpr int64_t kWidth = 2048;
constexpr int64_t kHeight = 1536;

// The disks the runs draw, in a 2048 x 1536 level. Halo 32 holds every one
// whole below and beside its anchor (diameters up to 30).
enum DiskIndex { Corner, SharedA, SharedB, Ring, TouchLeft, TouchRight, Edge, Plain, DiskCount };

ts::DisksSpec standardSpec(int32_t halo = 32) {
    ts::DisksSpec spec;
    spec.halo = halo;
    spec.disks = {
        {1024.0, 1024.0, 15.0, 0.0}, // across the corner of four tiles of 512 and of 1024
        {300.0, 300.0, 12.0, 0.0},   // these two share one value in every tile
        {340.0, 300.0, 12.0, 0.0},
        {700.0, 300.0, 15.0, 6.0},   // a ring: one object with a hole
        {500.0, 700.0, 14.0, 0.0},   // two overlapping disks, the second drawn over the
        {520.0, 700.0, 14.0, 0.0},   // first, across the seam x = 512: a shared edge
        {2040.0, 1530.0, 15.0, 0.0}, // cut by the level's right and bottom edges
        {1500.0, 400.0, 15.0, 0.0},
    };
    spec.sharedFirst = SharedA;
    spec.sharedSecond = SharedB;
    return spec;
}

PipelineSpec pipelineOf(const ts::DisksSpec& spec) {
    PipelineSpec pipeline;
    pipeline.operation("test.disks", ts::disksParameters(spec));
    return pipeline;
}

std::shared_ptr<slideio::Scene> sceneOf(int64_t width = kWidth, int64_t height = kHeight) {
    ts::registerDisksOperation();
    return ts::makeSyntheticScene({{width, height, 0, 0}}, {slideio::DataType::DT_Byte},
                                  [](int, int64_t, int64_t) { return 0.0; });
}

Configuration configOf(int32_t tileSize, int32_t workers) {
    return ConfigurationBuilder().tileSize(tileSize, tileSize).tileSizeFromSlide(false).workerCount(workers).build();
}

struct RunResult {
    std::vector<ObjectRecord> objects; // sorted by id
    int64_t truncated = 0;
};

RunResult runObjects(const std::shared_ptr<slideio::Scene>& scene, const ts::DisksSpec& spec, int32_t tileSize,
                     int32_t workers) {
    Pipeline pipeline(scene, configOf(tileSize, workers), pipelineOf(spec));
    pipeline.bindLayer("objects", ts::objectsLayer());
    ts::CollectingObjectSink sink;
    pipeline.runInto(sink, nullptr, CancellationToken());
    RunResult result;
    result.objects = sink.records();
    std::sort(result.objects.begin(), result.objects.end(),
              [](const ObjectRecord& a, const ObjectRecord& b) { return a.id() < b.id(); });
    result.truncated = pipeline.truncatedObjectCount();
    return result;
}

double measurementOf(const ObjectRecord& record, const char* name) {
    Measurement m;
    EXPECT_TRUE(record.measurements().find(name, m)) << name;
    return m.value();
}

// The pixels of the level drawn with disk @p index's value in a single-tile run:
// inside the disk, inside the level, and not covered by a later disk.
int64_t pixelsOf(const ts::DisksSpec& spec, size_t index, int64_t width = kWidth, int64_t height = kHeight) {
    const ts::Disk& d = spec.disks[index];
    int64_t count = 0;
    const int64_t x0 = std::max<int64_t>(0, static_cast<int64_t>(d.cx - d.radius) - 1);
    const int64_t x1 = std::min<int64_t>(width, static_cast<int64_t>(d.cx + d.radius) + 2);
    const int64_t y0 = std::max<int64_t>(0, static_cast<int64_t>(d.cy - d.radius) - 1);
    const int64_t y1 = std::min<int64_t>(height, static_cast<int64_t>(d.cy + d.radius) + 2);
    for (int64_t y = y0; y < y1; ++y) {
        for (int64_t x = x0; x < x1; ++x) {
            const double px = static_cast<double>(x) + 0.5 - d.cx;
            const double py = static_cast<double>(y) + 0.5 - d.cy;
            const double d2 = px * px + py * py;
            if (d2 > d.radius * d.radius || (d.innerRadius > 0.0 && d2 < d.innerRadius * d.innerRadius)) {
                continue;
            }
            bool covered = false;
            for (size_t later = index + 1; later < spec.disks.size(); ++later) {
                const ts::Disk& e = spec.disks[later];
                const double qx = static_cast<double>(x) + 0.5 - e.cx;
                const double qy = static_cast<double>(y) + 0.5 - e.cy;
                const double e2 = qx * qx + qy * qy;
                covered = covered ||
                          (e2 <= e.radius * e.radius && (e.innerRadius <= 0.0 || e2 >= e.innerRadius * e.innerRadius));
            }
            count += covered ? 0 : 1;
        }
    }
    return count;
}

// The objects whose bounding box lies within @p area.
std::vector<const ObjectRecord*> objectsWithin(const std::vector<ObjectRecord>& objects, const Rect64& area) {
    std::vector<const ObjectRecord*> found;
    for (const ObjectRecord& r : objects) {
        if (area.intersected(r.boundingBox()) == r.boundingBox()) {
            found.push_back(&r);
        }
    }
    return found;
}

// The box around disk @p d, two pixels wider on every side.
Rect64 around(const ts::Disk& d) {
    const int64_t r = static_cast<int64_t>(d.radius) + 2;
    return Rect64{static_cast<int64_t>(d.cx) - r, static_cast<int64_t>(d.cy) - r, 2 * r, 2 * r};
}

std::vector<Point64> ringOf(const Polygon& polygon, int32_t ring) {
    std::vector<Point64> points(static_cast<size_t>(polygon.ringPointCount(ring)));
    polygon.ringPoints(ring, points.data(), static_cast<int64_t>(points.size()));
    return points;
}

bool sameGeometry(const Polygon& a, const Polygon& b) {
    if (a.ringCount() != b.ringCount()) {
        return false;
    }
    for (int32_t r = 0; r < a.ringCount(); ++r) {
        if (a.ringIsHole(r) != b.ringIsHole(r) || ringOf(a, r) != ringOf(b, r)) {
            return false;
        }
    }
    return true;
}

} // namespace

TEST(LabelImage, ObjectsAcrossTileSeamsAreWhole) {
    const auto scene = sceneOf();
    const ts::DisksSpec spec = standardSpec();
    const Rect64 box = around(spec.disks[Corner]);
    const RunResult single = runObjects(scene, spec, 4096, 1);
    const std::vector<const ObjectRecord*> reference = objectsWithin(single.objects, box);
    ASSERT_EQ(reference.size(), 1u);
    EXPECT_EQ(measurementOf(*reference[0], "area"), static_cast<double>(pixelsOf(spec, Corner)));
    for (const int32_t tile : {512, 1024}) {
        const RunResult tiled = runObjects(scene, spec, tile, 2);
        const std::vector<const ObjectRecord*> found = objectsWithin(tiled.objects, box);
        ASSERT_EQ(found.size(), 1u) << "tile " << tile;
        EXPECT_EQ(measurementOf(*found[0], "area"), measurementOf(*reference[0], "area")) << "tile " << tile;
        EXPECT_TRUE(sameGeometry(found[0]->geometry(), reference[0]->geometry())) << "tile " << tile;
        EXPECT_EQ(tiled.truncated, 0) << "tile " << tile;
    }
}

TEST(LabelImage, SameValueDifferentObjectsStaySeparate) {
    const auto scene = sceneOf();
    const ts::DisksSpec spec = standardSpec();
    // The precondition: within their tile, the two disks carry one value.
    const Rect64 tile{0, 0, 512, 512};
    const ts::Disk& a = spec.disks[SharedA];
    const ts::Disk& b = spec.disks[SharedB];
    ASSERT_EQ(ts::diskLabelAt(spec, tile, 1.0, static_cast<int64_t>(a.cx), static_cast<int64_t>(a.cy)),
              ts::diskLabelAt(spec, tile, 1.0, static_cast<int64_t>(b.cx), static_cast<int64_t>(b.cy)));
    const RunResult run = runObjects(scene, spec, 512, 1);
    const std::vector<const ObjectRecord*> first = objectsWithin(run.objects, around(a));
    const std::vector<const ObjectRecord*> second = objectsWithin(run.objects, around(b));
    ASSERT_EQ(first.size(), 1u);
    ASSERT_EQ(second.size(), 1u);
    EXPECT_FALSE(first[0]->id() == second[0]->id());
    EXPECT_EQ(measurementOf(*first[0], "area"), static_cast<double>(pixelsOf(spec, SharedA)));
    EXPECT_EQ(measurementOf(*second[0], "area"), static_cast<double>(pixelsOf(spec, SharedB)));
}

TEST(LabelImage, TilingInvariance) {
    const auto scene = sceneOf();
    const ts::DisksSpec spec = standardSpec();
    const RunResult reference = runObjects(scene, spec, 512, 1);
    ASSERT_EQ(reference.objects.size(), static_cast<size_t>(DiskCount));
    EXPECT_EQ(reference.truncated, 0);
    for (const int32_t tile : {512, 1024}) {
        for (const int32_t workers : {1, 4}) {
            const std::string label = "tile " + std::to_string(tile) + ", workers " + std::to_string(workers);
            const RunResult run = runObjects(scene, spec, tile, workers);
            EXPECT_EQ(run.truncated, 0) << label;
            ASSERT_EQ(run.objects.size(), reference.objects.size()) << label;
            for (size_t i = 0; i < run.objects.size(); ++i) {
                const ObjectRecord& got = run.objects[i];
                const ObjectRecord& want = reference.objects[i];
                EXPECT_TRUE(got.id() == want.id()) << label << ", object " << i;
                EXPECT_TRUE(sameGeometry(got.geometry(), want.geometry())) << label << ", object " << i;
                EXPECT_EQ(measurementOf(got, "area"), measurementOf(want, "area")) << label << ", object " << i;
                EXPECT_EQ(measurementOf(got, "perimeter"), measurementOf(want, "perimeter"))
                    << label << ", object " << i;
            }
        }
    }
}

TEST(LabelImage, HaloTooSmallIsCountedAndDropped) {
    ts::registerDisksOperation();
    ts::TempDirectory dir;
    fs::create_directories(dir.path());
    const fs::path slide = ts::writeTestSlide(dir.path() / "slide.tif", 600, 600);
    const std::string workspacePath = (dir.path() / "ws").u8string();
    WorkspacePtr workspace =
        Workspace::create(workspacePath.c_str(), slide.u8string().c_str(), 0,
                          ConfigurationBuilder().tileSize(200, 200).tileSizeFromSlide(false).workerCount(2).build());
    const LayerId nuclei = workspace->layerLifecycle().declareSemantic("nuclei", workspace->layers().pixelLayer());
    // Halo 4 and disks of radius 20: one inside a tile, whole; one across the seam
    // x = 200, whose owning tile sees only 4 pixels past the seam.
    ts::DisksSpec spec;
    spec.halo = 4;
    spec.disks = {{100.0, 100.0, 20.0, 0.0}, {200.0, 300.0, 20.0, 0.0}};
    StageDefinition def("disks");
    def.writesLayer(nuclei).pipeline(pipelineOf(spec));
    const StageId id = workspace->stages().declare(def);
    StageRunner runner(*workspace, id);
    runner.run();
    EXPECT_GT(runner.truncatedObjects(), 0);
    const double whole = static_cast<double>(pixelsOf(spec, 0, 600, 600));
    int64_t stored = 0;
    const CursorPtr cursor = workspace->store()->objects().query(ObjectQuery().layer(nuclei));
    while (cursor->next()) {
        ++stored;
        EXPECT_GE(measurementOf(cursor->current(), "area"), whole) << "a stored object is smaller than a disk";
    }
    EXPECT_EQ(stored, 1);
    workspace->close();
}

TEST(LabelImage, ObjectsAtTheLevelEdgeAreKept) {
    const auto scene = sceneOf();
    for (const int32_t halo : {32, 0}) {
        ts::DisksSpec spec;
        spec.halo = halo;
        spec.disks = {standardSpec().disks[Edge]};
        for (const int32_t tile : {512, 1024}) {
            const std::string label = "halo " + std::to_string(halo) + ", tile " + std::to_string(tile);
            const RunResult run = runObjects(scene, spec, tile, 2);
            EXPECT_EQ(run.truncated, 0) << label;
            ASSERT_EQ(run.objects.size(), 1u) << label;
            const Rect64 box = run.objects[0].boundingBox();
            EXPECT_EQ(box.right(), kWidth) << label << ": cut at the level's right edge";
            EXPECT_EQ(box.bottom(), kHeight) << label << ": cut at the level's bottom edge";
            EXPECT_EQ(measurementOf(run.objects[0], "area"), static_cast<double>(pixelsOf(spec, 0))) << label;
        }
    }
}

TEST(LabelImage, HolesAndSharedEdges) {
    const auto scene = sceneOf();
    const ts::DisksSpec spec = standardSpec();
    const RunResult run = runObjects(scene, spec, 512, 2);

    const std::vector<const ObjectRecord*> ring = objectsWithin(run.objects, around(spec.disks[Ring]));
    ASSERT_EQ(ring.size(), 1u);
    const Polygon& rings = ring[0]->geometry();
    ASSERT_EQ(rings.ringCount(), 2);
    EXPECT_FALSE(rings.ringIsHole(0));
    EXPECT_TRUE(rings.ringIsHole(1));
    EXPECT_EQ(measurementOf(*ring[0], "area"), static_cast<double>(pixelsOf(spec, Ring)));

    const Rect64 pair = around(spec.disks[TouchLeft]).united(around(spec.disks[TouchRight]));
    const std::vector<const ObjectRecord*> touching = objectsWithin(run.objects, pair);
    ASSERT_EQ(touching.size(), 2u);
    EXPECT_EQ(measurementOf(*touching[0], "area") + measurementOf(*touching[1], "area"),
              static_cast<double>(pixelsOf(spec, TouchLeft) + pixelsOf(spec, TouchRight)));
    // The shared edge: the outer rings have vertices in common.
    const std::vector<Point64> left = ringOf(touching[0]->geometry(), 0);
    const std::vector<Point64> right = ringOf(touching[1]->geometry(), 0);
    int64_t common = 0;
    for (const Point64& p : left) {
        common += std::find(right.begin(), right.end(), p) != right.end() ? 1 : 0;
    }
    EXPECT_GE(common, 2);
}

TEST(LabelImage, OutputPlaneIsTheTileCrop) {
    const auto scene = sceneOf();
    const ts::DisksSpec spec = standardSpec();
    constexpr int32_t kTile = 512;
    const ts::Image image = ts::runEager(scene, pipelineOf(spec), kTile, 2, true);
    ASSERT_EQ(image.types.size(), 1u);
    ASSERT_EQ(image.types[0], DataType::Int32);
    ASSERT_EQ(image.width, kWidth);
    ASSERT_EQ(image.height, kHeight);
    int64_t mismatches = 0;
    int64_t labelled = 0;
    for (int64_t y = 0; y < kHeight; ++y) {
        for (int64_t x = 0; x < kWidth; ++x) {
            const Rect64 tile{x / kTile * kTile, y / kTile * kTile, kTile, kTile};
            const int32_t want = ts::diskLabelAt(spec, tile, 1.0, x, y);
            labelled += want != 0 ? 1 : 0;
            mismatches += image.at(0, x, y) != static_cast<double>(want) ? 1 : 0;
        }
    }
    EXPECT_GT(labelled, 0);
    EXPECT_EQ(mismatches, 0);
}

TEST(LabelImage, HaloBeyondTheBudgetIsRefused) {
    const auto scene = sceneOf();
    const Configuration config = ConfigurationBuilder()
                                     .tileSize(512, 512)
                                     .tileSizeFromSlide(false)
                                     .workerCount(1)
                                     .memoryCeilingBytes(int64_t(256) << 20)
                                     .build();
    // Halo 32 fits the ceiling.
    Pipeline fits(scene, config, pipelineOf(standardSpec(32)));
    fits.bindLayer("objects", ts::objectsLayer());
    EXPECT_NO_THROW(fits.validate());
    // Halo 2048: a halo rect of 4608 x 4608 level pixels does not.
    const int64_t calls = ts::disksPredictCount();
    Pipeline refused(scene, config, pipelineOf(standardSpec(2048)));
    refused.bindLayer("objects", ts::objectsLayer());
    EXPECT_THROW(refused.validate(), InvalidArgumentError);
    EXPECT_EQ(ts::disksPredictCount(), calls) << "no tile ran";
}

namespace {

// Records the level rect each tile's context reports.
class LevelRectProbe : public ts::DisksOperation {
public:
    explicit LevelRectProbe(const OperationParameters& parameters) : DisksOperation(parameters) {}
    const char* name() const override { return "test.level_rect_probe"; }
    void predictLabels(const OperationContext& context, BlockView& labels) override {
        {
            std::lock_guard<std::mutex> lock(mutex());
            seen().push_back(context.levelRectAtLevel());
        }
        DisksOperation::predictLabels(context, labels);
    }
    static std::mutex& mutex() {
        static std::mutex m;
        return m;
    }
    static std::vector<Rect64>& seen() {
        static std::vector<Rect64> s;
        return s;
    }
};

} // namespace

TEST(LabelImage, ContextReportsTheLevelRect) {
    OperationCatalog& catalog = OperationCatalog::instance();
    if (!catalog.isRegistered("test.level_rect_probe")) {
        catalog.registerFactory(std::make_shared<ts::PrototypeFactory>(
            [](const OperationParameters& p) -> OperationPtr { return std::make_shared<LevelRectProbe>(p); },
            "Analytic: records the level rect", "O(1)"));
    }
    const auto scene = sceneOf(700, 500);
    LevelRectProbe::seen().clear();
    PipelineSpec spec;
    spec.operation("test.level_rect_probe", ts::disksParameters(ts::DisksSpec()));
    Pipeline pipeline(scene, configOf(256, 2), spec);
    pipeline.bindLayer("objects", ts::objectsLayer());
    pipeline.run(nullptr, CancellationToken());
    // A 3 x 2 grid of 256-pixel tiles, each told the whole level.
    ASSERT_EQ(LevelRectProbe::seen().size(), 6u);
    for (const Rect64& rect : LevelRectProbe::seen()) {
        EXPECT_EQ(rect, (Rect64{0, 0, 700, 500}));
    }
    // A context built without a run does not know the level.
    EXPECT_TRUE(detail::makeOperationContext(detail::OperationContextState())->levelRectAtLevel().isEmpty());
}
