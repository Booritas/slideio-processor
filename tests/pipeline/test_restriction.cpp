// Requirements: UR-IMG-5, UR-STG-3
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

// Tiles whose restriction mask is empty read no pixel but still count as complete (P24, P27).
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
    int64_t straddling = 0;
    for (const ObjectRecord& r : all.records()) {
        const bool admitted = expectedMask.testLevel0(r.geometry().anchor());
        if (admitted) {
            expected.push_back(r.id());
        } else {
            // An excluded object that still touches the polygon's bounding box: it straddles the edge.
            const Rect64 box = r.geometry().boundingBox();
            if (!box.intersected(polygon.boundingBox()).isEmpty()) {
                ++straddling;
            }
        }
    }
    std::sort(expected.begin(), expected.end());
    ASSERT_GT(expected.size(), 50u);
    ASSERT_GT(straddling, 10) << "the fixture should have blobs across the polygon edge";
    EXPECT_LT(expected.size(), all.records().size());
    EXPECT_EQ(sortedIds(stored), expected);
    for (const ObjectRecord& r : stored.records()) {
        EXPECT_TRUE(expectedMask.testLevel0(r.geometry().anchor())) << "an object with its anchor outside was stored";
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

// A bound TileMaskSource never narrows the grid (P23).
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
    while (checked < 1000) {
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
