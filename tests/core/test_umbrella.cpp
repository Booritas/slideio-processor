// Requirements: UR-API-2, UR-API-7
#include <slideio/processor/core/core.hpp>

#include <gtest/gtest.h>

using namespace slideio::processor;

TEST(Umbrella, ExposesTheWholeModule) {
    Polygon p;
    ObjectQuery q;
    RasterMask m;
    MeasurementSet s;
    Provenance v;
    EXPECT_STREQ(libraryVersion(), "0.1.0");
    EXPECT_EQ(dataTypeSizeBytes(DataType::Byte), 1);
    (void)&makeMemoryBudget;
    (void)&makeLayerMaskSource;
    (void)sizeof(GeoJsonReader);
}
