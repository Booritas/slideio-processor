// Requirements: UR-IMG-5, UR-STG-3, UR-EXE-1, UR-QA-3
#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/control.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/core/raster_mask.hpp>
#include <slideio/processor/pipeline/operation_catalog.hpp>
#include <slideio/processor/pipeline/pipeline.hpp>

#include <pipeline/detail/pipeline_impl.hpp>
#include <pipeline/detail/restriction.hpp>
#include <pipeline/test_support/disc_mask_source.hpp>
#include <pipeline/test_support/synthetic_scene.hpp>
#include <pipeline/test_support/test_operations.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

using namespace slideio::processor;
namespace ts = slideio::processor::testing_support;

namespace {

constexpr int64_t kSide = 512;
constexpr int32_t kTile = 64;

double texture(int, int64_t x, int64_t y) { return static_cast<double>((x * 37 + y * 91 + (x * y) % 13) % 256); }

// Separate rectangular blobs, one per 13 x 11 cell; many straddle tile seams and polygon edges.
double blobs(int, int64_t x, int64_t y) {
    const int64_t cx = x / 13;
    const int64_t cy = y / 11;
    const int64_t w = 2 + (cx * 7 + cy * 3) % 5;
    const int64_t h = 2 + (cx * 5 + cy * 11) % 5;
    return x % 13 >= 1 && x % 13 < 1 + w && y % 11 >= 1 && y % 11 < 1 + h ? 255.0 : 0.0;
}

Configuration config() {
    return ConfigurationBuilder().tileSize(kTile, kTile).tileSizeFromSlide(false).workerCount(4).build();
}

Polygon triangle(int64_t x0, int64_t y0, int64_t x1, int64_t y1) {
    const Point64 points[] = {{x0, y0}, {x1, y0}, {x0, y1}};
    Polygon p;
    p.addRing(points, 3, false);
    return p;
}

Polygon rectangle(int64_t x0, int64_t y0, int64_t x1, int64_t y1) {
    const Point64 points[] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    Polygon p;
    p.addRing(points, 4, false);
    return p;
}

Polygon holedPolygon() {
    const Point64 outer[] = {{64, 64}, {448, 64}, {448, 448}, {64, 448}};
    const Point64 hole[] = {{200, 200}, {330, 200}, {330, 330}, {200, 330}};
    Polygon p;
    p.addRing(outer, 4, false);
    p.addRing(hole, 4, true);
    return p;
}

class RecordingObserver : public ProgressObserver {
public:
    void onProgress(const char*, double, int64_t completed, int64_t total) override {
        std::lock_guard<std::mutex> lock(mutex_);
        calls.emplace_back(completed, total);
    }
    std::mutex mutex_;
    std::vector<std::pair<int64_t, int64_t>> calls;
};

PipelineSpec spec1(const char* operation, const OperationParameters& parameters = OperationParameters()) {
    PipelineSpec spec;
    spec.operation(operation, parameters);
    return spec;
}

OperationParameters radius(int64_t r) {
    OperationParameters p;
    p.setInt("radius", r);
    return p;
}

std::shared_ptr<ts::SyntheticSource> source(ts::PixelFunction f) {
    ts::registerTestOperations();
    return std::make_shared<ts::SyntheticSource>(std::vector<ts::SyntheticLevel>{{kSide, kSide, 0, 0}},
                                                 std::vector<slideio::DataType>{slideio::DataType::DT_Byte},
                                                 std::move(f));
}

// Runs the pipeline with a progress observer; returns the (completed, total) calls.
std::vector<std::pair<int64_t, int64_t>> progressOf(Pipeline& p) {
    RecordingObserver observer;
    p.run(&observer, CancellationToken());
    return observer.calls;
}

std::vector<ObjectId> sortedIds(const ts::CollectingObjectSink& sink) {
    std::vector<ObjectId> ids;
    for (const ObjectRecord& r : sink.records()) {
        ids.push_back(r.id());
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

// Keeps every raster tile by its level-0 rect.
class TileKeepingRasterSink : public RasterSink {
public:
    using Key = std::tuple<int64_t, int64_t, int64_t, int64_t>;
    void writeTile(const BlockView& tile) override {
        std::vector<uint8_t> bytes;
        const int64_t rowBytes = tile.width() * dataTypeSizeBytes(tile.planeType(0));
        for (int64_t y = 0; y < tile.height(); ++y) {
            const auto* row = static_cast<const uint8_t*>(tile.planeData(0)) + y * tile.planeRowStrideBytes(0);
            bytes.insert(bytes.end(), row, row + rowBytes);
        }
        const Rect64 r = tile.slideRect();
        std::lock_guard<std::mutex> lock(mutex_);
        tiles_[Key{r.x, r.y, r.width, r.height}] = std::move(bytes);
    }
    void flush() override {}
    int64_t tilesWritten() const override { return static_cast<int64_t>(tiles().size()); }
    std::map<Key, std::vector<uint8_t>> tiles() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return tiles_;
    }

private:
    mutable std::mutex mutex_;
    std::map<Key, std::vector<uint8_t>> tiles_;
};

// "test.maskComponents": test.components that also reads context.mask(), and records what
// each apply saw.
struct MaskObservation {
    Rect64 tileRectAtLevel;
    Rect64 haloRectAtLevel;
    bool maskNull;
    RasterMask mask;
};

std::mutex g_observationMutex;
std::vector<MaskObservation> g_observations;

class MaskReadingComponents : public ts::ComponentsOperation {
public:
    using ts::ComponentsOperation::ComponentsOperation;
    const char* name() const override { return "test.maskComponents"; }
    void apply(OperationContext& context) override {
        const RasterMask* mask = context.mask();
        {
            std::lock_guard<std::mutex> lock(g_observationMutex);
            g_observations.push_back(MaskObservation{context.tileRectAtLevel(), context.haloRectAtLevel(),
                                                     mask == nullptr, mask != nullptr ? *mask : RasterMask()});
        }
        ts::ComponentsOperation::apply(context);
    }
};

OperationPtr makeMaskReading(const OperationParameters& p) { return std::make_unique<MaskReadingComponents>(p); }

void registerMaskReading() {
    ts::registerTestOperations();
    OperationCatalog& catalog = OperationCatalog::instance();
    if (!catalog.isRegistered("test.maskComponents")) {
        catalog.registerFactory(
            std::make_shared<ts::PrototypeFactory>(&makeMaskReading, "test reference", "test: linear"));
    }
}

} // namespace

// The grid of a polygon run spans the polygon's bounding box, not the scene.
TEST(Restriction, PolygonGridSpansItsBoundingBox) {
    auto scene = ts::makeSyntheticScene(source(texture));
    PipelineSpec spec = spec1("test.threshold");
    spec.restrictToPolygon(triangle(70, 70, 330, 200));  // bbox 260 x 130 -> 5 x 3 tiles of 64
    Pipeline p(scene, config(), spec);
    const auto calls = progressOf(p);
    ASSERT_FALSE(calls.empty());
    EXPECT_EQ(calls.back().second, 15);
    EXPECT_EQ(calls.back().first, 15);
    EXPECT_LT(calls.back().second, (kSide / kTile) * (kSide / kTile));
}

// Tiles whose restriction mask is empty read no pixel but still count as complete.
TEST(Restriction, EmptyMaskTilesReadNoPixels) {
    auto src = source(texture);
    auto scene = ts::makeSyntheticScene(src);
    const int64_t x0 = 64, y0 = 64, x1 = 448, y1 = 448;
    ts::DiscMaskSource disc(200.0, 200.0, 60.0);
    PipelineSpec spec = spec1("test.threshold");
    spec.restrictToPolygon(rectangle(x0, y0, x1, y1));
    Pipeline p(scene, config(), spec);
    p.maskSource(&disc);

    // Independent expectation: a tile of the bbox grid is read iff a pixel is in the rectangle and the disc.
    int64_t expectedReads = 0;
    int64_t tiles = 0;
    for (int64_t ty = y0; ty < y1; ty += kTile) {
        for (int64_t tx = x0; tx < x1; tx += kTile) {
            ++tiles;
            bool any = false;
            for (int64_t y = ty; y < std::min(ty + kTile, y1) && !any; ++y) {
                for (int64_t x = tx; x < std::min(tx + kTile, x1) && !any; ++x) {
                    any = disc.contains(static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5);
                }
            }
            expectedReads += any ? 1 : 0;
        }
    }
    ASSERT_GT(expectedReads, 0);
    ASSERT_LT(expectedReads, tiles / 2) << "the fixture should leave most tiles empty";

    const auto calls = progressOf(p);
    EXPECT_EQ(src->readCount(), expectedReads);
    ASSERT_EQ(static_cast<int64_t>(calls.size()), tiles) << "every tile reports progress, skipped ones included";
    EXPECT_EQ(calls.back().first, tiles);
    EXPECT_EQ(calls.back().second, tiles);
}

// An object is stored iff the tile mask holds its anchor (UR-STG-3).
TEST(Restriction, AnchorDecides) {
    auto scene = ts::makeSyntheticScene(source(blobs));
    // The bounding box starts on a tile corner, so the restricted grid's tiles are the unrestricted run's tiles.
    const Polygon polygon = triangle(64, 64, 448, 448);
    PipelineSpec restrictedSpec = spec1("test.components");
    restrictedSpec.restrictToPolygon(polygon);

    ts::CollectingObjectSink all;
    Pipeline unrestricted(scene, config(), spec1("test.components"));
    unrestricted.bindLayer("objects", LayerId(3));
    unrestricted.runInto(all, nullptr, CancellationToken());

    ts::CollectingObjectSink stored;
    Pipeline restricted(scene, config(), restrictedSpec);
    restricted.bindLayer("objects", LayerId(3));
    restricted.runInto(stored, nullptr, CancellationToken());

    RasterMask expectedMask(Rect64{0, 0, kSide, kSide}, 0, 1.0);
    rasteriseInto(expectedMask, polygon);

    std::vector<ObjectId> expected;
    int64_t excludedInsideBounds = 0;
    for (const ObjectRecord& r : all.records()) {
        const bool admitted = expectedMask.testLevel0(r.geometry().anchor());
        if (admitted) {
            expected.push_back(r.id());
        } else {
            // An excluded object that still lies within the polygon's bounding box: it is cut off by the polygon edge, not by the grid.
            const Rect64 box = r.geometry().boundingBox();
            if (!box.intersected(polygon.boundingBox()).isEmpty()) {
                ++excludedInsideBounds;
            }
        }
    }
    std::sort(expected.begin(), expected.end());
    ASSERT_GT(expected.size(), 50u);
    ASSERT_GT(excludedInsideBounds, 10) << "the fixture should exclude blobs that lie within the polygon's bounding box";
    EXPECT_LT(expected.size(), all.records().size());
    EXPECT_EQ(sortedIds(stored), expected);
    for (const ObjectRecord& r : stored.records()) {
        EXPECT_TRUE(expectedMask.testLevel0(r.geometry().anchor())) << "an object with its anchor outside was stored";
    }
}


// With a polygon and a mask source both bound, an object is stored exactly when its
// anchor lies in the polygon and in the mask: no stored anchor lies outside their
// intersection, and objects inside the polygon but outside the mask are dropped.
TEST(Restriction, AnchorDecidesWithPolygonAndMaskSource) {
    auto scene = ts::makeSyntheticScene(source(blobs));
    const Polygon polygon = triangle(64, 64, 448, 448);
    ts::DiscMaskSource disc(200.0, 200.0, 120.0);
    PipelineSpec restrictedSpec = spec1("test.components");
    restrictedSpec.restrictToPolygon(polygon);

    ts::CollectingObjectSink all;
    Pipeline unrestricted(scene, config(), spec1("test.components"));
    unrestricted.bindLayer("objects", LayerId(3));
    unrestricted.runInto(all, nullptr, CancellationToken());

    ts::CollectingObjectSink stored;
    Pipeline restricted(scene, config(), restrictedSpec);
    restricted.bindLayer("objects", LayerId(3)).maskSource(&disc);
    restricted.runInto(stored, nullptr, CancellationToken());

    RasterMask polygonMask(Rect64{0, 0, kSide, kSide}, 0, 1.0);
    rasteriseInto(polygonMask, polygon);
    const auto inDisc = [&disc](const Point64& p) {
        return disc.contains(static_cast<double>(p.x) + 0.5, static_cast<double>(p.y) + 0.5);
    };

    std::vector<ObjectId> expected;
    int64_t inPolygonOutsideDisc = 0;
    int64_t inDiscOutsidePolygon = 0;
    for (const ObjectRecord& r : all.records()) {
        const Point64 anchor = r.geometry().anchor();
        const bool inPolygon = polygonMask.testLevel0(anchor);
        if (inPolygon && inDisc(anchor)) {
            expected.push_back(r.id());
        } else if (inPolygon) {
            ++inPolygonOutsideDisc;
        } else if (inDisc(anchor)) {
            ++inDiscOutsidePolygon;
        }
    }
    std::sort(expected.begin(), expected.end());
    ASSERT_GT(expected.size(), 20u);
    ASSERT_GT(inPolygonOutsideDisc, 10) << "the fixture should have objects the mask alone excludes";
    ASSERT_GT(inDiscOutsidePolygon, 10) << "the fixture should have objects the polygon alone excludes";
    EXPECT_EQ(sortedIds(stored), expected);
    for (const ObjectRecord& r : stored.records()) {
        const Point64 anchor = r.geometry().anchor();
        EXPECT_TRUE(polygonMask.testLevel0(anchor) && inDisc(anchor))
            << "an object with its anchor outside the polygon and mask intersection was stored";
    }
}

// Filtering never alters a pixel: the tiles a restricted run processes equal the unrestricted ones.
TEST(Restriction, PixelsAreNeverAltered) {
    auto scene = ts::makeSyntheticScene(source(texture));
    TileKeepingRasterSink reference;
    {
        Pipeline p(scene, config(), spec1("test.boxMean", radius(2)));
        p.rasterSink(&reference);
        p.run(nullptr, CancellationToken());
    }
    PipelineSpec spec = spec1("test.boxMean", radius(2));
    spec.restrictToPolygon(triangle(64, 64, 448, 448));
    TileKeepingRasterSink restricted;
    Pipeline p(scene, config(), spec);
    p.rasterSink(&restricted);
    p.run(nullptr, CancellationToken());

    const auto ref = reference.tiles();
    const auto got = restricted.tiles();
    ASSERT_EQ(ref.size(), 64u);
    ASSERT_GT(got.size(), 10u);
    EXPECT_LT(got.size(), 36u) << "the triangle's far corner should be skipped";
    for (const auto& tile : got) {
        const auto match = ref.find(tile.first);
        ASSERT_TRUE(match != ref.end());
        EXPECT_TRUE(match->second == tile.second) << "a restricted tile differs from the unrestricted one";
    }
}

// A bound TileMaskSource never narrows the grid.
TEST(Restriction, MaskSourceDoesNotNarrowTheGrid) {
    auto src = source(texture);
    auto scene = ts::makeSyntheticScene(src);
    ts::DiscMaskSource disc(256.0, 256.0, 40.0);
    Pipeline p(scene, config(), spec1("test.threshold"));
    p.maskSource(&disc);
    const auto calls = progressOf(p);
    const int64_t whole = (kSide / kTile) * (kSide / kTile);
    ASSERT_FALSE(calls.empty());
    EXPECT_EQ(calls.back().second, whole);
    EXPECT_EQ(calls.back().first, whole);
    EXPECT_EQ(disc.fillCount(), whole) << "fillMask once per tile";
    EXPECT_LT(src->readCount(), whole);
    EXPECT_GT(src->readCount(), 0);
}

// context.mask() is the tile's mask when restricted and null otherwise; an operation that
// ignores it and one that reads it store the same objects.
TEST(Restriction, ContextMaskIsAdvisory) {
    registerMaskReading();
    auto scene = ts::makeSyntheticScene(source(blobs));

    {  // Unrestricted: every apply sees a null mask.
        g_observations.clear();
        ts::CollectingObjectSink sink;
        Pipeline p(scene, config(), spec1("test.maskComponents"));
        p.bindLayer("objects", LayerId(3));
        p.runInto(sink, nullptr, CancellationToken());
        ASSERT_EQ(g_observations.size(), 64u);
        for (const MaskObservation& o : g_observations) {
            EXPECT_TRUE(o.maskNull);
        }
    }

    const int64_t x0 = 64, y0 = 64, x1 = 400, y1 = 330;
    auto restrictedSpec = [&](const char* op) {
        PipelineSpec spec = spec1(op);
        spec.restrictToPolygon(rectangle(x0, y0, x1, y1));
        return spec;
    };
    g_observations.clear();
    ts::CollectingObjectSink reading;
    {
        Pipeline p(scene, config(), restrictedSpec("test.maskComponents"));
        p.bindLayer("objects", LayerId(3));
        p.runInto(reading, nullptr, CancellationToken());
    }
    ts::CollectingObjectSink ignoring;
    {
        Pipeline p(scene, config(), restrictedSpec("test.components"));
        p.bindLayer("objects", LayerId(3));
        p.runInto(ignoring, nullptr, CancellationToken());
    }
    EXPECT_FALSE(reading.records().empty());
    EXPECT_EQ(sortedIds(reading), sortedIds(ignoring));

    ASSERT_FALSE(g_observations.empty());
    for (const MaskObservation& o : g_observations) {
        ASSERT_FALSE(o.maskNull);
        // The chain has no halo, so the mask covers the tile and holds exactly the rectangle.
        EXPECT_EQ(o.mask.regionAtLevel(), o.haloRectAtLevel);
        EXPECT_EQ(o.haloRectAtLevel, o.tileRectAtLevel);
        const Rect64 region = o.mask.regionAtLevel();
        for (int64_t y = 0; y < region.height; ++y) {
            for (int64_t x = 0; x < region.width; ++x) {
                const int64_t lx = region.x + x;
                const int64_t ly = region.y + y;
                const bool inside = lx >= x0 && lx < x1 && ly >= y0 && ly < y1;
                ASSERT_EQ(o.mask.test(Point64{lx, ly}), inside);
            }
        }
    }
}

namespace {

// admits(anchor) against the answer of the tile mask of the tile holding the anchor.
void checkAdmits(int32_t level, const PipelineSpec& baseSpec, ts::DiscMaskSource* disc, bool expectRefusals = true) {
    ts::registerTestOperations();
    auto scene = ts::makeSyntheticScene({{kSide, kSide, 0, 0}, {kSide / 2, kSide / 2, 0, 0}},
                                        {slideio::DataType::DT_Byte}, texture);
    PipelineSpec spec = baseSpec;
    spec.analysisLevel(level);
    Pipeline p(scene, config(), spec);
    p.maskSource(disc);
    p.validate();
    auto& impl = detail::PipelineAccess::impl(p);
    const detail::RunPlan& plan = detail::PipelineAccess::plan(p);
    const detail::RestrictionFilter filter(plan, impl.spec, *impl.selector, impl.maskSource);
    ASSERT_TRUE(filter.restricted());

    const double scale = plan.levelScale;
    std::mt19937_64 rng(20261116);
    std::uniform_int_distribution<int64_t> dist(0, kSide - 1);
    int admitted = 0;
    int refused = 0;
    int checked = 0;
    for (int attempts = 0; checked < 1000 && attempts < 200000; ++attempts) {
        const Point64 anchor{dist(rng), dist(rng)};
        const Point64 pixel{static_cast<int64_t>(std::floor(static_cast<double>(anchor.x) / scale)),
                            static_cast<int64_t>(std::floor(static_cast<double>(anchor.y) / scale))};
        int64_t tile = -1;
        for (int64_t i = 0; i < plan.grid.tileCount() && tile < 0; ++i) {
            if (plan.grid.tileRectAtLevel(i).contains(pixel)) {
                tile = i;
            }
        }
        if (tile < 0) {
            continue;  // outside the grid region: no tile holds it
        }
        const RasterMask mask = filter.buildTileMask(tile);
        const bool expected = mask.testLevel0(anchor);
        EXPECT_EQ(filter.admits(anchor), expected);
        (expected ? admitted : refused)++;
        ++checked;
    }
    EXPECT_GT(admitted, 50);
    ASSERT_EQ(checked, 1000) << "the anchor draw hit its attempt cap";
    if (expectRefusals) {
        EXPECT_GT(refused, 50);
    }
}

} // namespace

// admits() answers as the tile mask does, through the ROI, the polygon and the mask source.
TEST(Restriction, AdmitsOutsideATile) {
    ts::DiscMaskSource disc(250.0, 230.0, 150.0);
    const Polygon polygon = triangle(40, 50, 480, 470);
    for (const int32_t level : {0, 1}) {
        {  // ROI only
            PipelineSpec spec = spec1("test.boxMean", radius(3));
            spec.restrictTo(Rect64{33, 41, 301, 257});
            checkAdmits(level, spec, nullptr, false);  // the ROI is the grid: no anchor inside it is refused
        }
        {  // polygon only
            PipelineSpec spec = spec1("test.boxMean", radius(3));
            spec.restrictToPolygon(polygon);
            checkAdmits(level, spec, nullptr);
        }
        {  // polygon with a hole
            PipelineSpec spec = spec1("test.boxMean", radius(3));
            spec.restrictToPolygon(holedPolygon());
            checkAdmits(level, spec, nullptr);
        }
        {  // mask source only
            checkAdmits(level, spec1("test.boxMean", radius(3)), &disc);
        }
        {  // all three
            PipelineSpec spec = spec1("test.boxMean", radius(3));
            spec.restrictTo(Rect64{20, 30, 500, 450}).restrictToPolygon(polygon);
            checkAdmits(level, spec, &disc);
        }
    }
}

// An anchor inside a polygon's hole is refused, in a run and through admits().
TEST(Restriction, HoleRefusesItsAnchors) {
    auto scene = ts::makeSyntheticScene(source(blobs));
    const Polygon polygon = holedPolygon();
    auto inHole = [](const Point64& a) { return a.x >= 200 && a.x < 330 && a.y >= 200 && a.y < 330; };

    ts::CollectingObjectSink all;
    Pipeline unrestricted(scene, config(), spec1("test.components"));
    unrestricted.bindLayer("objects", LayerId(3));
    unrestricted.runInto(all, nullptr, CancellationToken());
    int64_t inHoleUnrestricted = 0;
    for (const ObjectRecord& r : all.records()) {
        inHoleUnrestricted += inHole(r.geometry().anchor()) ? 1 : 0;
    }
    ASSERT_GT(inHoleUnrestricted, 20);

    PipelineSpec spec = spec1("test.components");
    spec.restrictToPolygon(polygon);
    ts::CollectingObjectSink stored;
    Pipeline restricted(scene, config(), spec);
    restricted.bindLayer("objects", LayerId(3));
    restricted.runInto(stored, nullptr, CancellationToken());
    ASSERT_FALSE(stored.records().empty());
    int64_t deep = 0;
    for (const ObjectRecord& r : stored.records()) {
        const Point64 a = r.geometry().anchor();
        // The conservative fill keeps the hole's edge pixels, so test the hole's interior.
        EXPECT_FALSE(a.x >= 202 && a.x < 328 && a.y >= 202 && a.y < 328) << "an anchor inside the hole was stored";
        deep += inHole(a) ? 1 : 0;
    }
    EXPECT_EQ(deep, 0) << "no anchor of the hole is expected at all with a pixel-aligned hole";

    restricted.validate();
    auto& impl = detail::PipelineAccess::impl(restricted);
    const detail::RestrictionFilter filter(detail::PipelineAccess::plan(restricted), impl.spec, *impl.selector,
                                           impl.maskSource);
    EXPECT_FALSE(filter.admits(Point64{260, 260}));
    EXPECT_TRUE(filter.admits(Point64{100, 100}));
    EXPECT_FALSE(filter.admits(Point64{30, 30}));
}

// admits() over many anchors in one tile builds that tile's mask once: the mask
// source is called once per tile, not once per anchor.
TEST(Restriction, AdmitsFillsOneMaskPerTile) {
    ts::registerTestOperations();
    auto scene = ts::makeSyntheticScene({{kSide, kSide, 0, 0}}, {slideio::DataType::DT_Byte}, texture);
    ts::DiscMaskSource disc(250.0, 230.0, 150.0);
    Pipeline p(scene, config(), spec1("test.boxMean", radius(3)));
    p.maskSource(&disc);
    p.validate();
    auto& impl = detail::PipelineAccess::impl(p);
    const detail::RunPlan& plan = detail::PipelineAccess::plan(p);
    const detail::RestrictionFilter filter(plan, impl.spec, *impl.selector, impl.maskSource);

    // Tiles 25 and 26 hold (100, 230), the disc's left edge: some anchors in, some out.
    const int64_t before = disc.fillCount();
    int64_t admitted = 0;
    int64_t refused = 0;
    for (const int64_t tile : {plan.grid.tileIndex(1, 3), plan.grid.tileIndex(2, 3)}) {
        const Rect64 r = plan.grid.tileRect(tile);
        for (int64_t y = r.y; y < r.y + r.height; ++y) {
            for (int64_t x = r.x; x < r.x + r.width; ++x) {
                const bool expected = disc.contains(static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5);
                ASSERT_EQ(filter.admits(Point64{x, y}), expected) << x << "," << y;
                (expected ? admitted : refused)++;
            }
        }
    }
    EXPECT_GT(admitted, 100);
    EXPECT_GT(refused, 100);
    EXPECT_EQ(disc.fillCount() - before, 2) << "one fillMask per tile, for 2 x 4096 anchors";
}

// A released tile's mask is built again on the next admits() in that tile; the other
// tiles' masks stay cached.
TEST(Restriction, ReleasedTileMaskIsRebuilt) {
    ts::registerTestOperations();
    auto scene = ts::makeSyntheticScene({{kSide, kSide, 0, 0}}, {slideio::DataType::DT_Byte}, texture);
    ts::DiscMaskSource disc(250.0, 230.0, 150.0);
    Pipeline p(scene, config(), spec1("test.boxMean", radius(3)));
    p.maskSource(&disc);
    p.validate();
    auto& impl = detail::PipelineAccess::impl(p);
    const detail::RunPlan& plan = detail::PipelineAccess::plan(p);
    const detail::RestrictionFilter filter(plan, impl.spec, *impl.selector, impl.maskSource);

    const int64_t first = plan.grid.tileIndex(1, 3);
    const int64_t second = plan.grid.tileIndex(2, 3);
    const int64_t before = disc.fillCount();
    EXPECT_EQ(filter.cachedTileMaskCount(), 0);
    filter.admits(Point64{100, 230});  // tile (1, 3)
    filter.admits(Point64{140, 230});  // tile (2, 3)
    EXPECT_EQ(filter.cachedTileMaskCount(), 2);
    filter.releaseTileMask(first);
    filter.releaseTileMask(first);  // releasing twice, or a tile never cached, is harmless
    filter.releaseTileMask(plan.grid.tileIndex(7, 7));
    EXPECT_EQ(filter.cachedTileMaskCount(), 1);
    EXPECT_EQ(filter.admits(Point64{101, 231}), disc.contains(101.5, 231.5));
    EXPECT_EQ(filter.admits(Point64{141, 231}), disc.contains(141.5, 231.5));
    EXPECT_EQ(disc.fillCount() - before, 3) << "only the released tile is built again";
    filter.releaseTileMask(first);
    filter.releaseTileMask(second);
    EXPECT_EQ(filter.cachedTileMaskCount(), 0);

    // An unrestricted filter caches nothing.
    Pipeline open(scene, config(), spec1("test.boxMean", radius(3)));
    open.validate();
    auto& openImpl = detail::PipelineAccess::impl(open);
    const detail::RestrictionFilter none(detail::PipelineAccess::plan(open), openImpl.spec, *openImpl.selector,
                                         openImpl.maskSource);
    EXPECT_TRUE(none.admits(Point64{100, 230}));
    EXPECT_EQ(none.cachedTileMaskCount(), 0);
}

// admits() from several threads at once answers as from one, and still fills each
// tile's mask once.
TEST(Restriction, AdmitsIsThreadSafe) {
    ts::registerTestOperations();
    auto scene = ts::makeSyntheticScene({{kSide, kSide, 0, 0}}, {slideio::DataType::DT_Byte}, texture);
    ts::DiscMaskSource disc(250.0, 230.0, 150.0);
    PipelineSpec spec = spec1("test.boxMean", radius(3));
    spec.restrictToPolygon(triangle(40, 50, 480, 470));
    Pipeline p(scene, config(), spec);
    p.maskSource(&disc);
    p.validate();
    auto& impl = detail::PipelineAccess::impl(p);
    const detail::RunPlan& plan = detail::PipelineAccess::plan(p);
    const detail::RestrictionFilter filter(plan, impl.spec, *impl.selector, impl.maskSource);
    const detail::RestrictionFilter reference(plan, impl.spec, *impl.selector, impl.maskSource);

    // Four tiles along the disc's edge and the triangle's hypotenuse.
    const std::vector<int64_t> tiles{plan.grid.tileIndex(1, 3), plan.grid.tileIndex(2, 3),
                                     plan.grid.tileIndex(3, 4), plan.grid.tileIndex(4, 3)};
    std::vector<Point64> anchors;
    for (const int64_t tile : tiles) {
        const Rect64 r = plan.grid.tileRect(tile);
        for (int64_t y = r.y; y < r.y + r.height; y += 3) {
            for (int64_t x = r.x; x < r.x + r.width; x += 2) {
                anchors.push_back(Point64{x, y});
            }
        }
    }
    std::vector<uint8_t> expected;
    for (const Point64& a : anchors) {
        expected.push_back(reference.admits(a) ? 1 : 0);
    }
    ASSERT_NE(std::count(expected.begin(), expected.end(), 1), 0);
    ASSERT_NE(std::count(expected.begin(), expected.end(), 0), 0);

    const int64_t before = disc.fillCount();
    constexpr int kThreads = 8;
    std::vector<std::vector<uint8_t>> answers(kThreads);
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t]() {
            // Each thread walks the anchors from a different start, so threads meet in every tile.
            const size_t n = anchors.size();
            answers[t].assign(n, 0);
            for (size_t i = 0; i < n; ++i) {
                const size_t j = (i + static_cast<size_t>(t) * n / kThreads) % n;
                answers[t][j] = filter.admits(anchors[j]) ? 1 : 0;
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    for (int t = 0; t < kThreads; ++t) {
        EXPECT_EQ(answers[t], expected) << "thread " << t;
    }
    EXPECT_EQ(disc.fillCount() - before, static_cast<int64_t>(tiles.size())) << "one fillMask per tile";
}
