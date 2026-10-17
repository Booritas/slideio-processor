// Requirements: UR-RES-2, UR-RES-3, UR-RES-4, UR-RES-5, UR-STG-3, UR-STG-5
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/core/object_query.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <utility>

using namespace slideio::processor;

TEST(ObjectQuery, DefaultsAreUnfilteredCompletedOnly) {
    ObjectQuery q; EXPECT_TRUE(q.isUnfiltered()); EXPECT_FALSE(q.hasSpatialFilter());
    EXPECT_EQ(q.visibilityValue(), StageVisibility::CompletedOnly); EXPECT_FALSE(q.hasLimit());
    q.limit(10).visibility(StageVisibility::IncludeInFlight); EXPECT_TRUE(q.isUnfiltered());
}
TEST(ObjectQuery, FluentSettersReadBack) {
    Point64 v[] = {{0,0},{4,0},{4,4}}; Polygon p; p.addRing(v, 3, false);
    ObjectQuery q; q.layer(LayerId(1)).layer(LayerId(2)).intersecting(Rect64{0,0,8,8})
        .withinPolygon(p, SpatialPredicate::ContainedBy).label(LabelId(5))
        .measurementAtLeast("area", 10).measurementAtMost("area", 50);
    EXPECT_EQ(q.layerValue(), LayerId(2)); EXPECT_TRUE(q.hasSpatialFilter());
    EXPECT_EQ(q.withinPolygonPredicate(), SpatialPredicate::ContainedBy);
    EXPECT_EQ(q.withinPolygonValue().ringPointCount(0), 3); EXPECT_EQ(q.measurementBoundCount(), 2);
    EXPECT_TRUE(q.measurementBoundAt(0).isLowerBound); EXPECT_EQ(q.measurementBoundAt(1).value, 50);
    EXPECT_STREQ(q.measurementBoundAt(1).name, "area"); EXPECT_FALSE(q.isUnfiltered());
    EXPECT_THROW(q.measurementBoundAt(2), InvalidArgumentError);
    ObjectQuery c(q); q.layer(LayerId(9)); EXPECT_EQ(c.layerValue(), LayerId(2));
}
TEST(ObjectQuery, AnyFiltersDoNotCount) {
    ObjectQuery q; q.labelFilter(LabelFilter::Any).attachment(AttachmentFilter::Any);
    EXPECT_TRUE(q.isUnfiltered()); q.attachment(AttachmentFilter::UnattachedOnly);
    EXPECT_FALSE(q.isUnfiltered());
}
TEST(ObjectQuery, NullNameStoredEmptyAndNegativeIndexThrows) {
    ObjectQuery q; q.measurementAtLeast(nullptr, 1);
    EXPECT_STREQ(q.measurementBoundAt(0).name, ""); EXPECT_THROW(q.measurementBoundAt(-1), InvalidArgumentError);
}
TEST(ObjectQuery, AssignmentAndMove) {
    ObjectQuery a; a.stage(StageId(3)).limit(7).measurementAtMost("m", 2);
    ObjectQuery b; b = a; a.limit(1); EXPECT_EQ(b.limitValue(), 7); EXPECT_EQ(b.stageValue(), StageId(3));
    ObjectQuery m(std::move(b)); EXPECT_EQ(m.measurementBoundCount(), 1);
    EXPECT_TRUE(b.isUnfiltered()); EXPECT_FALSE(b.hasLimit()); EXPECT_EQ(b.measurementBoundCount(), 0);
    b.layer(LayerId(4)); EXPECT_EQ(b.layerValue(), LayerId(4));
    ObjectQuery n; n = std::move(m); EXPECT_EQ(n.limitValue(), 7);
}
TEST(ObjectQuery, FlowInterfacesAreImplementable) {
    struct Src : ObjectSource {
        CursorPtr query(const ObjectQuery&) override { return nullptr; }
        int64_t count(const ObjectQuery& q) const override { return q.hasLimit() ? q.limitValue() : 0; }
        bool get(ObjectId, ObjectRecord&) const override { return false; }
    } s;
    ObjectQuery q; q.limit(3); EXPECT_EQ(s.count(q), 3);
    EXPECT_EQ(static_cast<int32_t>(LayerRasterContent::ObjectAnchor), 2);
}
