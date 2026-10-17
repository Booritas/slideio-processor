// Requirements: UR-STG-3, UR-STG-5, UR-EXT-4, UR-QA-3
#include "fake_object_source.hpp"

#include <slideio/processor/core/block_view.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/core/raster_mask.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <vector>

using namespace slideio::processor;

TEST(LayerMaskSource, QueriesCompletedOnlyAndRasterises) {
    FakeObjectSource src({rec(1, 4, 4, 4), rec(2, 0, 0, 16)});
    auto ms = makeLayerMaskSource(src, LayerId(1), 0.0);
    RasterMask m(Rect64{0, 0, 16, 16}, 0, 1.0);
    ms->fillMask(m);
    EXPECT_EQ(m.setPixelCount(), 16); // layer 2 excluded
    EXPECT_EQ(src.lastQuery().visibilityValue(), StageVisibility::CompletedOnly);
    EXPECT_EQ(src.lastQuery().layerValue(), LayerId(1));
    EXPECT_TRUE(src.lastCursorClosed());
}

TEST(LayerMaskSource, DilationReachesObjectsOutsideTheTile) {
    FakeObjectSource src({rec(1, 18, 4, 2)}); // starts 2 px right of the tile
    auto ms = makeLayerMaskSource(src, LayerId(1), 3.0);
    RasterMask m(Rect64{0, 0, 16, 16}, 0, 1.0);
    ms->fillMask(m);
    EXPECT_EQ(src.lastQuery().intersectingValue(), (Rect64{-3, -3, 22, 22}));
    EXPECT_TRUE(m.test({15, 5})); // dilated into the tile
    EXPECT_THROW(makeLayerMaskSource(src, LayerId(), 0.0), InvalidArgumentError);
    EXPECT_THROW(makeLayerMaskSource(src, LayerId(1), -1.0), InvalidArgumentError);
    EXPECT_THROW(makeLayerMaskSource(src, LayerId(1), std::numeric_limits<double>::quiet_NaN()),
                 InvalidArgumentError);
    EXPECT_THROW(makeLayerMaskSource(src, LayerId(1), std::numeric_limits<double>::infinity()),
                 InvalidArgumentError);
}

TEST(LayerMaskSource, OverwritesOutAndRejectsNull) {
    FakeObjectSource src({rec(1, 4, 4, 2)});
    auto ms = makeLayerMaskSource(src, LayerId(1), 0.0);
    RasterMask m(Rect64{0, 0, 16, 16}, 0, 1.0);
    for (int64_t y = 0; y < 16; ++y) {
        for (int64_t x = 0; x < 16; ++x) {
            m.row(y)[x] = 255;
        }
    }
    ms->fillMask(m);
    EXPECT_EQ(m.setPixelCount(), 4);
    RasterMask null;
    EXPECT_THROW(ms->fillMask(null), InvalidArgumentError);
}

TEST(LayerRasterSource, AnchorContentSmallestIdWins) {
    FakeObjectSource src({rec(1, 1, 1, 8), rec(1, 4, 4, 8)}); // overlap at [4,9)
    auto rs = makeLayerRasterSource(src, LayerId(1), LayerRasterContent::ObjectAnchor);
    EXPECT_EQ(rs->planeType(), DataType::Int64);
    EXPECT_EQ(rs->planeCount(), 1);
    std::vector<int64_t> buf(12 * 12, -1);
    BlockPlane pl{buf.data(), DataType::Int64, 12 * 8, 0};
    BlockView v(12, 12, Rect64{0, 0, 12, 12}, 0, &pl, 1);
    rs->fillTile(v);
    int64_t a = ObjectId::fromAnchor(LayerId(1), {1, 1}).anchorPacked();
    int64_t b = ObjectId::fromAnchor(LayerId(1), {4, 4}).anchorPacked();
    ASSERT_LT(a, b);
    EXPECT_EQ(buf[5 * 12 + 5], a);
    EXPECT_EQ(buf[10 * 12 + 10], b);
    EXPECT_EQ(buf[11 * 12 + 0], 0);
    EXPECT_EQ(src.lastQuery().visibilityValue(), StageVisibility::CompletedOnly);
    EXPECT_TRUE(src.lastCursorClosed());
}

TEST(LayerRasterSource, LabelAndMembershipAndTypeCheck) {
    FakeObjectSource src({rec(1, 0, 0, 2, 7), rec(1, 2, 0, 2)});
    auto lab = makeLayerRasterSource(src, LayerId(1), LayerRasterContent::Label);
    EXPECT_EQ(lab->planeType(), DataType::Int32);
    std::vector<int32_t> l(4 * 2);
    BlockPlane pl{l.data(), DataType::Int32, 16, 0};
    BlockView v(4, 2, Rect64{0, 0, 4, 2}, 0, &pl, 1);
    lab->fillTile(v);
    EXPECT_EQ(l[0], 7);
    EXPECT_EQ(l[3], 0);
    auto mem = makeLayerRasterSource(src, LayerId(1), LayerRasterContent::Membership);
    EXPECT_EQ(mem->planeType(), DataType::Byte);
    EXPECT_THROW(mem->fillTile(v), InvalidArgumentError); // plane is Int32, not Byte
    std::vector<uint8_t> mb(4 * 2, 9);
    BlockPlane pb{mb.data(), DataType::Byte, 4, 0};
    BlockView vb(4, 2, Rect64{0, 0, 4, 2}, 0, &pb, 1);
    mem->fillTile(vb);
    EXPECT_EQ(mb[0], 1);
    EXPECT_EQ(mb[3], 1);
    EXPECT_THROW(makeLayerRasterSource(src, LayerId(), LayerRasterContent::Label),
                 InvalidArgumentError);
}
