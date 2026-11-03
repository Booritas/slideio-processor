// Requirements: UR-OBS-2, UR-OBS-3, UR-OBS-7, UR-OBS-6
#include "storage/helper_process.hpp"
#include "storage/store_fixture.hpp"

#include "storage/detail/event_log.hpp"
#include "storage/detail/store_impl.hpp"

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/storage/event_journal.hpp>
#include <slideio/processor/storage/store.hpp>
#include <slideio/processor/storage/types.hpp>

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>

using namespace slideio::processor;
namespace detail = slideio::processor::detail;

namespace {

std::string nullable(int v) { return v == 0 ? "NULL" : std::to_string(v); }

// The INSERT of one event, every column written explicitly. A zero stage, layer, label
// or status is written as NULL.
std::string eventInsert(EventKind kind, int stage, int layer, int label, int64_t count, int64_t minX,
                        int64_t minY, int64_t maxX, int64_t maxY, int status, int64_t timestamp) {
    return "INSERT INTO events(kind, stage_id, layer_id, label_id, object_count, min_x, min_y, max_x, "
           "max_y, stage_status, timestamp_ms) VALUES (" +
           std::to_string(static_cast<int>(kind)) + ", " + nullable(stage) + ", " + nullable(layer) + ", " +
           nullable(label) + ", " + std::to_string(count) + ", " + std::to_string(minX) + ", " +
           std::to_string(minY) + ", " + std::to_string(maxX) + ", " + std::to_string(maxY) + ", " +
           nullable(status) + ", " + std::to_string(timestamp) + ");\n";
}

// The INSERTs, in one transaction, of ObjectsAdded events for stage 1, layer 2 whose
// counts are `first` to `last`; on an empty journal they take sequences `first` to `last`.
std::string simpleEvents(int64_t first, int64_t last) {
    std::string sql = "BEGIN;\n";
    for (int64_t n = first; n <= last; ++n) {
        sql += eventInsert(EventKind::ObjectsAdded, 1, 2, 0, n, 10 * n, 20 * n, 10 * n + 5, 20 * n + 5, 0, 1000 + n);
    }
    return sql + "COMMIT;\n";
}

// Every event a test appends comes from another process, as a foreign writer's would,
// while the fixture's store stays open (rule: helper_process.hpp). A raw insert never
// posts the wake-up signal.
class EventJournalTest : public StoreFixture {
protected:
    void appendFromAnotherProcess(const std::string& sql) {
        ASSERT_EQ(execInAnotherProcess(pathUtf8(), sql), 0) << sql;
    }
};

} // namespace

TEST_F(EventJournalTest, EmptyJournalPositions) {
    EventJournal& j = store().events();
    EXPECT_EQ(j.newestSequence(), 0);
    EXPECT_EQ(j.oldestSequence(), 1);
    EXPECT_EQ(j.eventCount(), 0);
    auto c = j.subscribe(EventJournal::kFromBeginning);
    EXPECT_FALSE(c->next(0));
    EXPECT_EQ(c->position(), 0);
}

TEST_F(EventJournalTest, PositionConstants) {
    EXPECT_EQ(EventJournal::kFromBeginning, -1);
    EXPECT_EQ(EventJournal::kFromNow, -2);
}

TEST_F(EventJournalTest, ReplayLateJoinAndResume) {
    EventJournal& j = store().events();
    appendFromAnotherProcess(simpleEvents(1, 5));
    EXPECT_EQ(j.newestSequence(), 5);
    EXPECT_EQ(j.oldestSequence(), 1);
    EXPECT_EQ(j.eventCount(), 5);

    auto all = j.subscribe(EventJournal::kFromBeginning);
    EXPECT_EQ(all->position(), 0);
    for (int64_t n = 1; n <= 5; ++n) {
        ASSERT_TRUE(all->next(0));
        const Event& e = all->current();
        EXPECT_EQ(e.sequence(), n);
        EXPECT_EQ(e.kind(), EventKind::ObjectsAdded);
        EXPECT_EQ(e.stage().value(), 1);
        EXPECT_EQ(e.layer().value(), 2);
        EXPECT_FALSE(e.label().isValid());
        EXPECT_EQ(e.objectCount(), n);
        EXPECT_EQ(e.boundingBox().x, 10 * n);
        EXPECT_EQ(e.boundingBox().y, 20 * n);
        EXPECT_EQ(e.boundingBox().width, 5);
        EXPECT_EQ(e.boundingBox().height, 5);
        EXPECT_EQ(e.timestampUnixMs(), 1000 + n);
        EXPECT_EQ(all->position(), n);
    }
    EXPECT_FALSE(all->next(0));

    auto late = j.subscribe(3);
    ASSERT_TRUE(late->next(0));
    EXPECT_EQ(late->current().sequence(), 4);
    ASSERT_TRUE(late->next(0));
    EXPECT_EQ(late->current().sequence(), 5);
    EXPECT_FALSE(late->next(0));

    auto now = j.subscribe(EventJournal::kFromNow);
    EXPECT_EQ(now->position(), 5);
    EXPECT_FALSE(now->next(0));
    appendFromAnotherProcess(simpleEvents(6, 6));
    ASSERT_TRUE(now->next(0));
    EXPECT_EQ(now->current().sequence(), 6);

    auto first = j.subscribe(EventJournal::kFromBeginning);
    for (int i = 0; i < 4; ++i) ASSERT_TRUE(first->next(0));
    EXPECT_EQ(first->position(), 4);
    auto resumed = j.subscribe(first->position());
    ASSERT_TRUE(resumed->next(0));
    EXPECT_EQ(resumed->current().sequence(), 5);
}

TEST_F(EventJournalTest, FieldsOfOtherKindsRoundTrip) {
    EventJournal& j = store().events();
    appendFromAnotherProcess(
        eventInsert(EventKind::StageStateChanged, 3, 0, 0, 0, 0, 0, 0, 0, static_cast<int>(StageStatus::Complete), 77) +
        eventInsert(EventKind::LabelAdded, 0, 0, 65536, 0, 0, 0, 0, 0, 0, 78));
    auto c = j.subscribe(EventJournal::kFromBeginning);
    ASSERT_TRUE(c->next(0));
    EXPECT_EQ(c->current().kind(), EventKind::StageStateChanged);
    EXPECT_EQ(c->current().stage().value(), 3);
    EXPECT_FALSE(c->current().layer().isValid());
    EXPECT_EQ(c->current().stageStatus(), StageStatus::Complete);
    ASSERT_TRUE(c->next(0));
    EXPECT_EQ(c->current().kind(), EventKind::LabelAdded);
    EXPECT_EQ(c->current().label().value(), 65536);
    EXPECT_EQ(c->current().objectCount(), 0);
    EXPECT_TRUE(c->current().boundingBox().isEmpty());
}

TEST_F(EventJournalTest, ReadsMoreThanOnePageInOrder) {
    EventJournal& j = store().events();
    appendFromAnotherProcess(simpleEvents(1, 600));
    auto c = j.subscribe(EventJournal::kFromBeginning);
    for (int64_t n = 1; n <= 600; ++n) {
        ASSERT_TRUE(c->next(0));
        ASSERT_EQ(c->current().sequence(), n);
    }
    EXPECT_FALSE(c->next(0));
}

TEST_F(EventJournalTest, CursorSeesEventsCommittedBetweenPages) {
    EventJournal& j = store().events();
    appendFromAnotherProcess(simpleEvents(1, 1));
    auto c = j.subscribe(EventJournal::kFromBeginning);
    ASSERT_TRUE(c->next(0));
    EXPECT_FALSE(c->next(0));
    appendFromAnotherProcess(simpleEvents(2, 2));  // committed after the cursor's first page
    ASSERT_TRUE(c->next(0));
    EXPECT_EQ(c->current().sequence(), 2);
}

TEST_F(EventJournalTest, TruncationIsReportedNotSkipped) {  // UR-OBS-7
    EventJournal& j = store().events();
    appendFromAnotherProcess(simpleEvents(1, 10));
    j.truncateBelow(6);
    EXPECT_EQ(j.eventCount(), 5);
    EXPECT_EQ(j.oldestSequence(), 6);
    EXPECT_EQ(j.newestSequence(), 10);

    EXPECT_NO_THROW(j.subscribe(5));
    try {
        j.subscribe(4);
        FAIL() << "subscribe(4) should throw";
    } catch (const InvalidArgumentError& e) {
        const std::string message = e.what();
        EXPECT_NE(message.find('4'), std::string::npos) << message;
        EXPECT_NE(message.find('6'), std::string::npos) << message;
    }
    auto c = j.subscribe(EventJournal::kFromBeginning);
    ASSERT_TRUE(c->next(0));
    EXPECT_EQ(c->current().sequence(), 6);

    j.truncateBelow(3);  // never lowers the watermark
    EXPECT_THROW(j.subscribe(4), InvalidArgumentError);
    EXPECT_EQ(j.oldestSequence(), 6);
}

TEST_F(EventJournalTest, TruncatingEverythingKeepsSequencesAndStartsAfterNewest) {
    EventJournal& j = store().events();
    appendFromAnotherProcess(simpleEvents(1, 3));
    j.truncateBelow(100);  // beyond the newest sequence
    EXPECT_EQ(j.eventCount(), 0);
    EXPECT_EQ(j.newestSequence(), 3);
    EXPECT_EQ(j.oldestSequence(), 4);
    appendFromAnotherProcess(simpleEvents(4, 4));  // sequences are never reused
    EXPECT_EQ(j.newestSequence(), 4);
    auto c = j.subscribe(EventJournal::kFromBeginning);
    ASSERT_TRUE(c->next(0));
    EXPECT_EQ(c->current().sequence(), 4);
}

// A position beyond the newest event came from another store, or from this path before
// it was recreated; starting there would skip the events up to it once they are written.
TEST_F(EventJournalTest, PositionBeyondTheNewestEventIsRefused) {
    EventJournal& j = store().events();
    ASSERT_EQ(j.newestSequence(), 0);
    try {
        j.subscribe(1);
        FAIL() << "subscribe(1) on an empty journal should throw";
    } catch (const InvalidArgumentError& e) {
        const std::string message = e.what();
        EXPECT_NE(message.find("position 1 is beyond the newest event 0"), std::string::npos) << message;
        EXPECT_NE(message.find(pathUtf8()), std::string::npos) << message;
    }
    EXPECT_NO_THROW(j.subscribe(0));

    appendFromAnotherProcess(simpleEvents(1, 1));
    auto c = j.subscribe(1);  // the newest event itself is a valid position
    EXPECT_EQ(c->position(), 1);
    EXPECT_THROW(j.subscribe(2), InvalidArgumentError);
}

TEST_F(EventJournalTest, WaitHonoursTimeoutAndArguments) {
    EventJournal& j = store().events();
    auto c = j.subscribe(EventJournal::kFromBeginning);
    EXPECT_FALSE(c->next(0));
    const auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(c->next(50));
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_GE(elapsed, std::chrono::milliseconds(50));
    EXPECT_LT(elapsed, std::chrono::milliseconds(2000));
    EXPECT_THROW(c->next(-3), InvalidArgumentError);
    EXPECT_THROW(j.subscribe(-7), InvalidArgumentError);
    EXPECT_THROW(j.subscribe(-3), InvalidArgumentError);
}

TEST_F(EventJournalTest, BlockingNextWakesOnAppendFromAnotherThread) {
    EventJournal& j = store().events();
    auto c = j.subscribe(EventJournal::kFromNow);
    const std::string path = pathUtf8();
    int helperExit = -1;
    std::thread writer([path, &helperExit] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        helperExit = execInAnotherProcess(path, simpleEvents(1, 1));
    });
    const auto start = std::chrono::steady_clock::now();
    const bool got = c->next(5000);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    writer.join();
    ASSERT_EQ(helperExit, 0);
    ASSERT_TRUE(got);
    EXPECT_EQ(c->current().sequence(), 1);
    EXPECT_LT(elapsed, std::chrono::milliseconds(3000));
}

TEST_F(EventJournalTest, ClosedCursorAndClosedStore) {
    EventJournal& j = store().events();
    auto c = j.subscribe(EventJournal::kFromBeginning);
    c->close();
    c->close();  // idempotent
    EXPECT_THROW(c->next(0), IoError);

    auto open = j.subscribe(EventJournal::kFromBeginning);
    store().close();
    EXPECT_THROW(open->next(0), IoError);
    EXPECT_THROW(j.newestSequence(), IoError);
    EXPECT_THROW(j.subscribe(EventJournal::kFromBeginning), IoError);
}

TEST_F(EventJournalTest, ReadOnlyStoreReadsButRefusesTruncation) {
    appendFromAnotherProcess(simpleEvents(1, 1));
    StorePtr ro = reopen(OpenMode::ReadOnly);
    EXPECT_EQ(ro->events().eventCount(), 1);
    auto c = ro->events().subscribe(EventJournal::kFromBeginning);
    EXPECT_TRUE(c->next(0));
    EXPECT_THROW(ro->events().truncateBelow(2), IoError);
}

TEST(EventValue, DefaultReadsAsNoEvent) {
    Event none;
    EXPECT_EQ(none.sequence(), 0);
    EXPECT_FALSE(none.stage().isValid());
    EXPECT_FALSE(none.layer().isValid());
    EXPECT_FALSE(none.label().isValid());
    EXPECT_EQ(none.objectCount(), 0);
    EXPECT_TRUE(none.boundingBox().isEmpty());
}

TEST_F(EventJournalTest, EventCopyAndMoveLeaveMovedFromDefault) {
    appendFromAnotherProcess(simpleEvents(7, 7));  // sequence 1, count 7
    auto c = store().events().subscribe(EventJournal::kFromBeginning);
    ASSERT_TRUE(c->next(0));
    Event copy = c->current();
    EXPECT_EQ(copy.sequence(), 1);
    EXPECT_EQ(copy.objectCount(), 7);
    Event moved = std::move(copy);
    EXPECT_EQ(moved.sequence(), 1);
    EXPECT_EQ(copy.sequence(), 0);  // NOLINT: a moved-from event reads as a default event
    EXPECT_FALSE(copy.layer().isValid());
    Event assigned;
    assigned = moved;
    EXPECT_EQ(assigned.objectCount(), 7);
    Event target;
    target = std::move(assigned);
    EXPECT_EQ(target.objectCount(), 7);
    EXPECT_EQ(assigned.sequence(), 0);  // NOLINT
}

namespace {

// The first column of the first row of a query on the store file, as an integer; -1 for NULL.
// Rule (helper_process.hpp): the test's own SQLite copy, so use it only on a file no Store
// has open. A StoreImpl compiled into this test shares this copy and is safe beside it.
int64_t rawScalar(const std::string& path, const char* sql) {
    sqlite3* db = nullptr;
    EXPECT_EQ(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr), SQLITE_OK);
    sqlite3_stmt* stmt = nullptr;
    EXPECT_EQ(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr), SQLITE_OK);
    int64_t value = -2;
    if (sqlite3_step(stmt) == SQLITE_ROW) value = sqlite3_column_type(stmt, 0) == SQLITE_NULL ? -1 : sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return value;
}

} // namespace

// appendEvent is internal; it is exercised on a StoreImpl compiled into the test.
TEST(EventLog, AppendEventJoinsTheCallersTransaction) {
    StoreFilesGuard files("eventlog");
    detail::StoreImpl impl(files.utf8().c_str(), OpenMode::CreateNew, Configuration());

    detail::EventRow batch;
    batch.kind = EventKind::ObjectsAdded;
    batch.stageId = 1;
    batch.layerId = 1;
    batch.objectCount = 3;
    batch.minX = 1;
    batch.minY = 2;
    batch.maxX = 3;
    batch.maxY = 4;
    batch.timestampMs = 555;
    detail::EventRow state;
    state.kind = EventKind::StageStateChanged;
    state.stageId = 1;
    state.hasStageStatus = true;
    state.stageStatus = StageStatus::Running;

    const int64_t last = impl.withWriteTransaction("test", [&](sqlite3* db) {
        EXPECT_EQ(detail::appendEvent(db, batch), 1);
        return detail::appendEvent(db, state);
    });
    impl.notifyCommitted();
    EXPECT_EQ(last, 2);
    EXPECT_EQ(rawScalar(files.utf8(), "SELECT COUNT(*) FROM events"), 2);
    EXPECT_EQ(rawScalar(files.utf8(), "SELECT object_count FROM events WHERE seq = 1"), 3);
    EXPECT_EQ(rawScalar(files.utf8(), "SELECT max_y FROM events WHERE seq = 1"), 4);
    EXPECT_EQ(rawScalar(files.utf8(), "SELECT timestamp_ms FROM events WHERE seq = 1"), 555);
    EXPECT_EQ(rawScalar(files.utf8(), "SELECT layer_id FROM events WHERE seq = 2"), -1);       // NULL
    EXPECT_EQ(rawScalar(files.utf8(), "SELECT stage_status FROM events WHERE seq = 1"), -1);   // NULL
    EXPECT_EQ(rawScalar(files.utf8(), "SELECT stage_status FROM events WHERE seq = 2"), 1);
    EXPECT_GT(rawScalar(files.utf8(), "SELECT timestamp_ms FROM events WHERE seq = 2"), 0);   // now

    // The row rolls back with the caller's transaction: appendEvent keeps none of its own.
    EXPECT_THROW(impl.withWriteTransaction("test", [&](sqlite3* db) {
                     detail::appendEvent(db, batch);
                     throw InternalError("boom");
                 }),
                 InternalError);
    EXPECT_EQ(rawScalar(files.utf8(), "SELECT COUNT(*) FROM events"), 2);
}

// A cursor is handed to observers as a shared_ptr and may outlive the Store.
TEST(EventCursorLifetime, OutlivesItsStore) {
    StoreFilesGuard files("cursorlife");
    Configuration config;
    StorePtr store = Store::open(files.utf8().c_str(), OpenMode::CreateNew, config);
    EventCursorPtr cursor = store->events().subscribe(EventJournal::kFromBeginning);
    EXPECT_FALSE(cursor->next(0));
    store.reset();
    EXPECT_THROW(cursor->next(0), IoError);
    EXPECT_THROW(cursor->next(50), IoError);
    cursor.reset();  // destroying it afterwards is clean
}

// next(t) returns false only once t ms have passed, even when the poll interval is small
// and t is not a multiple of it.
TEST(EventCursorTimeout, NeverReturnsEarlyWithSmallPollInterval) {
    StoreFilesGuard files("cursortimeout");
    Configuration config = ConfigurationBuilder().eventPollIntervalMs(10).build();
    StorePtr store = Store::open(files.utf8().c_str(), OpenMode::CreateNew, config);
    auto cursor = store->events().subscribe(EventJournal::kFromBeginning);
    for (int32_t timeout : {50, 53, 7, 1}) {
        const auto start = std::chrono::steady_clock::now();
        EXPECT_FALSE(cursor->next(timeout));
        EXPECT_GE(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(timeout)) << timeout;
    }
}

TEST_F(EventJournalTest, LiveCursorIsToldWhenItsPositionIsTruncatedAway) {  // UR-OBS-7
    EventJournal& j = store().events();
    appendFromAnotherProcess(simpleEvents(1, 10));
    auto c = j.subscribe(EventJournal::kFromBeginning);
    ASSERT_TRUE(c->next(0));
    ASSERT_TRUE(c->next(0));
    j.truncateBelow(6);
    try {
        c->next(0);
        FAIL() << "next() should have thrown";
    } catch (const InvalidArgumentError& e) {
        const std::string message = e.what();
        EXPECT_NE(message.find('2'), std::string::npos) << message;
        EXPECT_NE(message.find('6'), std::string::npos) << message;
    }
}
