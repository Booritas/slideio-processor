// Requirements: UR-IMG-11, UR-ALG-3, UR-ALG-4, UR-EXE-1, UR-IMG-5, UR-STG-3
#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/memory_budget.hpp>
#include <slideio/processor/pipeline/pipeline.hpp>

#include <pipeline/detail/pipeline_impl.hpp>
#include <pipeline/detail/run_plan.hpp>
#include <pipeline/test_support/synthetic_scene.hpp>
#include <pipeline/test_support/test_operations.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace slideio::processor;
namespace ts = slideio::processor::testing_support;

namespace {

double flat(int, int64_t, int64_t) { return 0.0; }

std::shared_ptr<ts::SyntheticSource> sourceOf(std::vector<ts::SyntheticLevel> levels,
                                              std::vector<slideio::DataType> types) {
    ts::registerTestOperations();
    return std::make_shared<ts::SyntheticSource>(std::move(levels), std::move(types), flat);
}

std::shared_ptr<ts::SyntheticSource> byteSource(int64_t width = 1000, int64_t height = 800) {
    return sourceOf({{width, height, 0, 0}}, {slideio::DataType::DT_Byte});
}

// Three levels, 1000, 500 and 250 pixels square.
std::shared_ptr<ts::SyntheticSource> pyramid(std::vector<slideio::DataType> types = {slideio::DataType::DT_Byte}) {
    return sourceOf({{1000, 1000, 0, 0}, {500, 500, 0, 0}, {250, 250, 0, 0}}, std::move(types));
}

Configuration config(int32_t tile = 256) { return ConfigurationBuilder().tileSize(tile, tile).build(); }

PipelineSpec specOf(const char* operation, const OperationParameters& parameters = OperationParameters()) {
    PipelineSpec spec;
    spec.operation(operation, parameters);
    return spec;
}

// Runs validate() and returns the message of the error it throws, or "" when it does not.
template <typename Error>
std::string validateMessage(const Pipeline& pipeline) {
    try {
        pipeline.validate();
    } catch (const Error& e) {
        return e.what();
    }
    return "";
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

const detail::RunPlan& planOf(const Pipeline& pipeline) { return detail::PipelineAccess::plan(pipeline); }

} // namespace

TEST(PipelineValidate, NullSceneIsRefusedAtConstruction) {
    ts::registerTestOperations();
    EXPECT_THROW(Pipeline(nullptr, config(), specOf("test.threshold")), InvalidArgumentError);
}

TEST(PipelineValidate, EmptySpecIsRefused) {
    auto source = byteSource();
    Pipeline p(ts::makeSyntheticScene(source), config(), PipelineSpec());
    EXPECT_THROW(p.validate(), InvalidArgumentError);
    EXPECT_EQ(source->readCount(), 0);
}

TEST(PipelineValidate, UnknownOperationIsNamed) {
    auto source = byteSource();
    Pipeline p(ts::makeSyntheticScene(source), config(), specOf("test.V.nope"));
    EXPECT_TRUE(contains(validateMessage<InvalidArgumentError>(p), "test.V.nope"));
    EXPECT_EQ(source->readCount(), 0);
}

TEST(PipelineValidate, UnsupportedTypeNamesOperationAndType) {
    auto source = sourceOf({{300, 200, 0, 0}}, {slideio::DataType::DT_UInt16});
    Pipeline p(ts::makeSyntheticScene(source), config(), specOf("test.threshold"));
    const std::string message = validateMessage<UnsupportedDataTypeError>(p);
    EXPECT_TRUE(contains(message, "test.threshold")) << message;
    EXPECT_TRUE(contains(message, "UInt16")) << message;
    EXPECT_EQ(source->readCount(), 0);
}

TEST(PipelineValidate, TypesFollowTheChain) {
    auto source = byteSource();
    PipelineSpec spec = specOf("test.boxMean");
    spec.operation("test.floatOnly", OperationParameters());
    Pipeline p(ts::makeSyntheticScene(source), config(), spec);
    EXPECT_NO_THROW(p.validate());
    const detail::RunPlan& plan = planOf(p);
    ASSERT_EQ(plan.planeTypes.size(), 3u);
    EXPECT_EQ(plan.planeTypes[0], std::vector<DataType>{DataType::Byte});
    EXPECT_EQ(plan.planeTypes[1], std::vector<DataType>{DataType::Float32});
    EXPECT_EQ(plan.planeTypes[2], std::vector<DataType>{DataType::Float32});
    ASSERT_EQ(plan.operations.size(), 2u);
    EXPECT_STREQ(plan.operations[1]->name(), "test.floatOnly");
    EXPECT_EQ(plan.chainHalo.left, 1);
    EXPECT_EQ(plan.chainHalo.bottom, 1);
    EXPECT_FALSE(plan.restricted);
    EXPECT_TRUE(plan.globalContextPositions.empty());

    // The same op straight on the Byte scene is refused.
    Pipeline direct(ts::makeSyntheticScene(source), config(), specOf("test.floatOnly"));
    const std::string message = validateMessage<UnsupportedDataTypeError>(direct);
    EXPECT_TRUE(contains(message, "test.floatOnly")) << message;
    EXPECT_TRUE(contains(message, "Byte")) << message;
    EXPECT_EQ(source->readCount(), 0);
}

TEST(PipelineValidate, JointUniformOnMixedTypesIsArityError) {
    auto source = sourceOf({{300, 200, 0, 0}}, {slideio::DataType::DT_Byte, slideio::DataType::DT_Float32});
    Pipeline p(ts::makeSyntheticScene(source), config(), specOf("test.jointUniform"));
    const std::string message = validateMessage<ChannelArityError>(p);
    EXPECT_TRUE(contains(message, "channel 0 (Byte)")) << message;
    EXPECT_TRUE(contains(message, "channel 1 (Float32)")) << message;
    EXPECT_TRUE(contains(message, "test.jointUniform")) << message;
    EXPECT_EQ(source->readCount(), 0);

    // Uniform types pass, and the Joint output is outputPlaneCount planes.
    PipelineSpec one = specOf("test.jointUniform");
    one.channels(ChannelSelection::single(1));
    Pipeline q(ts::makeSyntheticScene(source), config(), one);
    EXPECT_NO_THROW(q.validate());
    EXPECT_EQ(planOf(q).planeTypes[1], std::vector<DataType>{DataType::Float32});
    EXPECT_EQ(source->readCount(), 0);
}

TEST(PipelineValidate, PerChannelAcceptsMixedTypes) {
    auto source = sourceOf({{300, 200, 0, 0}}, {slideio::DataType::DT_Byte, slideio::DataType::DT_Float32});
    Pipeline p(ts::makeSyntheticScene(source), config(), specOf("test.threshold"));
    EXPECT_NO_THROW(p.validate());
    EXPECT_EQ(planOf(p).planeTypes[0], (std::vector<DataType>{DataType::Byte, DataType::Float32}));
    EXPECT_EQ(planOf(p).planeTypes[1], (std::vector<DataType>{DataType::Byte, DataType::Byte}));
    EXPECT_EQ(source->readCount(), 0);
}

TEST(PipelineValidate, ChannelAndLevelRange) {
    auto source = pyramid({slideio::DataType::DT_Byte, slideio::DataType::DT_Byte, slideio::DataType::DT_Byte});
    const auto scene = ts::makeSyntheticScene(source);

    PipelineSpec channel = specOf("test.threshold");
    channel.channels(ChannelSelection::single(5));
    Pipeline badChannel(scene, config(), channel);
    EXPECT_TRUE(contains(validateMessage<InvalidArgumentError>(badChannel), "channel 5"));

    PipelineSpec level = specOf("test.threshold");
    level.analysisLevel(9);
    Pipeline badLevel(scene, config(), level);
    EXPECT_TRUE(contains(validateMessage<InvalidArgumentError>(badLevel), "level 9"));

    PipelineSpec good = specOf("test.threshold");
    good.analysisLevel(2).channels(ChannelSelection::single(2));
    Pipeline ok(scene, config(), good);
    EXPECT_NO_THROW(ok.validate());
    EXPECT_EQ(ok.analysisLevel(), 2);
    EXPECT_EQ(planOf(ok).levelScale, 4.0);
    EXPECT_EQ(source->readCount(), 0);
}

TEST(PipelineValidate, RestrictionOutsideTheSceneFails) {
    auto source = byteSource();
    PipelineSpec spec = specOf("test.threshold");
    spec.restrictTo(Rect64{5000, 5000, 100, 100});
    Pipeline p(ts::makeSyntheticScene(source), config(), spec);
    EXPECT_TRUE(contains(validateMessage<InvalidArgumentError>(p), "does not intersect"));

    // A restriction inside the scene lays the grid over the restriction alone.
    PipelineSpec inside = specOf("test.threshold");
    inside.restrictTo(Rect64{100, 100, 300, 50});
    Pipeline q(ts::makeSyntheticScene(source), config(256), inside);
    q.validate();
    EXPECT_TRUE(planOf(q).restricted);
    EXPECT_EQ(planOf(q).grid.tileCount(), 2);
    const Rect64 first = planOf(q).grid.tileRectAtLevel(0);
    EXPECT_EQ(first.x, 100);
    EXPECT_EQ(first.y, 100);
    EXPECT_EQ(source->readCount(), 0);
}

TEST(PipelineValidate, TargetLayerResolution) {
    auto source = byteSource();
    const auto scene = ts::makeSyntheticScene(source);

    // No targetLayer, one bound layer: resolved to it.
    Pipeline one(scene, config(), specOf("test.components"));
    one.bindLayer("nuclei", LayerId(7));
    one.validate();
    ASSERT_EQ(planOf(one).targetLayers.size(), 1u);
    EXPECT_EQ(planOf(one).targetLayers[0], LayerId(7));

    // No targetLayer, two bound layers: refused.
    Pipeline two(scene, config(), specOf("test.components"));
    two.bindLayer("nuclei", LayerId(7)).bindLayer("cells", LayerId(8));
    EXPECT_TRUE(contains(validateMessage<InvalidArgumentError>(two), "test.components"));

    // An explicit targetLayer picks among several.
    OperationParameters cells;
    cells.setString("targetLayer", "cells");
    Pipeline picked(scene, config(), specOf("test.components", cells));
    picked.bindLayer("nuclei", LayerId(7)).bindLayer("cells", LayerId(8));
    picked.validate();
    EXPECT_EQ(planOf(picked).targetLayers[0], LayerId(8));

    // An unbound targetLayer is named.
    OperationParameters x;
    x.setString("targetLayer", "x");
    Pipeline unbound(scene, config(), specOf("test.components", x));
    unbound.bindLayer("nuclei", LayerId(7));
    const std::string message = validateMessage<InvalidArgumentError>(unbound);
    EXPECT_TRUE(contains(message, "\"x\"")) << message;
    EXPECT_TRUE(contains(message, "test.components")) << message;

    // No layer bound at all.
    Pipeline none(scene, config(), specOf("test.components"));
    EXPECT_TRUE(contains(validateMessage<InvalidArgumentError>(none), "no target layer"));

    // An operation producing no objects gets an invalid layer.
    Pipeline raster(scene, config(), specOf("test.threshold"));
    raster.bindLayer("nuclei", LayerId(7));
    raster.validate();
    EXPECT_FALSE(planOf(raster).targetLayers[0].isValid());

    // A binding call drops the cached result.
    one.bindLayer("cells", LayerId(8));
    EXPECT_THROW(one.analysisLevel(), InvalidArgumentError);
    EXPECT_THROW(one.validate(), InvalidArgumentError);
    EXPECT_EQ(source->readCount(), 0);
}

TEST(PipelineValidate, WholeLevelForcesOneTileAndChecksBudget) {
    auto source = sourceOf({{1000, 1000, 0, 0}}, {slideio::DataType::DT_Byte});
    const auto scene = ts::makeSyntheticScene(source);

    Pipeline fits(scene, config(256), specOf("test.wholeLevel"));
    fits.memoryBudget(makeMemoryBudget(int64_t(2) << 30));
    fits.validate();
    EXPECT_EQ(planOf(fits).grid.tileCount(), 1);
    EXPECT_EQ(planOf(fits).grid.tileRectAtLevel(0).width, 1000);

    // 1,000,000 pixels x (1 input + 4 output + 100 working set) bytes.
    Pipeline tooBig(scene, config(256), specOf("test.wholeLevel"));
    tooBig.memoryBudget(makeMemoryBudget(10000000));
    const std::string message = validateMessage<InvalidArgumentError>(tooBig);
    EXPECT_TRUE(contains(message, "test.wholeLevel")) << message;
    EXPECT_TRUE(contains(message, "level 0")) << message;
    EXPECT_TRUE(contains(message, "105000000")) << message;
    EXPECT_TRUE(contains(message, "10000000 bytes")) << message;
    EXPECT_EQ(source->readCount(), 0);
}

TEST(PipelineValidate, AutoLevel) {
    auto source = pyramid();
    const auto scene = ts::makeSyntheticScene(source);

    Pipeline halo(scene, config(), specOf("test.boxMean"));
    halo.validate();
    EXPECT_EQ(halo.analysisLevel(), 0);

    // Per pixel 105 bytes: level 0 needs 105,000,000, level 1 26,250,000, level 2 6,562,500.
    Pipeline whole(scene, config(), specOf("test.wholeLevel"));
    whole.memoryBudget(makeMemoryBudget(30000000));
    whole.validate();
    EXPECT_EQ(whole.analysisLevel(), 1);
    EXPECT_EQ(planOf(whole).grid.tileCount(), 1);

    Pipeline none(scene, config(), specOf("test.wholeLevel"));
    none.memoryBudget(makeMemoryBudget(5000000));
    const std::string message = validateMessage<InvalidArgumentError>(none);
    EXPECT_TRUE(contains(message, "test.wholeLevel")) << message;
    // No level fits: the coarsest, level 2, needs 250 x 250 x 105 bytes.
    EXPECT_TRUE(contains(message, "level 2")) << message;
    EXPECT_TRUE(contains(message, "6562500")) << message;
    EXPECT_TRUE(contains(message, "5000000 bytes")) << message;
    EXPECT_THROW(none.analysisLevel(), InvalidArgumentError);
    EXPECT_EQ(source->readCount(), 0);
}

namespace {

// An auxiliary of 200 Float32 planes: 800 bytes per tile pixel. Never read here.
class WideAuxiliary : public TileRasterSource {
public:
    void fillTile(BlockView&) override {}
    DataType planeType() const override { return DataType::Float32; }
    int32_t planeCount() const override { return 200; }
};

} // namespace

TEST(PipelineValidate, AutoLevelAlsoFitsTheTile) {
    auto source = pyramid();
    WideAuxiliary auxiliary;
    // Level 0: the whole-level need is 105,000,000 bytes, but its one tile needs
    // 1,000,000 x (1 + 4 + 800) = 805,000,000; level 1's tile needs 201,250,000.
    Pipeline p(ts::makeSyntheticScene(source), config(), specOf("test.wholeLevel"));
    p.memoryBudget(makeMemoryBudget(300000000)).auxiliaryRaster("coarse", &auxiliary);
    p.validate();
    EXPECT_EQ(p.analysisLevel(), 1);
    EXPECT_EQ(planOf(p).tileBytes, int64_t(500) * 500 * 805);
    EXPECT_EQ(source->readCount(), 0);
}

TEST(PipelineValidate, AnalysisLevelBeforeValidateThrows) {
    auto source = byteSource();
    Pipeline p(ts::makeSyntheticScene(source), config(), specOf("test.threshold"));
    EXPECT_THROW(p.analysisLevel(), InvalidArgumentError);
    p.validate();
    EXPECT_EQ(p.analysisLevel(), 0);
    EXPECT_EQ(source->readCount(), 0);
}

TEST(PipelineValidate, TileLargerThanTheCeilingFails) {
    auto source = byteSource(2000, 2000);
    const auto scene = ts::makeSyntheticScene(source);
    // A 1024 x 1024 tile grown by radius 1 is 1026 x 1026 pixels of 1 + 4 bytes.
    Pipeline p(scene, config(1024), specOf("test.boxMean"));
    p.memoryBudget(makeMemoryBudget(1000000));
    const std::string message = validateMessage<InvalidArgumentError>(p);
    EXPECT_TRUE(contains(message, "1024 x 1024")) << message;
    EXPECT_TRUE(contains(message, "1000000 bytes")) << message;

    Pipeline q(scene, config(1024), specOf("test.boxMean"));
    q.validate();
    EXPECT_EQ(planOf(q).tileBytes, int64_t(1026) * 1026 * 5);
    EXPECT_EQ(source->readCount(), 0);
}

TEST(PipelineValidate, BindingsCheckTheirArguments) {
    auto source = byteSource();
    Pipeline p(ts::makeSyntheticScene(source), config(), specOf("test.threshold"));
    EXPECT_THROW(p.bindLayer(nullptr, LayerId(1)), InvalidArgumentError);
    EXPECT_THROW(p.bindLayer("", LayerId(1)), InvalidArgumentError);
    EXPECT_THROW(p.bindLayer("a", LayerId()), InvalidArgumentError);
    EXPECT_THROW(p.stageName(nullptr), InvalidArgumentError);
    EXPECT_THROW(p.stageName(""), InvalidArgumentError);
    EXPECT_THROW(p.auxiliaryRaster(nullptr, nullptr), InvalidArgumentError);
    EXPECT_NO_THROW(p.stageName("segment"));
    EXPECT_TRUE(p.spec().equals(specOf("test.threshold")));
}

TEST(PipelineValidate, PlanCNotYetImplemented) {
    auto source = byteSource();
    Pipeline p(ts::makeSyntheticScene(source), config(), specOf("test.threshold"));
    EXPECT_THROW(p.asScene(), InternalError);
    EXPECT_THROW(p.levelCount(), InternalError);
    EXPECT_THROW(p.rect(0), InternalError);

    // run() validates first, so a bad spec fails with the validation error.
    Pipeline bad(ts::makeSyntheticScene(source), config(), specOf("test.V.nope"));
    EXPECT_THROW(bad.run(nullptr, CancellationToken()), InvalidArgumentError);
    EXPECT_EQ(source->readCount(), 0);
}

// P26: the tile mask plus the one temporary mask that a polygon or a mask source needs at its peak.
TEST(PipelineValidate, TileBytesCountTheRestrictionMasks) {
    auto source = byteSource(2000, 2000);
    const auto scene = ts::makeSyntheticScene(source);
    auto tileBytes = [&](const PipelineSpec& spec, bool maskSource) {
        Pipeline p(scene, config(256), spec);
        struct Source : TileMaskSource {
            void fillMask(RasterMask&) override {}
        } mask;
        if (maskSource) {
            p.maskSource(&mask);
        }
        p.validate();
        return planOf(p).tileBytes;
    };
    const Point64 ring[] = {{0, 0}, {2000, 0}, {2000, 2000}, {0, 2000}};
    Polygon polygon;
    polygon.addRing(ring, 4, false);

    const int64_t area = 256 * 256;
    const int64_t base = tileBytes(specOf("test.threshold"), false);
    PipelineSpec roi = specOf("test.threshold");
    roi.restrictTo(Rect64{0, 0, 2000, 2000});
    PipelineSpec poly = specOf("test.threshold");
    poly.restrictToPolygon(polygon);
    PipelineSpec both = poly;
    both.restrictTo(Rect64{0, 0, 2000, 2000});
    EXPECT_EQ(tileBytes(roi, false), base + area);
    EXPECT_EQ(tileBytes(poly, false), base + 2 * area);
    EXPECT_EQ(tileBytes(both, false), base + 2 * area);
    EXPECT_EQ(tileBytes(specOf("test.threshold"), true), base + 2 * area);
}
