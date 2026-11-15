// Requirements: UR-RES-7, UR-STG-10, UR-STG-9
#include "storage/store_fixture.hpp"

#include "storage/detail/raster_files.hpp"

#include <slideio/processor/core/data_type.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/storage/event_journal.hpp>
#include <slideio/processor/storage/layer_registry.hpp>
#include <slideio/processor/storage/raster_store.hpp>
#include <slideio/processor/storage/stage_state_writer.hpp>
#include <slideio/processor/storage/store.hpp>

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

using namespace slideio::processor;

namespace {

class RasterRegistryTest : public StoreFixture {
protected:
    // Runs `sql` on a connection of its own, as a foreign writer would; false on failure.
    bool rawExec(const std::string& sql) {
        sqlite3* db = nullptr;
        bool ok = false;
        if (sqlite3_open_v2(pathUtf8().c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK) {
            ok = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK;
        }
        sqlite3_close(db);
        return ok;
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

std::vector<Event> allEvents(Store& store) {
    std::vector<Event> events;
    EventCursorPtr cursor = store.events().subscribe(EventJournal::kFromBeginning);
    while (cursor->next(0)) events.push_back(cursor->current());
    return events;
}

}  // namespace

TEST_F(RasterRegistryTest, DeclareAndReadBack) {
    RasterRegistry& R = store().rasters();
    RasterId p = R.declare(RasterDeclaration("probability", DataType::Float32, 1).description("tumour p"));
    EXPECT_EQ(p, RasterId(1));
    EXPECT_EQ(R.count(), 1);
    EXPECT_EQ(R.at(0), p);
    RasterId f;
    EXPECT_TRUE(R.find("probability", f));
    EXPECT_EQ(f, p);
    EXPECT_FALSE(R.find("Probability", f));
    EXPECT_FALSE(R.find(nullptr, f));
    EXPECT_STREQ(R.name(p), "probability");
    EXPECT_TRUE(R.exists(p));
    EXPECT_FALSE(R.exists(RasterId(9)));
    EXPECT_EQ(R.planeType(p), DataType::Float32);
    EXPECT_EQ(R.planeCount(p), 1);
    EXPECT_FALSE(R.isPopulated(p));
    EXPECT_EQ(R.bytesOnDisk(p), 0);
    EXPECT_EQ(R.totalBytesOnDisk(), 0);
    EXPECT_THROW(R.writerStage(p), InvalidArgumentError);
    EXPECT_THROW(R.writtenAtLevel(p), InvalidArgumentError);
    EXPECT_THROW(R.name(RasterId(9)), InvalidArgumentError);
    EXPECT_THROW(R.planeType(RasterId(9)), InvalidArgumentError);
    EXPECT_THROW(R.planeCount(RasterId(9)), InvalidArgumentError);
    EXPECT_THROW(R.writerStage(RasterId(9)), InvalidArgumentError);
    EXPECT_THROW(R.writtenAtLevel(RasterId(9)), InvalidArgumentError);
    EXPECT_THROW(R.at(1), InvalidArgumentError);
    EXPECT_THROW(R.at(-1), InvalidArgumentError);
    EXPECT_EQ(R.bytesOnDisk(RasterId(9)), 0);

    const RasterId anchors = R.declare(RasterDeclaration("anchors", DataType::Int64, 3).compression(RasterCompression::Lzw));
    EXPECT_EQ(anchors, RasterId(2));
    EXPECT_EQ(R.count(), 2);
    EXPECT_EQ(R.at(1), anchors);
    EXPECT_EQ(R.planeType(anchors), DataType::Int64);
    EXPECT_EQ(R.planeCount(anchors), 3);
}

TEST_F(RasterRegistryTest, DeclarationIsValidated) {
    RasterRegistry& R = store().rasters();
    R.declare(RasterDeclaration("probability", DataType::Float32, 1));
    const int64_t events = store().events().eventCount();
    const std::vector<std::pair<const char*, RasterDeclaration>> refused = {
        {"empty name", RasterDeclaration("", DataType::Byte, 1)},
        {"null name", RasterDeclaration(nullptr, DataType::Byte, 1)},
        {"duplicate name", RasterDeclaration("probability", DataType::Byte, 1)},
        {"unknown type", RasterDeclaration("a", DataType::Unknown, 1)},
        {"out-of-range type", RasterDeclaration("b", static_cast<DataType>(42), 1)},
        {"negative type", RasterDeclaration("c", static_cast<DataType>(-1), 1)},
        {"zero planes", RasterDeclaration("d", DataType::Byte, 0)},
        {"65536 planes", RasterDeclaration("e", DataType::Byte, 65536)},
        {"negative planes", RasterDeclaration("f", DataType::Byte, -1)},
        {"unknown compression", RasterDeclaration("g", DataType::Byte, 1).compression(static_cast<RasterCompression>(7))},
    };
    for (const auto& [what, declaration] : refused) {
        EXPECT_THROW(R.declare(declaration), InvalidArgumentError) << what;
    }
    EXPECT_TRUE(contains(messageOf([&] { R.declare(RasterDeclaration("probability", DataType::Byte, 1)); }),
                         "probability"));
    EXPECT_EQ(R.count(), 1);
    EXPECT_EQ(store().events().eventCount(), events);

    // The bounds themselves are accepted.
    EXPECT_EQ(R.planeCount(R.declare(RasterDeclaration("wide", DataType::Byte, 65535))), 65535);
    EXPECT_EQ(R.planeCount(R.declare(RasterDeclaration("narrow", DataType::Float64, 1).compression(RasterCompression::None))), 1);
}

TEST_F(RasterRegistryTest, RasterNamesDoNotCollideWithLayers) {
    const LayerId layer = store().layers().declarePrimitive("cells");
    const RasterId raster = store().rasters().declare(RasterDeclaration("cells", DataType::Int64, 1));
    RasterId found;
    EXPECT_TRUE(store().rasters().find("cells", found));
    EXPECT_EQ(found, raster);
    LayerId foundLayer;
    EXPECT_TRUE(store().layers().find("cells", foundLayer));
    EXPECT_EQ(foundLayer, layer);
    EXPECT_FALSE(store().rasters().find("pixel", found));
    EXPECT_EQ(store().rasters().declare(RasterDeclaration("pixel", DataType::Byte, 1)), RasterId(2));
}

TEST_F(RasterRegistryTest, DeclarationIsJournalledWithItsRaster) {
    const LayerId layer = store().layers().declarePrimitive("cells");
    const RasterId p = store().rasters().declare(RasterDeclaration("probability", DataType::Float32, 1));
    int rasterDeclared = 0;
    for (const Event& ev : allEvents(store())) {
        if (ev.kind() == EventKind::RasterDeclared) {
            ++rasterDeclared;
            EXPECT_EQ(ev.raster(), p);
            EXPECT_FALSE(ev.layer().isValid());
            EXPECT_FALSE(ev.stage().isValid());
            EXPECT_EQ(ev.objectCount(), 0);
        } else {
            EXPECT_EQ(ev.kind(), EventKind::LayerDeclared);
            EXPECT_EQ(ev.layer(), layer);
            EXPECT_FALSE(ev.raster().isValid());
        }
    }
    EXPECT_EQ(rasterDeclared, 1);

    // Copies carry the raster; a default or moved-from event has none.
    const std::vector<Event> events = allEvents(store());
    Event copy = events.back();
    EXPECT_EQ(copy.raster(), p);
    Event moved = std::move(copy);
    EXPECT_EQ(moved.raster(), p);
    EXPECT_FALSE(copy.raster().isValid());  // NOLINT(bugprone-use-after-move): reads as a default event
    EXPECT_FALSE(Event().raster().isValid());
}

TEST_F(RasterRegistryTest, DirectorySitsBesideTheStore) {
    RasterRegistry& R = store().rasters();
    EXPECT_EQ(std::string(R.directory()), pathUtf8() + "-rasters");
    R.declare(RasterDeclaration("probability", DataType::Float32, 1));
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::u8path(R.directory())));
}

TEST(RasterRegistry, DirectorySitsBesideANonAsciiStore) {
    StoreFilesGuard files(u8"raster_ü_日本");
    StorePtr store = Store::open(files.utf8().c_str(), OpenMode::CreateNew, Configuration());
    EXPECT_EQ(std::string(store->rasters().directory()), files.utf8() + "-rasters");
    store->rasters().declare(RasterDeclaration(u8"ü", DataType::Byte, 1));
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::u8path(store->rasters().directory())));
    store->close();
    // Still answers after close, and is the same path.
    EXPECT_EQ(std::string(store->rasters().directory()), files.utf8() + "-rasters");
}

TEST_F(RasterRegistryTest, ReopenAndReadOnly) {
    store().rasters().declare(RasterDeclaration("probability", DataType::Float32, 1));
    store().rasters().declare(RasterDeclaration("classes", DataType::UInt16, 4).compression(RasterCompression::None));
    StorePtr ro = reopen(OpenMode::ReadOnly);
    RasterRegistry& R = ro->rasters();
    ASSERT_EQ(R.count(), 2);
    EXPECT_STREQ(R.name(R.at(0)), "probability");
    EXPECT_STREQ(R.name(R.at(1)), "classes");
    EXPECT_EQ(R.planeType(RasterId(1)), DataType::Float32);
    EXPECT_EQ(R.planeType(RasterId(2)), DataType::UInt16);
    EXPECT_EQ(R.planeCount(RasterId(1)), 1);
    EXPECT_EQ(R.planeCount(RasterId(2)), 4);
    EXPECT_FALSE(R.isPopulated(RasterId(2)));
    try {
        R.declare(RasterDeclaration("more", DataType::Byte, 1));
        ADD_FAILURE() << "declare on a ReadOnly store did not throw";
    } catch (const IoError& e) {
        EXPECT_TRUE(contains(e.what(), "read-only")) << e.what();
    }
    // Validation does not run ahead of the read-only check.
    EXPECT_THROW(R.declare(RasterDeclaration("", DataType::Byte, 1)), IoError);
    EXPECT_EQ(R.count(), 2);
}

TEST_F(RasterRegistryTest, ClosedStoreRefusesOrAnswersEmpty) {
    const RasterId p = store().rasters().declare(RasterDeclaration("probability", DataType::Float32, 1));
    store().close();
    RasterRegistry& R = store().rasters();
    EXPECT_EQ(R.count(), 0);
    RasterId f;
    EXPECT_FALSE(R.find("probability", f));
    EXPECT_FALSE(R.exists(p));
    EXPECT_FALSE(R.isPopulated(p));
    EXPECT_TRUE(contains(messageOf([&] { R.name(p); }), "store is closed"));
    EXPECT_THROW(R.planeType(p), IoError);
    EXPECT_THROW(R.at(0), IoError);
    EXPECT_THROW(R.bytesOnDisk(p), IoError);
    EXPECT_THROW(R.totalBytesOnDisk(), IoError);
    EXPECT_THROW(R.declare(RasterDeclaration("more", DataType::Byte, 1)), IoError);
}

// Population is the row's flag and the file's presence (ruling S48). Until the writer
// lands, a foreign connection publishes the row as flush will: in one transaction that
// also bumps meta.rasters_revision.
TEST_F(RasterRegistryTest, PopulatedIsTheRowAndItsFile) {
    RasterRegistry& R = store().rasters();
    const RasterId p = R.declare(RasterDeclaration("probability", DataType::Float32, 1));
    const RasterId q = R.declare(RasterDeclaration("classes", DataType::Byte, 1));
    const StageId stage = store().stageStateWriter().declare("segment", "{}");
    EXPECT_FALSE(R.isPopulated(p));  // the cache is loaded before the foreign write

    const std::string fileName = detail::generateRasterFileStem() + ".tif";
    ASSERT_TRUE(rawExec("BEGIN; UPDATE rasters SET writer_stage_id = " + std::to_string(stage.value()) +
                        ", level = 2, file_name = '" + fileName +
                        "', bytes = 1234, populated = 1 WHERE id = 1; "
                        "UPDATE meta SET value = value + 1 WHERE key = 'rasters_revision'; COMMIT;"));
    // The row says populated, but the file is missing.
    EXPECT_FALSE(R.isPopulated(p));
    EXPECT_EQ(R.bytesOnDisk(p), 0);
    EXPECT_THROW(R.writerStage(p), InvalidArgumentError);

    const std::filesystem::path directory = std::filesystem::u8path(R.directory());
    std::filesystem::create_directories(directory);
    writeFile(directory / fileName, "not really a tiff");
    EXPECT_TRUE(R.isPopulated(p));
    EXPECT_FALSE(R.isPopulated(q));
    EXPECT_EQ(R.writerStage(p), stage);
    EXPECT_EQ(R.writtenAtLevel(p), 2);
    EXPECT_EQ(R.bytesOnDisk(p), 1234);  // the row's size, recorded at publication
    EXPECT_EQ(R.bytesOnDisk(q), 0);
    EXPECT_EQ(R.totalBytesOnDisk(), 1234);

    // A store copied without its -rasters directory reads as declared, unpopulated.
    std::filesystem::remove(directory / fileName);
    EXPECT_FALSE(R.isPopulated(p));
    EXPECT_EQ(R.totalBytesOnDisk(), 0);
    EXPECT_THROW(R.writtenAtLevel(p), InvalidArgumentError);
}

TEST_F(RasterRegistryTest, LaterMembersAreNotYetImplemented) {
    RasterRegistry& R = store().rasters();
    const RasterId p = R.declare(RasterDeclaration("probability", DataType::Float32, 1));
    const StageId stage = store().stageStateWriter().declare("segment", "{}");
    EXPECT_THROW(R.writerForStage(stage, p), InternalError);
    EXPECT_THROW(R.sourceFor(p), InternalError);
    EXPECT_THROW(R.discardRasterOfStage(stage), InternalError);
    EXPECT_THROW(R.removeUnreferencedFiles(), InternalError);
}

TEST(RasterDeclaration, CopyMoveAndGetters) {
    RasterDeclaration d("probability", DataType::Float32, 2);
    EXPECT_STREQ(d.name(), "probability");
    EXPECT_EQ(d.planeType(), DataType::Float32);
    EXPECT_EQ(d.planeCount(), 2);
    EXPECT_EQ(d.compressionValue(), RasterCompression::Deflate);
    EXPECT_STREQ(d.descriptionText(), "");
    d.compression(RasterCompression::Lzw).description("tumour p");
    EXPECT_EQ(d.compressionValue(), RasterCompression::Lzw);
    EXPECT_STREQ(d.descriptionText(), "tumour p");
    d.description(nullptr);
    EXPECT_STREQ(d.descriptionText(), "");
    d.description("tumour p");

    // The constructor and setters never validate.
    const RasterDeclaration odd(nullptr, DataType::Unknown, -3);
    EXPECT_EQ(odd.name(), nullptr);
    EXPECT_EQ(odd.planeType(), DataType::Unknown);
    EXPECT_EQ(odd.planeCount(), -3);

    RasterDeclaration copy(d);
    copy.description("changed");
    EXPECT_STREQ(d.descriptionText(), "tumour p");
    EXPECT_STREQ(copy.descriptionText(), "changed");
    EXPECT_STREQ(copy.name(), "probability");

    RasterDeclaration assigned("other", DataType::Byte, 1);
    assigned = d;
    EXPECT_STREQ(assigned.name(), "probability");
    EXPECT_EQ(assigned.compressionValue(), RasterCompression::Lzw);

    RasterDeclaration moved(std::move(d));
    EXPECT_STREQ(moved.name(), "probability");
    EXPECT_EQ(moved.planeCount(), 2);
    // The moved-from declaration is valid to destroy and to assign.
    d = copy;  // NOLINT(bugprone-use-after-move)
    EXPECT_STREQ(d.descriptionText(), "changed");
    RasterDeclaration movedAssigned("x", DataType::Byte, 1);
    movedAssigned = std::move(moved);
    EXPECT_EQ(movedAssigned.planeType(), DataType::Float32);
    moved = RasterDeclaration("again", DataType::Int8, 5);  // NOLINT(bugprone-use-after-move)
    EXPECT_STREQ(moved.name(), "again");
    {
        RasterDeclaration doomed("doomed", DataType::Byte, 1);
        RasterDeclaration taker(std::move(doomed));
    }  // destroying a moved-from declaration is fine
}

TEST(RasterFiles, GeneratedNamesMatchTheSweepPattern) {
    const std::string stem = detail::generateRasterFileStem();
    ASSERT_EQ(stem.size(), 32u);
    EXPECT_NE(detail::generateRasterFileStem(), stem);
    EXPECT_TRUE(detail::isGeneratedRasterFileName(stem + ".tif"));
    EXPECT_TRUE(detail::isGeneratedRasterFileName(stem + ".spool"));
    EXPECT_TRUE(detail::isGeneratedRasterFileName("0123456789abcdef0123456789abcdef.tif"));
    for (const char* other : {"0123456789ABCDEF0123456789abcdef.tif", "0123456789abcdef0123456789abcde.tif",
                              "0123456789abcdef0123456789abcdef0.tif", "0123456789abcdef0123456789abcdef.tiff",
                              "0123456789abcdef0123456789abcdef.tif.bak", "0123456789abcdef0123456789abcdef.",
                              "0123456789abcdef0123456789abcdef", "0123456789abcdeg0123456789abcdef.tif",
                              "notes.txt", ""}) {
        EXPECT_FALSE(detail::isGeneratedRasterFileName(other)) << other;
    }
    EXPECT_EQ(detail::rasterDirectory("dir/w.store").u8string(), std::filesystem::u8path("dir/w.store-rasters").u8string());
}
