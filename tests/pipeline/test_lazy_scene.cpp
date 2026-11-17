// Requirements: UR-EXE-1, UR-PIX-6, UR-ALG-5, UR-IMG-4
#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/pipeline/operation_catalog.hpp>
#include <slideio/processor/pipeline/pipeline.hpp>
#include <slideio/processor/pipeline/tile_grid.hpp>

#include <pipeline/detail/pipeline_impl.hpp>
#include <pipeline/test_support/disc_mask_source.hpp>
#include <pipeline/test_support/global_ops.hpp>
#include <pipeline/test_support/synthetic_scene.hpp>
#include <pipeline/test_support/test_operations.hpp>

#include <slideio/core/levelinfo.hpp>
#include <slideio/slideio/scene.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

using namespace slideio::processor;
namespace ts = slideio::processor::testing_support;

namespace {

// A deterministic, uneven texture, so a box mean differs from its neighbours.
double texture(int channel, int64_t x, int64_t y) {
    return static_cast<double>((x * 37 + y * 91 + (x * y) % 13 + channel * 50) % 256);
}

Configuration config(int32_t tile) {
    return ConfigurationBuilder().tileSize(tile, tile).tileSizeFromSlide(false).workerCount(4).build();
}

PipelineSpec boxMean(int64_t radius) {
    ts::registerTestOperations();
    OperationParameters p;
    p.setInt("radius", radius);
    PipelineSpec spec;
    spec.operation("test.boxMean", p);
    return spec;
}

OperationParameters thresholdAt(double level) {
    OperationParameters p;
    p.setDouble("level", level);
    return p;
}

std::shared_ptr<ts::SyntheticSource> textureSource(std::vector<ts::SyntheticLevel> levels,
                                                   std::vector<slideio::DataType> types = {
                                                       slideio::DataType::DT_Byte}) {
    ts::registerTestOperations();
    return std::make_shared<ts::SyntheticSource>(std::move(levels), std::move(types), texture);
}

// Assembles every plane of every tile it receives, as raw samples, into images
// of the analysis level (level 0 in these tests, so the level-0 slide rect is the
// level rect).
class CaptureSink : public RasterSink {
public:
    CaptureSink(int64_t width, int64_t height, std::vector<int64_t> sampleBytes)
        : width_(width), height_(height), sampleBytes_(std::move(sampleBytes)) {
        for (int64_t bytes : sampleBytes_) {
            planes_.emplace_back(static_cast<size_t>(width * height * bytes), uint8_t{0});
        }
    }
    void writeTile(const BlockView& tile) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const Rect64 r = tile.slideRect();
        for (size_t p = 0; p < planes_.size(); ++p) {
            const int64_t bytes = sampleBytes_[p];
            for (int64_t y = 0; y < tile.height(); ++y) {
                const auto* row = static_cast<const uint8_t*>(tile.planeData(static_cast<int32_t>(p))) +
                                  y * tile.planeRowStrideBytes(static_cast<int32_t>(p));
                std::memcpy(&planes_[p][static_cast<size_t>(((r.y + y) * width_ + r.x) * bytes)], row,
                            static_cast<size_t>(tile.width() * bytes));
            }
        }
        ++tiles_;
    }
    void flush() override {}
    int64_t tilesWritten() const override { return tiles_; }
    const std::vector<uint8_t>& plane(size_t p) const { return planes_[p]; }

private:
    int64_t width_;
    int64_t height_;
    std::vector<int64_t> sampleBytes_;
    std::mutex mutex_;
    std::vector<std::vector<uint8_t>> planes_;
    int64_t tiles_ = 0;
};

std::vector<uint8_t> eager(std::shared_ptr<slideio::Scene> scene, const PipelineSpec& spec, int32_t tile,
                           int64_t width, int64_t height, int64_t sampleBytes) {
    CaptureSink sink(width, height, {sampleBytes});
    Pipeline p(std::move(scene), config(tile), spec);
    p.rasterSink(&sink);
    p.run(nullptr, CancellationToken());
    return sink.plane(0);
}

std::vector<uint8_t> readScene(const std::shared_ptr<slideio::Scene>& scene, int x, int y, int w, int h,
                               int bw, int bh, size_t sampleBytes) {
    std::vector<uint8_t> buffer(static_cast<size_t>(bw) * bh * sampleBytes);
    scene->readResampledBlock(std::make_tuple(x, y, w, h), std::make_tuple(bw, bh), buffer.data(), buffer.size());
    return buffer;
}

// One plane of `type` over `width` x `height`, owned.
struct OwnedBlock {
    OwnedBlock(int64_t width, int64_t height, std::vector<DataType> types) {
        for (size_t p = 0; p < types.size(); ++p) {
            storage.emplace_back(static_cast<size_t>(width * height * dataTypeSizeBytes(types[p])), uint8_t{0xAB});
            planes.push_back(BlockPlane{storage.back().data(), types[p], width * dataTypeSizeBytes(types[p]),
                                        static_cast<int32_t>(p)});
        }
        view = BlockView(width, height, Rect64{0, 0, width, height}, 0, planes.data(),
                         static_cast<int32_t>(planes.size()));
    }
    std::vector<std::vector<uint8_t>> storage;
    std::vector<BlockPlane> planes;
    BlockView view;
};

// "test.lazy.copy": PerChannel, Byte and Float32, halo 0, output of the input's
// type: copies the input tile.
class CopyOperation : public Operation {
public:
    explicit CopyOperation(const OperationParameters& parameters) : parameters_(parameters) {}
    const char* name() const override { return "test.lazy.copy"; }
    ChannelArity arity() const override { return ChannelArity::PerChannel; }
    bool requiresUniformTypes() const override { return false; }
    DataTypeSet supportedTypes() const override { return DataTypeSet().add(DataType::Byte).add(DataType::Float32); }
    Halo halo() const override { return Halo::uniform(0); }
    DataType outputType(DataType inputType) const override { return inputType; }
    const OperationParameters& parameters() const override { return parameters_; }
    void apply(OperationContext& context) override {
        const BlockView& in = context.input();
        BlockView& out = context.output();
        const int64_t bytes = out.width() * dataTypeSizeBytes(out.planeType(0));
        for (int64_t y = 0; y < out.height(); ++y) {
            std::memcpy(static_cast<uint8_t*>(out.planeData(0)) + y * out.planeRowStrideBytes(0),
                        static_cast<const uint8_t*>(in.planeData(0)) + y * in.planeRowStrideBytes(0),
                        static_cast<size_t>(bytes));
        }
    }

private:
    OperationParameters parameters_;
};

void registerCopyOperation() {
    OperationCatalog& catalog = OperationCatalog::instance();
    if (!catalog.isRegistered("test.lazy.copy")) {
        catalog.registerFactory(std::make_shared<ts::PrototypeFactory>(
            [](const OperationParameters& p) -> OperationPtr { return std::make_shared<CopyOperation>(p); },
            "Copies its input", "O(n) in the number of samples"));
    }
}

template <typename E, typename F>
std::string messageOf(F&& call) {
    try {
        call();
    } catch (const E& e) {
        return e.what();
    }
    return "<nothing thrown>";
}

} // namespace

TEST(LazyScene, ConstructionReadsNothing) {
    auto source = textureSource({{512, 384, 0, 0}, {256, 192, 0, 0}});
    Pipeline p(ts::makeSyntheticScene(source), config(64), boxMean(2));
    const std::shared_ptr<slideio::Scene> scene = p.asScene();
    ASSERT_TRUE(scene);
    EXPECT_EQ(source->readCount(), 0);
    EXPECT_EQ(scene->getNumChannels(), 1);
    EXPECT_EQ(scene->getChannelDataType(0), slideio::DataType::DT_Float32);
    EXPECT_EQ(scene->getChannelName(0), "c0");
    EXPECT_EQ(scene->getNumZoomLevels(), 2);
    EXPECT_EQ(std::get<2>(scene->getRect()), 512);
    EXPECT_EQ(std::get<3>(scene->getRect()), 384);
    EXPECT_EQ(scene->getLevelInfo(1)->getSize().width, 256);
    EXPECT_DOUBLE_EQ(scene->getLevelInfo(1)->getScale(), 0.5);
    EXPECT_EQ(scene->getLevelInfo(0)->getTileSize().width, 64);
    EXPECT_EQ(source->readCount(), 0);
}

TEST(LazyScene, PixelsEqualTheEagerRun) {
    constexpr int64_t width = 300;
    constexpr int64_t height = 200;
    auto source = textureSource({{width, height, 0, 0}});
    const auto expected = eager(ts::makeSyntheticScene(source), boxMean(2), 64, width, height, 4);

    Pipeline p(ts::makeSyntheticScene(source), config(64), boxMean(2));
    const auto scene = p.asScene();
    const auto lazy = readScene(scene, 0, 0, width, height, width, height, 4);
    EXPECT_TRUE(lazy == expected);

    // Pipeline::readBlock directly, over a block straddling the level's edge:
    // what lies beyond the level reads as zero.
    OwnedBlock block(100, 80, {DataType::Float32});
    p.readBlock(0, Rect64{width - 60, height - 50, 100, 80}, block.view);
    int64_t mismatches = 0;
    for (int64_t y = 0; y < 80; ++y) {
        for (int64_t x = 0; x < 100; ++x) {
            float got = 0;
            std::memcpy(&got, block.storage[0].data() + (y * 100 + x) * 4, 4);
            const int64_t sx = width - 60 + x;
            const int64_t sy = height - 50 + y;
            float want = 0;
            if (sx < width && sy < height) {
                std::memcpy(&want, expected.data() + (sy * width + sx) * 4, 4);
            }
            mismatches += got == want ? 0 : 1;
        }
    }
    EXPECT_EQ(mismatches, 0);
}

TEST(LazyScene, OutsideTheRegionIsZero) {
    constexpr int64_t width = 300;
    constexpr int64_t height = 200;
    PipelineSpec spec;
    spec.restrictTo(Rect64{40, 30, 150, 100});
    spec.operation("test.threshold", thresholdAt(0.0));  // 255 everywhere it is computed
    auto source = textureSource({{width, height, 0, 0}});
    Pipeline p(ts::makeSyntheticScene(source), config(64), spec);
    EXPECT_EQ(p.rect(0), (Rect64{40, 30, 150, 100}));
    OwnedBlock block(width, height, {DataType::Byte});
    p.readBlock(0, Rect64{0, 0, width, height}, block.view);
    int64_t mismatches = 0;
    for (int64_t y = 0; y < height; ++y) {
        for (int64_t x = 0; x < width; ++x) {
            const bool inside = x >= 40 && x < 190 && y >= 30 && y < 130;
            mismatches += block.storage[0][static_cast<size_t>(y * width + x)] == (inside ? 255 : 0) ? 0 : 1;
        }
    }
    EXPECT_EQ(mismatches, 0);
}

TEST(LazyScene, IsAValidPipelineInput) {
    constexpr int64_t width = 300;
    constexpr int64_t height = 200;
    auto source = textureSource({{width, height, 0, 0}});

    PipelineSpec chain = boxMean(2);
    chain.operation("test.threshold", thresholdAt(128.0));
    const auto expected = eager(ts::makeSyntheticScene(source), chain, 64, width, height, 1);

    Pipeline first(ts::makeSyntheticScene(source), config(64), boxMean(2));
    PipelineSpec second;
    second.operation("test.threshold", thresholdAt(128.0));
    const auto chained = eager(first.asScene(), second, 100, width, height, 1);
    EXPECT_TRUE(chained == expected);
    EXPECT_GT(std::count(expected.begin(), expected.end(), uint8_t{255}), 0);
    EXPECT_GT(std::count(expected.begin(), expected.end(), uint8_t{0}), 0);
}

TEST(LazyScene, CoarseReadUsesACoarseLevel) {
    auto source = textureSource({{1024, 768, 0, 0}, {512, 384, 0, 0}, {256, 192, 0, 0}, {128, 96, 0, 0}});
    PipelineSpec spec;
    spec.operation("test.threshold", thresholdAt(100.0));
    Pipeline p(ts::makeSyntheticScene(source), config(64), spec);
    const auto scene = p.asScene();

    // A sixteenth: level 3 (an eighth) is the coarsest that still satisfies it.
    const auto block = readScene(scene, 0, 0, 1024, 768, 64, 48, 1);
    const std::vector<int> levels = source->readLevels();
    ASSERT_FALSE(levels.empty());
    EXPECT_TRUE(std::all_of(levels.begin(), levels.end(), [](int level) { return level == 3; }));

    // Nearest-neighbour from level 3, sample (2x, 2y): no value is invented.
    int64_t mismatches = 0;
    for (int64_t y = 0; y < 48; ++y) {
        for (int64_t x = 0; x < 64; ++x) {
            const int want = texture(0, 2 * x, 2 * y) >= 100.0 ? 255 : 0;
            mismatches += block[static_cast<size_t>(y * 64 + x)] == want ? 0 : 1;
        }
    }
    EXPECT_EQ(mismatches, 0);
}

TEST(LazyScene, ConcurrentReads) {
    constexpr int width = 600;
    constexpr int height = 400;
    auto source = textureSource({{width, height, 0, 0}, {300, 200, 0, 0}});
    Pipeline p(ts::makeSyntheticScene(source), config(64), boxMean(2));
    const auto scene = p.asScene();

    struct Request {
        int x, y, w, h, bw, bh;
    };
    std::mt19937 random(20261116);
    std::vector<Request> requests;
    for (int i = 0; i < 64; ++i) {
        const int w = std::uniform_int_distribution<int>(1, 160)(random);
        const int h = std::uniform_int_distribution<int>(1, 160)(random);
        const int x = std::uniform_int_distribution<int>(-20, width)(random);
        const int y = std::uniform_int_distribution<int>(-20, height)(random);
        const bool half = i % 4 == 0 && w > 1 && h > 1;
        requests.push_back(Request{x, y, w, h, half ? w / 2 : w, half ? h / 2 : h});
    }
    std::vector<std::vector<uint8_t>> reference;
    for (const Request& r : requests) {
        reference.push_back(readScene(scene, r.x, r.y, r.w, r.h, r.bw, r.bh, 4));
    }

    std::atomic<int64_t> mismatches{0};
    std::atomic<int64_t> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&, t]() {
            for (size_t i = 0; i < requests.size(); ++i) {
                const size_t k = (i * 7 + static_cast<size_t>(t) * 13) % requests.size();
                const Request& r = requests[k];
                try {
                    if (readScene(scene, r.x, r.y, r.w, r.h, r.bw, r.bh, 4) != reference[k]) {
                        ++mismatches;
                    }
                } catch (...) {
                    ++failures;
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(failures.load(), 0);
    EXPECT_EQ(mismatches.load(), 0);
}

TEST(LazyScene, LevelsMirrorTheSourceFromTheAnalysisLevel) {
    const std::vector<ts::SyntheticLevel> levels{{800, 600, 0, 0}, {400, 300, 0, 0}, {200, 150, 0, 0}, {100, 75, 0, 0}};
    auto source = textureSource(levels);
    const auto sourceScene = ts::makeSyntheticScene(source);
    PipelineSpec spec;
    spec.analysisLevel(1);
    spec.operation("test.threshold", thresholdAt(0.0));
    Pipeline p(sourceScene, config(64), spec);
    ASSERT_EQ(p.levelCount(), 3);
    for (int32_t k = 0; k < 3; ++k) {
        const ts::SyntheticLevel& dims = levels[static_cast<size_t>(1 + k)];
        EXPECT_EQ(p.rect(k), (Rect64{0, 0, dims.width, dims.height})) << "level " << k;
    }
    EXPECT_THROW(p.rect(3), InvalidArgumentError);
    EXPECT_THROW(p.rect(-1), InvalidArgumentError);

    // With a ROI, rect(k) is the region mapped outward to source level 1 + k.
    const Rect64 roi{101, 61, 301, 203};
    PipelineSpec restricted = spec;
    restricted.restrictTo(roi);
    Pipeline q(sourceScene, config(64), restricted);
    const LevelSelector selector(sourceScene);
    for (int32_t k = 0; k < 3; ++k) {
        const ts::SyntheticLevel& dims = levels[static_cast<size_t>(1 + k)];
        EXPECT_EQ(q.rect(k), selector.mapToLevel(roi, 1 + k).intersected(Rect64{0, 0, dims.width, dims.height}))
            << "level " << k;
    }

    // Output level 1 is source level 2, and only it is read.
    const int64_t before = source->readCount();
    OwnedBlock block(200, 150, {DataType::Byte});
    q.readBlock(1, Rect64{0, 0, 200, 150}, block.view);
    const std::vector<int> read = source->readLevels();
    ASSERT_GT(static_cast<int64_t>(read.size()), before);
    EXPECT_TRUE(std::all_of(read.begin() + before, read.end(), [](int level) { return level == 2; }));
    const Rect64 inside = q.rect(1);
    int64_t mismatches = 0;
    for (int64_t y = 0; y < 150; ++y) {
        for (int64_t x = 0; x < 200; ++x) {
            const bool in = x >= inside.x && x < inside.x + inside.width && y >= inside.y &&
                            y < inside.y + inside.height;
            mismatches += block.storage[0][static_cast<size_t>(y * 200 + x)] == (in ? 255 : 0) ? 0 : 1;
        }
    }
    EXPECT_EQ(mismatches, 0);

    // As a scene: three levels, the first the analysis level's size.
    const auto scene = p.asScene();
    ASSERT_EQ(scene->getNumZoomLevels(), 3);
    EXPECT_EQ(std::get<2>(scene->getRect()), 400);
    EXPECT_EQ(std::get<3>(scene->getRect()), 300);
    EXPECT_EQ(scene->getLevelInfo(2)->getSize().width, 100);
    EXPECT_DOUBLE_EQ(scene->getLevelInfo(2)->getScale(), 0.25);
}

TEST(LazyScene, ReadBlockValidatesTheView) {
    auto source = textureSource({{300, 200, 0, 0}});
    Pipeline p(ts::makeSyntheticScene(source), config(64), boxMean(1));
    const Rect64 rect{0, 0, 50, 40};
    OwnedBlock twoPlanes(50, 40, {DataType::Float32, DataType::Float32});
    EXPECT_THROW(p.readBlock(0, rect, twoPlanes.view), InvalidArgumentError);
    OwnedBlock wrongType(50, 40, {DataType::Byte});
    EXPECT_THROW(p.readBlock(0, rect, wrongType.view), InvalidArgumentError);
    OwnedBlock wrongSize(40, 40, {DataType::Float32});
    EXPECT_THROW(p.readBlock(0, rect, wrongSize.view), InvalidArgumentError);
    OwnedBlock good(50, 40, {DataType::Float32});
    EXPECT_THROW(p.readBlock(1, rect, good.view), InvalidArgumentError);
    BlockView null;
    EXPECT_THROW(p.readBlock(0, rect, null), InvalidArgumentError);
    EXPECT_EQ(source->readCount(), 0);
    EXPECT_NO_THROW(p.readBlock(0, rect, good.view));
}

TEST(LazyScene, GlobalContextIsRefused) {
    ts::registerGlobalOperations();
    auto source = textureSource({{256, 256, 0, 0}});
    PipelineSpec spec;
    spec.operation("test.floatSum", OperationParameters());
    Pipeline p(ts::makeSyntheticScene(source), config(64), spec);
    EXPECT_NE(messageOf<InvalidArgumentError>([&] { p.asScene(); }).find("runInto"), std::string::npos);
    EXPECT_NE(messageOf<InvalidArgumentError>([&] { p.levelCount(); }).find("runInto"), std::string::npos);
    OwnedBlock block(16, 16, {DataType::Float32});
    EXPECT_NE(messageOf<InvalidArgumentError>([&] { p.readBlock(0, Rect64{0, 0, 16, 16}, block.view); })
                  .find("runInto"),
              std::string::npos);
    EXPECT_EQ(source->readCount(), 0);
}

TEST(LazyScene, MixedTypeOutputPlanesAreSeparateChannels) {
    registerCopyOperation();
    constexpr int width = 120;
    constexpr int height = 90;
    auto source = textureSource({{width, height, 0, 0}},
                                {slideio::DataType::DT_Byte, slideio::DataType::DT_Float32});
    PipelineSpec spec;
    spec.operation("test.lazy.copy", OperationParameters());
    Pipeline p(ts::makeSyntheticScene(source), config(32), spec);
    const auto scene = p.asScene();
    ASSERT_EQ(scene->getNumChannels(), 2);
    EXPECT_EQ(scene->getChannelDataType(0), slideio::DataType::DT_Byte);
    EXPECT_EQ(scene->getChannelDataType(1), slideio::DataType::DT_Float32);

    std::vector<uint8_t> bytes(static_cast<size_t>(width * height));
    scene->readBlockChannels(std::make_tuple(0, 0, width, height), {0}, bytes.data(), bytes.size());
    std::vector<float> floats(static_cast<size_t>(width * height));
    scene->readBlockChannels(std::make_tuple(0, 0, width, height), {1}, floats.data(), floats.size() * 4);
    int64_t mismatches = 0;
    for (int64_t y = 0; y < height; ++y) {
        for (int64_t x = 0; x < width; ++x) {
            const size_t at = static_cast<size_t>(y * width + x);
            mismatches += bytes[at] == static_cast<uint8_t>(texture(0, x, y)) ? 0 : 1;
            mismatches += floats[at] == static_cast<float>(texture(1, x, y)) ? 0 : 1;
        }
    }
    EXPECT_EQ(mismatches, 0);

    // One block cannot mix the two types.
    std::vector<uint8_t> both(static_cast<size_t>(width * height * 8));
    EXPECT_ANY_THROW(
        scene->readBlockChannels(std::make_tuple(0, 0, width, height), {0, 1}, both.data(), both.size()));
}

// The scene keeps the bindings it was built with; the Pipeline itself follows a
// later binding call. Pixels outside a mask source's mask read as zero.
TEST(LazyScene, SceneKeepsItsBindings) {
    constexpr int width = 200;
    constexpr int height = 160;
    auto source = textureSource({{width, height, 0, 0}});
    PipelineSpec spec;
    spec.operation("test.threshold", thresholdAt(0.0));  // 255 everywhere it is computed
    Pipeline p(ts::makeSyntheticScene(source), config(64), spec);
    const auto scene = p.asScene();

    ts::DiscMaskSource disc(100.0, 80.0, 50.0);
    p.maskSource(&disc);
    OwnedBlock block(width, height, {DataType::Byte});
    p.readBlock(0, Rect64{0, 0, width, height}, block.view);
    const auto unmasked = readScene(scene, 0, 0, width, height, width, height, 1);
    int64_t mismatches = 0;
    for (int64_t y = 0; y < height; ++y) {
        for (int64_t x = 0; x < width; ++x) {
            const size_t at = static_cast<size_t>(y * width + x);
            const bool inside = disc.contains(x + 0.5, y + 0.5);
            mismatches += block.storage[0][at] == (inside ? 255 : 0) ? 0 : 1;
            mismatches += unmasked[at] == 255 ? 0 : 1;
        }
    }
    EXPECT_EQ(mismatches, 0);
    EXPECT_GT(disc.fillCount(), 0);
}

// A pipeline whose input is another pipeline's scene, both on one budget whose
// ceiling is one outer tile plus one inner tile, with several workers: the workers
// holding outer tiles read inner tiles without waiting for bytes only they could
// free, so the run completes. A regression would deadlock; the wait is bounded so
// the test fails instead of hanging.
TEST(LazyScene, NestedPipelinesShareOneBudget) {
    constexpr int64_t width = 512;
    constexpr int64_t height = 512;
    auto source = textureSource({{width, height, 0, 0}});
    const Configuration innerConfig = config(128);
    const Configuration outerConfig = ConfigurationBuilder().tileSize(64, 64).tileSizeFromSlide(false).workerCount(4).build();
    PipelineSpec outerSpec;
    outerSpec.operation("test.threshold", thresholdAt(128.0));

    auto inner = std::make_shared<Pipeline>(ts::makeSyntheticScene(source), innerConfig, boxMean(2));
    inner->validate();
    const int64_t innerBytes = detail::PipelineAccess::plan(*inner).tileBytes;
    Pipeline probe(inner->asScene(), outerConfig, outerSpec);
    probe.validate();
    const int64_t outerBytes = detail::PipelineAccess::plan(probe).tileBytes;
    // Several outer tiles fit at once, and then no inner tile does.
    ASSERT_GE(innerBytes + outerBytes, 2 * outerBytes);
    ASSERT_LT((innerBytes + outerBytes) % outerBytes, innerBytes);
    ASSERT_LE((innerBytes + outerBytes) / outerBytes, outerConfig.workerCount());

    const MemoryBudgetPtr shared = makeMemoryBudget(innerBytes + outerBytes);
    inner->memoryBudget(shared);
    auto outer = std::make_shared<Pipeline>(inner->asScene(), outerConfig, outerSpec);
    outer->memoryBudget(shared);
    auto sink = std::make_shared<CaptureSink>(width, height, std::vector<int64_t>{1});
    outer->rasterSink(sink.get());

    PipelineSpec chain = boxMean(2);
    chain.operation("test.threshold", thresholdAt(128.0));
    const auto expected = eager(ts::makeSyntheticScene(source), chain, 64, width, height, 1);

    auto done = std::make_shared<std::promise<void>>();
    std::future<void> finished = done->get_future();
    std::thread([outer, sink, inner, done]() {
        try {
            outer->run(nullptr, CancellationToken());
            done->set_value();
        } catch (...) {
            done->set_exception(std::current_exception());
        }
    }).detach();
    if (finished.wait_for(std::chrono::seconds(30)) != std::future_status::ready) {
        std::fprintf(stderr, "LazyScene.NestedPipelinesShareOneBudget: the run deadlocked\n");
        std::fflush(stderr);
        std::_Exit(1);  // The run's threads cannot be stopped; failing beats hanging.
    }
    EXPECT_NO_THROW(finished.get());
    EXPECT_TRUE(sink->plane(0) == expected);
    EXPECT_EQ(shared->reservedBytes(), 0);
}
