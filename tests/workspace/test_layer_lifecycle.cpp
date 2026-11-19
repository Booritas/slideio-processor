// Requirements: UR-STG-10, UR-STG-12, UR-RES-16
#include "common/temp_path.hpp"
#include "workspace/support/test_slide.hpp"

#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/core/object_query.hpp>
#include <slideio/processor/core/object_record.hpp>
#include <slideio/processor/storage/layer_registry.hpp>
#include <slideio/processor/storage/object_store.hpp>
#include <slideio/processor/storage/stage_state_reader.hpp>
#include <slideio/processor/storage/stage_state_writer.hpp>
#include <slideio/processor/storage/store.hpp>
#include <slideio/processor/workspace/layer_lifecycle.hpp>
#include <slideio/processor/workspace/workspace.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>

using namespace slideio::processor;
using testing_support::TempDirectory;
using testing_support::writeTestSlide;

namespace fs = std::filesystem;

namespace {

struct Prepared {
    Prepared() {
        fs::create_directories(dir.path());
        slide = writeTestSlide(dir.path() / "slide.tif", 300, 200);
        workspace = dir.path() / "ws";
    }
    WorkspacePtr createWorkspace() const {
        return Workspace::create(workspace.u8string().c_str(), slide.u8string().c_str(), 0, config);
    }
    TempDirectory dir;
    fs::path slide;
    fs::path workspace;
    Configuration config = Configuration::defaults();
};

ObjectRecord square(LayerId layer, int64_t x, int64_t y) {
    const Point64 points[] = {{x, y}, {x + 1, y}, {x + 1, y + 1}, {x, y + 1}};
    Polygon polygon;
    polygon.addRing(points, 4, false);
    ObjectRecord record;
    record.setLayer(layer);
    record.setGeometry(polygon);
    return record;
}

void writeSquares(Store& store, StageId stage, LayerId layer, int count) {
    ObjectWriterPtr writer = store.objects().writerForStage(stage);
    for (int i = 0; i < count; ++i) writer->emit(square(layer, i, 0));
    writer->flush();
}

} // namespace

TEST(LayerLifecycle, DeclaresThroughTheStore) {
    Prepared p;
    WorkspacePtr ws = p.createWorkspace();
    LayerLifecycle& life = ws->layerLifecycle();
    const LayerId pixel = ws->layers().pixelLayer();
    const LayerId tissue = life.declareSemantic("tissue", pixel);
    const LayerId cells = life.declareSemantic("cells", tissue);
    const LayerId superpixels = life.declarePrimitive("superpixels");

    LayerId found;
    ASSERT_TRUE(ws->layers().find("cells", found));
    EXPECT_EQ(found, cells);
    EXPECT_EQ(ws->layers().parent(cells), tissue);
    EXPECT_TRUE(ws->layers().isAbove(cells, tissue));
    EXPECT_FALSE(ws->layers().isAbove(tissue, cells));
    EXPECT_EQ(ws->layers().kind(superpixels), LayerKind::Primitive);
}

TEST(LayerLifecycle, DropSemanticIsInvalidArgument) {
    Prepared p;
    WorkspacePtr ws = p.createWorkspace();
    LayerLifecycle& life = ws->layerLifecycle();
    const LayerId tissue = life.declareSemantic("tissue", ws->layers().pixelLayer());
    EXPECT_THROW(life.drop(tissue), InvalidArgumentError);
    EXPECT_THROW(life.drop(ws->layers().pixelLayer()), InvalidArgumentError);
    EXPECT_FALSE(life.isDropped(tissue));
}

TEST(LayerLifecycle, DropKeepsDerivedLayerComplete) {
    Prepared p;
    WorkspacePtr ws = p.createWorkspace();
    LayerLifecycle& life = ws->layerLifecycle();
    StorePtr store = ws->store();
    const LayerId superpixels = life.declarePrimitive("superpixels");
    const LayerId regions = life.declareSemantic("regions", ws->layers().pixelLayer());

    const StageId segment = store->stageStateWriter().declare("segment", "");
    const StageId group = store->stageStateWriter().declare("group", "");
    writeSquares(*store, segment, superpixels, 7);
    writeSquares(*store, group, regions, 3);
    store->stageStateWriter().setStatus(segment, StageStatus::Complete);
    store->stageStateWriter().setStatus(group, StageStatus::Complete);

    life.drop(superpixels);

    EXPECT_TRUE(life.isDropped(superpixels));
    EXPECT_TRUE(life.wasEverPopulated(superpixels));
    EXPECT_EQ(life.droppedObjectCount(superpixels), 7);
    EXPECT_EQ(store->objects().count(ObjectQuery().layer(regions)), 3);
    EXPECT_EQ(store->objects().count(ObjectQuery().layer(superpixels)), 0);
    EXPECT_FALSE(store->stageStates().isStale(segment));
    EXPECT_FALSE(store->stageStates().isStale(group));
    EXPECT_FALSE(life.isDropped(regions));

    EXPECT_NO_THROW(life.drop(superpixels));  // a second drop does nothing
    EXPECT_EQ(life.droppedObjectCount(superpixels), 7);
}

TEST(LayerLifecycle, NeverPopulatedIsNotDropped) {
    Prepared p;
    WorkspacePtr ws = p.createWorkspace();
    LayerLifecycle& life = ws->layerLifecycle();
    const LayerId superpixels = life.declarePrimitive("superpixels");
    EXPECT_FALSE(life.isDropped(superpixels));
    EXPECT_FALSE(life.wasEverPopulated(superpixels));
    EXPECT_EQ(life.droppedObjectCount(superpixels), 0);
}

TEST(LayerLifecycle, ReadOnlyWorkspaceCannotDeclare) {
    Prepared p;
    {
        WorkspacePtr ws = p.createWorkspace();
        ws->layerLifecycle().declarePrimitive("superpixels");
    }
    WorkspacePtr ro = Workspace::openReadOnly(p.workspace.u8string().c_str(), p.config);
    LayerLifecycle& life = ro->layerLifecycle();
    EXPECT_THROW(life.declarePrimitive("other"), IoError);
    EXPECT_THROW(life.declareSemantic("tissue", ro->layers().pixelLayer()), IoError);
    LayerId sp;
    ASSERT_TRUE(ro->layers().find("superpixels", sp));
    EXPECT_THROW(life.drop(sp), IoError);
}

TEST(LayerLifecycle, DropUnknownLayerIsInvalidArgument) {
    Prepared p;
    WorkspacePtr ws = p.createWorkspace();
    EXPECT_THROW(ws->layerLifecycle().drop(LayerId(9999)), InvalidArgumentError);
}
