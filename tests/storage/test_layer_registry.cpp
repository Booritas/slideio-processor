// Requirements: UR-STG-10, UR-RES-13, UR-RES-14, UR-SEG-4, UR-IMG-4
#include "storage/store_fixture.hpp"

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/storage/event_journal.hpp>
#include <slideio/processor/storage/layer_registry.hpp>
#include <slideio/processor/storage/stage_state_writer.hpp>
#include <slideio/processor/storage/store.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using namespace slideio::processor;

namespace {

class LayerRegistryTest : public StoreFixture {};

std::string messageOf(const std::function<void()>& call) {
    try {
        call();
    } catch (const std::exception& e) {
        return e.what();
    }
    return std::string();
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
