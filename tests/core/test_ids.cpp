// Requirements: UR-RES-4, UR-QA-3
#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/core/ids.hpp>

#include <gtest/gtest.h>

#include <cstdint>

using namespace slideio::processor;

TEST(Ids, SmallIdValidity) {
    EXPECT_FALSE(LayerId().isValid()); EXPECT_FALSE(LayerId(0).isValid());
    EXPECT_TRUE(LayerId(1).isValid()); EXPECT_TRUE(StageId(2) < StageId(3));
    EXPECT_NE(LabelId(1), LabelId(2)); EXPECT_EQ(RasterId(4).value(), 4);
}
TEST(ObjectId, PacksAndOrders) {
    ObjectId a = ObjectId::fromAnchor(LayerId(2), {120, 45});
    EXPECT_EQ(a.anchorPacked(), (int64_t(45) << 31) | 120);
    EXPECT_EQ(a.anchorPoint(), (Point64{120, 45})); EXPECT_EQ(a.layer(), LayerId(2));
    EXPECT_TRUE(ObjectId::fromAnchor(LayerId(1), {9, 99}) < a);   // layer first
    EXPECT_TRUE(ObjectId::fromAnchor(LayerId(2), {500, 44}) < a); // then scan position
}
TEST(ObjectId, OutOfRangeAnchorsAreInvalidNotWrapped) {            // out-of-range anchors must not wrap into another object's id
    EXPECT_FALSE(ObjectId::fromAnchor(LayerId(1), {-1, 0}).isValid());
    EXPECT_FALSE(ObjectId::fromAnchor(LayerId(1), {0, -1}).isValid());
    EXPECT_FALSE(ObjectId::fromAnchor(LayerId(1), {int64_t(1) << 31, 0}).isValid());
    EXPECT_FALSE(ObjectId::fromAnchor(LayerId(1), {0, int64_t(1) << 32}).isValid());
    EXPECT_TRUE(ObjectId::fromAnchor(LayerId(1), {(int64_t(1) << 31) - 1, (int64_t(1) << 32) - 1}).isValid());
    EXPECT_FALSE(ObjectId::fromAnchor(LayerId(), {1, 1}).isValid());
    EXPECT_EQ(ObjectId().anchorPacked(), -1);
}
TEST(ObjectId, StringRoundTrip) {
    ObjectId a = ObjectId::fromAnchor(LayerId(3), {120, 45}), b;
    char buf[32]; EXPECT_EQ(a.toString(buf, 32), 8); EXPECT_STREQ(buf, "3:120:45");
    char small[4]; EXPECT_EQ(a.toString(small, 4), 8); EXPECT_STREQ(small, "3:1");
    EXPECT_EQ(a.toString(nullptr, 0), 8);
    EXPECT_TRUE(ObjectId::fromString("3:120:45", b)); EXPECT_EQ(a, b);
    for (const char* bad : {"", "3:120", "3:120:45x", "-3:1:1", "3: 1:1", "0:1:1", "1:2147483648:0",
                             "3:001:2", "03:1:2", "3:1:", "3:1:2:", "2147483648:1:1",
                             "1:1:4294967296", "1:99999999999999999999:1", "1:1:99999999999999999999", ":1:1"})
        EXPECT_FALSE(ObjectId::fromString(bad, b)) << bad;
    EXPECT_EQ(a, b);                                                // unchanged on failure
}
TEST(ObjectId, StringEdgeCases) {
    ObjectId b = ObjectId::fromAnchor(LayerId(7), {1, 1});
    EXPECT_FALSE(ObjectId::fromString(nullptr, b));
    EXPECT_EQ(b, ObjectId::fromAnchor(LayerId(7), {1, 1}));
    EXPECT_TRUE(ObjectId::fromString("1:0:0", b));
    EXPECT_EQ(b.anchorPoint(), (Point64{0, 0}));
    EXPECT_TRUE(ObjectId::fromString("2147483647:2147483647:4294967295", b));
    EXPECT_EQ(b.layer(), LayerId(2147483647));
    EXPECT_EQ(b.anchorPoint(), (Point64{2147483647LL, 4294967295LL}));
    char buf[40];
    EXPECT_EQ(b.toString(buf, 40), 32); EXPECT_STREQ(buf, "2147483647:2147483647:4294967295");
    ObjectId inv;
    EXPECT_EQ(inv.toString(buf, 40), 7); EXPECT_STREQ(buf, "invalid");
    EXPECT_EQ(inv.toString(nullptr, 0), 7);
    ObjectId a = ObjectId::fromAnchor(LayerId(3), {120, 45});
    char one[1] = {'x'}; EXPECT_EQ(a.toString(one, 1), 8); EXPECT_EQ(one[0], 0);
    char untouched[4] = {'x', 'y', 'z', 'w'};
    EXPECT_EQ(a.toString(untouched, 0), 8); EXPECT_EQ(untouched[0], 'x');
}
TEST(Ids, ComparisonOperators) {
    EXPECT_TRUE(LayerId(1) == LayerId(1)); EXPECT_TRUE(LayerId(1) != LayerId(2));
    EXPECT_FALSE(LayerId(2) < LayerId(2));
    EXPECT_TRUE(RasterId(1) < RasterId(2)); EXPECT_TRUE(LabelId(3) == LabelId(3));
    EXPECT_TRUE(ObjectId() == ObjectId());
    EXPECT_TRUE(ObjectId::fromAnchor(LayerId(1), {1, 1}) != ObjectId());
}
