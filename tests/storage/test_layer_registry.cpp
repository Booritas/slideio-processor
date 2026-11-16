// Requirements: UR-STG-10, UR-RES-13, UR-RES-14, UR-STG-12, UR-RES-16, UR-SEG-4, UR-IMG-4
#include "storage/store_fixture.hpp"

#include <slideio/processor/core/block_view.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/core/object_query.hpp>
#include <slideio/processor/core/object_record.hpp>
#include <slideio/processor/storage/annotation_store.hpp>
#include <slideio/processor/storage/edge_store.hpp>
#include <slideio/processor/storage/event_journal.hpp>
#include <slideio/processor/storage/layer_registry.hpp>
#include <slideio/processor/storage/object_store.hpp>
#include <slideio/processor/storage/stage_state_reader.hpp>
#include <slideio/processor/storage/stage_state_writer.hpp>
#include <slideio/processor/storage/store.hpp>

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using namespace slideio::processor;

namespace {

class LayerRegistryTest : public StoreFixture {
protected:
    void complete(StageId stage) { store().stageStateWriter().setStatus(stage, StageStatus::Complete); }

    // Reads the single integer `sql` selects, on a connection of its own; -1 when it cannot.
    int64_t rawCount(const char* sql) {
        sqlite3* db = nullptr;
        int64_t count = -1;
        if (sqlite3_open_v2(pathUtf8().c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
            sqlite3_stmt* select = nullptr;
            if (sqlite3_prepare_v2(db, sql, -1, &select, nullptr) == SQLITE_OK && sqlite3_step(select) == SQLITE_ROW) {
                count = sqlite3_column_int64(select, 0);
            }
            sqlite3_finalize(select);
        }
        sqlite3_close(db);
        return count;
    }
};

std::string messageOf(const std::function<void()>& call) {
    try {
        call();
    } catch (const std::exception& e) {
        return e.what();
    }
    return std::string();
}

bool contains(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

// The square [x, x + side) x [y, y + side) as a record of `layer`.
ObjectRecord square(LayerId layer, int64_t x, int64_t y, int64_t side = 1) {
    const Point64 points[] = {{x, y}, {x + side, y}, {x + side, y + side}, {x, y + side}};
    Polygon polygon;
    polygon.addRing(points, 4, false);
    ObjectRecord record;
    record.setLayer(layer);
    record.setGeometry(polygon);
    return record;
}

std::vector<ObjectId> idsOf(const CursorPtr& cursor) {
    std::vector<ObjectId> ids;
    while (cursor->next()) ids.push_back(cursor->current().id());
    return ids;
}

std::vector<Event> eventsOfKind(Store& store, EventKind kind) {
    std::vector<Event> events;
    EventCursorPtr cursor = store.events().subscribe(EventJournal::kFromBeginning);
    while (cursor->next(0)) {
        if (cursor->current().kind() == kind) events.push_back(cursor->current());
    }
    return events;
}

// Fills a 64x64 view at level 0 over [0, 64)^2 from `source`; true when every byte is 0.
bool fillsBackground(TileRasterSource& source) {
    constexpr int64_t kSide = 64;
    const int64_t pixelBytes = dataTypeSizeBytes(source.planeType());
    std::vector<uint8_t> bytes(static_cast<size_t>(kSide * kSide * pixelBytes), 0xAB);
    BlockPlane plane{bytes.data(), source.planeType(), kSide * pixelBytes, 0};
    BlockView view(kSide, kSide, Rect64{0, 0, kSide, kSide}, 0, &plane, 1);
    source.fillTile(view);
    for (uint8_t b : bytes) {
        if (b != 0) return false;
    }
    return true;
}

}  // namespace

TEST_F(LayerRegistryTest, PixelLayerAndBranchingHierarchy) {
    LayerRegistry& L = store().layers();
    const LayerId px = L.pixelLayer();
    EXPECT_EQ(px, LayerId(1));
    EXPECT_STREQ(L.name(px), "pixel");
    EXPECT_EQ(L.kind(px), LayerKind::Semantic);
    EXPECT_EQ(L.depth(px), 0);
    EXPECT_FALSE(L.parent(px).isValid());
    EXPECT_EQ(L.count(), 1);
    EXPECT_EQ(L.at(0), px);

    const LayerId tissue = L.declareSemantic("tissue", px);
    EXPECT_EQ(tissue, LayerId(2));
    const LayerId vessels = L.declareSemantic("vessels", tissue);
    const LayerId glands = L.declareSemantic("glands", tissue);
    const LayerId cells = L.declareSemantic("cells", glands);
    EXPECT_EQ(L.depth(cells), 3);
    EXPECT_EQ(L.depth(tissue), 1);
    EXPECT_EQ(L.parent(vessels), tissue);
    EXPECT_EQ(L.parent(tissue), px);
    EXPECT_EQ(L.childCount(tissue), 2);
    EXPECT_EQ(L.childAt(tissue, 0), vessels);
    EXPECT_EQ(L.childAt(tissue, 1), glands);
    EXPECT_EQ(L.childCount(px), 1);
    EXPECT_EQ(L.childCount(cells), 0);
    EXPECT_THROW(L.childAt(tissue, 2), InvalidArgumentError);
    EXPECT_THROW(L.childAt(tissue, -1), InvalidArgumentError);
    EXPECT_TRUE(L.isAbove(cells, tissue));
    EXPECT_TRUE(L.isAbove(cells, px));
    EXPECT_TRUE(L.isAbove(cells, glands));
    EXPECT_FALSE(L.isAbove(cells, vessels));
    EXPECT_FALSE(L.isAbove(tissue, tissue));
    EXPECT_FALSE(L.isAbove(tissue, cells));
    EXPECT_FALSE(L.isAbove(vessels, glands));
    EXPECT_FALSE(L.isAbove(px, tissue));

    const LayerId sp = L.declarePrimitive("superpixels");
    EXPECT_EQ(L.kind(sp), LayerKind::Primitive);
    EXPECT_THROW(L.parent(sp), InvalidArgumentError);
    EXPECT_THROW(L.depth(sp), InvalidArgumentError);
    EXPECT_THROW(L.childCount(sp), InvalidArgumentError);
    EXPECT_THROW(L.childAt(sp, 0), InvalidArgumentError);
    EXPECT_THROW(L.isAbove(sp, px), InvalidArgumentError);
    EXPECT_THROW(L.isAbove(px, sp), InvalidArgumentError);
    EXPECT_THROW(L.isAbove(cells, LayerId(99)), InvalidArgumentError);
    EXPECT_THROW(L.declareSemantic("x", sp), InvalidArgumentError);
    EXPECT_THROW(L.declareSemantic("x", LayerId(99)), InvalidArgumentError);
    EXPECT_THROW(L.declareSemantic("x", LayerId()), InvalidArgumentError);
    EXPECT_EQ(L.count(), 6);  // the refused declarations added nothing
    EXPECT_EQ(L.childCount(px), 1);
}

TEST_F(LayerRegistryTest, NamesAreUniqueAndValidated) {
    LayerRegistry& L = store().layers();
    const LayerId px = L.pixelLayer();
    const LayerId tissue = L.declareSemantic("tissue", px);
    const LayerId sp = L.declarePrimitive("superpixels");

    for (const char* bad : {"tissue", "superpixels", "pixel", ""}) {
        EXPECT_THROW(L.declareSemantic(bad, px), InvalidArgumentError) << bad;
        EXPECT_THROW(L.declarePrimitive(bad), InvalidArgumentError) << bad;
    }
    EXPECT_THROW(L.declareSemantic(nullptr, px), InvalidArgumentError);
    EXPECT_THROW(L.declarePrimitive(nullptr), InvalidArgumentError);
    EXPECT_NE(messageOf([&] { L.declareSemantic("tissue", px); }).find("tissue"), std::string::npos);
    EXPECT_NE(messageOf([&] { L.declarePrimitive("pixel"); }).find("pixel"), std::string::npos);
    EXPECT_NO_THROW(L.declarePrimitive("Tissue"));  // exact comparison, no case folding

    LayerId found;
    EXPECT_TRUE(L.find("tissue", found));
    EXPECT_EQ(found, tissue);
    EXPECT_TRUE(L.find("pixel", found));
    EXPECT_EQ(found, L.pixelLayer());
    EXPECT_TRUE(L.find("superpixels", found));
    EXPECT_EQ(found, sp);
    found = LayerId(77);
    EXPECT_FALSE(L.find("TISSUE ", found));
    EXPECT_FALSE(L.find("", found));
    EXPECT_FALSE(L.find(nullptr, found));
    EXPECT_EQ(found, LayerId(77));  // unchanged on failure

    EXPECT_EQ(L.count(), 4);
    EXPECT_EQ(L.at(2), sp);
    EXPECT_THROW(L.at(L.count()), InvalidArgumentError);
    EXPECT_THROW(L.at(-1), InvalidArgumentError);
    EXPECT_TRUE(L.exists(tissue));
    EXPECT_FALSE(L.exists(LayerId(99)));
    EXPECT_FALSE(L.exists(LayerId()));
    EXPECT_THROW(L.name(LayerId(99)), InvalidArgumentError);
    EXPECT_THROW(L.kind(LayerId(99)), InvalidArgumentError);
    EXPECT_THROW(L.parent(LayerId(99)), InvalidArgumentError);
    EXPECT_THROW(L.depth(LayerId()), InvalidArgumentError);
}

TEST_F(LayerRegistryTest, NamePointersStayValidAsLayersAreAdded) {
    LayerRegistry& L = store().layers();
    const LayerId first = L.declarePrimitive("first");
    const char* name = L.name(first);
    for (int i = 0; i < 200; ++i) L.declarePrimitive(("layer" + std::to_string(i)).c_str());
    EXPECT_EQ(L.count(), 202);
    EXPECT_STREQ(name, "first");
    EXPECT_EQ(L.name(first), name);
}

TEST_F(LayerRegistryTest, DeclarationsAreJournalledAndSurviveReopen) {
    LayerId tissue;
    LayerId sp;
    {
        LayerRegistry& L = store().layers();
        EventCursorPtr cursor = store().events().subscribe(EventJournal::kFromBeginning);
        EXPECT_FALSE(cursor->next(0));  // the pixel layer is not an event
        tissue = L.declareSemantic("tissue", L.pixelLayer());
        sp = L.declarePrimitive("superpixels");
        EXPECT_THROW(L.declarePrimitive("tissue"), InvalidArgumentError);

        const LayerId expected[] = {tissue, sp};
        for (LayerId layer : expected) {
            ASSERT_TRUE(cursor->next(0));
            EXPECT_EQ(cursor->current().kind(), EventKind::LayerDeclared);
            EXPECT_EQ(cursor->current().layer(), layer);
            EXPECT_EQ(cursor->current().objectCount(), 0);
            EXPECT_TRUE(cursor->current().boundingBox() == (Rect64{0, 0, 0, 0}));
            EXPECT_FALSE(cursor->current().stage().isValid());
        }
        EXPECT_FALSE(cursor->next(0));  // the refused declaration appended nothing
    }

    StorePtr ro = reopen(OpenMode::ReadOnly);
    const LayerRegistry& L = ro->layers();
    EXPECT_EQ(L.count(), 3);
    EXPECT_STREQ(L.name(tissue), "tissue");
    EXPECT_EQ(L.kind(tissue), LayerKind::Semantic);
    EXPECT_EQ(L.parent(tissue), L.pixelLayer());
    EXPECT_EQ(L.depth(tissue), 1);
    EXPECT_EQ(L.childAt(L.pixelLayer(), 0), tissue);
    EXPECT_STREQ(L.name(sp), "superpixels");
    EXPECT_EQ(L.kind(sp), LayerKind::Primitive);
    LayerId found;
    EXPECT_TRUE(L.find("superpixels", found));
    EXPECT_EQ(found, sp);
}

TEST_F(LayerRegistryTest, ReadOnlyStoreSeesLayersDeclaredLater) {
    LayerRegistry& writer = store().layers();
    const LayerId tissue = writer.declareSemantic("tissue", writer.pixelLayer());
    // A second handle on the same file, as a viewer process would hold.
    StorePtr viewer = Store::open(pathUtf8().c_str(), OpenMode::ReadOnly, config());
    const LayerRegistry& L = viewer->layers();
    EXPECT_EQ(L.count(), 2);
    EXPECT_EQ(L.childCount(L.pixelLayer()), 1);

    const LayerId cells = writer.declareSemantic("cells", tissue);
    const LayerId sp = writer.declarePrimitive("superpixels");
    EXPECT_EQ(L.count(), 4);
    EXPECT_TRUE(L.exists(cells));
    EXPECT_STREQ(L.name(cells), "cells");
    EXPECT_EQ(L.childCount(tissue), 1);
    EXPECT_EQ(L.childAt(tissue, 0), cells);
    EXPECT_TRUE(L.isAbove(cells, L.pixelLayer()));
    EXPECT_EQ(L.kind(sp), LayerKind::Primitive);
    LayerId found;
    EXPECT_TRUE(L.find("cells", found));
    EXPECT_EQ(found, cells);

    writer.recordAnalysisLevel(cells, 3);
    EXPECT_EQ(L.coarsestAnalysisLevel(cells), 3);  // mutable columns are never stale

    EXPECT_THROW(viewer->layers().declarePrimitive("x"), IoError);
    EXPECT_THROW(viewer->layers().declareSemantic("x", tissue), IoError);
    EXPECT_THROW(viewer->layers().recordAnalysisLevel(cells, 5), IoError);
    EXPECT_NE(messageOf([&] { viewer->layers().declarePrimitive("x"); }).find("read-only"), std::string::npos);
    EXPECT_EQ(L.count(), 4);
    viewer->close();
}

TEST_F(LayerRegistryTest, AnalysisLevelIsTheMaximum) {
    LayerRegistry& L = store().layers();
    const LayerId cells = L.declareSemantic("cells", L.pixelLayer());
    EXPECT_EQ(L.coarsestAnalysisLevel(cells), 0);
    L.recordAnalysisLevel(cells, 2);
    EXPECT_EQ(L.coarsestAnalysisLevel(cells), 2);
    L.recordAnalysisLevel(cells, 4);
    EXPECT_EQ(L.coarsestAnalysisLevel(cells), 4);
    L.recordAnalysisLevel(cells, 1);
    EXPECT_EQ(L.coarsestAnalysisLevel(cells), 4);
    L.recordAnalysisLevel(cells, 0);
    EXPECT_EQ(L.coarsestAnalysisLevel(cells), 4);
    EXPECT_THROW(L.recordAnalysisLevel(cells, -1), InvalidArgumentError);
    EXPECT_EQ(L.coarsestAnalysisLevel(cells), 4);
    EXPECT_EQ(L.coarsestAnalysisLevel(LayerId(99)), 0);
    EXPECT_THROW(L.recordAnalysisLevel(LayerId(99), 1), InvalidArgumentError);
    EXPECT_THROW(L.recordAnalysisLevel(LayerId(), 1), InvalidArgumentError);
    EXPECT_EQ(L.coarsestAnalysisLevel(L.pixelLayer()), 0);
}

TEST_F(LayerRegistryTest, WriterStagesComeFromStageDeclarations) {
    LayerRegistry& L = store().layers();
    StageStateWriter& w = store().stageStateWriter();
    const LayerId cells = L.declareSemantic("cells", L.pixelLayer());
    const LayerId nuclei = L.declareSemantic("nuclei", cells);
    EXPECT_EQ(L.writerStageCount(cells), 0);
    EXPECT_THROW(L.writerStageAt(cells, 0), InvalidArgumentError);

    const StageId a = w.declare("a", "");
    const StageId b = w.declare("b", "");
    w.addWrittenLayer(b, cells);
    w.addWrittenLayer(a, cells);
    w.addWrittenLayer(a, cells);  // repeating changes nothing
    EXPECT_EQ(L.writerStageCount(cells), 2);
    EXPECT_EQ(L.writerStageAt(cells, 0), a);
    EXPECT_EQ(L.writerStageAt(cells, 1), b);
    EXPECT_THROW(L.writerStageAt(cells, 2), InvalidArgumentError);
    EXPECT_THROW(L.writerStageAt(cells, -1), InvalidArgumentError);
    EXPECT_EQ(L.writerStageCount(nuclei), 0);

    EXPECT_THROW(w.addWrittenLayer(a, L.pixelLayer()), InvalidArgumentError);
    EXPECT_THROW(L.writerStageCount(LayerId(99)), InvalidArgumentError);
    EXPECT_THROW(L.writerStageAt(LayerId(99), 0), InvalidArgumentError);
}

TEST_F(LayerRegistryTest, IsDroppedReadsItsColumn) {
    LayerRegistry& L = store().layers();
    const LayerId cells = L.declareSemantic("cells", L.pixelLayer());
    EXPECT_FALSE(L.isDropped(cells));
    EXPECT_FALSE(L.isDropped(LayerId(99)));
    EXPECT_FALSE(L.isDropped(L.pixelLayer()));
}

TEST_F(LayerRegistryTest, ClosedStore) {
    LayerRegistry& L = store().layers();
    const LayerId cells = L.declareSemantic("cells", L.pixelLayer());
    store().close();
    EXPECT_EQ(L.pixelLayer(), LayerId(1));  // answers without the store
    EXPECT_EQ(L.count(), 0);
    LayerId found;
    EXPECT_FALSE(L.find("cells", found));
    EXPECT_FALSE(L.exists(cells));
    EXPECT_FALSE(L.isDropped(cells));
    EXPECT_EQ(L.coarsestAnalysisLevel(cells), 0);
    EXPECT_THROW(L.declarePrimitive("x"), IoError);
    EXPECT_THROW(L.declareSemantic("x", L.pixelLayer()), IoError);
    EXPECT_THROW(L.at(0), IoError);
    EXPECT_THROW(L.name(cells), IoError);
    EXPECT_THROW(L.kind(cells), IoError);
    EXPECT_THROW(L.parent(cells), IoError);
    EXPECT_THROW(L.isAbove(cells, L.pixelLayer()), IoError);
    EXPECT_THROW(L.writerStageCount(cells), IoError);
    EXPECT_THROW(L.recordAnalysisLevel(cells, 1), IoError);
    EXPECT_NE(messageOf([&] { L.name(cells); }).find("store is closed"), std::string::npos);
}

TEST_F(LayerRegistryTest, ConcurrentDeclarationsAndReads) {
    LayerRegistry& L = store().layers();
    constexpr int kThreads = 4;
    constexpr int kEach = 25;
    std::atomic<bool> go{false};
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            while (!go.load()) std::this_thread::yield();
            try {
                for (int i = 0; i < kEach; ++i) {
                    const std::string name = "t" + std::to_string(t) + "_" + std::to_string(i);
                    const LayerId id = (i % 2 == 0) ? L.declarePrimitive(name.c_str())
                                                    : L.declareSemantic(name.c_str(), L.pixelLayer());
                    if (!L.exists(id) || name != L.name(id)) ++failures;
                    L.count();
                }
            } catch (...) {
                ++failures;
            }
        });
    }
    go = true;
    for (std::thread& th : threads) th.join();
    EXPECT_EQ(failures.load(), 0);
    EXPECT_EQ(L.count(), 1 + kThreads * kEach);
    EXPECT_EQ(L.childCount(L.pixelLayer()), kThreads * (kEach / 2));
}

TEST_F(LayerRegistryTest, DropPrimitiveKeepsDerivedObjects) {  // UR-STG-12 / UR-RES-16 acceptance
    LayerRegistry& L = store().layers();
    StageStateWriter& states = store().stageStateWriter();
    const LayerId tissue = L.declareSemantic("tissue", L.pixelLayer());
    const LayerId sp = L.declarePrimitive("sp");
    const StageId seg = states.declare("superpixels", "");
    const StageId tis = states.declare("tissue", "");
    const StageId link = states.declare("link", "");
    const StageId note = states.declare("note", "");
    EXPECT_FALSE(L.wasEverPopulated(sp));

    // 50,000 labelled unit squares on a 250 x 200 grid, each with one measurement.
    constexpr int64_t kObjects = 50000;
    constexpr int64_t kColumns = 250;
    std::vector<ObjectId> s;
    {
        ObjectWriterPtr w = store().objects().writerForStage(seg);
        w->setBatchSizeObjects(10000);
        for (int64_t i = 0; i < kObjects; ++i) {
            ObjectRecord r = square(sp, i % kColumns, i / kColumns);
            r.setLabel(LabelId(3));
            r.measurements().set("area", 1.0, MeasurementUnit::SquarePixels);
            w->emit(r);
            s.push_back(r.id());
        }
        w->flush();
    }
    ObjectWriterPtr tw = store().objects().writerForStage(tis);
    const ObjectRecord r1 = square(tissue, 0, 0, 100);
    const ObjectRecord r2 = square(tissue, 125, 0, 100);
    tw->emit(r1);
    tw->emit(r2);
    tw->flush();
    const ObjectId t1 = r1.id(), t2 = r2.id();

    EdgeWriterPtr ew = store().edges().writerForStage(link);
    ew->assertDerivedFrom(t1, s[0]);
    ew->assertDerivedFrom(t1, s[1]);
    ew->assertDerivedFrom(t1, s[kColumns]);
    ew->assertDerivedFrom(t2, s[130]);
    ew->flush();
    AnnotationWriterPtr aw = store().annotations().writerForStage(note);
    aw->assertLabel(s[0], LabelId(7));
    aw->assertMeasurement(s[1], "score", 0.5, MeasurementUnit::Dimensionless);
    aw->assertLabel(t1, LabelId(2));
    aw->flush();
    for (StageId stage : {seg, tis, link, note}) complete(stage);

    ASSERT_EQ(idsOf(store().edges().originsOf(t1)).size(), 3u);
    EXPECT_FALSE(fillsBackground(*store().annotations().layerRasterSource(sp, LayerRasterContent::Membership)));
    EXPECT_FALSE(fillsBackground(*store().annotations().layerRasterSource(sp, LayerRasterContent::Label)));
    EXPECT_TRUE(L.wasEverPopulated(sp));
    const int64_t derivedEdges = store().edges().derivedFromEdgeCount();
    EXPECT_EQ(derivedEdges, 4);

    L.dropPrimitive(sp);

    EXPECT_TRUE(L.isDropped(sp));
    EXPECT_EQ(L.droppedObjectCount(sp), kObjects);
    EXPECT_TRUE(L.wasEverPopulated(sp));
    EXPECT_EQ(store().objects().objectCountInLayer(sp), 0);
    EXPECT_EQ(idsOf(store().objects().query(ObjectQuery().layer(tissue))), (std::vector<ObjectId>{t1, t2}));
    EXPECT_TRUE(idsOf(store().edges().originsOf(t1)).empty());
    EXPECT_TRUE(idsOf(store().edges().originsOf(t2)).empty());
    EXPECT_TRUE(idsOf(store().edges().derivedFrom(s[0])).empty());
    EXPECT_TRUE(idsOf(store().edges().derivedFrom(s[130])).empty());
    EXPECT_EQ(store().edges().derivedFromEdgeCount(), derivedEdges);

    const std::vector<Event> dropped = eventsOfKind(store(), EventKind::LayerDropped);
    ASSERT_EQ(dropped.size(), 1u);
    EXPECT_EQ(dropped[0].layer(), sp);
    EXPECT_EQ(dropped[0].objectCount(), 0);
    EXPECT_TRUE(dropped[0].boundingBox() == (Rect64{0, 0, 0, 0}));
    EXPECT_FALSE(dropped[0].stage().isValid());
    for (StageId stage : {seg, tis, link, note}) {
        EXPECT_FALSE(store().stageStates().isStale(stage));
        EXPECT_EQ(store().stageStates().status(stage), StageStatus::Complete);
    }

    // The assertions on sp objects went with them; the one on t1 stays.
    EXPECT_EQ(store().annotations().labelAnnotationCount(), 1);
    EXPECT_EQ(store().annotations().measurementAnnotationCount(), 0);
    EXPECT_EQ(store().annotations().labelCount(t1), 1);
    EXPECT_EQ(rawCount("SELECT COUNT(*) FROM measurements"), 0);
    EXPECT_EQ(rawCount("SELECT COUNT(*) FROM object_rtree"), 2);
    EXPECT_EQ(rawCount("SELECT COUNT(*) FROM derived_from"), derivedEdges);

    // A dropped layer still exists and yields background (ruling S15).
    for (LayerRasterContent content : {LayerRasterContent::Membership, LayerRasterContent::Label}) {
        EXPECT_TRUE(fillsBackground(*store().annotations().layerRasterSource(sp, content)));
    }
}

TEST_F(LayerRegistryTest, DropErrorsAndIdempotence) {
    LayerRegistry& L = store().layers();
    const LayerId tissue = L.declareSemantic("tissue", L.pixelLayer());
    const LayerId sp = L.declarePrimitive("sp");
    const LayerId unwritten = L.declarePrimitive("unwritten");
    const StageId seg = store().stageStateWriter().declare("superpixels", "");

    std::string message = messageOf([&] { L.dropPrimitive(tissue); });
    EXPECT_TRUE(contains(message, "'tissue'")) << message;
    message = messageOf([&] { L.dropPrimitive(L.pixelLayer()); });
    EXPECT_TRUE(contains(message, "'pixel'")) << message;
    message = messageOf([&] { L.dropPrimitive(LayerId(99)); });
    EXPECT_TRUE(contains(message, "99")) << message;
    EXPECT_THROW(L.dropPrimitive(tissue), InvalidArgumentError);
    EXPECT_THROW(L.dropPrimitive(L.pixelLayer()), InvalidArgumentError);
    EXPECT_THROW(L.dropPrimitive(LayerId(99)), InvalidArgumentError);
    EXPECT_THROW(L.dropPrimitive(LayerId()), InvalidArgumentError);
    EXPECT_FALSE(L.isDropped(tissue));
    EXPECT_FALSE(L.wasEverPopulated(LayerId(99)));
    EXPECT_EQ(L.droppedObjectCount(LayerId(99)), 0);
    EXPECT_EQ(L.droppedObjectCount(sp), 0);

    {
        ObjectWriterPtr w = store().objects().writerForStage(seg);
        w->emit(square(sp, 0, 0));
        w->emit(square(sp, 5, 0));
        w->flush();
    }
    EXPECT_TRUE(L.wasEverPopulated(sp));
    EXPECT_FALSE(L.wasEverPopulated(tissue));

    {
        StorePtr viewer = Store::open(pathUtf8().c_str(), OpenMode::ReadOnly, config());
        EXPECT_THROW(viewer->layers().dropPrimitive(sp), IoError);
        EXPECT_FALSE(viewer->layers().isDropped(sp));
        viewer->close();
    }

    L.dropPrimitive(sp);
    EXPECT_EQ(L.droppedObjectCount(sp), 2);
    EXPECT_NO_THROW(L.dropPrimitive(sp));
    EXPECT_EQ(L.droppedObjectCount(sp), 2);
    EXPECT_EQ(eventsOfKind(store(), EventKind::LayerDropped).size(), 1u);

    EXPECT_FALSE(L.wasEverPopulated(unwritten));
    L.dropPrimitive(unwritten);
    EXPECT_TRUE(L.isDropped(unwritten));
    EXPECT_FALSE(L.wasEverPopulated(unwritten));
    EXPECT_EQ(L.droppedObjectCount(unwritten), 0);
    EXPECT_EQ(eventsOfKind(store(), EventKind::LayerDropped).size(), 2u);

    ObjectWriterPtr late = store().objects().writerForStage(seg);
    message = messageOf([&] { late->emit(square(sp, 10, 0)); });
    EXPECT_TRUE(contains(message, "'sp'")) << message;
    EXPECT_TRUE(contains(message, "dropped")) << message;
    EXPECT_THROW(late->emit(square(sp, 10, 0)), InvalidArgumentError);

    // A drop survives reopening.
    StorePtr ro = reopen(OpenMode::ReadOnly);
    EXPECT_TRUE(ro->layers().isDropped(sp));
    EXPECT_EQ(ro->layers().droppedObjectCount(sp), 2);
    EXPECT_TRUE(ro->layers().wasEverPopulated(sp));
}

TEST_F(LayerRegistryTest, DropRacesAPendingWriterAndOtherHandles) {  // Review Focus 4
    LayerRegistry& L = store().layers();
    const LayerId tissue = L.declareSemantic("tissue", L.pixelLayer());
    const LayerId sp = L.declarePrimitive("sp");
    const StageId seg = store().stageStateWriter().declare("superpixels", "");
    const StageId tis = store().stageStateWriter().declare("tissue", "");
    const StageId link = store().stageStateWriter().declare("link", "");

    ObjectWriterPtr tw = store().objects().writerForStage(tis);
    const ObjectRecord t1 = square(tissue, 0, 0, 100);
    tw->emit(t1);
    tw->flush();
    ObjectWriterPtr sw = store().objects().writerForStage(seg);
    const ObjectRecord committed = square(sp, 50, 50);
    sw->emit(committed);
    sw->flush();

    // A viewer opened before the drop, whose registry has already cached the layers.
    StorePtr viewer = Store::open(pathUtf8().c_str(), OpenMode::ReadOnly, config());
    EXPECT_FALSE(viewer->layers().isDropped(sp));
    EXPECT_EQ(viewer->layers().count(), 3);

    ObjectWriterPtr pending = store().objects().writerForStage(seg);
    for (int64_t x = 0; x < 3; ++x) pending->emit(square(sp, x, 0));
    EXPECT_EQ(pending->emittedCount(), 3);

    L.dropPrimitive(sp);

    std::string message;
    try {
        pending->flush();
        ADD_FAILURE() << "a pending batch into a dropped layer was committed";
    } catch (const InvalidArgumentError& e) {
        message = e.what();
    }
    EXPECT_TRUE(contains(message, "'sp'")) << message;
    // The writer validated sp before the drop: a later emit is accepted and refused at commit.
    pending->emit(square(sp, 3, 0));
    EXPECT_THROW(pending->flush(), InvalidArgumentError);
    EXPECT_EQ(store().objects().objectCountInLayer(sp), 0);
    EXPECT_EQ(L.droppedObjectCount(sp), 1);

    EXPECT_TRUE(viewer->layers().isDropped(sp));  // without reopening
    EXPECT_EQ(viewer->layers().droppedObjectCount(sp), 1);
    EXPECT_TRUE(viewer->layers().wasEverPopulated(sp));
    EXPECT_EQ(viewer->objects().objectCountInLayer(sp), 0);
    viewer->close();

    EdgeWriterPtr ew = store().edges().writerForStage(link);
    message.clear();
    try {
        ew->assertDerivedFrom(t1.id(), committed.id());
        ADD_FAILURE() << "assertDerivedFrom into a dropped layer was accepted";
    } catch (const InvalidArgumentError& e) {
        message = e.what();
    }
    EXPECT_TRUE(contains(message, "'sp'")) << message;
    EXPECT_EQ(ew->assertedCount(), 0);
}
