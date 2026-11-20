// Requirements: UR-EXE-1, UR-RES-5, UR-PIX-6
#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/core/ids.hpp>
#include <slideio/processor/pipeline/operation_catalog.hpp>
#include <slideio/processor/pipeline/pipeline.hpp>

#include <pipeline/detail/level_mapping.hpp>
#include <pipeline/detail/pipeline_impl.hpp>
#include <pipeline/test_support/synthetic_scene.hpp>
#include <pipeline/test_support/test_operations.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using namespace slideio::processor;
namespace ts = slideio::processor::testing_support;

namespace {

double texture(int, int64_t x, int64_t y) { return static_cast<double>((x * 5 + y * 3) % 256); }

std::shared_ptr<slideio::Scene> pyramidScene() {
    ts::registerTestOperations();
    // Level 1 has a non-integer scale, so slide rects exercise the edge mapping.
    return ts::makeSyntheticScene({{1000, 700, 0, 0}, {333, 233, 0, 0}}, {slideio::DataType::DT_Byte}, texture);
}

Configuration config(int32_t tile) {
    return ConfigurationBuilder().tileSize(tile, tile).tileSizeFromSlide(false).build();
}

// --- Sinks ---------------------------------------------------------------------------

class RecordingRasterSink : public RasterSink {
public:
    void writeTile(const BlockView& tile) override {
        std::lock_guard<std::mutex> lock(mutex_);
        rects_.push_back(tile.slideRect());
        levels_.insert(tile.level());
        pixels_ += tile.width() * tile.height();
    }
    void flush() override {}
    int64_t tilesWritten() const override { return static_cast<int64_t>(rects_.size()); }
    std::vector<Rect64> rects() const { return rects_; }
    std::set<int32_t> levels() const { return levels_; }
    int64_t pixels() const { return pixels_; }

private:
    mutable std::mutex mutex_;
    std::vector<Rect64> rects_;
    std::set<int32_t> levels_;
    int64_t pixels_ = 0;
};

class RecordingEdgeSink : public EdgeSink {
public:
    void assertParent(ObjectId child, ObjectId parent) override {
        std::lock_guard<std::mutex> lock(mutex_);
        parents_.emplace_back(child, parent);
    }
    void assertDerivedFrom(ObjectId, ObjectId) override {}
    void flush() override {}
    int64_t assertedCount() const override { return static_cast<int64_t>(parents_.size()); }
    std::vector<std::pair<ObjectId, ObjectId>> parents() const { return parents_; }

private:
    mutable std::mutex mutex_;
    std::vector<std::pair<ObjectId, ObjectId>> parents_;
};

class RecordingAnnotationSink : public AnnotationSink {
public:
    void assertLabel(ObjectId object, LabelId label) override {
        std::lock_guard<std::mutex> lock(mutex_);
        labels_.emplace_back(object, label.value());
    }
    void assertMeasurement(ObjectId, const char*, double, MeasurementUnit) override {}
    void flush() override {}
    int64_t assertedCount() const override { return static_cast<int64_t>(labels_.size()); }
    std::vector<std::pair<ObjectId, int32_t>> labels() const { return labels_; }

private:
    mutable std::mutex mutex_;
    std::vector<std::pair<ObjectId, int32_t>> labels_;
};

// An auxiliary of two UInt16 planes. Plane 0 holds (x + 7 y) mod 65536 of the level-0
// pixel it covers (meaningful at level 0, where the level scale is 1), plane 1 holds
// plane 0 plus one. Counts its calls and keeps the size of every view it was asked to fill.
class PatternSource : public TileRasterSource {
public:
    void fillTile(BlockView& out) override {
        const Rect64 origin = out.slideRect();
        for (int32_t plane = 0; plane < out.planeCount(); ++plane) {
            for (int64_t y = 0; y < out.height(); ++y) {
                auto* row = reinterpret_cast<uint16_t*>(static_cast<uint8_t*>(out.planeData(plane)) +
                                                        y * out.planeRowStrideBytes(plane));
                for (int64_t x = 0; x < out.width(); ++x) {
                    row[x] = static_cast<uint16_t>((origin.x + x + 7 * (origin.y + y) + plane) % 65536);
                }
            }
        }
        ++fills;
        std::lock_guard<std::mutex> lock(mutex_);
        sizes.emplace(out.width(), out.height());
    }
    DataType planeType() const override { return DataType::UInt16; }
    int32_t planeCount() const override { return 2; }
    std::atomic<int64_t> fills{0};
    std::set<std::pair<int64_t, int64_t>> sizes;

private:
    std::mutex mutex_;
};

// --- Probe operations ------------------------------------------------------------------

struct Seen {
    int64_t tag = 0;
    bool objectSink = false, edgeSink = false, annotationSink = false;
    bool hasAuxiliary = false;
    int64_t inW = 0, inH = 0, auxW = 0, auxH = 0;
    Rect64 inSlide, auxSlide, haloRect;
    int32_t inLevel = 0, auxLevel = 0, auxPlanes = 0;
    DataType auxType = DataType::Unknown;
    int64_t auxFirst = -1, auxLast = -1, auxFirst1 = -1;
    bool unboundIsNull = false;
};

std::mutex gMutex;
std::vector<Seen> gSeen;

// The pattern as stored in a UInt16 sample (halo beyond the level has negative coordinates).
int64_t wrap(int64_t v) { return static_cast<uint16_t>(v % 65536); }

uint16_t sampleOf(const BlockView& v, int32_t plane, int64_t x, int64_t y) {
    return reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(v.planeData(plane)) +
                                             y * v.planeRowStrideBytes(plane))[x];
}

// "test.B6.probe" and "test.B6.objects": PerChannel, Byte, halo from int "halo", output
// Byte zeros. Records what its context offers under int "tag". With int "assert" = 1 it
// asserts one parent and one label through whichever sinks it is given.
class ProbeOp : public Operation {
public:
    ProbeOp(const OperationParameters& parameters, bool produces) : parameters_(parameters), produces_(produces) {
        parameters_.getInt("halo", halo_);
        parameters_.getInt("tag", tag_);
        parameters_.getInt("assert", assert_);
    }
    const char* name() const override { return produces_ ? "test.B6.objects" : "test.B6.probe"; }
    ChannelArity arity() const override { return ChannelArity::PerChannel; }
    bool requiresUniformTypes() const override { return false; }
    DataTypeSet supportedTypes() const override { return DataTypeSet().add(DataType::Byte); }
    Halo halo() const override { return Halo::uniform(static_cast<int32_t>(halo_)); }
    DataType outputType(DataType) const override { return DataType::Byte; }
    const OperationParameters& parameters() const override { return parameters_; }
    bool producesObjects() const override { return produces_; }
    void apply(OperationContext& context) override {
        Seen s;
        s.tag = tag_;
        s.objectSink = context.objectSink() != nullptr;
        s.edgeSink = context.edgeSink() != nullptr;
        s.annotationSink = context.annotationSink() != nullptr;
        const BlockView& in = context.input();
        s.inW = in.width();
        s.inH = in.height();
        s.inSlide = in.slideRect();
        s.inLevel = in.level();
        s.haloRect = context.haloRect();
        if (const BlockView* aux = context.auxiliary("coarse")) {
            s.hasAuxiliary = true;
            s.auxW = aux->width();
            s.auxH = aux->height();
            s.auxSlide = aux->slideRect();
            s.auxLevel = aux->level();
            s.auxPlanes = aux->planeCount();
            s.auxType = aux->planeType(0);
            s.auxFirst = sampleOf(*aux, 0, 0, 0);
            s.auxLast = sampleOf(*aux, 0, aux->width() - 1, aux->height() - 1);
            s.auxFirst1 = sampleOf(*aux, 1, 0, 0);
        }
        s.unboundIsNull = context.auxiliary("other") == nullptr;
        if (assert_ == 1) {
            const ObjectId child = ObjectId::fromAnchor(LayerId(1), Point64{3, 4});
            const ObjectId parent = ObjectId::fromAnchor(LayerId(1), Point64{5, 6});
            if (context.edgeSink() != nullptr) {
                context.edgeSink()->assertParent(child, parent);
            }
            if (context.annotationSink() != nullptr) {
                context.annotationSink()->assertLabel(child, LabelId(9));
            }
        }
        std::lock_guard<std::mutex> lock(gMutex);
        gSeen.push_back(s);
    }

private:
    OperationParameters parameters_;
    bool produces_;
    int64_t halo_ = 0, tag_ = 0, assert_ = 0;
};

void registerProbes() {
    ts::registerTestOperations();
    OperationCatalog& catalog = OperationCatalog::instance();
    if (!catalog.isRegistered("test.B6.probe")) {
        catalog.registerFactory(std::make_shared<ts::PrototypeFactory>(
            [](const OperationParameters& p) -> OperationPtr { return std::make_shared<ProbeOp>(p, false); },
            "Records its context; writes zeros", "O(n) in the number of samples"));
        catalog.registerFactory(std::make_shared<ts::PrototypeFactory>(
            [](const OperationParameters& p) -> OperationPtr { return std::make_shared<ProbeOp>(p, true); },
            "Records its context; writes zeros", "O(n) in the number of samples"));
    }
}

OperationParameters probeParams(int64_t halo, int64_t tag, int64_t assertAny = 0) {
    OperationParameters p;
    p.setInt("halo", halo);
    p.setInt("tag", tag);
    p.setInt("assert", assertAny);
    return p;
}

std::vector<Seen> takeSeen() {
    std::lock_guard<std::mutex> lock(gMutex);
    std::vector<Seen> out;
    out.swap(gSeen);
    return out;
}

std::vector<Seen> withTag(const std::vector<Seen>& all, int64_t tag) {
    std::vector<Seen> out;
    for (const Seen& s : all) {
        if (s.tag == tag) {
            out.push_back(s);
        }
    }
    return out;
}

} // namespace

TEST(Bindings, RasterSinkGetsTheLastOutputPerTile) {
    auto scene = pyramidScene();
    PipelineSpec spec;
    spec.analysisLevel(1).operation("test.threshold", OperationParameters());
    Pipeline p(scene, config(100), spec);
    RecordingRasterSink sink;
    p.rasterSink(&sink);
    p.run(nullptr, CancellationToken());

    const detail::RunPlan& plan = detail::PipelineAccess::plan(p);
    EXPECT_EQ(sink.tilesWritten(), plan.grid.tileCount());
    EXPECT_EQ(sink.levels(), std::set<int32_t>{1});
    EXPECT_EQ(sink.pixels(), 333 * 233) << "the tiles union to the region, with no overlap";
    std::vector<Rect64> expected;
    for (int64_t i = 0; i < plan.grid.tileCount(); ++i) {
        expected.push_back(detail::rectToLevel0(plan.grid.tileRectAtLevel(i), plan.levelScale));
    }
    auto order = [](const Rect64& a, const Rect64& b) { return std::tie(a.y, a.x) < std::tie(b.y, b.x); };
    std::vector<Rect64> actual = sink.rects();
    std::sort(actual.begin(), actual.end(), order);
    std::sort(expected.begin(), expected.end(), order);
    EXPECT_EQ(actual, expected) << "each slideRect is the edge-mapped tile rect";
}

TEST(Bindings, RasterSinkReceivesExactlyTheTileRectWhateverTheHalo) {
    registerProbes();
    auto scene = pyramidScene();
    PipelineSpec spec;
    spec.operation("test.B6.probe", probeParams(2, 1)).operation("test.B6.probe", probeParams(3, 2));
    Pipeline p(scene, config(100), spec);
    RecordingRasterSink sink;
    p.rasterSink(&sink);
    p.run(nullptr, CancellationToken());
    takeSeen();
    EXPECT_EQ(sink.pixels(), 1000 * 700);
}

TEST(Bindings, NoRasterSinkDiscards) {
    registerProbes();
    auto scene = pyramidScene();
    PipelineSpec spec;
    spec.operation("test.B6.probe", probeParams(1, 1));
    Pipeline p(scene, config(250), spec);
    takeSeen();
    EXPECT_NO_THROW(p.run(nullptr, CancellationToken()));
    EXPECT_EQ(static_cast<int64_t>(takeSeen().size()), detail::PipelineAccess::plan(p).grid.tileCount());
}

TEST(Bindings, AuxiliaryCoversTheHaloRect) {
    registerProbes();
    auto scene = pyramidScene();
    PipelineSpec spec;
    spec.operation("test.B6.probe", probeParams(2, 1)).operation("test.B6.probe", probeParams(3, 2));
    PatternSource coarse;
    Pipeline p(scene, config(100), spec);
    p.auxiliaryRaster("coarse", &coarse);
    takeSeen();
    p.run(nullptr, CancellationToken());
    const auto seen = takeSeen();
    const int64_t tiles = detail::PipelineAccess::plan(p).grid.tileCount();

    ASSERT_EQ(static_cast<int64_t>(withTag(seen, 1).size()), tiles);
    ASSERT_EQ(static_cast<int64_t>(withTag(seen, 2).size()), tiles);
    for (const int64_t tag : {int64_t{1}, int64_t{2}}) {
        for (const Seen& s : withTag(seen, tag)) {
            ASSERT_TRUE(s.hasAuxiliary);
            EXPECT_EQ(s.auxW, s.inW) << "chain position " << tag;
            EXPECT_EQ(s.auxH, s.inH) << "chain position " << tag;
            EXPECT_EQ(s.auxSlide, s.inSlide) << "chain position " << tag;
            EXPECT_EQ(s.auxLevel, s.inLevel);
            EXPECT_EQ(s.auxPlanes, 2);
            EXPECT_EQ(s.auxType, DataType::UInt16);
            // Level 0: the pattern names the level pixel each sample must cover, so a
            // view over the wrong part of the buffer would show here.
            const int64_t x0 = s.haloRect.x, y0 = s.haloRect.y;
            EXPECT_EQ(s.auxFirst, wrap(x0 + 7 * y0)) << "chain position " << tag;
            EXPECT_EQ(s.auxLast, wrap(x0 + s.inW - 1 + 7 * (y0 + s.inH - 1))) << "chain position " << tag;
            EXPECT_EQ(s.auxFirst1, wrap(x0 + 7 * y0 + 1));
            EXPECT_TRUE(s.unboundIsNull) << "an unbound name gives null";
        }
    }
    EXPECT_EQ(coarse.fills.load(), tiles) << "filled once per tile, whatever the chain length";
    // The one fill covers the first operation's halo rect: 5 pixels around a tile at most.
    for (const auto& size : coarse.sizes) {
        EXPECT_LE(size.first, 110);
        EXPECT_LE(size.second, 110);
    }
}

TEST(Bindings, AuxiliarySlideRectIsEdgeMappedAtACoarseLevel) {
    registerProbes();
    auto scene = pyramidScene();
    PipelineSpec spec;
    spec.analysisLevel(1).operation("test.B6.probe", probeParams(2, 1)).operation("test.B6.probe", probeParams(3, 2));
    PatternSource coarse;
    Pipeline p(scene, config(100), spec);
    p.auxiliaryRaster("coarse", &coarse);
    takeSeen();
    p.run(nullptr, CancellationToken());
    const auto seen = takeSeen();
    ASSERT_FALSE(seen.empty());
    for (const Seen& s : seen) {
        ASSERT_TRUE(s.hasAuxiliary);
        EXPECT_EQ(s.auxSlide, s.inSlide);
        EXPECT_EQ(s.auxSlide, s.haloRect);
        EXPECT_EQ(s.auxLevel, 1);
        EXPECT_EQ(s.auxW, s.inW);
        EXPECT_EQ(s.auxH, s.inH);
    }
}

TEST(Bindings, AuxiliaryReplaceAndRemove) {
    registerProbes();
    auto scene = pyramidScene();
    PipelineSpec spec;
    spec.operation("test.B6.probe", probeParams(0, 1));
    PatternSource first, second;
    Pipeline p(scene, config(500), spec);

    p.auxiliaryRaster("coarse", &first);
    p.auxiliaryRaster("coarse", &second); // same name replaces
    takeSeen();
    p.run(nullptr, CancellationToken());
    EXPECT_EQ(first.fills.load(), 0);
    EXPECT_GT(second.fills.load(), 0);
    for (const Seen& s : takeSeen()) {
        EXPECT_TRUE(s.hasAuxiliary);
    }

    p.auxiliaryRaster("coarse", nullptr); // null removes
    const int64_t before = second.fills.load();
    p.run(nullptr, CancellationToken());
    EXPECT_EQ(second.fills.load(), before);
    for (const Seen& s : takeSeen()) {
        EXPECT_FALSE(s.hasAuxiliary);
    }
    EXPECT_NO_THROW(p.auxiliaryRaster("coarse", nullptr)) << "removing an absent name is not an error";

    EXPECT_THROW(p.auxiliaryRaster(nullptr, &first), InvalidArgumentError);
    EXPECT_THROW(p.auxiliaryRaster("", &first), InvalidArgumentError);
    EXPECT_THROW(p.auxiliaryRaster(nullptr, nullptr), InvalidArgumentError);
    EXPECT_THROW(p.auxiliaryRaster("", nullptr), InvalidArgumentError);
}

TEST(Bindings, EdgeAndAnnotationSinksReachOperations) {
    registerProbes();
    auto scene = pyramidScene();
    PipelineSpec spec;
    spec.operation("test.B6.probe", probeParams(0, 1, 1)).operation("test.B6.probe", probeParams(0, 2, 1));
    RecordingEdgeSink edges;
    RecordingAnnotationSink annotations;
    Pipeline p(scene, config(500), spec);
    p.edgeSink(&edges).annotationSink(&annotations);
    takeSeen();
    p.run(nullptr, CancellationToken());
    const int64_t tiles = detail::PipelineAccess::plan(p).grid.tileCount();
    // Every operation, producer or not, gets both sinks: two operations on every tile.
    for (const Seen& s : takeSeen()) {
        EXPECT_TRUE(s.edgeSink);
        EXPECT_TRUE(s.annotationSink);
    }
    ASSERT_EQ(static_cast<int64_t>(edges.parents().size()), 2 * tiles);
    ASSERT_EQ(static_cast<int64_t>(annotations.labels().size()), 2 * tiles);
    const ObjectId child = ObjectId::fromAnchor(LayerId(1), Point64{3, 4});
    const ObjectId parent = ObjectId::fromAnchor(LayerId(1), Point64{5, 6});
    for (const auto& e : edges.parents()) {
        EXPECT_TRUE(e.first == child);
        EXPECT_TRUE(e.second == parent);
    }
    for (const auto& l : annotations.labels()) {
        EXPECT_TRUE(l.first == child);
        EXPECT_EQ(l.second, 9);
    }

    // Unbound: both are null, and the operation asserts nothing.
    Pipeline bare(scene, config(500), spec);
    bare.run(nullptr, CancellationToken());
    for (const Seen& s : takeSeen()) {
        EXPECT_FALSE(s.edgeSink);
        EXPECT_FALSE(s.annotationSink);
    }
    EXPECT_EQ(static_cast<int64_t>(edges.parents().size()), 2 * tiles);
}

TEST(Bindings, PerChannelObjectProducerNeedsExactlyOneChannel) {
    // One plane per channel would emit each object once per channel into one layer.
    registerProbes();
    const auto scene = ts::makeSyntheticScene({{200, 100, 0, 0}},
                                              {slideio::DataType::DT_Byte, slideio::DataType::DT_Byte}, texture);
    PipelineSpec both;
    both.operation("test.B6.objects", probeParams(0, 1));
    Pipeline refused(scene, config(100), both);
    refused.bindLayer("cells", LayerId(1));
    try {
        refused.validate();
        FAIL() << "validate() accepted a per-channel object producer over two channels";
    } catch (const InvalidArgumentError& e) {
        const std::string message = e.what();
        EXPECT_STREQ(e.operationName(), "test.B6.objects");
        EXPECT_NE(message.find("exactly one channel"), std::string::npos) << message;
    }

    PipelineSpec one = both;
    one.channels(ChannelSelection::single(1));
    Pipeline accepted(scene, config(100), one);
    accepted.bindLayer("cells", LayerId(1));
    EXPECT_NO_THROW(accepted.validate());

    // A per-channel operation producing no objects still takes every channel.
    PipelineSpec raster;
    raster.operation("test.B6.probe", probeParams(0, 1));
    Pipeline plain(scene, config(100), raster);
    EXPECT_NO_THROW(plain.validate());
}

TEST(Bindings, OnlyObjectProducersGetAnObjectSink) {
    registerProbes();
    auto scene = pyramidScene();
    PipelineSpec spec;
    spec.operation("test.B6.probe", probeParams(0, 1)).operation("test.B6.objects", probeParams(0, 2));
    ts::CollectingObjectSink objects;
    Pipeline p(scene, config(500), spec);
    p.bindLayer("cells", LayerId(1));
    takeSeen();
    p.runInto(objects, nullptr, CancellationToken());
    const auto seen = takeSeen();
    ASSERT_FALSE(withTag(seen, 1).empty());
    ASSERT_FALSE(withTag(seen, 2).empty());
    for (const Seen& s : withTag(seen, 1)) {
        EXPECT_FALSE(s.objectSink) << "a non-producer never gets an object sink";
    }
    for (const Seen& s : withTag(seen, 2)) {
        EXPECT_TRUE(s.objectSink);
    }
}
