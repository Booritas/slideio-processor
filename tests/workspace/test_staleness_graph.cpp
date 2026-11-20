// Requirements: UR-STG-8, UR-STG-11, UR-STG-12
#include "workspace/support/test_slide.hpp"

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
#include <slideio/processor/workspace/staleness_graph.hpp>
#include <slideio/processor/workspace/workspace.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace slideio::processor;
using testing_support::TempDirectory;
using testing_support::writeTestSlide;

namespace fs = std::filesystem;

namespace {

// Layers tissue > cells > nuclei (semantic, each the parent of the next), labels beside
// them, and the primitive layer superpixels.
struct Fixture {
    Fixture() {
        fs::create_directories(dir.path());
        const fs::path slide = writeTestSlide(dir.path() / "slide.tif", 300, 200);
        const fs::path ws = dir.path() / "ws";
        workspace = Workspace::create(ws.u8string().c_str(), slide.u8string().c_str(), 0, Configuration::defaults());
        LayerLifecycle& l = workspace->layerLifecycle();
        const LayerId pixel = workspace->layers().pixelLayer();
        tissue = l.declareSemantic("tissue", pixel);
        cells = l.declareSemantic("cells", tissue);
        nuclei = l.declareSemantic("nuclei", cells);
        labels = l.declareSemantic("labels", pixel);
        other = l.declareSemantic("other", pixel);
        superpixels = l.declarePrimitive("superpixels");
        probability = workspace->store()->rasters().declare(RasterDeclaration("probability", DataType::Float32, 1));
    }
    ~Fixture() { workspace->close(); }

    StageRegistry& stages() { return workspace->stages(); }
    std::string path() const { return (dir.path() / "ws").u8string(); }

    TempDirectory dir;
    WorkspacePtr workspace;
    LayerId tissue, cells, nuclei, labels, other, superpixels;
    RasterId probability;
};

using Entry = std::pair<StageId, StalenessReason>;

std::vector<Entry> affected(const StageRegistry& registry, StageId stage) {
    std::vector<Entry> result;
    const StalenessGraph& graph = registry.staleness();
    for (int32_t i = 0; i < graph.affectedCount(stage); ++i) {
        result.emplace_back(graph.affectedAt(stage, i), graph.reasonAt(stage, i));
    }
    return result;
}

bool has(const std::vector<Entry>& entries, StageId stage, StalenessReason reason) {
    return std::count(entries.begin(), entries.end(), Entry(stage, reason)) == 1;
}

int32_t indexOf(const StageRegistry& registry, StageId from, StageId stage, StalenessReason reason) {
    const StalenessGraph& graph = registry.staleness();
    for (int32_t i = 0; i < graph.affectedCount(from); ++i) {
        if (graph.affectedAt(from, i) == stage && graph.reasonAt(from, i) == reason) return i;
    }
    return -1;
}

StageDefinition writerOf(const char* name, LayerId layer) {
    StageDefinition def(name);
    def.writesLayer(layer);
    return def;
}

} // namespace

TEST(StalenessGraph, LayerReadEdge) {
    Fixture f;
    const StageId background = f.stages().declare(writerOf("background", f.tissue));
    StageDefinition c("cells");
    c.restrictToLayer(f.tissue).writesLayer(f.cells);
    const StageId cells = f.stages().declare(c);

    const std::vector<Entry> list = affected(f.stages(), background);
    EXPECT_TRUE(has(list, cells, StalenessReason::LayerRead));
    const int32_t at = indexOf(f.stages(), background, cells, StalenessReason::LayerRead);
    ASSERT_GE(at, 0);
    const std::string detail = f.stages().staleness().reasonDetailAt(background, at);
    EXPECT_EQ(detail, "reads layer \"tissue\" written by stage \"background\"");
    EXPECT_TRUE(affected(f.stages(), cells).empty());
}

TEST(StalenessGraph, RasterReadEdge) {
    Fixture f;
    StageDefinition prob("prob");
    prob.writesRaster(f.probability);
    const StageId probId = f.stages().declare(prob);
    StageDefinition use("use");
    use.readsRaster("prob", f.probability).writesLayer(f.labels);
    const StageId useId = f.stages().declare(use);

    const std::vector<Entry> list = affected(f.stages(), probId);
    ASSERT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0], Entry(useId, StalenessReason::RasterRead));
    EXPECT_STREQ(f.stages().staleness().reasonDetailAt(probId, 0),
                 "reads raster \"probability\" written by stage \"prob\"");
}

TEST(StalenessGraph, LayerOrderingReachesUndeclaredDependents) {
    Fixture f;
    const StageId background = f.stages().declare(writerOf("background", f.tissue));
    const StageId nuclei = f.stages().declare(writerOf("nuclei", f.nuclei));
    EXPECT_EQ(f.stages().dependencyCount(nuclei), 0);

    const std::vector<Entry> list = affected(f.stages(), background);
    ASSERT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0], Entry(nuclei, StalenessReason::LayerOrdering));
    EXPECT_STREQ(f.stages().staleness().reasonDetailAt(background, 0),
                 "writes layer \"nuclei\" above layer \"tissue\" written by stage \"background\"");
    // The ordering runs one way only.
    EXPECT_TRUE(affected(f.stages(), nuclei).empty());
}

TEST(StalenessGraph, DerivedFromReplacesLayerReadForPrimitives) {
    Fixture f;
    const StageId sp = f.stages().declare(writerOf("sp", f.superpixels));
    StageDefinition c("cells");
    c.readsLayer(f.superpixels).writesLayer(f.cells);
    const StageId cells = f.stages().declare(c);

    const std::vector<Entry> list = affected(f.stages(), sp);
    ASSERT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0], Entry(cells, StalenessReason::DerivedFrom));
    EXPECT_NE(std::string(f.stages().staleness().reasonDetailAt(sp, 0)).find("\"superpixels\""), std::string::npos);

    // A reader that writes no semantic layer derives nothing: it reads the layer.
    StageDefinition probe("probe");
    probe.readsLayer(f.superpixels);
    const StageId probeId = f.stages().declare(probe);
    EXPECT_TRUE(has(affected(f.stages(), sp), probeId, StalenessReason::LayerRead));
}

TEST(StalenessGraph, ExplicitDependencyIsStageDependency) {
    Fixture f;
    const StageId a = f.stages().declare(writerOf("a", f.labels));
    StageDefinition b("b");
    b.writesLayer(f.other).dependsOn("a");
    const StageId bId = f.stages().declare(b);

    const std::vector<Entry> list = affected(f.stages(), a);
    ASSERT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0], Entry(bId, StalenessReason::StageDependency));
    EXPECT_STREQ(f.stages().staleness().reasonDetailAt(a, 0), "depends on stage \"a\"");
}

TEST(StalenessGraph, TransitiveAndMultiPathEntries) {
    Fixture f;
    const StageId background = f.stages().declare(writerOf("background", f.tissue));
    StageDefinition c("cells");
    c.readsLayer(f.tissue).writesLayer(f.cells);
    const StageId cells = f.stages().declare(c);
    StageDefinition n("nuclei");
    n.readsLayer(f.cells).writesLayer(f.nuclei);
    const StageId nuclei = f.stages().declare(n);

    // Breadth first: the edges of background (by ascending stage, then reason), then
    // those of cells. cells and nuclei are each reached by two reasons.
    const std::vector<Entry> list = affected(f.stages(), background);
    const std::vector<Entry> expected = {{cells, StalenessReason::LayerRead},
                                         {cells, StalenessReason::LayerOrdering},
                                         {nuclei, StalenessReason::LayerOrdering},
                                         {nuclei, StalenessReason::LayerRead},
                                         {nuclei, StalenessReason::LayerOrdering}};
    EXPECT_EQ(list, expected);
    // The stage two hops away carries the reason of its own edge.
    EXPECT_STREQ(f.stages().staleness().reasonDetailAt(background, 3),
                 "reads layer \"cells\" written by stage \"cells\"");
}

TEST(StalenessGraph, LayerOrderingThatWouldCloseACycleIsSuppressed) {
    Fixture f;
    // cells reads nuclei, a layer above the one it writes; nucStage writes nuclei.
    StageDefinition c("cells");
    c.readsLayer(f.nuclei).writesLayer(f.cells);
    const StageId cells = f.stages().declare(c);
    const StageId nucStage = f.stages().declare(writerOf("nucStage", f.nuclei));

    // The effective graph: cells depends on nucStage and not the reverse.
    EXPECT_EQ(f.stages().dependencyCount(cells), 1);
    EXPECT_EQ(f.stages().dependencyCount(nucStage), 0);

    // nucStage reaches cells by LayerRead. The LayerOrdering edge cells to nucStage would
    // close a cycle, so it is suppressed: re-running cells invalidates nothing.
    const std::vector<Entry> fromNuclei = affected(f.stages(), nucStage);
    ASSERT_EQ(fromNuclei.size(), 1u);
    EXPECT_EQ(fromNuclei[0], Entry(cells, StalenessReason::LayerRead));
    EXPECT_TRUE(affected(f.stages(), cells).empty());

    f.stages().markStale(cells);
    EXPECT_TRUE(f.stages().isStale(cells));
    EXPECT_FALSE(f.stages().isStale(nucStage));
    f.stages().clearStale(cells);
    f.stages().markStale(nucStage);
    EXPECT_TRUE(f.stages().isStale(nucStage));
    EXPECT_TRUE(f.stages().isStale(cells));
}

// The suppression does not depend on the declaration order: with the writer of nuclei
// declared first, the LayerOrdering edge from cells is still the one left out.
TEST(StalenessGraph, CycleSuppressionIsOrderIndependent) {
    Fixture f;
    const StageId nucStage = f.stages().declare(writerOf("nucStage", f.nuclei));
    StageDefinition c("cells");
    c.readsLayer(f.nuclei).writesLayer(f.cells);
    const StageId cells = f.stages().declare(c);

    const std::vector<Entry> fromNuclei = affected(f.stages(), nucStage);
    ASSERT_EQ(fromNuclei.size(), 1u);
    EXPECT_EQ(fromNuclei[0], Entry(cells, StalenessReason::LayerRead));
    EXPECT_TRUE(affected(f.stages(), cells).empty());

    // A tissue writer still reaches both by LayerOrdering (UR-STG-11).
    const StageId background = f.stages().declare(writerOf("background", f.tissue));
    const std::vector<Entry> fromBackground = affected(f.stages(), background);
    EXPECT_TRUE(has(fromBackground, nucStage, StalenessReason::LayerOrdering));
    EXPECT_TRUE(has(fromBackground, cells, StalenessReason::LayerOrdering));
}

TEST(StalenessGraph, MarkStaleFollowsLayerOrdering) {
    Fixture f;
    const StageId background = f.stages().declare(writerOf("background", f.tissue));
    const StageId nuclei = f.stages().declare(writerOf("nuclei", f.nuclei));
    const StageId unrelated = f.stages().declare(writerOf("unrelated", f.labels));

    f.stages().markStale(background);
    EXPECT_TRUE(f.stages().isStale(background));
    EXPECT_TRUE(f.stages().isStale(nuclei));
    EXPECT_FALSE(f.stages().isStale(unrelated));
}

TEST(StalenessGraph, UnknownStageAndIndexAreRejected) {
    Fixture f;
    const StageId background = f.stages().declare(writerOf("background", f.tissue));
    const StalenessGraph& graph = f.stages().staleness();
    EXPECT_THROW(graph.affectedCount(StageId(9999)), InvalidArgumentError);
    EXPECT_THROW(graph.affectedAt(StageId(9999), 0), InvalidArgumentError);
    EXPECT_THROW(graph.reasonAt(background, 0), InvalidArgumentError);
    EXPECT_THROW(graph.reasonDetailAt(background, -1), InvalidArgumentError);
}

TEST(StalenessGraph, ReferenceSurvivesDeclareAndSeesRedeclaration) {
    Fixture f;
    const StalenessGraph& graph = f.stages().staleness();
    const StageId background = f.stages().declare(writerOf("background", f.tissue));
    EXPECT_EQ(graph.affectedCount(background), 0);
    const StageId nuclei = f.stages().declare(writerOf("nuclei", f.nuclei));
    EXPECT_EQ(graph.affectedCount(background), 1);
    // The writer a redeclaration dropped no longer reaches anything.
    f.stages().declare(writerOf("nuclei", f.other));
    EXPECT_EQ(graph.affectedCount(background), 0);
    (void)nuclei;
}

TEST(StalenessGraph, ReadOnlyViewFollowsLiveWriter) {
    Fixture f;
    const StageId background = f.stages().declare(writerOf("background", f.tissue));
    WorkspacePtr ro = Workspace::openReadOnly(f.path().c_str(), Configuration::defaults());
    const StalenessGraph& graph = ro->stages().staleness();
    ASSERT_EQ(graph.affectedCount(background), 0);

    const StageId nuclei = f.stages().declare(writerOf("nuclei", f.nuclei));
    ASSERT_EQ(graph.affectedCount(background), 1);
    EXPECT_EQ(graph.affectedAt(background, 0), nuclei);
    EXPECT_EQ(graph.reasonAt(background, 0), StalenessReason::LayerOrdering);
    EXPECT_STREQ(graph.reasonDetailAt(background, 0),
                 "writes layer \"nuclei\" above layer \"tissue\" written by stage \"background\"");
}

TEST(StalenessGraph, DerivedFromWinsPerPair) {
    Fixture f;
    StageDefinition sp("sp");
    sp.writesLayer(f.superpixels).writesLayer(f.labels);
    const StageId spId = f.stages().declare(sp);
    StageDefinition c("cells");
    c.readsLayer(f.superpixels).readsLayer(f.labels).writesLayer(f.cells);
    const StageId cells = f.stages().declare(c);

    // V reads a primitive and a semantic layer U writes: only DerivedFrom is reported.
    const std::vector<Entry> list = affected(f.stages(), spId);
    ASSERT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0], Entry(cells, StalenessReason::DerivedFrom));
}

TEST(StalenessGraph, PinnedEdgesAreStageDependency) {
    Fixture f;
    const StageId w = f.stages().declare(writerOf("w", f.labels));
    StageDefinition rd("r");
    rd.readsLayer(f.labels).writesLayer(f.other);
    const StageId r = f.stages().declare(rd);
    EXPECT_TRUE(has(affected(f.stages(), w), r, StalenessReason::LayerRead));
    f.workspace->store()->stageStateWriter().setConfiguration(w, "this is not a record");
    f.workspace->close();

    // The unreadable writer has an empty definition, so only the pinned edge reaches r.
    WorkspacePtr ws = Workspace::open(f.path().c_str(), Configuration::defaults());
    ASSERT_TRUE(ws->stages().isUnreadable(w));
    const std::vector<Entry> list = affected(ws->stages(), w);
    ASSERT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0], Entry(r, StalenessReason::StageDependency));
    EXPECT_STREQ(ws->stages().staleness().reasonDetailAt(w, 0), "depends on stage \"w\"");
    ws->stages().markStale(w);
    EXPECT_TRUE(ws->stages().isStale(r));
    ws->close();
}

namespace {

// Marks a stage Complete a few milliseconds after the previous completion, so the
// completion times of two stages are never equal.
void complete(Fixture& f, StageId stage) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    f.workspace->store()->stageStateWriter().setStatus(stage, StageStatus::Complete);
}

// What StageRunner does for a re-run of the stage: its dependents go stale, it runs, it completes.
void rerun(Fixture& f, StageId stage) {
    f.stages().markAffectedStale(stage);
    f.workspace->store()->stageStateWriter().setStatus(stage, StageStatus::Running);
    complete(f, stage);
}

struct Cause {
    StageId stage;
    StalenessReason reason;
    std::string detail;
    bool operator==(const Cause& other) const {
        return stage == other.stage && reason == other.reason && detail == other.detail;
    }
};

std::vector<Cause> causesOf(const StageRegistry& registry, StageId stage) {
    std::vector<Cause> result;
    const StalenessGraph& graph = registry.staleness();
    for (int32_t i = 0; i < graph.causeCount(stage); ++i) {
        result.push_back({graph.causeAt(stage, i), graph.causeReasonAt(stage, i), graph.causeReasonDetailAt(stage, i)});
    }
    return result;
}

} // namespace

TEST(StalenessGraph, CausesNameTheChangedUpstream) {
    Fixture f;
    const StageId background = f.stages().declare(writerOf("background", f.tissue));
    StageDefinition c("cells");
    c.restrictToLayer(f.tissue).writesLayer(f.cells);
    const StageId cells = f.stages().declare(c);
    complete(f, background);
    complete(f, cells);
    EXPECT_EQ(f.stages().staleness().causeCount(cells), 0);

    rerun(f, background);
    ASSERT_TRUE(f.stages().isStale(cells));
    const std::vector<Cause> causes = causesOf(f.stages(), cells);
    // background reaches cells by LayerRead (it reads tissue) and by LayerOrdering (cells is below tissue).
    ASSERT_EQ(causes.size(), 2u);
    EXPECT_EQ(causes[0].stage, background);
    EXPECT_EQ(causes[0].reason, StalenessReason::LayerRead);
    EXPECT_EQ(causes[0].detail, "reads layer \"tissue\" written by stage \"background\"");
    EXPECT_EQ(causes[1].stage, background);
    EXPECT_EQ(causes[1].reason, StalenessReason::LayerOrdering);
}

TEST(StalenessGraph, CausesAreTheSameAfterReopen) {
    Fixture f;
    const StageId background = f.stages().declare(writerOf("background", f.tissue));
    StageDefinition c("cells");
    c.restrictToLayer(f.tissue).writesLayer(f.cells);
    const StageId cells = f.stages().declare(c);
    const StageId nuclei = f.stages().declare(writerOf("nuclei", f.nuclei));
    complete(f, background);
    complete(f, cells);
    complete(f, nuclei);
    rerun(f, background);
    ASSERT_TRUE(f.stages().isStale(nuclei));

    const std::vector<Cause> nucleiBefore = causesOf(f.stages(), nuclei);
    const std::vector<Cause> cellsBefore = causesOf(f.stages(), cells);
    ASSERT_FALSE(nucleiBefore.empty());
    ASSERT_EQ(cellsBefore.size(), 2u);
    EXPECT_EQ(cellsBefore[1].stage, background);
    EXPECT_EQ(cellsBefore[1].reason, StalenessReason::LayerOrdering);
    // nuclei is reached from background, which completed after it, by LayerOrdering, and
    // from cells by LayerOrdering. cells completed before nuclei and is Complete, so it is
    // a cause only because it is stale.
    const std::vector<Cause> nucleiExpected = {
        {background, StalenessReason::LayerOrdering,
         "writes layer \"nuclei\" above layer \"tissue\" written by stage \"background\""},
        {cells, StalenessReason::LayerOrdering,
         "writes layer \"nuclei\" above layer \"cells\" written by stage \"cells\""}};
    EXPECT_EQ(nucleiBefore, nucleiExpected);
    ASSERT_LT(f.stages().completedAtUnixMs(cells), f.stages().completedAtUnixMs(nuclei));
    ASSERT_EQ(f.stages().status(cells), StageStatus::Complete);

    f.workspace->close();
    WorkspacePtr ro = Workspace::openReadOnly(f.path().c_str(), Configuration::defaults());
    EXPECT_EQ(causesOf(ro->stages(), nuclei), nucleiBefore);
    EXPECT_EQ(causesOf(ro->stages(), cells), cellsBefore);
    const std::vector<Cause> cellsAfter = causesOf(ro->stages(), cells);
    ASSERT_EQ(cellsAfter.size(), 2u);
    EXPECT_EQ(cellsAfter[1].reason, StalenessReason::LayerOrdering);
    ro->close();
    f.workspace = Workspace::open(f.path().c_str(), Configuration::defaults());
}

// A source whose re-run did not complete is a cause even though it completed, last,
// before the dependent did: it is not Complete.
TEST(StalenessGraph, SourceLeftIncompleteIsACause) {
    for (const StageStatus left : {StageStatus::Failed, StageStatus::Running}) {
        Fixture f;
        const StageId background = f.stages().declare(writerOf("background", f.tissue));
        StageDefinition c("cells");
        c.restrictToLayer(f.tissue).writesLayer(f.cells);
        const StageId cells = f.stages().declare(c);
        complete(f, background);
        complete(f, cells);

        // The re-run of background marks cells stale and is left Failed or Running: its
        // completion time is the old one, before cells, and it is not stale itself.
        f.stages().markAffectedStale(background);
        f.workspace->store()->stageStateWriter().setStatus(background, left);
        ASSERT_TRUE(f.stages().isStale(cells));
        ASSERT_FALSE(f.stages().isStale(background));
        ASSERT_LT(f.stages().completedAtUnixMs(background), f.stages().completedAtUnixMs(cells));

        const std::vector<Cause> causes = causesOf(f.stages(), cells);
        ASSERT_EQ(causes.size(), 2u);
        EXPECT_EQ(causes[0].stage, background);
        EXPECT_EQ(causes[0].reason, StalenessReason::LayerRead);
        EXPECT_EQ(causes[0].detail, "reads layer \"tissue\" written by stage \"background\"");
        EXPECT_EQ(causes[1].stage, background);
        EXPECT_EQ(causes[1].reason, StalenessReason::LayerOrdering);
    }
}

// A detail string stays valid for the life of the registry: an identical redeclare and a
// new declaration rebuild the edges, and the string held across them is still the one
// the graph returns.
TEST(StalenessGraph, DetailSurvivesRebuilds) {
    Fixture f;
    const StageId background = f.stages().declare(writerOf("background", f.tissue));
    StageDefinition c("cells");
    c.readsLayer(f.tissue).writesLayer(f.cells);
    const StageId cells = f.stages().declare(c);
    const StalenessGraph& graph = f.stages().staleness();
    const int32_t at = indexOf(f.stages(), background, cells, StalenessReason::LayerRead);
    ASSERT_GE(at, 0);
    const char* held = graph.reasonDetailAt(background, at);

    ASSERT_EQ(f.stages().declare(c), cells);  // identical: rebuilds the edges
    f.stages().declare(writerOf("other", f.other));  // rebuilds them again
    EXPECT_STREQ(held, "reads layer \"tissue\" written by stage \"background\"");
    // The same interned storage is handed out again.
    EXPECT_EQ(held, graph.reasonDetailAt(background, indexOf(f.stages(), background, cells,
                                                              StalenessReason::LayerRead)));
}

TEST(StalenessGraph, ReconfiguredStageIsItsOwnCause) {
    Fixture f;
    const StageId background = f.stages().declare(writerOf("background", f.tissue));
    StageDefinition c("cells");
    c.restrictToLayer(f.tissue).writesLayer(f.cells);
    const StageId cells = f.stages().declare(c);
    complete(f, background);
    complete(f, cells);

    StageDefinition changed("cells");
    changed.restrictToLayer(f.tissue).writesLayer(f.cells).writesLayer(f.labels);
    ASSERT_EQ(f.stages().declare(changed), cells);
    ASSERT_TRUE(f.stages().isStale(cells));
    const std::vector<Cause> causes = causesOf(f.stages(), cells);
    ASSERT_EQ(causes.size(), 1u);
    EXPECT_EQ(causes[0].stage, cells);
    EXPECT_EQ(causes[0].reason, StalenessReason::Reconfigured);
    EXPECT_EQ(causes[0].detail, "the stage was redeclared or marked stale directly");
}

TEST(StalenessGraph, CurrentStageHasNoCauses) {
    Fixture f;
    const StageId background = f.stages().declare(writerOf("background", f.tissue));
    complete(f, background);
    EXPECT_EQ(f.stages().staleness().causeCount(background), 0);
    EXPECT_THROW(f.stages().staleness().causeAt(background, 0), InvalidArgumentError);
    EXPECT_THROW(f.stages().staleness().causeCount(StageId(9999)), InvalidArgumentError);
}

TEST(StalenessGraph, DroppingAPrimitiveLayerMarksNothing) {
    Fixture f;
    const StageId sp = f.stages().declare(writerOf("sp", f.superpixels));
    StageDefinition c("cells");
    c.readsLayer(f.superpixels).writesLayer(f.cells);
    const StageId cells = f.stages().declare(c);
    complete(f, sp);
    complete(f, cells);

    f.workspace->layerLifecycle().drop(f.superpixels);
    EXPECT_FALSE(f.stages().isStale(sp));
    EXPECT_FALSE(f.stages().isStale(cells));
    EXPECT_EQ(f.stages().staleness().causeCount(cells), 0);
}
