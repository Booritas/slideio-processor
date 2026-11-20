// Requirements: UR-STG-8, UR-STG-11
#include "workspace/support/test_slide.hpp"

#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/data_type.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/storage/layer_registry.hpp>
#include <slideio/processor/storage/raster_store.hpp>
#include <slideio/processor/storage/stage_state_writer.hpp>
#include <slideio/processor/storage/store.hpp>
#include <slideio/processor/workspace/stage_definition.hpp>
#include <slideio/processor/workspace/stage_registry.hpp>
#include <slideio/processor/workspace/staleness_graph.hpp>
#include <slideio/processor/workspace/workspace.hpp>

#include <gtest/gtest.h>

#include <algorithm>

#include <cstdint>
#include <filesystem>
#include <string>
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
                 "writes layer \"nuclei\" below layer \"tissue\" written by stage \"background\"");
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

TEST(StalenessGraph, CycleThroughLayerOrderingTerminates) {
    Fixture f;
    StageDefinition c("cells");
    c.readsLayer(f.nuclei).writesLayer(f.cells);
    const StageId cells = f.stages().declare(c);
    const StageId nucStage = f.stages().declare(writerOf("nucStage", f.nuclei));

    // The effective graph stays acyclic: cells depends on nucStage and not the reverse.
    EXPECT_EQ(f.stages().dependencyCount(cells), 1);
    EXPECT_EQ(f.stages().dependencyCount(nucStage), 0);

    const std::vector<Entry> fromCells = affected(f.stages(), cells);
    EXPECT_TRUE(has(fromCells, nucStage, StalenessReason::LayerOrdering));
    EXPECT_TRUE(has(affected(f.stages(), nucStage), cells, StalenessReason::LayerRead));
    // The cycle leads back to the stage itself, once.
    EXPECT_TRUE(has(fromCells, cells, StalenessReason::LayerRead));
    EXPECT_EQ(fromCells.size(), 2u);

    f.stages().markStale(cells);
    EXPECT_TRUE(f.stages().isStale(cells));
    EXPECT_TRUE(f.stages().isStale(nucStage));
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
                 "writes layer \"nuclei\" below layer \"tissue\" written by stage \"background\"");
}
