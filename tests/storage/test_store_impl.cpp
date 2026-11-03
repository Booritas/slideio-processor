// Requirements: UR-OBS-1
// Drives detail::StoreImpl and detail::WriterLock directly, for what no public call can
// arrange (a read transaction left open, a race with another opener); their sources are
// compiled into the test (they are not exported).
#include "storage/detail/schema.hpp"
#include "storage/detail/sqlite.hpp"
#include "storage/detail/store_impl.hpp"
#include "storage/detail/writer_lock.hpp"
#include "storage/store_fixture.hpp"

#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/storage/types.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace slideio::processor;

TEST(StoreImpl, AbandonedReadTransactionIsRolledBackBeforePooling) {
    StoreFilesGuard files;
    detail::StoreImpl store(files.utf8().c_str(), OpenMode::CreateNew, Configuration());
    store.withWriteTransaction("test", [](sqlite3* db) { detail::StoreImpl::setMetaText(db, "probe", "old"); });

    // fn opens a read transaction, reads, and throws without committing.
    EXPECT_THROW(store.withReadConnection("test",
                                          [](sqlite3* db) -> int {
                                              detail::exec(db, "BEGIN");
                                              EXPECT_EQ(*detail::StoreImpl::metaText(db, "probe"), "old");
                                              throw std::runtime_error("abandoned");
                                          }),
                 InternalError);

    store.withWriteTransaction("test", [](sqlite3* db) { detail::StoreImpl::setMetaText(db, "probe", "new"); });

    // The same pooled connection is reused: it must not still sit on the old snapshot.
    store.withReadConnection("test", [](sqlite3* db) {
        EXPECT_NE(sqlite3_get_autocommit(db), 0);
        EXPECT_EQ(*detail::StoreImpl::metaText(db, "probe"), "new");
    });

    // A lease that ends inside a transaction is cleaned up the same way.
    {
        detail::ReadLease lease = store.acquireRead("test");
        detail::exec(lease.get(), "BEGIN; SELECT count(*) FROM meta");
    }
    store.withReadConnection("test", [](sqlite3* db) { EXPECT_NE(sqlite3_get_autocommit(db), 0); });
    store.close();
}

namespace {

#ifdef _WIN32
std::string currentPidText() { return std::to_string(_getpid()); }
#else
std::string currentPidText() { return std::to_string(getpid()); }
#endif

} // namespace

TEST(WriterLock, LockOnAnUnlinkedLockFileRetriesOnTheFileThePathNames) {
    StoreFilesGuard files;
    const std::string store = files.utf8();
    detail::WriterLock first = detail::WriterLock::acquire(store);
    // `second` opens the lock file, then `first` removes it and releases; `second` then
    // locks the orphaned file, which no later opener can see.
    detail::WriterLock second =
        detail::WriterLock::acquire(store, [&] { first.removeFileAndRelease(store); });
    EXPECT_FALSE(first.held());
    EXPECT_TRUE(second.held());

    try {
        detail::WriterLock third = detail::WriterLock::acquire(store);
        ADD_FAILURE() << "a second writer took the lock";
    } catch (const IoError& e) {
        EXPECT_NE(std::string(e.what()).find("already open for writing"), std::string::npos) << e.what();
    }
    std::filesystem::path lockFile = files.path();
    lockFile += "-writer.lock";
    EXPECT_EQ(readFile(lockFile), currentPidText());
}

TEST(WriterLock, LockHeldOnAnUnlinkedLockFileDoesNotRefuseTheNextWriter) {
    StoreFilesGuard files;
    const std::string store = files.utf8();
    std::filesystem::path lockFile = files.path();
    lockFile += "-writer.lock";
    detail::WriterLock first = detail::WriterLock::acquire(store);
    // `second` opens the lock file, which is then removed while `first` still holds it:
    // the instant inside removeFileAndRelease before the release. The file `second`
    // finds locked is no longer the one the path names, so it is no writer's.
    detail::WriterLock second = detail::WriterLock::acquire(store, [&] { std::filesystem::remove(lockFile); });
    EXPECT_TRUE(second.held());
    first.release();

    EXPECT_THROW(detail::WriterLock::acquire(store), IoError);
    EXPECT_EQ(readFile(lockFile), currentPidText());
}

namespace {

// lockForCreateNew is what CreateNew runs after its pre-check found the path free; a
// store present by then was made by a creator that won the race.
void expectAlreadyExists(const std::string& path) {
    try {
        detail::WriterLock lock = detail::lockForCreateNew(path);
        ADD_FAILURE() << "the lock for an existing store was taken";
    } catch (const IoError& e) {
        EXPECT_NE(std::string(e.what()).find("already exists"), std::string::npos) << e.what();
    }
}

void expectStoreAndLockFileExist(const StoreFilesGuard& files) {
    std::filesystem::path lockFile = files.path();
    lockFile += "-writer.lock";
    EXPECT_TRUE(std::filesystem::exists(files.path()));
    EXPECT_TRUE(std::filesystem::exists(lockFile));
}

} // namespace

TEST(StoreImpl, CreateNewLosingToALiveCreatorReportsTheExistingStore) {
    StoreFilesGuard files;
    detail::StoreImpl winner(files.utf8().c_str(), OpenMode::CreateNew, Configuration());
    expectAlreadyExists(files.utf8());
    expectStoreAndLockFileExist(files);
    winner.close();
}

TEST(StoreImpl, CreateNewLosingToAClosedCreatorReportsTheExistingStoreAndReleasesTheLock) {
    StoreFilesGuard files;
    detail::StoreImpl(files.utf8().c_str(), OpenMode::CreateNew, Configuration()).close();
    expectAlreadyExists(files.utf8());
    expectStoreAndLockFileExist(files);
    // The lock was released, and the store opens for writing.
    detail::StoreImpl reopened(files.utf8().c_str(), OpenMode::ReadWrite, Configuration());
    reopened.close();
}
