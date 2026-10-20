// Requirements: UR-RES-1, UR-RES-2, UR-RES-3, UR-EXE-7, UR-STG-5, UR-STG-6, UR-OBS-2, UR-OBS-4, UR-QA-3
#include "storage/detail/polygon_predicates.hpp"
#include "storage/store_fixture.hpp"

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/core/object_query.hpp>
#include <slideio/processor/core/object_record.hpp>
#include <slideio/processor/storage/object_store.hpp>
#include <slideio/processor/storage/store.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace slideio::processor;
namespace detail = slideio::processor::detail;

namespace {

Polygon polygonOf(std::vector<Point64> points) {
    Polygon polygon;
    polygon.addRing(points.data(), static_cast<int64_t>(points.size()), false);
    return polygon;
}

Polygon square(int64_t x, int64_t y, int64_t side) {
    return polygonOf({{x, y}, {x + side, y}, {x + side, y + side}, {x, y + side}});
}

ObjectRecord rec(LayerId layer, int64_t x, int64_t y, int64_t side) {
    ObjectRecord record;
    record.setLayer(layer);
    record.setGeometry(square(x, y, side));
    return record;
}

// Every record a cursor yields, checking position() as it goes.
std::vector<ObjectRecord> drain(const CursorPtr& cursor) {
    std::vector<ObjectRecord> records;
    while (cursor->next()) {
        EXPECT_EQ(cursor->position(), static_cast<int64_t>(records.size()));
        records.push_back(cursor->current());
    }
    return records;
}

std::set<ObjectId> idsOf(const std::vector<ObjectRecord>& records) {
    std::set<ObjectId> ids;
    for (const ObjectRecord& r : records) ids.insert(r.id());
    return ids;
}

class ObjectQueryTest : public StoreFixture {
protected:
    void SetUp() override {
        StoreFixture::SetUp();
        cells = store().layers().declareSemantic("cells", store().layers().pixelLayer());
        nuclei = store().layers().declareSemantic("nuclei", cells);
        a = store().stageStateWriter().declare("a", "");
        b = store().stageStateWriter().declare("b", "");
    }
    ObjectStore& objects() { return store().objects(); }
    void setStatus(StageId stage, StageStatus status) { store().stageStateWriter().setStatus(stage, status); }
    // Writes the records through one writer for `stage`, flushed.
    void write(StageId stage, const std::vector<ObjectRecord>& records) {
        ObjectWriterPtr w = objects().writerForStage(stage);
        for (const ObjectRecord& r : records) w->emit(r);
        w->flush();
    }
    // count(q) must equal what query(q) yields; returns that number.
    int64_t agreedCount(const ObjectQuery& q) {
        const int64_t yielded = static_cast<int64_t>(drain(objects().query(q)).size());
        EXPECT_EQ(objects().count(q), yielded);
        return yielded;
    }

    LayerId cells;
    LayerId nuclei;
    StageId a;
    StageId b;
};

const ObjectQuery inFlight() { return ObjectQuery().visibility(StageVisibility::IncludeInFlight); }

// ---- Brute-force references for a convex polygon against axis-aligned squares -------------

int64_t cross(Point64 o, Point64 p, Point64 q) { return (p.x - o.x) * (q.y - o.y) - (p.y - o.y) * (q.x - o.x); }

std::vector<Point64> corners(int64_t x, int64_t y, int64_t side) {
    return {{x, y}, {x + side, y}, {x + side, y + side}, {x, y + side}};
}

// Closed convex sets meet unless an edge normal of either strictly separates them.
bool convexMeet(const std::vector<Point64>& p, const std::vector<Point64>& q) {
    for (const std::vector<Point64>* shape : {&p, &q}) {
        for (size_t i = 0; i < shape->size(); ++i) {
            const Point64 e0 = (*shape)[i];
            const Point64 e1 = (*shape)[(i + 1) % shape->size()];
            const int64_t nx = -(e1.y - e0.y), ny = e1.x - e0.x;
            auto range = [&](const std::vector<Point64>& s) {
                int64_t lo = std::numeric_limits<int64_t>::max(), hi = std::numeric_limits<int64_t>::min();
                for (const Point64& v : s) {
                    lo = std::min(lo, nx * v.x + ny * v.y);
                    hi = std::max(hi, nx * v.x + ny * v.y);
                }
                return std::make_pair(lo, hi);
            };
            const auto rp = range(p), rq = range(q);
            if (rp.second < rq.first || rq.second < rp.first) return false;
        }
    }
    return true;
}

// Every corner of the square inside or on the convex polygon.
bool convexContains(const std::vector<Point64>& convex, const std::vector<Point64>& points) {
    const int64_t turn = cross(convex[0], convex[1], convex[2]) > 0 ? 1 : -1;
    for (const Point64& v : points) {
        for (size_t i = 0; i < convex.size(); ++i) {
            if (turn * cross(convex[i], convex[(i + 1) % convex.size()], v) < 0) return false;
        }
    }
    return true;
}

} // namespace

TEST_F(ObjectQueryTest, VisibilityFollowsStageStatus) {  // UR-STG-5, UR-OBS-4
    setStatus(a, StageStatus::Running);
    write(a, {rec(cells, 0, 0, 4), rec(cells, 10, 0, 4), rec(cells, 20, 0, 4), rec(cells, 30, 0, 4)});
    const ObjectId first = ObjectId::fromAnchor(cells, Point64{0, 0});
    ObjectRecord out;
    EXPECT_EQ(agreedCount(ObjectQuery()), 0);
    EXPECT_FALSE(objects().get(first, out));
    EXPECT_EQ(agreedCount(inFlight()), 4);
    EXPECT_EQ(objects().totalObjectCount(), 4);

    setStatus(a, StageStatus::Complete);
    EXPECT_EQ(agreedCount(ObjectQuery()), 4);
    EXPECT_TRUE(objects().get(first, out));
    EXPECT_EQ(out.stage(), a);

    store().stageStateWriter().setStale(a, true);
    EXPECT_EQ(agreedCount(ObjectQuery()), 4);
    EXPECT_TRUE(objects().get(first, out));

    setStatus(a, StageStatus::Failed);
    EXPECT_EQ(agreedCount(ObjectQuery()), 0);
    EXPECT_EQ(agreedCount(inFlight()), 4);
}

TEST_F(ObjectQueryTest, ResultsAreInObjectIdOrderWhateverTheArrival) {  // UR-QA-3
    std::vector<ObjectRecord> records;
    for (LayerId layer : {cells, nuclei}) {
        for (int i = 0; i < 250; ++i) records.push_back(rec(layer, (i % 25) * 10, (i / 25) * 10, 4));
    }
    std::mt19937 random(20261020);
    std::shuffle(records.begin(), records.end(), random);
    {
        ObjectWriterPtr w = objects().writerForStage(a);
        w->setBatchSizeObjects(37);  // several batches, each in arrival order
        for (const ObjectRecord& r : records) w->emit(r);
    }
    setStatus(a, StageStatus::Complete);

    CursorPtr cursor = objects().query(ObjectQuery());
    const std::vector<ObjectRecord> yielded = drain(cursor);
    ASSERT_EQ(yielded.size(), 500u);
    EXPECT_EQ(cursor->position(), 499);
    for (size_t i = 1; i < yielded.size(); ++i) {
        EXPECT_TRUE(yielded[i - 1].id() < yielded[i].id()) << i;
    }
    EXPECT_EQ(idsOf(yielded), idsOf(records));
    EXPECT_FALSE(cursor->next());  // exhausted stays exhausted
}

TEST_F(ObjectQueryTest, FiltersComposeAndCountAgrees) {
    // cells by a: 6 squares, the even ones labelled 2, area = 10 * i.
    std::vector<ObjectRecord> fromA;
    for (int i = 0; i < 6; ++i) {
        ObjectRecord r = rec(cells, i * 10, 0, 4);
        if (i % 2 == 0) r.setLabel(LabelId(2));
        r.measurements().set("area", 10.0 * i, MeasurementUnit::SquarePixels);
        fromA.push_back(r);
    }
    write(a, fromA);
    // nuclei by b: 4 squares, labelled 3, no measurements.
    std::vector<ObjectRecord> fromB;
    for (int i = 0; i < 4; ++i) {
        ObjectRecord r = rec(nuclei, i * 10, 0, 2);
        r.setLabel(LabelId(3));
        fromB.push_back(r);
    }
    write(b, fromB);
    setStatus(a, StageStatus::Complete);
    setStatus(b, StageStatus::Complete);

    EXPECT_EQ(agreedCount(ObjectQuery()), 10);
    EXPECT_EQ(agreedCount(ObjectQuery().layer(cells)), 6);
    EXPECT_EQ(agreedCount(ObjectQuery().layer(nuclei)), 4);
    EXPECT_EQ(agreedCount(ObjectQuery().layer(LayerId(99))), 0);
    EXPECT_EQ(agreedCount(ObjectQuery().stage(b)), 4);
    EXPECT_EQ(agreedCount(ObjectQuery().stage(a).layer(nuclei)), 0);
    EXPECT_EQ(agreedCount(ObjectQuery().label(LabelId(2))), 3);
    EXPECT_EQ(agreedCount(ObjectQuery().label(LabelId(3)).layer(nuclei)), 4);
    EXPECT_EQ(agreedCount(ObjectQuery().labelFilter(LabelFilter::LabelledOnly)), 7);
    EXPECT_EQ(agreedCount(ObjectQuery().labelFilter(LabelFilter::UnlabelledOnly)), 3);
    EXPECT_EQ(agreedCount(ObjectQuery().labelFilter(LabelFilter::UnlabelledOnly).label(LabelId(2))), 0);
    EXPECT_EQ(agreedCount(ObjectQuery().measurementAtLeast("area", 20.0)), 4);                  // 20..50
    EXPECT_EQ(agreedCount(ObjectQuery().measurementAtMost("area", 20.0)), 3);                   // 0..20
    EXPECT_EQ(agreedCount(ObjectQuery().measurementAtLeast("area", 15).measurementAtMost("area", 35)), 2);
    EXPECT_EQ(agreedCount(ObjectQuery().measurementAtLeast("perimeter", 0)), 0);  // nobody has it
    EXPECT_EQ(agreedCount(ObjectQuery().attachment(AttachmentFilter::UnattachedOnly)), 10);
    EXPECT_EQ(agreedCount(ObjectQuery().attachment(AttachmentFilter::AttachedOnly)), 0);
    EXPECT_EQ(agreedCount(ObjectQuery().attachment(AttachmentFilter::Any)), 10);
    EXPECT_EQ(agreedCount(ObjectQuery().limit(3)), 3);
    EXPECT_EQ(agreedCount(ObjectQuery().limit(0)), 0);
    EXPECT_EQ(agreedCount(ObjectQuery().limit(100)), 10);
    EXPECT_EQ(agreedCount(ObjectQuery().layer(cells).label(LabelId(2)).limit(2)), 2);
    EXPECT_EQ(agreedCount(ObjectQuery().intersecting(Rect64{0, 0, 25, 4})), 6);  // 3 cells, 3 nuclei
    const Polygon wide = polygonOf({{0, 0}, {100, 0}, {100, 10}, {0, 10}});
    EXPECT_EQ(agreedCount(ObjectQuery().withinPolygon(wide, SpatialPredicate::ContainedBy).limit(4)), 4);
    EXPECT_EQ(agreedCount(ObjectQuery().withinPolygon(wide, SpatialPredicate::ContainedBy).layer(nuclei)), 4);

    // The limited cursor yields the first objects in id order.
    const std::vector<ObjectRecord> firstThree = drain(objects().query(ObjectQuery().limit(3)));
    ASSERT_EQ(firstThree.size(), 3u);
    EXPECT_EQ(firstThree[0].id(), ObjectId::fromAnchor(cells, Point64{0, 0}));
    EXPECT_EQ(firstThree[2].id(), ObjectId::fromAnchor(cells, Point64{20, 0}));

    EXPECT_THROW(objects().query(ObjectQuery().limit(-1)), InvalidArgumentError);
    EXPECT_THROW(objects().count(ObjectQuery().limit(-1)), InvalidArgumentError);
    EXPECT_THROW(objects().query(ObjectQuery().labelFilter(static_cast<LabelFilter>(7))), InvalidArgumentError);
    EXPECT_THROW(objects().count(ObjectQuery().visibility(static_cast<StageVisibility>(-1))), InvalidArgumentError);
}

TEST_F(ObjectQueryTest, RectAndPolygonPredicates) {
    // A 10 x 10 grid of 8-pixel squares at a pitch of 10.
    std::vector<ObjectRecord> grid;
    for (int j = 0; j < 10; ++j) {
        for (int i = 0; i < 10; ++i) grid.push_back(rec(cells, i * 10, j * 10, 8));
    }
    write(a, grid);
    setStatus(a, StageStatus::Complete);

    for (const Rect64& rect : {Rect64{15, 15, 20, 20}, Rect64{8, 0, 2, 100}, Rect64{0, 0, 1, 1}, Rect64{-50, -50, 51, 51},
                               Rect64{17, 0, 1, 100}, Rect64{30, 30, 0, 10}}) {
        std::set<ObjectId> expected;
        for (const ObjectRecord& r : grid) {
            if (r.boundingBox().intersects(rect)) expected.insert(r.id());
        }
        const ObjectQuery q = ObjectQuery().intersecting(rect);
        EXPECT_EQ(idsOf(drain(objects().query(q))), expected) << rect.x << "," << rect.y;
        EXPECT_EQ(objects().count(q), static_cast<int64_t>(expected.size()));
    }

    // A triangle whose edges straddle squares, and one vertex exactly on a square corner.
    const std::vector<Point64> triangle = {{5, 5}, {95, 18}, {28, 90}};
    std::set<ObjectId> meets, inside;
    for (int j = 0; j < 10; ++j) {
        for (int i = 0; i < 10; ++i) {
            const std::vector<Point64> c = corners(i * 10, j * 10, 8);
            const ObjectId id = ObjectId::fromAnchor(cells, Point64{i * 10, j * 10});
            if (convexMeet(triangle, c)) meets.insert(id);
            if (convexContains(triangle, c)) inside.insert(id);
        }
    }
    ASSERT_FALSE(inside.empty());
    ASSERT_LT(inside.size(), meets.size());  // the boundary-straddling squares differ
    const Polygon tri = polygonOf(triangle);
    const ObjectQuery intersects = ObjectQuery().withinPolygon(tri, SpatialPredicate::Intersects);
    const ObjectQuery contained = ObjectQuery().withinPolygon(tri, SpatialPredicate::ContainedBy);
    EXPECT_EQ(idsOf(drain(objects().query(intersects))), meets);
    EXPECT_EQ(idsOf(drain(objects().query(contained))), inside);
    EXPECT_EQ(objects().count(intersects), static_cast<int64_t>(meets.size()));
    EXPECT_EQ(objects().count(contained), static_cast<int64_t>(inside.size()));

    // Touching counts for Intersects: a triangle meeting square (0,0) only at its corner (8,8).
    const Polygon touching = polygonOf({{8, 8}, {9, 8}, {9, 9}});
    EXPECT_EQ(idsOf(drain(objects().query(ObjectQuery().withinPolygon(touching, SpatialPredicate::Intersects)))),
              (std::set<ObjectId>{ObjectId::fromAnchor(cells, Point64{0, 0})}));
    // A rectangle and a polygon compose.
    EXPECT_EQ(agreedCount(ObjectQuery().intersecting(Rect64{0, 0, 50, 50}).withinPolygon(tri, SpatialPredicate::ContainedBy)),
              static_cast<int64_t>(std::count_if(inside.begin(), inside.end(), [](const ObjectId& id) {
                  return id.anchorPoint().x < 50 && id.anchorPoint().y < 50;
              })));
}

TEST_F(ObjectQueryTest, NoFalseNegativesBeyondFloatPrecision) {  // Review Focus 4
    const int64_t y = (int64_t(1) << 32) - 8;
    std::vector<ObjectRecord> row;
    for (int64_t x = (int64_t(1) << 31) - 8; x <= (int64_t(1) << 31) - 2; ++x) row.push_back(rec(cells, x, y, 1));
    write(a, row);
    setStatus(a, StageStatus::Complete);
    for (const ObjectRecord& r : row) {
        const int64_t x = r.boundingBox().x;
        const std::vector<ObjectRecord> hit = drain(objects().query(ObjectQuery().intersecting(Rect64{x, y, 1, 1})));
        ASSERT_EQ(hit.size(), 1u) << x;
        EXPECT_EQ(hit[0].id(), r.id());
        const std::vector<ObjectRecord> inside =
            drain(objects().query(ObjectQuery().withinPolygon(square(x, y, 1), SpatialPredicate::ContainedBy)));
        ASSERT_EQ(inside.size(), 1u) << x;
        EXPECT_EQ(inside[0].id(), r.id());
        // Its neighbours share an edge, so Intersects also admits them.
        const int64_t touching = (x == row.front().boundingBox().x || x == row.back().boundingBox().x) ? 2 : 3;
        EXPECT_EQ(agreedCount(ObjectQuery().withinPolygon(square(x, y, 1), SpatialPredicate::Intersects)), touching);
    }
}

TEST_F(ObjectQueryTest, CursorKeepsItsSnapshot) {  // Review Focus 2
    std::vector<ObjectRecord> first, second;
    for (int i = 0; i < 1000; ++i) {
        first.push_back(rec(cells, (i % 100) * 10, (i / 100) * 10, 4));
        second.push_back(rec(cells, (i % 100) * 10, 1000 + (i / 100) * 10, 4));
    }
    write(a, first);
    CursorPtr cursor = objects().query(inFlight());
    std::set<ObjectId> seen;
    for (int i = 0; i < 10; ++i) {
        ASSERT_TRUE(cursor->next());
        seen.insert(cursor->current().id());
    }
    std::thread other([&] { write(a, second); });
    other.join();
    EXPECT_EQ(objects().totalObjectCount(), 2000);
    int64_t yielded = 10;
    while (cursor->next()) {
        EXPECT_TRUE(seen.insert(cursor->current().id()).second) << "duplicate";
        ++yielded;
    }
    EXPECT_EQ(yielded, 1000);
    EXPECT_EQ(seen, idsOf(first));
    EXPECT_EQ(agreedCount(inFlight()), 2000);

    cursor->close();
    EXPECT_THROW(cursor->next(), IoError);
}

TEST_F(ObjectQueryTest, ClosedStoreRefusesReads) {
    write(a, {rec(cells, 0, 0, 4)});
    store().close();
    ObjectRecord out;
    EXPECT_THROW(objects().query(ObjectQuery()), IoError);
    EXPECT_THROW(objects().count(ObjectQuery()), IoError);
    EXPECT_THROW(objects().get(ObjectId::fromAnchor(cells, Point64{0, 0}), out), IoError);
    EXPECT_THROW(objects().totalObjectCount(), IoError);
    EXPECT_THROW(objects().writerForStage(a), IoError);
}

// ---- The exact predicates themselves --------------------------------------------------------

TEST(PolygonPredicates, OrientationIsExactAtExtremeCoordinates) {
    constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
    constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
    // The line y = x across the whole int64 range; differences reach 2^64 - 1.
    const Point64 lo{kMin, kMin}, hi{kMax, kMax};
    EXPECT_EQ(detail::orientation(lo, hi, Point64{0, 0}), 0);
    EXPECT_EQ(detail::orientation(lo, hi, Point64{kMax - 1, kMax}), 1);
    EXPECT_EQ(detail::orientation(lo, hi, Point64{kMax, kMax - 1}), -1);
    EXPECT_EQ(detail::orientation(lo, hi, Point64{kMin + 1, kMin}), -1);
    EXPECT_EQ(detail::orientation(hi, lo, Point64{kMin + 1, kMin}), 1);
    // A near-collinear point no 64-bit-mantissa product can tell apart.
    const Point64 p{kMin, kMin + 1}, q{kMax, kMax};
    EXPECT_EQ(detail::orientation(p, q, Point64{kMax - 1, kMax - 1}), -1);
    EXPECT_EQ(detail::orientation(Point64{0, 0}, Point64{kMax, kMax - 1}, Point64{kMax - 1, kMax - 2}), -1);
    EXPECT_EQ(detail::orientation(Point64{0, 0}, Point64{kMax, kMax - 1}, Point64{kMax - 1, kMax - 1}), 1);
    EXPECT_EQ(detail::orientation(Point64{0, 0}, Point64{kMax - 1, kMax - 3}, Point64{kMax, kMax - 2}), 1);
}

TEST(PolygonPredicates, ExactNearTheInt64Limit) {
    constexpr int64_t m = std::numeric_limits<int64_t>::max();
    // Below the diagonal y = x, and one unit above it: disjoint.
    const Polygon below = polygonOf({{m - 4, m - 4}, {m, m - 4}, {m, m}});
    const Polygon above = polygonOf({{m - 4, m - 3}, {m - 1, m}, {m - 4, m}});
    EXPECT_FALSE(detail::polygonsIntersect(below, above));
    EXPECT_FALSE(detail::polygonsIntersect(above, below));
    // On the diagonal: touching.
    const Polygon onDiagonal = polygonOf({{m - 4, m - 4}, {m - 1, m - 1}, {m - 4, m}});
    EXPECT_TRUE(detail::polygonsIntersect(below, onDiagonal));
    EXPECT_FALSE(detail::polygonContainedBy(onDiagonal, below));
    // Containment, boundary included.
    const Polygon small = polygonOf({{m - 3, m - 4}, {m, m - 4}, {m, m - 1}});
    EXPECT_TRUE(detail::polygonContainedBy(small, below));
    EXPECT_TRUE(detail::polygonContainedBy(below, below));
    EXPECT_FALSE(detail::polygonContainedBy(below, small));
    // Far apart across the whole range: no overflow, no false hit.
    constexpr int64_t n = std::numeric_limits<int64_t>::min();
    const Polygon farCorner = polygonOf({{n, n}, {n + 4, n}, {n, n + 4}});
    EXPECT_FALSE(detail::polygonsIntersect(farCorner, below));
}

TEST(PolygonPredicates, HolesAndNesting) {
    // A frame: 0..30 square with a 10..20 hole.
    Polygon frame = square(0, 0, 30);
    const Point64 hole[] = {{10, 10}, {20, 10}, {20, 20}, {10, 20}};
    frame.addRing(hole, 4, true);
    const Polygon inHole = square(12, 12, 4);
    const Polygon onRim = square(2, 2, 4);
    const Polygon overHole = square(5, 5, 20);
    EXPECT_FALSE(detail::polygonsIntersect(frame, inHole));
    EXPECT_FALSE(detail::polygonContainedBy(inHole, frame));
    EXPECT_TRUE(detail::polygonsIntersect(frame, onRim));
    EXPECT_TRUE(detail::polygonContainedBy(onRim, frame));
    // Every vertex of overHole is in the frame and no edges cross, but it covers the hole.
    EXPECT_TRUE(detail::polygonsIntersect(overHole, frame));
    EXPECT_FALSE(detail::polygonContainedBy(overHole, frame));
    // One polygon wholly inside another, boundaries apart.
    EXPECT_TRUE(detail::polygonsIntersect(square(0, 0, 100), square(40, 40, 2)));
    EXPECT_TRUE(detail::polygonsIntersect(square(40, 40, 2), square(0, 0, 100)));
    EXPECT_FALSE(detail::polygonsIntersect(Polygon(), square(0, 0, 1)));
    EXPECT_FALSE(detail::polygonContainedBy(Polygon(), square(0, 0, 1)));
}
