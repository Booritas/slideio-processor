// Requirements: UR-RES-4, UR-RES-17, UR-RES-18, UR-STG-4, UR-STG-6, UR-QA-3
#include "storage/detail/store_impl.hpp"
#include "storage/store_fixture.hpp"

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/core/measurement.hpp>
#include <slideio/processor/core/object_record.hpp>
#include <slideio/processor/storage/annotation_store.hpp>
#include <slideio/processor/storage/event_journal.hpp>
#include <slideio/processor/storage/object_store.hpp>
#include <slideio/processor/storage/store.hpp>
#include <slideio/processor/storage/vocabulary_registry.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <memory>
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

ObjectRecord labelled(LayerId layer, int64_t x, int64_t y, int64_t side, LabelId label) {
    ObjectRecord record = rec(layer, x, y, side);
    record.setLabel(label);
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

bool contains(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

// Layers: cells on the pixel layer and nuclei above cells. Stage a produces objects;
// c, d and e annotate them.
class AnnotationStoreTest : public StoreFixture {
protected:
    void SetUp() override {
        StoreFixture::SetUp();
        LayerRegistry& layers = store().layers();
        cells = layers.declareSemantic("cells", layers.pixelLayer());
        nuclei = layers.declareSemantic("nuclei", cells);
        a = store().stageStateWriter().declare("stage-a", "");
        b = store().stageStateWriter().declare("stage-b", "");
        c = store().stageStateWriter().declare("stage-c", "");
        d = store().stageStateWriter().declare("stage-d", "");
        e = store().stageStateWriter().declare("stage-e", "");
    }
    AnnotationStore& annotations() { return store().annotations(); }
    void setStatus(StageId stage, StageStatus status) { store().stageStateWriter().setStatus(stage, status); }
    void complete(StageId stage) { setStatus(stage, StageStatus::Complete); }
    // Commits the records under `stage` and returns their ids, in order.
    std::vector<ObjectId> commit(StageId stage, std::initializer_list<ObjectRecord> records) {
        ObjectWriterPtr writer = store().objects().writerForStage(stage);
        std::vector<ObjectId> ids;
        for (const ObjectRecord& r : records) {
            writer->emit(r);
            ids.push_back(r.id());
        }
        writer->flush();
        return ids;
    }

    LayerId cells, nuclei;
    StageId a, b, c, d, e;
};

} // namespace

TEST_F(AnnotationStoreTest, EffectiveSetRowLabelFirstThenAscending) {
    const std::vector<ObjectId> ids = commit(a, {labelled(cells, 0, 0, 4, LabelId(3)), rec(cells, 10, 0, 4)});
    const ObjectId o = ids[0], unlabelled = ids[1];
    complete(a);

    AnnotationWriterPtr wc = annotations().writerForStage(c);
    EXPECT_EQ(wc->stage(), c);
    EXPECT_EQ(wc->batchSizeAnnotations(), config().objectBatchSize());
    wc->assertLabel(o, LabelId(7));
    wc->assertLabel(o, LabelId(2));
    wc->flush();
    AnnotationWriterPtr wd = annotations().writerForStage(d);
    wd->assertLabel(o, LabelId(7));
    wd->flush();
    complete(c);
    complete(d);

    ASSERT_EQ(annotations().labelCount(o), 3);
    EXPECT_EQ(annotations().labelAt(o, 0), LabelId(3));
    EXPECT_EQ(annotations().labelAt(o, 1), LabelId(2));
    EXPECT_EQ(annotations().labelAt(o, 2), LabelId(7));
    EXPECT_EQ(annotations().labelAssertedBy(o, 0), a);
    EXPECT_EQ(annotations().labelAssertedBy(o, 1), c);
    EXPECT_EQ(annotations().labelAssertedBy(o, 2), c);  // 7 by c and d: the smallest stage
    EXPECT_EQ(annotations().labelAnnotationCount(), 3);

    EXPECT_THROW(annotations().labelAt(o, 3), InvalidArgumentError);
    EXPECT_THROW(annotations().labelAt(o, -1), InvalidArgumentError);
    EXPECT_THROW(annotations().labelAssertedBy(o, 3), InvalidArgumentError);

    EXPECT_EQ(annotations().labelCount(unlabelled), 0);
    EXPECT_THROW(annotations().labelAt(unlabelled, 0), InvalidArgumentError);
    EXPECT_EQ(annotations().labelCount(ObjectId()), 0);
    EXPECT_EQ(annotations().labelCount(rec(cells, 500, 500, 4).id()), 0);  // never stored

    // A stage asserting the row label adds no second entry for it.
    AnnotationWriterPtr we = annotations().writerForStage(e);
    we->assertLabel(o, LabelId(3));
    we->flush();
    complete(e);
    EXPECT_EQ(annotations().labelCount(o), 3);
    EXPECT_EQ(annotations().labelAssertedBy(o, 0), a);
}

// UR-RES-18: a workspace-local label applied to objects of an already completed stage.
TEST_F(AnnotationStoreTest, LocalLabelAppliedAfterProducerCompleted) {
    ObjectRecord nucleus = rec(nuclei, 0, 0, 4);
    nucleus.measurements().set("area", 16.0, MeasurementUnit::SquarePixels);
    const ObjectId o = commit(a, {nucleus})[0];
    complete(a);
    ObjectRecord before;
    ASSERT_TRUE(store().objects().get(o, before));

    const LabelId ki67 = store().vocabulary().addLocal("ki67_positive");
    AnnotationWriterPtr w = annotations().writerForStage(c);
    w->assertLabel(o, ki67);
    w->flush();
    complete(c);

    ObjectRecord after;
    ASSERT_TRUE(store().objects().get(o, after));
    EXPECT_FALSE(after.hasLabel());
    EXPECT_EQ(after.stage(), a);
    ASSERT_EQ(after.measurements().count(), 1);
    EXPECT_STREQ(after.measurements().at(0).name(), "area");
    EXPECT_EQ(after.measurements().at(0).value(), before.measurements().at(0).value());

    ASSERT_EQ(annotations().labelCount(o), 1);
    EXPECT_EQ(annotations().labelAt(o, 0), ki67);
    EXPECT_EQ(annotations().labelAssertedBy(o, 0), c);
    EXPECT_EQ(store().vocabulary().origin(annotations().labelAt(o, 0)), LabelOrigin::WorkspaceLocal);
}

TEST_F(AnnotationStoreTest, MeasurementsIdempotentAndConflicting) {
    const std::vector<ObjectId> ids = commit(a, {rec(cells, 0, 0, 4), rec(cells, 10, 0, 4)});
    const ObjectId o = ids[0], o2 = ids[1];
    complete(a);

    AnnotationWriterPtr wc = annotations().writerForStage(c);
    wc->assertMeasurement(o, "ki67", 0.4, MeasurementUnit::Dimensionless);
    wc->assertMeasurement(o, "ki67", 0.4, MeasurementUnit::Dimensionless);
    EXPECT_EQ(wc->assertedCount(), 2);
    const std::string pending = invalidArgumentMessage(
        [&] { wc->assertMeasurement(o, "ki67", 0.5, MeasurementUnit::Dimensionless); });
    EXPECT_TRUE(contains(pending, "ki67")) << pending;
    EXPECT_TRUE(contains(pending, "cells/")) << pending;
    EXPECT_THROW(wc->assertMeasurement(o, "ki67", 0.4, MeasurementUnit::Pixels), InvalidArgumentError);
    EXPECT_EQ(wc->assertedCount(), 2);
    wc->flush();
    EXPECT_EQ(annotations().measurementAnnotationCount(), 1);
    ASSERT_EQ(eventsOfKind(store(), EventKind::ObjectsAnnotated).size(), 1u);

    // An identical committed value from the same stage is a no-op: no row, no event.
    AnnotationWriterPtr same = annotations().writerForStage(c);
    same->assertMeasurement(o, "ki67", 0.4, MeasurementUnit::Dimensionless);
    same->flush();
    EXPECT_EQ(annotations().measurementAnnotationCount(), 1);
    EXPECT_EQ(eventsOfKind(store(), EventKind::ObjectsAnnotated).size(), 1u);

    // New, valid rows first ((o, "area") is inserted before (o, "ki67")), then a value
    // conflicting with the committed 0.4.
    AnnotationWriterPtr other = annotations().writerForStage(c);
    other->assertLabel(o2, LabelId(2));
    other->assertMeasurement(o, "area", 2.0, MeasurementUnit::SquarePixels);
    other->assertMeasurement(o, "ki67", 0.5, MeasurementUnit::Dimensionless);
    const std::string message = invalidArgumentMessage([&] { other->flush(); });
    EXPECT_TRUE(contains(message, "AnnotationWriter::flush")) << message;
    EXPECT_TRUE(contains(message, "ki67")) << message;
    EXPECT_TRUE(contains(message, "cells/")) << message;
    EXPECT_EQ(annotations().measurementAnnotationCount(), 1);  // "area" rolled back too
    EXPECT_EQ(annotations().labelAnnotationCount(), 0);        // and o2's label
    EXPECT_EQ(eventsOfKind(store(), EventKind::ObjectsAnnotated).size(), 1u);
    EXPECT_EQ(other->assertedCount(), 0);

    AnnotationWriterPtr wd = annotations().writerForStage(d);
    wd->assertMeasurement(o, "ki67", 0.9, MeasurementUnit::Dimensionless);
    wd->assertMeasurement(o, "area", 3.0, MeasurementUnit::SquareMicrometres);
    wd->flush();
    complete(c);
    complete(d);

    ASSERT_EQ(annotations().assertedMeasurementCount(o), 3);
    const Measurement first = annotations().assertedMeasurementAt(o, 0);
    EXPECT_STREQ(first.name(), "area");
    EXPECT_EQ(first.value(), 3.0);
    EXPECT_EQ(first.unit(), MeasurementUnit::SquareMicrometres);
    EXPECT_EQ(annotations().measurementAssertedBy(o, 0), d);
    const Measurement second = annotations().assertedMeasurementAt(o, 1);
    EXPECT_STREQ(second.name(), "ki67");
    EXPECT_EQ(second.value(), 0.4);
    EXPECT_EQ(annotations().measurementAssertedBy(o, 1), c);
    const Measurement third = annotations().assertedMeasurementAt(o, 2);
    EXPECT_STREQ(third.name(), "ki67");
    EXPECT_EQ(third.value(), 0.9);
    EXPECT_EQ(annotations().measurementAssertedBy(o, 2), d);
    EXPECT_THROW(annotations().assertedMeasurementAt(o, 3), InvalidArgumentError);
    EXPECT_THROW(annotations().measurementAssertedBy(o, -1), InvalidArgumentError);
    EXPECT_EQ(annotations().assertedMeasurementCount(o2), 0);

    Measurement m;
    ASSERT_TRUE(annotations().assertedMeasurement(o, "ki67", d, m));
    EXPECT_STREQ(m.name(), "ki67");
    EXPECT_EQ(m.value(), 0.9);
    EXPECT_EQ(m.unit(), MeasurementUnit::Dimensionless);
    EXPECT_FALSE(annotations().assertedMeasurement(o, "ki67", e, m));
    EXPECT_FALSE(annotations().assertedMeasurement(o, "absent", d, m));
    EXPECT_FALSE(annotations().assertedMeasurement(o, nullptr, d, m));
    EXPECT_EQ(m.value(), 0.9);  // unchanged when not found

    AnnotationWriterPtr we = annotations().writerForStage(e);
    EXPECT_THROW(we->assertMeasurement(o, "x", std::nan(""), MeasurementUnit::Dimensionless), InvalidArgumentError);
    EXPECT_THROW(we->assertMeasurement(o, "x", std::numeric_limits<double>::infinity(), MeasurementUnit::Pixels),
                 InvalidArgumentError);
    EXPECT_THROW(we->assertMeasurement(o, "", 1.0, MeasurementUnit::Dimensionless), InvalidArgumentError);
    EXPECT_THROW(we->assertMeasurement(o, nullptr, 1.0, MeasurementUnit::Dimensionless), InvalidArgumentError);
    EXPECT_THROW(we->assertMeasurement(o, "x", 1.0, static_cast<MeasurementUnit>(99)), InvalidArgumentError);
    EXPECT_THROW(we->assertMeasurement(o, "x", 1.0, static_cast<MeasurementUnit>(-1)), InvalidArgumentError);
    EXPECT_THROW(we->assertMeasurement(ObjectId(), "x", 1.0, MeasurementUnit::Dimensionless), InvalidArgumentError);
    EXPECT_EQ(we->assertedCount(), 0);
}

// Review Focus 1: an annotation naming an object still pending in an unflushed ObjectWriter.
TEST_F(AnnotationStoreTest, UnknownLabelAndMissingObject) {
    const ObjectId o = commit(a, {rec(cells, 0, 0, 4)})[0];
    AnnotationWriterPtr w = annotations().writerForStage(c);
    const std::string unknown = invalidArgumentMessage([&] { w->assertLabel(o, LabelId(4242)); });
    EXPECT_TRUE(contains(unknown, "4242")) << unknown;
    EXPECT_THROW(w->assertLabel(o, LabelId()), InvalidArgumentError);
    EXPECT_THROW(w->assertLabel(ObjectId(), LabelId(2)), InvalidArgumentError);
    EXPECT_EQ(w->assertedCount(), 0);

    ObjectWriterPtr objects = store().objects().writerForStage(a);
    const ObjectRecord pendingCell = rec(cells, 10, 10, 4);
    objects->emit(pendingCell);
    const ObjectId p = pendingCell.id();

    w->assertLabel(o, LabelId(2));  // valid, and inserted before the failing one
    w->assertLabel(p, LabelId(2));
    EXPECT_EQ(w->assertedCount(), 2);
    const std::string message = invalidArgumentMessage([&] { w->flush(); });
    EXPECT_TRUE(contains(message, "AnnotationWriter::flush: object cells/")) << message;
    EXPECT_TRUE(contains(message, "does not exist")) << message;
    EXPECT_EQ(annotations().labelAnnotationCount(), 0);
    EXPECT_EQ(w->assertedCount(), 0);
    EXPECT_TRUE(eventsOfKind(store(), EventKind::ObjectsAnnotated).empty());
    w->flush();  // nothing pending any more
    EXPECT_EQ(annotations().labelAnnotationCount(), 0);

    // A measurement on a missing object fails the same way.
    w->assertMeasurement(p, "ki67", 1.0, MeasurementUnit::Dimensionless);
    EXPECT_TRUE(contains(invalidArgumentMessage([&] { w->flush(); }), "does not exist"));
    EXPECT_EQ(annotations().measurementAnnotationCount(), 0);

    objects->flush();
    w->assertLabel(o, LabelId(2));
    w->assertLabel(p, LabelId(2));
    w->assertMeasurement(p, "ki67", 1.0, MeasurementUnit::Dimensionless);
    w->flush();
    EXPECT_EQ(annotations().labelAnnotationCount(), 2);
    EXPECT_EQ(annotations().measurementAnnotationCount(), 1);
    EXPECT_EQ(w->assertedCount(), 3);
}

TEST_F(AnnotationStoreTest, VisibilityEventAndDiscard) {
    const std::vector<ObjectId> ids =
        commit(a, {labelled(cells, 0, 0, 4, LabelId(3)), rec(cells, 10, 20, 6), rec(nuclei, 40, 40, 2)});
    const ObjectId o1 = ids[0], o2 = ids[1], n = ids[2];
    const ObjectId hidden = commit(b, {rec(cells, 100, 100, 4)})[0];  // stage b stays Pending
    complete(a);

    setStatus(c, StageStatus::Running);
    AnnotationWriterPtr w = annotations().writerForStage(c);
    w->assertLabel(o1, LabelId(7));
    w->assertMeasurement(o1, "ki67", 1.0, MeasurementUnit::Dimensionless);
    w->assertLabel(o2, LabelId(2));
    w->assertLabel(hidden, LabelId(2));
    w->flush();

    EXPECT_EQ(annotations().labelCount(o1), 1);  // only the row label while c runs
    EXPECT_EQ(annotations().labelCount(o2), 0);
    EXPECT_EQ(annotations().assertedMeasurementCount(o1), 0);
    Measurement m;
    EXPECT_FALSE(annotations().assertedMeasurement(o1, "ki67", c, m));

    complete(c);
    EXPECT_EQ(annotations().labelCount(o1), 2);
    EXPECT_EQ(annotations().labelCount(o2), 1);
    EXPECT_EQ(annotations().assertedMeasurementCount(o1), 1);
    EXPECT_TRUE(annotations().assertedMeasurement(o1, "ki67", c, m));
    EXPECT_EQ(annotations().labelCount(hidden), 0);  // its producer is not Complete
    EXPECT_EQ(annotations().assertedMeasurementCount(hidden), 0);

    std::vector<Event> annotated = eventsOfKind(store(), EventKind::ObjectsAnnotated);
    ASSERT_EQ(annotated.size(), 1u);
    EXPECT_EQ(annotated[0].stage(), c);
    EXPECT_EQ(annotated[0].objectCount(), 3);  // o1 (two rows), o2 and hidden
    EXPECT_EQ(annotated[0].layer(), cells);
    EXPECT_FALSE(annotated[0].label().isValid());
    EXPECT_EQ(annotated[0].boundingBox(), (Rect64{0, 0, 104, 104}));

    // A batch of repeats inserts nothing and appends no event.
    w->assertLabel(o1, LabelId(7));
    w->assertMeasurement(o1, "ki67", 1.0, MeasurementUnit::Dimensionless);
    w->flush();
    EXPECT_EQ(eventsOfKind(store(), EventKind::ObjectsAnnotated).size(), 1u);

    // Objects of two layers: an invalid layer.
    w->assertLabel(o2, LabelId(5));
    w->assertLabel(n, LabelId(5));
    w->flush();
    annotated = eventsOfKind(store(), EventKind::ObjectsAnnotated);
    ASSERT_EQ(annotated.size(), 2u);
    EXPECT_EQ(annotated[1].objectCount(), 2);
    EXPECT_FALSE(annotated[1].layer().isValid());
    EXPECT_EQ(annotated[1].boundingBox(), (Rect64{10, 20, 32, 22}));

    // Another stage's annotation survives c's discard.
    AnnotationWriterPtr wd = annotations().writerForStage(d);
    wd->assertLabel(o2, LabelId(9));
    wd->flush();
    complete(d);

    annotations().discardAnnotationsOfStage(c);
    EXPECT_EQ(annotations().labelCount(o1), 1);
    EXPECT_EQ(annotations().labelCount(o2), 1);
    EXPECT_EQ(annotations().labelAt(o2, 0), LabelId(9));
    EXPECT_EQ(annotations().assertedMeasurementCount(o1), 0);
    EXPECT_EQ(annotations().labelAnnotationCount(), 1);
    EXPECT_EQ(annotations().measurementAnnotationCount(), 0);
    ObjectRecord record;
    ASSERT_TRUE(store().objects().get(o1, record));
    EXPECT_EQ(record.label(), LabelId(3));
    EXPECT_TRUE(store().objects().get(o2, record));
    EXPECT_TRUE(store().objects().get(n, record));

    const std::vector<Event> discarded = eventsOfKind(store(), EventKind::StageDiscarded);
    ASSERT_EQ(discarded.size(), 1u);
    EXPECT_EQ(discarded[0].stage(), c);
    EXPECT_EQ(discarded[0].objectCount(), 0);
    EXPECT_THROW(annotations().discardAnnotationsOfStage(StageId(99)), InvalidArgumentError);
}

TEST_F(AnnotationStoreTest, WriterBatchingAndLifecycle) {
    const std::vector<ObjectId> ids = commit(a, {rec(cells, 0, 0, 4), rec(cells, 10, 0, 4)});
    EXPECT_THROW(annotations().writerForStage(StageId(99)), InvalidArgumentError);

    AnnotationWriterPtr w = annotations().writerForStage(c);
    EXPECT_THROW(w->setBatchSizeAnnotations(0), InvalidArgumentError);
    EXPECT_THROW(w->setBatchSizeAnnotations(-2), InvalidArgumentError);
    w->setBatchSizeAnnotations(2);
    EXPECT_EQ(w->batchSizeAnnotations(), 2);
    w->assertLabel(ids[0], LabelId(2));
    EXPECT_EQ(annotations().labelAnnotationCount(), 0);
    w->assertMeasurement(ids[1], "ki67", 1.0, MeasurementUnit::Dimensionless);  // reaches the batch size
    EXPECT_EQ(annotations().labelAnnotationCount(), 1);
    EXPECT_EQ(annotations().measurementAnnotationCount(), 1);

    w->assertLabel(ids[1], LabelId(2));  // pending at close
    store().close();
    EXPECT_THROW(w->assertLabel(ids[0], LabelId(4)), IoError);
    EXPECT_THROW(annotations().labelAnnotationCount(), IoError);

    StorePtr reopened = reopen(OpenMode::ReadOnly);
    EXPECT_EQ(reopened->annotations().labelAnnotationCount(), 2);
    EXPECT_THROW(reopened->annotations().writerForStage(c), IoError);
    EXPECT_THROW(reopened->annotations().discardAnnotationsOfStage(c), IoError);
}

namespace {

// Tracked on the gate after the writer and in the writer's flush phase, so close() calls it
// once the writer is flushed and before the store is closed: it asserts into the writer in
// that window.
class LateAnnotator final : public detail::GateClient {
public:
    LateAnnotator(AnnotationWriter& writer, ObjectId object) : writer_(writer), object_(object) {}
    void flushBeforeClose() override {
        try {
            writer_.assertMeasurement(object_, "late", 1.0, MeasurementUnit::Dimensionless);
        } catch (const IoError& e) {
            refusal = e.what();
        }
    }
    int flushPhase() const noexcept override { return 1; }
    std::string refusal;  // empty when the assert was accepted

private:
    AnnotationWriter& writer_;
    const ObjectId object_;
};

// "m0042" for 42: names sort in assertion order, so a reader's last index is the newest.
std::string measurementName(int i) {
    std::string digits = std::to_string(i);
    return "m" + std::string(4 - digits.size(), '0') + digits;
}

} // namespace

TEST(AnnotationStoreLifetime, ConcurrentAssertsReadsAndCloseFlushes) {
    constexpr int kThreads = 4;
    constexpr int kReaders = 3;
    constexpr int kPerThread = 500;
    StoreFilesGuard files("annclose");
    Configuration config;
    StorePtr store = Store::open(files.utf8().c_str(), OpenMode::CreateNew, config);
    const LayerId cells = store->layers().declareSemantic("cells", store->layers().pixelLayer());
    const StageId a = store->stageStateWriter().declare("a", "");
    const StageId b = store->stageStateWriter().declare("b", "");

    std::vector<ObjectId> objects;
    {
        ObjectWriterPtr writer = store->objects().writerForStage(a);
        for (int t = 0; t < kThreads; ++t) {
            const ObjectRecord r = rec(cells, t * 10, 0, 4);
            writer->emit(r);
            objects.push_back(r.id());
        }
        writer->flush();
    }
    // Both stages complete, so the readers see each batch as it commits.
    store->stageStateWriter().setStatus(a, StageStatus::Complete);
    store->stageStateWriter().setStatus(b, StageStatus::Complete);

    AnnotationWriterPtr writer = store->annotations().writerForStage(b);
    writer->setBatchSizeAnnotations(300);  // 2004 is not a multiple: close() commits a tail
    std::atomic<bool> writing{true};
    // Readers that have read a committed row, or stopped; the writers wait for all of them
    // halfway through, so reads overlap the second half of the writes.
    std::atomic<int> readersReady{0};
    std::vector<std::string> readerFailures(kReaders);
    std::vector<std::thread> readers;
    for (int r = 0; r < kReaders; ++r) {
        readers.emplace_back([&, r] {
            bool ready = false;
            try {
                while (writing.load() && readerFailures[r].empty()) {
                    for (ObjectId object : objects) {
                        const int32_t count = store->annotations().assertedMeasurementCount(object);
                        if (count == 0) continue;
                        // Rows only accumulate, so the last index read above stays in range.
                        const Measurement m = store->annotations().assertedMeasurementAt(object, count - 1);
                        if (std::string(m.name()) != measurementName(static_cast<int>(m.value()))) {
                            readerFailures[r] = std::string("mismatched row ") + m.name();
                        }
                        if (!ready) {
                            ready = true;
                            ++readersReady;
                        }
                    }
                }
            } catch (const std::exception& e) {
                readerFailures[r] = e.what();
            }
            if (!ready) ++readersReady;
        });
    }
    std::vector<std::thread> writers;
    for (int t = 0; t < kThreads; ++t) {
        writers.emplace_back([&, t] {
            writer->assertLabel(objects[t], LabelId(2));
            for (int i = 0; i < kPerThread; ++i) {
                // Once every writer waits here, 1004 calls have committed 3 batches, so
                // every reader finds rows and the wait ends.
                if (i == kPerThread / 2) {
                    while (readersReady.load() < kReaders) std::this_thread::yield();
                }
                writer->assertMeasurement(objects[t], measurementName(i).c_str(), i, MeasurementUnit::Dimensionless);
            }
        });
    }
    for (std::thread& thread : writers) thread.join();
    writing.store(false);
    for (std::thread& thread : readers) thread.join();
    for (const std::string& failure : readerFailures) EXPECT_EQ(failure, "");
    constexpr int64_t kAsserted = kThreads * (kPerThread + 1);
    EXPECT_EQ(writer->assertedCount(), kAsserted);
    EXPECT_LT(store->annotations().measurementAnnotationCount() + store->annotations().labelAnnotationCount(),
              kAsserted);  // a tail is pending

    const auto late = std::make_shared<LateAnnotator>(*writer, objects[0]);
    detail::trackForCloseTesting(*store, late);
    store->close();
    EXPECT_NE(late->refusal.find("store is closed"), std::string::npos) << "refusal: '" << late->refusal << "'";
    EXPECT_EQ(writer->assertedCount(), kAsserted);
    EXPECT_THROW(writer->assertLabel(objects[0], LabelId(2)), IoError);
    writer.reset();
    store.reset();

    store = Store::open(files.utf8().c_str(), OpenMode::ReadOnly, config);
    EXPECT_EQ(store->annotations().labelAnnotationCount(), kThreads);
    EXPECT_EQ(store->annotations().measurementAnnotationCount(), kThreads * kPerThread);
    for (ObjectId object : objects) {
        EXPECT_EQ(store->annotations().assertedMeasurementCount(object), kPerThread);
        EXPECT_EQ(store->annotations().labelCount(object), 1);
    }
    EXPECT_THROW(store->annotations().writerForStage(b), IoError);
}
