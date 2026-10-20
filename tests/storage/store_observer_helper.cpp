// Requirements: UR-OBS-1, UR-OBS-3, UR-OBS-6
//
// Test-only: an observer in another process, for test_cross_process.cpp.
//
//   store_observer_helper <store-path> <mode> <out-file> [N]
//
// It opens the store ReadOnly with the default Configuration.
//   query      For 3 s, repeatedly counts the first declared layer's objects with
//              StageVisibility::IncludeInFlight and writes each count on its own line.
//   subscribe  Reads events from kFromBeginning until the ObjectsAdded counts total N,
//              writing each ObjectsAdded sequence on its own line. Gives up after 30 s.
// Every line is flushed as it is written. Exits 0 on success; on any exception prints
// what() to stderr and exits 1; on bad arguments or the subscribe deadline, exits 2.

#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/object_query.hpp>
#include <slideio/processor/storage/event_journal.hpp>
#include <slideio/processor/storage/layer_registry.hpp>
#include <slideio/processor/storage/object_store.hpp>
#include <slideio/processor/storage/store.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
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

} // namespace

int main(int argc, char** argv) {
    try {
        const std::string mode = argc >= 4 ? argv[2] : "";
        if (!((mode == "query" && argc == 4) || (mode == "subscribe" && argc == 5))) {
            std::fprintf(stderr,
                         "usage: store_observer_helper <store-path> query <out-file>\n"
                         "       store_observer_helper <store-path> subscribe <out-file> <N>\n");
            return 2;
        }
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
