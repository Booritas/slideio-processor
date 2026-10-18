// Requirements: UR-SEG-5, UR-SEG-6
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/measurement.hpp>

#include <gtest/gtest.h>

#include <utility>

using namespace slideio::processor;

TEST(MeasurementSet, InsertionOrderAndReplaceInPlace) {
    MeasurementSet s; s.set("area", 10, MeasurementUnit::SquareMicrometres);
    s.set("mean", 3, MeasurementUnit::Intensity); s.set("area", 12, MeasurementUnit::SquarePixels);
    EXPECT_EQ(s.count(), 2); EXPECT_STREQ(s.at(0).name(), "area"); EXPECT_EQ(s.at(0).value(), 12);
    EXPECT_FALSE(s.at(0).isCalibrated()); EXPECT_TRUE(s.at(1).isCalibrated());
    Measurement m; EXPECT_TRUE(s.find("mean", m)); EXPECT_EQ(m.unit(), MeasurementUnit::Intensity);
    EXPECT_FALSE(s.find("nope", m)); s.remove("area"); s.remove("nope"); EXPECT_EQ(s.count(), 1);
    EXPECT_THROW(s.set("", 1, MeasurementUnit::Pixels), InvalidArgumentError);
    EXPECT_THROW(s.at(5), InvalidArgumentError);
    MeasurementSet c(s); s.clear(); EXPECT_EQ(c.count(), 1); EXPECT_EQ(s.count(), 0);
}

TEST(MeasurementSet, NullNameAndDefaults) {
    MeasurementSet s;
    EXPECT_THROW(s.set(nullptr, 1, MeasurementUnit::Pixels), InvalidArgumentError);
    EXPECT_THROW(s.at(-1), InvalidArgumentError);
    Measurement m;
    EXPECT_FALSE(s.find(nullptr, m));
    s.remove(nullptr);
    EXPECT_STREQ(m.name(), ""); EXPECT_EQ(m.value(), 0); EXPECT_EQ(m.unit(), MeasurementUnit::Dimensionless);
    EXPECT_TRUE(m.isCalibrated());
}

TEST(MeasurementSet, CopyAndMoveMembers) {
    MeasurementSet a; a.set("x", 1, MeasurementUnit::Pixels); a.set("y", 2, MeasurementUnit::Micrometres);
    MeasurementSet b; b = a; a.clear();
    EXPECT_EQ(b.count(), 2);
    MeasurementSet c(std::move(b)); EXPECT_EQ(c.count(), 2); EXPECT_EQ(b.count(), 0);
    MeasurementSet d; d.set("z", 3, MeasurementUnit::Intensity); d = std::move(c);
    EXPECT_EQ(d.count(), 2); EXPECT_EQ(c.count(), 0); EXPECT_STREQ(d.at(1).name(), "y");
    b.set("again", 1, MeasurementUnit::Pixels); EXPECT_EQ(b.count(), 1);
}

TEST(MeasurementSet, FindMissLeavesOutUnchanged) {
    MeasurementSet s; s.set("a", 1, MeasurementUnit::Intensity);
    Measurement m; ASSERT_TRUE(s.find("a", m));
    EXPECT_FALSE(s.find("zzz", m));
    EXPECT_STREQ(m.name(), "a"); EXPECT_EQ(m.value(), 1);
}

TEST(MeasurementSet, SelfAssignmentIsHarmless) {
    MeasurementSet s; s.set("a", 1, MeasurementUnit::Pixels);
    MeasurementSet& r = s;
    s = r; EXPECT_EQ(s.count(), 1);
    s = std::move(r); EXPECT_EQ(s.count(), 1); EXPECT_STREQ(s.at(0).name(), "a");
}

TEST(MeasurementSet, RemoveThenSetAppends) {
    MeasurementSet s; s.set("a", 1, MeasurementUnit::Pixels); s.set("b", 2, MeasurementUnit::Pixels);
    s.remove("a"); s.set("a", 3, MeasurementUnit::Pixels);
    EXPECT_EQ(s.count(), 2); EXPECT_STREQ(s.at(0).name(), "b"); EXPECT_STREQ(s.at(1).name(), "a");
}
