// Requirements: UR-RES-12
// Store format v1 is frozen (ruling S53). A fresh store's schema must match the checked-in
// snapshot, which catches drift in new stores, additive drift included; and the checked-in
// golden store, written by v1, must still open and read back its content, which catches a
// change that would leave existing stores unreadable. The disabled generator rewrites both.
// The v1 fixtures are never regenerated once v1 is frozen: a version 2 gets store-v2.* fixtures
// of its own and keeps the v1 golden store as the input its migration is tested on. Only a
// SQLite upgrade may change the snapshot, and only its three rtree shadow-table sql lines
// (object_rtree_node, _parent and _rowid); regenerate the snapshot then, deliberately.
#include "common/temp_path.hpp"
#include "storage/raster_test_util.hpp"
#include "storage/store_fixture.hpp"

#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/data_type.hpp>
#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/core/measurement.hpp>
#include <slideio/processor/core/object_query.hpp>
#include <slideio/processor/core/object_record.hpp>
#include <slideio/processor/core/provenance.hpp>
#include <slideio/processor/storage/store.hpp>

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

using namespace slideio::processor;

namespace {

const std::filesystem::path kDataDir = std::filesystem::u8path(SLIDEIO_PROCESSOR_TEST_DATA_DIR).make_preferred();
const std::filesystem::path kBuildDir = std::filesystem::u8path(SLIDEIO_PROCESSOR_TEST_BUILD_DIR).make_preferred();
constexpr const char* kSchemaFile = "store-v1.schema.sql";
constexpr const char* kSchemaActualFile = "store-v1.schema.actual.sql";
constexpr const char* kGoldenFile = "store-v1.golden.store";
constexpr const char* kFrozenMessage =
    "store format v1 is frozen: a schema change needs format_version 2 and a migration (SDS 03 §3.12)";

// The golden content. Ten cells along a row, each with area 400 + i.
constexpr int kCellCount = 10;
constexpr const char* kScoreName = "ki67_score";
constexpr double kScore = 0.75;

std::filesystem::path sidePath(const std::filesystem::path& store, const char* suffix) {
    std::filesystem::path side = store;
    side += suffix;
    return side;
}

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

ObjectRecord tissueRecord(LayerId tissue) { return rec(tissue, 0, 0, 1024); }

ObjectRecord cellRecord(LayerId cells, int i) {
    ObjectRecord record = rec(cells, 10 + i * 100, 10, 20);
    record.measurements().set("area", 400.0 + i, MeasurementUnit::SquarePixels);
    return record;
}

ObjectRecord superpixelRecord(LayerId superpixels, int j) { return rec(superpixels, j * 512, 512, 256); }

// The raster's value at level pixel (x, y).
float probabilityAt(int64_t x, int64_t y) { return static_cast<float>(x + y); }

// Text of one column, "" for NULL.
std::string columnText(sqlite3_stmt* statement, int column) {
    const unsigned char* text = sqlite3_column_text(statement, column);
    return text == nullptr ? std::string() : reinterpret_cast<const char*>(text);
}

// The S53 dump of the store at `path`: `type|name|tbl_name|sql` of every sqlite_master row,
// ordered by type and name, then the sorted meta keys. Lines end in "\n" alone.
std::string schemaDump(const std::filesystem::path& path) {
    std::string dump;
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path.u8string().c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        ADD_FAILURE() << "schemaDump: cannot open '" << path.u8string() << "': " << sqlite3_errmsg(db);
        sqlite3_close(db);
        return dump;
    }
    const auto lines = [&](const char* sql, const char* heading, int columns) {
        dump += heading;
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) != SQLITE_OK) {
            ADD_FAILURE() << "schemaDump: " << sqlite3_errmsg(db);
            return;
        }
        while (sqlite3_step(statement) == SQLITE_ROW) {
            for (int c = 0; c < columns; ++c) dump += (c == 0 ? "" : "|") + columnText(statement, c);
            dump += "\n";
        }
        sqlite3_finalize(statement);
    };
    lines("SELECT type, name, tbl_name, sql FROM sqlite_master ORDER BY type, name",
          "-- sqlite_master: type|name|tbl_name|sql, ordered by type, name\n", 4);
    lines("SELECT key FROM meta ORDER BY key", "-- meta keys, sorted\n", 1);
    sqlite3_close(db);
    return dump;
}

// `text` without its carriage returns, so a CRLF checkout compares equal.
std::string withoutCarriageReturns(std::string text) {
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    return text;
}

// The schema dump of a fresh CreateNew store.
std::string freshSchemaDump() {
    StoreFilesGuard files("format_v1");
    Store::open(files.utf8().c_str(), OpenMode::CreateNew, Configuration())->close();
    return schemaDump(files.path());
}

// A directory in the temp directory, removed with its contents when the guard goes out of scope.
class TempDirGuard {
public:
    explicit TempDirGuard(const char* stem) : path_(uniqueTempPath(stem, "")) {
        std::filesystem::create_directories(path_);
    }
    TempDirGuard(const TempDirGuard&) = delete;
    TempDirGuard& operator=(const TempDirGuard&) = delete;
    ~TempDirGuard() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

// Writes the golden content into a new store at `path` and closes it.
void writeGoldenStore(const std::filesystem::path& path) {
    StorePtr store = Store::open(path.u8string().c_str(), OpenMode::CreateNew, Configuration());
    Provenance provenance;
    provenance.setSlideId("golden");
    provenance.setSceneIndex(0);
    store->setProvenance(provenance);

    LayerRegistry& layers = store->layers();
    const LayerId tissue = layers.declareSemantic("tissue", layers.pixelLayer());
    const LayerId cells = layers.declareSemantic("cells", tissue);
    const LayerId superpixels = layers.declarePrimitive("superpixels");
    const LabelId ki67 = store->vocabulary().addLocal("ki67_positive");
    StageStateWriter& stages = store->stageStateWriter();
    const StageId seg = stages.declare("seg", "{}");
    const StageId prob = stages.declare("prob", "{}");
    const StageId classify = stages.declare("classify", "{}");

    // One flush per layer, so the journal holds one ObjectsAdded per layer in a known order.
    ObjectWriterPtr objects = store->objects().writerForStage(seg);
    objects->emit(tissueRecord(tissue));
    objects->flush();
    for (int i = 0; i < kCellCount; ++i) objects->emit(cellRecord(cells, i));
    objects->flush();
    for (int j = 0; j < 2; ++j) objects->emit(superpixelRecord(superpixels, j));
    objects->flush();
    stages.setStatus(seg, StageStatus::Complete);

    // Level 1 over 1024 x 1024 level-0 pixels: four 256 x 256 tiles of x + y.
    RasterRegistry& rasters = store->rasters();
    const RasterId probability = rasters.declare(
        RasterDeclaration("probability", DataType::Float32, 1).compression(RasterCompression::Deflate));
    RasterWriterPtr raster = rasters.writerForStage(prob, probability);
    for (int64_t ty = 0; ty < 2; ++ty) {
        for (int64_t tx = 0; tx < 2; ++tx) {
            const int64_t x0 = tx * 256, y0 = ty * 256;
            raster->writeTile(makeView(1, Rect64{x0 * 2, y0 * 2, 512, 512}, 256, 256, DataType::Float32, 1,
                                       [&](int64_t x, int64_t y, int32_t) {
                                           return double(probabilityAt(x0 + x, y0 + y));
                                       })
                                  ->view);
        }
    }
    raster->flush();
    stages.setStatus(prob, StageStatus::Complete);

    EdgeWriterPtr edges = store->edges().writerForStage(classify);
    edges->assertParent(cellRecord(cells, 0).id(), tissueRecord(tissue).id());
    edges->assertDerivedFrom(cellRecord(cells, 1).id(), superpixelRecord(superpixels, 0).id());
    edges->flush();
    AnnotationWriterPtr annotations = store->annotations().writerForStage(classify);
    annotations->assertLabel(cellRecord(cells, 2).id(), ki67);
    annotations->assertMeasurement(cellRecord(cells, 2).id(), kScoreName, kScore, MeasurementUnit::Dimensionless);
    annotations->flush();
    stages.setStatus(classify, StageStatus::Complete);

    layers.dropPrimitive(superpixels);
    store->close();
}

// Looks a name up in a registry that answers find(name, out), failing the test when absent.
template <class Registry, class Id>
Id mustFind(const Registry& registry, const char* name) {
    Id id;
    EXPECT_TRUE(registry.find(name, id)) << name;
    return id;
}

// Checks the golden content through every public read.
void expectGoldenContent(Store& store) {
    EXPECT_EQ(store.formatVersion(), 1);
    EXPECT_STREQ(store.provenance().slideId(), "golden");
    EXPECT_EQ(store.provenance().sceneIndex(), 0);

    // Layers, labels and stages, as generated.
    LayerRegistry& layers = store.layers();
    EXPECT_EQ(layers.count(), 4);
    const LayerId tissue = mustFind<LayerRegistry, LayerId>(layers, "tissue");
    const LayerId cells = mustFind<LayerRegistry, LayerId>(layers, "cells");
    const LayerId superpixels = mustFind<LayerRegistry, LayerId>(layers, "superpixels");
    EXPECT_EQ(layers.kind(tissue), LayerKind::Semantic);
    EXPECT_EQ(layers.parent(tissue), layers.pixelLayer());
    EXPECT_EQ(layers.parent(cells), tissue);
    EXPECT_EQ(layers.kind(superpixels), LayerKind::Primitive);
    EXPECT_TRUE(layers.isDropped(superpixels));
    EXPECT_EQ(layers.droppedObjectCount(superpixels), 2);
    EXPECT_TRUE(layers.wasEverPopulated(cells));

    VocabularyRegistry& vocabulary = store.vocabulary();
    const LabelId ki67 = mustFind<VocabularyRegistry, LabelId>(vocabulary, "ki67_positive");
    EXPECT_EQ(ki67, LabelId(65536));
    EXPECT_EQ(vocabulary.origin(ki67), LabelOrigin::WorkspaceLocal);

    StageStateReader& stages = store.stageStates();
    EXPECT_EQ(stages.count(), 3);
    const StageId seg = mustFind<StageStateReader, StageId>(stages, "seg");
    const StageId prob = mustFind<StageStateReader, StageId>(stages, "prob");
    const StageId classify = mustFind<StageStateReader, StageId>(stages, "classify");
    for (StageId stage : {seg, prob, classify}) EXPECT_EQ(stages.status(stage), StageStatus::Complete);

    // Objects: ten cells, one of which round-trips its area.
    ObjectStore& objects = store.objects();
    EXPECT_EQ(objects.count(ObjectQuery().layer(cells)), kCellCount);
    ObjectRecord cell;
    ASSERT_TRUE(objects.get(cellRecord(cells, 3).id(), cell));
    Measurement area;
    ASSERT_TRUE(cell.measurements().find("area", area));
    EXPECT_EQ(area.value(), 403.0);
    EXPECT_EQ(area.unit(), MeasurementUnit::SquarePixels);
    EXPECT_EQ(cell.stage(), seg);

    // Edges and assertions by classify.
    EdgeStore& edges = store.edges();
    ObjectId parent;
    ASSERT_TRUE(edges.parentOf(cellRecord(cells, 0).id(), parent));
    EXPECT_EQ(parent, tissueRecord(tissue).id());
    EXPECT_EQ(edges.parentEdgeCount(), 1);
    EXPECT_EQ(edges.derivedFromEdgeCount(), 1);
    EXPECT_FALSE(edges.originsOf(cellRecord(cells, 1).id())->next());

    AnnotationStore& annotations = store.annotations();
    const ObjectId annotated = cellRecord(cells, 2).id();
    ASSERT_EQ(annotations.labelCount(annotated), 1);
    EXPECT_EQ(annotations.labelAt(annotated, 0), ki67);
    EXPECT_EQ(annotations.labelAssertedBy(annotated, 0), classify);
    Measurement score;
    ASSERT_TRUE(annotations.assertedMeasurement(annotated, kScoreName, classify, score));
    EXPECT_EQ(score.value(), kScore);
    EXPECT_EQ(score.unit(), MeasurementUnit::Dimensionless);

    // The journal, in order.
    std::vector<EventKind> kinds;
    EventCursorPtr cursor = store.events().subscribe(EventJournal::kFromBeginning);
    while (cursor->next(0)) kinds.push_back(cursor->current().kind());
    const std::vector<EventKind> expected = {
        EventKind::LayerDeclared,     EventKind::LayerDeclared,     EventKind::LayerDeclared,
        EventKind::LabelAdded,        EventKind::ObjectsAdded,      EventKind::ObjectsAdded,
        EventKind::ObjectsAdded,      EventKind::StageStateChanged, EventKind::RasterDeclared,
        EventKind::RasterWritten,     EventKind::StageStateChanged, EventKind::EdgesAsserted,
        EventKind::ObjectsAnnotated,  EventKind::StageStateChanged, EventKind::LayerDropped};
    EXPECT_EQ(kinds, expected);

    // The raster, published by prob, reads x + y at a known level-1 pixel.
    RasterRegistry& rasters = store.rasters();
    const RasterId probability = mustFind<RasterRegistry, RasterId>(rasters, "probability");
    ASSERT_TRUE(rasters.isPopulated(probability));
    EXPECT_EQ(rasters.planeType(probability), DataType::Float32);
    EXPECT_EQ(rasters.writerStage(probability), prob);
    EXPECT_EQ(rasters.writtenAtLevel(probability), 1);
    TileRasterSourcePtr source = rasters.sourceFor(probability);
    // Level pixels (300, 200) to (303, 203), at the written scale 2.
    auto view = makeView(1, Rect64{600, 400, 8, 8}, 4, 4, DataType::Float32, 1, 7.0);
    source->fillTile(view->view);
    for (int64_t y = 0; y < 4; ++y) {
        for (int64_t x = 0; x < 4; ++x) {
            float sample = 0;
            const auto* row = static_cast<const uint8_t*>(view->view.planeData(0)) + y * view->view.planeRowStrideBytes(0);
            std::memcpy(&sample, row + x * sizeof(float), sizeof(float));
            EXPECT_EQ(sample, probabilityAt(300 + x, 200 + y)) << x << ", " << y;
        }
    }
}

}  // namespace

TEST(StoreFormatV1, SchemaMatchesTheFrozenSnapshot) {
    const std::filesystem::path snapshot = kDataDir / kSchemaFile;
    const std::string expected = withoutCarriageReturns(readFile(snapshot));
    ASSERT_FALSE(expected.empty()) << "no snapshot at '" << snapshot.u8string() << "'";
    const std::string actual = freshSchemaDump();
    if (actual != expected) {
        const std::filesystem::path written = kBuildDir / kSchemaActualFile;
        writeFile(written, actual);
        FAIL() << kFrozenMessage << "\n  snapshot: " << snapshot.u8string() << "\n  actual:   " << written.u8string();
    }
}

TEST(StoreFormatV1, GoldenStoreStillOpens) {
    const std::filesystem::path golden = kDataDir / kGoldenFile;
    ASSERT_TRUE(std::filesystem::exists(golden)) << golden.u8string();
    TempDirGuard dir("golden");
    const std::filesystem::path copy = dir.path() / kGoldenFile;
    std::filesystem::copy_file(golden, copy);
    std::filesystem::copy(sidePath(golden, "-rasters"), sidePath(copy, "-rasters"),
                          std::filesystem::copy_options::recursive);

    for (OpenMode mode : {OpenMode::ReadOnly, OpenMode::ReadWrite}) {
        SCOPED_TRACE(mode == OpenMode::ReadOnly ? "ReadOnly" : "ReadWrite");
        StorePtr store = Store::open(copy.u8string().c_str(), mode, Configuration());
        expectGoldenContent(*store);
        store->close();
    }
}

// Rewrites both v1 fixtures in SLIDEIO_PROCESSOR_TEST_DATA_DIR. Never run it while v1 is frozen
// (see the top of this file); a version 2 copies and adapts this generator to write store-v2.*
// files. Run: --gtest_also_run_disabled_tests --gtest_filter=StoreFormatV1.DISABLED_GenerateFixtures
TEST(StoreFormatV1, DISABLED_GenerateFixtures) {
    writeFile(kDataDir / kSchemaFile, freshSchemaDump());

    // Built in a temp directory and copied, so neither -wal, -shm nor -writer.lock reaches
    // the data directory.
    TempDirGuard dir("golden_gen");
    const std::filesystem::path built = dir.path() / kGoldenFile;
    writeGoldenStore(built);
    const std::filesystem::path golden = kDataDir / kGoldenFile;
    removeStoreFiles(golden);
    std::filesystem::copy_file(built, golden);
    std::filesystem::copy(sidePath(built, "-rasters"), sidePath(golden, "-rasters"),
                          std::filesystem::copy_options::recursive);
}
