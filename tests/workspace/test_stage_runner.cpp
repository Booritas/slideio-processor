// Requirements: UR-STG-3, UR-STG-10, UR-EXE-1, UR-EXE-4
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
#include <slideio/processor/storage/layer_registry.hpp>
#include <slideio/processor/storage/object_store.hpp>
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
#include <mutex>
#include <string>
#include <vector>

using namespace slideio::processor;
using testing_support::TempDirectory;
using testing_support::writeBlobSlide;
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
enum class SlideKind { Blobs, HalfBlobs };

// A 600 x 600 blob slide in a workspace with the layers the tests name.
struct Fixture {
    explicit Fixture(SlideKind kind = SlideKind::Blobs, int32_t levels = 1) {
        fs::create_directories(dir.path());
        // A pyramid needs the SVS driver, which the extension selects.
        const fs::path file = dir.path() / (levels > 1 ? "slide.svs" : "slide.tif");
        const fs::path slide = kind == SlideKind::Blobs ? writeBlobSlide(file, 600, 600, levels)
                                                        : writeHalfBlobSlide(file, 600, 600);
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

bool contains(const Rect64& outer, const Rect64& inner, int64_t margin) {
    return inner.x >= outer.x - margin && inner.y >= outer.y - margin &&
           inner.right() <= outer.right() + margin && inner.bottom() <= outer.bottom() + margin;
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
    ASSERT_EQ(bounds.size(), 3u);
    for (const Rect64& box : bounds) EXPECT_LT(box.right(), 300) << "an object right of the layer";
}

TEST(StageRunner, RestrictionToCoarseLayerIsOneSided) {
    Fixture f(SlideKind::Blobs, 3);

    // The background layer is found at level 2, a quarter of the resolution.
    PipelineSpec coarse = chainFor(128.0, "Below", nullptr);
    coarse.analysisLevel(2);
    StageDefinition bg("background");
    bg.writesLayer(f.background).pipeline(coarse);
    StageRunner(*f.workspace, f.stages().declare(bg)).run();
    ASSERT_EQ(f.workspace->layers().coarsestAnalysisLevel(f.background), 2);
    const std::vector<Rect64> background = boundsOf(f, f.background);
    ASSERT_EQ(background.size(), 9u);

    StageDefinition def("nuclei");
    def.writesLayer(f.nuclei).restrictToLayer(f.background).pipeline(chainFor(128.0, "Below", nullptr));
    const StageId id = f.stages().declare(def);
    StageRunner(*f.workspace, id).run();
    EXPECT_EQ(f.stages().status(id), StageStatus::Complete);

    const double halfStep = LevelSelector(f.workspace->scene()).scaleOfLevel(2) / 2.0;
    ASSERT_DOUBLE_EQ(halfStep, 2.0);
    const std::vector<Rect64> nuclei = boundsOf(f, f.nuclei);
    for (const Rect64& box : nuclei) {
        bool inside = false;
        for (const Rect64& area : background) inside = inside || contains(area, box, 2);
        EXPECT_TRUE(inside) << "a nucleus lies outside the dilated background";
    }
    // Every disc whose centre is in the undilated background is found.
    for (const Rect64& area : background) {
        const int64_t cx = area.x + area.width / 2;
        const int64_t cy = area.y + area.height / 2;
        bool found = false;
        for (const Rect64& box : nuclei) {
            found = found || (cx >= box.x && cx < box.right() && cy >= box.y && cy < box.bottom());
        }
        EXPECT_TRUE(found) << "a disc inside the background was missed";
    }
}
