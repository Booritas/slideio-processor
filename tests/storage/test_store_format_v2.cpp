// Requirements: UR-RES-12
// Store format v2 (SDS 03 §3.12): v1 plus stages.stale_since_ms. Frozen as v1 is (ruling
// S53): a fresh store's schema must match the checked-in snapshot, and the checked-in golden
// store, written by v2, must still open and read back its content. A v1 store opened writable
// is migrated, and then has the schema of a fresh v2 store, apart from the three rtree
// shadow-table sql lines (object_rtree_node, _parent and _rowid), which it keeps as the
// v1 golden store's SQLite wrote them, so that comparison skips them. The disabled generator
// writes the v2 fixtures; never run it once they are checked in. Only a SQLite upgrade may
// change the snapshot, and only those three lines; regenerate the snapshot then, deliberately.
#include "common/raw_sqlite.hpp"
#include "storage/detail/schema.hpp"
#include "storage/store_format_golden.hpp"
#include "storage/store_fixture.hpp"

#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/storage/store.hpp>

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <filesystem>
#include <string>

using namespace slideio::processor;
using namespace slideio::processor::golden;

namespace {

constexpr const char* kSchemaFile = "store-v2.schema.sql";
constexpr const char* kSchemaActualFile = "store-v2.schema.actual.sql";
constexpr const char* kMigratedActualFile = "store-v2.schema.migrated.actual.sql";
constexpr const char* kGoldenFile = "store-v2.golden.store";
constexpr const char* kV1GoldenFile = "store-v1.golden.store";
constexpr const char* kFrozenMessage =
    "store format v2 is frozen: a schema change needs format_version 3 and a migration (SDS 03 §3.12)";
constexpr const char* kMigratedMessage =
    "a migrated v1 store differs from a fresh v2 store: the migration and the v2 DDL disagree (SDS 03 §3.12)";

}  // namespace

TEST(StoreFormatV2, SchemaMatchesTheFrozenSnapshot) {
    expectSchemaMatchesSnapshot(freshSchemaDump(), kSchemaFile, kSchemaActualFile, kFrozenMessage);
}

TEST(StoreFormatV2, MigratedV1SchemaMatchesTheV2Snapshot) {
    TempDirGuard dir("golden_v1_migrated");
    const std::filesystem::path copy = dir.path() / kV1GoldenFile;
    copyStore(dataDir() / kV1GoldenFile, copy);

    StorePtr store = Store::open(copy.u8string().c_str(), OpenMode::ReadWrite, Configuration());
    EXPECT_EQ(store->formatVersion(), 2);
    expectGoldenContent(*store, 2, GoldenContent::V1);  // every staleSinceUnixMs is 0
    store->close();
    store.reset();

    expectSchemaMatchesSnapshot(schemaDump(copy), kSchemaFile, kMigratedActualFile, kMigratedMessage,
                                SchemaLines::WithoutRtreeShadowTables);
    StorePtr reader = Store::open(copy.u8string().c_str(), OpenMode::ReadOnly, Configuration());
    EXPECT_EQ(reader->formatVersion(), 2);
    reader->close();
}

// A migration that fails leaves the store at version 1, throws IoError, and releases the
// writer lock: a column of the same name, added behind the library's back, makes it fail.
TEST(StoreFormatV2, FailedMigrationLeavesVersionOne) {
    TempDirGuard dir("golden_v1_failed");
    const std::filesystem::path copy = dir.path() / kV1GoldenFile;
    copyStore(dataDir() / kV1GoldenFile, copy);
    rawExec(copy, "ALTER TABLE stages ADD COLUMN stale_since_ms INTEGER");

    EXPECT_THROW(Store::open(copy.u8string().c_str(), OpenMode::ReadWrite, Configuration()), IoError);
    StorePtr reader = Store::open(copy.u8string().c_str(), OpenMode::ReadOnly, Configuration());
    EXPECT_EQ(reader->formatVersion(), 1);
    reader->close();
    reader.reset();

    // With the obstacle gone, the lock is free and the migration runs.
    rawExec(copy, "ALTER TABLE stages DROP COLUMN stale_since_ms");
    StorePtr store = Store::open(copy.u8string().c_str(), OpenMode::ReadWrite, Configuration());
    EXPECT_EQ(store->formatVersion(), 2);
    expectGoldenContent(*store, 2, GoldenContent::V1);
    store->close();
}

// The migration reads the version again under the writer lock: Store::open reads version 1
// before it takes the lock, and another process may upgrade the store in between. Driven
// directly, as the race cannot be timed from a test: on a store already upgraded, a second
// run changes nothing and does not fail.
TEST(StoreFormatV2, MigrationOfAnUpgradedStoreChangesNothing) {
    TempDirGuard dir("golden_v1_twice");
    const std::filesystem::path copy = dir.path() / kV1GoldenFile;
    copyStore(dataDir() / kV1GoldenFile, copy);

    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open_v2(copy.u8string().c_str(), &db, SQLITE_OPEN_READWRITE, nullptr), SQLITE_OK);
    for (const bool expectUpgrade : {true, false}) {
        SCOPED_TRACE(expectUpgrade ? "first run" : "second run");
        ASSERT_EQ(sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr), SQLITE_OK);
        EXPECT_EQ(detail::migrateSchemaFromVersion1(db), expectUpgrade);
        ASSERT_EQ(sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr), SQLITE_OK);
    }
    sqlite3_close(db);

    expectSchemaMatchesSnapshot(schemaDump(copy), kSchemaFile, kMigratedActualFile, kMigratedMessage,
                                SchemaLines::WithoutRtreeShadowTables);
    StorePtr store = Store::open(copy.u8string().c_str(), OpenMode::ReadWrite, Configuration());
    EXPECT_EQ(store->formatVersion(), 2);
    expectGoldenContent(*store, 2, GoldenContent::V1);
    store->close();
}

// A handle that opened a version 1 store read-only keeps reading it as version 1 while a
// writable open migrates the store under it: staleSinceUnixMs still answers 0.
TEST(StoreFormatV2, ReadOnlyV1HandleOutlivesAMigration) {
    TempDirGuard dir("golden_v1_reader");
    const std::filesystem::path copy = dir.path() / kV1GoldenFile;
    copyStore(dataDir() / kV1GoldenFile, copy);

    StorePtr reader = Store::open(copy.u8string().c_str(), OpenMode::ReadOnly, Configuration());
    StageId seg;
    ASSERT_TRUE(reader->stageStates().find("seg", seg));
    EXPECT_EQ(reader->stageStates().staleSinceUnixMs(seg), 0);

    StorePtr writer = Store::open(copy.u8string().c_str(), OpenMode::ReadWrite, Configuration());
    EXPECT_EQ(writer->formatVersion(), 2);
    writer->close();
    writer.reset();

    EXPECT_EQ(reader->formatVersion(), 1);
    EXPECT_EQ(reader->stageStates().staleSinceUnixMs(seg), 0);
    EXPECT_EQ(reader->stageStates().status(seg), StageStatus::Complete);
    reader->close();
}

TEST(StoreFormatV2, GoldenStoreStillOpens) {
    const std::filesystem::path golden = dataDir() / kGoldenFile;
    ASSERT_TRUE(std::filesystem::exists(golden)) << golden.u8string();
    TempDirGuard dir("golden_v2");
    const std::filesystem::path copy = dir.path() / kGoldenFile;
    copyStore(golden, copy);

    for (OpenMode mode : {OpenMode::ReadOnly, OpenMode::ReadWrite}) {
        SCOPED_TRACE(mode == OpenMode::ReadOnly ? "ReadOnly" : "ReadWrite");
        StorePtr store = Store::open(copy.u8string().c_str(), mode, Configuration());
        expectGoldenContent(*store, 2, GoldenContent::V2);
        store->close();
    }
}

// Writes both v2 fixtures into SLIDEIO_PROCESSOR_TEST_DATA_DIR. Never run it once they are
// checked in (see the top of this file); a version 3 writes store-v3.* fixtures of its own.
// Run: --gtest_also_run_disabled_tests --gtest_filter=StoreFormatV2.DISABLED_GenerateFixtures
TEST(StoreFormatV2, DISABLED_GenerateFixtures) {
    writeFile(dataDir() / kSchemaFile, freshSchemaDump());

    // Built in a temp directory and copied, so neither -wal, -shm nor -writer.lock reaches
    // the data directory.
    TempDirGuard dir("golden_v2_gen");
    const std::filesystem::path built = dir.path() / kGoldenFile;
    writeGoldenStore(built);
    const std::filesystem::path golden = dataDir() / kGoldenFile;
    removeStoreFiles(golden);
    copyStore(built, golden);
}
