// Requirements: UR-SEG-8, UR-IMG-4, UR-STG-10
#include "workspace/support/aux_probe.hpp"
#include "workspace/support/test_slide.hpp"

#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/pipeline/operation_parameters.hpp>
#include <slideio/processor/pipeline/pipeline_spec.hpp>
#include <slideio/processor/workspace/coarse_to_fine.hpp>
#include <slideio/processor/workspace/stage_definition.hpp>
#include <slideio/processor/workspace/stage_registry.hpp>
#include <slideio/processor/workspace/stage_runner.hpp>
#include <slideio/processor/workspace/staleness_graph.hpp>
#include <slideio/processor/workspace/workspace.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>

using namespace slideio::processor;
using testing_support::TempDirectory;
using testing_support::writeBlobSlide;

namespace fs = std::filesystem;

namespace {

// A blob slide with a three-level pyramid, and the two layers a pair writes.
struct Fixture {
    Fixture() {
        fs::create_directories(dir.path());
        const fs::path slide = writeBlobSlide(dir.path() / "slide.svs", 600, 600, 3);
        workspacePath = (dir.path() / "ws").u8string();
        workspace = Workspace::create(workspacePath.c_str(), slide.u8string().c_str(), 0, Configuration::defaults());
        LayerLifecycle& l = workspace->layerLifecycle();
        nestsCoarse = l.declareSemantic("nests_coarse", workspace->layers().pixelLayer());
        nests = l.declareSemantic("nests", workspace->layers().pixelLayer());
    }
    ~Fixture() { workspace->close(); }

    StageRegistry& stages() { return workspace->stages(); }

    TempDirectory dir;
    std::string workspacePath;
    WorkspacePtr workspace;
    LayerId nestsCoarse, nests;
};

PipelineSpec blobChain(int32_t level) {
    PipelineSpec spec;
    spec.operation("pixel.colour_convert", OperationParameters().setString("from", "Rgb").setString("to", "Grey"));
    spec.operation("pixel.threshold_fixed",
                   OperationParameters().setDouble("value", 128.0).setString("direction", "Below"));
    spec.operation("segment.connected_components", OperationParameters());
    spec.analysisLevel(level);
    return spec;
}

PipelineSpec probeChain(const char* binding, int32_t level) {
    PipelineSpec spec;
    spec.operation("test.ws.auxProbe", OperationParameters().setString("binding", binding));
    spec.analysisLevel(level);
    return spec;
}

CoarseToFineSpec pair(Fixture& f, int32_t coarseLevel, int32_t fineLevel) {
    CoarseToFineSpec spec("nests_coarse", "nests");
    spec.coarse(blobChain(coarseLevel), f.nestsCoarse).fine(probeChain("coarse", fineLevel), f.nests);
    return spec;
}

// The message of the StageError declaring the pair throws; empty when none.
std::string refusal(Fixture& f, const CoarseToFineSpec& spec) {
    StageId c, fi;
    try {
        declareCoarseToFine(f.stages(), spec, c, fi);
    } catch (const StageError& e) {
        return e.what();
    }
    return "";
}

} // namespace

TEST(CoarseToFine, DeclaresTwoWiredStages) {
    Fixture f;
    StageId coarse, fine;
    declareCoarseToFine(f.stages(), pair(f, 2, 0), coarse, fine);
    ASSERT_TRUE(coarse.isValid());
    ASSERT_TRUE(fine.isValid());
    EXPECT_EQ(f.stages().count(), 2);
    EXPECT_STREQ(f.stages().name(coarse), "nests_coarse");
    EXPECT_STREQ(f.stages().name(fine), "nests");

    const StageDefinition& coarseDef = f.stages().definition(coarse);
    ASSERT_EQ(coarseDef.writtenLayerCount(), 1);
    EXPECT_EQ(coarseDef.writtenLayerAt(0), f.nestsCoarse);
    EXPECT_EQ(coarseDef.pipeline().analysisLevel(), 2);

    const StageDefinition& def = f.stages().definition(fine);
    ASSERT_EQ(def.writtenLayerCount(), 1);
    EXPECT_EQ(def.writtenLayerAt(0), f.nests);
    ASSERT_EQ(def.rasterLayerCount(), 1);
    EXPECT_STREQ(def.rasterBindingAt(0), "coarse");
    EXPECT_EQ(def.rasterContentAt(0), LayerRasterContent::ObjectAnchor);
    EXPECT_EQ(def.rasterLayerAt(0), f.nestsCoarse);
    ASSERT_TRUE(def.hasLayerRestriction());
    EXPECT_EQ(def.layerRestriction(), f.nestsCoarse);

    ASSERT_EQ(f.stages().dependencyCount(fine), 1);
    EXPECT_EQ(f.stages().dependencyAt(fine, 0), coarse);
    EXPECT_FALSE(f.stages().isExplicitDependency(fine, 0));
    EXPECT_EQ(f.stages().dependencyCount(coarse), 0);

    const StalenessGraph& graph = f.stages().staleness();
    ASSERT_EQ(graph.affectedCount(coarse), 1);
    EXPECT_EQ(graph.affectedAt(coarse, 0), fine);
    EXPECT_EQ(graph.reasonAt(coarse, 0), StalenessReason::LayerRead);
}

TEST(CoarseToFine, BindingAndRestrictionCanBeChanged) {
    Fixture f;
    CoarseToFineSpec spec = pair(f, 2, 0);
    spec.binding("prob", LayerRasterContent::Membership).restrictFineToCoarse(false);
    StageId coarse, fine;
    declareCoarseToFine(f.stages(), spec, coarse, fine);

    const StageDefinition& def = f.stages().definition(fine);
    ASSERT_EQ(def.rasterLayerCount(), 1);
    EXPECT_STREQ(def.rasterBindingAt(0), "prob");
    EXPECT_EQ(def.rasterContentAt(0), LayerRasterContent::Membership);
    EXPECT_EQ(def.rasterLayerAt(0), f.nestsCoarse);
    EXPECT_FALSE(def.hasLayerRestriction());
    // The dependency comes from the raster read alone.
    ASSERT_EQ(f.stages().dependencyCount(fine), 1);
    EXPECT_EQ(f.stages().dependencyAt(fine, 0), coarse);
}

TEST(CoarseToFine, CopyAndAssignmentAreIndependent) {
    Fixture f;
    CoarseToFineSpec original = pair(f, 2, 0);
    CoarseToFineSpec copy(original);
    CoarseToFineSpec assigned("other_coarse", "other_fine");
    assigned = original;
    original.binding("changed", LayerRasterContent::Label);
    StageId coarse, fine;
    declareCoarseToFine(f.stages(), assigned, coarse, fine);
    EXPECT_STREQ(f.stages().name(coarse), "nests_coarse");
    EXPECT_STREQ(f.stages().definition(fine).rasterBindingAt(0), "coarse");
    EXPECT_EQ(refusal(f, copy), "") << "redeclaring the same pair is a no-op";
    EXPECT_EQ(f.stages().count(), 2);
}

TEST(CoarseToFine, LevelsMustBeStrictlyOrdered) {
    Fixture f;
    std::string message = refusal(f, pair(f, 2, 2));
    EXPECT_NE(message.find("levels"), std::string::npos) << message;
    EXPECT_EQ(f.stages().count(), 0);
    message = refusal(f, pair(f, 1, 3));
    EXPECT_NE(message.find("levels"), std::string::npos) << message;
    EXPECT_EQ(f.stages().count(), 0);

    // An automatic fine level is accepted.
    StageId coarse, fine;
    declareCoarseToFine(f.stages(), pair(f, 2, -1), coarse, fine);
    EXPECT_EQ(f.stages().count(), 2);
    EXPECT_EQ(f.stages().definition(fine).pipeline().analysisLevel(), -1);
}

TEST(CoarseToFine, SameNamesOrLayersAreRefused) {
    Fixture f;
    CoarseToFineSpec names("nests", "nests");
    names.coarse(blobChain(2), f.nestsCoarse).fine(probeChain("coarse", 0), f.nests);
    std::string message = refusal(f, names);
    EXPECT_NE(message.find("names"), std::string::npos) << message;
    EXPECT_EQ(f.stages().count(), 0);

    CoarseToFineSpec layers("nests_coarse", "nests");
    layers.coarse(blobChain(2), f.nests).fine(probeChain("coarse", 0), f.nests);
    message = refusal(f, layers);
    EXPECT_NE(message.find("layers"), std::string::npos) << message;
    EXPECT_EQ(f.stages().count(), 0);
}

TEST(CoarseToFine, MissingHalfIsRefused) {
    Fixture f;
    CoarseToFineSpec noCoarse("nests_coarse", "nests");
    noCoarse.fine(probeChain("coarse", 0), f.nests);
    std::string message = refusal(f, noCoarse);
    EXPECT_NE(message.find("coarse"), std::string::npos) << message;
    EXPECT_EQ(f.stages().count(), 0);

    CoarseToFineSpec noFine("nests_coarse", "nests");
    noFine.coarse(blobChain(2), f.nestsCoarse);
    message = refusal(f, noFine);
    EXPECT_NE(message.find("fine"), std::string::npos) << message;
    EXPECT_EQ(f.stages().count(), 0);
}

TEST(CoarseToFine, FailedFineDeclareLeavesTheCoarseStageDeclared) {
    Fixture f;
    // An unknown layer id passes the helper's checks and fails the fine declare.
    CoarseToFineSpec spec("nests_coarse", "nests");
    spec.coarse(blobChain(2), f.nestsCoarse).fine(probeChain("coarse", 0), LayerId(9999));
    StageId coarse, fine;
    EXPECT_THROW(declareCoarseToFine(f.stages(), spec, coarse, fine), InvalidArgumentError);
    EXPECT_FALSE(coarse.isValid()) << "outputs are untouched on failure";
    EXPECT_FALSE(fine.isValid());
    EXPECT_EQ(f.stages().count(), 1);
    StageId found;
    EXPECT_TRUE(f.stages().find("nests_coarse", found));
}

TEST(CoarseToFine, PairRunsEndToEnd) {
    testing_support::registerAuxProbe();
    testing_support::resetAuxProbe();
    Fixture f;
    StageId coarse, fine;
    declareCoarseToFine(f.stages(), pair(f, 2, 0), coarse, fine);

    StageRunner(*f.workspace, coarse).run();
    ASSERT_EQ(f.stages().status(coarse), StageStatus::Complete);
    StageRunner(*f.workspace, fine).run();
    EXPECT_EQ(f.stages().status(fine), StageStatus::Complete);
    EXPECT_FALSE(f.stages().isStale(fine));
    EXPECT_GT(testing_support::auxProbeTiles("coarse"), 0);

    StageRunner(*f.workspace, coarse).run();
    EXPECT_TRUE(f.stages().isStale(fine));
}
