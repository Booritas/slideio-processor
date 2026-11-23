// Requirements: UR-API-2, UR-STG-1, UR-STG-4, UR-EXE-4, UR-QA-3, UR-RES-7, UR-RES-8, UR-RES-9, UR-RES-11
#include "algorithm/support/disks_operation.hpp"
#include "workspace/support/test_slide.hpp"

#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/control.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/object_query.hpp>
#include <slideio/processor/core/object_record.hpp>
#include <slideio/processor/pipeline/operation_parameters.hpp>
#include <slideio/processor/pipeline/pipeline_spec.hpp>
#include <slideio/processor/run/run.hpp>
#include <slideio/processor/storage/layer_registry.hpp>
#include <slideio/processor/storage/object_store.hpp>
#include <slideio/processor/storage/raster_store.hpp>
#include <slideio/processor/storage/store.hpp>
#include <slideio/processor/workspace/stage_definition.hpp>
#include <slideio/processor/workspace/stage_registry.hpp>
#include <slideio/processor/workspace/stage_runner.hpp>
#include <slideio/processor/workspace/workspace.hpp>

#include <slideio/core/levelinfo.hpp>
#include <slideio/slideio/scene.hpp>
#include <slideio/slideio/slide.hpp>
#include <slideio/slideio/slideio.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <regex>
#include <string>
#include <tuple>
#include <vector>

using namespace slideio::processor;
namespace fs = std::filesystem;
namespace ts = slideio::processor::testing_support;

namespace {

std::string pipelineJson(const PipelineSpec& spec) {
    const int32_t length = spec.toJson(nullptr, 0);
    std::string text(static_cast<size_t>(length) + 1, '\0');
    spec.toJson(&text[0], length + 1);
    text.resize(static_cast<size_t>(length));
    return text;
}

// A stage record (stageRecordVersion 1); the lists are JSON array contents, the
// restriction and raster JSON values.
std::string record(const std::string& name, const std::string& writes, const std::string& restriction,
                   const std::string& raster, const PipelineSpec& pipeline) {
    return "{\"stageRecordVersion\":1,\"stage\":\"" + name + "\",\"readsLayers\":[],\"writesLayers\":[" + writes +
           "],\"annotatesLayers\":[],\"restrictToLayer\":" + restriction +
           ",\"readsLayersAsRaster\":[],\"writesRaster\":" + raster +
           ",\"readsRasters\":[],\"dependsOn\":[],\"pipeline\":" + pipelineJson(pipeline) + "}";
}

// Grey, dark pixels foreground, connected components: the nine discs of a blob slide.
PipelineSpec blobChain(double threshold) {
    PipelineSpec spec;
    spec.operation("pixel.colour_convert", OperationParameters().setString("from", "Rgb").setString("to", "Grey"));
    spec.operation("pixel.threshold_fixed",
                   OperationParameters().setDouble("value", threshold).setString("direction", "Below"));
    spec.operation("segment.connected_components", OperationParameters());
    return spec;
}

PipelineSpec probChain() {
    PipelineSpec spec;
    spec.operation("pixel.colour_convert", OperationParameters().setString("from", "Rgb").setString("to", "Grey"));
    spec.operation("pixel.gaussian_blur", OperationParameters());
    return spec;
}

const char* kCsv = "nuclei.csv";
const char* kGeoJson = "nuclei.geojson";
const char* kMask = "nuclei-mask.ome.tif";
const char* kProb = "prob.ome.tif";

// The three-stage file: tissue (a built-in segmenter), nuclei restricted to tissue, and
// prob, independent, writing a raster; the four export kinds.
std::string threeStageJson(double tissueThreshold = 128.0) {
    const std::string stages = record("tissue", "\"tissue\"", "null", "null", blobChain(tissueThreshold)) + "," +
                               record("nuclei", "\"nuclei\"", "\"tissue\"", "null", blobChain(128.0)) + "," +
                               record("prob", "", "null", "\"prob\"", probChain());
    return std::string("{\"formatVersion\":1,\"requires\":[],"
                       "\"layers\":[{\"name\":\"tissue\"},{\"name\":\"nuclei\",\"parent\":\"tissue\"}],"
                       "\"rasters\":[{\"name\":\"prob\",\"type\":\"Float32\",\"planes\":1}],"
                       "\"stages\":[") +
           stages +
           "],\"exports\":["
           "{\"kind\":\"csv\",\"file\":\"" + kCsv + "\",\"layer\":\"nuclei\"},"
           "{\"kind\":\"geojson\",\"file\":\"" + kGeoJson + "\",\"layer\":\"nuclei\"},"
           "{\"kind\":\"labelMask\",\"file\":\"" + kMask + "\",\"layer\":\"nuclei\"},"
           "{\"kind\":\"rasterTiff\",\"file\":\"" + kProb + "\",\"raster\":\"prob\"}]}";
}

PipelineFile threeStageFile(double tissueThreshold = 128.0) {
    return PipelineFile::fromJson(threeStageJson(tissueThreshold).c_str());
}

// A blob slide in a temporary directory, with a workspace and an export directory beside it.
struct Fixture {
    Fixture() {
        fs::create_directories(dir.path());
        slide = ts::writeBlobSlide(dir.path() / "slide.tif", 600, 600).u8string();
        workspace = (dir.path() / "ws").u8string();
        out = (dir.path() / "out").u8string();
    }
    SlideRunResult run(const PipelineFile& file, const std::string& ws, const std::string& exports,
                       const Configuration& config = Configuration::defaults()) {
        return SlideRun(file, slide.c_str(), 0, ws.c_str(), exports.c_str(), config).run();
    }
    SlideRunResult run(const PipelineFile& file) { return run(file, workspace, out); }

    ts::TempDirectory dir;
    std::string slide;
    std::string workspace;
    std::string out;
};

std::string readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

int32_t stageIndex(const SlideRunResult& result, const char* name) {
    for (int32_t i = 0; i < result.stageCount(); ++i) {
        if (std::string(result.stageName(i)) == name) return i;
    }
    ADD_FAILURE() << "no stage " << name;
    return 0;
}

bool ran(const SlideRunResult& result, const char* name) { return result.stageRan(stageIndex(result, name)); }

// The objects of a layer: (anchor, bounding box), sorted.
using ObjectKey = std::tuple<int64_t, int64_t, int64_t, int64_t, int64_t>;
std::vector<ObjectKey> objectsOf(Workspace& ws, const char* layerName) {
    LayerId layer;
    EXPECT_TRUE(ws.layers().find(layerName, layer)) << layerName;
    std::vector<ObjectKey> keys;
    const CursorPtr cursor = ws.store()->objects().query(ObjectQuery().layer(layer));
    while (cursor->next()) {
        const Rect64 box = cursor->current().boundingBox();
        keys.emplace_back(cursor->current().id().anchorPacked(), box.x, box.y, box.width, box.height);
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

// The text with the store's creation time, the one provenance value two fresh workspaces
// cannot share, blanked: the CSV's "# created_at_unix_ms: <n>" line and the GeoJSON
// provenance's "createdAtUnixMs":<n> member. Everything else must match byte for byte.
std::string withoutCreationTime(const std::string& text) {
    const std::string csv = std::regex_replace(text, std::regex("# created_at_unix_ms: [0-9]+"), "# created_at_unix_ms: 0");
    return std::regex_replace(csv, std::regex("\"createdAtUnixMs\": ?[0-9]+"), "\"createdAtUnixMs\":0");
}

// True for SQLite's WAL-mode sidecars of a store, which any open of it, a read-only one
// included, may create: housekeeping, not data.
bool isSqliteSidecar(const std::string& name) {
    const auto endsWith = [&name](const std::string& suffix) {
        return name.size() >= suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    return endsWith("-wal") || endsWith("-shm");
}

// Everything under a directory, by relative path: a file's whole contents, or "<dir>" for
// a directory. SQLite's -wal and -shm sidecars are left out when `withSidecars` is false.
std::map<std::string, std::string> contentsUnder(const fs::path& root, bool withSidecars) {
    std::map<std::string, std::string> contents;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        const std::string name = fs::relative(entry.path(), root).u8string();
        if (!withSidecars && isSqliteSidecar(name)) continue;
        contents[name] = entry.is_directory() ? std::string("<dir>") : readFile(entry.path());
    }
    return contents;
}

// Opens a workspace for writing and closes it: the last writer's close checkpoints and
// removes SQLite's sidecars, so the store is left cleanly closed.
void closeCleanly(const std::string& workspace) {
    Workspace::open(workspace.c_str(), Configuration::defaults())->close();
}

// Every stage's stored record, by name.
std::map<std::string, std::string> stageRecords(const std::string& workspace) {
    WorkspacePtr ws = Workspace::openReadOnly(workspace.c_str(), Configuration::defaults());
    std::map<std::string, std::string> records;
    for (int32_t i = 0; i < ws->stages().count(); ++i) {
        const StageId id = ws->stages().at(i);
        records[ws->stages().name(id)] = ws->stages().configuration(id);
    }
    ws->close();
    return records;
}

std::shared_ptr<slideio::Scene> reopen(const fs::path& file) {
    std::shared_ptr<slideio::Slide> slide = slideio::openSlide(file.u8string(), "OMETIFF");
    if (!slide || slide->getNumScenes() != 1) return nullptr;
    return slide->getScene(0);
}

// Cancels its source on the first progress report.
class CancelOnFirstReport : public ProgressObserver {
public:
    explicit CancelOnFirstReport(CancellationSource& source) : source_(source) {}
    void onProgress(const char*, double, int64_t, int64_t) override {
        reports_.fetch_add(1);
        source_.cancel();
    }
    int64_t reports() const { return reports_.load(); }

private:
    CancellationSource& source_;
    std::atomic<int64_t> reports_{0};
};

} // namespace

TEST(SlideRun, FirstRunRunsEverythingAndExports) {
    Fixture f;
    const SlideRunResult result = f.run(threeStageFile());
    ASSERT_EQ(result.stageCount(), 3);
    EXPECT_STREQ(result.stageName(0), "tissue");
    EXPECT_STREQ(result.stageName(1), "nuclei");
    EXPECT_STREQ(result.stageName(2), "prob");
    for (int32_t i = 0; i < 3; ++i) EXPECT_TRUE(result.stageRan(i)) << result.stageName(i);
    EXPECT_EQ(result.stageObjectsWritten(0), 9);
    EXPECT_EQ(result.stageObjectsWritten(1), 9);
    EXPECT_EQ(result.stageObjectsWritten(2), 0);

    // The same stages run directly with StageRunner in another workspace.
    WorkspacePtr direct = Workspace::create((f.dir.path() / "direct").u8string().c_str(), f.slide.c_str(), 0,
                                            Configuration::defaults());
    const LayerId tissue = direct->layerLifecycle().declareSemantic("tissue", direct->layers().pixelLayer());
    const LayerId nuclei = direct->layerLifecycle().declareSemantic("nuclei", tissue);
    const RasterId prob = direct->store()->rasters().declare(RasterDeclaration("prob", DataType::Float32, 1));
    StageDefinition tissueStage("tissue");
    tissueStage.writesLayer(tissue).pipeline(blobChain(128.0));
    StageDefinition nucleiStage("nuclei");
    nucleiStage.writesLayer(nuclei).restrictToLayer(tissue).pipeline(blobChain(128.0));
    StageDefinition probStage("prob");
    probStage.writesRaster(prob).pipeline(probChain());
    for (const StageDefinition* def : {&tissueStage, &nucleiStage, &probStage}) {
        StageRunner(*direct, direct->stages().declare(*def)).run();
    }

    WorkspacePtr ws = Workspace::openReadOnly(f.workspace.c_str(), Configuration::defaults());
    EXPECT_EQ(objectsOf(*ws, "tissue"), objectsOf(*direct, "tissue"));
    EXPECT_EQ(objectsOf(*ws, "nuclei"), objectsOf(*direct, "nuclei"));
    EXPECT_EQ(objectsOf(*ws, "nuclei").size(), 9u);
    RasterId stored;
    ASSERT_TRUE(ws->store()->rasters().find("prob", stored));
    const int32_t level = ws->store()->rasters().writtenAtLevel(stored);
    const slideio::Size levelSize = ws->scene()->getLevelInfo(level)->getSize();
    const auto sceneRect = ws->scene()->getRect();
    ws->close();
    direct->close();

    ASSERT_EQ(result.exportCount(), 4);
    const char* files[] = {kCsv, kGeoJson, kMask, kProb};
    for (int32_t i = 0; i < 4; ++i) {
        EXPECT_STREQ(result.exportFile(i), files[i]);
        const fs::path path = fs::u8path(f.out) / files[i];
        ASSERT_TRUE(fs::exists(path)) << files[i];
        EXPECT_EQ(static_cast<int64_t>(fs::file_size(path)), result.exportBytes(i)) << files[i];
        EXPECT_GT(result.exportBytes(i), 0) << files[i];
    }

    const std::shared_ptr<slideio::Scene> mask = reopen(fs::u8path(f.out) / kMask);
    ASSERT_NE(mask, nullptr);
    EXPECT_EQ(std::get<2>(mask->getRect()), std::get<2>(sceneRect));
    EXPECT_EQ(std::get<3>(mask->getRect()), std::get<3>(sceneRect));
    const std::shared_ptr<slideio::Scene> raster = reopen(fs::u8path(f.out) / kProb);
    ASSERT_NE(raster, nullptr);
    EXPECT_EQ(std::get<2>(raster->getRect()), levelSize.width);
    EXPECT_EQ(std::get<3>(raster->getRect()), levelSize.height);
}

TEST(SlideRun, SecondRunRunsNothing) {
    Fixture f;
    f.run(threeStageFile());
    const std::string csv = readFile(fs::u8path(f.out) / kCsv);
    const std::string geoJson = readFile(fs::u8path(f.out) / kGeoJson);
    ASSERT_FALSE(csv.empty());
    ASSERT_FALSE(geoJson.empty());

    const SlideRunResult second = f.run(threeStageFile());
    ASSERT_EQ(second.stageCount(), 3);
    for (int32_t i = 0; i < 3; ++i) {
        EXPECT_FALSE(second.stageRan(i)) << second.stageName(i);
        EXPECT_EQ(second.stageObjectsWritten(i), 0);
    }
    EXPECT_EQ(readFile(fs::u8path(f.out) / kCsv), csv);
    EXPECT_EQ(readFile(fs::u8path(f.out) / kGeoJson), geoJson);
}

TEST(SlideRun, ChangedParameterRerunsTheStageAndDependents) {
    Fixture f;
    f.run(threeStageFile(128.0));
    const SlideRunResult second = f.run(threeStageFile(140.0));
    EXPECT_TRUE(ran(second, "tissue"));
    EXPECT_TRUE(ran(second, "nuclei"));
    EXPECT_FALSE(ran(second, "prob"));
}

TEST(SlideRun, RefusesAWorkspaceOfAnotherSlide) {
    Fixture f;
    f.run(threeStageFile());
    const std::string other = ts::writeTestSlide(f.dir.path() / "other.tif", 300, 300).u8string();
    const std::string otherOut = (f.dir.path() / "other-out").u8string();
    const fs::path root = fs::u8path(f.workspace);

    // The records are read first (a read-only open may leave SQLite's sidecars), then the
    // store is closed cleanly and every file and directory snapshotted, sidecars included:
    // the refusal starts from a workspace with no sidecars, so any file it creates other
    // than them shows.
    const std::map<std::string, std::string> records = stageRecords(f.workspace);
    closeCleanly(f.workspace);
    std::map<std::string, std::string> before = contentsUnder(root, true);
    for (const auto& entry : before) EXPECT_FALSE(isSqliteSidecar(entry.first)) << entry.first;
    ASSERT_EQ(before.count("workspace.store"), 1u);

    // Another slide.
    try {
        SlideRun(threeStageFile(), other.c_str(), 0, f.workspace.c_str(), otherOut.c_str(), Configuration::defaults())
            .run();
        ADD_FAILURE() << "a workspace of another slide was accepted";
    } catch (const InvalidArgumentError& e) {
        // The refusal names the workspace it is about and the slide given to the run (paths in
        // diagnostics, 2026-11-23).
        EXPECT_EQ(std::string(e.what()), "SlideRun: the workspace (" + f.workspace +
                                             ") belongs to another slide or scene than the slide (" + other +
                                             "), scene 0");
    }
    // Every file but the sidecars, and every directory, is as it was, with the same bytes.
    EXPECT_EQ(contentsUnder(root, false), before);
    EXPECT_EQ(stageRecords(f.workspace), records);
    EXPECT_FALSE(fs::exists(fs::u8path(otherOut)));

    // The same slide, another scene. No test slide has two scenes, so the scene asked for
    // is one this slide lacks; the workspace's recorded scene index differs all the same.
    closeCleanly(f.workspace);
    before = contentsUnder(root, true);
    for (const auto& entry : before) EXPECT_FALSE(isSqliteSidecar(entry.first)) << entry.first;
    EXPECT_THROW(SlideRun(threeStageFile(), f.slide.c_str(), 1, f.workspace.c_str(), otherOut.c_str(),
                          Configuration::defaults())
                     .run(),
                 InvalidArgumentError);
    EXPECT_EQ(contentsUnder(root, false), before);
    EXPECT_EQ(stageRecords(f.workspace), records);
    EXPECT_FALSE(fs::exists(fs::u8path(otherOut)));
}

TEST(SlideRun, UnregisteredOperationFailsFirst) {
    Fixture f;
    PipelineSpec missing = blobChain(128.0);
    missing.operation("test.run.no_such_operation", OperationParameters());
    const std::string json = std::string("{\"formatVersion\":1,\"requires\":[],\"layers\":[{\"name\":\"tissue\"}],"
                                         "\"rasters\":[],\"stages\":[") +
                             record("tissue", "\"tissue\"", "null", "null", missing) +
                             "],\"exports\":[{\"kind\":\"csv\",\"file\":\"t.csv\",\"layer\":\"tissue\"}]}";
    try {
        f.run(PipelineFile::fromJson(json.c_str()));
        ADD_FAILURE() << "an unregistered operation was accepted";
    } catch (const InvalidArgumentError& e) {
        EXPECT_NE(std::string(e.what()).find("test.run.no_such_operation"), std::string::npos) << e.what();
    }
    EXPECT_FALSE(fs::exists(fs::u8path(f.workspace)));
    EXPECT_FALSE(fs::exists(fs::u8path(f.out)));
}

TEST(SlideRun, CancelledRunLeavesNoPartialFiles) {
    Fixture f;
    f.run(threeStageFile());
    const std::string geoJson = readFile(fs::u8path(f.out) / kGeoJson);
    ASSERT_FALSE(geoJson.empty());

    // Every stage is current, so the first progress report comes from the exports.
    CancellationSource source;
    CancelOnFirstReport observer(source);
    SlideRun run(threeStageFile(), f.slide.c_str(), 0, f.workspace.c_str(), f.out.c_str(), Configuration::defaults());
    run.progress(&observer).cancellation(source.token());
    EXPECT_THROW(run.run(), CancelledError);
    EXPECT_GT(observer.reports(), 0);

    for (const auto& entry : fs::directory_iterator(fs::u8path(f.out))) {
        const std::string name = entry.path().filename().u8string();
        EXPECT_EQ(name.find(".partial"), std::string::npos) << name;
    }
    for (const char* file : {kCsv, kGeoJson, kMask, kProb}) {
        EXPECT_TRUE(fs::exists(fs::u8path(f.out) / file)) << file;
    }
    EXPECT_EQ(readFile(fs::u8path(f.out) / kGeoJson), geoJson);
}

TEST(SlideRun, ReportsTruncatedObjects) {
    ts::registerDisksOperation();
    Fixture f;
    // Halo 4 and disks of radius 20: one inside a tile, one across the seam x = 200, whose
    // owning tile sees only 4 pixels past the seam.
    ts::DisksSpec spec;
    spec.halo = 4;
    spec.disks = {{100.0, 100.0, 20.0, 0.0}, {200.0, 300.0, 20.0, 0.0}};
    PipelineSpec pipeline;
    pipeline.operation("test.disks", ts::disksParameters(spec));
    const std::string json = std::string("{\"formatVersion\":1,\"requires\":[],\"layers\":[{\"name\":\"cells\"}],"
                                         "\"rasters\":[],\"stages\":[") +
                             record("disks", "\"cells\"", "null", "null", pipeline) + "],\"exports\":[]}";
    const Configuration config =
        ConfigurationBuilder().tileSize(200, 200).tileSizeFromSlide(false).workerCount(2).build();
    const SlideRunResult result = f.run(PipelineFile::fromJson(json.c_str()), f.workspace, f.out, config);
    ASSERT_EQ(result.stageCount(), 1);
    EXPECT_TRUE(result.stageRan(0));
    EXPECT_GT(result.stageTruncatedObjects(0), 0);
    EXPECT_EQ(result.exportCount(), 0);
}

// UR-QA-3 across two fresh workspaces. The exports carry the store's provenance, whose
// creation time differs between two workspaces made at different moments; it is the one
// value blanked before the byte comparison (see withoutCreationTime).
TEST(SlideRun, ExportsAreDeterministic) {
    Fixture f;
    const std::string otherWorkspace = (f.dir.path() / "ws2").u8string();
    const std::string otherOut = (f.dir.path() / "out2").u8string();
    f.run(threeStageFile());
    f.run(threeStageFile(), otherWorkspace, otherOut);
    for (const char* file : {kCsv, kGeoJson}) {
        const std::string first = withoutCreationTime(readFile(fs::u8path(f.out) / file));
        ASSERT_FALSE(first.empty()) << file;
        EXPECT_EQ(withoutCreationTime(readFile(fs::u8path(otherOut) / file)), first) << file;
    }
}

// No export carries the slide's path, absolute or as given, or its file name: CSV and
// GeoJSON write the run's slide id (empty by default) in place of the store's, and the
// TIFF exports carry no identifier at all.
TEST(SlideRun, ExportsCarryNoSlidePath) {
    Fixture f;
    const fs::path slide = fs::u8path(f.slide);
    const std::vector<std::string> forbidden = {f.slide, fs::absolute(slide).u8string(),
                                                fs::absolute(slide).generic_u8string(),
                                                slide.filename().u8string()};
    const std::string namedOut = (f.dir.path() / "named-out").u8string();
    f.run(threeStageFile());
    SlideRun(threeStageFile(), f.slide.c_str(), 0, f.workspace.c_str(), namedOut.c_str(), Configuration::defaults())
        .slideId("study-17")
        .run();

    for (const std::string& directory : {f.out, namedOut}) {
        int32_t files = 0;
        for (const auto& entry : fs::directory_iterator(fs::u8path(directory))) {
            ++files;
            const std::string bytes = readFile(entry.path());
            for (const std::string& text : forbidden) {
                EXPECT_EQ(bytes.find(text), std::string::npos) << entry.path().filename().u8string();
            }
        }
        EXPECT_EQ(files, 4);
    }
    EXPECT_NE(readFile(fs::u8path(f.out) / kCsv).find("# slide_id: \n"), std::string::npos);
    EXPECT_NE(readFile(fs::u8path(f.out) / kGeoJson).find("\"slideId\":\"\""), std::string::npos);
    EXPECT_NE(readFile(fs::u8path(namedOut) / kCsv).find("# slide_id: study-17\n"), std::string::npos);
    EXPECT_NE(readFile(fs::u8path(namedOut) / kGeoJson).find("\"slideId\":\"study-17\""), std::string::npos);
}

// A slide that has moved or been deleted since the workspace was made: each error names the
// file it is about by its path (paths in diagnostics, 2026-11-23). The workspace's binding is
// read from its store alone, so a run naming the old path is told the slide cannot be found
// (IoError, naming that path), and a run naming the new path is refused as another slide (the
// recorded one no longer exists to compare), naming the workspace. A run with no workspace
// yet names the missing slide and passes SlideIO's text through, which names it too.
TEST(SlideRun, AMovedSlideNamesItsPath) {
    Fixture f;
    f.run(threeStageFile());
    const fs::path slide = fs::u8path(f.slide);
    const fs::path moved = slide.parent_path() / "moved-slide.tif";
    closeCleanly(f.workspace);
    fs::rename(slide, moved);

    // The error's text, prefixed "IoError: " for an IoError; `path` must be in it.
    const auto expectPath = [&](const std::function<void()>& action, const std::string& path,
                                const char* label) -> std::string {
        try {
            action();
            ADD_FAILURE() << label << " did not throw";
        } catch (const std::exception& e) {
            const std::string message = e.what();
            EXPECT_NE(message.find("slide"), std::string::npos) << label << ": " << message;
            EXPECT_NE(message.find(path), std::string::npos) << label << ": " << message;
            return (dynamic_cast<const IoError*>(&e) != nullptr ? "IoError: " : "") + message;
        }
        return std::string();
    };
    // The workspace is there and the run names the recorded slide, which is gone.
    EXPECT_EQ(expectPath([&] { f.run(threeStageFile()); }, f.slide, "an existing workspace whose slide moved"),
              "IoError: SlideRun: the slide cannot be found (" + f.slide + ")");
    // The run names the slide where it now is: the recorded slide is missing, so it is not this one.
    const std::string movedText = moved.u8string();
    EXPECT_EQ(expectPath(
                  [&] {
                      SlideRun(threeStageFile(), movedText.c_str(), 0, f.workspace.c_str(), f.out.c_str(),
                               Configuration::defaults())
                          .run();
                  },
                  f.workspace, "an existing workspace run on the moved slide"),
              "SlideRun: the workspace (" + f.workspace + ") belongs to another slide or scene than the slide (" +
                  movedText + "), scene 0");
    // No workspace yet, and the slide named for the run is missing: the message names it, and
    // SlideIO's own text, unaltered, follows as the cause, naming it too.
    const std::string fresh = (f.dir.path() / "fresh-ws").u8string();
    const std::string message = expectPath([&] { f.run(threeStageFile(), fresh, f.out); }, f.slide, "a missing slide");
    const std::string lead = "IoError: Workspace::create: cannot open the slide (" + f.slide + "); cause: ";
    EXPECT_EQ(message.rfind(lead, 0), 0u) << message;
    EXPECT_NE(message.find(f.slide, lead.size()), std::string::npos) << message;
}
