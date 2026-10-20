// Requirements: UR-RES-12, UR-BLD-5
#include <slideio/processor/storage/store_format.hpp>
#include <slideio/processor/storage/types.hpp>

#include <gtest/gtest.h>
#include <sqlite3.h>

using namespace slideio::processor;

TEST(StorageTypes, EnumeratorValuesArePinned) {  // persisted: never renumber
    EXPECT_EQ(static_cast<int32_t>(OpenMode::CreateNew), 2);
    EXPECT_EQ(static_cast<int32_t>(LabelOrigin::ImportRegistered), 2);
    EXPECT_EQ(static_cast<int32_t>(StageStatus::Failed), 3);
    EXPECT_EQ(static_cast<int32_t>(EventKind::LayerDeclared), 0);
    EXPECT_EQ(static_cast<int32_t>(EventKind::RasterDropped), 10);
}

TEST(StorageTypes, StoreFormatVersionIsOne) {
    EXPECT_EQ(storeFormatVersion(), 1);
}

TEST(StorageTypes, SqliteHasRtree) {  // UR-BLD-5
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    EXPECT_EQ(sqlite3_exec(db, "CREATE VIRTUAL TABLE t USING rtree(id, x0, x1, y0, y1)",
                           nullptr, nullptr, nullptr),
              SQLITE_OK);
    sqlite3_close(db);
}
