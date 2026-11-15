// Requirements: UR-RES-5, UR-RES-14, UR-RES-16, UR-STG-4, UR-STG-6, UR-STG-12, UR-OBS-2
#include "storage/detail/store_impl.hpp"
#include "storage/store_fixture.hpp"

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/core/object_record.hpp>
#include <slideio/processor/storage/edge_store.hpp>
#include <slideio/processor/storage/event_journal.hpp>
#include <slideio/processor/storage/object_store.hpp>
#include <slideio/processor/storage/store.hpp>

#include <gtest/gtest.h>

#include <initializer_list>
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

std::vector<Event> eventsOfKind(Store& store, EventKind kind) {
    std::vector<Event> events;
    EventCursorPtr cursor = store.events().subscribe(EventJournal::kFromBeginning);
    while (cursor->next(0)) {
        if (cursor->current().kind() == kind) events.push_back(cursor->current());
    }
    return events;
}

std::vector<ObjectId> idsOf(const CursorPtr& cursor) {
    std::vector<ObjectId> ids;
    while (cursor->next()) ids.push_back(cursor->current().id());
    return ids;
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

// Layers: tissue on the pixel layer, cells above tissue, nuclei above cells, vessels on
// another branch above tissue, and the primitive layer sp.
class EdgeStoreTest : public StoreFixture {
protected:
    void SetUp() override {
        StoreFixture::SetUp();
        LayerRegistry& layers = store().layers();
        tissue = layers.declareSemantic("tissue", layers.pixelLayer());
        cells = layers.declareSemantic("cells", tissue);
        nuclei = layers.declareSemantic("nuclei", cells);
        vessels = layers.declareSemantic("vessels", tissue);
        sp = layers.declarePrimitive("sp");
        a = store().stageStateWriter().declare("stage-a", "");
        b = store().stageStateWriter().declare("stage-b", "");
        c = store().stageStateWriter().declare("stage-c", "");
    }
    EdgeStore& edges() { return store().edges(); }
    void complete(StageId stage) { store().stageStateWriter().setStatus(stage, StageStatus::Complete); }
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

    LayerId tissue, cells, nuclei, vessels, sp;
    StageId a, b, c;
};

} // namespace

TEST_F(EdgeStoreTest, NucleusAttachesToTissueSkippingCells) {
    const std::vector<ObjectId> ids = commit(a, {rec(nuclei, 10, 10, 4), rec(tissue, 0, 0, 100)});
    const ObjectId n = ids[0], t = ids[1];
    complete(a);

    EdgeWriterPtr w = edges().writerForStage(b);
    EXPECT_EQ(w->stage(), b);
    EXPECT_EQ(w->batchSizeEdges(), config().objectBatchSize());
    w->assertParent(n, t);
    EXPECT_EQ(w->assertedCount(), 1);
    w->flush();
    complete(b);

    ObjectId parent;
    ASSERT_TRUE(edges().parentOf(n, parent));
    EXPECT_EQ(parent, t);
    EXPECT_TRUE(edges().isAttached(n));
    EXPECT_FALSE(edges().isAttached(t));
    EXPECT_EQ(idsOf(edges().childrenOf(t)), std::vector<ObjectId>{n});
    EXPECT_TRUE(idsOf(edges().childrenOf(n)).empty());
    EXPECT_EQ(edges().parentEdgeCount(), 1);
    EXPECT_EQ(edges().derivedFromEdgeCount(), 0);

    EXPECT_THROW(edges().writerForStage(StageId(99)), InvalidArgumentError);
    EXPECT_THROW(w->setBatchSizeEdges(0), InvalidArgumentError);
    EXPECT_THROW(w->setBatchSizeEdges(-3), InvalidArgumentError);
    w->setBatchSizeEdges(7);
    EXPECT_EQ(w->batchSizeEdges(), 7);
}

TEST_F(EdgeStoreTest, HierarchyViolationsNameBothLayers) {
    const ObjectId cell = rec(cells, 0, 0, 4).id();
    const ObjectId cell2 = rec(cells, 10, 0, 4).id();
    const ObjectId nucleus = rec(nuclei, 1, 1, 2).id();
    const ObjectId vessel = rec(vessels, 20, 0, 4).id();
    const ObjectId primitive = rec(sp, 30, 0, 4).id();
    const ObjectId region = rec(tissue, 0, 0, 100).id();
    EdgeWriterPtr w = edges().writerForStage(b);

    struct Case {
        ObjectId child, parent;
        const char* childLayer;
        const char* parentLayer;
    };
    for (const Case& k : {Case{cell, cell2, "cells", "cells"}, Case{cell, nucleus, "cells", "nuclei"},
                          Case{cell, vessel, "cells", "vessels"}, Case{cell, primitive, "cells", "sp"},
                          Case{primitive, region, "sp", "tissue"}}) {
        const std::string message = invalidArgumentMessage([&] { w->assertParent(k.child, k.parent); });
        EXPECT_TRUE(contains(message, k.childLayer)) << message;
        EXPECT_TRUE(contains(message, k.parentLayer)) << message;
    }
    EXPECT_THROW(w->assertParent(ObjectId(), region), InvalidArgumentError);
    EXPECT_THROW(w->assertParent(cell, ObjectId()), InvalidArgumentError);

    const std::string derived = invalidArgumentMessage([&] { w->assertDerivedFrom(region, cell); });
    EXPECT_TRUE(contains(derived, "tissue")) << derived;
    EXPECT_TRUE(contains(derived, "cells")) << derived;
    const std::string reversed = invalidArgumentMessage([&] { w->assertDerivedFrom(primitive, region); });
    EXPECT_TRUE(contains(reversed, "sp")) << reversed;
    EXPECT_TRUE(contains(reversed, "tissue")) << reversed;
    EXPECT_THROW(w->assertDerivedFrom(ObjectId(), primitive), InvalidArgumentError);
    EXPECT_THROW(w->assertDerivedFrom(region, ObjectId()), InvalidArgumentError);

    EXPECT_EQ(w->assertedCount(), 0);
    w->assertParent(cell, region);  // a valid edge is still accepted
    w->assertDerivedFrom(region, primitive);
    EXPECT_EQ(w->assertedCount(), 2);
}

// An endpoint in the pixel layer is refused at the call, so it never fails a batch at flush.
TEST_F(EdgeStoreTest, PixelLayerEndpointIsRefusedAtTheCall) {
    const std::vector<ObjectId> ids = commit(a, {rec(tissue, 0, 0, 100), rec(cells, 10, 10, 4), rec(sp, 30, 0, 4)});
    const ObjectId region = ids[0], cell = ids[1], primitive = ids[2];
    complete(a);
    const ObjectId pixelObject = rec(store().layers().pixelLayer(), 0, 0, 200).id();
    EdgeWriterPtr w = edges().writerForStage(b);
    w->assertParent(cell, region);

    const std::string parentMessage = invalidArgumentMessage([&] { w->assertParent(region, pixelObject); });
    EXPECT_TRUE(contains(parentMessage, "tissue")) << parentMessage;
    EXPECT_TRUE(contains(parentMessage, "pixel")) << parentMessage;
    const std::string childMessage = invalidArgumentMessage([&] { w->assertParent(pixelObject, region); });
    EXPECT_TRUE(contains(childMessage, "tissue")) << childMessage;
    EXPECT_TRUE(contains(childMessage, "pixel")) << childMessage;
    const std::string derivedMessage = invalidArgumentMessage([&] { w->assertDerivedFrom(pixelObject, primitive); });
    EXPECT_TRUE(contains(derivedMessage, "pixel")) << derivedMessage;
    EXPECT_TRUE(contains(derivedMessage, "sp")) << derivedMessage;
    EXPECT_EQ(w->assertedCount(), 1);

    w->flush();  // the rest of the batch commits
    complete(b);
    EXPECT_EQ(edges().parentEdgeCount(), 1);
    EXPECT_EQ(edges().derivedFromEdgeCount(), 0);
    EXPECT_TRUE(edges().isAttached(cell));
}


// Review Focus 1: an edge naming an object still pending in an unflushed ObjectWriter.
TEST_F(EdgeStoreTest, FlushRejectsMissingEndpointWholeBatch) {
    const std::vector<ObjectId> ids = commit(a, {rec(tissue, 0, 0, 100), rec(cells, 20, 20, 4)});
    const ObjectId t = ids[0], c2 = ids[1];
    ObjectWriterPtr objects = store().objects().writerForStage(a);
    const ObjectRecord pendingCell = rec(cells, 10, 10, 4);
    objects->emit(pendingCell);
    const ObjectId c1 = pendingCell.id();

    EdgeWriterPtr w = edges().writerForStage(b);
    w->assertParent(c2, t);
    w->assertParent(c1, t);
    EXPECT_EQ(w->assertedCount(), 2);
    const std::string message = invalidArgumentMessage([&] { w->flush(); });
    EXPECT_TRUE(contains(message, "EdgeWriter::flush: child cells/")) << message;
    EXPECT_TRUE(contains(message, "does not exist")) << message;
    EXPECT_EQ(edges().parentEdgeCount(), 0);  // c2's edge, inserted first, rolled back with c1's
    EXPECT_EQ(w->assertedCount(), 0);         // the batch was dropped
    EXPECT_TRUE(eventsOfKind(store(), EventKind::EdgesAsserted).empty());
    w->flush();  // nothing pending any more
    EXPECT_EQ(edges().parentEdgeCount(), 0);

    objects->flush();
    w->assertParent(c1, t);
    w->assertParent(c2, t);
    w->flush();
    EXPECT_EQ(edges().parentEdgeCount(), 2);
    EXPECT_EQ(w->assertedCount(), 2);

    // A missing parent and a missing primitive are named in their roles.
    const ObjectId ghostRegion = rec(tissue, 500, 500, 10).id();
    w->assertParent(rec(nuclei, 21, 21, 1).id(), t);
    EXPECT_TRUE(contains(invalidArgumentMessage([&] { w->flush(); }), "child nuclei/"));
    w->assertParent(c1, ghostRegion);  // c1 already attached; the missing parent is found first
    EXPECT_TRUE(contains(invalidArgumentMessage([&] { w->flush(); }), "parent tissue/"));
    w->assertDerivedFrom(ghostRegion, rec(sp, 0, 0, 1).id());
    EXPECT_TRUE(contains(invalidArgumentMessage([&] { w->flush(); }), "derived tissue/"));
    w->assertDerivedFrom(t, rec(sp, 0, 0, 1).id());
    EXPECT_TRUE(contains(invalidArgumentMessage([&] { w->flush(); }), "primitive sp/"));
    EXPECT_EQ(edges().parentEdgeCount(), 2);
    EXPECT_EQ(edges().derivedFromEdgeCount(), 0);
}

// Rows a batch inserted before its failing edge are rolled back with it.
TEST_F(EdgeStoreTest, FailingBatchRollsBackRowsAlreadyInserted) {
    const std::vector<ObjectId> ids =
        commit(a, {rec(tissue, 0, 0, 100), rec(tissue, 200, 0, 100), rec(nuclei, 10, 10, 4), rec(nuclei, 30, 10, 4),
                   rec(sp, 50, 50, 4)});
    const ObjectId t = ids[0], t2 = ids[1], n1 = ids[2], n2 = ids[3], s = ids[4];

    // Valid parent and derived-from edges first, then a derived-from row whose primitive is missing.
    EdgeWriterPtr w = edges().writerForStage(b);
    w->assertParent(n1, t);
    w->assertParent(n2, t);
    w->assertDerivedFrom(t, s);
    w->assertDerivedFrom(t2, rec(sp, 90, 90, 4).id());
    EXPECT_TRUE(contains(invalidArgumentMessage([&] { w->flush(); }), "primitive sp/"));
    EXPECT_EQ(edges().parentEdgeCount(), 0);
    EXPECT_EQ(edges().derivedFromEdgeCount(), 0);
    EXPECT_TRUE(eventsOfKind(store(), EventKind::EdgesAsserted).empty());
    EXPECT_EQ(w->assertedCount(), 0);

    w->assertParent(n1, t);
    w->flush();
    EXPECT_EQ(edges().parentEdgeCount(), 1);
    ASSERT_EQ(eventsOfKind(store(), EventKind::EdgesAsserted).size(), 1u);

    // A new, valid edge first, then one conflicting with the committed (n1, t) by stage-b.
    EdgeWriterPtr other = edges().writerForStage(c);
    other->assertParent(n2, t2);
    other->assertDerivedFrom(t2, s);
    other->assertParent(n1, t2);
    const std::string message = invalidArgumentMessage([&] { other->flush(); });
    EXPECT_TRUE(contains(message, "stage-b")) << message;
    EXPECT_EQ(edges().parentEdgeCount(), 1);
    EXPECT_EQ(edges().derivedFromEdgeCount(), 0);
    EXPECT_EQ(eventsOfKind(store(), EventKind::EdgesAsserted).size(), 1u);
    EXPECT_EQ(other->assertedCount(), 0);
    ObjectId parent;
    complete(a);
    complete(b);
    complete(c);
    EXPECT_FALSE(edges().isAttached(n2));
    ASSERT_TRUE(edges().parentOf(n1, parent));
    EXPECT_EQ(parent, t);
}

TEST_F(EdgeStoreTest, OneParentPerChild) {
    const std::vector<ObjectId> ids = commit(a, {rec(nuclei, 10, 10, 4), rec(tissue, 0, 0, 100), rec(tissue, 200, 0, 100)});
    const ObjectId n = ids[0], t = ids[1], t2 = ids[2];

    EdgeWriterPtr w = edges().writerForStage(b);
    w->assertParent(n, t);
    w->assertParent(n, t);  // identical: one row
    EXPECT_EQ(w->assertedCount(), 2);
    const std::string pending = invalidArgumentMessage([&] { w->assertParent(n, t2); });
    EXPECT_TRUE(contains(pending, "nuclei/")) << pending;
    EXPECT_TRUE(contains(pending, "tissue/")) << pending;
    EXPECT_TRUE(contains(pending, "stage-b")) << pending;
    EXPECT_EQ(w->assertedCount(), 2);
    w->flush();
    EXPECT_EQ(edges().parentEdgeCount(), 1);
    const std::vector<Event> first = eventsOfKind(store(), EventKind::EdgesAsserted);
    ASSERT_EQ(first.size(), 1u);
    EXPECT_EQ(first[0].objectCount(), 1);

    // The identical edge from the same stage, in a later batch, is a no-op with no event.
    EdgeWriterPtr again = edges().writerForStage(b);
    again->assertParent(n, t);
    again->flush();
    EXPECT_EQ(edges().parentEdgeCount(), 1);
    EXPECT_EQ(eventsOfKind(store(), EventKind::EdgesAsserted).size(), 1u);

    // The same edge from another stage conflicts with the committed one.
    EdgeWriterPtr other = edges().writerForStage(c);
    other->assertParent(n, t);
    const std::string committed = invalidArgumentMessage([&] { other->flush(); });
    EXPECT_TRUE(contains(committed, "nuclei/")) << committed;
    EXPECT_TRUE(contains(committed, "tissue/")) << committed;
    EXPECT_TRUE(contains(committed, "stage-b")) << committed;
    other->assertParent(n, t2);
    EXPECT_THROW(other->flush(), InvalidArgumentError);
    EXPECT_EQ(edges().parentEdgeCount(), 1);
}

// UR-RES-16 acceptance: superpixel s2 straddles two tissue regions.
TEST_F(EdgeStoreTest, DerivedFromIsManyToManyBothWays) {
    const std::vector<ObjectId> ids = commit(a, {rec(tissue, 0, 0, 50), rec(tissue, 50, 0, 50), rec(sp, 10, 0, 5),
                                                 rec(sp, 45, 0, 10), rec(sp, 80, 0, 5)});
    const ObjectId t1 = ids[0], t2 = ids[1], s1 = ids[2], s2 = ids[3], s3 = ids[4];
    complete(a);

    EdgeWriterPtr w = edges().writerForStage(b);
    w->assertDerivedFrom(t1, s2);  // out of order: reads sort
    w->assertDerivedFrom(t1, s1);
    w->assertDerivedFrom(t2, s2);
    w->assertDerivedFrom(t2, s3);
    w->flush();
    complete(b);

    EXPECT_EQ(idsOf(edges().originsOf(t1)), (std::vector<ObjectId>{s1, s2}));
    EXPECT_EQ(idsOf(edges().originsOf(t2)), (std::vector<ObjectId>{s2, s3}));
    EXPECT_EQ(idsOf(edges().derivedFrom(s2)), (std::vector<ObjectId>{t1, t2}));
    EXPECT_EQ(idsOf(edges().derivedFrom(s1)), std::vector<ObjectId>{t1});
    EXPECT_TRUE(idsOf(edges().originsOf(s1)).empty());
    EXPECT_EQ(edges().derivedFromEdgeCount(), 4);

    EdgeWriterPtr second = edges().writerForStage(c);
    second->assertDerivedFrom(t1, s1);
    second->flush();
    complete(c);
    EXPECT_EQ(edges().derivedFromEdgeCount(), 5);  // two stages, two rows
    EXPECT_EQ(idsOf(edges().originsOf(t1)), (std::vector<ObjectId>{s1, s2}));
    EXPECT_EQ(idsOf(edges().derivedFrom(s1)), std::vector<ObjectId>{t1});
}

TEST_F(EdgeStoreTest, VisibilityFollowsTheAssertingStage) {
    const std::vector<ObjectId> ids = commit(a, {rec(nuclei, 10, 10, 4), rec(tissue, 0, 0, 100), rec(sp, 0, 0, 4)});
    const ObjectId n = ids[0], t = ids[1], s = ids[2];
    complete(a);
    store().stageStateWriter().setStatus(b, StageStatus::Running);

    EdgeWriterPtr w = edges().writerForStage(b);
    w->assertParent(n, t);
    w->assertDerivedFrom(t, s);
    w->flush();

    ObjectId parent;
    EXPECT_FALSE(edges().parentOf(n, parent));
    EXPECT_FALSE(edges().isAttached(n));
    EXPECT_TRUE(idsOf(edges().childrenOf(t)).empty());
    EXPECT_TRUE(idsOf(edges().originsOf(t)).empty());
    EXPECT_TRUE(idsOf(edges().derivedFrom(s)).empty());
    EXPECT_EQ(edges().parentEdgeCount(), 1);
    EXPECT_EQ(edges().derivedFromEdgeCount(), 1);

    complete(b);
    store().stageStateWriter().setStale(b, true);
    ASSERT_TRUE(edges().parentOf(n, parent));
    EXPECT_EQ(parent, t);
    EXPECT_TRUE(edges().isAttached(n));
    EXPECT_EQ(idsOf(edges().childrenOf(t)), std::vector<ObjectId>{n});
    EXPECT_EQ(idsOf(edges().originsOf(t)), std::vector<ObjectId>{s});
    EXPECT_EQ(idsOf(edges().derivedFrom(s)), std::vector<ObjectId>{t});

    // An unknown or invalid child has no parent.
    EXPECT_FALSE(edges().parentOf(rec(nuclei, 90, 90, 4).id(), parent));
    EXPECT_FALSE(edges().parentOf(ObjectId(), parent));
    EXPECT_FALSE(edges().isAttached(ObjectId()));
    EXPECT_TRUE(idsOf(edges().childrenOf(ObjectId())).empty());
}

// Ruling S16: a drop keeps the derived-from rows, which then resolve to nothing, and a
// derived-from edge still pending into the dropped layer fails at flush.
TEST_F(EdgeStoreTest, DroppedPrimitiveKeepsItsDerivedFromRows) {
    const std::vector<ObjectId> ids =
        commit(a, {rec(tissue, 0, 0, 50), rec(sp, 10, 0, 5), rec(sp, 20, 0, 5), rec(sp, 30, 0, 5)});
    const ObjectId t = ids[0], s1 = ids[1], s2 = ids[2], s3 = ids[3];
    complete(a);
    EdgeWriterPtr w = edges().writerForStage(b);
    w->assertDerivedFrom(t, s1);
    w->assertDerivedFrom(t, s2);
    w->flush();
    complete(b);
    EdgeWriterPtr pending = edges().writerForStage(c);
    pending->assertDerivedFrom(t, s3);
    ASSERT_EQ(idsOf(edges().originsOf(t)), (std::vector<ObjectId>{s1, s2}));

    store().layers().dropPrimitive(sp);

    EXPECT_TRUE(idsOf(edges().originsOf(t)).empty());
    EXPECT_TRUE(idsOf(edges().derivedFrom(s1)).empty());  // the primitive no longer exists
    EXPECT_TRUE(idsOf(edges().derivedFrom(s2)).empty());
    EXPECT_EQ(edges().derivedFromEdgeCount(), 2);
    const std::string message = invalidArgumentMessage([&] { pending->flush(); });
    EXPECT_TRUE(contains(message, "primitive sp/")) << message;
    EXPECT_EQ(pending->assertedCount(), 0);
    EXPECT_EQ(edges().derivedFromEdgeCount(), 2);
    EXPECT_TRUE(contains(invalidArgumentMessage([&] { w->assertDerivedFrom(t, s1); }), "'sp' is dropped"));
    ObjectRecord stored;
    EXPECT_TRUE(store().objects().get(t, stored));
}

TEST_F(EdgeStoreTest, BatchEventAndDiscardEdgesOfStage) {
    const std::vector<ObjectId> ids =
        commit(a, {rec(nuclei, 10, 10, 4), rec(nuclei, 30, 12, 4), rec(nuclei, 20, 40, 6), rec(cells, 0, 0, 100),
                   rec(tissue, 0, 0, 200), rec(sp, 0, 0, 4)});
    const ObjectId n1 = ids[0], n2 = ids[1], n3 = ids[2], cell = ids[3], t = ids[4], s = ids[5];

    EdgeWriterPtr w = edges().writerForStage(b);
    w->setBatchSizeEdges(3);
    w->assertParent(n1, cell);
    w->assertParent(n2, cell);
    EXPECT_EQ(edges().parentEdgeCount(), 0);
    EXPECT_TRUE(eventsOfKind(store(), EventKind::EdgesAsserted).empty());
    w->assertParent(n3, cell);  // the third commits
    EXPECT_EQ(edges().parentEdgeCount(), 3);
    std::vector<Event> asserted = eventsOfKind(store(), EventKind::EdgesAsserted);
    ASSERT_EQ(asserted.size(), 1u);
    EXPECT_EQ(asserted[0].stage(), b);
    EXPECT_EQ(asserted[0].objectCount(), 3);
    EXPECT_EQ(asserted[0].layer(), nuclei);
    EXPECT_EQ(asserted[0].boundingBox(), (Rect64{10, 10, 24, 36}));

    // A batch over two layers carries no layer; its box covers the children and the derived.
    w->assertParent(cell, t);
    w->assertDerivedFrom(t, s);
    w->flush();
    asserted = eventsOfKind(store(), EventKind::EdgesAsserted);
    ASSERT_EQ(asserted.size(), 2u);
    EXPECT_EQ(asserted[1].objectCount(), 2);
    EXPECT_FALSE(asserted[1].layer().isValid());
    EXPECT_EQ(asserted[1].boundingBox(), (Rect64{0, 0, 200, 200}));

    const int64_t objectsBefore = store().objects().totalObjectCount();
    edges().discardEdgesOfStage(b);
    EXPECT_EQ(edges().parentEdgeCount(), 0);
    EXPECT_EQ(edges().derivedFromEdgeCount(), 0);
    EXPECT_EQ(store().objects().totalObjectCount(), objectsBefore);
    const std::vector<Event> discarded = eventsOfKind(store(), EventKind::StageDiscarded);
    ASSERT_EQ(discarded.size(), 1u);
    EXPECT_EQ(discarded[0].stage(), b);
    EXPECT_EQ(discarded[0].objectCount(), 0);
    EXPECT_THROW(edges().discardEdgesOfStage(StageId(99)), InvalidArgumentError);
}

namespace {

// Tracked on the gate after the writer, so close() calls it once the writer is flushed and
// before the store is closed: it asserts into the writer in that window.
class LateAsserter final : public detail::GateClient {
public:
    LateAsserter(EdgeWriter& writer, ObjectId derived, ObjectId primitive)
        : writer_(writer), derived_(derived), primitive_(primitive) {}
    void flushBeforeClose() override {
        try {
            writer_.assertDerivedFrom(derived_, primitive_);
        } catch (const IoError& e) {
            refusal = e.what();
        }
    }
    std::string refusal;  // empty when the assert was accepted

private:
    EdgeWriter& writer_;
    const ObjectId derived_;
    const ObjectId primitive_;
};

} // namespace

TEST(EdgeHandleLifetime, ConcurrentAssertsAndCloseFlushes) {
    constexpr int kThreads = 4;
    constexpr int kPerThread = 500;
    StoreFilesGuard files("edgeclose");
    Configuration config;
    StorePtr store = Store::open(files.utf8().c_str(), OpenMode::CreateNew, config);
    const LayerId tissue = store->layers().declareSemantic("tissue", store->layers().pixelLayer());
    const LayerId sp = store->layers().declarePrimitive("sp");
    const StageId a = store->stageStateWriter().declare("a", "");
    const StageId b = store->stageStateWriter().declare("b", "");

    std::vector<ObjectId> regions;
    std::vector<ObjectId> primitives;
    {
        ObjectWriterPtr objects = store->objects().writerForStage(a);
        for (int t = 0; t < kThreads; ++t) {
            const ObjectRecord r = rec(tissue, t * 1000, 0, 1000);
            objects->emit(r);
            regions.push_back(r.id());
        }
        for (int i = 0; i < kPerThread; ++i) {
            const ObjectRecord r = rec(sp, i * 4, 2000, 4);
            objects->emit(r);
            primitives.push_back(r.id());
        }
        objects->flush();
    }

    EdgeWriterPtr writer = store->edges().writerForStage(b);
    writer->setBatchSizeEdges(300);  // 2000 is not a multiple: close() commits a tail
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kPerThread; ++i) writer->assertDerivedFrom(regions[t], primitives[i]);
        });
    }
    for (std::thread& thread : threads) thread.join();
    EXPECT_EQ(writer->assertedCount(), kThreads * kPerThread);
    EXPECT_LT(store->edges().derivedFromEdgeCount(), kThreads * kPerThread);  // a tail is pending

    const auto late = std::make_shared<LateAsserter>(*writer, regions[0], primitives[0]);
    detail::trackForCloseTesting(*store, late);
    store->close();
    EXPECT_NE(late->refusal.find("store is closed"), std::string::npos) << "refusal: '" << late->refusal << "'";
    EXPECT_EQ(writer->assertedCount(), kThreads * kPerThread);
    EXPECT_THROW(writer->assertDerivedFrom(regions[0], primitives[1]), IoError);
    writer.reset();
    store.reset();

    store = Store::open(files.utf8().c_str(), OpenMode::ReadOnly, config);
    EXPECT_EQ(store->edges().derivedFromEdgeCount(), kThreads * kPerThread);
    EXPECT_THROW(store->edges().writerForStage(b), IoError);
    EXPECT_THROW(store->edges().discardEdgesOfStage(b), IoError);
}
