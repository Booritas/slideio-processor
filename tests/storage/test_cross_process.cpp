// Requirements: UR-OBS-1, UR-OBS-3, UR-OBS-6
#include "storage/helper_process.hpp"
#include "storage/store_fixture.hpp"

#include "storage/detail/change_signal.hpp"

#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/core/object_record.hpp>
#include <slideio/processor/storage/event_journal.hpp>
#include <slideio/processor/storage/layer_registry.hpp>
#include <slideio/processor/storage/object_store.hpp>
#include <slideio/processor/storage/stage_state_writer.hpp>
#include <slideio/processor/storage/store.hpp>

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if !defined(_WIN32) && !defined(__APPLE__)
#include <cerrno>
#include <fcntl.h>
#include <semaphore.h>
#endif

using namespace slideio::processor;
namespace detail = slideio::processor::detail;

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kBatchObjects = 200;

Polygon square(int64_t x, int64_t y, int64_t side) {
    const Point64 points[] = {{x, y}, {x + side, y}, {x + side, y + side}, {x, y + side}};
    Polygon polygon;
    polygon.addRing(points, 4, false);
    return polygon;
}

// A store with one semantic layer and one Running stage, ready for batches.
struct WrittenStore {
    WrittenStore(const std::string& path, const Configuration& config) {
        store = Store::open(path.c_str(), OpenMode::CreateNew, config);
        layer = store->layers().declareSemantic("cells", store->layers().pixelLayer());
        stage = store->stageStateWriter().declare("segment", "");
        store->stageStateWriter().setStatus(stage, StageStatus::Running);
        openWriter();
    }

    // Closes the store: no writer holds the file until reopen().
    void close() {
        writer.reset();
        store->close();
    }

    // Opens the store ReadWrite again, with a new writer for the same stage.
    void reopen(const std::string& path, const Configuration& config) {
        store = Store::open(path.c_str(), OpenMode::ReadWrite, config);
        openWriter();
    }

    void openWriter() {
        writer = store->objects().writerForStage(stage);
        writer->setBatchSizeObjects(kBatchObjects);
    }

    // Emits one batch of kBatchObjects squares in row `batch`; the last one commits it.
    void writeBatch(int batch) {
        for (int i = 0; i < kBatchObjects; ++i) {
            ObjectRecord record;
            record.setLayer(layer);
            record.setGeometry(square(int64_t(i) * 10, int64_t(batch) * 10, 4));
            writer->emit(record);
        }
    }

    StorePtr store;
    LayerId layer;
    StageId stage;
    ObjectWriterPtr writer;
};

// A temp file removed when the guard goes out of scope.
class FileGuard {
public:
    explicit FileGuard(const char* stem) : path_(uniqueTempPath(stem, ".txt")) {}
    FileGuard(const FileGuard&) = delete;
    FileGuard& operator=(const FileGuard&) = delete;
    ~FileGuard() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

// Starts the helper in another thread; the future yields its exit code.
std::future<int> launchHelper(std::vector<std::string> args) {
    return std::async(std::launch::async, [args] { return runHelper(args); });
}

std::vector<int64_t> readLines(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::vector<int64_t> values;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) values.push_back(std::stoll(line));
    }
    return values;
}

// Waits until the helper has written its first line, or has exited, or 10 s pass.
void waitForFirstLine(const std::filesystem::path& out, std::future<int>& helper) {
    const Clock::time_point deadline = Clock::now() + std::chrono::seconds(10);
    while (Clock::now() < deadline && helper.wait_for(std::chrono::milliseconds(10)) != std::future_status::ready) {
        std::error_code ec;
        if (std::filesystem::file_size(out, ec) > 0 && !ec) return;
    }
}

std::vector<int64_t> objectsAddedSequences(Store& store) {
    std::vector<int64_t> sequences;
    EventCursorPtr cursor = store.events().subscribe(EventJournal::kFromBeginning);
    while (cursor->next(0)) {
        if (cursor->current().kind() == EventKind::ObjectsAdded) sequences.push_back(cursor->current().sequence());
    }
    return sequences;
}

// Reads events until `batches` ObjectsAdded events have arrived (or 20 s pass) and
// returns every sequence seen, in order.
std::vector<int64_t> followUntilBatches(EventCursorPtr cursor, int batches) {
    std::vector<int64_t> sequences;
    int added = 0;
    const Clock::time_point deadline = Clock::now() + std::chrono::seconds(20);
    while (added < batches && Clock::now() < deadline) {
        if (!cursor->next(1000)) continue;
        sequences.push_back(cursor->current().sequence());
        if (cursor->current().kind() == EventKind::ObjectsAdded) ++added;
    }
    return sequences;
}

// True when a wake-up signal can be opened here: the platform has one and, on Linux,
// /dev/shm is usable (a sandbox may deny it).
bool signalCanOpen() {
    if (!detail::ChangeSignal::isSupported()) return false;
    const std::string uuid = "probe" + std::to_string(Clock::now().time_since_epoch().count());
    detail::ChangeSignal probe(uuid, "probe", detail::ChangeSignal::Role::Writer);
    const bool open = probe.isOpen();
    probe.unlink();
    return open;
}

// Blocks `cursor` in next(5000) on another thread, runs `commit` 300 ms later, and
// expects the cursor to wake with the commit's event within 1000 ms of it, well inside
// the 2000 ms poll interval the stores use: only the signal can wake it that soon.
template <class Commit>
void expectPromptWake(const EventCursorPtr& cursor, Commit&& commit) {
    std::future<std::pair<bool, Clock::time_point>> woken = std::async(std::launch::async, [cursor] {
        const bool got = cursor->next(5000);
        return std::make_pair(got, Clock::now());
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));  // the cursor is blocked by now
    const Clock::time_point committed = Clock::now();
    commit();
    const std::pair<bool, Clock::time_point> result = woken.get();

    ASSERT_TRUE(result.first);
    EXPECT_EQ(cursor->current().kind(), EventKind::ObjectsAdded);
    const auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(result.second - committed);
    EXPECT_LT(latency.count(), 1000) << "the cursor woke " << latency.count() << " ms after the commit";
}

#if !defined(_WIN32) && !defined(__APPLE__)
// meta.store_uuid of the store at `path`, read directly.
// Rule (helper_process.hpp): the test's own SQLite copy; call it only while no Store has
// the file open.
std::string storeUuidOf(const std::string& path) {
    sqlite3* db = nullptr;
    std::string uuid;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
        sqlite3_stmt* select = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT value FROM meta WHERE key = 'store_uuid'", -1, &select, nullptr) ==
                SQLITE_OK &&
            sqlite3_step(select) == SQLITE_ROW) {
            uuid = reinterpret_cast<const char*>(sqlite3_column_text(select, 0));
        }
        sqlite3_finalize(select);
    }
    sqlite3_close(db);
    return uuid;
}
#endif

} // namespace

TEST(CrossProcess, ReaderNeverSeesAPartialBatchAndNeverErrors) {  // Review Focus 3
    StoreFilesGuard files("xproc_query");
    FileGuard out("xproc_query_out");
    WrittenStore written(files.utf8(), Configuration());

    std::future<int> helper = launchHelper({files.utf8(), "query", out.path().u8string()});
    waitForFirstLine(out.path(), helper);  // the reader is querying before the first batch lands
    for (int batch = 0; batch < 50; ++batch) {
        written.writeBatch(batch);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(written.store->objects().objectCountInLayer(written.layer), 50 * kBatchObjects);

    ASSERT_EQ(helper.get(), 0);
    const std::vector<int64_t> counts = readLines(out.path());
    ASSERT_FALSE(counts.empty());
    for (size_t i = 0; i < counts.size(); ++i) {
        EXPECT_EQ(counts[i] % kBatchObjects, 0) << "count " << i << " is " << counts[i];
        if (i > 0) EXPECT_GE(counts[i], counts[i - 1]) << "count " << i;
    }
    // The reader ran while the batches landed, so the checks above are not vacuous. The
    // helper was querying before the first commit, its 3 s window outlasts the ~1 s of
    // writes by far, and each count takes milliseconds, so it records many mid-write
    // counts; one is enough.
    EXPECT_TRUE(std::any_of(counts.begin(), counts.end(),
                            [](int64_t c) { return c > 0 && c < 50 * kBatchObjects; }))
        << "no count was taken while the batches were being written";
    written.store->close();
}

TEST(CrossProcess, SubscriberInAnotherProcessSeesEveryBatch) {
    StoreFilesGuard files("xproc_subscribe");
    FileGuard out("xproc_subscribe_out");
    WrittenStore written(files.utf8(), Configuration());

    std::future<int> helper =
        launchHelper({files.utf8(), "subscribe", out.path().u8string(), std::to_string(50 * kBatchObjects)});
    for (int batch = 0; batch < 50; ++batch) written.writeBatch(batch);

    ASSERT_EQ(helper.wait_for(std::chrono::seconds(20)), std::future_status::ready) << "the helper is still waiting";
    ASSERT_EQ(helper.get(), 0);
    const std::vector<int64_t> seen = readLines(out.path());
    EXPECT_EQ(seen.size(), 50u);
    EXPECT_EQ(seen, objectsAddedSequences(*written.store));
    written.store->close();
}

TEST(ChangeSignal, SignalAndPollingSeeTheSameSequence) {  // UR-OBS-6
    StoreFilesGuard files("signal_vs_poll");
    WrittenStore written(files.utf8(), Configuration());
    StorePtr signalled = Store::open(files.utf8().c_str(), OpenMode::ReadOnly, Configuration());
    StorePtr polling = Store::open(files.utf8().c_str(), OpenMode::ReadOnly,
                                   ConfigurationBuilder().eventSignalEnabled(false).build());
    EventCursorPtr viaSignal = signalled->events().subscribe(EventJournal::kFromBeginning);
    EventCursorPtr viaPolling = polling->events().subscribe(EventJournal::kFromBeginning);

    std::thread writer([&written] {
        for (int batch = 0; batch < 30; ++batch) {
            written.writeBatch(batch);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
    std::future<std::vector<int64_t>> signalSeen =
        std::async(std::launch::async, followUntilBatches, viaSignal, 30);
    const std::vector<int64_t> pollSeen = followUntilBatches(viaPolling, 30);
    writer.join();
    const std::vector<int64_t> fromSignal = signalSeen.get();

    EXPECT_EQ(fromSignal, pollSeen);
    // Not two empty runs: the followers saw every batch the journal holds.
    const std::vector<int64_t> journal = objectsAddedSequences(*signalled);
    EXPECT_EQ(journal.size(), 30u);
    EXPECT_TRUE(std::includes(pollSeen.begin(), pollSeen.end(), journal.begin(), journal.end()))
        << "the polling follower missed ObjectsAdded events";
    viaSignal->close();
    viaPolling->close();
    signalled->close();
    polling->close();
    written.store->close();
}

TEST(ChangeSignal, SignalWakesBeforeThePollInterval) {
    if (!signalCanOpen()) GTEST_SKIP() << "no wake-up signal can be opened here; cursors poll";
    StoreFilesGuard files("signal_latency");
    const Configuration config = ConfigurationBuilder().eventPollIntervalMs(2000).build();
    WrittenStore written(files.utf8(), config);
    StorePtr reader = Store::open(files.utf8().c_str(), OpenMode::ReadOnly, config);
    EventCursorPtr cursor = reader->events().subscribe(EventJournal::kFromNow);

    expectPromptWake(cursor, [&written] { written.writeBatch(0); });
    cursor->close();
    reader->close();
    written.store->close();
}

TEST(ChangeSignal, SignalReachesAReaderOpenedBeforeTheWriter) {
    if (!signalCanOpen()) GTEST_SKIP() << "no wake-up signal can be opened here; cursors poll";
    StoreFilesGuard files("signal_late_writer");
    const Configuration config = ConfigurationBuilder().eventPollIntervalMs(2000).build();
    WrittenStore written(files.utf8(), config);
    written.close();
    // No writer while the reader opens: on Linux the semaphore's name does not exist.
    StorePtr reader = Store::open(files.utf8().c_str(), OpenMode::ReadOnly, config);
    EventCursorPtr cursor = reader->events().subscribe(EventJournal::kFromNow);
    written.reopen(files.utf8(), config);

    expectPromptWake(cursor, [&written] { written.writeBatch(0); });
    cursor->close();
    reader->close();
    written.store->close();
}

#if !defined(_WIN32) && !defined(__APPLE__)
TEST(ChangeSignal, ReadOnlyHandleLeavesNoSemaphoreName) {
    StoreFilesGuard files("signal_no_leak");
    Store::open(files.utf8().c_str(), OpenMode::CreateNew, Configuration())->close();  // the writer unlinks
    const std::string name = "/slideio-processor-" + storeUuidOf(files.utf8());
    ASSERT_EQ(name.size(), std::string("/slideio-processor-").size() + 32);

    StorePtr reader = Store::open(files.utf8().c_str(), OpenMode::ReadOnly,
                                  ConfigurationBuilder().eventPollIntervalMs(50).build());
    EventCursorPtr cursor = reader->events().subscribe(EventJournal::kFromNow);
    EXPECT_FALSE(cursor->next(300));  // several waits, each trying to open the name
    cursor->close();
    reader->close();

    errno = 0;
    sem_t* left = sem_open(name.c_str(), 0);
    if (left != SEM_FAILED) {
        sem_close(left);
        sem_unlink(name.c_str());
        FAIL() << "a read-only handle left the semaphore " << name << " behind";
    }
    EXPECT_EQ(errno, ENOENT);
}
#endif
