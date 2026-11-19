// Requirements: UR-STG-2, UR-STG-4, UR-STG-8
#include "workspace/support/test_slide.hpp"

#include <workspace/detail/stage_record.hpp>

#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/data_type.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/storage/layer_registry.hpp>
#include <slideio/processor/storage/raster_store.hpp>
#include <slideio/processor/storage/stage_state_reader.hpp>
#include <slideio/processor/storage/stage_state_writer.hpp>
#include <slideio/processor/storage/store.hpp>
#include <slideio/processor/workspace/stage_definition.hpp>
#include <slideio/processor/workspace/stage_registry.hpp>
#include <slideio/processor/workspace/workspace.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

using namespace slideio::processor;
using testing_support::TempDirectory;
using testing_support::writeTestSlide;

namespace fs = std::filesystem;

namespace {

// A workspace holding the layers and rasters the tests name.
struct Fixture {
    Fixture() {
        fs::create_directories(dir.path());
        const fs::path slide = writeTestSlide(dir.path() / "slide.tif", 300, 200);
        const fs::path ws = dir.path() / "ws";
        workspace = Workspace::create(ws.u8string().c_str(), slide.u8string().c_str(), 0, Configuration::defaults());
        LayerLifecycle& l = workspace->layerLifecycle();
        const LayerId pixel = workspace->layers().pixelLayer();
        tissue = l.declareSemantic("tissue", pixel);
        background = l.declareSemantic("background", pixel);
        nuclei = l.declareSemantic("nuclei", tissue);
        x = l.declareSemantic("x", pixel);
        y = l.declareSemantic("y", pixel);
        probability = workspace->store()->rasters().declare(RasterDeclaration("probability", DataType::Float32, 1));
    }
    ~Fixture() { workspace->close(); }

    StageRegistry& stages() { return workspace->stages(); }

    TempDirectory dir;
    WorkspacePtr workspace;
    LayerId tissue, background, nuclei, x, y;
    RasterId probability;
};

std::string messageOf(const std::function<void()>& call) {
    try {
        call();
    } catch (const std::exception& e) {
        return e.what();
    }
    return std::string();
}

StageDefinition writer(const char* name, LayerId layer) {
    StageDefinition def(name);
    def.writesLayer(layer);
    return def;
}

} // namespace

TEST(StageRegistry, DeclarePersistsRecordAndWrittenLayers) {
    Fixture f;
    StageDefinition def("background");
    def.writesLayer(f.background);
    const StageId id = f.stages().declare(def);
    EXPECT_TRUE(id.isValid());
    EXPECT_EQ(f.stages().count(), 1);
    EXPECT_EQ(f.stages().at(0), id);
    EXPECT_STREQ(f.stages().name(id), "background");
    EXPECT_EQ(f.stages().status(id), StageStatus::Pending);
    EXPECT_FALSE(f.stages().isStale(id));
    EXPECT_EQ(f.stages().completedAtUnixMs(id), 0);
    EXPECT_EQ(std::string(f.stages().configuration(id)),
              detail::stageRecordJson(def, f.workspace->layers(), f.workspace->store()->rasters()));
    EXPECT_STREQ(f.workspace->store()->stageStates().configuration(id), f.stages().configuration(id));
    LayerRegistry& layers = f.workspace->layers();
    ASSERT_EQ(layers.writerStageCount(f.background), 1);
    EXPECT_EQ(layers.writerStageAt(f.background, 0), id);
    StageId found;
    EXPECT_TRUE(f.stages().find("background", found));
    EXPECT_EQ(found, id);
    EXPECT_FALSE(f.stages().find("nope", found));
    EXPECT_EQ(f.stages().definition(id).name(), std::string("background"));
}

TEST(StageRegistry, DependencyDerivedFromReads) {
    Fixture f;
    const StageId background = f.stages().declare(writer("background", f.background));
    StageDefinition nuclei("nuclei");
    nuclei.readsLayer(f.background).writesLayer(f.nuclei);
    const StageId n = f.stages().declare(nuclei);
    EXPECT_EQ(f.stages().dependencyCount(background), 0);
    ASSERT_EQ(f.stages().dependencyCount(n), 1);
    EXPECT_EQ(f.stages().dependencyAt(n, 0), background);
    EXPECT_FALSE(f.stages().isExplicitDependency(n, 0));
    StageStateReader& reader = f.workspace->store()->stageStates();
    ASSERT_EQ(reader.dependencyCount(n), 1);
    EXPECT_EQ(reader.dependencyAt(n, 0), background);
}

TEST(StageRegistry, ReaderDeclaredBeforeWriter) {
    Fixture f;
    StageDefinition nuclei("nuclei");
    nuclei.readsLayer(f.background).writesLayer(f.nuclei);
    const StageId n = f.stages().declare(nuclei);
    EXPECT_EQ(f.stages().dependencyCount(n), 0);
    EXPECT_TRUE(f.stages().isRunnable(n));
    const StageId background = f.stages().declare(writer("background", f.background));
    ASSERT_EQ(f.stages().dependencyCount(n), 1);
    EXPECT_EQ(f.stages().dependencyAt(n, 0), background);
    EXPECT_EQ(f.workspace->store()->stageStates().dependencyCount(n), 1);
    EXPECT_FALSE(f.stages().isRunnable(n));
    f.workspace->store()->stageStateWriter().setStatus(background, StageStatus::Complete);
    EXPECT_TRUE(f.stages().isRunnable(n));
}

TEST(StageRegistry, RasterReadDerivesDependency) {
    Fixture f;
    StageDefinition prob("prob");
    prob.writesRaster(f.probability);
    const StageId p = f.stages().declare(prob);
    StageDefinition use("use");
    use.readsRaster("prob", f.probability).writesLayer(f.nuclei);
    const StageId u = f.stages().declare(use);
    ASSERT_EQ(f.stages().dependencyCount(u), 1);
    EXPECT_EQ(f.stages().dependencyAt(u, 0), p);
    EXPECT_FALSE(f.stages().isExplicitDependency(u, 0));
}

TEST(StageRegistry, ExplicitDependencyFlagged) {
    Fixture f;
    const StageId a = f.stages().declare(writer("a", f.x));
    StageDefinition b("b");
    b.dependsOn("a").writesLayer(f.y);
    const StageId bid = f.stages().declare(b);
    ASSERT_EQ(f.stages().dependencyCount(bid), 1);
    EXPECT_EQ(f.stages().dependencyAt(bid, 0), a);
    EXPECT_TRUE(f.stages().isExplicitDependency(bid, 0));
    // Also derived: still flagged explicit, and listed once.
    StageDefinition c("c");
    c.dependsOn("a").readsLayer(f.x);
    const StageId cid = f.stages().declare(c);
    ASSERT_EQ(f.stages().dependencyCount(cid), 1);
    EXPECT_TRUE(f.stages().isExplicitDependency(cid, 0));
}

TEST(StageRegistry, UnknownExplicitDependencyIsStageError) {
    Fixture f;
    StageDefinition b("b");
    b.dependsOn("ghost");
    const std::string message = messageOf([&] { f.stages().declare(b); });
    EXPECT_NE(message.find("b"), std::string::npos);
    EXPECT_NE(message.find("ghost"), std::string::npos);
    EXPECT_THROW(f.stages().declare(b), StageError);
    EXPECT_EQ(f.stages().count(), 0);
}

TEST(StageRegistry, ReadOfUndeclaredLayerIsInvalidArgument) {
    Fixture f;
    StageDefinition s("s");
    s.readsLayer(LayerId(9999));
    EXPECT_THROW(f.stages().declare(s), InvalidArgumentError);
    EXPECT_EQ(f.stages().count(), 0);
    EXPECT_EQ(f.workspace->store()->stageStates().count(), 0);
}

TEST(StageRegistry, DuplicateNameIsInvalidArgument) {
    Fixture f;
    f.stages().declare(writer("a", f.x));
    EXPECT_THROW(f.stages().declare(writer("a", f.y)), InvalidArgumentError);
    EXPECT_EQ(f.stages().count(), 1);
}

TEST(StageRegistry, CycleIsStageErrorNamingStages) {
    Fixture f;
    StageDefinition a("alpha");
    a.readsLayer(f.x).writesLayer(f.y);
    const StageId aid = f.stages().declare(a);
    StageDefinition b("beta");
    b.readsLayer(f.y).writesLayer(f.x);
    const std::string message = messageOf([&] { f.stages().declare(b); });
    EXPECT_NE(message.find("alpha"), std::string::npos);
    EXPECT_NE(message.find("beta"), std::string::npos);
    EXPECT_THROW(f.stages().declare(b), StageError);
    EXPECT_EQ(f.stages().count(), 1);
    EXPECT_EQ(f.stages().dependencyCount(aid), 0);
    EXPECT_EQ(f.workspace->store()->stageStates().count(), 1);
    EXPECT_EQ(f.workspace->layers().writerStageCount(f.x), 0);
}

TEST(StageRegistry, LayerWrittenByNoStageIsNotAnError) {
    Fixture f;
    StageDefinition s("s");
    s.readsLayer(f.tissue).writesLayer(f.nuclei);
    const StageId id = f.stages().declare(s);
    EXPECT_EQ(f.stages().dependencyCount(id), 0);
    EXPECT_TRUE(f.stages().isRunnable(id));
}

TEST(StageRegistry, RunnableWhenDependenciesCompleteAndCurrent) {
    Fixture f;
    const StageId a = f.stages().declare(writer("a", f.x));
    StageDefinition b("b");
    b.readsLayer(f.x);
    const StageId bid = f.stages().declare(b);
    StageStateWriter& w = f.workspace->store()->stageStateWriter();
    EXPECT_TRUE(f.stages().isRunnable(a));
    EXPECT_FALSE(f.stages().isRunnable(bid));
    w.setStatus(a, StageStatus::Running);
    EXPECT_FALSE(f.stages().isRunnable(bid));
    w.setStatus(a, StageStatus::Failed);
    EXPECT_FALSE(f.stages().isRunnable(bid));
    w.setStatus(a, StageStatus::Complete);
    EXPECT_TRUE(f.stages().isRunnable(bid));
    w.setStale(a, true);
    EXPECT_FALSE(f.stages().isRunnable(bid));
    w.setStale(a, false);
    EXPECT_TRUE(f.stages().isRunnable(bid));
}

TEST(StageRegistry, TwoStagesWritingOneLayerBothCount) {
    Fixture f;
    const StageId a = f.stages().declare(writer("a", f.x));
    const StageId b = f.stages().declare(writer("b", f.x));
    StageDefinition r("r");
    r.readsLayer(f.x);
    const StageId rid = f.stages().declare(r);
    ASSERT_EQ(f.stages().dependencyCount(rid), 2);
    EXPECT_EQ(f.stages().dependencyAt(rid, 0), a);
    EXPECT_EQ(f.stages().dependencyAt(rid, 1), b);
    EXPECT_EQ(f.workspace->layers().writerStageCount(f.x), 2);
}

TEST(StageRegistry, SelfReadAndWriteIsNotACycle) {
    Fixture f;
    StageDefinition s("s");
    s.readsLayer(f.x).writesLayer(f.x);
    const StageId id = f.stages().declare(s);
    EXPECT_EQ(f.stages().dependencyCount(id), 0);
}

TEST(StageRegistry, WritingPixelLayerWritesNothing) {
    Fixture f;
    StageDefinition s("s");
    s.writesLayer(f.workspace->layers().pixelLayer());
    EXPECT_THROW(f.stages().declare(s), InvalidArgumentError);
    EXPECT_EQ(f.workspace->store()->stageStates().count(), 0);
}

TEST(StageRegistry, ReadOnlyWorkspaceRefusesDeclare) {
    TempDirectory dir;
    fs::create_directories(dir.path());
    const fs::path slide = writeTestSlide(dir.path() / "slide.tif", 300, 200);
    const std::string ws = (dir.path() / "ws").u8string();
    WorkspacePtr w = Workspace::create(ws.c_str(), slide.u8string().c_str(), 0, Configuration::defaults());
    w->close();
    WorkspacePtr ro = Workspace::openReadOnly(ws.c_str(), Configuration::defaults());
    EXPECT_THROW(ro->stages().declare(StageDefinition("s")), IoError);
    EXPECT_EQ(ro->store()->stageStates().count(), 0);
    ro->close();
}
