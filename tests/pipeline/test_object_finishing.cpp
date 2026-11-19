// Requirements: UR-SEG-3, UR-SEG-5, UR-SEG-7, UR-EXT-4
#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/control.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/core/measurement.hpp>
#include <slideio/processor/core/object_record.hpp>
#include <slideio/processor/pipeline/operation_catalog.hpp>
#include <slideio/processor/pipeline/pipeline.hpp>

#include <pipeline/detail/finishing_sink.hpp>
#include <pipeline/detail/pipeline_impl.hpp>
#include <pipeline/detail/tile_evaluator.hpp>
#include <pipeline/test_support/blob_scene.hpp>
#include <pipeline/test_support/synthetic_scene.hpp>
#include <pipeline/test_support/test_operations.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace slideio::processor;
namespace ts = slideio::processor::testing_support;

namespace {

const LayerId kLayer{5};
const int32_t kTileSides[] = {64, 100, 257};

Configuration config(int32_t tileSide) {
    return ConfigurationBuilder().tileSize(tileSide, tileSide).tileSizeFromSlide(false).workerCount(4).build();
}

OperationParameters componentParameters(bool seams) {
    OperationParameters parameters;
    parameters.setBool("seams", seams);
    parameters.setBool("finishTag", true);
    return parameters;
}

std::vector<ObjectRecord> run(const std::shared_ptr<slideio::Scene>& scene, int32_t tileSide,
                              const PipelineSpec& spec, int64_t* droppedByFilter = nullptr) {
    Pipeline p(scene, config(tileSide), spec);
    p.bindLayer("objects", kLayer);
    ts::CollectingObjectSink sink;
    p.runInto(sink, nullptr, CancellationToken());
    if (droppedByFilter != nullptr) {
        *droppedByFilter = detail::PipelineAccess::impl(p).counters.droppedByFilter.load();
    }
    return sink.records();
}

std::vector<Point64> ringOf(const Polygon& polygon, int32_t ring) {
    std::vector<Point64> points(static_cast<size_t>(polygon.ringPointCount(ring)));
    polygon.ringPoints(ring, points.data(), static_cast<int64_t>(points.size()));
    return points;
}

std::vector<ObjectRecord> sortedById(std::vector<ObjectRecord> records) {
    std::sort(records.begin(), records.end(),
              [](const ObjectRecord& a, const ObjectRecord& b) { return a.id() < b.id(); });
    return records;
}

// Whether every record carries the finisher's tag.
::testing::AssertionResult allFinished(const std::vector<ObjectRecord>& records) {
    if (records.empty()) {
        return ::testing::AssertionFailure() << "no object reached the sink";
    }
    for (size_t i = 0; i < records.size(); ++i) {
        Measurement tag;
        if (!records[i].measurements().find("finished", tag) || tag.value() != 1.0) {
            return ::testing::AssertionFailure() << "object " << i << " of " << records.size() << " is not finished";
        }
    }
    return ::testing::AssertionSuccess();
}

// Whether two runs stored the same objects: ids, rings point for point and areas.
::testing::AssertionResult sameObjects(const std::vector<ObjectRecord>& expected,
                                       const std::vector<ObjectRecord>& actual) {
    if (expected.size() != actual.size()) {
        return ::testing::AssertionFailure() << actual.size() << " objects where " << expected.size()
                                             << " are expected";
    }
    const std::vector<ObjectRecord> e = sortedById(expected);
    const std::vector<ObjectRecord> a = sortedById(actual);
    for (size_t i = 0; i < e.size(); ++i) {
        if (e[i].id() != a[i].id()) {
            return ::testing::AssertionFailure() << "object " << i << ": the id differs";
        }
        if (e[i].geometry().ringCount() != a[i].geometry().ringCount()) {
            return ::testing::AssertionFailure() << "object " << i << ": the ring count differs";
        }
        for (int32_t r = 0; r < e[i].geometry().ringCount(); ++r) {
            if (ringOf(e[i].geometry(), r) != ringOf(a[i].geometry(), r)) {
                return ::testing::AssertionFailure() << "object " << i << ": ring " << r << " differs";
            }
        }
        Measurement ea;
        Measurement aa;
        if (!e[i].measurements().find("area", ea) || !a[i].measurements().find("area", aa) ||
            ea.value() != aa.value()) {
            return ::testing::AssertionFailure() << "object " << i << ": the area differs";
        }
    }
    return ::testing::AssertionSuccess();
}

std::shared_ptr<slideio::Scene> blobScene() {
    ts::registerTestOperations();
    return ts::makeBlobScene({{1000, 800, 0, 0}}, 0, ts::BlobChannels::Mask);
}

} // namespace

TEST(ObjectFinishing, EveryTileObjectIsFinished) {
    const std::shared_ptr<slideio::Scene> scene = blobScene();
    PipelineSpec spec;
    spec.operation("test.components", componentParameters(false));
    for (int32_t side : kTileSides) {
        SCOPED_TRACE("tile side " + std::to_string(side));
        EXPECT_TRUE(allFinished(run(scene, side, spec)));
    }
}

TEST(ObjectFinishing, EveryMergedObjectIsFinished) {
    const std::shared_ptr<slideio::Scene> scene = blobScene();
    PipelineSpec spec;
    spec.operation("test.components", componentParameters(true));
    const std::vector<ObjectRecord> reference = run(scene, 1000, spec);
    for (int32_t side : kTileSides) {
        SCOPED_TRACE("tile side " + std::to_string(side));
        const std::vector<ObjectRecord> records = run(scene, side, spec);
        EXPECT_TRUE(allFinished(records));
        EXPECT_TRUE(sameObjects(reference, records));
    }
}

TEST(ObjectFinishing, FinisherDiscardsMergedObject) {
    ts::registerTestOperations();
    OperationParameters parameters = componentParameters(true);
    parameters.setDouble("finishMinArea", 50.0);
    PipelineSpec spec;
    spec.operation("test.components", parameters);
    for (int32_t side : kTileSides) {
        SCOPED_TRACE("tile side " + std::to_string(side));
        // Two rectangles 10 rows high straddling the first vertical seam: 4 wide
        // (area 40) and 6 wide (area 60).
        const int64_t seam = side;
        const auto pixel = [seam](int, int64_t x, int64_t y) {
            const bool small = y >= 10 && y < 20 && x >= seam - 2 && x < seam + 2;
            const bool large = y >= 40 && y < 50 && x >= seam - 3 && x < seam + 3;
            return small || large ? 255.0 : 0.0;
        };
        const std::shared_ptr<slideio::Scene> scene =
            ts::makeSyntheticScene({{600, 300, 0, 0}}, {slideio::DataType::DT_Byte}, pixel);

        int64_t dropped = 0;
        const std::vector<ObjectRecord> records = run(scene, side, spec, &dropped);
        ASSERT_EQ(records.size(), 1u);
        Measurement area;
        ASSERT_TRUE(records.front().measurements().find("area", area));
        EXPECT_EQ(area.value(), 60.0);
        EXPECT_EQ(dropped, 1);
        EXPECT_TRUE(allFinished(records));

        int64_t droppedWhole = 0;
        EXPECT_TRUE(sameObjects(run(scene, 600, spec, &droppedWhole), records));
        EXPECT_EQ(droppedWhole, 1);
    }
}

TEST(ObjectFinishing, TileFilterCountsOnlyWhatFinishingKeeps) {
    ts::registerTestOperations();
    OperationParameters parameters = componentParameters(false);
    parameters.setDouble("finishMinArea", 50.0);
    const OperationPtr producer = OperationCatalog::instance().create("test.components", parameters);
    ts::CollectingObjectSink target;
    detail::FinishingSink finishing(target, {{kLayer, producer.get()}}, 0.0, 0);
    detail::RunCounters counters;
    detail::TileObjectFilter filter(&finishing, Rect64{0, 0, 100, 100}, nullptr, counters);

    // A record of `area` whose ring starts at its anchor (normalised) or not.
    const auto record = [](double area, bool normalised) {
        const std::vector<Point64> fromAnchor = {{10, 10}, {20, 10}, {20, 20}, {10, 20}};
        const std::vector<Point64> rotated = {{20, 10}, {20, 20}, {10, 20}, {10, 10}};
        const std::vector<Point64>& ring = normalised ? fromAnchor : rotated;
        Polygon polygon;
        polygon.addRing(ring.data(), static_cast<int32_t>(ring.size()), false);
        ObjectRecord r;
        r.setGeometry(polygon);
        r.setLayer(kLayer);
        r.measurements().set("area", area, MeasurementUnit::Dimensionless);
        return r;
    };
    for (const bool normalised : {true, false}) {
        filter.emit(record(10.0, normalised));
        filter.emit(record(100.0, normalised));
    }
    EXPECT_EQ(target.emittedCount(), 2);
    EXPECT_EQ(filter.emittedCount(), 2);
    EXPECT_EQ(finishing.emittedCount(), 2);
}

namespace {

// A sink noting, for each record, whether it arrived through emitOwned(), and
// keeping the records that did.
class OwnershipSink : public ObjectSink {
public:
    void emit(const ObjectRecord&) override { owned.push_back(false); }
    bool emitOwned(ObjectRecord&& record) override {
        owned.push_back(true);
        records.push_back(std::move(record));
        return true;
    }
    void flush() override {}
    int64_t emittedCount() const override { return static_cast<int64_t>(owned.size()); }

    std::vector<bool> owned;
    std::vector<ObjectRecord> records;
};

// A 10 x 10 square of area 100 on kLayer whose ring starts at its anchor
// (normalised) or not.
ObjectRecord squareRecord(bool normalised) {
    const std::vector<Point64> fromAnchor = {{10, 10}, {20, 10}, {20, 20}, {10, 20}};
    const std::vector<Point64> rotated = {{20, 10}, {20, 20}, {10, 20}, {10, 10}};
    const std::vector<Point64>& ring = normalised ? fromAnchor : rotated;
    Polygon polygon;
    polygon.addRing(ring.data(), static_cast<int32_t>(ring.size()), false);
    ObjectRecord r;
    r.setGeometry(polygon);
    r.setLayer(kLayer);
    r.measurements().set("area", 100.0, MeasurementUnit::Dimensionless);
    return r;
}

} // namespace

TEST(ObjectFinishing, OwnedRecordsAreNotCopied) {
    ts::registerTestOperations();
    OperationParameters parameters = componentParameters(false);
    parameters.setDouble("finishMinArea", 50.0);
    const OperationPtr producer = OperationCatalog::instance().create("test.components", parameters);
    OwnershipSink target;
    detail::FinishingSink finishing(target, {{kLayer, producer.get()}}, 0.0, 0);
    detail::RunCounters counters;
    detail::TileObjectFilter filter(&finishing, Rect64{0, 0, 100, 100}, nullptr, counters);

    for (const bool normalised : {false, true}) {
        filter.emit(squareRecord(normalised));
    }
    EXPECT_EQ(target.owned, (std::vector<bool>{true, true}));
    EXPECT_EQ(filter.emittedCount(), 2);
    EXPECT_EQ(finishing.emittedCount(), 2);

    // A record handed over not normalised is normalised in place and passed on.
    EXPECT_TRUE(filter.emitOwned(squareRecord(false)));
    EXPECT_EQ(target.owned, (std::vector<bool>{true, true, true}));
    ASSERT_EQ(target.records.size(), 3u);
    const std::vector<Point64> outer = ringOf(target.records.back().geometry(), 0);
    ASSERT_FALSE(outer.empty());
    EXPECT_TRUE(outer.front() == (Point64{10, 10}));
    EXPECT_TRUE(outer.front() == target.records.back().geometry().anchor());
    EXPECT_EQ(filter.emittedCount(), 3);
    EXPECT_EQ(finishing.emittedCount(), 3);
}

TEST(ObjectFinishing, OperationWithoutFinishingIsPassedThrough) {
    const std::shared_ptr<slideio::Scene> scene = blobScene();
    for (bool seams : {false, true}) {
        SCOPED_TRACE(seams ? "with seams" : "without seams");
        OperationParameters finishing = componentParameters(seams);
        finishing.setDouble("finishMinArea", 50.0);
        PipelineSpec finished;
        finished.operation("test.components", finishing);
        OperationParameters passing = finishing;
        passing.setBool("noFinishing", true);
        PipelineSpec passed;
        passed.operation("test.components", passing);

        // With finishing, objects are tagged, and without seams some
        // tile-clipped pieces fall below the minimum area.
        int64_t droppedFinishing = -1;
        EXPECT_TRUE(allFinished(run(scene, 100, finished, &droppedFinishing)));
        if (!seams) {
            ASSERT_GT(droppedFinishing, 0);
        }

        int64_t dropped = -1;
        const std::vector<ObjectRecord> records = run(scene, 100, passed, &dropped);
        ASSERT_FALSE(records.empty());
        for (size_t i = 0; i < records.size(); ++i) {
            Measurement tag;
            EXPECT_FALSE(records[i].measurements().find("finished", tag)) << "object " << i << " is finished";
        }
        EXPECT_EQ(dropped, 0);
    }
}

namespace {

// An operation overriding finishObject() alone.
class FinishOnlyOperation : public Operation {
public:
    const char* name() const override { return "test.finishOnly"; }
    ChannelArity arity() const override { return ChannelArity::Joint; }
    bool requiresUniformTypes() const override { return false; }
    DataTypeSet supportedTypes() const override { return DataTypeSet().add(DataType::Byte); }
    Halo halo() const override { return Halo::uniform(0); }
    DataType outputType(DataType) const override { return DataType::Byte; }
    const OperationParameters& parameters() const override { return parameters_; }
    void apply(OperationContext&) override {}
    bool finishObject(ObjectRecord&, const ObjectFinishContext&) const override { return false; }

private:
    OperationParameters parameters_;
};

} // namespace

TEST(ObjectFinishing, FinishingIsTheDefault) {
    const FinishOnlyOperation operation;
    EXPECT_TRUE(operation.finishesObjects());

    // So the sink calls its finishObject(), which discards the record.
    ts::CollectingObjectSink target;
    std::atomic<int64_t> dropped{0};
    detail::FinishingSink finishing(target, {{kLayer, &operation}}, 0.0, 0, &dropped);
    EXPECT_FALSE(finishing.emitKept(squareRecord(true)));
    EXPECT_EQ(target.emittedCount(), 0);
    EXPECT_EQ(dropped.load(), 1);
}

TEST(ObjectFinishing, TileObjectRingsAreNormalised) {
    const std::shared_ptr<slideio::Scene> scene = blobScene();
    PipelineSpec plain;
    plain.operation("test.components", componentParameters(false));
    OperationParameters rotate = componentParameters(false);
    rotate.setBool("rotateRings", true);
    PipelineSpec rotated;
    rotated.operation("test.components", rotate);

    const std::vector<ObjectRecord> records = run(scene, 100, rotated);
    ASSERT_FALSE(records.empty());
    for (size_t i = 0; i < records.size(); ++i) {
        const Polygon& polygon = records[i].geometry();
        const std::vector<Point64> outer = ringOf(polygon, 0);
        ASSERT_FALSE(outer.empty());
        const Point64 anchor = polygon.anchor();
        EXPECT_TRUE(outer.front().x == anchor.x && outer.front().y == anchor.y)
            << "object " << i << " does not start at its anchor";
    }
    EXPECT_TRUE(sameObjects(run(scene, 100, plain), records));
}

TEST(ObjectFinishing, TwoProducersOnOneLayerRefused) {
    const std::shared_ptr<slideio::Scene> scene = blobScene();
    PipelineSpec spec;
    spec.operation("test.components", OperationParameters());
    spec.operation("test.components", OperationParameters());
    Pipeline p(scene, config(100), spec);
    p.bindLayer("objects", kLayer);
    try {
        p.validate();
        FAIL() << "validate() accepted two producers on one layer";
    } catch (const InvalidArgumentError& e) {
        const std::string message = e.what();
        EXPECT_NE(message.find("test.components"), std::string::npos) << message;
        EXPECT_NE(message.find("\"objects\""), std::string::npos) << message;
    }
}

TEST(ObjectFinishing, ObjectProducerFollowedByHaloRefused) {
    const std::shared_ptr<slideio::Scene> scene = blobScene();
    PipelineSpec spec;
    spec.operation("test.components", OperationParameters());
    OperationParameters box;
    box.setInt("radius", 1);
    spec.operation("test.boxMean", box);
    Pipeline p(scene, config(100), spec);
    p.bindLayer("objects", kLayer);
    try {
        p.validate();
        FAIL() << "validate() accepted an object producer followed by a halo";
    } catch (const InvalidArgumentError& e) {
        const std::string message = e.what();
        EXPECT_NE(message.find("test.components"), std::string::npos) << message;
    }
}

TEST(ObjectFinishing, ObjectProducerFollowedByPointOpAccepted) {
    const std::shared_ptr<slideio::Scene> scene = blobScene();
    PipelineSpec spec;
    spec.operation("test.components", OperationParameters());
    spec.operation("test.threshold", OperationParameters());
    Pipeline p(scene, config(100), spec);
    p.bindLayer("objects", kLayer);
    EXPECT_NO_THROW(p.validate());
}
