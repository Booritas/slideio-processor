// Test-only: one statement on a store file through a raw sqlite3 connection.
#pragma once

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <filesystem>

namespace slideio::processor {

// Runs `sql` on the database at `path` through a connection of its own; fails the test on error.
// Rule (storage/helper_process.hpp): this uses the test's own SQLite copy, so call it only
// while no Store has the file open.
inline void rawExec(const std::filesystem::path& path, const char* sql) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open_v2(path.u8string().c_str(), &db, SQLITE_OPEN_READWRITE, nullptr), SQLITE_OK);
    char* error = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &error);
    EXPECT_EQ(rc, SQLITE_OK) << (error != nullptr ? error : "");
    sqlite3_free(error);
    sqlite3_close(db);
}

}  // namespace slideio::processor
