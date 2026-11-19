// Requirements: UR-EXT-1, UR-EXT-2, UR-EXT-3, UR-ALG-3, UR-ALG-4, UR-IMG-11, UR-EXE-1, UR-STG-3
#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/block_view.hpp>
#include <slideio/processor/core/channel_selection.hpp>
#include <slideio/processor/core/control.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/core/ids.hpp>
#include <slideio/processor/core/measurement.hpp>
#include <slideio/processor/core/raster_mask.hpp>
#include <slideio/processor/pipeline/operation.hpp>
#include <slideio/processor/pipeline/operation_parameters.hpp>
#include <slideio/processor/pipeline/pipeline.hpp>
#include <slideio/processor/pipeline/pipeline_spec.hpp>
#include <slideio/processor/pipeline/standard_measurements.hpp>

#include <pipeline/detail/combine_partials.hpp>
#include <pipeline/detail/level_mapping.hpp>
#include <pipeline/detail/operation_context_state.hpp>
#include <pipeline/detail/pipeline_impl.hpp>
#include <pipeline/detail/tile_evaluator.hpp>

#include <pipeline/public_only/third_party_operation.hpp>
#include <pipeline/test_support/synthetic_scene.hpp>
#include <pipeline/test_support/test_operations.hpp>

#include <slideio/slideio/scene.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using slideio::processor::BlockPlane;
using slideio::processor::BlockView;
using slideio::processor::CancellationSource;
using slideio::processor::CancellationToken;
using slideio::processor::ChannelArity;
using slideio::processor::ChannelSelection;
using slideio::processor::DataType;
using slideio::processor::Halo;
using slideio::processor::IntensityPartial;
using slideio::processor::InternalError;
using slideio::processor::InvalidArgumentError;
using slideio::processor::LayerId;
using slideio::processor::Measurement;
using slideio::processor::MeasurementSet;
using slideio::processor::MeasurementUnit;
using slideio::processor::ObjectRecord;
using slideio::processor::OperationContext;
using slideio::processor::OperationParameters;
using slideio::processor::Point64;
using slideio::processor::Polygon;
using slideio::processor::RasterMask;
using slideio::processor::Rect64;
using slideio::processor::TileRequirement;
namespace detail = slideio::processor::detail;
namespace ts = slideio::processor::testing_support;

namespace {

std::vector<Point64> ringOf(const Polygon& p, int32_t ring) {
    std::vector<Point64> points(static_cast<size_t>(p.ringPointCount(ring)));
    p.ringPoints(ring, points.data(), static_cast<int64_t>(points.size()));
    return points;
}

Polygon square(int64_t x, int64_t y, int64_t side) {
    const Point64 pts[] = {{x, y}, {x + side, y}, {x + side, y + side}, {x, y + side}};
    Polygon p;
    p.addRing(pts, 4, false);
    return p;
}

double measured(const MeasurementSet& set, const char* name) {
    Measurement m;
    EXPECT_TRUE(set.find(name, m)) << name;
    return m.value();
}

MeasurementUnit unitOf(const MeasurementSet& set, const char* name) {
    Measurement m;
    EXPECT_TRUE(set.find(name, m)) << name;
    return m.unit();
}

// A Byte tile of the given rows ('#' = 1..9 digit or '#' -> 200, '.' -> 0) and the
// state a framework would build for it at level 0, scale 1.
struct ByteTile {
    std::vector<uint8_t> pixels;
    int64_t width = 0;
    int64_t height = 0;
    BlockView view;

    ByteTile(const std::vector<std::string>& rows, Rect64 levelRect, double scale) {
        height = static_cast<int64_t>(rows.size());
        width = static_cast<int64_t>(rows[0].size());
        pixels.resize(static_cast<size_t>(width * height));
        for (int64_t y = 0; y < height; ++y) {
            for (int64_t x = 0; x < width; ++x) {
                const char c = rows[static_cast<size_t>(y)][static_cast<size_t>(x)];
                pixels[static_cast<size_t>(y * width + x)] =
                    c == '.' ? 0 : (c == '#' ? 200 : static_cast<uint8_t>(c - '0'));
            }
        }
        const BlockPlane plane{pixels.data(), DataType::Byte, width, 2};
        view = BlockView(width, height, detail::rectToLevel0(levelRect, scale), 0, &plane, 1);
    }
};

} // namespace

// --- Halo -----------------------------------------------------------------------

TEST(Halo, UniformNoneAndEmpty) {
    const Halo h = Halo::uniform(3);
    EXPECT_EQ(h.left, 3);
    EXPECT_EQ(h.top, 3);
    EXPECT_EQ(h.right, 3);
    EXPECT_EQ(h.bottom, 3);
    EXPECT_FALSE(h.isEmpty());
    EXPECT_TRUE(Halo::none().isEmpty());
    EXPECT_EQ(Halo::none().left, 0);
    EXPECT_EQ(Halo::none().bottom, 0);
    EXPECT_TRUE(Halo::uniform(0).isEmpty());
    EXPECT_FALSE((Halo{0, 0, 0, 1}.isEmpty()));
    EXPECT_THROW(Halo::uniform(-1), InvalidArgumentError);
}

// --- Operation defaults ------------------------------------------------------------

TEST(Operation, DefaultsForGlobalContextAndLayout) {
    ts::ThresholdOperation op{OperationParameters()};
    EXPECT_FALSE(op.needsGlobalContext());
    EXPECT_EQ(op.tileRequirement(), TileRequirement::Halo);
    EXPECT_EQ(op.workingSetBytesPerLevelPixel(), 0);
    EXPECT_EQ(op.workingSetBytesPerTile(), 0);
    EXPECT_FALSE(op.producesObjects());
    EXPECT_EQ(op.outputPlaneCount(3), 3);

    detail::OperationContextState state;
    const auto ctx = detail::makeOperationContext(std::move(state));
    op.accumulate(*ctx);
    ts::ThresholdOperation other{OperationParameters()};
    op.combine(other);
    op.finalizeAccumulation();
}

TEST(Operation, TestOperationsDeclareTheirContracts) {
    ts::ThresholdOperation threshold{OperationParameters().setDouble("level", 10.0)};
    EXPECT_STREQ(threshold.name(), "test.threshold");
    EXPECT_EQ(threshold.arity(), ChannelArity::PerChannel);
    EXPECT_FALSE(threshold.requiresUniformTypes());
    EXPECT_TRUE(threshold.supportedTypes().contains(DataType::Byte));
    EXPECT_TRUE(threshold.supportedTypes().contains(DataType::Float32));
    EXPECT_EQ(threshold.supportedTypes().count(), 2);
    EXPECT_TRUE(threshold.halo().isEmpty());
    EXPECT_EQ(threshold.outputType(DataType::Float32), DataType::Byte);
    double level = 0;
    EXPECT_TRUE(threshold.parameters().getDouble("level", level));
    EXPECT_EQ(level, 10.0);

    ts::BoxMeanOperation box{OperationParameters().setInt("radius", 2)};
    EXPECT_EQ(box.arity(), ChannelArity::PerChannel);
    EXPECT_EQ(box.halo().left, 2);
    EXPECT_EQ(box.halo().bottom, 2);
    EXPECT_EQ(box.outputType(DataType::Byte), DataType::Float32);
    EXPECT_THROW(ts::BoxMeanOperation{OperationParameters().setInt("radius", -1)}, InvalidArgumentError);

    ts::ComponentsOperation components{OperationParameters()};
    EXPECT_EQ(components.arity(), ChannelArity::Joint);
    EXPECT_TRUE(components.producesObjects());
    // Byte masks, and Float32 as the intensity plane of the two-plane form.
    EXPECT_EQ(components.supportedTypes().count(), 2);
    EXPECT_TRUE(components.supportedTypes().contains(DataType::Byte));
    EXPECT_TRUE(components.supportedTypes().contains(DataType::Float32));
    EXPECT_FALSE(components.requiresUniformTypes());
    EXPECT_TRUE(components.halo().isEmpty());
    EXPECT_EQ(components.outputType(DataType::Byte), DataType::Byte);
    EXPECT_EQ(components.outputType(DataType::Float32), DataType::Byte);
    EXPECT_EQ(components.outputPlaneCount(1), 1);
    EXPECT_EQ(components.outputPlaneCount(2), 1);
}

// --- OperationContext ------------------------------------------------------------

TEST(OperationContext, AccessorsReflectState) {
    uint8_t inPixels[30 * 40] = {};
    uint8_t outPixels[30 * 40] = {};
    uint8_t auxPixels[30 * 40] = {};
    const BlockPlane inPlane{inPixels, DataType::Byte, 30, 0};
    const BlockPlane outPlane{outPixels, DataType::Byte, 30, 0};
    const BlockPlane auxPlane{auxPixels, DataType::Byte, 30, 0};
    const Rect64 levelRect{10, 20, 30, 40};
    const double scale = 4.0005;
    const Rect64 level0Rect = detail::rectToLevel0(levelRect, scale);
    BlockView input(30, 40, level0Rect, 2, &inPlane, 1);
    BlockView output(30, 40, level0Rect, 2, &outPlane, 1);
    const BlockView aux(30, 40, level0Rect, 2, &auxPlane, 1);
    const RasterMask mask(levelRect, 2, scale);
    const ChannelSelection channels = ChannelSelection::single(1);
    CancellationSource cancel;
    const CancellationToken token = cancel.token();
    ts::CollectingObjectSink sink;

    detail::OperationContextState state;
    state.input = &input;
    state.output = &output;
    state.objectSink = &sink;
    state.edgeSink = nullptr;
    state.annotationSink = nullptr;
    state.auxiliaries.emplace_back("coarse", &aux);
    state.targetLayer = LayerId(7);
    state.level = 2;
    state.tileRectAtLevel = levelRect;
    state.haloRectAtLevel = Rect64{9, 19, 32, 42};
    state.tileRect = level0Rect;
    state.haloRect = detail::rectToLevel0(state.haloRectAtLevel, scale);
    state.levelScale = scale;
    state.channels = &channels;
    state.cancellation = &token;
    state.mask = &mask;
    const auto ctx = detail::makeOperationContext(std::move(state));

    EXPECT_EQ(&ctx->input(), &input);
    // Without the scene's samples, sourceInput() is the input itself.
    EXPECT_EQ(&ctx->sourceInput(), &input);
    EXPECT_EQ(&ctx->output(), &output);
    EXPECT_EQ(ctx->objectSink(), &sink);
    EXPECT_EQ(ctx->edgeSink(), nullptr);
    EXPECT_EQ(ctx->annotationSink(), nullptr);
    EXPECT_EQ(ctx->auxiliary("coarse"), &aux);
    EXPECT_EQ(ctx->auxiliary("x"), nullptr);
    EXPECT_EQ(ctx->auxiliary(nullptr), nullptr);
    EXPECT_EQ(ctx->targetLayer(), LayerId(7));
    EXPECT_EQ(ctx->level(), 2);
    EXPECT_EQ(ctx->tileRectAtLevel(), levelRect);
    EXPECT_EQ(ctx->haloRectAtLevel(), (Rect64{9, 19, 32, 42}));
    EXPECT_EQ(ctx->tileRect(), level0Rect);
    EXPECT_EQ(ctx->haloRect(), detail::rectToLevel0(Rect64{9, 19, 32, 42}, scale));
    EXPECT_EQ(ctx->levelScale(), scale);
    EXPECT_EQ(&ctx->channels(), &channels);
    EXPECT_EQ(&ctx->cancellation(), &token);
    EXPECT_EQ(ctx->mask(), &mask);
    EXPECT_FALSE(ctx->cancellation().isCancelled());
    cancel.cancel();
    EXPECT_TRUE(ctx->cancellation().isCancelled());
}

TEST(OperationContext, DefaultStateIsSafe) {
    const auto ctx = detail::makeOperationContext(detail::OperationContextState());
    EXPECT_TRUE(ctx->input().isNull());
    EXPECT_TRUE(ctx->output().isNull());
    EXPECT_EQ(ctx->objectSink(), nullptr);
    EXPECT_EQ(ctx->mask(), nullptr);
    EXPECT_FALSE(ctx->targetLayer().isValid());
    EXPECT_TRUE(ctx->channels().isAll());
    EXPECT_FALSE(ctx->cancellation().isCancelled());
    EXPECT_EQ(ctx->levelScale(), 1.0);
}

TEST(OperationContext, ToLevel0IsTheSingleMapping) {
    detail::OperationContextState state;
    state.levelScale = 4.0005;
    const auto ctx = detail::makeOperationContext(std::move(state));

    const Point64 p = ctx->toLevel0(Point64{7, 9});
    EXPECT_EQ(p.x, std::llround(7 * 4.0005));
    EXPECT_EQ(p.y, std::llround(9 * 4.0005));
    EXPECT_EQ(p.x, detail::levelToLevel0(7, 4.0005));

    Polygon level;
    const Point64 outer[] = {{0, 0}, {11, 0}, {11, 13}, {0, 13}};
    const Point64 hole[] = {{3, 3}, {3, 5}, {6, 5}, {6, 3}};
    level.addRing(outer, 4, false);
    level.addRing(hole, 4, true);
    const Polygon mapped = ctx->toLevel0(level);
    ASSERT_EQ(mapped.ringCount(), 2);
    EXPECT_FALSE(mapped.ringIsHole(0));
    EXPECT_TRUE(mapped.ringIsHole(1));
    for (int32_t r = 0; r < 2; ++r) {
        const std::vector<Point64> before = ringOf(level, r);
        const std::vector<Point64> after = ringOf(mapped, r);
        ASSERT_EQ(before.size(), after.size());
        for (size_t i = 0; i < before.size(); ++i) {
            EXPECT_EQ(after[i].x, detail::levelToLevel0(before[i].x, 4.0005));
            EXPECT_EQ(after[i].y, detail::levelToLevel0(before[i].y, 4.0005));
        }
    }
}

TEST(LevelMapping, RectanglesMapByTheirEdges) {
    const double s = 4.0005;
    // Two tiles sharing the level edge x = 1000 share the level-0 edge exactly.
    const Rect64 a = detail::rectToLevel0(Rect64{0, 0, 1000, 500}, s);
    const Rect64 b = detail::rectToLevel0(Rect64{1000, 500, 999, 1000}, s);
    EXPECT_EQ(a.x + a.width, b.x);
    EXPECT_EQ(a.y + a.height, b.y);
    EXPECT_EQ(b.x, std::llround(1000 * s));
    EXPECT_EQ(b.width, std::llround(1999 * s) - std::llround(1000 * s));
    EXPECT_EQ(detail::levelToLevel0(-3, 2.5), std::llround(-7.5));
}

TEST(OperationContext, SeamFragmentWithoutMergerIsInternalError) {
    const auto ctx = detail::makeOperationContext(detail::OperationContextState());
    const Polygon p = square(0, 0, 4);
    EXPECT_THROW(ctx->emitSeamFragment(p, LayerId(1)), InternalError);
    const IntensityPartial partial{1, 2.0, 4.0, 2.0, 2.0, 0};
    EXPECT_THROW(ctx->emitSeamFragment(p, LayerId(1), &partial, 1), InternalError);
}

TEST(OperationContext, SeamFragmentReachesCallback) {
    struct Call {
        Rect64 box;
        LayerId layer;
        std::vector<IntensityPartial> partials;
        int64_t joinKey;
        int64_t tileIndex;
    };
    std::vector<Call> calls;
    detail::OperationContextState state;
    state.tileIndex = 17;
    state.seamFragment = [&calls](const Polygon& p, LayerId layer, const IntensityPartial* partials,
                                  int32_t count, int64_t joinKey, int64_t tileIndex) {
        calls.push_back(
            {p.boundingBox(), layer, std::vector<IntensityPartial>(partials, partials + count), joinKey, tileIndex});
    };
    const auto ctx = detail::makeOperationContext(std::move(state));

    ctx->emitSeamFragment(square(1, 2, 3), LayerId(4));
    const IntensityPartial partials[] = {{3, 6.0, 14.0, 1.0, 3.0, 0}, {3, 9.0, 29.0, 2.0, 4.0, 1}};
    ctx->emitSeamFragment(square(5, 6, 7), LayerId(8), partials, 2);

    ASSERT_EQ(calls.size(), 2u);
    EXPECT_EQ(calls[0].box, (Rect64{1, 2, 3, 3}));
    EXPECT_EQ(calls[0].layer, LayerId(4));
    EXPECT_TRUE(calls[0].partials.empty());
    EXPECT_EQ(calls[1].box, (Rect64{5, 6, 7, 7}));
    EXPECT_EQ(calls[1].layer, LayerId(8));
    ASSERT_EQ(calls[1].partials.size(), 2u);
    EXPECT_EQ(calls[1].partials[1].channel, 1);
    EXPECT_EQ(calls[1].partials[1].sumOfSquares, 29.0);
    EXPECT_EQ(calls[0].tileIndex, 17) << "the emitting tile reaches the callback";
    EXPECT_EQ(calls[1].tileIndex, 17);
    EXPECT_EQ(calls[0].joinKey, 0) << "the overloads without a key mean key 0";
    EXPECT_EQ(calls[1].joinKey, 0);
    ctx->emitSeamFragment(square(2, 3, 4), LayerId(8), partials, 2, -42);
    ASSERT_EQ(calls.size(), 3u);
    EXPECT_EQ(calls[2].joinKey, -42);
    EXPECT_EQ(calls[2].partials.size(), 2u);
    EXPECT_EQ(calls[2].tileIndex, 17);

    EXPECT_THROW(ctx->emitSeamFragment(square(0, 0, 1), LayerId(1), partials, -1), InvalidArgumentError);
    EXPECT_THROW(ctx->emitSeamFragment(square(0, 0, 1), LayerId(1), nullptr, 1), InvalidArgumentError);
    ctx->emitSeamFragment(square(0, 0, 1), LayerId(1), nullptr, 0);
    EXPECT_EQ(calls.size(), 4u);
}

// --- Standard measurements ----------------------------------------------------------

TEST(StandardMeasurements, GeometryAndIntensity) {
    MeasurementSet out;
    const IntensityPartial partial{4, 10.0, 30.0, 1.0, 4.0, 2};
    slideio::processor::applyStandardMeasurements(square(0, 0, 10), &partial, 1, out);
    EXPECT_EQ(measured(out, "area"), 100.0);
    EXPECT_EQ(unitOf(out, "area"), MeasurementUnit::SquarePixels);
    EXPECT_EQ(measured(out, "perimeter"), 40.0);
    EXPECT_EQ(unitOf(out, "perimeter"), MeasurementUnit::Pixels);
    EXPECT_EQ(measured(out, "intensity.2.mean"), 2.5);
    EXPECT_DOUBLE_EQ(measured(out, "intensity.2.stddev"), std::sqrt(30.0 / 4 - 6.25));
    EXPECT_EQ(measured(out, "intensity.2.min"), 1.0);
    EXPECT_EQ(measured(out, "intensity.2.max"), 4.0);
    EXPECT_EQ(unitOf(out, "intensity.2.mean"), MeasurementUnit::Intensity);
    EXPECT_EQ(unitOf(out, "intensity.2.stddev"), MeasurementUnit::Intensity);
    EXPECT_EQ(out.count(), 6);

    const IntensityPartial twice[] = {partial, partial};
    MeasurementSet again;
    EXPECT_THROW(slideio::processor::applyStandardMeasurements(square(0, 0, 10), twice, 2, again),
                 InvalidArgumentError);
    EXPECT_THROW(slideio::processor::applyStandardMeasurements(square(0, 0, 10), nullptr, 1, again),
                 InvalidArgumentError);
    EXPECT_THROW(slideio::processor::applyStandardMeasurements(square(0, 0, 10), &partial, -1, again),
                 InvalidArgumentError);
}

TEST(StandardMeasurements, EmptyPartialAddsNoIntensityAndStddevNeverNegative) {
    MeasurementSet out;
    const IntensityPartial partials[] = {{0, 0.0, 0.0, 0.0, 0.0, 0}, {3, 3.0e8, 3.0e16, 1.0e8, 1.0e8, 1}};
    slideio::processor::applyStandardMeasurements(square(0, 0, 2), partials, 2, out);
    Measurement m;
    EXPECT_FALSE(out.find("intensity.0.mean", m));
    EXPECT_GE(measured(out, "intensity.1.stddev"), 0.0);
    EXPECT_EQ(measured(out, "intensity.1.mean"), 1.0e8);
}

TEST(StandardMeasurements, CombinePartials) {
    const IntensityPartial a{2, 3.0, 5.0, 1.0, 2.0, 1};
    const IntensityPartial b{3, 12.0, 50.0, 3.0, 5.0, 1};
    const IntensityPartial c = detail::combinePartials(a, b);
    EXPECT_EQ(c.count, 5);
    EXPECT_EQ(c.sum, 15.0);
    EXPECT_EQ(c.sumOfSquares, 55.0);
    EXPECT_EQ(c.minimum, 1.0);
    EXPECT_EQ(c.maximum, 5.0);
    EXPECT_EQ(c.channel, 1);
    const IntensityPartial other{1, 1.0, 1.0, 1.0, 1.0, 2};
    EXPECT_THROW(detail::combinePartials(a, other), InvalidArgumentError);
}

// --- Test-support operations ---------------------------------------------------------

TEST(TestSupport, NormaliseRingDropsCollinearAndStartsTopLeft) {
    const std::vector<Point64> ring = {{2, 2}, {2, 1}, {2, 0}, {1, 0}, {0, 0}, {0, 1}, {0, 2}, {1, 2}};
    const std::vector<Point64> n = ts::normaliseRing(ring);
    ASSERT_EQ(n.size(), 4u);
    EXPECT_EQ(n[0], (Point64{0, 0}));
    EXPECT_EQ(n[1], (Point64{0, 2}));
    EXPECT_EQ(n[2], (Point64{2, 2}));
    EXPECT_EQ(n[3], (Point64{2, 0}));
}

TEST(TestSupport, ThresholdAndBoxMeanApply) {
    std::vector<float> in = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    std::vector<uint8_t> thresholded(9, 7);
    const BlockPlane inPlane{in.data(), DataType::Float32, 3 * 4, 0};
    const BlockPlane outPlane{thresholded.data(), DataType::Byte, 3, 0};
    BlockView input(3, 3, Rect64{0, 0, 3, 3}, 0, &inPlane, 1);
    BlockView output(3, 3, Rect64{0, 0, 3, 3}, 0, &outPlane, 1);
    detail::OperationContextState state;
    state.input = &input;
    state.output = &output;
    state.tileRectAtLevel = state.haloRectAtLevel = Rect64{0, 0, 3, 3};
    ts::ThresholdOperation threshold{OperationParameters().setDouble("level", 5.0)};
    threshold.apply(*detail::makeOperationContext(state));
    EXPECT_EQ(thresholded, (std::vector<uint8_t>{0, 0, 0, 0, 255, 255, 255, 255, 255}));

    // Box mean of radius 1: the halo rect is the 3x3 input, the tile its centre pixel.
    float mean = -1.0f;
    const BlockPlane meanPlane{&mean, DataType::Float32, 4, 0};
    BlockView meanView(1, 1, Rect64{1, 1, 1, 1}, 0, &meanPlane, 1);
    state.output = &meanView;
    state.tileRectAtLevel = Rect64{1, 1, 1, 1};
    ts::BoxMeanOperation box{OperationParameters().setInt("radius", 1)};
    box.apply(*detail::makeOperationContext(state));
    EXPECT_FLOAT_EQ(mean, 5.0f);
}

TEST(TestSupport, ComponentsEmitsPixelEdgePolygonsInLevel0) {
    // A ring with a hole, a diagonal neighbour (a separate component) and a single pixel.
    const ByteTile tile({
        ".......",
        ".###...",
        ".#.#...",
        ".###...",
        "....#..",
        "......9",
    }, Rect64{10, 20, 7, 6}, 2.0);
    std::vector<uint8_t> copy(tile.pixels.size(), 0);
    const BlockPlane outPlane{copy.data(), DataType::Byte, tile.width, 2};
    BlockView output(tile.width, tile.height, tile.view.slideRect(), 0, &outPlane, 1);
    ts::CollectingObjectSink sink;
    detail::OperationContextState state;
    state.input = &tile.view;
    state.output = &output;
    state.objectSink = &sink;
    state.targetLayer = LayerId(3);
    state.tileRectAtLevel = state.haloRectAtLevel = Rect64{10, 20, 7, 6};
    state.levelScale = 2.0;
    ts::ComponentsOperation op{OperationParameters()};
    op.apply(*detail::makeOperationContext(std::move(state)));

    EXPECT_EQ(copy, tile.pixels);
    const std::vector<ObjectRecord> records = sink.records();
    ASSERT_EQ(records.size(), 3u);
    // Ordered by anchor: the ring, then the diagonal pixel, then the 9.
    const Polygon& ring = records[0].geometry();
    ASSERT_EQ(ring.ringCount(), 2);
    EXPECT_FALSE(ring.ringIsHole(0));
    EXPECT_TRUE(ring.ringIsHole(1));
    EXPECT_EQ(ringOf(ring, 0), (std::vector<Point64>{{22, 42}, {28, 42}, {28, 48}, {22, 48}}));
    EXPECT_EQ(ringOf(ring, 1), (std::vector<Point64>{{24, 44}, {24, 46}, {26, 46}, {26, 44}}));
    EXPECT_EQ(records[0].layer(), LayerId(3));
    EXPECT_EQ(measured(records[0].measurements(), "area"), 32.0);
    EXPECT_EQ(measured(records[0].measurements(), "perimeter"), 32.0);
    EXPECT_EQ(measured(records[0].measurements(), "intensity.2.mean"), 200.0);
    EXPECT_EQ(measured(records[0].measurements(), "intensity.2.stddev"), 0.0);

    EXPECT_EQ(records[1].geometry().boundingBox(), (Rect64{28, 48, 2, 2}));
    EXPECT_EQ(records[2].geometry().boundingBox(), (Rect64{32, 50, 2, 2}));
    EXPECT_EQ(measured(records[2].measurements(), "intensity.2.max"), 9.0);
}

TEST(TestSupport, ComponentsTwoPlaneFormMeasuresAFloatIntensityPlane) {
    // Plane 0 is the Byte mask, plane 1 a Float32 intensity on source channel 5:
    // the polygons come from the mask, the partials from plane 1.
    ByteTile mask({
        "##..",
        "##..",
        "...#",
    }, Rect64{0, 0, 4, 3}, 1.0);
    std::vector<float> intensity = {0.25f, 1.5f, 9.0f, 9.0f, -2.0f, 0.125f, 9.0f, 9.0f, 9.0f, 9.0f, 9.0f, 3.75f};
    const BlockPlane planes[] = {BlockPlane{mask.pixels.data(), DataType::Byte, 4, 2},
                                 BlockPlane{intensity.data(), DataType::Float32, 4 * 4, 5}};
    const BlockView input(4, 3, Rect64{0, 0, 4, 3}, 0, planes, 2);
    std::vector<uint8_t> copy(mask.pixels.size(), 0);
    const BlockPlane outPlane{copy.data(), DataType::Byte, 4, 2};
    BlockView output(4, 3, Rect64{0, 0, 4, 3}, 0, &outPlane, 1);
    ts::CollectingObjectSink sink;
    std::vector<IntensityPartial> fragments;
    detail::OperationContextState state;
    state.input = &input;
    state.output = &output;
    state.objectSink = &sink;
    state.tileRectAtLevel = state.haloRectAtLevel = Rect64{0, 0, 4, 3};
    state.seamFragment = [&fragments](const Polygon&, LayerId, const IntensityPartial* partials, int32_t count,
                                      int64_t, int64_t) {
        ASSERT_EQ(count, 1);
        fragments.push_back(partials[0]);
    };
    ts::ComponentsOperation op{OperationParameters()};
    op.apply(*detail::makeOperationContext(state));

    EXPECT_EQ(copy, mask.pixels);
    const std::vector<ObjectRecord> records = sink.records();
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[0].geometry().boundingBox(), (Rect64{0, 0, 2, 2}));
    EXPECT_EQ(measured(records[0].measurements(), "intensity.5.mean"), (0.25 + 1.5 - 2.0 + 0.125) / 4);
    EXPECT_EQ(measured(records[0].measurements(), "intensity.5.min"), -2.0);
    EXPECT_EQ(measured(records[0].measurements(), "intensity.5.max"), 1.5);
    Measurement unused;
    EXPECT_FALSE(records[0].measurements().find("intensity.2.mean", unused));
    EXPECT_EQ(measured(records[1].measurements(), "intensity.5.max"), 3.75);

    // With "seams" the partial handed to the merger also comes from plane 1.
    ts::ComponentsOperation seams{OperationParameters().setBool("seams", true)};
    seams.apply(*detail::makeOperationContext(state));
    ASSERT_EQ(fragments.size(), 2u);
    EXPECT_EQ(fragments[0].count, 4);
    EXPECT_EQ(fragments[0].sum, -0.125);
    EXPECT_EQ(fragments[0].sumOfSquares, 0.0625 + 2.25 + 4.0 + 0.015625);
    EXPECT_EQ(fragments[0].channel, 5);
    EXPECT_EQ(fragments[1].sum, 3.75);
}

TEST(TestSupport, ComponentsTwoPlaneFormAcceptsAByteIntensityAndRefusesBadPlanes) {
    ByteTile mask({"#."}, Rect64{0, 0, 2, 1}, 1.0);
    std::vector<uint8_t> byteIntensity = {77, 3};
    std::vector<float> floatMask = {1.0f, 0.0f};
    std::vector<uint16_t> wideIntensity = {7, 7};
    std::vector<uint8_t> copy(2, 0);
    const BlockPlane outPlane{copy.data(), DataType::Byte, 2, 0};
    BlockView output(2, 1, Rect64{0, 0, 2, 1}, 0, &outPlane, 1);
    ts::CollectingObjectSink sink;
    auto run = [&](const BlockPlane* planes, int32_t count) {
        const BlockView input(2, 1, Rect64{0, 0, 2, 1}, 0, planes, count);
        detail::OperationContextState state;
        state.input = &input;
        state.output = &output;
        state.objectSink = &sink;
        state.tileRectAtLevel = state.haloRectAtLevel = Rect64{0, 0, 2, 1};
        ts::ComponentsOperation op{OperationParameters()};
        op.apply(*detail::makeOperationContext(state));
    };
    const BlockPlane maskPlane{mask.pixels.data(), DataType::Byte, 2, 0};
    const BlockPlane byteIntensityPlane{byteIntensity.data(), DataType::Byte, 2, 1};
    const BlockPlane twoByte[] = {maskPlane, byteIntensityPlane};
    run(twoByte, 2);
    ASSERT_EQ(sink.records().size(), 1u);
    EXPECT_EQ(measured(sink.records()[0].measurements(), "intensity.1.mean"), 77.0);

    // Float32 is an intensity type only, UInt16 is neither, and three planes are neither form.
    const BlockPlane floatMaskPlane{floatMask.data(), DataType::Float32, 2 * 4, 0};
    const BlockPlane floatFirst[] = {floatMaskPlane, byteIntensityPlane};
    EXPECT_THROW(run(floatFirst, 2), slideio::processor::UnsupportedDataTypeError);
    EXPECT_THROW(run(&floatMaskPlane, 1), slideio::processor::UnsupportedDataTypeError);
    const BlockPlane widePlane{wideIntensity.data(), DataType::UInt16, 2 * 2, 1};
    const BlockPlane wideSecond[] = {maskPlane, widePlane};
    EXPECT_THROW(run(wideSecond, 2), slideio::processor::UnsupportedDataTypeError);
    const BlockPlane three[] = {maskPlane, byteIntensityPlane, byteIntensityPlane};
    EXPECT_THROW(run(three, 3), InvalidArgumentError);
}

TEST(TestSupport, ComponentsChecksTheTileAndOutputBeforeReading) {
    const ByteTile input({"##", "##"}, Rect64{0, 0, 2, 2}, 1.0);
    std::vector<uint8_t> copy(9, 0);
    const BlockPlane outPlane{copy.data(), DataType::Byte, 3, 2};
    detail::OperationContextState state;
    state.input = &input.view;
    ts::CollectingObjectSink sink;
    state.objectSink = &sink;
    ts::ComponentsOperation op{OperationParameters()};

    // A tile reaching past the input is refused before any pixel is copied.
    BlockView big(3, 3, Rect64{0, 0, 3, 3}, 0, &outPlane, 1);
    state.output = &big;
    state.haloRectAtLevel = Rect64{0, 0, 2, 2};
    state.tileRectAtLevel = Rect64{0, 0, 3, 3};
    EXPECT_THROW(op.apply(*detail::makeOperationContext(state)), InvalidArgumentError);
    EXPECT_EQ(copy, std::vector<uint8_t>(9, 0));

    // An output whose size differs from the tile is refused too.
    state.tileRectAtLevel = Rect64{0, 0, 2, 2};
    EXPECT_THROW(op.apply(*detail::makeOperationContext(state)), InvalidArgumentError);
    EXPECT_EQ(copy, std::vector<uint8_t>(9, 0));
    EXPECT_TRUE(sink.records().empty());
}

TEST(TestSupport, ComponentsSelfTouchingShapeIsOneRing) {
    // A 4-connected U whose ends touch diagonally: the background stays 8-connected,
    // so there is no hole and the outer ring passes the pinch vertex twice.
    const ByteTile tile({
        ".....",
        "..##.",
        ".#.#.",
        ".###.",
        ".....",
    }, Rect64{0, 0, 5, 5}, 1.0);
    std::vector<uint8_t> copy(tile.pixels.size(), 0);
    const BlockPlane outPlane{copy.data(), DataType::Byte, tile.width, 2};
    BlockView output(tile.width, tile.height, tile.view.slideRect(), 0, &outPlane, 1);
    ts::CollectingObjectSink sink;
    detail::OperationContextState state;
    state.input = &tile.view;
    state.output = &output;
    state.objectSink = &sink;
    state.tileRectAtLevel = state.haloRectAtLevel = Rect64{0, 0, 5, 5};
    ts::ComponentsOperation op{OperationParameters()};
    op.apply(*detail::makeOperationContext(std::move(state)));
    const std::vector<ObjectRecord> records = sink.records();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].geometry().ringCount(), 1);
    EXPECT_EQ(records[0].geometry().area(), 7.0);
    EXPECT_EQ(ringOf(records[0].geometry(), 0).front(), (Point64{2, 1}));
}

TEST(TestSupport, ComponentsSendsEdgeTouchingComponentsToSeamsWhenAsked) {
    const ByteTile tile({
        "#....",
        ".....",
        "..#..",
        ".....",
    }, Rect64{0, 0, 5, 4}, 1.0);
    std::vector<uint8_t> copy(tile.pixels.size(), 0);
    const BlockPlane outPlane{copy.data(), DataType::Byte, tile.width, 2};
    BlockView output(tile.width, tile.height, tile.view.slideRect(), 0, &outPlane, 1);
    ts::CollectingObjectSink sink;
    std::vector<IntensityPartial> fragments;
    detail::OperationContextState state;
    state.input = &tile.view;
    state.output = &output;
    state.objectSink = &sink;
    state.tileRectAtLevel = state.haloRectAtLevel = Rect64{0, 0, 5, 4};
    state.seamFragment = [&fragments](const Polygon& p, LayerId, const IntensityPartial* partials,
                                      int32_t count, int64_t, int64_t) {
        EXPECT_EQ(p.boundingBox(), (Rect64{0, 0, 1, 1}));
        ASSERT_EQ(count, 1);
        fragments.push_back(partials[0]);
    };
    ts::ComponentsOperation op{OperationParameters().setBool("seams", true)};
    op.apply(*detail::makeOperationContext(state));
    ASSERT_EQ(fragments.size(), 1u);
    EXPECT_EQ(fragments[0].count, 1);
    EXPECT_EQ(fragments[0].sum, 200.0);
    EXPECT_EQ(fragments[0].channel, 2);
    ASSERT_EQ(sink.records().size(), 1u);
    EXPECT_EQ(sink.records()[0].geometry().boundingBox(), (Rect64{2, 2, 1, 1}));

    // Without "seams" both are finished objects.
    ts::CollectingObjectSink all;
    state.objectSink = &all;
    ts::ComponentsOperation plain{OperationParameters()};
    plain.apply(*detail::makeOperationContext(state));
    EXPECT_EQ(all.records().size(), 2u);
}

TEST(TestSupport, ComponentsIgnoreTheHaloAndClipToTheTile) {
    // The input spans the halo rect {8, 19, 8, 5}; the tile is {10, 20, 4, 3}, which is
    // input columns 2..5 and rows 1..3. Components anchored in the halo (the '#' at
    // (0, 0), the pair at (6..7, 2)) are never emitted; the two that cross the tile
    // edge are emitted as their tile-clipped pieces.
    const ByteTile input({
        "#..#....",
        "..##....",
        "......##",
        ".####...",
        "........",
    }, Rect64{8, 19, 8, 5}, 1.0);
    std::vector<uint8_t> copy(4 * 3, 0);
    const BlockPlane outPlane{copy.data(), DataType::Byte, 4, 2};
    BlockView output(4, 3, Rect64{10, 20, 4, 3}, 0, &outPlane, 1);
    std::vector<std::pair<Rect64, int64_t>> fragments;
    ts::CollectingObjectSink sink;
    detail::OperationContextState state;
    state.input = &input.view;
    state.output = &output;
    state.objectSink = &sink;
    state.haloRectAtLevel = Rect64{8, 19, 8, 5};
    state.tileRectAtLevel = Rect64{10, 20, 4, 3};
    state.seamFragment = [&fragments](const Polygon& p, LayerId, const IntensityPartial* partials, int32_t,
                                      int64_t, int64_t) {
        fragments.emplace_back(p.boundingBox(), partials[0].count);
    };
    ts::ComponentsOperation seams{OperationParameters().setBool("seams", true)};
    seams.apply(*detail::makeOperationContext(state));

    EXPECT_EQ(copy, (std::vector<uint8_t>{200, 200, 0, 0, 0, 0, 0, 0, 200, 200, 200, 0}));
    EXPECT_TRUE(sink.records().empty());
    ASSERT_EQ(fragments.size(), 2u);
    EXPECT_EQ(fragments[0].first, (Rect64{10, 20, 2, 1}));
    EXPECT_EQ(fragments[0].second, 2);
    EXPECT_EQ(fragments[1].first, (Rect64{10, 22, 3, 1}));
    EXPECT_EQ(fragments[1].second, 3);

    ts::ComponentsOperation plain{OperationParameters()};
    plain.apply(*detail::makeOperationContext(state));
    const std::vector<ObjectRecord> records = sink.records();
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[0].geometry().boundingBox(), (Rect64{10, 20, 2, 1}));
    EXPECT_EQ(records[1].geometry().boundingBox(), (Rect64{10, 22, 3, 1}));
    EXPECT_EQ(measured(records[1].measurements(), "area"), 3.0);
}

// --- Synthetic scene -------------------------------------------------------------------

TEST(TestSupport, SyntheticSceneSaturatesOutOfRangeValues) {
    auto scene = ts::makeSyntheticScene(
        std::vector<ts::SyntheticLevel>{{4, 1, 0, 0}},
        std::vector<slideio::DataType>{slideio::DataType::DT_Byte},
        [](int, int64_t x, int64_t) {
            const double values[] = {300.0, -5.0, std::numeric_limits<double>::quiet_NaN(), 7.0};
            return values[x];
        });
    uint8_t v[4] = {9, 9, 9, 9};
    scene->readResampledLevelBlockChannels(0, std::make_tuple(0, 0, 4, 1), std::make_tuple(4, 1),
                                           std::vector<int>{0}, v, sizeof v);
    EXPECT_EQ(v[0], 255);
    EXPECT_EQ(v[1], 0);
    EXPECT_EQ(v[2], 0);
    EXPECT_EQ(v[3], 7);
}

TEST(TestSupport, SyntheticSceneReadsLevelsAndCountsReads) {
    auto source = std::make_shared<ts::SyntheticSource>(
        std::vector<ts::SyntheticLevel>{{100, 80, 32, 32}, {50, 40, 16, 16}},
        std::vector<slideio::DataType>{slideio::DataType::DT_Byte, slideio::DataType::DT_Byte},
        [](int channel, int64_t x, int64_t y) { return static_cast<double>((channel * 50 + x + y) % 256); });
    const std::shared_ptr<slideio::Scene> scene = ts::makeSyntheticScene(source);
    ASSERT_EQ(scene->getNumZoomLevels(), 2);
    EXPECT_EQ(scene->getLevelInfo(1)->getSize().width, 50);
    EXPECT_EQ(scene->getLevelInfo(1)->getTileSize().width, 16);
    EXPECT_EQ(source->readCount(), 0);

    // Level 1, two channels interleaved, partly outside the level (background zero).
    std::vector<uint8_t> buffer(4 * 2 * 2, 0xEE);
    scene->readResampledLevelBlockChannels(1, std::make_tuple(48, 10, 4, 2), std::make_tuple(4, 2),
                                           std::vector<int>{0, 1}, buffer.data(), buffer.size());
    EXPECT_EQ(source->readCount(), 1);
    EXPECT_EQ(buffer[0], 58);      // channel 0 at (48, 10)
    EXPECT_EQ(buffer[1], 108);     // channel 1 at (48, 10)
    EXPECT_EQ(buffer[2], 59);      // channel 0 at (49, 10)
    EXPECT_EQ(buffer[3], 109);
    EXPECT_EQ(buffer[4], 0);       // (50, 10) is outside the level
    EXPECT_EQ(buffer[8 * 2 - 1], 0);

    // Level 0 through the scene-coordinate path.
    std::vector<uint8_t> one(1, 0);
    scene->readBlockChannels(std::make_tuple(7, 3, 1, 1), std::vector<int>{1}, one.data(), one.size());
    EXPECT_EQ(one[0], 60);
    EXPECT_EQ(source->readCount(), 2);
}

TEST(TestSupport, SyntheticSceneWritesTheChannelType) {
    auto scene = ts::makeSyntheticScene(
        std::vector<ts::SyntheticLevel>{{8, 8, 0, 0}},
        std::vector<slideio::DataType>{slideio::DataType::DT_Float32},
        [](int, int64_t x, int64_t y) { return 0.5 + static_cast<double>(x * 10 + y); });
    float v = 0;
    scene->readResampledLevelBlockChannels(0, std::make_tuple(3, 2, 1, 1), std::make_tuple(1, 1),
                                           std::vector<int>{0}, &v, sizeof v);
    EXPECT_EQ(v, 32.5f);
}

// --- Public headers only (UR-EXT-2) -----------------------------------------------------

TEST(PublicOnly, ThirdPartyOperationBuildsFromPublicHeaders) {
    const auto op = thirdparty::makeInvertOperation(OperationParameters().setInt("bias", 5));
    ASSERT_NE(op, nullptr);
    EXPECT_STREQ(op->name(), "thirdparty.invert");
    EXPECT_EQ(op->arity(), ChannelArity::PerChannel);
    EXPECT_FALSE(op->requiresUniformTypes());
    EXPECT_TRUE(op->supportedTypes().contains(DataType::Byte));
    EXPECT_EQ(op->supportedTypes().count(), 1);
    EXPECT_EQ(op->halo().left, 1);
    EXPECT_EQ(op->outputType(DataType::Byte), DataType::Byte);
    EXPECT_EQ(op->tileRequirement(), TileRequirement::Halo);
    int64_t bias = 0;
    EXPECT_TRUE(op->parameters().getInt("bias", bias));
    EXPECT_EQ(bias, 5);

    uint8_t in[9] = {0, 0, 0, 0, 10, 0, 0, 0, 0};
    uint8_t out = 0;
    const BlockPlane inPlane{in, DataType::Byte, 3, 0};
    const BlockPlane outPlane{&out, DataType::Byte, 1, 0};
    BlockView input(3, 3, Rect64{0, 0, 3, 3}, 0, &inPlane, 1);
    BlockView output(1, 1, Rect64{1, 1, 1, 1}, 0, &outPlane, 1);
    detail::OperationContextState state;
    state.input = &input;
    state.output = &output;
    state.haloRectAtLevel = Rect64{0, 0, 3, 3};
    state.tileRectAtLevel = Rect64{1, 1, 1, 1};
    op->apply(*detail::makeOperationContext(std::move(state)));
    EXPECT_EQ(out, 255 - 10 - 5);
}

// The evaluator hands each seam fragment the index of the tile that emitted it, in
// the final pass, and the fragment lies in that tile.
TEST(OperationContext, EvaluatorPassesTheEmittingTileIndex) {
    namespace sp = slideio::processor;
    ts::registerTestOperations();
    // 15 x 15 blobs on a 20-pixel pitch: every tile has blobs on its edges.
    auto scene = ts::makeSyntheticScene({{256, 256, 0, 0}}, {slideio::DataType::DT_Byte}, [](int, int64_t x, int64_t y) {
        return x % 20 < 15 && y % 20 < 15 ? 255.0 : 0.0;
    });
    sp::PipelineSpec spec;
    spec.operation("test.components", OperationParameters().setBool("seams", true));
    sp::Pipeline p(scene, sp::ConfigurationBuilder().tileSize(64, 64).tileSizeFromSlide(false).workerCount(1).build(),
                   spec);
    p.bindLayer("objects", LayerId(3));
    p.validate();
    auto& impl = detail::PipelineAccess::impl(p);
    const detail::RunPlan& plan = detail::PipelineAccess::plan(p);

    std::vector<std::pair<int64_t, Rect64>> fragments;  // (tile index, bounding box)
    ts::CollectingObjectSink sink;
    detail::Bindings bindings;
    bindings.objectSink = &sink;
    bindings.seamFragment = [&fragments](const Polygon& polygon, LayerId, const IntensityPartial*, int32_t,
                                         int64_t, int64_t tileIndex) {
        fragments.emplace_back(tileIndex, polygon.boundingBox());
    };
    bindings.spec = &impl.spec;
    bindings.selector = impl.selector.get();
    bindings.counters = &impl.counters;
    const detail::TileEvaluator evaluator(plan, plan.operations, *impl.reader, std::move(bindings));

    ASSERT_EQ(plan.grid.tileCount(), 16);
    for (int64_t tile = 0; tile < plan.grid.tileCount(); ++tile) {
        const size_t before = fragments.size();
        evaluator.evaluate(tile, CancellationToken());
        ASSERT_GT(fragments.size(), before) << "tile " << tile << " emitted no fragment";
        for (size_t i = before; i < fragments.size(); ++i) {
            EXPECT_EQ(fragments[i].first, tile);
            EXPECT_EQ(plan.grid.tileRect(tile).intersected(fragments[i].second), fragments[i].second)
                << "a fragment of tile " << tile << " lies outside it";
        }
    }
}
