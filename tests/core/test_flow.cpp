// Requirements: UR-RES-2, UR-EXT-4
#include <slideio/processor/core/flow.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

using namespace slideio::processor;

namespace {

// A sink overriding only what ObjectSink requires.
class RecordingSink : public ObjectSink {
public:
    void emit(const ObjectRecord& record) override { records.push_back(record); }
    void flush() override {}
    int64_t emittedCount() const override { return static_cast<int64_t>(records.size()); }

    std::vector<ObjectRecord> records;
};

ObjectRecord sampleRecord() {
    const Point64 v[] = {{5, 2}, {9, 2}, {9, 8}, {5, 8}};
    Polygon polygon;
    polygon.addRing(v, 4, false);
    ObjectRecord r;
    r.setGeometry(polygon);
    r.setLayer(LayerId(3));
    r.measurements().set("area", 24.0, MeasurementUnit::SquarePixels);
    return r;
}

std::vector<Point64> outerRing(const ObjectRecord& r) {
    std::vector<Point64> points(static_cast<size_t>(r.geometry().ringPointCount(0)));
    r.geometry().ringPoints(0, points.data(), static_cast<int64_t>(points.size()));
    return points;
}

} // namespace

TEST(ObjectSinkDefaults, OwnedAndKeptReachEmit) {
    const ObjectRecord r = sampleRecord();
    RecordingSink sink;
    ObjectSink& base = sink;
    EXPECT_TRUE(base.emitKept(r));
    EXPECT_TRUE(base.emitOwned(ObjectRecord(r)));
    ASSERT_EQ(sink.records.size(), 2u);
    for (const ObjectRecord& seen : sink.records) {
        EXPECT_EQ(seen.id(), r.id());
        EXPECT_EQ(seen.layer(), r.layer());
        EXPECT_EQ(outerRing(seen), outerRing(r));
        Measurement area;
        ASSERT_TRUE(seen.measurements().find("area", area));
        EXPECT_EQ(area.value(), 24.0);
        EXPECT_EQ(seen.measurements().count(), 1);
    }
}
