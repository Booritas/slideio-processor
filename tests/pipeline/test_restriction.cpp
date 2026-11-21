// Requirements: UR-IMG-5, UR-STG-3, UR-EXE-1, UR-QA-3
#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/control.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/core/memory_budget.hpp>
#include <slideio/processor/core/raster_mask.hpp>
#include <slideio/processor/pipeline/operation_catalog.hpp>
#include <slideio/processor/pipeline/pipeline.hpp>
#include <slideio/processor/pipeline/tile_grid.hpp>

#include <pipeline/detail/pipeline_impl.hpp>
#include <pipeline/detail/restriction.hpp>
#include <pipeline/test_support/disc_mask_source.hpp>
#include <pipeline/test_support/synthetic_scene.hpp>
#include <pipeline/test_support/test_operations.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
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

// The level pixel holding a level-0 anchor, as the restriction tests it: the top-left of the
// anchor's one-pixel rectangle mapped to the level.
Point64 levelPixelOracle(const LevelSelector& selector, Point64 anchor, int32_t level) {
    const Rect64 pixel = selector.mapToLevel(Rect64{anchor.x, anchor.y, 1, 1}, level);
    return Point64{pixel.x, pixel.y};
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

// The grid of a polygon run producing objects spans the level, so the polygon's edge is no
// grid edge; a run producing none keeps the grid over the polygon's bounding box. rect()
// stays the bounding box either way.
TEST(Restriction, PolygonObjectRunGridSpansTheLevel) {
    registerMaskReading();
    auto scene = ts::makeSyntheticScene(source(blobs));
    const Polygon polygon = triangle(70, 70, 330, 200);  // bbox 260 x 130 -> 5 x 3 tiles of 64
    const Rect64 box{70, 70, 260, 130};

    PipelineSpec objectSpec = spec1("test.maskComponents");
    objectSpec.restrictToPolygon(polygon);
    Pipeline objects(scene, config(), objectSpec);
    objects.bindLayer("objects", LayerId(3));
    RecordingObserver observer;
    ts::CollectingObjectSink sink;
    objects.runInto(sink, &observer, CancellationToken());
    const int64_t whole = (kSide / kTile) * (kSide / kTile);
    ASSERT_FALSE(observer.calls.empty());
    EXPECT_EQ(observer.calls.back().second, whole);
    EXPECT_EQ(observer.calls.back().first, whole);
    EXPECT_EQ(objects.rect(0), box);

    PipelineSpec rasterSpec = spec1("test.threshold");
    rasterSpec.restrictToPolygon(polygon);
    Pipeline raster(scene, config(), rasterSpec);
    const auto calls = progressOf(raster);
    ASSERT_FALSE(calls.empty());
    EXPECT_EQ(calls.back().second, 15);
    EXPECT_EQ(calls.back().first, 15);
    EXPECT_EQ(raster.rect(0), box);
}

namespace {

// Records the level-0 rect of every raster tile written.
class RectRecordingRasterSink : public RasterSink {
public:
    void writeTile(const BlockView& tile) override {
        std::lock_guard<std::mutex> lock(mutex_);
        rects_.push_back(tile.slideRect());
    }
    void flush() override {}
    int64_t tilesWritten() const override { return static_cast<int64_t>(rects().size()); }
    std::vector<Rect64> rects() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return rects_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<Rect64> rects_;
};

// The pixels of the scene that `rects` cover, one flag per pixel.
std::vector<bool> coverage(const std::vector<Rect64>& rects) {
    std::vector<bool> covered(static_cast<size_t>(kSide * kSide), false);
    for (const Rect64& r : rects) {
        for (int64_t y = r.y; y < r.y + r.height; ++y) {
            for (int64_t x = r.x; x < r.x + r.width; ++x) {
                covered[static_cast<size_t>(y * kSide + x)] = true;
            }
        }
    }
    return covered;
}

} // namespace

// An ROI run producing objects tiles the level, yet writes raster pixels only inside its
// region, and exactly the pixels a run producing none writes over its region grid.
TEST(Restriction, RoiObjectRunWritesRasterOnlyInsideTheRegion) {
    auto scene = ts::makeSyntheticScene(source(blobs));
    const Rect64 roi{70, 70, 260, 130};

    PipelineSpec objectSpec = spec1("test.components");
    objectSpec.restrictTo(roi);
    RectRecordingRasterSink objectRaster;
    Pipeline objects(scene, config(), objectSpec);
    objects.bindLayer("objects", LayerId(3)).rasterSink(&objectRaster);
    ts::CollectingObjectSink sink;
    objects.runInto(sink, nullptr, CancellationToken());
    const Rect64 region = objects.rect(0);
    EXPECT_EQ(region, roi);
    EXPECT_EQ(detail::PipelineAccess::plan(objects).grid.tileCount(), (kSide / kTile) * (kSide / kTile));

    PipelineSpec rasterSpec = spec1("test.threshold");
    rasterSpec.restrictTo(roi);
    RectRecordingRasterSink regionRaster;
    Pipeline raster(scene, config(), rasterSpec);
    raster.rasterSink(&regionRaster);
    raster.run(nullptr, CancellationToken());

    const std::vector<Rect64> written = objectRaster.rects();
    ASSERT_FALSE(written.empty());
    for (const Rect64& r : written) {
        EXPECT_FALSE(r.isEmpty());
        EXPECT_EQ(r.intersected(region), r) << "a raster tile reaches beyond the region";
    }
    EXPECT_TRUE(coverage(written) == coverage(regionRaster.rects()));
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

// admits(anchor) against the answer of the tile mask of the tile holding the anchor, and inRoi():
// the ROI is tested exactly in level 0, while the mask also sets the pixels straddling its edge.
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

    std::mt19937_64 rng(20261116);
    std::uniform_int_distribution<int64_t> dist(0, kSide - 1);
    int admitted = 0;
    int refused = 0;
    int checked = 0;
    for (int attempts = 0; checked < 1000 && attempts < 200000; ++attempts) {
        const Point64 anchor{dist(rng), dist(rng)};
        const Point64 pixel = levelPixelOracle(*impl.selector, anchor, level);
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
        const bool expected = mask.testLevel0(anchor) && filter.inRoi(anchor);
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

// Every mask admits() caches is charged to the budget while it is held and given
// back when it is dropped; a mask that does not fit is not cached, and admits()
// answers without it, the same.
TEST(Restriction, CachedTileMasksAreChargedToTheBudget) {
    ts::registerTestOperations();
    auto scene = ts::makeSyntheticScene({{kSide, kSide, 0, 0}}, {slideio::DataType::DT_Byte}, texture);
    ts::DiscMaskSource disc(250.0, 230.0, 150.0);
    Pipeline p(scene, config(), spec1("test.boxMean", radius(3)));
    p.maskSource(&disc);
    p.validate();
    auto& impl = detail::PipelineAccess::impl(p);
    const detail::RunPlan& plan = detail::PipelineAccess::plan(p);
    const int64_t first = plan.grid.tileIndex(1, 3);
    const int64_t second = plan.grid.tileIndex(2, 3);
    const int64_t firstBytes = plan.grid.tileRectAtLevel(first).area();
    const int64_t secondBytes = plan.grid.tileRectAtLevel(second).area();

    // Roomy: held masks are visible in reservedBytes(), dropped ones are not.
    {
        const MemoryBudgetPtr budget = makeMemoryBudget(int64_t{1} << 30);
        ASSERT_TRUE(budget->tryReserve(1000));  // Someone else's bytes, untouched throughout.
        {
            const detail::RestrictionFilter filter(plan, impl.spec, *impl.selector, impl.maskSource, budget.get());
            filter.admits(Point64{100, 230});  // tile (1, 3)
            EXPECT_EQ(budget->reservedBytes(), 1000 + firstBytes);
            filter.admits(Point64{101, 231});  // the same tile: no second charge
            filter.admits(Point64{140, 230});  // tile (2, 3)
            EXPECT_EQ(budget->reservedBytes(), 1000 + firstBytes + secondBytes);
            filter.releaseTileMask(first);
            EXPECT_EQ(budget->reservedBytes(), 1000 + secondBytes);
            filter.releaseTileMask(first);  // nothing cached: nothing given back
            EXPECT_EQ(budget->reservedBytes(), 1000 + secondBytes);
        }
        EXPECT_EQ(budget->reservedBytes(), 1000) << "destroying the filter gives back what its cache held";
    }

    // Tight: no mask fits, so none is cached, and every answer is still right.
    {
        const MemoryBudgetPtr budget = makeMemoryBudget(firstBytes + 100);
        ASSERT_TRUE(budget->tryReserve(200));  // Leaves less than one tile mask.
        const detail::RestrictionFilter filter(plan, impl.spec, *impl.selector, impl.maskSource, budget.get());
        int64_t admitted = 0;
        int64_t refused = 0;
        for (const int64_t tile : {first, second}) {
            const Rect64 r = plan.grid.tileRect(tile);
            for (int64_t y = r.y; y < r.y + r.height; y += 3) {
                for (int64_t x = r.x; x < r.x + r.width; x += 2) {
                    const bool expected = disc.contains(static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5);
                    ASSERT_EQ(filter.admits(Point64{x, y}), expected) << x << "," << y;
                    (expected ? admitted : refused)++;
                }
            }
        }
        EXPECT_GT(admitted, 10);
        EXPECT_GT(refused, 10);
        EXPECT_EQ(filter.cachedTileMaskCount(), 0);
        EXPECT_EQ(budget->reservedBytes(), 200);
    }
}

namespace {

// Full-width bars two level rows high, one every three rows: every tile holds a piece of
// most bars, each anchored on the tile's first level-0 column.
double bars(int, int64_t, int64_t y) { return y % 3 < 2 ? 255.0 : 0.0; }

// The objects of a "test.components" run at level 1 of a 600 -> 250 pyramid (scale 2.4),
// restricted to the whole slide or not.
std::vector<ObjectId> barObjects(int32_t tile, bool restricted, bool seams) {
    ts::registerTestOperations();
    auto scene = ts::makeSyntheticScene({{600, 600, 0, 0}, {250, 250, 0, 0}}, {slideio::DataType::DT_Byte}, bars);
    OperationParameters parameters;
    parameters.setBool("seams", seams);
    PipelineSpec spec = spec1("test.components", parameters);
    spec.analysisLevel(1);
    if (restricted) {
        spec.restrictTo(Rect64{0, 0, 600, 600});
    }
    const Configuration configuration =
        ConfigurationBuilder().tileSize(tile, tile).tileSizeFromSlide(false).workerCount(4).build();
    ts::CollectingObjectSink sink;
    Pipeline p(scene, configuration, spec);
    p.bindLayer("objects", LayerId(3));
    p.runInto(sink, nullptr, CancellationToken());
    return sortedIds(sink);
}

} // namespace

// At a non-integer level scale, a tile's first level-0 column or row is the level-0 image of
// its first level pixel. An object anchored there is admitted when the restriction covers it,
// so a restriction covering the slide keeps every object.
TEST(Restriction, NonIntegerScaleKeepsObjectsOnTileEdges) {
    for (const int32_t tile : {64, 100}) {
        const std::vector<ObjectId> unrestricted = barObjects(tile, false, false);
        ASSERT_FALSE(unrestricted.empty());
        EXPECT_EQ(barObjects(tile, true, false), unrestricted) << "tile " << tile;
    }
    // Merged across seams, the objects match a pass over the whole level in one tile.
    const std::vector<ObjectId> whole = barObjects(256, true, true);
    ASSERT_FALSE(whole.empty());
    for (const int32_t tile : {64, 100}) {
        EXPECT_EQ(barObjects(tile, true, true), whole) << "tile " << tile;
    }
}

namespace {

// Short vertical dashes one level column wide, on every fourth column from column 3: column 127
// is anchored at level-0 305, in the level pixel 127 that holds level-0 305 and 306.
double dashes(int, int64_t x, int64_t y) { return x % 4 == 3 && y % 3 < 2 ? 255.0 : 0.0; }

// The same, turned: short horizontal dashes one level row high, on every fourth row from row 3.
double hdashes(int, int64_t x, int64_t y) { return y % 4 == 3 && x % 3 < 2 ? 255.0 : 0.0; }

// Sets only the level column (or row) `index`, over the whole slide.
class LineMaskSource : public TileMaskSource {
public:
    LineMaskSource(bool row, int64_t index) : row_(row), index_(index) {}
    void fillMask(RasterMask& out) override {
        const Rect64 region = out.regionAtLevel();
        for (int64_t y = 0; y < region.height; ++y) {
            uint8_t* line = out.row(y);
            for (int64_t x = 0; x < region.width; ++x) {
                const int64_t at = row_ ? region.y + y : region.x + x;
                line[x] = at == index_ ? 255 : 0;
            }
        }
    }

private:
    bool row_;
    int64_t index_;
};
using SkipRing = std::vector<std::pair<int64_t, int64_t>>;
using SkipKey = std::tuple<ObjectId, std::vector<std::pair<bool, SkipRing>>,
                           std::vector<std::tuple<std::string, double, int32_t>>>;

// A record as its id, rings and measurements, as sortedKeys of test_seam_run.cpp does.
SkipKey keyOf(const ObjectRecord& record) {
    std::vector<std::pair<bool, SkipRing>> rings;
    const Polygon& polygon = record.geometry();
    for (int32_t r = 0; r < polygon.ringCount(); ++r) {
        std::vector<Point64> points(static_cast<size_t>(polygon.ringPointCount(r)));
        polygon.ringPoints(r, points.data(), static_cast<int64_t>(points.size()));
        SkipRing ring;
        for (const Point64& p : points) {
            ring.emplace_back(p.x, p.y);
        }
        rings.emplace_back(polygon.ringIsHole(r), std::move(ring));
    }
    std::vector<std::tuple<std::string, double, int32_t>> measurements;
    const MeasurementSet& m = record.measurements();
    for (int32_t i = 0; i < m.count(); ++i) {
        const Measurement measurement = m.at(i);
        measurements.emplace_back(measurement.name(), measurement.value(), static_cast<int32_t>(measurement.unit()));
    }
    return SkipKey(record.id(), std::move(rings), std::move(measurements));
}


// At level 1 (scale 2.4) with 64-pixel tiles, level column (row) 127 is the last of tile 1 and
// holds level-0 305 and 306; tile 2 starts at 307. With only that line set, tile 2's rect is
// empty: the skip test must agree with the mask each tile was given, so tile 2 is skipped and
// the objects anchored in the line, owned by tile 1, are emitted, with or without a halo.
void expectSkipAgreesWithTheMask(bool row, int32_t halo) {
    ts::registerTestOperations();
    auto scene = ts::makeSyntheticScene({{600, 600, 0, 0}, {250, 250, 0, 0}}, {slideio::DataType::DT_Byte},
                                        row ? hdashes : dashes);
    const LevelSelector selector(scene);
    const Configuration configuration =
        ConfigurationBuilder().tileSize(64, 64).tileSizeFromSlide(false).workerCount(4).build();
    const auto run = [&](bool restricted) {
        OperationParameters parameters;
        parameters.setInt("halo", halo);
        PipelineSpec spec = spec1("test.components", parameters);
        spec.analysisLevel(1);
        LineMaskSource line(row, 127);
        ts::CollectingObjectSink sink;
        Pipeline p(scene, configuration, spec);
        p.bindLayer("objects", LayerId(3));
        if (restricted) {
            p.maskSource(&line);
        }
        p.runInto(sink, nullptr, CancellationToken());
        return sink.records();
    };
    const std::vector<ObjectRecord> all = run(false);
    std::vector<SkipKey> expected;
    for (const ObjectRecord& r : all) {
        const Point64 anchor = r.geometry().anchor();
        const int64_t along = row ? anchor.y : anchor.x;
        const Point64 pixel = levelPixelOracle(selector, anchor, 1);
        if ((row ? pixel.y : pixel.x) == 127) {
            // Owned by tile 1: level pixel 127 holds level-0 305 and 306.
            EXPECT_GE(along, 305);
            EXPECT_LT(along, 307);
            expected.push_back(keyOf(r));
        }
    }
    std::sort(expected.begin(), expected.end());
    ASSERT_FALSE(expected.empty());
    ASSERT_LT(expected.size(), all.size());
    std::vector<SkipKey> got;
    for (const ObjectRecord& r : run(true)) {
        got.push_back(keyOf(r));
    }
    std::sort(got.begin(), got.end());
    EXPECT_TRUE(got == expected) << "the stored objects differ from the unrestricted run's (ids, rings or measurements)";
}

} // namespace

TEST(Restriction, SkipAgreesWithTheBuiltMaskAtANonIntegerScale) { expectSkipAgreesWithTheMask(false, 0); }

TEST(Restriction, SkipAgreesWithTheBuiltMaskAtANonIntegerScaleWithAHalo) { expectSkipAgreesWithTheMask(false, 2); }

TEST(Restriction, SkipAgreesWithTheBuiltMaskAtANonIntegerScaleForRows) { expectSkipAgreesWithTheMask(true, 0); }

TEST(Restriction, SkipAgreesWithTheBuiltMaskAtANonIntegerScaleForRowsWithAHalo) {
    expectSkipAgreesWithTheMask(true, 2);
}

namespace {

// Sets every pixel and records the region of each fill.
class RegionRecordingMaskSource : public TileMaskSource {
public:
    void fillMask(RasterMask& out) override {
        const Rect64 region = out.regionAtLevel();
        for (int64_t y = 0; y < region.height; ++y) {
            std::memset(out.row(y), 255, static_cast<size_t>(region.width));
        }
        std::lock_guard<std::mutex> lock(mutex_);
        regions_.push_back(region);
    }
    std::vector<Rect64> regions() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return regions_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<Rect64> regions_;
};

// An ROI in an object run, and a mask source over the whole level: a tile whose owned rect misses
// the ROI builds no mask in a pass, so only a tile evaluated on demand fills a mask outside it.
void expectOutsideTilesBuildNoMask() {
    auto scene = ts::makeSyntheticScene(source(blobs));
    const Rect64 roi{70, 70, 260, 130};
    RegionRecordingMaskSource fills;
    Pipeline p(scene, config(), [&] {
        PipelineSpec spec = spec1("test.components");
        spec.restrictTo(roi);
        return spec;
    }());
    p.bindLayer("objects", LayerId(3)).maskSource(&fills);
    ts::CollectingObjectSink sink;
    p.runInto(sink, nullptr, CancellationToken());

    const detail::RunPlan& plan = detail::PipelineAccess::plan(p);
    // Tiles 1..5 by 1..3 meet the ROI; every other tile of the 8 x 8 grid is skipped.
    ASSERT_EQ(plan.grid.tileCount(), (kSide / kTile) * (kSide / kTile));
    const auto& counters = detail::PipelineAccess::impl(p).counters;
    EXPECT_EQ(counters.tilesSkipped.load(), 64 - 5 * 3);
    int64_t outside = 0;
    for (const Rect64& r : fills.regions()) {
        outside += r.intersects(roi) ? 0 : 1;
    }
    EXPECT_LE(outside, counters.tilesEvaluatedOnDemand.load());
}

} // namespace

TEST(Restriction, OutsideRegionTilesBuildNoMask) { expectOutsideTilesBuildNoMask(); }

namespace {

// For every tile of an object run at level 1 (scale 2.4), outsideRegion implies the built mask
// has no set pixel in the owned rect, so the shortcut never skips a tile the mask would keep.
void expectOutsideRegionAgreesWithTheMask(int32_t halo, int variant) {
    ts::registerTestOperations();
    auto scene = ts::makeSyntheticScene({{600, 600, 0, 0}, {250, 250, 0, 0}}, {slideio::DataType::DT_Byte}, blobs);
    OperationParameters parameters;
    parameters.setInt("halo", halo);
    PipelineSpec spec = spec1("test.components", parameters);
    spec.analysisLevel(1);
    if (variant == 1) {
        spec.restrictToPolygon(triangle(307, 307, 1000, 700));
    } else if (variant == 2) {
        // The bounding box ends inside the level: the raster can set the pixel past the region.
        spec.restrictToPolygon(rectangle(100, 100, 305, 305));
    } else if (variant == 3) {
        spec.restrictToPolygon(rectangle(100, 100, 305, 305));
        spec.restrictTo(Rect64{0, 0, 450, 450});
    } else {
        spec.restrictTo(Rect64{307, 307, 700, 400});
    }
    Pipeline p(scene, config(), spec);
    p.bindLayer("objects", LayerId(3));
    p.validate();
    auto& impl = detail::PipelineAccess::impl(p);
    const detail::RunPlan& plan = detail::PipelineAccess::plan(p);
    const detail::RestrictionFilter filter(plan, impl.spec, *impl.selector, impl.maskSource);
    ASSERT_TRUE(filter.restricted());
    int64_t outside = 0;
    for (int64_t t = 0; t < plan.grid.tileCount(); ++t) {
        if (filter.outsideRegion(t)) {
            ++outside;
            EXPECT_TRUE(filter.skips(filter.buildTileMask(t), t)) << "tile " << t;
        }
    }
    EXPECT_GT(outside, 0);
    EXPECT_LT(outside, plan.grid.tileCount());
}

} // namespace

TEST(Restriction, OutsideRegionAgreesWithTheBuiltMaskAtANonIntegerScale) { expectOutsideRegionAgreesWithTheMask(0, 0); }

TEST(Restriction, OutsideRegionAgreesWithTheBuiltMaskAtANonIntegerScaleWithAHalo) {
    expectOutsideRegionAgreesWithTheMask(2, 0);
}

TEST(Restriction, OutsideRegionAgreesWithTheBuiltMaskAtANonIntegerScaleForAPolygon) {
    expectOutsideRegionAgreesWithTheMask(0, 1);
}

TEST(Restriction, OutsideRegionAgreesWithTheBuiltMaskForAPolygonEndingInsideTheLevel) {
    expectOutsideRegionAgreesWithTheMask(0, 2);
}

TEST(Restriction, OutsideRegionAgreesWithTheBuiltMaskForARoiPastThePolygonBox) {
    expectOutsideRegionAgreesWithTheMask(0, 3);
}

namespace {

// A "test.components" run (whose grid spans the level) at level 1 of a `side0` -> `side1`
// pyramid, restricted to `roi` and to `source` when not null, and its restriction filter.
struct RoiRun {
    RoiRun(int64_t side0, int64_t side1, const Rect64& roi, TileMaskSource* source = nullptr)
        : pipeline(ts::makeSyntheticScene({{side0, side0, 0, 0}, {side1, side1, 0, 0}}, {slideio::DataType::DT_Byte},
                                          blobs),
                   config(), [&] {
                       ts::registerTestOperations();
                       PipelineSpec spec = spec1("test.components");
                       spec.analysisLevel(1);
                       spec.restrictTo(roi);
                       return spec;
                   }()) {
        pipeline.bindLayer("objects", LayerId(3)).maskSource(source);
        pipeline.validate();
        auto& impl = detail::PipelineAccess::impl(pipeline);
        filter = std::make_unique<detail::RestrictionFilter>(detail::PipelineAccess::plan(pipeline), impl.spec,
                                                             *impl.selector, impl.maskSource);
    }
    const detail::RunPlan& plan() const { return detail::PipelineAccess::plan(pipeline); }

    Pipeline pipeline;
    std::unique_ptr<detail::RestrictionFilter> filter;
};

// admits() and inRoi() both give `expected` for `anchor`.
void expectAdmits(const detail::RestrictionFilter& filter, Point64 anchor, bool expected) {
    EXPECT_EQ(filter.admits(anchor), expected) << "admits, anchor " << anchor.x << ", " << anchor.y;
    EXPECT_EQ(filter.inRoi(anchor), expected) << "inRoi, anchor " << anchor.x << ", " << anchor.y;
}

} // namespace

// An ROI admits exactly the anchors inside it in level 0, though at level 1 its edges straddle
// level pixels: at scale 2.4, level-0 407, past the ROI, is in level pixel 169, inside the outward
// mapping. At scale 2, 306 shares level pixel 153 with the ROI's first column 307.
TEST(Restriction, RoiAdmitsExactlyItsLevel0Anchors) {
    {
        const RoiRun run(600, 250, Rect64{307, 307, 100, 100});
        ASSERT_DOUBLE_EQ(run.plan().levelScale, 2.4);
        expectAdmits(*run.filter, Point64{307, 307}, true);
        expectAdmits(*run.filter, Point64{406, 406}, true);
        expectAdmits(*run.filter, Point64{306, 307}, false);
        expectAdmits(*run.filter, Point64{307, 306}, false);
        expectAdmits(*run.filter, Point64{407, 406}, false);
        expectAdmits(*run.filter, Point64{406, 407}, false);
    }
    {
        // The ROI starts inside level pixel 128, which holds level-0 307 to 309.
        const RoiRun run(600, 250, Rect64{308, 308, 99, 99});
        expectAdmits(*run.filter, Point64{307, 308}, false);
        expectAdmits(*run.filter, Point64{308, 308}, true);
    }
    {
        // As above, with the anchor before the ROI in the ROI's first level pixel: 308 and 309 are
        // both in pixel 128, so only the level-0 test refuses 308.
        const RoiRun run(600, 250, Rect64{309, 309, 98, 98});
        expectAdmits(*run.filter, Point64{308, 309}, false);
        expectAdmits(*run.filter, Point64{309, 308}, false);
        expectAdmits(*run.filter, Point64{309, 309}, true);
    }
    {
        const RoiRun run(512, 256, Rect64{307, 307, 100, 100});
        ASSERT_DOUBLE_EQ(run.plan().levelScale, 2.0);
        expectAdmits(*run.filter, Point64{306, 307}, false);
        expectAdmits(*run.filter, Point64{307, 307}, true);
    }
}

// The tile masks, which skipping and operations see, set the level pixel of every anchor on an
// ROI's first and last columns and rows, in the tile owning that anchor, so no such tile is skipped.
TEST(Restriction, RoiMaskSetsEveryPixelHoldingAnRoiAnchor) {
    for (const Rect64& roi : {Rect64{307, 307, 100, 100}, Rect64{308, 308, 99, 99}}) {
        const RoiRun run(600, 250, roi);
        std::vector<Point64> edge;
        for (int64_t i = 0; i < roi.width; ++i) {
            edge.push_back(Point64{roi.x + i, roi.y});
            edge.push_back(Point64{roi.x + i, roi.y + roi.height - 1});
        }
        for (int64_t i = 0; i < roi.height; ++i) {
            edge.push_back(Point64{roi.x, roi.y + i});
            edge.push_back(Point64{roi.x + roi.width - 1, roi.y + i});
        }
        size_t owned = 0;
        const TileGrid& grid = run.plan().grid;
        for (int64_t t = 0; t < grid.tileCount(); ++t) {
            const Rect64 tile = grid.tileRect(t);
            const RasterMask mask = run.filter->buildTileMask(t);
            bool holdsAny = false;
            for (const Point64& a : edge) {
                if (a.x >= tile.x && a.y >= tile.y && a.x < tile.x + tile.width && a.y < tile.y + tile.height) {
                    EXPECT_TRUE(mask.testLevel0(a)) << "tile " << t << ", anchor " << a.x << ", " << a.y;
                    holdsAny = true;
                    ++owned;
                }
            }
            if (holdsAny) {
                EXPECT_FALSE(run.filter->skips(mask, t)) << "tile " << t;
            }
        }
        EXPECT_EQ(owned, edge.size()) << "every edge anchor is owned by one tile";
    }
}

// With an ROI and a mask source, both decide: the mask's ROI part sets level pixel 128, which holds
// level-0 307 and the ROI's first column 308, but 307 lies outside the ROI.
TEST(Restriction, RoiAndMaskSourceBothDecide) {
    ts::DiscMaskSource disc(300.0, 356.0, 40.0);
    RoiRun run(600, 250, Rect64{308, 308, 99, 99}, &disc);
    const double scale = run.plan().levelScale;
    const LevelSelector& selector = *detail::PipelineAccess::impl(run.pipeline).selector;
    const auto discHolds = [&](Point64 a) {
        // The disc tests the level-0 centre of the anchor's level pixel.
        const Point64 pixel = levelPixelOracle(selector, a, 1);
        return disc.contains((static_cast<double>(pixel.x) + 0.5) * scale,
                             (static_cast<double>(pixel.y) + 0.5) * scale);
    };
    const Point64 outsideDisc{400, 356};
    const Point64 leftOfRoi{307, 356};
    const Point64 inBoth{310, 356};
    ASSERT_FALSE(discHolds(outsideDisc));
    ASSERT_TRUE(discHolds(leftOfRoi));
    ASSERT_TRUE(discHolds(inBoth));
    // The tile mask is set at 307: only the level-0 ROI test refuses it.
    EXPECT_TRUE(run.filter->buildTileMask(run.filter->tileOf(leftOfRoi)).testLevel0(leftOfRoi));

    EXPECT_FALSE(run.filter->admits(outsideDisc));
    EXPECT_TRUE(run.filter->inRoi(outsideDisc));
    EXPECT_FALSE(run.filter->admits(leftOfRoi));
    EXPECT_TRUE(run.filter->admits(inBoth));
}

namespace {

// A "test.components" run at level 1 of a 600 -> 250 pyramid (scale 2.4) restricted to an ROI
// stores exactly the unrestricted run's objects anchored inside the ROI in level 0.
void expectRoiRunStoresExactlyItsAnchors(const ts::PixelFunction& pixels, const Rect64& roi) {
    ts::registerTestOperations();
    auto scene = ts::makeSyntheticScene({{600, 600, 0, 0}, {250, 250, 0, 0}}, {slideio::DataType::DT_Byte}, pixels);
    const LevelSelector selector(scene);
    const auto run = [&](bool restricted) {
        PipelineSpec spec = spec1("test.components");
        spec.analysisLevel(1);
        if (restricted) {
            spec.restrictTo(roi);
        }
        ts::CollectingObjectSink sink;
        Pipeline p(scene, config(), spec);
        p.bindLayer("objects", LayerId(3));
        p.runInto(sink, nullptr, CancellationToken());
        return sink.records();
    };
    std::vector<ObjectId> expected;
    const int64_t px0 = levelPixelOracle(selector, Point64{roi.x, roi.y}, 1).x;
    const int64_t py0 = levelPixelOracle(selector, Point64{roi.x, roi.y}, 1).y;
    const int64_t px1 = levelPixelOracle(selector, Point64{roi.x + roi.width - 1, roi.y}, 1).x;
    const int64_t py1 = levelPixelOracle(selector, Point64{roi.x, roi.y + roi.height - 1}, 1).y;
    int64_t straddling = 0;
    for (const ObjectRecord& r : run(false)) {
        const Point64 a = r.geometry().anchor();
        const bool insideRoi = a.x >= roi.x && a.y >= roi.y && a.x < roi.x + roi.width && a.y < roi.y + roi.height;
        if (insideRoi) {
            expected.push_back(r.id());
        }
        // Anchored outside the ROI, in a level pixel that holds one of its edge anchors: the mask
        // sets that pixel, so only the exact test refuses it.
        const Point64 pixel = levelPixelOracle(selector, a, 1);
        straddling += !insideRoi && pixel.x >= px0 && pixel.x <= px1 && pixel.y >= py0 && pixel.y <= py1 ? 1 : 0;
    }
    std::sort(expected.begin(), expected.end());
    ASSERT_GT(straddling, 0);
    std::vector<ObjectId> got;
    for (const ObjectRecord& r : run(true)) {
        got.push_back(r.id());
    }
    std::sort(got.begin(), got.end());
    EXPECT_EQ(got, expected);
}

} // namespace

// An ROI run at level 1 stores exactly its level-0 anchors, through the tile filter. The bars are
// anchored on each tile's first level-0 column, and tile 2's is 307: with the ROI at 308, they lie
// outside it, in its first level pixel 128, and are refused. The dashes on level column 127 are
// anchored at 305: with the ROI at 306, they lie outside it, in its first level pixel 127, and are
// refused.
TEST(Restriction, RoiRunStoresExactlyItsLevel0Anchors) {
    expectRoiRunStoresExactlyItsAnchors(bars, Rect64{308, 308, 99, 99});
    expectRoiRunStoresExactlyItsAnchors(dashes, Rect64{306, 307, 100, 100});
}

namespace {

// A restricted "test.components" pipeline at level 1 of a 600 -> 250 pyramid (scale 2.4) with
// its restriction filter, restricted to `polygon` when not null and to `source` when not null.
struct LevelOneRun {
    LevelOneRun(TileMaskSource* source, const Polygon* polygon)
        : pipeline(ts::makeSyntheticScene({{600, 600, 0, 0}, {250, 250, 0, 0}}, {slideio::DataType::DT_Byte}, blobs),
                   config(), [&] {
                       ts::registerTestOperations();
                       PipelineSpec spec = spec1("test.components");
                       spec.analysisLevel(1);
                       if (polygon != nullptr) {
                           spec.restrictToPolygon(*polygon);
                       }
                       return spec;
                   }()) {
        pipeline.bindLayer("objects", LayerId(3)).maskSource(source);
        pipeline.validate();
        auto& impl = detail::PipelineAccess::impl(pipeline);
        filter = std::make_unique<detail::RestrictionFilter>(detail::PipelineAccess::plan(pipeline), impl.spec,
                                                             *impl.selector, impl.maskSource);
    }

    Pipeline pipeline;
    std::unique_ptr<detail::RestrictionFilter> filter;
};

} // namespace

// A mask source's level pixel c holds the anchors at level-0 llround(c x 2.4) and not the one
// before it: the anchor of an object found at pixel c is tested at pixel c.
TEST(Restriction, MaskSourceTestsTheAnchorsOwnLevelPixel) {
    for (const bool row : {false, true}) {
        for (int64_t c = 120; c <= 135; ++c) {
            LineMaskSource line(row, c);
            const LevelOneRun run(&line, nullptr);
            const int64_t a = std::llround(static_cast<double>(c) * 2.4);
            const int64_t next = std::llround(static_cast<double>(c + 1) * 2.4);
            const auto at = [&](int64_t along) { return row ? Point64{300, along} : Point64{along, 300}; };
            EXPECT_TRUE(run.filter->admits(at(a))) << (row ? "row " : "column ") << c;
            EXPECT_FALSE(run.filter->admits(at(a - 1))) << (row ? "row " : "column ") << c;
            EXPECT_FALSE(run.filter->admits(at(next))) << (row ? "row " : "column ") << c;
        }
    }
}

// A run restricted by a mask source stores the objects found in its pixels: level column 123
// starts at level-0 295 (295.2 rounded), where 295 / 2.4 would floor to 122.
TEST(Restriction, MaskSourceRunKeepsTheObjectsFoundInItsPixels) {
    ts::registerTestOperations();
    auto scene = ts::makeSyntheticScene({{600, 600, 0, 0}, {250, 250, 0, 0}}, {slideio::DataType::DT_Byte}, dashes);
    const auto run = [&](bool restricted, int32_t workers) {
        PipelineSpec spec = spec1("test.components");
        spec.analysisLevel(1);
        LineMaskSource line(false, 123);
        const Configuration configuration =
            ConfigurationBuilder().tileSize(kTile, kTile).tileSizeFromSlide(false).workerCount(workers).build();
        ts::CollectingObjectSink sink;
        Pipeline p(scene, configuration, spec);
        p.bindLayer("objects", LayerId(3));
        if (restricted) {
            p.maskSource(&line);
        }
        p.runInto(sink, nullptr, CancellationToken());
        return sink.records();
    };
    std::vector<ObjectId> expected;
    for (const ObjectRecord& r : run(false, 4)) {
        if (r.geometry().anchor().x == 295) {
            expected.push_back(r.id());
        }
    }
    std::sort(expected.begin(), expected.end());
    ASSERT_FALSE(expected.empty());
    for (const int32_t workers : {1, 4}) {
        std::vector<ObjectId> got;
        for (const ObjectRecord& r : run(true, workers)) {
            got.push_back(r.id());
        }
        std::sort(got.begin(), got.end());
        EXPECT_EQ(got, expected) << workers << " workers";
    }
}

// A polygon admits every anchor that is the level-0 image of a level pixel whose centre it holds.
TEST(Restriction, PolygonAdmitsEveryAnchorWhosePixelCentreItContains) {
    const Point64 corners[] = {{70, 70}, {380, 90}, {400, 380}, {90, 410}};
    Polygon quad;
    quad.addRing(corners, 4, false);
    const LevelOneRun run(nullptr, &quad);
    int checked = 0;
    for (int64_t py = 0; py < 250; ++py) {
        for (int64_t px = 0; px < 250; ++px) {
            const Point64 a{std::llround(static_cast<double>(px) * 2.4), std::llround(static_cast<double>(py) * 2.4)};
            if (quad.contains(a)) {
                EXPECT_TRUE(run.filter->admits(a)) << "pixel " << px << ", " << py;
                ++checked;
            }
        }
    }
    EXPECT_GE(checked, 100);
}
