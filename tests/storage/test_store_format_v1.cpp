// Requirements: UR-RES-12
// Store format v1 is frozen (ruling S53), and this library no longer writes it: a fresh store
// is version 2, so the v1 generator is gone and the v1 fixtures can never be regenerated. The
// checked-in golden store, written by v1, must still match the v1 schema snapshot and open
// read-only without being changed, reading back its content. A writable open migrates it to
// version 2; test_store_format_v2.cpp tests that, with this golden store as its input. Only a
// SQLite upgrade could change the snapshot's three rtree shadow-table sql lines
// (object_rtree_node, _parent and _rowid), and the golden store was written by the SQLite of
// its day, so the snapshot stays as it is.
#include "storage/store_format_golden.hpp"

#include "common/temp_path.hpp"

#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/storage/store.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

using namespace slideio::processor;
using namespace slideio::processor::golden;

namespace {

constexpr const char* kSchemaFile = "store-v1.schema.sql";
constexpr const char* kSchemaActualFile = "store-v1.schema.actual.sql";
constexpr const char* kGoldenFile = "store-v1.golden.store";
constexpr const char* kFrozenMessage =
    "store format v1 is frozen: the v1 golden store no longer matches its snapshot (SDS 03 §3.12)";

// A copy of the v1 golden store in `dir`.
std::filesystem::path copyGolden(const TempDirGuard& dir) {
    const std::filesystem::path golden = dataDir() / kGoldenFile;
    EXPECT_TRUE(std::filesystem::exists(golden)) << golden.u8string();
    const std::filesystem::path copy = dir.path() / kGoldenFile;
    copyStore(golden, copy);
    return copy;
}

}  // namespace

TEST(StoreFormatV1, GoldenStoreSchemaMatchesTheV1Snapshot) {
    // Dumped from a copy: even a read-only SQLite open may leave -wal and -shm beside the file.
    TempDirGuard dir("golden_v1_schema");
    const std::filesystem::path copy = copyGolden(dir);
    expectSchemaMatchesSnapshot(schemaDump(copy), kSchemaFile, kSchemaActualFile, kFrozenMessage);
}

TEST(StoreFormatV1, GoldenStoreStillOpens) {
    TempDirGuard dir("golden_v1");
    const std::filesystem::path copy = copyGolden(dir);
    StorePtr store = Store::open(copy.u8string().c_str(), OpenMode::ReadOnly, Configuration());
    expectGoldenContent(*store, 1, GoldenContent::V1);
    store->close();
}

TEST(StoreFormatV1, GoldenStoreOpensReadOnlyWithoutMigrating) {
    TempDirGuard dir("golden_v1_ro");
    const std::filesystem::path copy = copyGolden(dir);
    StorePtr store = Store::open(copy.u8string().c_str(), OpenMode::ReadOnly, Configuration());
    EXPECT_EQ(store->formatVersion(), 1);
    expectGoldenContent(*store, 1, GoldenContent::V1);  // every staleSinceUnixMs is 0
    store->close();
    store.reset();
    const std::string original = readFile(dataDir() / kGoldenFile);
    ASSERT_FALSE(original.empty());
    EXPECT_TRUE(readFile(copy) == original) << "a read-only open changed the v1 store";
}
