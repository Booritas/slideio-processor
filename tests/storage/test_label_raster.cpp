// Requirements: UR-RES-4, UR-QA-3, UR-STG-5, UR-EXT-4
#include "storage/store_fixture.hpp"

#include <slideio/processor/core/block_view.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/core/object_record.hpp>
#include <slideio/processor/storage/annotation_store.hpp>
#include <slideio/processor/storage/layer_registry.hpp>
#include <slideio/processor/storage/object_store.hpp>
#include <slideio/processor/storage/store.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

using namespace slideio::processor;

namespace {

constexpr int64_t kSide = 64;

Polygon square(int64_t x, int64_t y, int64_t side) {
    const Point64 points[] = {{x, y}, {x + side, y}, {x + side, y + side}, {x, y + side}};
    Polygon polygon;
    polygon.addRing(points, 4, false);
    return polygon;
}

ObjectRecord rec(LayerId layer, int64_t x, int64_t y, int64_t side) {
    ObjectRecord record;
    record.setLayer(layer);
    record.setGeometry(square(x, y, side));
    return record;
}

ObjectRecord labelled(LayerId layer, int64_t x, int64_t y, int64_t side, LabelId label) {
    ObjectRecord record = rec(layer, x, y, side);
    record.setLabel(label);
    return record;
}

// Fills a 64x64 view at level 0 over [0, 64)^2 from `source`, and returns its bytes.
std::vector<uint8_t> fill(TileRasterSource& source) {
    const int64_t pixelBytes = dataTypeSizeBytes(source.planeType());
    std::vector<uint8_t> bytes(static_cast<size_t>(kSide * kSide * pixelBytes), 0xAB);
    BlockPlane plane{bytes.data(), source.planeType(), kSide * pixelBytes, 0};
    BlockView view(kSide, kSide, Rect64{0, 0, kSide, kSide}, 0, &plane, 1);
    source.fillTile(view);
    return bytes;
}

// The rectangle [x0, x1) x [y0, y1) as a labelled record of `layer`.
ObjectRecord box(LayerId layer, int64_t x0, int64_t y0, int64_t x1, int64_t y1, LabelId label) {
    const Point64 points[] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    ObjectRecord record;
    record.setLayer(layer);
    Polygon polygon;
    polygon.addRing(points, 4, false);
    record.setGeometry(polygon);
    if (label.isValid()) record.setLabel(label);
    return record;
}

// Fills a 64x64 view at `level` over `slideRect` from `source`, and returns its bytes.
std::vector<uint8_t> fillView(TileRasterSource& source, Rect64 slideRect, int32_t level) {
    const int64_t pixelBytes = dataTypeSizeBytes(source.planeType());
    std::vector<uint8_t> bytes(static_cast<size_t>(kSide * kSide * pixelBytes), 0xAB);
    BlockPlane plane{bytes.data(), source.planeType(), kSide * pixelBytes, 0};
    BlockView view(kSide, kSide, slideRect, level, &plane, 1);
    source.fillTile(view);
    return bytes;
}

// The Int32 pixel (x, y) of a Label fill.
int32_t labelAt(const std::vector<uint8_t>& bytes, int64_t x, int64_t y) {
    int32_t value = 0;
    std::memcpy(&value, bytes.data() + (y * kSide + x) * sizeof(int32_t), sizeof(int32_t));
    return value;
}

// Layers: cells on the pixel layer. Stage a produces objects; c and d annotate them.
class LabelRaster : public StoreFixture {
protected:
    void SetUp() override {
        StoreFixture::SetUp();
        LayerRegistry& layers = store().layers();
        cells = layers.declareSemantic("cells", layers.pixelLayer());
        sp = layers.declarePrimitive("sp");
        a = store().stageStateWriter().declare("stage-a", "");
        c = store().stageStateWriter().declare("stage-c", "");
        d = store().stageStateWriter().declare("stage-d", "");
    }
    AnnotationStore& annotations() { return store().annotations(); }
    void complete(StageId stage) { store().stageStateWriter().setStatus(stage, StageStatus::Complete); }
    // Commits the records under `stage` and returns their ids, in order.
    std::vector<ObjectId> commit(StageId stage, std::initializer_list<ObjectRecord> records) {
        ObjectWriterPtr writer = store().objects().writerForStage(stage);
        std::vector<ObjectId> ids;
        for (const ObjectRecord& r : records) {
            writer->emit(r);
            ids.push_back(r.id());
        }
        writer->flush();
        return ids;
    }
    // Asserts each label on `object`, attributed to `stage`.
    void assertLabels(StageId stage, ObjectId object, std::initializer_list<LabelId> labels) {
        AnnotationWriterPtr writer = annotations().writerForStage(stage);
        for (LabelId label : labels) writer->assertLabel(object, label);
        writer->flush();
    }

    LayerId cells, sp;
    StageId a, c, d;
};

} // namespace

TEST_F(LabelRaster, AssertedLabelReachesTheRaster) {
    const ObjectId squareA = commit(a, {rec(cells, 8, 8, 16)})[0];
    complete(a);
    assertLabels(c, squareA, {LabelId(5)});

    TileRasterSourcePtr label = annotations().layerRasterSource(cells, LayerRasterContent::Label);
    ASSERT_TRUE(label);
    EXPECT_EQ(label->planeType(), DataType::Int32);
    EXPECT_EQ(label->planeCount(), 1);
    std::vector<uint8_t> before = fill(*label);
    EXPECT_EQ(labelAt(before, 10, 10), 0);  // c is not Complete yet
    EXPECT_EQ(labelAt(before, 40, 40), 0);

    complete(c);
    std::vector<uint8_t> after = fill(*label);
    EXPECT_EQ(labelAt(after, 10, 10), 5);
    EXPECT_EQ(labelAt(after, 23, 23), 5);
    EXPECT_EQ(labelAt(after, 24, 24), 0);  // outside the square
    EXPECT_EQ(labelAt(after, 40, 40), 0);

    // The gap the source closes: core's Label reads the row label only.
    TileRasterSourcePtr rowOnly = makeLayerRasterSource(store().objects(), cells, LayerRasterContent::Label);
    EXPECT_EQ(labelAt(fill(*rowOnly), 10, 10), 0);
}

TEST_F(LabelRaster, FixedChoiceAndSmallestIdWins) {
    const std::vector<ObjectId> ids =
        commit(a, {labelled(cells, 0, 0, 20, LabelId(4)), rec(cells, 10, 10, 20)});
    const ObjectId squareA = ids[0], squareB = ids[1];
    ASSERT_LT(squareA, squareB);
    complete(a);
    assertLabels(c, squareA, {LabelId(2)});
    assertLabels(c, squareB, {LabelId(9)});
    assertLabels(d, squareB, {LabelId(3)});
    complete(c);
    complete(d);

    TileRasterSourcePtr label = annotations().layerRasterSource(cells, LayerRasterContent::Label);
    const std::vector<uint8_t> first = fill(*label);
    EXPECT_EQ(labelAt(first, 2, 2), 4);    // A only: the row label before the asserted 2
    EXPECT_EQ(labelAt(first, 25, 25), 3);  // B only: the smallest asserted label
    EXPECT_EQ(labelAt(first, 15, 15), 4);  // overlap: A has the smaller ObjectId
    EXPECT_EQ(labelAt(first, 40, 40), 0);
    EXPECT_EQ(fill(*label), first);
}

TEST_F(LabelRaster, DiscardedProducerLeavesNoStaleLabel) {  // Review Focus 3
    const ObjectId squareA = commit(a, {rec(cells, 8, 8, 16)})[0];
    complete(a);
    assertLabels(c, squareA, {LabelId(5)});
    complete(c);
    TileRasterSourcePtr label = annotations().layerRasterSource(cells, LayerRasterContent::Label);
    ASSERT_EQ(labelAt(fill(*label), 10, 10), 5);

    store().objects().discardStage(a);
    EXPECT_EQ(labelAt(fill(*label), 10, 10), 0);

    // The re-run puts A's geometry back under the same anchor, unlabelled.
    const ObjectId again = commit(a, {rec(cells, 8, 8, 16)})[0];
    ASSERT_EQ(again, squareA);
    complete(a);
    const std::vector<uint8_t> rerun = fill(*label);
    EXPECT_EQ(labelAt(rerun, 10, 10), 0);
    EXPECT_EQ(labelAt(rerun, 23, 23), 0);
}

// With row labels only, the effective label is the row label, so the store's Label source
// must agree byte for byte with core's over store().objects(): same grid, same query
// extent, same order. The views exercise the rounded grid and the padding.
TEST_F(LabelRaster, RowLabelsOnlyMatchCoreByteForByte) {
    commit(a, {
                  // View 1: slideRect {3,30,100,100} at level 0, pixel 1.5625; its grid is
                  // [3.125, 103.125) x [29.6875, 129.6875).
                  box(cells, 10, 20, 30, 30, LabelId(7)),   // edge y=30 inside grid row 0
                  box(cells, 103, 65, 120, 85, LabelId(8)), // edge x=103 inside the last column
                  box(cells, 20, 45, 80, 105, LabelId(3)),
                  box(cells, 50, 75, 90, 115, LabelId(6)),
                  box(cells, 30, 85, 45, 120, LabelId()),
                  // View 2: slideRect {1000,2001,256,256} at level 2, pixel 4; its grid is
                  // [1000, 1256) x [2000, 2256).
                  box(cells, 1010, 1990, 1030, 2001, LabelId(1)), // edge y=2001 inside row 0
                  box(cells, 990, 2100, 1001, 2120, LabelId(2)),  // a quarter of column 0
                  box(cells, 1255, 2050, 1300, 2070, LabelId(4)), // a quarter of column 63
                  box(cells, 1100, 2256, 1120, 2300, LabelId(5)), // touches the grid's bottom
                  box(cells, 1050, 2050, 1200, 2200, LabelId(9)),
                  box(cells, 1100, 2100, 1250, 2250, LabelId()),
              });
    complete(a);

    TileRasterSourcePtr ours = annotations().layerRasterSource(cells, LayerRasterContent::Label);
    TileRasterSourcePtr core = makeLayerRasterSource(store().objects(), cells, LayerRasterContent::Label);

    const Rect64 view1{3, 30, 100, 100};
    const std::vector<uint8_t> first = fillView(*ours, view1, 0);
    EXPECT_EQ(first, fillView(*core, view1, 0));
    EXPECT_EQ(labelAt(first, 10, 0), 7);   // the slivers are drawn, so the comparison is not vacuous
    EXPECT_EQ(labelAt(first, 63, 29), 8);

    const Rect64 view2{1000, 2001, 256, 256};
    const std::vector<uint8_t> second = fillView(*ours, view2, 2);
    EXPECT_EQ(second, fillView(*core, view2, 2));
    EXPECT_EQ(labelAt(second, 5, 0), 1);
    EXPECT_EQ(labelAt(second, 0, 27), 2);
    EXPECT_EQ(labelAt(second, 63, 15), 4);
}

TEST_F(LabelRaster, OtherContentsDelegateAndErrors) {
    commit(a, {labelled(cells, 0, 0, 20, LabelId(4)), rec(cells, 10, 10, 20), rec(cells, 40, 2, 8)});
    complete(a);

    for (LayerRasterContent content : {LayerRasterContent::Membership, LayerRasterContent::ObjectAnchor}) {
        TileRasterSourcePtr ours = annotations().layerRasterSource(cells, content);
        TileRasterSourcePtr core = makeLayerRasterSource(store().objects(), cells, content);
        EXPECT_EQ(ours->planeType(), core->planeType());
        EXPECT_EQ(ours->planeCount(), 1);
        EXPECT_EQ(fill(*ours), fill(*core));
    }
    EXPECT_EQ(annotations().layerRasterSource(cells, LayerRasterContent::Label)->planeType(), DataType::Int32);
    EXPECT_EQ(annotations().layerRasterSource(cells, LayerRasterContent::Membership)->planeType(), DataType::Byte);
    EXPECT_EQ(annotations().layerRasterSource(cells, LayerRasterContent::ObjectAnchor)->planeType(),
              DataType::Int64);

    EXPECT_THROW(annotations().layerRasterSource(LayerId(99), LayerRasterContent::Label), InvalidArgumentError);
    EXPECT_THROW(annotations().layerRasterSource(LayerId(), LayerRasterContent::Label), InvalidArgumentError);
    EXPECT_THROW(annotations().layerRasterSource(store().layers().pixelLayer(), LayerRasterContent::Membership),
                 InvalidArgumentError);
    EXPECT_THROW(annotations().layerRasterSource(cells, static_cast<LayerRasterContent>(7)), InvalidArgumentError);
    // A primitive layer is an ordinary layer of objects.
    EXPECT_EQ(labelAt(fill(*annotations().layerRasterSource(sp, LayerRasterContent::Label)), 2, 2), 0);

    // A view without an Int32 plane 0 is refused, as by core.
    TileRasterSourcePtr label = annotations().layerRasterSource(cells, LayerRasterContent::Label);
    std::vector<uint8_t> bytes(kSide * kSide, 0);
    BlockPlane plane{bytes.data(), DataType::Byte, kSide, 0};
    BlockView byteView(kSide, kSide, Rect64{0, 0, kSide, kSide}, 0, &plane, 1);
    EXPECT_THROW(label->fillTile(byteView), InvalidArgumentError);

    store().close();
    try {
        fill(*label);
        ADD_FAILURE() << "no IoError";
    } catch (const IoError& e) {
        EXPECT_NE(std::string(e.what()).find("store is closed"), std::string::npos) << e.what();
    }
}
