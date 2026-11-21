// Requirements: UR-RES-6, UR-RES-9, UR-QA-3
#include "storage/store_fixture.hpp"

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/reporting/reporting.hpp>
#include <slideio/processor/storage/object_store.hpp>
#include <slideio/processor/storage/store.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

using namespace slideio::processor;

namespace {

ObjectRecord square(LayerId layer, int64_t x) {
    const Point64 points[] = {{x, 0}, {x + 5, 0}, {x + 5, 5}, {x, 5}};
    Polygon polygon;
    polygon.addRing(points, 4, false);
    ObjectRecord record;
    record.setLayer(layer);
    record.setGeometry(polygon);
    return record;
}

class MaterialiseTest : public StoreFixture {
protected:
    void SetUp() override {
        StoreFixture::SetUp();
        nuclei = store().layers().declareSemantic("nuclei", store().layers().pixelLayer());
        const StageId stage = store().stageStateWriter().declare("seg", "");
        ObjectWriterPtr writer = store().objects().writerForStage(stage);
        for (int64_t i = 0; i < 5; ++i) writer->emit(square(nuclei, i * 10));
        writer->flush();
        store().stageStateWriter().setStatus(stage, StageStatus::Complete);
    }
    LayerId nuclei;
};

} // namespace

TEST_F(MaterialiseTest, ReadsAFilteredQuery) {
    const ObjectQuery query = ObjectQuery().layer(nuclei);
    ObjectArrayPtr array = materialise(store(), query, 5);
    ASSERT_NE(array, nullptr);
    ASSERT_EQ(array->count(), 5);
    CursorPtr cursor = store().objects().query(query);
    for (int64_t i = 0; i < 5; ++i) {
        ASSERT_TRUE(cursor->next());
        EXPECT_EQ(array->at(i).id(), cursor->current().id());
    }
    EXPECT_FALSE(cursor->next());
}

TEST_F(MaterialiseTest, RefusesUnfilteredQuery) {
    EXPECT_THROW(materialise(store(), ObjectQuery(), 100), InvalidArgumentError);
}

TEST_F(MaterialiseTest, RefusesNonPositiveCap) {
    const ObjectQuery query = ObjectQuery().layer(nuclei);
    EXPECT_THROW(materialise(store(), query, 0), InvalidArgumentError);
    EXPECT_THROW(materialise(store(), query, -1), InvalidArgumentError);
}

TEST_F(MaterialiseTest, RefusesCountAboveCap) {
    EXPECT_THROW(materialise(store(), ObjectQuery().layer(nuclei), 4), InvalidArgumentError);
}

TEST_F(MaterialiseTest, AtOutOfRangeThrows) {
    ObjectArrayPtr array = materialise(store(), ObjectQuery().layer(nuclei), 5);
    EXPECT_THROW(array->at(-1), InvalidArgumentError);
    EXPECT_THROW(array->at(5), InvalidArgumentError);
}

TEST(ExportResult, ReportsItsValues) {
    const ExportResult result(7, 1234, true);
    EXPECT_EQ(result.objectsWritten(), 7);
    EXPECT_EQ(result.bytesWritten(), 1234);
    EXPECT_TRUE(result.wasCancelled());
    EXPECT_FALSE(ExportResult(0, 0, false).wasCancelled());
}
