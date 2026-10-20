// Requirements: UR-RES-1, UR-RES-2, UR-RES-3, UR-EXE-7, UR-STG-5, UR-STG-6, UR-OBS-2, UR-OBS-4, UR-QA-3
#include "storage/store_fixture.hpp"

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/core/object_record.hpp>
#include <slideio/processor/storage/event_journal.hpp>
#include <slideio/processor/storage/object_store.hpp>
#include <slideio/processor/storage/store.hpp>

#include <gtest/gtest.h>

#include <string>
#include <thread>
#include <vector>

using namespace slideio::processor;

namespace {

Polygon square(int64_t x, int64_t y, int64_t side) {
    const Point64 points[] = {{x, y}, {x + side, y}, {x + side, y + side}, {x, y + side}};
    Polygon polygon;
    polygon.addRing(points, 4, false);
    return polygon;
}

ObjectRecord rec(LayerId layer, int64_t x, int64_t y, int64_t side) {
    ObjectRecord record;
    record.setLayer(layer);
    record.setGeometry(square(x, y, side));
    return record;
}

std::vector<Event> eventsOfKind(Store& store, EventKind kind) {
    std::vector<Event> events;
    EventCursorPtr cursor = store.events().subscribe(EventJournal::kFromBeginning);
    while (cursor->next(0)) {
        if (cursor->current().kind() == kind) events.push_back(cursor->current());
    }
    return events;
}

int64_t objectsAddedTotal(Store& store) {
    int64_t total = 0;
    for (const Event& e : eventsOfKind(store, EventKind::ObjectsAdded)) total += e.objectCount();
    return total;
}

// Runs fn, which must throw InvalidArgumentError, and returns the message.
template <class Fn>
std::string invalidArgumentMessage(Fn&& fn) {
    try {
        fn();
    } catch (const InvalidArgumentError& e) {
        return e.what();
    }
    ADD_FAILURE() << "no InvalidArgumentError";
    return {};
}

class ObjectWriterTest : public StoreFixture {
protected:
    void SetUp() override {
        StoreFixture::SetUp();
        cells = store().layers().declareSemantic("cells", store().layers().pixelLayer());
        nuclei = store().layers().declareSemantic("nuclei", cells);
        a = store().stageStateWriter().declare("a", "");
        b = store().stageStateWriter().declare("b", "");
    }
    ObjectStore& objects() { return store().objects(); }
    void complete(StageId stage) { store().stageStateWriter().setStatus(stage, StageStatus::Complete); }

    LayerId cells;
    LayerId nuclei;
    StageId a;
    StageId b;
};

} // namespace

TEST_F(ObjectWriterTest, BatchesCommitWithOneEventPerLayer) {
    ObjectStore& o = objects();
    ObjectWriterPtr w = o.writerForStage(a);
    EXPECT_EQ(w->stage(), a);
    EXPECT_EQ(w->batchSizeObjects(), config().objectBatchSize());
    w->setBatchSizeObjects(3);
    EXPECT_EQ(w->batchSizeObjects(), 3);
    w->emit(rec(cells, 0, 0, 4));
    w->emit(rec(cells, 10, 0, 4));
    EXPECT_EQ(o.totalObjectCount(), 0);  // pending
    EXPECT_EQ(w->emittedCount(), 2);
    EXPECT_TRUE(eventsOfKind(store(), EventKind::ObjectsAdded).empty());
    w->emit(rec(nuclei, 20, 0, 4));  // the third commits
    EXPECT_EQ(o.totalObjectCount(), 3);
    EXPECT_EQ(w->emittedCount(), 3);
    EXPECT_EQ(o.objectCountInLayer(cells), 2);
    EXPECT_EQ(o.objectCountInLayer(nuclei), 1);
    EXPECT_EQ(o.objectCountInLayer(LayerId(99)), 0);

    const std::vector<Event> added = eventsOfKind(store(), EventKind::ObjectsAdded);
    ASSERT_EQ(added.size(), 2u);
    EXPECT_EQ(added[0].layer(), cells);
    EXPECT_EQ(added[0].stage(), a);
    EXPECT_EQ(added[0].objectCount(), 2);
    EXPECT_EQ(added[0].boundingBox(), (Rect64{0, 0, 14, 4}));
    EXPECT_EQ(added[1].layer(), nuclei);
    EXPECT_EQ(added[1].stage(), a);
    EXPECT_EQ(added[1].objectCount(), 1);
    EXPECT_EQ(added[1].boundingBox(), (Rect64{20, 0, 4, 4}));
    EXPECT_EQ(added[1].sequence(), added[0].sequence() + 1);  // one transaction, nothing between

    w->flush();  // nothing pending: no event
    EXPECT_EQ(eventsOfKind(store(), EventKind::ObjectsAdded).size(), 2u);
    EXPECT_THROW(w->setBatchSizeObjects(0), InvalidArgumentError);
    EXPECT_THROW(w->setBatchSizeObjects(-5), InvalidArgumentError);
    EXPECT_EQ(w->batchSizeObjects(), 3);
}

TEST_F(ObjectWriterTest, DestructorFlushes) {
    {
        ObjectWriterPtr w = objects().writerForStage(a);
        w->emit(rec(cells, 0, 0, 4));
        EXPECT_EQ(objects().totalObjectCount(), 0);
    }
    EXPECT_EQ(objects().totalObjectCount(), 1);
}

TEST_F(ObjectWriterTest, WriterOwnsTheStage) {
    ObjectWriterPtr w = objects().writerForStage(a);
    const ObjectRecord claimed = ObjectRecord::restored(rec(cells, 0, 0, 4), b);
    ASSERT_EQ(claimed.stage(), b);
    w->emit(claimed);
    w->flush();
    complete(a);
    ObjectRecord out;
    ASSERT_TRUE(objects().get(claimed.id(), out));
    EXPECT_EQ(out.stage(), a);
    EXPECT_EQ(out.id(), claimed.id());
    // b is not complete, so the object would be invisible had it been attributed to b.
    EXPECT_EQ(objects().count(ObjectQuery().stage(b).visibility(StageVisibility::IncludeInFlight)), 0);
}

TEST_F(ObjectWriterTest, RejectsBadRecordsNamingTheProblem) {
    ObjectWriterPtr w = objects().writerForStage(a);
    EXPECT_NE(invalidArgumentMessage([&] { w->emit(rec(LayerId(99), 0, 0, 4)); }).find("99"), std::string::npos);
    EXPECT_THROW(w->emit(rec(LayerId(), 0, 0, 4)), InvalidArgumentError);
    EXPECT_NE(invalidArgumentMessage([&] { w->emit(rec(store().layers().pixelLayer(), 0, 0, 4)); }).find("pixel"),
              std::string::npos);

    ObjectRecord empty;
    empty.setLayer(cells);
    EXPECT_NE(invalidArgumentMessage([&] { w->emit(empty); }).find("empty"), std::string::npos);

    const std::string anchor = invalidArgumentMessage([&] { w->emit(rec(cells, -1, 0, 4)); });
    EXPECT_NE(anchor.find("(-1, 0)"), std::string::npos) << anchor;
    EXPECT_NE(invalidArgumentMessage([&] { w->emit(rec(cells, 0, int64_t(1) << 32, 4)); }).find("4294967296"),
              std::string::npos);

    ObjectRecord labelled = rec(cells, 0, 0, 4);
    labelled.setLabel(LabelId(4242));
    EXPECT_NE(invalidArgumentMessage([&] { w->emit(labelled); }).find("4242"), std::string::npos);

    EXPECT_EQ(w->emittedCount(), 0);  // nothing pending afterwards
    w->flush();
    EXPECT_EQ(objects().totalObjectCount(), 0);
    EXPECT_TRUE(eventsOfKind(store(), EventKind::ObjectsAdded).empty());

    // A held local label is accepted.
    labelled.setLabel(store().vocabulary().addLocal("mitosis"));
    w->emit(labelled);
    w->flush();
    EXPECT_EQ(objects().totalObjectCount(), 1);
}

TEST_F(ObjectWriterTest, WriterForStageValidates) {
    EXPECT_THROW(objects().writerForStage(StageId(99)), InvalidArgumentError);
    EXPECT_THROW(objects().writerForStage(StageId()), InvalidArgumentError);
    EXPECT_THROW(objects().discardStage(StageId(99)), InvalidArgumentError);
    StorePtr readOnly = reopen(OpenMode::ReadOnly);
    EXPECT_THROW(readOnly->objects().writerForStage(a), IoError);
    EXPECT_THROW(readOnly->objects().discardStage(a), IoError);
    EXPECT_EQ(readOnly->objects().totalObjectCount(), 0);
}

TEST_F(ObjectWriterTest, AnchorCollisionsNameLayerAndAnchor) {  // Review Focus 1
    ObjectStore& o = objects();
    ObjectWriterPtr w = o.writerForStage(a);
    w->emit(rec(cells, 5, 5, 4));
    // The same anchor pending twice in one writer: the second emit throws.
    const std::string pending = invalidArgumentMessage([&] { w->emit(rec(cells, 5, 5, 8)); });
    EXPECT_NE(pending.find("cells"), std::string::npos) << pending;
    EXPECT_NE(pending.find("(5, 5)"), std::string::npos) << pending;
    EXPECT_EQ(w->emittedCount(), 1);
    w->flush();
    EXPECT_EQ(o.totalObjectCount(), 1);

    // Committed in batch 1, re-emitted in batch 2: the commit throws and batch 2 rolls back.
    w->emit(rec(cells, 50, 50, 4));
    w->emit(rec(cells, 5, 5, 4));
    EXPECT_EQ(w->emittedCount(), 3);
    const std::string committed = invalidArgumentMessage([&] { w->flush(); });
    EXPECT_NE(committed.find("cells"), std::string::npos) << committed;
    EXPECT_NE(committed.find("(5, 5)"), std::string::npos) << committed;
    EXPECT_EQ(o.totalObjectCount(), 1);
    EXPECT_EQ(w->emittedCount(), 1);  // the dropped batch no longer counts
    EXPECT_EQ(objectsAddedTotal(store()), 1);
    w->flush();  // the batch was dropped: nothing to retry
    EXPECT_EQ(o.totalObjectCount(), 1);

    // A collision found by a commit that emit triggered throws from that emit.
    w->setBatchSizeObjects(1);
    EXPECT_THROW(w->emit(rec(cells, 5, 5, 4)), InvalidArgumentError);
    EXPECT_EQ(o.totalObjectCount(), 1);

    // Another stage emitting stage a's anchor in the same layer.
    ObjectWriterPtr other = o.writerForStage(b);
    other->emit(rec(cells, 5, 5, 4));
    EXPECT_THROW(other->flush(), InvalidArgumentError);
    EXPECT_EQ(o.totalObjectCount(), 1);

    // The same anchor in a different layer is a different object.
    other->emit(rec(nuclei, 5, 5, 4));
    other->flush();
    EXPECT_EQ(o.totalObjectCount(), 2);
    EXPECT_EQ(objectsAddedTotal(store()), 2);
}

TEST_F(ObjectWriterTest, ConcurrentWritersFromManyThreads) {
    constexpr int kThreads = 8;
    constexpr int kPerThread = 2000;
    std::vector<std::thread> threads;
    std::vector<std::string> failures(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            try {
                ObjectWriterPtr w = objects().writerForStage(a);
                w->setBatchSizeObjects(300);
                for (int i = 0; i < kPerThread; ++i) w->emit(rec(cells, int64_t(i) * 5, int64_t(t) * 5, 4));
                w->flush();
            } catch (const std::exception& e) {
                failures[t] = e.what();
            }
        });
    }
    for (std::thread& thread : threads) thread.join();
    for (const std::string& failure : failures) EXPECT_EQ(failure, "");
    EXPECT_EQ(objects().totalObjectCount(), kThreads * kPerThread);
    EXPECT_EQ(objectsAddedTotal(store()), kThreads * kPerThread);

    // emit is ThreadSafe on one shared writer too.
    ObjectWriterPtr shared = objects().writerForStage(b);
    shared->setBatchSizeObjects(64);
    threads.clear();
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < 500; ++i) shared->emit(rec(nuclei, int64_t(i) * 5, int64_t(t) * 5, 4));
        });
    }
    for (std::thread& thread : threads) thread.join();
    shared->flush();
    EXPECT_EQ(shared->emittedCount(), 2000);
    EXPECT_EQ(objects().objectCountInLayer(nuclei), 2000);
}

TEST_F(ObjectWriterTest, DiscardStageRemovesOnlyItsObjects) {  // UR-STG-6
    ObjectStore& o = objects();
    {
        ObjectWriterPtr wa = o.writerForStage(a);
        for (int i = 0; i < 5; ++i) {
            ObjectRecord r = rec(cells, int64_t(i) * 10, 0, 4);
            r.measurements().set("area", 16.0, MeasurementUnit::SquarePixels);
            wa->emit(r);
        }
        ObjectWriterPtr wb = o.writerForStage(b);
        for (int i = 0; i < 3; ++i) wb->emit(rec(cells, int64_t(i) * 10, 100, 4));
    }
    ASSERT_EQ(o.totalObjectCount(), 8);
    o.discardStage(a);
    EXPECT_EQ(o.totalObjectCount(), 3);
    EXPECT_EQ(o.objectCountInLayer(cells), 3);

    const std::vector<Event> discarded = eventsOfKind(store(), EventKind::StageDiscarded);
    ASSERT_EQ(discarded.size(), 1u);
    EXPECT_EQ(discarded[0].stage(), a);
    EXPECT_EQ(discarded[0].objectCount(), 5);

    complete(a);
    complete(b);
    const ObjectQuery overA = ObjectQuery().intersecting(Rect64{0, 0, 50, 10});
    EXPECT_EQ(o.count(overA), 0);
    EXPECT_FALSE(o.query(overA)->next());
    EXPECT_EQ(o.count(ObjectQuery().intersecting(Rect64{0, 100, 50, 10})), 3);

    // A re-run writes the same anchors again.
    ObjectWriterPtr rerun = o.writerForStage(a);
    rerun->emit(rec(cells, 0, 0, 4));
    rerun->flush();
    EXPECT_EQ(o.count(overA), 1);
    o.discardStage(b);  // nothing measured: still one transaction, one event
    EXPECT_EQ(o.totalObjectCount(), 1);
    EXPECT_EQ(eventsOfKind(store(), EventKind::StageDiscarded).back().objectCount(), 3);
}

TEST_F(ObjectWriterTest, MeasurementsAndLabelRoundTrip) {
    ObjectRecord first = rec(cells, 0, 0, 4);
    first.setLabel(LabelId(2));
    first.measurements().set("area", 16.5, MeasurementUnit::SquareMicrometres);
    first.measurements().set("perimeter", 14.25, MeasurementUnit::Micrometres);
    ObjectRecord second = rec(cells, 10, 0, 4);  // the same names, the other order, no label
    second.measurements().set("perimeter", 3.0, MeasurementUnit::Pixels);
    second.measurements().set("area", 9.0, MeasurementUnit::SquarePixels);
    ObjectWriterPtr w = objects().writerForStage(a);
    w->emit(first);
    w->emit(second);
    w->flush();
    complete(a);

    ObjectRecord out;
    ASSERT_TRUE(objects().get(first.id(), out));
    EXPECT_EQ(out.layer(), cells);
    EXPECT_EQ(out.boundingBox(), (Rect64{0, 0, 4, 4}));
    ASSERT_TRUE(out.hasLabel());
    EXPECT_EQ(out.label(), LabelId(2));
    ASSERT_EQ(out.measurements().count(), 2);
    EXPECT_STREQ(out.measurements().at(0).name(), "area");
    EXPECT_EQ(out.measurements().at(0).value(), 16.5);
    EXPECT_EQ(out.measurements().at(0).unit(), MeasurementUnit::SquareMicrometres);
    EXPECT_STREQ(out.measurements().at(1).name(), "perimeter");
    EXPECT_EQ(out.measurements().at(1).value(), 14.25);
    EXPECT_EQ(out.measurements().at(1).unit(), MeasurementUnit::Micrometres);

    ASSERT_TRUE(objects().get(second.id(), out));
    EXPECT_FALSE(out.hasLabel());
    ASSERT_EQ(out.measurements().count(), 2);
    EXPECT_STREQ(out.measurements().at(0).name(), "perimeter");
    EXPECT_EQ(out.measurements().at(0).unit(), MeasurementUnit::Pixels);
    EXPECT_STREQ(out.measurements().at(1).name(), "area");
    EXPECT_EQ(out.measurements().at(1).value(), 9.0);

    EXPECT_FALSE(objects().get(ObjectId(), out));
    EXPECT_FALSE(objects().get(ObjectId::fromAnchor(cells, Point64{1, 1}), out));
}

// A writer and a cursor are handed out as shared_ptrs and may outlive the Store.
TEST(ObjectHandleLifetime, WriterAndCursorOutliveTheirStore) {
    StoreFilesGuard files("objectlife");
    Configuration config;
    StorePtr store = Store::open(files.utf8().c_str(), OpenMode::CreateNew, config);
    const LayerId cells = store->layers().declareSemantic("cells", store->layers().pixelLayer());
    const StageId a = store->stageStateWriter().declare("a", "");
    ObjectWriterPtr writer = store->objects().writerForStage(a);
    writer->emit(rec(cells, 0, 0, 4));
    writer->flush();
    writer->emit(rec(cells, 10, 0, 4));
    CursorPtr cursor = store->objects().query(ObjectQuery().visibility(StageVisibility::IncludeInFlight));
    store.reset();
    EXPECT_THROW(cursor->next(), IoError);
    EXPECT_THROW(writer->flush(), IoError);  // the pending batch is dropped
    EXPECT_EQ(writer->emittedCount(), 1);
    EXPECT_THROW(writer->emit(rec(cells, 20, 0, 4)), IoError);
    cursor.reset();
    writer.reset();  // nothing pending: destroying it afterwards is clean
}
