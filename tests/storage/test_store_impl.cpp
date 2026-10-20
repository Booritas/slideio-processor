// Requirements: UR-OBS-1
// No public API opens a read transaction yet, so this drives detail::StoreImpl
// directly; its sources are compiled into the test (they are not exported).
#include "storage/detail/schema.hpp"
#include "storage/detail/sqlite.hpp"
#include "storage/detail/store_impl.hpp"
#include "storage/store_fixture.hpp"

#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/storage/types.hpp>

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

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
