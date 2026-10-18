// Requirements: UR-STG-3, UR-STG-5, UR-EXT-4, UR-QA-3
#include "fake_object_source.hpp"

#include <slideio/processor/core/block_view.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/core/raster_mask.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>
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
    // [-3, 19) grown by one level-0 pixel on each side against rounding slivers.
    EXPECT_EQ(src.lastQuery().intersectingValue(), (Rect64{-4, -4, 24, 24}));
    EXPECT_TRUE(m.test({15, 5})); // dilated into the tile
    EXPECT_FALSE(m.test({14, 5})); // and no further
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
    EXPECT_EQ(buf[11 * 12 + 0], -1); // no object
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

TEST(LayerMaskSource, ScaleOtherThanOneUsesLevelRadiusAndLevel0Query) {
    FakeObjectSource src({rec(1, 81, 40, 6)}); // level pixels x 27..28, y 13..15 at scale 3
    auto ms = makeLayerMaskSource(src, LayerId(1), 4.0); // r = ceil(4 / 3) = 2
    RasterMask m(Rect64{10, 10, 16, 16}, 1, 3.0);
    ms->fillMask(m);
    // [3 * (10 - 2), 3 * (28 + 2)) grown by one level-0 pixel on each side.
    EXPECT_EQ(src.lastQuery().intersectingValue(),
              (Rect64{3 * (10 - 2) - 1, 3 * (10 - 2) - 1, 60 + 2, 60 + 2}));
    EXPECT_TRUE(m.test({25, 14}));
    EXPECT_FALSE(m.test({24, 14})); // r is 2, not 3
}

TEST(LayerRasterSource, SmallestIdWinsWhateverTheInputOrder) {
    FakeObjectSource src({rec(1, 4, 4, 8), rec(1, 1, 1, 8)});
    auto rs = makeLayerRasterSource(src, LayerId(1), LayerRasterContent::ObjectAnchor);
    std::vector<int64_t> buf(12 * 12, -1);
    BlockPlane pl{buf.data(), DataType::Int64, 12 * 8, 0};
    BlockView v(12, 12, Rect64{0, 0, 12, 12}, 0, &pl, 1);
    rs->fillTile(v);
    const int64_t a = ObjectId::fromAnchor(LayerId(1), {1, 1}).anchorPacked();
    EXPECT_EQ(buf[5 * 12 + 5], a); // overlap pixel: the smaller id, not the later record
}

TEST(LayerRasterSource, QueryCoversTheWholeRasterisedGrid) {
    // sx = 4, origin round(20481 / 4) = 5120: the grid spans [20480, 21504), wider than slideRect.
    FakeObjectSource src({rec(1, 20480, 100, 1)});
    auto rs = makeLayerRasterSource(src, LayerId(1), LayerRasterContent::Membership);
    std::vector<uint8_t> buf(256 * 256, 0);
    BlockPlane pl{buf.data(), DataType::Byte, 256, 0};
    BlockView v(256, 256, Rect64{20481, 0, 1024, 1024}, 0, &pl, 1);
    rs->fillTile(v);
    // The grid [20480, 21504) x [0, 1024), grown by one level-0 pixel on each side.
    EXPECT_EQ(src.lastQuery().intersectingValue(), (Rect64{20479, -1, 1026, 1026}));
    EXPECT_EQ(buf[25 * 256 + 0], 1); // the sliver object lands in pixel (0, 25)
}

TEST(LayerRasterSource, AnchorAtOriginIsZeroAndBackgroundIsMinusOne) {
    // A full-image annotation anchored at (0, 0) packs to 0, so 0 cannot mean "none".
    FakeObjectSource src({rec(1, 0, 0, 4)});
    auto rs = makeLayerRasterSource(src, LayerId(1), LayerRasterContent::ObjectAnchor);
    std::vector<int64_t> buf(8 * 8, 123);
    BlockPlane pl{buf.data(), DataType::Int64, 8 * 8, 0};
    BlockView v(8, 8, Rect64{0, 0, 8, 8}, 0, &pl, 1);
    rs->fillTile(v);
    ASSERT_EQ(ObjectId::fromAnchor(LayerId(1), {0, 0}).anchorPacked(), 0);
    for (int64_t y = 0; y < 8; ++y) {
        for (int64_t x = 0; x < 8; ++x) {
            const int64_t expected = (x < 4 && y < 4) ? 0 : -1;
            EXPECT_EQ(buf[static_cast<size_t>(y * 8 + x)], expected) << x << "," << y;
        }
    }
}

TEST(LayerRasterSource, AnchorContentSkipsRecordsWithAnInvalidId) {
    // rec(1, -4, 0, 8) anchors at x = -4, outside the packable range, so its id is
    // invalid; it sorts before every valid id and would otherwise win the overlap.
    FakeObjectSource src({rec(1, -4, 0, 8), rec(1, 2, 2, 4)});
    ASSERT_FALSE(rec(1, -4, 0, 8).id().isValid());
    auto rs = makeLayerRasterSource(src, LayerId(1), LayerRasterContent::ObjectAnchor);
    std::vector<int64_t> buf(8 * 8, 123);
    BlockPlane pl{buf.data(), DataType::Int64, 8 * 8, 0};
    BlockView v(8, 8, Rect64{0, 0, 8, 8}, 0, &pl, 1);
    rs->fillTile(v);
    const int64_t valid = ObjectId::fromAnchor(LayerId(1), {2, 2}).anchorPacked();
    EXPECT_EQ(buf[2 * 8 + 2], valid); // overlap: the valid object, not the skipped one
    EXPECT_EQ(buf[0 * 8 + 0], -1);    // covered only by the skipped record
    EXPECT_EQ(buf[5 * 8 + 5], valid);
    EXPECT_EQ(buf[7 * 8 + 7], -1);
}

namespace {
// An ObjectSource that breaks the contract by throwing a non-ProcessorError.
template <typename E>
class ThrowingSource final : public ObjectSource {
public:
    CursorPtr query(const ObjectQuery&) override { throw E("source failed"); }
    int64_t count(const ObjectQuery&) const override { return 0; }
    bool get(ObjectId, ObjectRecord&) const override { return false; }
};
class BadAllocSource final : public ObjectSource {
public:
    CursorPtr query(const ObjectQuery&) override { throw std::bad_alloc(); }
    int64_t count(const ObjectQuery&) const override { return 0; }
    bool get(ObjectId, ObjectRecord&) const override { return false; }
};
} // namespace

TEST(LayerSources, ForeignExceptionsFromTheSourceAreTranslated) {
    ThrowingSource<std::runtime_error> bad;
    RasterMask m(Rect64{0, 0, 4, 4}, 0, 1.0);
    auto ms = makeLayerMaskSource(bad, LayerId(1), 0.0);
    try {
        ms->fillMask(m);
        FAIL() << "expected InternalError";
    } catch (const InternalError& e) {
        EXPECT_STREQ(e.cause(), "source failed");
    }
    std::vector<uint8_t> buf(4 * 4, 0);
    BlockPlane pl{buf.data(), DataType::Byte, 4, 0};
    BlockView v(4, 4, Rect64{0, 0, 4, 4}, 0, &pl, 1);
    auto rs = makeLayerRasterSource(bad, LayerId(1), LayerRasterContent::Membership);
    EXPECT_THROW(rs->fillTile(v), InternalError);
    // std::bad_alloc is the one non-ProcessorError that crosses unchanged.
    BadAllocSource oom;
    auto ms2 = makeLayerMaskSource(oom, LayerId(1), 0.0);
    EXPECT_THROW(ms2->fillMask(m), std::bad_alloc);
}

TEST(LayerMaskSource, RadiusPastInt32IsRejectedNotClamped) {
    FakeObjectSource src({rec(1, 4, 4, 2)});
    // r = ceil(3e9 / 1) level pixels: clamping to 2^31 - 1 would dilate less than asked.
    auto ms = makeLayerMaskSource(src, LayerId(1), 3e9);
    RasterMask m(Rect64{0, 0, 16, 16}, 0, 1.0);
    EXPECT_THROW(ms->fillMask(m), InvalidArgumentError);
    // At a coarse enough level the same dilation is a small radius: ceil(3e9 / 1e9) = 3.
    RasterMask coarse(Rect64{0, 0, 4, 4}, 5, 1e9);
    EXPECT_NO_THROW(ms->fillMask(coarse));
    EXPECT_EQ(coarse.setPixelCount(), 16);
}
