// Requirements: UR-RES-7
#include <slideio/processor/reporting/label_mask.hpp>

#include "reporting/export_fixture.hpp"

#include <slideio/processor/core/block_view.hpp>
#include <slideio/processor/core/error.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace slideio::processor {
namespace {

constexpr int64_t kSide = 64;
constexpr int32_t kTumour = 1;
constexpr int32_t kStroma = 2;
constexpr int32_t kKi67 = 3;

ObjectRecord record(LayerId layer, int64_t x, int64_t y, int64_t side, int32_t label) {
    ObjectRecord r;
    r.setLayer(layer);
    r.setGeometry(export_test::square(x, y, side));
    if (label != 0) r.setLabel(LabelId(label));
    return r;
}

// Fills a size x size Int32 view over `extent` level-0 pixels from the origin.
std::vector<int32_t> fill(TileRasterSource& source, int64_t size, int64_t extent = kSide) {
    std::vector<int32_t> pixels(static_cast<size_t>(size * size), -1);
    BlockPlane plane{pixels.data(), DataType::Int32, size * static_cast<int64_t>(sizeof(int32_t)), 0};
    BlockView view(size, size, Rect64{0, 0, extent, extent}, 0, &plane, 1);
    source.fillTile(view);
    return pixels;
}

int32_t at(const std::vector<int32_t>& pixels, int64_t x, int64_t y, int64_t size = kSide) {
    return pixels[static_cast<size_t>(y * size + x)];
}

// Layer nuclei. Stage a: object 1 (tumour row label) and object 2 (stroma row label)
// overlap on [10, 20)^2. Stage b: object 3, no row label. Stage c asserts ki67 on
// objects 2 and 3. All stages are complete.
class LabelMaskTest : public StoreFixture {
protected:
    void SetUp() override {
        StoreFixture::SetUp();
        LayerRegistry& layers = store().layers();
        nuclei = layers.declareSemantic("nuclei", layers.pixelLayer());
        stageA = store().stageStateWriter().declare("stage-a", "{}");
        stageB = store().stageStateWriter().declare("stage-b", "{}");
        stageC = store().stageStateWriter().declare("stage-c", "{}");
        {
            ObjectWriterPtr writer = store().objects().writerForStage(stageA);
            ObjectRecord first = record(nuclei, 0, 0, 20, kTumour);
            ObjectRecord second = record(nuclei, 10, 10, 20, kStroma);
            writer->emit(first);
            writer->emit(second);
            writer->flush();
            one = first.id();
            two = second.id();
        }
        {
            ObjectWriterPtr writer = store().objects().writerForStage(stageB);
            ObjectRecord third = record(nuclei, 40, 40, 16, 0);
            writer->emit(third);
            writer->flush();
            three = third.id();
        }
        store().stageStateWriter().setStatus(stageA, StageStatus::Complete);
        store().stageStateWriter().setStatus(stageB, StageStatus::Complete);
        AnnotationWriterPtr annotations = store().annotations().writerForStage(stageC);
        annotations->assertLabel(two, LabelId(kKi67));
        annotations->assertLabel(three, LabelId(kKi67));
        annotations->flush();
        store().stageStateWriter().setStatus(stageC, StageStatus::Complete);
    }

    LayerId nuclei;
    StageId stageA, stageB, stageC;
    ObjectId one, two, three;
};

TEST_F(LabelMaskTest, ValuesFollowTheQuery) {
    ObjectQuery all;
    all.layer(nuclei);
    TileRasterSourcePtr source = makeLabelMaskSource(store(), all);
    ASSERT_TRUE(source);
    EXPECT_EQ(source->planeType(), DataType::Int32);
    EXPECT_EQ(source->planeCount(), 1);
    const std::vector<int32_t> pixels = fill(*source, kSide);
    EXPECT_EQ(at(pixels, 5, 5), kTumour);
    EXPECT_EQ(at(pixels, 15, 15), kTumour);  // the overlap: the smaller ObjectId wins
    EXPECT_EQ(at(pixels, 25, 25), kStroma);
    EXPECT_EQ(at(pixels, 45, 45), kKi67);  // object 3 has no row label: its first effective label
    EXPECT_EQ(at(pixels, 60, 60), 0);

    ObjectQuery ki67;
    ki67.layer(nuclei).label(LabelId(kKi67));
    TileRasterSourcePtr filtered = makeLabelMaskSource(store(), ki67);
    const std::vector<int32_t> mask = fill(*filtered, kSide);
    EXPECT_EQ(at(mask, 5, 5), 0);  // object 1 carries no ki67
    EXPECT_EQ(at(mask, 15, 15), kKi67);
    EXPECT_EQ(at(mask, 25, 25), kKi67);
    EXPECT_EQ(at(mask, 45, 45), kKi67);
    EXPECT_EQ(at(mask, 60, 60), 0);
}

TEST_F(LabelMaskTest, RequiresALayer) {
    ObjectQuery none;
    EXPECT_THROW(makeLabelMaskSource(store(), none), InvalidArgumentError);
    ObjectQuery stageOnly;
    stageOnly.stage(stageA);
    EXPECT_THROW(makeLabelMaskSource(store(), stageOnly), InvalidArgumentError);
}

TEST_F(LabelMaskTest, QueryFiltersApply) {
    ObjectQuery query;
    query.layer(nuclei).stage(stageB);
    TileRasterSourcePtr source = makeLabelMaskSource(store(), query);
    const std::vector<int32_t> pixels = fill(*source, kSide);
    EXPECT_EQ(at(pixels, 5, 5), 0);    // stage a's objects are omitted
    EXPECT_EQ(at(pixels, 25, 25), 0);
    EXPECT_EQ(at(pixels, 45, 45), kKi67);

    // The query's own box is intersected with the tile's: nothing outside it is drawn.
    ObjectQuery boxed;
    boxed.layer(nuclei).intersecting(Rect64{0, 0, 30, 30});
    TileRasterSourcePtr inBox = makeLabelMaskSource(store(), boxed);
    const std::vector<int32_t> clipped = fill(*inBox, kSide);
    EXPECT_EQ(at(clipped, 5, 5), kTumour);
    EXPECT_EQ(at(clipped, 45, 45), 0);

    ObjectQuery elsewhere;
    elsewhere.layer(nuclei).intersecting(Rect64{1000, 1000, 10, 10});
    TileRasterSourcePtr nothing = makeLabelMaskSource(store(), elsewhere);
    for (int32_t v : fill(*nothing, kSide)) EXPECT_EQ(v, 0);
}

// Storage's conservative-coverage rule (core's rasteriseInto): a pixel is covered when an
// edge passes through its open interior or its centre is inside. A coarser pixel therefore
// covers whatever its top-left finer sample covers, and where no edge crosses the coarse
// pixel the two agree exactly. Objects with even edges give exact equality with the 2x2
// top-left sampling; an object with odd edges only guarantees that the coarse pixel shows
// the object wherever the fine top-left sample does.
TEST_F(LabelMaskTest, ScaledTilesMatchTheirLevel) {
    const StageId stageD = store().stageStateWriter().declare("stage-d", "{}");
    {
        ObjectWriterPtr writer = store().objects().writerForStage(stageD);
        writer->emit(record(nuclei, 3, 45, 7, 9));  // odd edges, clear of the others
        writer->flush();
    }
    store().stageStateWriter().setStatus(stageD, StageStatus::Complete);

    ObjectQuery query;
    query.layer(nuclei);
    TileRasterSourcePtr source = makeLabelMaskSource(store(), query);
    const std::vector<int32_t> fine = fill(*source, kSide);
    const std::vector<int32_t> coarse = fill(*source, kSide / 2);

    int64_t odd = 0;
    for (int64_t j = 0; j < kSide / 2; ++j) {
        for (int64_t i = 0; i < kSide / 2; ++i) {
            const int32_t sample = at(fine, 2 * i, 2 * j);
            const int32_t value = at(coarse, i, j, kSide / 2);
            const bool oddObject = 2 * i >= 2 && 2 * i < 12 && 2 * j >= 44 && 2 * j < 54;
            if (oddObject) {
                if (sample != 0) EXPECT_EQ(value, sample) << i << "," << j;
                ++odd;
            } else {
                EXPECT_EQ(value, sample) << i << "," << j;
            }
        }
    }
    EXPECT_GT(odd, 0);
}

} // namespace
} // namespace slideio::processor
