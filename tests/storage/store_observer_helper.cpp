// Requirements: UR-OBS-1, UR-OBS-3, UR-OBS-6
//
// Test-only: another process on a store, for the storage tests.
//
//   store_observer_helper <store-path> <mode> <file> [N]
//
//   query      Opens the store ReadOnly with the default Configuration. For 3 s,
//              repeatedly counts the first declared layer's objects with
//              StageVisibility::IncludeInFlight and writes each count on its own line
//              of the out-file <file>.
//   subscribe  Opens the store ReadOnly with the default Configuration. Reads events
//              from kFromBeginning until the ObjectsAdded counts total N, writing each
//              ObjectsAdded sequence on its own line of the out-file <file>. Gives up
//              after 30 s.
//   exec       Runs the SQL in <file> on a raw sqlite3 connection to the store file,
//              as a foreign writer would, and closes it. It never opens a Store.
// Every line is flushed as it is written. Exits 0 on success; on any exception or SQL
// error prints it to stderr and exits 1; on bad arguments or the subscribe deadline,
// exits 2.
//
// Rule: this process carries two SQLite copies on Linux, its own and the hidden one
// inside slideio-processor-data. A mode uses exactly one of them on the store file:
// exec uses only the raw connection, query and subscribe only the Store. POSIX locks
// are per process and each copy tracks them on its own, so two copies on one file in
// one process can delete the WAL under each other. Between processes they are safe.

#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/object_query.hpp>
#include <slideio/processor/storage/event_journal.hpp>
#include <slideio/processor/storage/layer_registry.hpp>
#include <slideio/processor/storage/object_store.hpp>
#include <slideio/processor/storage/store.hpp>

#include <sqlite3.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace slideio::processor;

namespace {

using Clock = std::chrono::steady_clock;

int runQuery(Store& store, std::ofstream& out) {
    if (store.layers().count() < 2) {
        std::fprintf(stderr, "store_observer_helper: the store declares no layer\n");
        return 2;
    }
    const LayerId layer = store.layers().at(1);  // at(0) is the pixel layer
    ObjectQuery query;
    query.layer(layer).visibility(StageVisibility::IncludeInFlight);
    const Clock::time_point end = Clock::now() + std::chrono::seconds(3);
    while (Clock::now() < end) {
        out << store.objects().count(query) << '\n' << std::flush;
    }
    return 0;
}

int runSubscribe(Store& store, std::ofstream& out, int64_t expectedTotal) {
    EventCursorPtr cursor = store.events().subscribe(EventJournal::kFromBeginning);
    const Clock::time_point deadline = Clock::now() + std::chrono::seconds(30);
    int64_t total = 0;
    while (total < expectedTotal) {
        if (Clock::now() >= deadline) {
            std::fprintf(stderr, "store_observer_helper: saw %lld of %lld objects in 30 s\n",
                         static_cast<long long>(total), static_cast<long long>(expectedTotal));
            return 2;
        }
        if (!cursor->next(1000)) continue;
        const Event& event = cursor->current();
        if (event.kind() != EventKind::ObjectsAdded) continue;
        total += event.objectCount();
        out << event.sequence() << '\n' << std::flush;
    }
    return 0;
}

int runExec(const char* storePath, const char* sqlPath) {
    std::ifstream in(std::filesystem::u8path(sqlPath), std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "store_observer_helper: cannot read '%s'\n", sqlPath);
        return 2;
    }
    const std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    sqlite3* db = nullptr;
    int rc = sqlite3_open_v2(storePath, &db, SQLITE_OPEN_READWRITE, nullptr);
    char* error = nullptr;
    if (rc == SQLITE_OK) {
        sqlite3_busy_timeout(db, 5000);
        rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error);
    }
    if (rc != SQLITE_OK) {
        std::fprintf(stderr, "store_observer_helper: %s\n",
                     error != nullptr ? error : (db != nullptr ? sqlite3_errmsg(db) : "cannot open"));
    }
    sqlite3_free(error);
    sqlite3_close(db);
    return rc == SQLITE_OK ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const std::string mode = argc >= 4 ? argv[2] : "";
        if (!((mode == "query" && argc == 4) || (mode == "subscribe" && argc == 5) ||
              (mode == "exec" && argc == 4))) {
            std::fprintf(stderr,
                         "usage: store_observer_helper <store-path> query <out-file>\n"
                         "       store_observer_helper <store-path> subscribe <out-file> <N>\n"
                         "       store_observer_helper <store-path> exec <sql-file>\n");
            return 2;
        }
        if (mode == "exec") return runExec(argv[1], argv[3]);
        std::ofstream out(std::filesystem::u8path(argv[3]), std::ios::binary | std::ios::trunc);
        if (!out) {
            std::fprintf(stderr, "store_observer_helper: cannot write '%s'\n", argv[3]);
            return 2;
        }
        StorePtr store = Store::open(argv[1], OpenMode::ReadOnly, Configuration());
        const int rc = mode == "query" ? runQuery(*store, out)
                                       : runSubscribe(*store, out, std::strtoll(argv[4], nullptr, 10));
        store->close();
        return rc;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "store_observer_helper: %s\n", e.what());
        return 1;
    }
}
