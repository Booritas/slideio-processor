// Requirements: UR-RES-12, UR-OBS-1, UR-RES-11
#pragma once

// The shared core of an open store (rulings S2, S3, S4): its connections, the write
// mutex, the writer lock and the meta table. Every storage class holds a StoreImpl&.
//
// Writes run on one write connection, serialised by the write mutex, each inside
// withWriteTransaction. Reads check a connection out of a pool, one per concurrent
// caller: withReadConnection for a call, acquireRead for a cursor's lifetime.

#include "storage/detail/sqlite.hpp"
#include "storage/detail/writer_lock.hpp"

#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/provenance.hpp>
#include <slideio/processor/storage/types.hpp>

#include <atomic>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace slideio::processor::detail {

class ConnectionPool;

// A read connection checked out of the pool; returned (or closed, once the store has
// closed) on destruction. Move-only.
class ReadLease {
public:
    ReadLease(std::shared_ptr<ConnectionPool> pool, Connection connection) noexcept;
    ReadLease(ReadLease&& other) noexcept = default;
    ReadLease& operator=(ReadLease&&) = delete;
    ReadLease(const ReadLease&) = delete;
    ReadLease& operator=(const ReadLease&) = delete;
    ~ReadLease();

    sqlite3* get() const noexcept { return connection_.get(); }

private:
    std::shared_ptr<ConnectionPool> pool_;
    Connection connection_;
};

class StoreImpl {
public:
    // Opens or creates the store per S2-S4. Throws InvalidArgumentError for a null or
    // empty path or an unknown mode; IoError when the path exists (CreateNew) or is
    // missing (otherwise), when the writer lock is held, or on a SQLite failure;
    // StoreFormatError when the file is not a store of this format version.
    StoreImpl(const char* path, OpenMode mode, const Configuration& configuration);
    StoreImpl(const StoreImpl&) = delete;
    StoreImpl& operator=(const StoreImpl&) = delete;
    ~StoreImpl();  // closes; never throws

    const std::string& path() const noexcept { return path_; }
    OpenMode mode() const noexcept { return mode_; }
    bool isReadOnly() const noexcept { return mode_ == OpenMode::ReadOnly; }
    int32_t formatVersion() const noexcept { return formatVersion_; }
    const Configuration& configuration() const noexcept { return configuration_; }
    bool isOpen() const noexcept { return open_.load(std::memory_order_acquire); }

    // Throws IoError "<operation>: store is closed ('<path>')" after close().
    void checkOpen(const char* operation) const;
    // checkOpen, then throws IoError "<operation>: the store '<path>' is read-only" on a
    // ReadOnly store.
    void checkWritable(const char* operation) const;

    // Runs fn(sqlite3* writeConnection) inside BEGIN IMMEDIATE ... COMMIT, holding the
    // write mutex, and returns its result. Rolls back and rethrows when fn throws; a
    // standard exception other than std::bad_alloc becomes InternalError. Throws
    // IoError when closed, read-only, or on a SQLite failure.
    template <class Fn>
    auto withWriteTransaction(const char* operation, Fn&& fn) -> decltype(fn(std::declval<sqlite3*>()));

    // Runs fn(sqlite3* readConnection) on a pooled read connection and returns its
    // result. No transaction is opened: a single statement is its own snapshot; fn
    // opens one itself when it needs several statements to agree. A transaction still
    // open when the connection returns to the pool is rolled back. Throws IoError when
    // closed or on a SQLite failure.
    template <class Fn>
    auto withReadConnection(const char* operation, Fn&& fn) const -> decltype(fn(std::declval<sqlite3*>()));

    // Checks a read connection out for as long as the lease lives (a cursor). A
    // transaction still open when the lease ends is rolled back. Throws IoError when
    // closed or on a SQLite failure.
    ReadLease acquireRead(const char* operation) const;

    // meta accessors, on any connection, inside or outside a transaction. A missing key
    // reads as nullopt. Throw IoError.
    static std::optional<std::string> metaText(sqlite3* db, const char* key);
    static std::optional<int64_t> metaInt64(sqlite3* db, const char* key);
    static void setMetaText(sqlite3* db, const char* key, const std::string& value);
    static void setMetaInt64(sqlite3* db, const char* key, int64_t value);

    // Called after any write transaction that appended an event has committed, so that
    // subscribers waiting in waitForCommit wake at once. A no-op until the wake-up
    // signal arrives (ruling S13); correctness never depends on it.
    void notifyCommitted() noexcept;

    // Blocks the calling event cursor for at most `timeoutMs` (never more than one poll
    // interval is requested) or until a commit has been signalled. Today it sleeps the
    // whole interval; the signal wait replaces the sleep.
    void waitForCommit(int32_t timeoutMs) const;

    // Releases every connection and the writer lock. Idempotent. Throws IoError when
    // the write connection fails to close.
    void close();

private:
    void openExisting();
    void createNew();
    void openWriteConnection(int flags);
    void configureWriteConnection();
    int32_t readFormatVersion(sqlite3* db) const;
    void beginWrite(const char* operation);
    void commitWrite(const char* operation);
    void rollbackWrite() noexcept;
    [[noreturn]] void throwTranslated(const char* operation, const std::exception& e) const;

    std::string path_;
    OpenMode mode_;
    Configuration configuration_;
    int32_t formatVersion_ = 0;
    std::atomic<bool> open_{false};
    WriterLock writerLock_;
    mutable std::mutex writeMutex_;
    Connection writeConnection_;
    std::shared_ptr<ConnectionPool> pool_;
};

// Returns Provenance::toJson as a string, the form meta.provenance holds. Throws
// InternalError and std::bad_alloc, as toJson does.
std::string provenanceToJson(const Provenance& provenance);

// Parses meta.provenance (Provenance::toJson) and restores the stored record. Throws
// InternalError naming the store path, the parse-error offset and the record size, but
// never the record's text, when it is not a provenance record.
Provenance parseProvenanceJson(const std::string& json, const std::string& storePath);

template <class Fn>
auto StoreImpl::withWriteTransaction(const char* operation, Fn&& fn) -> decltype(fn(std::declval<sqlite3*>())) {
    checkWritable(operation);
    std::lock_guard<std::mutex> guard(writeMutex_);
    checkOpen(operation);  // close() may have run while this call waited
    beginWrite(operation);
    try {
        if constexpr (std::is_void_v<decltype(fn(std::declval<sqlite3*>()))>) {
            fn(writeConnection_.get());
            commitWrite(operation);
        } else {
            auto result = fn(writeConnection_.get());
            commitWrite(operation);
            return result;
        }
    } catch (const ProcessorError&) {
        rollbackWrite();
        throw;
    } catch (const std::bad_alloc&) {
        rollbackWrite();
        throw;
    } catch (const std::exception& e) {
        rollbackWrite();
        throwTranslated(operation, e);
    }
}

template <class Fn>
auto StoreImpl::withReadConnection(const char* operation, Fn&& fn) const -> decltype(fn(std::declval<sqlite3*>())) {
    ReadLease lease = acquireRead(operation);
    try {
        return fn(lease.get());
    } catch (const ProcessorError&) {
        throw;
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& e) {
        throwTranslated(operation, e);
    }
}

} // namespace slideio::processor::detail
