// Requirements: UR-RES-12, UR-OBS-1, UR-RES-11
#pragma once

// The shared core of an open store (rulings S2, S3, S4): its connections, the write
// mutex, the writer lock and the meta table. Every storage class owned by Store holds a
// StoreImpl&; a handle that may outlive the Store holds a ReadLease (cursors) or the
// StoreGate (writers) instead.
//
// Writes run on one write connection, serialised by the write mutex, each inside
// withWriteTransaction. Reads check a connection out of a pool, one per concurrent
// caller: withReadConnection for a call, acquireRead for a cursor's lifetime.

#include "storage/detail/change_signal.hpp"
#include "storage/detail/sqlite.hpp"
#include "storage/detail/writer_lock.hpp"

#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/export.hpp>
#include <slideio/processor/core/provenance.hpp>
#include <slideio/processor/storage/types.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <shared_mutex>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace slideio::processor {
class Store;
} // namespace slideio::processor

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

    // Throws IoError "<operation>: store is closed ('<path>')" once the store that issued
    // this lease has closed or been destroyed. Reads only state the lease itself shares.
    void checkStoreOpen(const char* operation, const std::string& path) const;

private:
    std::shared_ptr<ConnectionPool> pool_;
    Connection connection_;
};

// What a waiting event cursor blocks on. Shared (shared_ptr) between the store and its
// cursors, so a cursor never reaches back into a destroyed StoreImpl. It holds the
// store's wake-up signal (ruling S13), or none: signal disabled, unsupported, or failed
// to open. Without one, waits sleep and cursors poll.
class CommitWaiter {
public:
    explicit CommitWaiter(std::unique_ptr<ChangeSignal> signal = nullptr) noexcept : signal_(std::move(signal)) {}

    // Posts the wake-up after a commit that appended an event; a no-op without a signal.
    void notify() noexcept;
    // Blocks for at most timeoutMs, or until a commit in any process posts the signal.
    // Without a signal it sleeps the whole time. Never throws.
    void wait(int32_t timeoutMs) const noexcept;
    // Removes the signal's name (the writer, on close; see ChangeSignal::unlink).
    void unlinkSignal() noexcept;

private:
    std::unique_ptr<ChangeSignal> signal_;
};

class StoreImpl;

// A handle holding writes that close() must commit before the store goes (a writer's
// pending batch, SDS 03 §3.2).
class GateClient {
public:
    virtual ~GateClient() = default;
    // Commits what is pending. Called by StoreImpl::close() while the store is still
    // open, holding no store lock; it reaches the store through the gate as usual.
    virtual void flushBeforeClose() = 0;
};

// The route by which a handle that may outlive its Store (an ObjectWriterPtr) reaches
// the StoreImpl. Shared (shared_ptr) between the store and such handles. close() first
// flushes the live clients, then detaches the gate, waiting for calls already inside
// with() to return; from then on with() throws IoError "<operation>: store is closed
// ('<path>')" and never touches the StoreImpl again.
class StoreGate {
public:
    StoreGate(StoreImpl* store, std::string path) : store_(store), path_(std::move(path)) {}
    StoreGate(const StoreGate&) = delete;
    StoreGate& operator=(const StoreGate&) = delete;

    // Runs fn(StoreImpl&) while the store cannot be detached, and returns its result.
    // fn must not call with() again on this gate.
    template <class Fn>
    auto with(const char* operation, Fn&& fn) -> decltype(fn(std::declval<StoreImpl&>()));

    // Remembers a client, weakly, for flushClients. Throws std::bad_alloc.
    void track(const std::shared_ptr<GateClient>& client) {
        std::lock_guard<std::mutex> guard(clientsMutex_);
        clients_.erase(std::remove_if(clients_.begin(), clients_.end(),
                                      [](const std::weak_ptr<GateClient>& c) { return c.expired(); }),
                       clients_.end());
        clients_.push_back(client);
    }

    // Calls flushBeforeClose on every live client, in registration order, and returns the
    // first exception thrown (null when none), after trying them all.
    std::exception_ptr flushClients() noexcept {
        std::vector<std::shared_ptr<GateClient>> live;
        std::exception_ptr first;
        try {
            std::lock_guard<std::mutex> guard(clientsMutex_);
            for (const std::weak_ptr<GateClient>& c : clients_) {
                if (std::shared_ptr<GateClient> client = c.lock()) live.push_back(std::move(client));
            }
        } catch (...) {
            first = std::current_exception();
        }
        for (const std::shared_ptr<GateClient>& client : live) {
            try {
                client->flushBeforeClose();
            } catch (...) {
                if (!first) first = std::current_exception();
            }
        }
        return first;
    }

    void detach() noexcept {
        std::unique_lock<std::shared_mutex> guard(mutex_);
        store_ = nullptr;
    }

    const std::string& path() const noexcept { return path_; }

private:
    std::shared_mutex mutex_;
    StoreImpl* store_;
    const std::string path_;
    std::mutex clientsMutex_;
    std::vector<std::weak_ptr<GateClient>> clients_;
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
    // checkWritable, then throws IoError "<operation>: store is closed ('<path>')" once
    // close() has begun: a new object accepted then might be queued after its writer's
    // flush and lost. For ObjectWriter::emit only; commits (close's own flush among them)
    // use checkWritable.
    void checkAcceptingObjects(const char* operation) const;

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
    // subscribers waiting on the CommitWaiter wake at once. Correctness never depends
    // on it.
    void notifyCommitted() noexcept;

    // The waiter event cursors share; outlives this object while a cursor holds it.
    std::shared_ptr<CommitWaiter> commitWaiter() const noexcept { return waiter_; }

    // Flushes every live writer (GateClient), then releases every connection and the
    // writer lock. Idempotent. The store is closed even when a flush fails; the first
    // flush failure is then rethrown (InvalidArgumentError for an anchor collision,
    // IoError for SQLite). Throws IoError when
    // the write connection fails to close.
    void close();

    // The gate handles that may outlive this store reach it through; detached by
    // close().
    std::shared_ptr<StoreGate> gate() const noexcept { return gate_; }

private:
    void openExisting();
    void createNew();
    // Gives waiter_ the wake-up signal named for storeUuid_ when the configuration enables
    // it and the platform supports it. Never throws: a failure logs a Warning, and
    // cursors poll.
    void openChangeSignal() noexcept;
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
    std::string storeUuid_;  // meta.store_uuid; names the wake-up signal
    std::atomic<bool> open_{false};
    std::atomic<bool> closing_{false};  // set once close() begins; never cleared
    WriterLock writerLock_;
    mutable std::mutex writeMutex_;
    Connection writeConnection_;
    std::shared_ptr<ConnectionPool> pool_;
    std::shared_ptr<CommitWaiter> waiter_ = std::make_shared<CommitWaiter>();
    std::shared_ptr<StoreGate> gate_;
};

template <class Fn>
auto StoreGate::with(const char* operation, Fn&& fn) -> decltype(fn(std::declval<StoreImpl&>())) {
    std::shared_lock<std::shared_mutex> guard(mutex_);
    if (store_ == nullptr) {
        const std::string message = std::string(operation) + ": store is closed ('" + path_ + "')";
        throw IoError(message.c_str());
    }
    return fn(*store_);
}

// Test access to an open Store's internals, for detail tests that need to act inside
// Store::close() (a GateClient tracked on the gate). Exported, as a Store lives in the
// library; nothing in the library calls it.
struct SLIDEIO_PROCESSOR_DATA_EXPORTS StoreAccess {
    static std::shared_ptr<StoreGate> gate(Store& store);
};

// Takes the writer lock for creating a store at `path` (UTF-8), which the caller found
// free; another creator may have taken it since. Throws IoError "Store::open: '<path>'
// already exists; CreateNew needs a new path" when the store exists by then, whether
// its creator still holds the lock or not, and leaves that store and its lock file
// untouched; otherwise throws as WriterLock::acquire.
WriterLock lockForCreateNew(const std::string& path);

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
