// Requirements: UR-STG-2, UR-STG-4, UR-STG-8, UR-STG-10, UR-STG-11, UR-OBS-1
#include "workspace/support/test_slide.hpp"

#include <workspace/detail/declare_fault.hpp>
#include <workspace/detail/refresh_fault.hpp>
#include <workspace/detail/stage_record.hpp>

#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/data_type.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/log.hpp>
#include <slideio/processor/pipeline/operation_catalog.hpp>
#include <slideio/processor/pipeline/operation_parameters.hpp>
#include <slideio/processor/pipeline/pipeline_spec.hpp>
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
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

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

std::string workspacePath(const Fixture& f) { return (f.dir.path() / "ws").u8string(); }

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

TEST(StageRegistry, UnknownExplicitDependencyStaysUnresolved) {
    Fixture f;
    StageDefinition b("b");
    b.dependsOn("ghost");
    const StageId id = f.stages().declare(b);
    EXPECT_EQ(f.stages().count(), 1);
    EXPECT_EQ(f.stages().dependencyCount(id), 0);
    ASSERT_EQ(f.stages().unresolvedDependencyCount(id), 1);
    EXPECT_STREQ(f.stages().unresolvedDependencyAt(id, 0), "ghost");
    EXPECT_THROW(f.stages().unresolvedDependencyAt(id, 1), InvalidArgumentError);
    EXPECT_THROW(f.stages().unresolvedDependencyAt(id, -1), InvalidArgumentError);
    EXPECT_FALSE(f.stages().isRunnable(id));
    EXPECT_EQ(f.workspace->store()->stageStates().dependencyCount(id), 0);
    EXPECT_THROW(f.stages().unresolvedDependencyCount(StageId(9999)), InvalidArgumentError);
}

TEST(StageRegistry, LateDeclaredDependencyResolves) {
    Fixture f;
    // a reads `tissue`, which nothing writes yet, so a has no dependency of its own.
    StageDefinition a("a");
    a.readsLayer(f.tissue).writesLayer(f.x);
    StageDefinition b("b");
    b.dependsOn("a").writesLayer(f.y);
    const StageId bid = f.stages().declare(b);
    EXPECT_FALSE(f.stages().isRunnable(bid));
    ASSERT_EQ(f.stages().unresolvedDependencyCount(bid), 1);
    EXPECT_STREQ(f.stages().unresolvedDependencyAt(bid, 0), "a");
    EXPECT_EQ(f.stages().dependencyCount(bid), 0);

    const StageId aid = f.stages().declare(a);
    ASSERT_EQ(f.stages().dependencyCount(bid), 1);
    EXPECT_EQ(f.stages().dependencyAt(bid, 0), aid);
    EXPECT_TRUE(f.stages().isExplicitDependency(bid, 0));
    EXPECT_EQ(f.stages().unresolvedDependencyCount(bid), 0);
    EXPECT_FALSE(f.stages().isRunnable(bid)) << "a is declared but not complete";
    f.workspace->store()->stageStateWriter().setStatus(aid, StageStatus::Complete);
    EXPECT_TRUE(f.stages().isRunnable(bid));
    ASSERT_EQ(f.workspace->store()->stageStates().dependencyCount(bid), 1);
    EXPECT_EQ(f.workspace->store()->stageStates().dependencyAt(bid, 0), aid);

    // c depends on b and writes what a reads: a -> c -> b -> a.
    StageDefinition c("c");
    c.dependsOn("b").writesLayer(f.tissue);
    const std::string message = messageOf([&] { f.stages().declare(c); });
    EXPECT_NE(message.find("\"a\""), std::string::npos) << message;
    EXPECT_NE(message.find("\"b\""), std::string::npos) << message;
    EXPECT_NE(message.find("\"c\""), std::string::npos) << message;
    StageId unused;
    EXPECT_FALSE(f.stages().find("c", unused));
    EXPECT_EQ(f.stages().unresolvedDependencyCount(bid), 0);
    EXPECT_EQ(f.stages().dependencyCount(aid), 0);
    ASSERT_EQ(f.stages().dependencyCount(bid), 1);
    EXPECT_EQ(f.workspace->store()->stageStates().dependencyCount(aid), 0);
    EXPECT_EQ(f.workspace->store()->stageStates().dependencyCount(bid), 1);
}

TEST(StageRegistry, LateDeclarationClosingACycleThroughAnUnresolvedNameIsRefused) {
    Fixture f;
    StageDefinition a("a");
    a.dependsOn("b").writesLayer(f.x);
    f.stages().declare(a);
    StageDefinition b("b");
    b.dependsOn("a").writesLayer(f.y);
    const std::string message = messageOf([&] { f.stages().declare(b); });
    EXPECT_NE(message.find("\"a\""), std::string::npos) << message;
    EXPECT_NE(message.find("\"b\""), std::string::npos) << message;
    EXPECT_EQ(f.stages().count(), 1);
    StageId aid;
    ASSERT_TRUE(f.stages().find("a", aid));
    ASSERT_EQ(f.stages().unresolvedDependencyCount(aid), 1);
    EXPECT_STREQ(f.stages().unresolvedDependencyAt(aid, 0), "b");
    EXPECT_EQ(f.stages().dependencyCount(aid), 0);
    EXPECT_EQ(f.workspace->store()->stageStates().dependencyCount(aid), 0);
}

TEST(StageRegistry, UnresolvedNameSurvivesRedeclare) {
    Fixture f;
    StageDefinition b("b");
    b.dependsOn("ghost-with-a-name-long-enough-to-leave-the-small-buffer").writesLayer(f.y);
    const StageId id = f.stages().declare(b);
    const char* old = f.stages().unresolvedDependencyAt(id, 0);
    const StageDefinition& held = f.stages().definition(id);
    ASSERT_EQ(held.explicitDependencyCount(), 1);

    StageDefinition changed("b");
    changed.dependsOn("other").writesLayer(f.y);
    f.stages().declare(changed);
    // Overwrite freed memory, if any, so a dangling read is visible.
    std::vector<std::string> noise(64, std::string(80, 'x'));
    EXPECT_STREQ(old, "ghost-with-a-name-long-enough-to-leave-the-small-buffer");
    EXPECT_STREQ(held.explicitDependencyAt(0), "ghost-with-a-name-long-enough-to-leave-the-small-buffer");
    ASSERT_EQ(f.stages().unresolvedDependencyCount(id), 1);
    EXPECT_STREQ(f.stages().unresolvedDependencyAt(id, 0), "other");
}

TEST(StageRegistry, UnresolvedDependencySurvivesReopen) {
    Fixture f;
    StageDefinition b("b");
    b.dependsOn("a").writesLayer(f.y);
    const StageId bid = f.stages().declare(b);
    f.workspace->close();

    {
        WorkspacePtr ro = Workspace::openReadOnly(workspacePath(f).c_str(), Configuration::defaults());
        ASSERT_EQ(ro->stages().unresolvedDependencyCount(bid), 1);
        EXPECT_STREQ(ro->stages().unresolvedDependencyAt(bid, 0), "a");
        EXPECT_EQ(ro->stages().dependencyCount(bid), 0);
        EXPECT_FALSE(ro->stages().isRunnable(bid));
        EXPECT_FALSE(ro->stages().isUnreadable(bid));
        ro->close();
    }
    WorkspacePtr ws = Workspace::open(workspacePath(f).c_str(), Configuration::defaults());
    StageRegistry& s = ws->stages();
    ASSERT_EQ(s.unresolvedDependencyCount(bid), 1);
    EXPECT_FALSE(s.isUnreadable(bid));
    EXPECT_FALSE(s.isRunnable(bid));
    const StageId aid = s.declare(writer("a", f.x));
    EXPECT_EQ(s.unresolvedDependencyCount(bid), 0);
    ASSERT_EQ(s.dependencyCount(bid), 1);
    EXPECT_EQ(s.dependencyAt(bid, 0), aid);
    ws->close();
}

TEST(StageRegistry, SelfDependencyIsStillRefused) {
    Fixture f;
    StageDefinition s("s");
    s.dependsOn("s").writesLayer(f.x);
    const std::string message = messageOf([&] { f.stages().declare(s); });
    EXPECT_NE(message.find("\"s\""), std::string::npos) << message;
    EXPECT_NE(message.find("itself"), std::string::npos) << message;
    EXPECT_THROW(f.stages().declare(s), StageError);
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

// One locked call answers the first reason a stage cannot run, in StageRunner's order,
// with the name it concerns, so the gate has no count/at race (PR #17 finding 4).
TEST(StageRegistry, RunBlockerReportsTheFirstReasonWithItsName) {
    Fixture f;
    const StageId a = f.stages().declare(writer("a", f.x));
    const StageId c = f.stages().declare(writer("c", f.background));
    StageDefinition b("b");
    b.readsLayer(f.x).readsLayer(f.background).dependsOn("later").dependsOn("after");
    const StageId bid = f.stages().declare(b);
    StageStateWriter& w = f.workspace->store()->stageStateWriter();
    const char* name = "untouched";

    EXPECT_EQ(f.stages().runBlocker(a, name), RunBlocker::None);
    EXPECT_EQ(name, nullptr);
    EXPECT_EQ(f.stages().runBlocker(bid, name), RunBlocker::UnresolvedDependency);
    EXPECT_STREQ(name, "later");
    f.stages().declare(writer("later", f.y));
    EXPECT_EQ(f.stages().runBlocker(bid, name), RunBlocker::UnresolvedDependency);
    EXPECT_STREQ(name, "after");
    const StageId after = f.stages().declare(writer("after", f.tissue));
    StageId later;
    ASSERT_TRUE(f.stages().find("later", later));

    // The dependencies in dependencyAt order: a, c, later, after.
    EXPECT_EQ(f.stages().runBlocker(bid, name), RunBlocker::DependencyIncomplete);
    EXPECT_STREQ(name, "a");
    for (const StageId d : {a, c, later, after}) w.setStatus(d, StageStatus::Complete);
    w.setStale(c, true);
    EXPECT_EQ(f.stages().runBlocker(bid, name), RunBlocker::DependencyStale);
    EXPECT_STREQ(name, "c");
    w.setStale(c, false);
    w.setStatus(after, StageStatus::Failed);
    EXPECT_EQ(f.stages().runBlocker(bid, name), RunBlocker::DependencyIncomplete);
    EXPECT_STREQ(name, "after");
    w.setStatus(after, StageStatus::Complete);
    EXPECT_EQ(f.stages().runBlocker(bid, name), RunBlocker::None);
    EXPECT_EQ(name, nullptr);
    EXPECT_TRUE(f.stages().isRunnable(bid));
    EXPECT_THROW(f.stages().runBlocker(StageId(99), name), InvalidArgumentError);
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

namespace {

// Four stages: background -> nuclei -> cells, and an unrelated one.
struct Chain {
    explicit Chain(Fixture& f) {
        background = f.stages().declare(writer("background", f.background));
        StageDefinition n("nuclei");
        n.readsLayer(f.background).writesLayer(f.nuclei);
        nuclei = f.stages().declare(n);
        StageDefinition c("cells");
        c.readsLayer(f.nuclei).writesLayer(f.x);
        cells = f.stages().declare(c);
        other = f.stages().declare(writer("other", f.y));
    }
    StageId background, nuclei, cells, other;
};

PipelineSpec blurSpec(double sigma) {
    PipelineSpec spec;
    spec.operation("pixel.gaussian_blur", OperationParameters().setDouble("sigma", sigma));
    return spec;
}

class CaptureSink : public LogSink {
public:
    void write(LogLevel level, const char* message) override {
        std::lock_guard<std::mutex> guard(mutex);
        got.emplace_back(level, message);
    }
    std::mutex mutex;
    std::vector<std::pair<LogLevel, std::string>> got;
};

// Installs a capture sink for the scope and restores the process defaults.
class LogCapture {
public:
    LogCapture() : sink_(std::make_shared<CaptureSink>()) {
        setLogSink(sink_);
        setLogLevel(LogLevel::Warning);
    }
    ~LogCapture() {
        setLogSink(nullptr);
        setLogLevel(LogLevel::Warning);
    }
    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;
    std::vector<std::pair<LogLevel, std::string>> messages() const {
        std::lock_guard<std::mutex> guard(sink_->mutex);
        return sink_->got;
    }

private:
    std::shared_ptr<CaptureSink> sink_;
};

} // namespace

TEST(StageRegistry, MarkStaleIsTransitive) {
    Fixture f;
    const Chain c(f);
    f.stages().markStale(c.background);
    EXPECT_TRUE(f.stages().isStale(c.background));
    EXPECT_TRUE(f.stages().isStale(c.nuclei));
    EXPECT_TRUE(f.stages().isStale(c.cells));
    EXPECT_FALSE(f.stages().isStale(c.other));
    StageStateReader& reader = f.workspace->store()->stageStates();
    EXPECT_TRUE(reader.isStale(c.nuclei));
    EXPECT_TRUE(reader.isStale(c.cells));
    EXPECT_FALSE(reader.isStale(c.other));
    EXPECT_THROW(f.stages().markStale(StageId(9999)), InvalidArgumentError);
}

// A diamond a -> {b, c} -> d. The journal records no staleness writes and setStale has no
// seam, so "each stage once" is not observable; the test asserts the final flags (d is
// reached down two paths, a is not marked: it is the stage being re-run).
TEST(StageRegistry, MarkAffectedStaleMarksEveryPathOnce) {
    Fixture f;
    const StageId a = f.stages().declare(writer("a", f.x));
    StageDefinition bDef("b");
    bDef.readsLayer(f.x).writesLayer(f.y);
    const StageId b = f.stages().declare(bDef);
    StageDefinition cDef("c");
    cDef.readsLayer(f.x).writesLayer(f.background);
    const StageId c = f.stages().declare(cDef);
    StageDefinition dDef("d");
    dDef.readsLayer(f.y).readsLayer(f.background).writesLayer(f.nuclei);
    const StageId d = f.stages().declare(dDef);
    ASSERT_EQ(f.stages().staleness().affectedCount(a), 4);  // b, c, then d down both paths

    f.stages().markAffectedStale(a);
    EXPECT_FALSE(f.stages().isStale(a));
    EXPECT_TRUE(f.stages().isStale(b));
    EXPECT_TRUE(f.stages().isStale(c));
    EXPECT_TRUE(f.stages().isStale(d));
    EXPECT_THROW(f.stages().markAffectedStale(StageId(9999)), InvalidArgumentError);

    WorkspacePtr ro = Workspace::openReadOnly((f.dir.path() / "ws").u8string().c_str(), Configuration::defaults());
    EXPECT_THROW(ro->stages().markAffectedStale(a), IoError);
    EXPECT_THROW(ro->stages().markAffectedStale(StageId(9999)), IoError);
    ro->close();
}

TEST(StageRegistry, ClearStaleIsNotTransitive) {
    Fixture f;
    const Chain c(f);
    f.stages().markStale(c.background);
    f.stages().clearStale(c.nuclei);
    EXPECT_TRUE(f.stages().isStale(c.background));
    EXPECT_FALSE(f.stages().isStale(c.nuclei));
    EXPECT_TRUE(f.stages().isStale(c.cells));
    EXPECT_THROW(f.stages().clearStale(StageId(9999)), InvalidArgumentError);
}

TEST(StageRegistry, ReconfiguredStageAndDependentsGoStale) {
    Fixture f;
    StageDefinition bg("background");
    bg.writesLayer(f.background);
    const StageId background = f.stages().declare(bg);
    StageDefinition n("nuclei");
    n.readsLayer(f.background).writesLayer(f.nuclei).pipeline(blurSpec(1.0));
    const StageId nuclei = f.stages().declare(n);
    StageDefinition c("cells");
    c.readsLayer(f.nuclei).writesLayer(f.x);
    const StageId cells = f.stages().declare(c);
    const std::string before = f.stages().configuration(nuclei);

    StageDefinition changed("nuclei");
    changed.readsLayer(f.background).writesLayer(f.nuclei).pipeline(blurSpec(2.0));
    EXPECT_EQ(f.stages().declare(changed), nuclei);
    EXPECT_EQ(f.stages().count(), 3);
    EXPECT_FALSE(f.stages().isStale(background));
    EXPECT_TRUE(f.stages().isStale(nuclei));
    EXPECT_TRUE(f.stages().isStale(cells));
    EXPECT_NE(std::string(f.stages().configuration(nuclei)), before);
    EXPECT_EQ(std::string(f.stages().configuration(nuclei)),
              detail::stageRecordJson(changed, f.workspace->layers(), f.workspace->store()->rasters()));
    EXPECT_TRUE(detail::sameDeclaration(f.stages().definition(nuclei), changed));
    ASSERT_EQ(f.stages().dependencyCount(nuclei), 1);
    EXPECT_EQ(f.stages().dependencyAt(nuclei, 0), background);
}

TEST(StageRegistry, EquivalentRedeclarationMarksNothingStale) {
    Fixture f;
    // The created operation holds every parameter, defaults included, so the by-name spec
    // spells out the defaulted kernelSize. A by-name spec giving only some parameters
    // compares unequal to a then()-built one, because the created operation carries its
    // defaults: the comparison errs toward recomputation (a redeclaration marks stale).
    // Normalising defaults is a PipelineSpec follow-up.
    const OperationParameters params = OperationParameters().setDouble("sigma", 1.5).setInt("kernelSize", 0);
    PipelineSpec viaThen;
    viaThen.then(OperationCatalog::instance().create("pixel.gaussian_blur", params));
    PipelineSpec viaOperation;
    viaOperation.operation("pixel.gaussian_blur", params);

    StageDefinition first("blur");
    first.writesLayer(f.x).pipeline(viaThen);
    const StageId id = f.stages().declare(first);
    StageDefinition second("blur");
    second.writesLayer(f.x).pipeline(viaOperation);
    EXPECT_EQ(f.stages().declare(second), id);
    EXPECT_FALSE(f.stages().isStale(id));
    EXPECT_EQ(f.stages().count(), 1);
}

TEST(StageRegistry, RedeclarationThatWouldCycleIsRefused) {
    Fixture f;
    const Chain c(f);
    const std::string record = f.stages().configuration(c.background);
    StageDefinition loop("background");
    loop.readsLayer(f.x).writesLayer(f.background);
    const std::string message = messageOf([&] { f.stages().declare(loop); });
    EXPECT_NE(message.find("background"), std::string::npos);
    EXPECT_THROW(f.stages().declare(loop), StageError);
    EXPECT_EQ(std::string(f.stages().configuration(c.background)), record);
    EXPECT_EQ(f.stages().dependencyCount(c.background), 0);
    EXPECT_FALSE(f.stages().isStale(c.background));
    EXPECT_FALSE(f.stages().isStale(c.nuclei));
    EXPECT_EQ(f.stages().count(), 4);
}

TEST(StageRegistry, ReopenedWorkspaceRestoresDefinitions) {
    Fixture f;
    StageDefinition bg("background");
    bg.writesLayer(f.background);
    StageDefinition n("nuclei");
    n.readsLayer(f.background).writesLayer(f.nuclei).pipeline(blurSpec(1.0)).dependsOn("background");
    StageDefinition c("cells");
    c.readsLayer(f.nuclei).writesLayer(f.x);
    const StageId bgId = f.stages().declare(bg);
    const StageId nId = f.stages().declare(n);
    const StageId cId = f.stages().declare(c);
    f.stages().markStale(cId);
    f.workspace->close();

    WorkspacePtr w = Workspace::open(workspacePath(f).c_str(), Configuration::defaults());
    StageRegistry& r = w->stages();
    ASSERT_EQ(r.count(), 3);
    StageId found;
    ASSERT_TRUE(r.find("nuclei", found));
    EXPECT_EQ(found, nId);
    EXPECT_STREQ(r.name(bgId), "background");
    EXPECT_STREQ(r.name(cId), "cells");
    ASSERT_EQ(r.dependencyCount(nId), 1);
    EXPECT_EQ(r.dependencyAt(nId, 0), bgId);
    EXPECT_TRUE(r.isExplicitDependency(nId, 0));
    ASSERT_EQ(r.dependencyCount(cId), 1);
    EXPECT_EQ(r.dependencyAt(cId, 0), nId);
    EXPECT_FALSE(r.isExplicitDependency(cId, 0));
    EXPECT_FALSE(r.isStale(bgId));
    EXPECT_FALSE(r.isStale(nId));
    EXPECT_TRUE(r.isStale(cId));
    EXPECT_TRUE(detail::sameDeclaration(r.definition(bgId), bg));
    EXPECT_TRUE(detail::sameDeclaration(r.definition(nId), n));
    EXPECT_TRUE(detail::sameDeclaration(r.definition(cId), c));

    EXPECT_EQ(r.declare(n), nId);
    EXPECT_EQ(r.declare(bg), bgId);
    EXPECT_FALSE(r.isStale(nId));
    EXPECT_FALSE(r.isStale(bgId));
    w->close();
}

TEST(StageRegistry, ReadOnlyWorkspaceSeesStagesButCannotDeclare) {
    Fixture f;
    const Chain c(f);
    WorkspacePtr ro = Workspace::openReadOnly(workspacePath(f).c_str(), Configuration::defaults());
    StageRegistry& r = ro->stages();
    EXPECT_EQ(r.count(), 4);
    ASSERT_EQ(r.dependencyCount(c.cells), 1);
    EXPECT_EQ(r.dependencyAt(c.cells, 0), c.nuclei);
    EXPECT_STREQ(r.definition(c.nuclei).name(), "nuclei");
    EXPECT_THROW(r.declare(writer("more", f.y)), IoError);
    EXPECT_THROW(r.markStale(c.background), IoError);
    EXPECT_THROW(r.clearStale(c.background), IoError);
    EXPECT_EQ(r.count(), 4);
    ro->close();
}

TEST(StageRegistry, DeclareAfterReopenDerivesFromPersistedStages) {
    Fixture f;
    StageDefinition w("writer");
    w.readsLayer(f.y).writesLayer(f.x);
    const StageId writerId = f.stages().declare(w);
    f.workspace->close();

    WorkspacePtr ws = Workspace::open(workspacePath(f).c_str(), Configuration::defaults());
    StageRegistry& r = ws->stages();
    StageDefinition reader("reader");
    reader.readsLayer(f.x).writesLayer(f.tissue);
    const StageId readerId = r.declare(reader);
    ASSERT_EQ(r.dependencyCount(readerId), 1);
    EXPECT_EQ(r.dependencyAt(readerId, 0), writerId);
    StageStateReader& states = ws->store()->stageStates();
    ASSERT_EQ(states.dependencyCount(readerId), 1);
    EXPECT_EQ(states.dependencyAt(readerId, 0), writerId);

    // A stage writing y would make the persisted writer depend on a stage that reads x.
    StageDefinition closer("closer");
    closer.readsLayer(f.x).writesLayer(f.y);
    const std::string message = messageOf([&] { r.declare(closer); });
    EXPECT_NE(message.find("writer"), std::string::npos);
    EXPECT_NE(message.find("closer"), std::string::npos);
    EXPECT_THROW(r.declare(closer), StageError);
    EXPECT_EQ(r.count(), 2);
    EXPECT_EQ(states.count(), 2);
    EXPECT_EQ(r.dependencyCount(writerId), 0);
    EXPECT_EQ(states.dependencyCount(writerId), 0);
    EXPECT_EQ(ws->layers().writerStageCount(f.y), 0);
    ws->close();
}

TEST(StageRegistry, UnreadableRecordIsKeptByNameAndNeverRunnable) {
    Fixture f;
    f.workspace->store()->stageStateWriter().declare("broken", "this is not a record");
    const StageId good = f.stages().declare(writer("good", f.x));
    f.workspace->close();

    const LogCapture log;
    WorkspacePtr ws = Workspace::open(workspacePath(f).c_str(), Configuration::defaults());
    StageRegistry& r = ws->stages();
    ASSERT_EQ(r.count(), 2);
    StageId broken;
    ASSERT_TRUE(r.find("broken", broken));
    EXPECT_FALSE(r.isRunnable(broken));
    const char* blockerName = "untouched";
    EXPECT_EQ(r.runBlocker(broken, blockerName), RunBlocker::Unreadable);
    EXPECT_EQ(blockerName, nullptr);
    EXPECT_TRUE(r.isRunnable(good));
    EXPECT_EQ(r.dependencyCount(broken), 0);
    EXPECT_EQ(r.definition(broken).writtenLayerCount(), 0);
    bool warned = false;
    for (const auto& m : log.messages()) {
        if (m.first == LogLevel::Warning && m.second.find("\"broken\"") != std::string::npos) warned = true;
        EXPECT_EQ(m.second.find("\"good\""), std::string::npos);
    }
    EXPECT_TRUE(warned);

    // Declaring the stage again with a real definition repairs it.
    EXPECT_EQ(r.declare(writer("broken", f.y)), broken);
    EXPECT_TRUE(r.isRunnable(broken));
    EXPECT_TRUE(r.isStale(broken));
    ws->close();
}

TEST(StageRegistry, UnreadableWriterKeepsReaderDependency) {
    Fixture f;
    const StageId w = f.stages().declare(writer("w", f.x));
    StageDefinition rd("r");
    rd.readsLayer(f.x).writesLayer(f.y);
    const StageId r = f.stages().declare(rd);
    ASSERT_EQ(f.stages().dependencyCount(r), 1);
    f.workspace->store()->stageStateWriter().setConfiguration(w, "this is not a record");
    f.workspace->close();

    // A read-only open restores the pinned edge and writes nothing.
    {
        WorkspacePtr ro = Workspace::openReadOnly(workspacePath(f).c_str(), Configuration::defaults());
        ASSERT_EQ(ro->stages().dependencyCount(r), 1);
        EXPECT_EQ(ro->stages().dependencyAt(r, 0), w);
        EXPECT_FALSE(ro->stages().isExplicitDependency(r, 0));
        EXPECT_FALSE(ro->stages().isRunnable(r));
        ro->close();
    }

    WorkspacePtr ws = Workspace::open(workspacePath(f).c_str(), Configuration::defaults());
    StageRegistry& s = ws->stages();
    ASSERT_EQ(s.dependencyCount(r), 1);
    EXPECT_EQ(s.dependencyAt(r, 0), w);
    EXPECT_FALSE(s.isExplicitDependency(r, 0));
    EXPECT_FALSE(s.isRunnable(r));
    EXPECT_THROW(s.dependencyAt(r, 1), InvalidArgumentError);

    // An unrelated declare rewrites every row and carries the pinned edge.
    s.declare(writer("other", f.tissue));
    ASSERT_EQ(s.dependencyCount(r), 1);
    ASSERT_EQ(ws->store()->stageStates().dependencyCount(r), 1);
    EXPECT_EQ(ws->store()->stageStates().dependencyAt(r, 0), w);

    // Redeclaring w validly drops the pin; derivation is normal again.
    EXPECT_EQ(s.declare(writer("w", f.x)), w);
    EXPECT_TRUE(s.isRunnable(w));
    ASSERT_EQ(s.dependencyCount(r), 1);
    EXPECT_EQ(s.dependencyAt(r, 0), w);
    EXPECT_TRUE(s.isStale(w));
    ws->close();
}

TEST(StageRegistry, RestoreFallsBackToPersistedDependenciesWhenRederivationFails) {
    Fixture f;
    const StageId w = f.stages().declare(writer("w", f.x));
    StageDefinition rd("r");
    rd.readsLayer(f.x).writesLayer(f.y);
    const StageId r = f.stages().declare(rd);
    // A record whose explicit dependency names the stage itself, with a stored
    // dependency row on w.
    StageDefinition ghost("orphan");
    ghost.readsLayer(f.x).dependsOn("orphan");
    const std::string ghostRecord =
        detail::stageRecordJson(ghost, f.workspace->layers(), f.workspace->store()->rasters());
    StageStateWriter& raw = f.workspace->store()->stageStateWriter();
    const StageId orphan = raw.declare("orphan", ghostRecord.c_str());
    raw.addDependency(orphan, w);
    raw.setStatus(w, StageStatus::Complete);
    f.workspace->close();

    const LogCapture log;
    WorkspacePtr ws = Workspace::open(workspacePath(f).c_str(), Configuration::defaults());
    StageRegistry& s = ws->stages();
    ASSERT_EQ(s.count(), 3);
    ASSERT_EQ(s.dependencyCount(r), 1);
    EXPECT_EQ(s.dependencyAt(r, 0), w);
    EXPECT_TRUE(s.isRunnable(r));
    // The damaged stage is kept by name only, never runnable, its stored edge pinned.
    ASSERT_EQ(s.dependencyCount(orphan), 1);
    EXPECT_EQ(s.dependencyAt(orphan, 0), w);
    EXPECT_FALSE(s.isExplicitDependency(orphan, 0));
    EXPECT_FALSE(s.isRunnable(orphan));
    EXPECT_EQ(s.definition(orphan).explicitDependencyCount(), 0);
    bool warned = false;
    for (const auto& m : log.messages()) {
        if (m.first == LogLevel::Warning && m.second.find("\"orphan\"") != std::string::npos) warned = true;
        EXPECT_EQ(m.second.find("\"r\""), std::string::npos);
    }
    EXPECT_TRUE(warned);

    // Any declare carries the pin into the stored rows.
    s.declare(writer("other", f.tissue));
    ASSERT_EQ(ws->store()->stageStates().dependencyCount(orphan), 1);
    EXPECT_EQ(ws->store()->stageStates().dependencyAt(orphan, 0), w);
    ws->close();
}

TEST(StageRegistry, DanglingNameInRecordStaysUnresolvedNotUnreadable) {
    Fixture f;
    StageDefinition dangling("dangling");
    dangling.writesLayer(f.y).dependsOn("ghost");
    const std::string record =
        detail::stageRecordJson(dangling, f.workspace->layers(), f.workspace->store()->rasters());
    const StageId id = f.workspace->store()->stageStateWriter().declare("dangling", record.c_str());
    f.workspace->close();

    const LogCapture log;
    WorkspacePtr ws = Workspace::open(workspacePath(f).c_str(), Configuration::defaults());
    StageRegistry& s = ws->stages();
    EXPECT_FALSE(s.isUnreadable(id));
    EXPECT_EQ(s.definition(id).explicitDependencyCount(), 1);
    ASSERT_EQ(s.unresolvedDependencyCount(id), 1);
    EXPECT_STREQ(s.unresolvedDependencyAt(id, 0), "ghost");
    EXPECT_FALSE(s.isRunnable(id));
    for (const auto& m : log.messages()) EXPECT_NE(m.first, LogLevel::Warning) << m.second;
    ws->close();
}

TEST(StageRegistry, ExplicitDependencyCycleIsStageError) {
    Fixture f;
    f.stages().declare(writer("a", f.x));
    StageDefinition b("b");
    b.readsLayer(f.x).writesLayer(f.y);
    f.stages().declare(b);
    StageDefinition loop("a");
    loop.writesLayer(f.x).dependsOn("b");
    const std::string message = messageOf([&] { f.stages().declare(loop); });
    EXPECT_NE(message.find("\"a\""), std::string::npos);
    EXPECT_NE(message.find("\"b\""), std::string::npos);
    EXPECT_THROW(f.stages().declare(loop), StageError);
    StageId a;
    ASSERT_TRUE(f.stages().find("a", a));
    EXPECT_EQ(f.stages().dependencyCount(a), 0);
    EXPECT_EQ(f.stages().definition(a).explicitDependencyCount(), 0);
}

TEST(StageRegistry, ClosedWorkspaceRefusesDeclareWithIoError) {
    Fixture f;
    const Chain c(f);
    f.workspace->close();
    StageDefinition late("late");
    late.readsLayer(f.x).writesLayer(f.tissue);
    const std::string message = messageOf([&] { f.stages().declare(late); });
    EXPECT_EQ(message.find("not declared"), std::string::npos);
    EXPECT_NE(message.find("StageRegistry::declare"), std::string::npos);
    EXPECT_THROW(f.stages().declare(late), IoError);
    EXPECT_THROW(f.stages().declare(writer("background", f.background)), IoError);
    EXPECT_THROW(f.stages().markStale(c.background), IoError);
    EXPECT_THROW(f.stages().clearStale(c.background), IoError);
    // The graph answers from memory.
    ASSERT_EQ(f.stages().dependencyCount(c.cells), 1);
    EXPECT_EQ(f.stages().dependencyAt(c.cells, 0), c.nuclei);
}

TEST(StageRegistry, IdenticalRedeclarationRepairsRows) {
    Fixture f;
    const StageId w = f.stages().declare(writer("w", f.x));
    // A stage whose declare stored the record and failed before its other rows.
    StageDefinition rd("r");
    rd.readsLayer(f.x).writesLayer(f.y);
    const std::string record = detail::stageRecordJson(rd, f.workspace->layers(), f.workspace->store()->rasters());
    const StageId r = f.workspace->store()->stageStateWriter().declare("r", record.c_str());
    f.workspace->close();

    // Read-only: the same declaration returns the id and writes nothing.
    {
        WorkspacePtr ro = Workspace::openReadOnly(workspacePath(f).c_str(), Configuration::defaults());
        EXPECT_EQ(ro->stages().declare(rd), r);
        EXPECT_EQ(ro->layers().writerStageCount(f.y), 0);
        EXPECT_EQ(ro->store()->stageStates().dependencyCount(r), 0);
        ro->close();
    }

    WorkspacePtr ws = Workspace::open(workspacePath(f).c_str(), Configuration::defaults());
    EXPECT_EQ(ws->layers().writerStageCount(f.y), 0);
    EXPECT_EQ(ws->stages().declare(rd), r);
    ASSERT_EQ(ws->layers().writerStageCount(f.y), 1);
    EXPECT_EQ(ws->layers().writerStageAt(f.y, 0), r);
    StageStateReader& states = ws->store()->stageStates();
    ASSERT_EQ(states.dependencyCount(r), 1);
    EXPECT_EQ(states.dependencyAt(r, 0), w);
    EXPECT_FALSE(ws->stages().isStale(r));
    EXPECT_FALSE(ws->stages().isStale(w));
    ws->close();
}

TEST(StageRegistry, ReadOnlyRegistryFollowsLiveWriter) {
    Fixture f;
    StageDefinition bg("background");
    bg.writesLayer(f.background);
    const StageId background = f.stages().declare(bg);
    const StageId other = f.stages().declare(writer("other", f.y));
    StageDefinition n("nuclei");
    n.readsLayer(f.background).writesLayer(f.nuclei).pipeline(blurSpec(1.0));
    const StageId nuclei = f.stages().declare(n);
    f.workspace->store()->stageStateWriter().setStatus(background, StageStatus::Complete);

    WorkspacePtr ro = Workspace::openReadOnly(workspacePath(f).c_str(), Configuration::defaults());
    StageRegistry& r = ro->stages();
    ASSERT_EQ(r.dependencyCount(nuclei), 1);
    const StageDefinition& before = r.definition(nuclei);

    // The writer declares a reader of an existing stage's layer, and redeclares nuclei
    // with a changed pipeline and an explicit dependency.
    StageDefinition c("cells");
    c.readsLayer(f.nuclei).writesLayer(f.x);
    const StageId cells = f.stages().declare(c);
    StageDefinition changed("nuclei");
    changed.readsLayer(f.background).writesLayer(f.nuclei).pipeline(blurSpec(2.0)).dependsOn("other");
    EXPECT_EQ(f.stages().declare(changed), nuclei);

    ASSERT_EQ(r.count(), 4);
    const StageStateReader& states = ro->store()->stageStates();
    for (int32_t i = 0; i < r.count(); ++i) {
        const StageId id = r.at(i);
        SCOPED_TRACE(r.name(id));
        ASSERT_EQ(r.dependencyCount(id), states.dependencyCount(id));
        ASSERT_EQ(r.dependencyCount(id), f.stages().dependencyCount(id));
        for (int32_t d = 0; d < r.dependencyCount(id); ++d) {
            EXPECT_EQ(r.dependencyAt(id, d), states.dependencyAt(id, d));
            EXPECT_EQ(r.dependencyAt(id, d), f.stages().dependencyAt(id, d));
            EXPECT_EQ(r.isExplicitDependency(id, d), f.stages().isExplicitDependency(id, d));
        }
        EXPECT_EQ(r.isRunnable(id), f.stages().isRunnable(id));
        EXPECT_TRUE(detail::sameDeclaration(r.definition(id), f.stages().definition(id)));
        EXPECT_EQ(std::string(r.configuration(id)),
                  detail::stageRecordJson(r.definition(id), ro->layers(), ro->store()->rasters()));
    }
    ASSERT_EQ(r.dependencyCount(cells), 1);
    EXPECT_EQ(r.dependencyAt(cells, 0), nuclei);
    ASSERT_EQ(r.dependencyCount(nuclei), 2);
    EXPECT_EQ(r.dependencyAt(nuclei, 1), other);
    EXPECT_TRUE(r.isExplicitDependency(nuclei, 1));
    // A reference taken before the change still names the old declaration.
    EXPECT_TRUE(detail::sameDeclaration(before, n));
    // The declaration the writer made is no change to the reader, which writes nothing.
    EXPECT_EQ(r.declare(changed), nuclei);

    // A closed reader answers from what it last read.
    ro->close();
    EXPECT_EQ(r.dependencyCount(nuclei), 2);
    EXPECT_TRUE(detail::sameDeclaration(r.definition(nuclei), changed));
}

TEST(StageRegistry, IsUnreadableReportsRestoredNameOnlyStages) {
    Fixture f;
    f.workspace->store()->stageStateWriter().declare("broken", "this is not a record");
    const StageId good = f.stages().declare(writer("good", f.x));
    EXPECT_FALSE(f.stages().isUnreadable(good));
    f.workspace->close();

    {
        WorkspacePtr ro = Workspace::openReadOnly(workspacePath(f).c_str(), Configuration::defaults());
        StageId broken;
        ASSERT_TRUE(ro->stages().find("broken", broken));
        EXPECT_TRUE(ro->stages().isUnreadable(broken));
        EXPECT_FALSE(ro->stages().isUnreadable(good));
        EXPECT_THROW(ro->stages().isUnreadable(StageId(99)), InvalidArgumentError);
        ro->close();
    }

    WorkspacePtr ws = Workspace::open(workspacePath(f).c_str(), Configuration::defaults());
    StageRegistry& r = ws->stages();
    StageId broken;
    ASSERT_TRUE(r.find("broken", broken));
    EXPECT_TRUE(r.isUnreadable(broken));
    EXPECT_FALSE(r.isUnreadable(good));
    EXPECT_THROW(r.isUnreadable(StageId(99)), InvalidArgumentError);

    // Redeclaring the stage with a real definition repairs it.
    r.declare(writer("broken", f.y));
    EXPECT_FALSE(r.isUnreadable(broken));
    ws->close();
}

// A read-only refresh whose store reads as empty (the writer closed between the change
// check and the restore) keeps the snapshot: it never swaps in a state without stages.
TEST(StageRegistry, RefreshNeverShrinksToEmpty) {
    Fixture f;
    const StageId a = f.stages().declare(writer("a", f.x));
    WorkspacePtr ro = Workspace::openReadOnly(workspacePath(f).c_str(), Configuration::defaults());
    StageRegistry& r = ro->stages();
    ASSERT_EQ(r.dependencyCount(a), 0);

    // The writer declares a stage, so the observer's next graph accessor restores, and
    // that restore reads no stages: the snapshot stands, and the accessor answers from it.
    const StageId b = f.stages().declare(writer("b", f.y));
    detail::readNextStoredStageCountAsZeroForTesting();
    EXPECT_EQ(r.dependencyCount(a), 0);
    EXPECT_FALSE(r.isUnreadable(a));
    EXPECT_STREQ(r.definition(a).name(), "a");

    // The next refresh reads the store again and follows the writer.
    EXPECT_EQ(r.dependencyCount(b), 0);
    EXPECT_STREQ(r.definition(b).name(), "b");
    ro->close();
}

// A refresh that finds the stored dependency rows of an unreadable stage empty (a writer
// mid-rewrite) keeps the pins it already held, so a reader of its layers never runs.
TEST(StageRegistry, RefreshKeepsPinsOfAStillUnreadableStage) {
    Fixture f;
    const StageId w = f.stages().declare(writer("w", f.x));
    StageDefinition rd("r");
    rd.readsLayer(f.x).writesLayer(f.y);
    const StageId r = f.stages().declare(rd);
    StageStateWriter& raw = f.workspace->store()->stageStateWriter();
    raw.setConfiguration(w, "this is not a record");

    WorkspacePtr ro = Workspace::openReadOnly(workspacePath(f).c_str(), Configuration::defaults());
    StageRegistry& s = ro->stages();
    ASSERT_TRUE(s.isUnreadable(w));
    ASSERT_EQ(s.dependencyCount(r), 1);

    // The writer is between clearing the rows of r and adding them back, and declares
    // another stage meanwhile.
    raw.clearDependencies(r);
    raw.declare("z", "this is not a record");
    ASSERT_EQ(s.count(), 3);
    ASSERT_EQ(s.dependencyCount(r), 1);
    EXPECT_EQ(s.dependencyAt(r, 0), w);
    EXPECT_TRUE(s.isUnreadable(w));
    EXPECT_FALSE(s.isRunnable(r));
    ro->close();
}

// A refresh never keeps a pin the writer dropped: redeclaring an unreadable stage drops
// every pin around it, so once its record parses the reader drops them too, even while
// the other end is still unreadable (PR #17 finding 3).
TEST(StageRegistry, RefreshDropsPinsOfARedeclaredStage) {
    Fixture f;
    const StageId b = f.stages().declare(writer("b", f.x));
    StageDefinition ad("a");
    ad.readsLayer(f.x).writesLayer(f.y);
    const StageId a = f.stages().declare(ad);
    StageStateWriter& raw = f.workspace->store()->stageStateWriter();
    raw.setConfiguration(a, "this is not a record");
    raw.setConfiguration(b, "this is not a record either");

    WorkspacePtr ro = Workspace::openReadOnly(workspacePath(f).c_str(), Configuration::defaults());
    StageRegistry& s = ro->stages();
    ASSERT_TRUE(s.isUnreadable(a));
    ASSERT_TRUE(s.isUnreadable(b));
    ASSERT_EQ(s.dependencyCount(a), 1);
    ASSERT_EQ(s.dependencyAt(a, 0), b);

    // The writer redeclares a with dependencies that no longer include b; b stays unreadable.
    const StageId redeclared = f.stages().declare(writer("a", f.y));
    ASSERT_EQ(redeclared, a);
    ASSERT_EQ(f.stages().dependencyCount(a), 0);
    EXPECT_FALSE(s.isUnreadable(a));
    EXPECT_TRUE(s.isUnreadable(b));
    EXPECT_EQ(s.dependencyCount(a), 0);
    EXPECT_TRUE(s.isRunnable(a));
    EXPECT_EQ(s.staleness().affectedCount(b), 0);
    ro->close();
}

// markStale and clearStale on a read-only registry refuse with IoError, also for a
// stage the writer declared after the observer opened.
TEST(StageRegistry, ReadOnlyMarkStaleOfALaterStageIsIoError) {
    Fixture f;
    f.stages().declare(writer("a", f.x));
    WorkspacePtr ro = Workspace::openReadOnly(workspacePath(f).c_str(), Configuration::defaults());
    StageRegistry& r = ro->stages();
    const StageId later = f.stages().declare(writer("later", f.y));
    EXPECT_THROW(r.markStale(later), IoError);
    EXPECT_THROW(r.clearStale(later), IoError);
    EXPECT_FALSE(f.stages().isStale(later));
    ro->close();
}

// The read-only refusals name the workspace's store file.
TEST(StageRegistry, ReadOnlyRefusalsNameTheStore) {
    Fixture f;
    const StageId a = f.stages().declare(writer("a", f.x));
    WorkspacePtr ro = Workspace::openReadOnly(workspacePath(f).c_str(), Configuration::defaults());
    StageRegistry& r = ro->stages();
    const std::string store = ro->store()->path();
    const auto refusal = [](const std::function<void()>& action) {
        try {
            action();
        } catch (const IoError& e) {
            return std::string(e.what());
        }
        return std::string("<no IoError>");
    };
    EXPECT_EQ(refusal([&] { r.markStale(a); }), "StageRegistry::markStale: the workspace is read-only (" + store + ")");
    EXPECT_EQ(refusal([&] { r.markAffectedStale(a); }),
              "StageRegistry::markAffectedStale: the workspace is read-only (" + store + ")");
    EXPECT_EQ(refusal([&] { r.clearStale(a); }), "StageRegistry::clearStale: the workspace is read-only (" + store + ")");
    ro->close();
}

namespace {

// The reader registry a hookNextDeclareMidwayForTesting hook queries, the stage, and how
// many times the hook ran.
struct ReaderProbe {
    StageRegistry* registry;
    StageId stage;
    int calls = 0;
};

void queryReader(void* context) {
    auto* probe = static_cast<ReaderProbe*>(context);
    ++probe->calls;
    (void)probe->registry->dependencyCount(probe->stage);
}

void throwInjected(void*) { throw IoError("injected"); }

// Arms the declare hook for its scope and disarms it at the end, so a hook the test did not
// reach never runs in a later test.
class ArmedDeclareHook {
public:
    ArmedDeclareHook(void (*hook)(void*), void* context) { detail::hookNextDeclareMidwayForTesting(hook, context); }
    ~ArmedDeclareHook() { detail::hookNextDeclareMidwayForTesting(nullptr, nullptr); }
    ArmedDeclareHook(const ArmedDeclareHook&) = delete;
    ArmedDeclareHook& operator=(const ArmedDeclareHook&) = delete;
};

} // namespace

// A reader that refreshes while a writer redeclares an unreadable stage sees the old
// state or the new one, never the new configuration with the old rows: the redeclaration
// is one transaction, so the reader keeps no pin the writer dropped.
TEST(StageRegistry, RefreshMidRedeclareKeepsReaderAndWriterInAgreement) {
    Fixture f;
    const StageId u = f.stages().declare(writer("u", f.x));
    StageDefinition xd("x");
    xd.readsLayer(f.x).writesLayer(f.y);
    const StageId x = f.stages().declare(xd);
    ASSERT_EQ(f.workspace->store()->stageStates().dependencyCount(x), 1);
    StageStateWriter& raw = f.workspace->store()->stageStateWriter();
    raw.setConfiguration(u, "not json");
    raw.setConfiguration(x, "not json");
    f.workspace->close();

    const LogCapture log;
    WorkspacePtr ws = Workspace::open(workspacePath(f).c_str(), Configuration::defaults());
    StageRegistry& w = ws->stages();
    ASSERT_TRUE(w.isUnreadable(u));
    ASSERT_TRUE(w.isUnreadable(x));
    ASSERT_EQ(w.dependencyCount(x), 1);
    WorkspacePtr ro = Workspace::openReadOnly(workspacePath(f).c_str(), Configuration::defaults());
    StageRegistry& r = ro->stages();
    ASSERT_EQ(r.dependencyCount(x), 1);
    ASSERT_EQ(r.dependencyAt(x, 0), u);

    ReaderProbe probe{&r, x};
    const ArmedDeclareHook armed(&queryReader, &probe);
    EXPECT_EQ(w.declare(writer("x", f.y)), x);
    EXPECT_EQ(probe.calls, 1);

    EXPECT_EQ(w.dependencyCount(x), 0);
    EXPECT_EQ(r.dependencyCount(x), 0);
    const char* writerName = nullptr;
    const char* readerName = nullptr;
    EXPECT_EQ(r.runBlocker(x, readerName), w.runBlocker(x, writerName));
    ro->close();
    ws->close();
}

// A redeclaration that fails part way leaves the store and the registry as they were:
// no stale flag, no new configuration, no rewritten row.
TEST(StageRegistry, FailedRedeclareChangesNothing) {
    Fixture f;
    StageDefinition ad("a");
    ad.writesLayer(f.x).pipeline(blurSpec(1.0));
    const StageId a = f.stages().declare(ad);
    StageDefinition bd("b");
    bd.readsLayer(f.x).writesLayer(f.y);
    const StageId b = f.stages().declare(bd);
    f.workspace->store()->stageStateWriter().setStatus(a, StageStatus::Complete);
    const std::string before = f.stages().configuration(a);
    const StageStateReader& states = f.workspace->store()->stageStates();
    ASSERT_EQ(states.dependencyCount(b), 1);

    const ArmedDeclareHook armed(&throwInjected, nullptr);
    StageDefinition changed("a");
    changed.writesLayer(f.x).pipeline(blurSpec(2.0));
    bool threw = false;
    try {
        f.stages().declare(changed);
    } catch (const IoError& e) {
        threw = true;
        EXPECT_STREQ(e.what(), "injected");
    }
    EXPECT_TRUE(threw);

    EXPECT_EQ(std::string(f.stages().configuration(a)), before);
    EXPECT_EQ(f.stages().status(a), StageStatus::Complete);
    EXPECT_FALSE(f.stages().isStale(a));
    EXPECT_FALSE(f.stages().isStale(b));
    EXPECT_EQ(states.dependencyCount(a), 0);
    ASSERT_EQ(states.dependencyCount(b), 1);
    EXPECT_EQ(states.dependencyAt(b, 0), a);
    EXPECT_TRUE(detail::sameDeclaration(f.stages().definition(a), ad));
    ASSERT_EQ(f.stages().dependencyCount(b), 1);
    EXPECT_EQ(f.stages().dependencyAt(b, 0), a);
    f.workspace->close();

    WorkspacePtr ws = Workspace::open(workspacePath(f).c_str(), Configuration::defaults());
    StageRegistry& s = ws->stages();
    EXPECT_EQ(std::string(s.configuration(a)), before);
    EXPECT_FALSE(s.isStale(a));
    EXPECT_FALSE(s.isStale(b));
    EXPECT_TRUE(detail::sameDeclaration(s.definition(a), ad));
    ASSERT_EQ(s.dependencyCount(b), 1);
    EXPECT_EQ(s.dependencyAt(b, 0), a);
    ws->close();
}

// A declaration of a new stage that fails part way leaves no stage row behind, so the
// same declaration succeeds afterwards.
TEST(StageRegistry, FailedDeclareOfANewStageLeavesNoRow) {
    Fixture f;
    f.stages().declare(writer("a", f.x));
    const int32_t before = f.stages().count();

    const ArmedDeclareHook armed(&throwInjected, nullptr);
    EXPECT_THROW(f.stages().declare(writer("n", f.y)), IoError);
    EXPECT_EQ(f.stages().count(), before);
    StageId found;
    EXPECT_FALSE(f.stages().find("n", found));
    EXPECT_EQ(f.workspace->layers().writerStageCount(f.y), 0);

    const StageId n = f.stages().declare(writer("n", f.y));
    EXPECT_TRUE(n.isValid());
    EXPECT_EQ(f.stages().count(), before + 1);
    ASSERT_TRUE(f.stages().find("n", found));
    EXPECT_EQ(found, n);
    EXPECT_EQ(f.stages().dependencyCount(n), 0);
    ASSERT_EQ(f.workspace->layers().writerStageCount(f.y), 1);
    EXPECT_EQ(f.workspace->layers().writerStageAt(f.y, 0), n);
}

// An equivalent redeclaration that only reorders dependsOn keeps the held definition, so
// the unresolved positions stay those of the held list and name the undeclared stage.
TEST(StageRegistry, ReorderedDependsOnKeepsTheUnresolvedName) {
    Fixture f;
    f.stages().declare(writer("q", f.x));
    StageDefinition held("s");
    held.dependsOn("p").dependsOn("q").writesLayer(f.y);
    const StageId s = f.stages().declare(held);
    ASSERT_EQ(f.stages().unresolvedDependencyCount(s), 1);
    ASSERT_STREQ(f.stages().unresolvedDependencyAt(s, 0), "p");

    StageDefinition reordered("s");
    reordered.dependsOn("q").dependsOn("p").writesLayer(f.y);
    EXPECT_EQ(f.stages().declare(reordered), s);
    EXPECT_FALSE(f.stages().isStale(s));
    ASSERT_EQ(f.stages().unresolvedDependencyCount(s), 1);
    EXPECT_STREQ(f.stages().unresolvedDependencyAt(s, 0), "p");
    const char* name = nullptr;
    EXPECT_EQ(f.stages().runBlocker(s, name), RunBlocker::UnresolvedDependency);
    EXPECT_STREQ(name, "p");
}

namespace {

// A factory whose operations may vary between runs; a spec may name it without creating it.
class NondeterministicFactory : public OperationFactory {
public:
    explicit NondeterministicFactory(const char* name) : name_(name) {}
    OperationPtr create(const OperationParameters&) const override { return nullptr; }
    const char* name() const override { return name_; }
    ChannelArity arity() const override { return ChannelArity::PerChannel; }
    bool requiresUniformTypes() const override { return false; }
    DataTypeSet supportedTypes() const override { return DataTypeSet(); }
    Halo halo() const override { return Halo::none(); }
    const char* correctnessReference() const override { return "x"; }
    const char* complexityNote() const override { return ""; }
    bool isDeterministic() const override { return false; }

private:
    const char* name_;
};

const char* registerNondeterministic() {
    static const char* const name = "test.RecordsNondeterministicOperations.gpu";
    if (!OperationCatalog::instance().isRegistered(name)) {
        OperationCatalog::instance().registerFactory(std::make_shared<NondeterministicFactory>(name));
    }
    return name;
}

} // namespace

TEST(StageRegistry, RecordsNondeterministicOperations) {
    const char* gpu = registerNondeterministic();
    Fixture f;

    StageDefinition model("model");
    model.writesLayer(f.x);
    PipelineSpec modelSpec = blurSpec(1.0);
    modelSpec.operation(gpu, OperationParameters());
    model.pipeline(modelSpec);
    const StageId modelId = f.stages().declare(model);
    const std::string record = f.stages().configuration(modelId);
    EXPECT_NE(record.find(std::string("\"nondeterministicOperations\":[\"") + gpu + "\"]"), std::string::npos)
        << record;
    EXPECT_FALSE(f.stages().isDeterministic(modelId));

    StageDefinition plain("plain");
    plain.writesLayer(f.y);
    plain.pipeline(blurSpec(1.0));
    const StageId plainId = f.stages().declare(plain);
    EXPECT_EQ(std::string(f.stages().configuration(plainId)).find("nondeterministicOperations"), std::string::npos);
    EXPECT_TRUE(f.stages().isDeterministic(plainId));

    // Redeclaring either is a no-op: nothing is stale, and the records are unchanged.
    EXPECT_EQ(f.stages().declare(model), modelId);
    EXPECT_EQ(f.stages().declare(plain), plainId);
    EXPECT_EQ(std::string(f.stages().configuration(modelId)), record);
    EXPECT_FALSE(f.stages().isStale(modelId));
    EXPECT_FALSE(f.stages().isStale(plainId));
    EXPECT_FALSE(f.stages().isDeterministic(modelId));
    EXPECT_TRUE(f.stages().isDeterministic(plainId));
}
