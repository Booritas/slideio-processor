// Requirements: UR-RES-4, UR-RES-13, UR-STG-4, UR-STG-6
#include <slideio/processor/core/object_record.hpp>

#include <gtest/gtest.h>

#include <utility>

using namespace slideio::processor;

TEST(ObjectRecord, IdDerivesFromLayerAndAnchor) {
    ObjectRecord r; EXPECT_FALSE(r.id().isValid());
    Point64 v[] = {{5,2},{9,2},{9,8},{5,8}}; Polygon p; p.addRing(v, 4, false);
    r.setGeometry(p); EXPECT_FALSE(r.id().isValid());              // no layer yet
    r.setLayer(LayerId(4));
    EXPECT_EQ(r.id(), ObjectId::fromAnchor(LayerId(4), {5, 2}));
    EXPECT_EQ(r.boundingBox(), (Rect64{5,2,4,6})); EXPECT_FALSE(r.stage().isValid());
}

TEST(ObjectRecord, LabelMeasurementsCopyReset) {
    ObjectRecord r; EXPECT_FALSE(r.hasLabel()); r.setLabel(LabelId(7));
    EXPECT_EQ(r.label(), LabelId(7)); r.measurements().set("a", 1, MeasurementUnit::Pixels);
    ObjectRecord c(r); r.clearLabel(); EXPECT_TRUE(c.hasLabel()); EXPECT_FALSE(r.label().isValid());
    EXPECT_EQ(c.measurements().count(), 1);
    r.reset(); EXPECT_EQ(r.measurements().count(), 0); EXPECT_TRUE(r.geometry().isEmpty());
}

TEST(ObjectRecord, AssignmentAndMove) {
    Point64 v[] = {{5,2},{9,2},{9,8}}; Polygon p; p.addRing(v, 3, false);
    ObjectRecord a; a.setGeometry(p); a.setLayer(LayerId(2)); a.setLabel(LabelId(3));
    a.measurements().set("m", 1, MeasurementUnit::Intensity);
    ObjectRecord b; b = a;
    EXPECT_EQ(b.id(), a.id()); EXPECT_EQ(b.label(), LabelId(3)); EXPECT_EQ(b.measurements().count(), 1);
    ObjectRecord c(std::move(a));
    EXPECT_EQ(c.id(), b.id());
    EXPECT_FALSE(a.id().isValid()); EXPECT_FALSE(a.hasLabel());
    EXPECT_EQ(a.measurements().count(), 0); EXPECT_TRUE(a.geometry().isEmpty());
    ObjectRecord d; d = std::move(c);
    EXPECT_EQ(d.layer(), LayerId(2)); EXPECT_EQ(d.measurements().count(), 1); EXPECT_FALSE(c.layer().isValid());
}

TEST(ObjectRecord, SelfAssignmentIsHarmless) {
    ObjectRecord a; a.setLayer(LayerId(2)); a.setLabel(LabelId(3));
    ObjectRecord& r = a;
    a = r; EXPECT_EQ(a.layer(), LayerId(2));
    a = std::move(r); EXPECT_EQ(a.layer(), LayerId(2)); EXPECT_EQ(a.label(), LabelId(3));
}

TEST(ObjectRecord, MovedFromIsUsable) {
    ObjectRecord a; a.setLayer(LayerId(2));
    ObjectRecord b(std::move(a));
    EXPECT_TRUE(a.geometry().isEmpty()); EXPECT_EQ(a.measurements().count(), 0);
    EXPECT_FALSE(a.boundingBox().width > 0);
    a.measurements().set("x", 1, MeasurementUnit::Pixels); a.setLabel(LabelId(1));
    EXPECT_EQ(a.measurements().count(), 1); EXPECT_TRUE(a.hasLabel());
}

TEST(ObjectRecord, IdInvalidForOutOfRangeAnchor) {
    Point64 v[] = {{-5,2},{9,2},{9,8}}; Polygon p; p.addRing(v, 3, false);
    ObjectRecord r; r.setGeometry(p); r.setLayer(LayerId(4));
    EXPECT_FALSE(r.id().isValid());
}

TEST(ObjectRecord, MoveAssignLeavesTheSourceDefaultNeverSwaps) {
    Point64 v[] = {{5,2},{9,2},{9,8}}; Polygon p; p.addRing(v, 3, false);
    ObjectRecord src; src.setGeometry(p); src.setLayer(LayerId(2)); src.setLabel(LabelId(3));
    ObjectRecord dst; dst.setGeometry(p); dst.setLayer(LayerId(7)); dst.setLabel(LabelId(8));
    dst.measurements().set("m", 1, MeasurementUnit::Intensity);
    dst = std::move(src); // non-default target: a swap would hand its state to src
    EXPECT_EQ(dst.layer(), LayerId(2)); EXPECT_EQ(dst.label(), LabelId(3));
    EXPECT_EQ(dst.measurements().count(), 0);
    // The source is a default record.
    EXPECT_FALSE(src.layer().isValid()); EXPECT_FALSE(src.hasLabel());
    EXPECT_TRUE(src.geometry().isEmpty()); EXPECT_EQ(src.measurements().count(), 0);
    EXPECT_FALSE(src.id().isValid());
    // And usable: a setter re-initialises it.
    src.setLayer(LayerId(4)); EXPECT_EQ(src.layer(), LayerId(4));
}
