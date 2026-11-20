// Requirements: UR-STG-3, UR-STG-10, UR-EXE-1, UR-EXE-4
#include "storage/raster_test_util.hpp"
#include "workspace/support/aux_probe.hpp"
#include "workspace/support/test_slide.hpp"

#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/control.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/core/object_query.hpp>
#include <slideio/processor/core/object_record.hpp>
#include <slideio/processor/pipeline/operation_parameters.hpp>
#include <slideio/processor/pipeline/pipeline.hpp>
#include <slideio/processor/pipeline/pipeline_spec.hpp>
#include <slideio/processor/pipeline/tile_grid.hpp>
#include <slideio/processor/storage/event_journal.hpp>
#include <slideio/processor/storage/layer_registry.hpp>
#include <slideio/processor/storage/object_store.hpp>
#include <slideio/processor/storage/raster_store.hpp>
#include <slideio/processor/storage/stage_state_reader.hpp>
#include <slideio/processor/storage/stage_state_writer.hpp>
#include <slideio/processor/storage/store.hpp>
#include <slideio/processor/workspace/stage_definition.hpp>
#include <slideio/processor/workspace/stage_registry.hpp>
#include <slideio/processor/workspace/stage_runner.hpp>
#include <slideio/processor/workspace/workspace.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace slideio::processor;
using testing_support::TempDirectory;
using testing_support::writeBlobSlide;
using testing_support::writeEdgeBlobSlide;
using testing_support::writeHalfBlobSlide;

namespace fs = std::filesystem;

namespace {

// The chain that finds the nine discs of a blob slide: grey, dark pixels foreground,
// connected components.
PipelineSpec blobChain() {
    PipelineSpec spec;
    spec.operation("pixel.colour_convert", OperationParameters().setString("from", "Rgb").setString("to", "Grey"));
    spec.operation("pixel.threshold_fixed",
                   OperationParameters().setDouble("value", 128.0).setString("direction", "Below"));
    spec.operation("segment.connected_components", OperationParameters());
    return spec;
}

StageDefinition blobStage(const char* name, LayerId layer) {
    StageDefinition def(name);
    def.writesLayer(layer).pipeline(blobChain());
    return def;
}

// Which 600 x 600 slide a Fixture writes.
enum class SlideKind { Blobs, HalfBlobs, Edge };

// A 600 x 600 blob slide in a workspace with the layers the tests name.
struct Fixture {
    explicit Fixture(SlideKind kind = SlideKind::Blobs, int32_t levels = 1) {
        fs::create_directories(dir.path());
        // A pyramid needs the SVS driver, which the extension selects.
        const fs::path file = dir.path() / (levels > 1 ? "slide.svs" : "slide.tif");
        fs::path slide;
        switch (kind) {
        case SlideKind::Blobs: slide = writeBlobSlide(file, 600, 600, levels); break;
        case SlideKind::HalfBlobs: slide = writeHalfBlobSlide(file, 600, 600); break;
        case SlideKind::Edge: slide = writeEdgeBlobSlide(file, 600, 600, levels); break;
        }
        workspacePath = (dir.path() / "ws").u8string();
        workspace = Workspace::create(workspacePath.c_str(), slide.u8string().c_str(), 0, Configuration::defaults());
        LayerLifecycle& l = workspace->layerLifecycle();
        tissue = l.declareSemantic("tissue", workspace->layers().pixelLayer());
        background = l.declareSemantic("background", workspace->layers().pixelLayer());
        nuclei = l.declareSemantic("nuclei", tissue);
        cells = l.declareSemantic("cells", tissue);
    }
    ~Fixture() { workspace->close(); }

    StageRegistry& stages() { return workspace->stages(); }
    int64_t objectsOf(LayerId layer) { return workspace->store()->objects().count(ObjectQuery().layer(layer)); }

    TempDirectory dir;
    std::string workspacePath;
    WorkspacePtr workspace;
    LayerId tissue, background, nuclei, cells;
};

// The level-0 bounding boxes of the objects of a layer.
std::vector<Rect64> boundsOf(Fixture& f, LayerId layer) {
    std::vector<Rect64> bounds;
    const CursorPtr cursor = f.workspace->store()->objects().query(ObjectQuery().layer(layer));
    while (cursor->next()) bounds.push_back(cursor->current().boundingBox());
    return bounds;
}

// The grey, threshold and connected-components chain with `targetLayer` set on the last
// operation when it is not null.
PipelineSpec chainFor(double threshold, const char* direction, const char* targetLayer) {
    PipelineSpec spec;
    spec.operation("pixel.colour_convert", OperationParameters().setString("from", "Rgb").setString("to", "Grey"));
    spec.operation("pixel.threshold_fixed",
                   OperationParameters().setDouble("value", threshold).setString("direction", direction));
    OperationParameters components;
    if (targetLayer != nullptr) components.setString("targetLayer", targetLayer);
    spec.operation("segment.connected_components", components);
    return spec;
}

// Runs the runner and returns the message of the StageError it throws; empty when none.
std::string messageOf(StageRunner& runner, std::string* stageName = nullptr) {
    try {
        runner.run();
    } catch (const StageError& e) {
        if (stageName != nullptr && e.stageName() != nullptr) *stageName = e.stageName();
        return e.what();
    }
    return std::string();
}

class RecordingObserver : public ProgressObserver {
public:
    void onProgress(const char* stageName, double, int64_t, int64_t) override {
        std::lock_guard<std::mutex> guard(mutex_);
        names_.push_back(stageName == nullptr ? "" : stageName);
    }
    std::vector<std::string> names() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return names_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::string> names_;
};

// Calls another runner's run() from inside its first progress report and keeps what that
// call threw.
class NestedRunObserver : public ProgressObserver {
public:
    explicit NestedRunObserver(StageRunner& inner) : inner_(inner) {}
    void onProgress(const char*, double, int64_t, int64_t) override {
        if (called_.exchange(true)) return;
        try {
            inner_.run();
        } catch (const StageError& e) {
            message_ = e.what();
            if (e.stageName() != nullptr) stageName_ = e.stageName();
        }
    }
    bool called() const { return called_.load(); }
    const std::string& message() const { return message_; }
    const std::string& stageName() const { return stageName_; }

private:
    StageRunner& inner_;
    std::atomic<bool> called_{false};
    std::string message_;
    std::string stageName_;
};

} // namespace

TEST(StageRunner, FirstRunWritesObjectsAndCompletes) {
    Fixture f;
    const StageId id = f.stages().declare(blobStage("nuclei", f.nuclei));
    StageRunner runner(*f.workspace, id);
    EXPECT_FALSE(runner.isRerun());
    EXPECT_EQ(runner.objectsWritten(), 0);
    EXPECT_EQ(runner.edgesAsserted(), 0);
    runner.run();
    EXPECT_EQ(f.stages().status(id), StageStatus::Complete);
    EXPECT_GT(f.stages().completedAtUnixMs(id), 0);
    EXPECT_FALSE(runner.isRerun());
    EXPECT_EQ(runner.objectsWritten(), 9);
    EXPECT_EQ(runner.edgesAsserted(), 0);
    EXPECT_EQ(f.objectsOf(f.nuclei), 9);
    EXPECT_FALSE(f.stages().isStale(id));
}

TEST(StageRunner, AnalysisLevelRecordedOnSuccess) {
    Fixture f;
    const StageDefinition def = blobStage("nuclei", f.nuclei);
    const StageId id = f.stages().declare(def);
    StageRunner(*f.workspace, id).run();

    // The level the pipeline picks for this chain, which holds no whole-level operation.
    Pipeline p(f.workspace->scene(), f.workspace->scratchConfiguration(), def.pipeline());
    p.bindLayer("nuclei", f.nuclei);
    p.validate();
    EXPECT_EQ(f.workspace->layers().coarsestAnalysisLevel(f.nuclei), p.analysisLevel());
    EXPECT_EQ(f.workspace->layers().coarsestAnalysisLevel(f.nuclei), 0);
}

TEST(StageRunner, ProgressNamesTheStage) {
    Fixture f;
    const StageId id = f.stages().declare(blobStage("nuclei", f.nuclei));
    RecordingObserver observer;
    StageRunner runner(*f.workspace, id);
    runner.progress(&observer);
    runner.run();
    const std::vector<std::string> names = observer.names();
    ASSERT_FALSE(names.empty());
    for (const std::string& name : names) EXPECT_EQ(name, "nuclei");
}

TEST(StageRunner, BlockedByIncompleteDependency) {
    Fixture f;
    f.stages().declare(blobStage("background", f.tissue));
    StageDefinition def = blobStage("nuclei", f.nuclei);
    def.restrictToLayer(f.tissue);
    const StageId nuclei = f.stages().declare(def);
    StageRunner runner(*f.workspace, nuclei);
    std::string stageName;
    const std::string message = messageOf(runner, &stageName);
    EXPECT_NE(message.find("nuclei"), std::string::npos) << message;
    EXPECT_NE(message.find("background"), std::string::npos) << message;
    EXPECT_EQ(stageName, "nuclei");
    EXPECT_EQ(f.stages().status(nuclei), StageStatus::Pending);
    EXPECT_EQ(f.objectsOf(f.nuclei), 0);
}

TEST(StageRunner, BlockedByStaleDependency) {
    Fixture f;
    const StageId background = f.stages().declare(blobStage("background", f.tissue));
    StageDefinition def = blobStage("nuclei", f.nuclei);
    def.dependsOn("background");
    const StageId nuclei = f.stages().declare(def);
    StageRunner(*f.workspace, background).run();
    f.stages().markStale(background);
    StageRunner runner(*f.workspace, nuclei);
    const std::string message = messageOf(runner);
    EXPECT_NE(message.find("background"), std::string::npos) << message;
    EXPECT_EQ(f.stages().status(nuclei), StageStatus::Pending);
}

TEST(StageRunner, UnreadableStageIsRefused) {
    Fixture f;
    const StageId id = f.stages().declare(blobStage("nuclei", f.nuclei));
    f.workspace->store()->stageStateWriter().setConfiguration(id, "this is not a record");
    f.workspace->close();

    WorkspacePtr ws = Workspace::open(f.workspacePath.c_str(), Configuration::defaults());
    StageRunner runner(*ws, id);
    std::string stageName;
    const std::string message = messageOf(runner, &stageName);
    EXPECT_NE(message.find("nuclei"), std::string::npos) << message;
    EXPECT_EQ(stageName, "nuclei");
    EXPECT_EQ(ws->stages().status(id), StageStatus::Pending);
    ws->close();
}

TEST(StageRunner, ReadOnlyWorkspaceCannotRun) {
    Fixture f;
    const StageId id = f.stages().declare(blobStage("nuclei", f.nuclei));
    WorkspacePtr ro = Workspace::openReadOnly(f.workspacePath.c_str(), Configuration::defaults());
    StageRunner runner(*ro, id);
    EXPECT_THROW(runner.run(), IoError);
    EXPECT_EQ(f.stages().status(id), StageStatus::Pending);
    EXPECT_EQ(ro->stages().status(id), StageStatus::Pending);
    ro->close();
}

TEST(StageRunner, OneRunnerPerWorkspace) {
    Fixture f;
    const StageId first = f.stages().declare(blobStage("first", f.nuclei));
    const StageId second = f.stages().declare(blobStage("second", f.background));
    StageRunner inner(*f.workspace, second);
    NestedRunObserver observer(inner);
    StageRunner outer(*f.workspace, first);
    outer.progress(&observer);
    outer.run();

    EXPECT_TRUE(observer.called());
    EXPECT_EQ(observer.stageName(), "second");
    EXPECT_NE(observer.message().find("second"), std::string::npos) << observer.message();
    EXPECT_EQ(f.stages().status(first), StageStatus::Complete);
    EXPECT_EQ(f.stages().status(second), StageStatus::Pending);

    // The slot is free again once the first run is over.
    inner.run();
    EXPECT_EQ(f.stages().status(second), StageStatus::Complete);
}

TEST(StageRunner, UnknownStageIsInvalidArgument) {
    Fixture f;
    f.stages().declare(blobStage("nuclei", f.nuclei));
    EXPECT_THROW(StageRunner(*f.workspace, StageId(99)), InvalidArgumentError);
    EXPECT_THROW(StageRunner(*f.workspace, StageId()), InvalidArgumentError);
}

TEST(StageRunner, TargetLayerNotDeclaredIsStageError) {
    Fixture f;
    StageDefinition def("nuclei");
    def.writesLayer(f.nuclei).pipeline(chainFor(128.0, "Below", "cells"));
    const StageId id = f.stages().declare(def);
    StageRunner runner(*f.workspace, id);
    std::string stageName;
    const std::string message = messageOf(runner, &stageName);
    EXPECT_NE(message.find("nuclei"), std::string::npos) << message;
    EXPECT_NE(message.find("segment.connected_components"), std::string::npos) << message;
    EXPECT_NE(message.find("cells"), std::string::npos) << message;
    EXPECT_EQ(stageName, "nuclei");
    EXPECT_EQ(f.stages().status(id), StageStatus::Failed);
    EXPECT_EQ(f.objectsOf(f.nuclei), 0);
    EXPECT_EQ(f.objectsOf(f.cells), 0);
}

TEST(StageRunner, MissingTargetWithTwoLayersIsStageError) {
    Fixture f;
    StageDefinition def("both");
    def.writesLayer(f.cells).writesLayer(f.nuclei).pipeline(chainFor(128.0, "Below", nullptr));
    const StageId id = f.stages().declare(def);
    StageRunner runner(*f.workspace, id);
    std::string stageName;
    const std::string message = messageOf(runner, &stageName);
    EXPECT_NE(message.find("both"), std::string::npos) << message;
    EXPECT_NE(message.find("segment.connected_components"), std::string::npos) << message;
    EXPECT_EQ(stageName, "both");
    EXPECT_EQ(f.stages().status(id), StageStatus::Failed);
    EXPECT_EQ(f.objectsOf(f.nuclei) + f.objectsOf(f.cells), 0);
}

TEST(StageRunner, MissingTargetWithNoLayerIsStageError) {
    Fixture f;
    StageDefinition def("nowhere");
    def.pipeline(chainFor(128.0, "Below", nullptr));
    const StageId id = f.stages().declare(def);
    StageRunner runner(*f.workspace, id);
    std::string stageName;
    const std::string message = messageOf(runner, &stageName);
    EXPECT_NE(message.find("nowhere"), std::string::npos) << message;
    EXPECT_NE(message.find("segment.connected_components"), std::string::npos) << message;
    EXPECT_EQ(stageName, "nowhere");
    EXPECT_EQ(f.stages().status(id), StageStatus::Failed);
}

TEST(StageRunner, EmptyTargetLayerCountsAsAbsent) {
    Fixture f;
    StageDefinition def("both");
    def.writesLayer(f.cells).writesLayer(f.nuclei).pipeline(chainFor(128.0, "Below", ""));
    const StageId id = f.stages().declare(def);
    StageRunner runner(*f.workspace, id);
    const std::string message = messageOf(runner);
    // The runner's refusal, not the pipeline's: it names the stage.
    EXPECT_NE(message.find("stage \"both\""), std::string::npos) << message;
    EXPECT_NE(message.find("segment.connected_components"), std::string::npos) << message;
    EXPECT_EQ(f.stages().status(id), StageStatus::Failed);

    // With one written layer the empty name resolves to it.
    StageDefinition one("one");
    one.writesLayer(f.nuclei).pipeline(chainFor(128.0, "Below", ""));
    const StageId oneId = f.stages().declare(one);
    StageRunner(*f.workspace, oneId).run();
    EXPECT_EQ(f.stages().status(oneId), StageStatus::Complete);
    EXPECT_EQ(f.objectsOf(f.nuclei), 9);
}

TEST(StageRunner, TwoLayersEachNamedRun) {
    Fixture f;
    PipelineSpec spec;
    spec.operation("pixel.colour_convert", OperationParameters().setString("from", "Rgb").setString("to", "Grey"));
    spec.operation("pixel.threshold_fixed",
                   OperationParameters().setDouble("value", 128.0).setString("direction", "Below"));
    spec.operation("segment.connected_components", OperationParameters().setString("targetLayer", "cells"));
    spec.operation("segment.connected_components", OperationParameters().setString("targetLayer", "nuclei"));
    StageDefinition def("both");
    def.writesLayer(f.cells).writesLayer(f.nuclei).pipeline(spec);
    const StageId id = f.stages().declare(def);
    StageRunner runner(*f.workspace, id);
    runner.run();
    EXPECT_EQ(f.stages().status(id), StageStatus::Complete);
    EXPECT_EQ(f.objectsOf(f.cells), 9);
    EXPECT_EQ(f.objectsOf(f.nuclei), 9);
    EXPECT_EQ(runner.objectsWritten(), 18);
}

TEST(StageRunner, RasterOnlyStageMayDeclareNoLayer) {
    Fixture f;
    PipelineSpec spec;
    spec.operation("pixel.gaussian_blur", OperationParameters());
    StageDefinition def("blur");
    def.pipeline(spec);
    const StageId id = f.stages().declare(def);
    StageRunner runner(*f.workspace, id);
    runner.run();
    EXPECT_EQ(f.stages().status(id), StageStatus::Complete);
    EXPECT_EQ(runner.objectsWritten(), 0);
}

TEST(StageRunner, RestrictionKeepsObjectsInsideTheLayer) {
    Fixture f(SlideKind::HalfBlobs);

    // The left half of the slide as one object (its discs are holes, so extractHoles is off).
    PipelineSpec halfSpec;
    halfSpec.operation("pixel.colour_convert", OperationParameters().setString("from", "Rgb").setString("to", "Grey"));
    halfSpec.operation("pixel.threshold_fixed",
                       OperationParameters().setDouble("value", 230.0).setString("direction", "Above"));
    halfSpec.operation("segment.connected_components", OperationParameters().setBool("extractHoles", false));
    StageDefinition half("half");
    half.writesLayer(f.background).pipeline(halfSpec);
    const StageId halfId = f.stages().declare(half);

    // Control: with no restriction, all six discs are found.
    StageDefinition control("control");
    control.writesLayer(f.cells).pipeline(chainFor(128.0, "Below", nullptr));
    StageRunner(*f.workspace, f.stages().declare(control)).run();
    EXPECT_EQ(f.objectsOf(f.cells), 6);

    StageRunner(*f.workspace, halfId).run();
    ASSERT_EQ(f.objectsOf(f.background), 1);

    StageDefinition restricted("nuclei");
    restricted.writesLayer(f.nuclei).restrictToLayer(f.background).pipeline(chainFor(128.0, "Below", nullptr));
    const StageId id = f.stages().declare(restricted);
    StageRunner runner(*f.workspace, id);
    runner.run();
    EXPECT_EQ(f.stages().status(id), StageStatus::Complete);
    EXPECT_EQ(runner.objectsWritten(), 3);
    const std::vector<Rect64> bounds = boundsOf(f, f.nuclei);
    const std::vector<Rect64> region = boundsOf(f, f.background);
    ASSERT_EQ(bounds.size(), 3u);
    ASSERT_EQ(region.size(), 1u);
    for (const Rect64& box : bounds) {
        // Each object is one of the three left discs: its centre is in the white half, and
        // inside the layer's bounds.
        const int64_t cx = box.x + box.width / 2;
        EXPECT_GE(box.x, 0);
        EXPECT_EQ(cx, 100) << "an object that is not a left disc";
        EXPECT_GE(cx, region[0].x);
        EXPECT_LT(cx, region[0].right());
    }
}

TEST(StageRunner, RestrictionToCoarseLayerIsOneSided) {
    Fixture f(SlideKind::Edge, 3);

    // The background layer is the white region, found at level 2: its edge, at 302, is
    // rounded out to the level-2 pixel grid.
    PipelineSpec coarse;
    coarse.operation("pixel.colour_convert", OperationParameters().setString("from", "Rgb").setString("to", "Grey"));
    coarse.operation("pixel.threshold_fixed",
                     OperationParameters().setDouble("value", 230.0).setString("direction", "Above"));
    coarse.operation("segment.connected_components", OperationParameters().setBool("extractHoles", false));
    coarse.analysisLevel(2);
    StageDefinition bg("background");
    bg.writesLayer(f.background).pipeline(coarse);
    StageRunner(*f.workspace, f.stages().declare(bg)).run();
    const int32_t level = f.workspace->layers().coarsestAnalysisLevel(f.background);
    ASSERT_EQ(level, 2);
    const std::vector<Rect64> background = boundsOf(f, f.background);
    ASSERT_EQ(background.size(), 1u);
    const Rect64 region = background[0];
    EXPECT_GT(region.right(), testing_support::kEdgeX) << "the coarse region should reach past the true edge";

    StageDefinition def("nuclei");
    def.writesLayer(f.nuclei).restrictToLayer(f.background).pipeline(chainFor(128.0, "Below", nullptr));
    const StageId id = f.stages().declare(def);
    StageRunner(*f.workspace, id).run();
    EXPECT_EQ(f.stages().status(id), StageStatus::Complete);

    // Discs A, B, C of writeEdgeBlobSlide, by their level-0 centres.
    const double halfStep = LevelSelector(f.workspace->scene()).scaleOfLevel(level) / 2.0;
    const int64_t centreA = testing_support::kEdgeX - 25;
    const int64_t centreB = testing_support::kEdgeX + 60;
    // B lies wholly outside the region dilated by the half-step, A and C have their centres inside.
    ASSERT_GT(static_cast<double>(centreB - 20), static_cast<double>(region.right()) + halfStep);
    ASSERT_LT(centreA, region.right());

    const std::vector<Rect64> nuclei = boundsOf(f, f.nuclei);
    bool foundA = false, foundB = false, foundC = false;
    for (const Rect64& box : nuclei) {
        const int64_t cx = box.x + box.width / 2;
        const int64_t cy = box.y + box.height / 2;
        foundA = foundA || (cx == centreA && cy == 150);
        foundB = foundB || cx == centreB;
        foundC = foundC || (cx == 100 && cy == 450);
        // No object lies outside the dilated region (by its centre: the anchor decides).
        EXPECT_LE(static_cast<double>(cx), static_cast<double>(region.right()) + halfStep);
    }
    EXPECT_TRUE(foundA) << "the disc 25 px inside the edge was missed";
    EXPECT_TRUE(foundC) << "the disc inside the region was missed";
    EXPECT_FALSE(foundB) << "the disc outside the dilated region was found";
    EXPECT_EQ(nuclei.size(), 2u);
}

namespace {

// The blur chain of a raster-writing stage.
PipelineSpec blurChain() {
    PipelineSpec spec;
    spec.operation("pixel.gaussian_blur", OperationParameters());
    return spec;
}

// A chain of the auxiliary probe, reading the input bound as `binding`.
PipelineSpec probeChain(const char* binding) {
    PipelineSpec spec;
    spec.operation("test.ws.auxProbe", OperationParameters().setString("binding", binding));
    return spec;
}

// A raster sink that keeps a copy of every Float32 tile it is given.
class CopyingRasterSink : public RasterSink {
public:
    void writeTile(const BlockView& tile) override {
        std::lock_guard<std::mutex> lock(mutex_);
        tiles_.push_back(makeView(tile.level(), tile.slideRect(), tile.width(), tile.height(), DataType::Float32,
                                  tile.planeCount(), [&tile](int64_t x, int64_t y, int32_t plane) {
                                      const uint8_t* row = static_cast<const uint8_t*>(tile.planeData(plane)) +
                                                           y * tile.planeRowStrideBytes(plane);
                                      return static_cast<double>(reinterpret_cast<const float*>(row)[x]);
                                  }));
    }
    void flush() override {}
    int64_t tilesWritten() const override { return static_cast<int64_t>(tiles_.size()); }
    const std::vector<std::unique_ptr<OwnedView>>& tiles() const { return tiles_; }

private:
    std::mutex mutex_;
    std::vector<std::unique_ptr<OwnedView>> tiles_;
};

// The kinds of the journal's events, in order.
std::vector<EventKind> eventKinds(Fixture& f) {
    std::vector<EventKind> kinds;
    const EventCursorPtr cursor = f.workspace->store()->events().subscribe(EventJournal::kFromBeginning);
    while (cursor->next(0)) kinds.push_back(cursor->current().kind());
    return kinds;
}

// The index of the first event of `kind`; -1 when there is none.
int64_t firstOf(const std::vector<EventKind>& kinds, EventKind kind) {
    for (size_t i = 0; i < kinds.size(); ++i) {
        if (kinds[i] == kind) return static_cast<int64_t>(i);
    }
    return -1;
}

// A raster the blur stage writes: three Float32 planes, as the blur of an RGB slide.
RasterId declareProb(Fixture& f) {
    return f.workspace->store()->rasters().declare(RasterDeclaration("prob", DataType::Float32, 3));
}

} // namespace

TEST(StageRunner, WrittenRasterIsPublishedOnSuccess) {
    Fixture f;
    const RasterId prob = declareProb(f);
    StageDefinition def("blur");
    def.writesRaster(prob).pipeline(blurChain());
    const StageId id = f.stages().declare(def);
    RasterRegistry& rasters = f.workspace->store()->rasters();
    EXPECT_FALSE(rasters.isPopulated(prob));

    StageRunner(*f.workspace, id).run();
    EXPECT_EQ(f.stages().status(id), StageStatus::Complete);
    ASSERT_TRUE(rasters.isPopulated(prob));
    EXPECT_EQ(rasters.writerStage(prob), id);

    // The same blur run directly: the sink keeps a copy of each tile the pipeline writes.
    CopyingRasterSink direct;
    Pipeline reference(f.workspace->scene(), f.workspace->scratchConfiguration(), blurChain());
    reference.rasterSink(&direct);
    reference.run(nullptr, CancellationToken());
    ASSERT_FALSE(direct.tiles().empty());

    // Every stored tile of the same rect and level matches it; some sample is non-zero
    // (an empty raster reads as zeros) and some is below white (a disc edge).
    const TileRasterSourcePtr stored = rasters.sourceFor(prob);
    bool nonZero = false, edge = false;
    for (const auto& tile : direct.tiles()) {
        const BlockView& want = tile->view;
        auto got = makeView(want.level(), want.slideRect(), want.width(), want.height(), DataType::Float32, 3, 7.0);
        stored->fillTile(got->view);
        for (int32_t plane = 0; plane < 3; ++plane) {
            for (int64_t y = 0; y < want.height(); ++y) {
                const float* a = reinterpret_cast<const float*>(static_cast<const uint8_t*>(want.planeData(plane)) +
                                                                y * want.planeRowStrideBytes(plane));
                const float* b = reinterpret_cast<const float*>(static_cast<const uint8_t*>(got->view.planeData(plane)) +
                                                                y * got->view.planeRowStrideBytes(plane));
                for (int64_t x = 0; x < want.width(); ++x) {
                    ASSERT_NEAR(a[x], b[x], 1e-3) << "plane " << plane << " at (" << x << ", " << y << ")";
                    nonZero = nonZero || b[x] > 0.0f;
                    edge = edge || (b[x] > 0.0f && b[x] < 250.0f);
                }
            }
        }
    }
    EXPECT_TRUE(nonZero);
    EXPECT_TRUE(edge) << "the compared tiles cover a disc edge";
}

TEST(StageRunner, StoredRasterIsReadByBinding) {
    testing_support::registerAuxProbe();
    testing_support::resetAuxProbe();
    Fixture f;
    const RasterId prob = declareProb(f);
    StageDefinition blur("blur");
    blur.writesRaster(prob).pipeline(blurChain());
    const StageId first = f.stages().declare(blur);
    StageDefinition probe("probe");
    probe.readsRaster("prob", prob).pipeline(probeChain("prob"));
    const StageId second = f.stages().declare(probe);

    bool dependsOnFirst = false;
    for (int32_t i = 0; i < f.stages().dependencyCount(second); ++i) {
        dependsOnFirst = dependsOnFirst || f.stages().dependencyAt(second, i) == first;
    }
    EXPECT_TRUE(dependsOnFirst);

    StageRunner second1(*f.workspace, second);
    EXPECT_NE(messageOf(second1).find("blur"), std::string::npos) << "refused until the writer is complete";
    EXPECT_EQ(testing_support::auxProbeTiles("prob"), 0);

    StageRunner(*f.workspace, first).run();
    StageRunner(*f.workspace, second).run();
    EXPECT_EQ(f.stages().status(second), StageStatus::Complete);
    EXPECT_GT(testing_support::auxProbeTiles("prob"), 0);
}

TEST(StageRunner, LayerAsRasterIsBound) {
    testing_support::registerAuxProbe();
    testing_support::resetAuxProbe();
    Fixture f;
    const StageId first = f.stages().declare(blobStage("nuclei", f.nuclei));
    StageDefinition probe("probe");
    probe.readsLayerAsRaster("coarse", f.nuclei, LayerRasterContent::Membership).pipeline(probeChain("coarse"));
    const StageId second = f.stages().declare(probe);

    StageRunner(*f.workspace, first).run();
    StageRunner(*f.workspace, second).run();
    EXPECT_EQ(f.stages().status(second), StageStatus::Complete);
    EXPECT_GT(testing_support::auxProbeTiles("coarse"), 0);
}

TEST(StageRunner, FlushOrderObjectsEdgesAnnotationsRaster) {
    Fixture f;
    // The blobs chain ends in one Int32 label plane, which is what the stage's raster stores.
    const RasterId mask = f.workspace->store()->rasters().declare(RasterDeclaration("mask", DataType::Int32, 1));
    const PipelineSpec spec = blobChain();
    StageDefinition def("both");
    def.writesLayer(f.nuclei).writesRaster(mask).pipeline(spec);
    const StageId id = f.stages().declare(def);
    StageRunner(*f.workspace, id).run();

    const std::vector<EventKind> kinds = eventKinds(f);
    const int64_t objects = firstOf(kinds, EventKind::ObjectsAdded);
    const int64_t raster = firstOf(kinds, EventKind::RasterWritten);
    ASSERT_GE(objects, 0);
    ASSERT_GE(raster, 0);
    EXPECT_LT(objects, raster);
    EXPECT_EQ(f.objectsOf(f.nuclei), 9);
}
