// Requirements: UR-RES-7
// rasteriseLabels() (reporting plan C, decision 4): a lazy Int32 scene over a label mask.
#include <slideio/processor/masks/label_scene.hpp>

#include "masks/mask_fixture.hpp"

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/reporting/label_mask.hpp>

#include <slideio/slideio/scene.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <tuple>
#include <vector>

using namespace slideio::processor;
using Masks = masks_test::MaskStoreTest;

namespace {

// makeLabelMaskSource's tile of `w` x `h` pixels over the level-0 rect.
std::vector<int32_t> sourceTile(Store& store, const ObjectQuery& query, const Rect64& rect, int64_t w, int64_t h) {
    TileRasterSourcePtr source = makeLabelMaskSource(store, query);
    std::vector<int32_t> pixels(static_cast<size_t>(w * h), -1);
    BlockPlane plane{pixels.data(), DataType::Int32, w * static_cast<int64_t>(sizeof(int32_t)), 0};
    BlockView view(w, h, rect, 0, &plane, 1);
    source->fillTile(view);
    return pixels;
}

std::vector<int32_t> sceneBlock(slideio::Scene& scene, int x, int y, int w, int h, int bw, int bh) {
    std::vector<int32_t> pixels(static_cast<size_t>(bw) * static_cast<size_t>(bh), -1);
    scene.readResampledBlock(std::make_tuple(x, y, w, h), std::make_tuple(bw, bh), pixels.data(),
                             pixels.size() * sizeof(int32_t));
    return pixels;
}

} // namespace

TEST_F(Masks, LabelSceneIsLazyAndExact) {
    const ObjectQuery query = all();
    std::shared_ptr<slideio::Scene> scene = rasteriseLabels(store(), query, Rect64{0, 0, kWidth, kHeight});
    ASSERT_TRUE(scene);

    // Laziness, shown without timing: an object stored after the scene was created is in
    // its pixels, so the store was first queried by the read, not by the creation.
    addLate(700, 100, 80, 8);

    EXPECT_EQ(scene->getRect(), std::make_tuple(0, 0, kWidth, kHeight));
    ASSERT_EQ(scene->getNumChannels(), 1);
    EXPECT_EQ(scene->getChannelDataType(0), slideio::DataType::DT_Int32);
    EXPECT_EQ(scene->getNumZSlices(), 1);
    EXPECT_EQ(scene->getNumTFrames(), 1);
    EXPECT_DOUBLE_EQ(std::get<0>(scene->getResolution()), 0.0);

    // A full-resolution read equals the label source's tile over the same rect.
    const std::vector<int32_t> block = sceneBlock(*scene, 50, 40, 900, 700, 900, 700);
    EXPECT_EQ(block, sourceTile(store(), query, Rect64{50, 40, 900, 700}, 900, 700));
    const auto at = [&](int x, int y) { return block[static_cast<size_t>((y - 40) * 900 + (x - 50))]; };
    EXPECT_EQ(at(150, 150), 9);
    EXPECT_EQ(at(275, 275), 9);  // the overlap: the lower ObjectId wins, though its label is larger
    EXPECT_EQ(at(400, 400), 3);
    EXPECT_EQ(at(700, 500), 5);
    EXPECT_EQ(at(720, 120), 8);   // stored after the scene was created
    EXPECT_EQ(at(60, 50), 0);
    ASSERT_LT(first, second);

    // A reduced read is rasterised at its own size, not resampled from level 0.
    EXPECT_EQ(sceneBlock(*scene, 0, 0, kWidth, kHeight, 250, 200),
              sourceTile(store(), query, Rect64{0, 0, kWidth, kHeight}, 250, 200));
}

TEST_F(Masks, LabelSceneClipsToItsRegion) {
    const ObjectQuery query = all();
    // A region that starts off the origin: scene pixel (x, y) is level-0 (200 + x, 100 + y).
    std::shared_ptr<slideio::Scene> scene = rasteriseLabels(store(), query, Rect64{200, 100, 800, 700});
    EXPECT_EQ(scene->getRect(), std::make_tuple(0, 0, 800, 700));
    EXPECT_EQ(sceneBlock(*scene, 0, 0, 800, 700, 800, 700),
              sourceTile(store(), query, Rect64{200, 100, 800, 700}, 800, 700));

    // Past the far corner the square running out of the region and the square outside it
    // read 0: a read is clipped to the scene.
    const std::vector<int32_t> edge = sceneBlock(*scene, 700, 600, 200, 200, 200, 200);
    EXPECT_EQ(edge[static_cast<size_t>(50 * 200 + 50)], 7);     // level-0 (950, 750), inside
    EXPECT_EQ(edge[static_cast<size_t>(150 * 200 + 150)], 0);   // level-0 (1050, 850), outside
    const std::vector<int32_t> right = sceneBlock(*scene, 850, 0, 100, 100, 100, 100);
    for (int32_t value : right) EXPECT_EQ(value, 0);            // level-0 x >= 1050
}

TEST_F(Masks, LabelSceneRefusesBadArguments) {
    ObjectQuery noLayer;
    EXPECT_THROW(rasteriseLabels(store(), noLayer, Rect64{0, 0, 10, 10}), InvalidArgumentError);
    EXPECT_THROW(rasteriseLabels(store(), all(), Rect64{0, 0, 0, 10}), InvalidArgumentError);
    EXPECT_THROW(rasteriseLabels(store(), all(), Rect64{0, 0, int64_t{1} << 31, 10}), InvalidArgumentError);
}
