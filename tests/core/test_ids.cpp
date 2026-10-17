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
TEST(ObjectId, OutOfRangeAnchorsAreInvalidNotWrapped) {            // Review Focus 5
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
    for (const char* bad : {"", "3:120", "3:120:45x", "-3:1:1", "3: 1:1", "0:1:1", "1:2147483648:0"})
        EXPECT_FALSE(ObjectId::fromString(bad, b)) << bad;
    EXPECT_EQ(a, b);                                                // unchanged on failure
}
