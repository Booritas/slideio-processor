// Requirements: UR-STG-4, UR-STG-2, UR-OBS-5
#include "storage/store_fixture.hpp"

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/storage/event_journal.hpp>
#include <slideio/processor/storage/stage_state_reader.hpp>
#include <slideio/processor/storage/stage_state_writer.hpp>
#include <slideio/processor/storage/store.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace slideio::processor;

namespace {

class StageState : public StoreFixture {};

}  // namespace

TEST_F(StageState, DeclareAndReadBack) {
    StageStateWriter& w = store().stageStateWriter();
    const StageStateReader& r = store().stageStates();
    EXPECT_EQ(r.count(), 0);
    const StageId a = w.declare("nuclei", R"({"t":1})");
    EXPECT_EQ(a, StageId(1));
    EXPECT_EQ(r.count(), 1);
    EXPECT_EQ(r.at(0), a);
    StageId f;
    EXPECT_TRUE(r.find("nuclei", f));
    EXPECT_EQ(f, a);
    EXPECT_FALSE(r.find("Nuclei", f));
    EXPECT_FALSE(r.find("", f));
    EXPECT_FALSE(r.find(nullptr, f));
    EXPECT_STREQ(r.name(a), "nuclei");
    EXPECT_EQ(r.status(a), StageStatus::Pending);
    EXPECT_FALSE(r.isStale(a));
    EXPECT_STREQ(r.configuration(a), R"({"t":1})");
    EXPECT_EQ(r.completedAtUnixMs(a), 0);
    EXPECT_EQ(r.dependencyCount(a), 0);
    EXPECT_EQ(r.writtenLayerCount(a), 0);
    EXPECT_THROW(w.declare("nuclei", ""), InvalidArgumentError);
    EXPECT_THROW(w.declare("", ""), InvalidArgumentError);
    EXPECT_THROW(w.declare(nullptr, ""), InvalidArgumentError);
    EXPECT_THROW(r.status(StageId(99)), InvalidArgumentError);
    EXPECT_THROW(r.status(StageId()), InvalidArgumentError);
    EXPECT_THROW(r.name(StageId(99)), InvalidArgumentError);
    EXPECT_THROW(r.at(1), InvalidArgumentError);
    EXPECT_THROW(r.at(-1), InvalidArgumentError);
    EXPECT_EQ(r.count(), 1);  // failed declares added nothing

    const StageId b = w.declare("cells", nullptr);
    EXPECT_EQ(b, StageId(2));
    EXPECT_STREQ(r.configuration(b), "");
    EXPECT_EQ(r.at(1), b);
}

TEST_F(StageState, StringsStayValidAcrossChanges) {
    StageStateWriter& w = store().stageStateWriter();
    const StageStateReader& r = store().stageStates();
    const StageId a = w.declare("nuclei", "one");
    const char* name = r.name(a);
    const char* first = r.configuration(a);
    w.setConfiguration(a, "two");
    const char* second = r.configuration(a);
    EXPECT_STREQ(first, "one");  // still valid, still the value that call returned
    EXPECT_STREQ(second, "two");
    EXPECT_STREQ(name, "nuclei");
    w.setConfiguration(a, nullptr);
    EXPECT_STREQ(r.configuration(a), "");
}

TEST_F(StageState, StatusChangesAreJournalled) {
    StageStateWriter& w = store().stageStateWriter();
    const StageStateReader& r = store().stageStates();
    const StageId a = w.declare("nuclei", "");
    EventCursorPtr cursor = store().events().subscribe(EventJournal::kFromBeginning);
    EXPECT_FALSE(cursor->next(0));  // declare appends nothing

    w.setStatus(a, StageStatus::Running);
    EXPECT_EQ(r.status(a), StageStatus::Running);
    EXPECT_EQ(r.completedAtUnixMs(a), 0);
    w.setStatus(a, StageStatus::Complete);
    EXPECT_EQ(r.status(a), StageStatus::Complete);
    const int64_t completed = r.completedAtUnixMs(a);
    EXPECT_GT(completed, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    w.setStatus(a, StageStatus::Running);
    EXPECT_EQ(r.completedAtUnixMs(a), completed);  // another transition leaves it alone
    w.setStatus(a, StageStatus::Failed);
    EXPECT_EQ(r.completedAtUnixMs(a), completed);

    const StageStatus expected[] = {StageStatus::Running, StageStatus::Complete, StageStatus::Running,
                                    StageStatus::Failed};
    for (StageStatus status : expected) {
        ASSERT_TRUE(cursor->next(0));
        EXPECT_EQ(cursor->current().kind(), EventKind::StageStateChanged);
        EXPECT_EQ(cursor->current().stage(), a);
        EXPECT_EQ(cursor->current().stageStatus(), status);
        EXPECT_EQ(cursor->current().objectCount(), 0);
        EXPECT_EQ(cursor->current().layer(), LayerId());
    }
    EXPECT_FALSE(cursor->next(0));

    EXPECT_THROW(w.setStatus(StageId(99), StageStatus::Running), InvalidArgumentError);
    EXPECT_THROW(w.setStatus(a, static_cast<StageStatus>(17)), InvalidArgumentError);
    EXPECT_FALSE(cursor->next(0));  // the failed calls appended nothing
}

TEST_F(StageState, ReenteringCompleteRefreshesTheTime) {
    StageStateWriter& w = store().stageStateWriter();
    const StageStateReader& r = store().stageStates();
    const StageId a = w.declare("nuclei", "");
    w.setStatus(a, StageStatus::Complete);
    const int64_t first = r.completedAtUnixMs(a);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    w.setStatus(a, StageStatus::Complete);
    EXPECT_GT(r.completedAtUnixMs(a), first);
}

TEST_F(StageState, DependenciesAndStale) {
    StageStateWriter& w = store().stageStateWriter();
    const StageStateReader& r = store().stageStates();
    const StageId a = w.declare("nuclei", "");
    const StageId b = w.declare("cells", "");
    const StageId c = w.declare("tissue", "");
    w.addDependency(b, a);
    w.addDependency(b, a);
    EXPECT_EQ(r.dependencyCount(b), 1);
    w.addDependency(b, c);
    EXPECT_EQ(r.dependencyCount(b), 2);
    EXPECT_EQ(r.dependencyAt(b, 0), a);
    EXPECT_EQ(r.dependencyAt(b, 1), c);
    EXPECT_THROW(r.dependencyAt(b, 2), InvalidArgumentError);
    EXPECT_THROW(r.dependencyAt(b, -1), InvalidArgumentError);
    EXPECT_EQ(r.dependencyCount(a), 0);
    w.clearDependencies(b);
    EXPECT_EQ(r.dependencyCount(b), 0);
    EXPECT_THROW(w.addDependency(a, a), InvalidArgumentError);
    EXPECT_THROW(w.addDependency(a, StageId(99)), InvalidArgumentError);
    EXPECT_THROW(w.addDependency(StageId(99), a), InvalidArgumentError);
    EXPECT_THROW(w.clearDependencies(StageId(99)), InvalidArgumentError);
    EXPECT_THROW(r.dependencyCount(StageId(99)), InvalidArgumentError);

    EXPECT_FALSE(r.isStale(a));
    w.setStale(a, true);
    EXPECT_TRUE(r.isStale(a));
    EXPECT_FALSE(r.isStale(b));
    w.setStale(a, false);
    EXPECT_FALSE(r.isStale(a));
    EXPECT_THROW(w.setStale(StageId(99), true), InvalidArgumentError);
    EXPECT_THROW(w.setConfiguration(StageId(99), ""), InvalidArgumentError);
}

TEST_F(StageState, WrittenLayerValidation) {
    StageStateWriter& w = store().stageStateWriter();
    const StageStateReader& r = store().stageStates();
    const StageId a = w.declare("nuclei", "");
    // Layers cannot be declared before Task 6; the positive path is tested there.
    EXPECT_THROW(w.addWrittenLayer(a, LayerId(2)), InvalidArgumentError);   // unknown layer
    EXPECT_THROW(w.addWrittenLayer(a, LayerId()), InvalidArgumentError);    // invalid layer
    EXPECT_THROW(w.addWrittenLayer(a, LayerId(1)), InvalidArgumentError);   // the pixel layer
    EXPECT_THROW(w.addWrittenLayer(StageId(99), LayerId(1)), InvalidArgumentError);
    EXPECT_EQ(r.writtenLayerCount(a), 0);
    EXPECT_THROW(r.writtenLayerAt(a, 0), InvalidArgumentError);
    EXPECT_THROW(r.writtenLayerCount(StageId(99)), InvalidArgumentError);
}

TEST_F(StageState, ReadOnlyAnswersAndRefusesWrites) {
    StageStateWriter& w = store().stageStateWriter();
    const StageId a = w.declare("nuclei", "cfg");
    const StageId b = w.declare("cells", "");
    w.addDependency(b, a);
    w.setStale(a, true);
    w.setStatus(a, StageStatus::Complete);

    StorePtr ro = reopen(OpenMode::ReadOnly);
    const StageStateReader& r = ro->stageStates();
    StageStateWriter& rw = ro->stageStateWriter();
    EXPECT_EQ(r.count(), 2);
    EXPECT_EQ(r.at(1), b);
    StageId f;
    EXPECT_TRUE(r.find("cells", f));
    EXPECT_STREQ(r.name(a), "nuclei");
    EXPECT_EQ(r.status(a), StageStatus::Complete);
    EXPECT_TRUE(r.isStale(a));
    EXPECT_STREQ(r.configuration(a), "cfg");
    EXPECT_GT(r.completedAtUnixMs(a), 0);
    EXPECT_EQ(r.dependencyCount(b), 1);
    EXPECT_EQ(r.dependencyAt(b, 0), a);
    EXPECT_EQ(r.writtenLayerCount(a), 0);

    EXPECT_THROW(rw.declare("x", ""), IoError);
    EXPECT_THROW(rw.setStatus(a, StageStatus::Running), IoError);
    EXPECT_THROW(rw.setStale(a, false), IoError);
    EXPECT_THROW(rw.setConfiguration(a, ""), IoError);
    EXPECT_THROW(rw.addDependency(a, b), IoError);
    EXPECT_THROW(rw.clearDependencies(b), IoError);
    EXPECT_THROW(rw.addWrittenLayer(a, LayerId(2)), IoError);
    try {
        rw.declare("x", "");
        FAIL();
    } catch (const IoError& e) {
        EXPECT_NE(std::string(e.what()).find("read-only"), std::string::npos);
    }
    EXPECT_EQ(r.status(a), StageStatus::Complete);  // nothing changed
}

TEST_F(StageState, ClosedStore) {
    StageStateWriter& w = store().stageStateWriter();
    const StageStateReader& r = store().stageStates();
    const StageId a = w.declare("nuclei", "");
    store().close();
    EXPECT_EQ(r.count(), 0);
    StageId f;
    EXPECT_FALSE(r.find("nuclei", f));
    EXPECT_THROW(r.status(a), IoError);
    EXPECT_THROW(r.at(0), IoError);
    EXPECT_THROW(w.setStatus(a, StageStatus::Running), IoError);
    EXPECT_THROW(w.declare("x", ""), IoError);
}

TEST_F(StageState, ConcurrentDeclaresGetDistinctIds) {
    StageStateWriter& w = store().stageStateWriter();
    const StageStateReader& r = store().stageStates();
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&w, t] {
            for (int i = 0; i < 10; ++i) w.declare(("s" + std::to_string(t) + "_" + std::to_string(i)).c_str(), "");
        });
    }
    for (auto& t : threads) t.join();
    EXPECT_EQ(r.count(), 40);
    for (int i = 0; i < 40; ++i) EXPECT_EQ(r.at(i), StageId(i + 1));
}
