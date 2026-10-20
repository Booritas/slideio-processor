// Requirements: UR-RES-12, UR-BLD-5, UR-OBS-1, UR-RES-11
#include "storage/store_fixture.hpp"

#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/provenance.hpp>
#include <slideio/processor/core/version.hpp>
#include <slideio/processor/storage/store.hpp>
#include <slideio/processor/storage/types.hpp>

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace slideio::processor;

namespace {

// Runs one statement on the file through a raw sqlite3 connection.
void rawExec(const std::string& path, const char* sql) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr), SQLITE_OK);
    char* error = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &error);
    EXPECT_EQ(rc, SQLITE_OK) << (error != nullptr ? error : "");
    sqlite3_free(error);
    sqlite3_close(db);
}

// Reads the first column of the first row of a query, as text; "" when there is no row.
std::string rawQueryText(const std::string& path, const char* sql) {
    sqlite3* db = nullptr;
    std::string result;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char* text = sqlite3_column_text(stmt, 0);
            result = text != nullptr ? reinterpret_cast<const char*>(text) : "";
        }
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
    return result;
}

bool contains(const char* text, const char* fragment) {
    return std::string(text).find(fragment) != std::string::npos;
}

} // namespace

TEST(Store, CreateReopenAndModes) {
    StoreFilesGuard files;
    StoreFilesGuard missingFiles("missing");
    const std::string path = files.utf8();
    const std::string missing = missingFiles.utf8();
    const Configuration cfg;

    auto s = Store::open(path.c_str(), OpenMode::CreateNew, cfg);
    EXPECT_EQ(s->formatVersion(), 1);
    EXPECT_EQ(s->mode(), OpenMode::CreateNew);
    EXPECT_FALSE(s->isReadOnly());
    EXPECT_STREQ(s->path(), path.c_str());
    s->close();
    s->close();  // idempotent
    EXPECT_THROW(Store::open(path.c_str(), OpenMode::CreateNew, cfg), IoError);  // exists

    auto r = Store::open(path.c_str(), OpenMode::ReadOnly, cfg);
    EXPECT_TRUE(r->isReadOnly());
    EXPECT_EQ(r->mode(), OpenMode::ReadOnly);
    EXPECT_EQ(r->formatVersion(), 1);
    r->close();

    auto w = Store::open(path.c_str(), OpenMode::ReadWrite, cfg);
    EXPECT_FALSE(w->isReadOnly());
    EXPECT_EQ(w->mode(), OpenMode::ReadWrite);
    w->close();

    try {
        Store::open(missing.c_str(), OpenMode::ReadWrite, cfg);
        FAIL() << "opened a missing store";
    } catch (const IoError& e) {
        EXPECT_TRUE(contains(e.what(), missing.c_str())) << e.what();
    }
    EXPECT_THROW(Store::open(missing.c_str(), OpenMode::ReadOnly, cfg), IoError);
    EXPECT_FALSE(std::filesystem::exists(missingFiles.path()));

    EXPECT_THROW(Store::open(nullptr, OpenMode::ReadOnly, cfg), InvalidArgumentError);
    EXPECT_THROW(Store::open("", OpenMode::ReadOnly, cfg), InvalidArgumentError);
}

TEST(Store, SchemaAndMetaAreWrittenAtCreation) {
    StoreFilesGuard files;
    const std::string path = files.utf8();
    Store::open(path.c_str(), OpenMode::CreateNew, Configuration())->close();

    for (const char* table : {"meta", "layers", "labels", "stages", "stage_deps", "stage_layers", "objects",
                              "object_rtree", "measurements", "measurement_names", "events"}) {
        const std::string sql = std::string("SELECT name FROM sqlite_master WHERE name = '") + table + "'";
        EXPECT_EQ(rawQueryText(path, sql.c_str()), table);
    }
    EXPECT_EQ(rawQueryText(path, "PRAGMA journal_mode"), "wal");
    EXPECT_EQ(rawQueryText(path, "SELECT value FROM meta WHERE key = 'format_version'"), "1");
    EXPECT_EQ(rawQueryText(path, "SELECT value FROM meta WHERE key = 'import_policy'"), "0");
    EXPECT_FALSE(rawQueryText(path, "SELECT value FROM meta WHERE key = 'vocabulary_version'").empty());
    EXPECT_FALSE(rawQueryText(path, "SELECT value FROM meta WHERE key = 'journal_truncated_below'").empty());
    const std::string uuid = rawQueryText(path, "SELECT value FROM meta WHERE key = 'store_uuid'");
    EXPECT_EQ(uuid.size(), 32u);
    EXPECT_EQ(uuid.find_first_not_of("0123456789abcdef"), std::string::npos) << uuid;
    EXPECT_EQ(rawQueryText(path, "SELECT name FROM layers WHERE id = 1 AND kind = 0 AND depth = 0 "
                                 "AND parent_id IS NULL"),
              "pixel");

    StoreFilesGuard other;
    Store::open(other.utf8().c_str(), OpenMode::CreateNew, Configuration())->close();
    EXPECT_NE(rawQueryText(other.utf8(), "SELECT value FROM meta WHERE key = 'store_uuid'"), uuid);
}

TEST(Store, IncompatibleVersionIsRefusedNamingBoth) {  // UR-RES-12
    StoreFilesGuard files;
    const std::string path = files.utf8();
    const Configuration cfg;
    Store::open(path.c_str(), OpenMode::CreateNew, cfg)->close();
    rawExec(path, "UPDATE meta SET value = 2 WHERE key = 'format_version'");

    for (OpenMode mode : {OpenMode::ReadOnly, OpenMode::ReadWrite}) {
        try {
            Store::open(path.c_str(), mode, cfg);
            FAIL() << "opened a version-2 store";
        } catch (const StoreFormatError& e) {
            EXPECT_EQ(e.storeVersion(), 2);
            EXPECT_EQ(e.libraryVersion(), 1);
            EXPECT_TRUE(contains(e.what(), "2")) << e.what();
            EXPECT_TRUE(contains(e.what(), "1")) << e.what();
        }
    }
    // The refusal changed nothing in the file.
    EXPECT_EQ(rawQueryText(path, "SELECT value FROM meta WHERE key = 'format_version'"), "2");
}

TEST(Store, NonStoreFileIsAFormatError) {
    StoreFilesGuard files;
    const Configuration cfg;
    writeFile(files.path(), "hello");
    for (OpenMode mode : {OpenMode::ReadOnly, OpenMode::ReadWrite}) {
        try {
            Store::open(files.utf8().c_str(), mode, cfg);
            FAIL() << "opened a text file";
        } catch (const StoreFormatError& e) {
            EXPECT_EQ(e.storeVersion(), 0);
            EXPECT_EQ(e.libraryVersion(), 1);
        }
    }
    EXPECT_EQ(readFile(files.path()), "hello");

    // A SQLite database without a meta table.
    StoreFilesGuard plain;
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(plain.utf8().c_str(), &db), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db, "CREATE TABLE t(x)", nullptr, nullptr, nullptr), SQLITE_OK);
    sqlite3_close(db);
    try {
        Store::open(plain.utf8().c_str(), OpenMode::ReadOnly, cfg);
        FAIL() << "opened a database without meta";
    } catch (const StoreFormatError& e) {
        EXPECT_EQ(e.storeVersion(), 0);
    }
}

TEST(Store, SecondWriterIsRefusedReaderIsNot) {
    StoreFilesGuard files;
    const std::string path = files.utf8();
    const Configuration cfg;
    Store::open(path.c_str(), OpenMode::CreateNew, cfg)->close();

    auto w = Store::open(path.c_str(), OpenMode::ReadWrite, cfg);
    try {
        Store::open(path.c_str(), OpenMode::ReadWrite, cfg);
        FAIL() << "a second writer was admitted";
    } catch (const IoError& e) {
        EXPECT_TRUE(contains(e.what(), "process")) << e.what();
        EXPECT_TRUE(contains(e.what(), path.c_str())) << e.what();
    }
    EXPECT_NO_THROW(Store::open(path.c_str(), OpenMode::ReadOnly, cfg));
    w->close();
    EXPECT_NO_THROW(Store::open(path.c_str(), OpenMode::ReadWrite, cfg));  // lock released
}

TEST(Store, ClosedAndReadOnlyRefuseWork) {
    StoreFilesGuard files;
    const std::string path = files.utf8();
    const Configuration cfg;
    auto s = Store::open(path.c_str(), OpenMode::CreateNew, cfg);
    s->close();
    try {
        s->provenance();
        FAIL() << "a closed store answered";
    } catch (const IoError& e) {
        EXPECT_TRUE(contains(e.what(), "store is closed")) << e.what();
    }
    EXPECT_THROW(s->setProvenance(Provenance()), IoError);
    EXPECT_STREQ(s->path(), path.c_str());
    EXPECT_EQ(s->formatVersion(), 1);
    EXPECT_EQ(s->mode(), OpenMode::CreateNew);

    auto r = Store::open(path.c_str(), OpenMode::ReadOnly, cfg);
    try {
        r->setProvenance(Provenance());
        FAIL() << "a read-only store accepted a write";
    } catch (const IoError& e) {
        EXPECT_TRUE(contains(e.what(), "read-only")) << e.what();
        EXPECT_TRUE(contains(e.what(), path.c_str())) << e.what();
    }
    EXPECT_NO_THROW(r->provenance());
}

TEST(Store, ProvenanceRoundTripsStoredVersionAndTime) {  // UR-RES-11
    StoreFilesGuard files;
    const std::string path = files.utf8();
    const Configuration cfg;
    auto s = Store::open(path.c_str(), OpenMode::CreateNew, cfg);

    const Provenance initial = s->provenance();
    EXPECT_STREQ(initial.slideId(), "");
    EXPECT_STREQ(initial.libraryVersion(), libraryVersion());
    EXPECT_GT(initial.createdAtUnixMs(), 0);

    Provenance p;
    p.setSlideId("s");
    p.setSceneIndex(1);
    s->setProvenance(p);
    const int64_t created = s->provenance().createdAtUnixMs();
    EXPECT_EQ(created, p.createdAtUnixMs());
    s->close();

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto r = Store::open(path.c_str(), OpenMode::ReadWrite, cfg);
    const Provenance back = r->provenance();
    EXPECT_STREQ(back.slideId(), "s");
    EXPECT_EQ(back.sceneIndex(), 1);
    EXPECT_STREQ(back.libraryVersion(), libraryVersion());
    EXPECT_EQ(back.createdAtUnixMs(), created);
    r->close();

    // The stored version is the one returned, not the running library's.
    rawExec(path, "UPDATE meta SET value = '{\"slideId\":\"x\",\"sceneIndex\":3,\"libraryVersion\":\"0.0.1\","
                  "\"createdAtUnixMs\":1234}' WHERE key = 'provenance'");
    auto o = Store::open(path.c_str(), OpenMode::ReadOnly, cfg);
    const Provenance old = o->provenance();
    EXPECT_STREQ(old.slideId(), "x");
    EXPECT_EQ(old.sceneIndex(), 3);
    EXPECT_STREQ(old.libraryVersion(), "0.0.1");
    EXPECT_EQ(old.createdAtUnixMs(), 1234);
    o->close();

    rawExec(path, "UPDATE meta SET value = '{\"slideId\":1}' WHERE key = 'provenance'");
    EXPECT_THROW(Store::open(path.c_str(), OpenMode::ReadOnly, cfg)->provenance(), InternalError);
}

TEST(Store, NonAsciiPath) {  // Review Focus 5
    StoreFilesGuard files(u8"ü_日本");
    const std::string path = files.utf8();
    const Configuration cfg;
    auto s = Store::open(path.c_str(), OpenMode::CreateNew, cfg);
    s->close();
    EXPECT_TRUE(std::filesystem::exists(files.path()));
    auto w = Store::open(path.c_str(), OpenMode::ReadWrite, cfg);
    EXPECT_NO_THROW(w->provenance());
    w->close();
}

TEST(Store, ConcurrentReadsFromManyThreads) {  // UR-OBS-1: each thread checks out its own connection
    StoreFilesGuard files;
    auto s = Store::open(files.utf8().c_str(), OpenMode::CreateNew, Configuration());
    Provenance p;
    p.setSlideId("shared");
    s->setProvenance(p);
    std::vector<std::thread> threads;
    std::atomic<int> failures{0};
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 50; ++i) {
                try {
                    if (std::string(s->provenance().slideId()) != "shared") ++failures;
                } catch (...) {
                    ++failures;
                }
            }
        });
    }
    for (auto& t : threads) t.join();
    EXPECT_EQ(failures.load(), 0);
    s->close();
}

TEST_F(StoreFixture, FixtureCreatesAWritableStore) {
    EXPECT_EQ(store().mode(), OpenMode::CreateNew);
    StorePtr r = reopen(OpenMode::ReadOnly);
    EXPECT_TRUE(r->isReadOnly());
}
