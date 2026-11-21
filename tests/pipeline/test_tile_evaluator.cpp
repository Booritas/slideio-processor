// Requirements: UR-EXT-3, UR-IMG-5, UR-ALG-5, UR-EXE-1
#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/pipeline/operation_catalog.hpp>
#include <slideio/processor/pipeline/pipeline.hpp>

#include <core/detail/level_mapping.hpp>
#include <pipeline/detail/pipeline_impl.hpp>
#include <pipeline/test_support/synthetic_scene.hpp>
#include <pipeline/test_support/test_operations.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

using namespace slideio::processor;
namespace ts = slideio::processor::testing_support;

namespace {

// A deterministic, uneven Byte texture, so a box mean differs from its neighbours.
double texture(int, int64_t x, int64_t y) {
    return static_cast<double>((x * 37 + y * 91 + (x * y) % 13) % 256);
}

constexpr int64_t kWidth = 1000;
constexpr int64_t kHeight = 700;

std::shared_ptr<slideio::Scene> textureScene() {
    ts::registerTestOperations();
    return ts::makeSyntheticScene({{kWidth, kHeight, 0, 0}}, {slideio::DataType::DT_Byte}, texture);
}

Configuration config(int32_t tileWidth, int32_t tileHeight) {
    return ConfigurationBuilder().tileSize(tileWidth, tileHeight).build();
}

OperationParameters intParam(const char* key, int64_t value) {
    OperationParameters p;
    p.setInt(key, value);
    return p;
}

// Assembles the Float32 plane 0 of every tile it receives into one level-0 image.
class CapturingRasterSink : public RasterSink {
public:
    CapturingRasterSink(int64_t width, int64_t height)
        : width_(width), height_(height), pixels_(static_cast<size_t>(width * height), -1.0f),
          written_(static_cast<size_t>(width * height), 0) {}

    void writeTile(const BlockView& tile) override {
        std::lock_guard<std::mutex> lock(mutex_);
        EXPECT_EQ(tile.planeType(0), DataType::Float32);
        const Rect64 r = tile.slideRect();
        for (int64_t y = 0; y < tile.height(); ++y) {
            const auto* row = static_cast<const unsigned char*>(tile.planeData(0)) + y * tile.planeRowStrideBytes(0);
            for (int64_t x = 0; x < tile.width(); ++x) {
                const size_t at = static_cast<size_t>((r.y + y) * width_ + r.x + x);
                std::memcpy(&pixels_[at], row + x * 4, 4);
                ++written_[at];
            }
        }
        ++tiles_;
    }
    void flush() override {}
    int64_t tilesWritten() const override { return tiles_; }

    float at(int64_t x, int64_t y) const { return pixels_[static_cast<size_t>(y * width_ + x)]; }
    int writes(int64_t x, int64_t y) const { return written_[static_cast<size_t>(y * width_ + x)]; }
    const std::vector<float>& pixels() const { return pixels_; }

private:
    int64_t width_;
    int64_t height_;
    std::mutex mutex_;
    std::vector<float> pixels_;
    std::vector<int> written_;
    int64_t tiles_ = 0;
};

// Runs `spec` on `scene` with tiles of the given size and returns the assembled output.
std::unique_ptr<CapturingRasterSink> runRaster(const std::shared_ptr<slideio::Scene>& scene, const PipelineSpec& spec,
                                               int32_t tileWidth, int32_t tileHeight, int64_t width = kWidth,
                                               int64_t height = kHeight) {
    auto sink = std::make_unique<CapturingRasterSink>(width, height);
    Pipeline p(scene, config(tileWidth, tileHeight), spec);
    p.rasterSink(sink.get());
    p.run(nullptr, CancellationToken());
    return sink;
}

// The single-tile reference: one tile covering the whole level.
std::unique_ptr<CapturingRasterSink> wholeLevel(const std::shared_ptr<slideio::Scene>& scene,
                                                const PipelineSpec& spec, int64_t width = kWidth,
                                                int64_t height = kHeight) {
    return runRaster(scene, spec, static_cast<int32_t>(width), static_cast<int32_t>(height), width, height);
}

PipelineSpec boxMean(int64_t radius) {
    PipelineSpec spec;
    spec.operation("test.boxMean", intParam("radius", radius));
    return spec;
}

// --- Object comparison ----------------------------------------------------------------

using Ring = std::vector<std::pair<int64_t, int64_t>>;
struct ObjectKey {
    std::vector<std::pair<bool, Ring>> rings;
    std::vector<std::tuple<std::string, double, int32_t>> measurements;
    bool operator<(const ObjectKey& o) const {
        return std::tie(rings, measurements) < std::tie(o.rings, o.measurements);
    }
    bool operator==(const ObjectKey& o) const {
        return std::tie(rings, measurements) == std::tie(o.rings, o.measurements);
    }
};

std::vector<ObjectKey> sortedKeys(const std::vector<ObjectRecord>& records) {
    std::vector<ObjectKey> keys;
    for (const ObjectRecord& record : records) {
        ObjectKey key;
        const Polygon& polygon = record.geometry();
        for (int32_t r = 0; r < polygon.ringCount(); ++r) {
            std::vector<Point64> points(static_cast<size_t>(polygon.ringPointCount(r)));
            polygon.ringPoints(r, points.data(), static_cast<int64_t>(points.size()));
            Ring ring;
            for (const Point64& p : points) {
                ring.emplace_back(p.x, p.y);
            }
            key.rings.emplace_back(polygon.ringIsHole(r), std::move(ring));
        }
        const MeasurementSet& m = record.measurements();
        for (int32_t i = 0; i < m.count(); ++i) {
            const Measurement measurement = m.at(i);
            key.measurements.emplace_back(measurement.name(), measurement.value(),
                                          static_cast<int32_t>(measurement.unit()));
        }
        keys.push_back(std::move(key));
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

// Separate rectangular blobs of 2..6 pixels a side, one per 13 x 11 cell, so they
// never touch; many straddle 64-pixel tile seams.
double blobs(int, int64_t x, int64_t y) {
    const int64_t cx = x / 13;
    const int64_t cy = y / 11;
    const int64_t lx = x % 13;
    const int64_t ly = y % 11;
    const int64_t w = 2 + (cx * 7 + cy * 3) % 5;
    const int64_t h = 2 + (cx * 5 + cy * 11) % 5;
    return lx >= 1 && lx < 1 + w && ly >= 1 && ly < 1 + h ? 255.0 : 0.0;
}

// --- A recording operation ----------------------------------------------------------

struct Recorded {
    Rect64 tileRectAtLevel, haloRectAtLevel, tileRect, haloRect;
    int64_t inputWidth, inputHeight, outputWidth, outputHeight;
    double levelScale;
};

std::mutex gRecordMutex;
std::map<int64_t, std::vector<Recorded>> gRecords; // by the "tag" parameter

// "test.B3.record": PerChannel, Byte and Float32, halo from int parameter "halo",
// output Float32 zeros; records its context under int parameter "tag".
class RecordingOperation : public Operation {
public:
    explicit RecordingOperation(const OperationParameters& parameters) : parameters_(parameters) {
        parameters_.getInt("halo", halo_);
        parameters_.getInt("tag", tag_);
    }
    const char* name() const override { return "test.B3.record"; }
    ChannelArity arity() const override { return ChannelArity::PerChannel; }
    bool requiresUniformTypes() const override { return false; }
    DataTypeSet supportedTypes() const override { return DataTypeSet().add(DataType::Byte).add(DataType::Float32); }
    Halo halo() const override { return Halo::uniform(static_cast<int32_t>(halo_)); }
    DataType outputType(DataType) const override { return DataType::Float32; }
    const OperationParameters& parameters() const override { return parameters_; }
    void apply(OperationContext& context) override {
        std::lock_guard<std::mutex> lock(gRecordMutex);
        gRecords[tag_].push_back(Recorded{context.tileRectAtLevel(), context.haloRectAtLevel(), context.tileRect(),
                                          context.haloRect(), context.input().width(), context.input().height(),
                                          context.output().width(), context.output().height(),
                                          context.levelScale()});
    }

private:
    OperationParameters parameters_;
    int64_t halo_ = 0;
    int64_t tag_ = 0;
};

void registerRecordingOperation() {
    OperationCatalog& catalog = OperationCatalog::instance();
    if (!catalog.isRegistered("test.B3.record")) {
        catalog.registerFactory(std::make_shared<ts::PrototypeFactory>(
            [](const OperationParameters& p) -> OperationPtr { return std::make_shared<RecordingOperation>(p); },
            "Records its context; writes zeros", "O(n) in the number of samples"));
    }
}

Rect64 grow(const Rect64& r, int64_t by) { return Rect64{r.x - by, r.y - by, r.width + 2 * by, r.height + 2 * by}; }

} // namespace

TEST(TileEvaluator, HaloMakesSeamsInvisible) {
    auto scene = textureScene();
    const auto reference = wholeLevel(scene, boxMean(3));
    const auto tiled = runRaster(scene, boxMean(3), 128, 128);
    EXPECT_EQ(tiled->tilesWritten(), 8 * 6);
    for (int64_t y = 0; y < kHeight; ++y) {
        for (int64_t x = 0; x < kWidth; ++x) {
            ASSERT_EQ(tiled->writes(x, y), 1) << "pixel written once";
        }
    }
    EXPECT_TRUE(tiled->pixels() == reference->pixels());
}

TEST(TileEvaluator, ZeroHaloDeclaredShowsTheSeam) {
    auto scene = textureScene();
    const auto reference = wholeLevel(scene, boxMean(3));
    OperationParameters p = intParam("radius", 3);
    p.setInt("declaredHalo", 0);
    PipelineSpec spec;
    spec.operation("test.boxMean", p);
    const auto tiled = runRaster(scene, spec, 128, 128);

    int64_t seamDiffs = 0;
    int64_t interiorDiffs = 0;
    for (int64_t y = 0; y < kHeight; ++y) {
        for (int64_t x = 0; x < kWidth; ++x) {
            const int64_t dx = std::min(x % 128, 127 - x % 128);
            const int64_t dy = std::min(y % 128, 127 - y % 128);
            const bool nearSeam = (dx < 3 && x >= 3 && x < kWidth - 3) || (dy < 3 && y >= 3 && y < kHeight - 3);
            const bool differs = tiled->at(x, y) != reference->at(x, y);
            if (nearSeam) {
                seamDiffs += differs ? 1 : 0;
            } else {
                interiorDiffs += differs ? 1 : 0;
            }
        }
    }
    EXPECT_GT(seamDiffs, 0);
    EXPECT_EQ(interiorDiffs, 0);
}

TEST(TileEvaluator, ChainHalosAdd) {
    auto scene = textureScene();
    PipelineSpec chain;
    chain.operation("test.boxMean", intParam("radius", 2)).operation("test.boxMean", intParam("radius", 3));
    const auto tiled = runRaster(scene, chain, 128, 128);

    // Two whole-level passes: the first pass's output becomes the second pass's scene.
    const auto first = wholeLevel(scene, boxMean(2));
    const std::vector<float> firstPixels = first->pixels();
    auto firstScene = ts::makeSyntheticScene({{kWidth, kHeight, 0, 0}}, {slideio::DataType::DT_Float32},
                                             [firstPixels](int, int64_t x, int64_t y) {
                                                 return static_cast<double>(
                                                     firstPixels[static_cast<size_t>(y * kWidth + x)]);
                                             });
    const auto second = wholeLevel(firstScene, boxMean(3));
    EXPECT_TRUE(tiled->pixels() == second->pixels());
}

TEST(TileEvaluator, RoiMatchesWholeLevelCropped) {
    auto scene = textureScene();
    const auto reference = wholeLevel(scene, boxMean(3));
    const Rect64 roi{150, 90, 517, 333};
    PipelineSpec spec = boxMean(3);
    spec.restrictTo(roi);
    const auto restricted = runRaster(scene, spec, 128, 128);
    for (int64_t y = 0; y < kHeight; ++y) {
        for (int64_t x = 0; x < kWidth; ++x) {
            const bool inside = x >= roi.x && x < roi.x + roi.width && y >= roi.y && y < roi.y + roi.height;
            if (inside) {
                ASSERT_EQ(restricted->writes(x, y), 1);
                ASSERT_EQ(restricted->at(x, y), reference->at(x, y)) << "differs inside the ROI";
            } else {
                ASSERT_EQ(restricted->writes(x, y), 0) << "written outside the ROI";
            }
        }
    }
}

TEST(TileEvaluator, ObjectsAreOwnedByTheirAnchorTile) {
    ts::registerTestOperations();
    constexpr int64_t width = 400;
    constexpr int64_t height = 300;
    auto scene = ts::makeSyntheticScene({{width, height, 0, 0}}, {slideio::DataType::DT_Byte}, blobs);
    OperationParameters parameters;
    parameters.setBool("haloScan", true);
    parameters.setInt("halo", 8);
    PipelineSpec spec;
    spec.operation("test.components", parameters);
    const LayerId layer(7);

    ts::CollectingObjectSink single;
    {
        Pipeline p(scene, config(static_cast<int32_t>(width), static_cast<int32_t>(height)), spec);
        p.bindLayer("objects", layer);
        p.runInto(single, nullptr, CancellationToken());
    }
    ts::CollectingObjectSink tiled;
    Pipeline p(scene, config(64, 64), spec);
    p.bindLayer("objects", layer);
    p.runInto(tiled, nullptr, CancellationToken());

    const std::vector<ObjectKey> expected = sortedKeys(single.records());
    const std::vector<ObjectKey> actual = sortedKeys(tiled.records());
    ASSERT_GT(expected.size(), 500u);
    EXPECT_EQ(std::adjacent_find(actual.begin(), actual.end()), actual.end()) << "an object was stored twice";
    EXPECT_TRUE(actual == expected);
    EXPECT_GT(detail::PipelineAccess::impl(p).counters.droppedOutsideTile.load(), 0);
}

TEST(TileEvaluator, ContextGeometryAtEachChainPosition) {
    ts::registerTestOperations();
    registerRecordingOperation();
    // Level 1 has a non-integer scale, so the level-0 rects exercise the edge mapping.
    auto scene = ts::makeSyntheticScene({{1000, 700, 0, 0}, {333, 233, 0, 0}}, {slideio::DataType::DT_Byte}, texture);
    OperationParameters first = intParam("halo", 2);
    first.setInt("tag", 1);
    OperationParameters second = intParam("halo", 3);
    second.setInt("tag", 2);
    PipelineSpec spec;
    spec.analysisLevel(1).operation("test.B3.record", first).operation("test.B3.record", second);
    {
        std::lock_guard<std::mutex> lock(gRecordMutex);
        gRecords.clear();
    }
    Pipeline p(scene, config(100, 100), spec);
    p.run(nullptr, CancellationToken());
    const detail::RunPlan& plan = detail::PipelineAccess::plan(p);
    const TileGrid& grid = plan.grid;
    const double scale = plan.levelScale;

    std::lock_guard<std::mutex> lock(gRecordMutex);
    ASSERT_EQ(static_cast<int64_t>(gRecords[1].size()), grid.tileCount());
    ASSERT_EQ(static_cast<int64_t>(gRecords[2].size()), grid.tileCount());
    // Tiles complete in any order on several workers; row-major order is (y, x) order.
    for (auto* records : {&gRecords[1], &gRecords[2]}) {
        std::sort(records->begin(), records->end(), [](const Recorded& a, const Recorded& b) {
            return std::tie(a.tileRectAtLevel.y, a.tileRectAtLevel.x) < std::tie(b.tileRectAtLevel.y, b.tileRectAtLevel.x);
        });
    }
    for (int64_t i = 0; i < grid.tileCount(); ++i) {
        const Rect64 tile = grid.tileRectAtLevel(i);
        const Recorded& last = gRecords[2][static_cast<size_t>(i)];
        EXPECT_EQ(last.tileRectAtLevel, tile);
        EXPECT_EQ(last.haloRectAtLevel, grow(tile, 3));
        EXPECT_EQ(last.tileRect, detail::rectToLevel0(tile, scale));
        EXPECT_EQ(last.haloRect, detail::rectToLevel0(grow(tile, 3), scale));
        EXPECT_EQ(last.inputWidth, tile.width + 6);
        EXPECT_EQ(last.inputHeight, tile.height + 6);
        EXPECT_EQ(last.outputWidth, tile.width);
        EXPECT_EQ(last.outputHeight, tile.height);
        EXPECT_EQ(last.levelScale, scale);

        const Recorded& firstOp = gRecords[1][static_cast<size_t>(i)];
        EXPECT_EQ(firstOp.tileRectAtLevel, grow(tile, 3));
        EXPECT_EQ(firstOp.haloRectAtLevel, grow(tile, 5));
        EXPECT_EQ(firstOp.tileRect, detail::rectToLevel0(grow(tile, 3), scale));
        EXPECT_EQ(firstOp.haloRect, detail::rectToLevel0(grow(tile, 5), scale));
        EXPECT_EQ(firstOp.inputWidth, tile.width + 10);
        EXPECT_EQ(firstOp.inputHeight, tile.height + 10);
        EXPECT_EQ(firstOp.outputWidth, tile.width + 6);
        EXPECT_EQ(firstOp.outputHeight, tile.height + 6);
    }
}

TEST(TileEvaluator, ThreeOperationChainHoldsNoIntermediateOnDisk) {
    ts::registerTestOperations();
    // The configuration's scratch directory is where a directly-constructed
    // Pipeline would spill; a fresh, empty one must stay empty.
    namespace fs = std::filesystem;
    const fs::path scratch = fs::temp_directory_path() / "slideio_processor_b3_scratch";
    fs::remove_all(scratch);
    fs::create_directories(scratch);
    const Configuration cfg =
        ConfigurationBuilder().tileSize(128, 128).scratchDirectory(scratch.u8string().c_str()).build();

    OperationParameters threshold;
    threshold.setDouble("level", 100.0);
    PipelineSpec spec;
    spec.operation("test.boxMean", intParam("radius", 1))
        .operation("test.threshold", threshold)
        .operation("test.boxMean", intParam("radius", 2));
    auto scene = textureScene();
    CapturingRasterSink sink(kWidth, kHeight);
    Pipeline p(scene, cfg, spec);
    p.rasterSink(&sink);
    p.run(nullptr, CancellationToken());

    EXPECT_EQ(sink.tilesWritten(), 8 * 6);
    EXPECT_TRUE(fs::is_empty(scratch)) << "an intermediate reached the scratch directory";
    fs::remove_all(scratch);
}
