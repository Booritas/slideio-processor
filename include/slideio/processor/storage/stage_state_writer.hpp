// Requirements: UR-STG-4, UR-STG-2, UR-OBS-5
#pragma once

/**
 * @file stage_state_writer.hpp
 * @brief StageStateWriter: the write side of stage state.
 */

#include <slideio/processor/core/export.hpp>
#include <slideio/processor/core/ids.hpp>
#include <slideio/processor/storage/types.hpp>

#include <cstdint>
#include <memory>

namespace slideio::processor {

namespace detail {
class StoreImpl;
} // namespace detail

/**
 * @brief Records stage state in a store; the workspace writes stage rows only through
 * this class, and viewer processes read them through StageStateReader.
 *
 * Every member is one write transaction. Every member throws IoError on a closed or
 * ReadOnly store, and InvalidArgumentError for an unknown stage. A null configuration
 * is stored as "". A change of status appends a StageStateChanged event in the same
 * transaction.
 * @par Thread-safety
 * ThreadSafe: every member may be called from any thread with no external locking.
 * Writes are serialised.
 * @par Requirements
 * UR-STG-4, UR-STG-2, UR-OBS-5
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS StageStateWriter {
public:
    /** @brief Releases the writer. */
    ~StageStateWriter();
    StageStateWriter(const StageStateWriter&) = delete;             ///< Not copyable.
    StageStateWriter& operator=(const StageStateWriter&) = delete;  ///< Not copyable.

    /**
     * @brief Adds a stage in status Pending, not stale, never completed.
     *
     * Stage ids number from 1. No event is appended: a stage that has not changed
     * status has nothing to report.
     * @param name UTF-8 stage name, unique and compared exactly.
     * @param configurationJson The stage configuration as JSON; null stores "".
     * @return The new stage id.
     * @throws InvalidArgumentError When name is null, empty or already declared.
     * @throws IoError When the store is closed or read-only, or when SQLite fails.
     * @throws std::bad_alloc When memory is exhausted.
     */
    StageId declare(const char* name, const char* configurationJson);
    /**
     * @brief Sets the status of a stage and appends a StageStateChanged event.
     *
     * Entering Complete sets the completion time to now; no other transition changes
     * it. The event is appended even when the status is unchanged.
     * @param stage The stage.
     * @param status The new status.
     * @throws InvalidArgumentError When the stage is unknown or status is not a
     * StageStatus.
     * @throws IoError When the store is closed or read-only, or when SQLite fails.
     */
    void setStatus(StageId stage, StageStatus status);
    /**
     * @brief Marks a stage stale or fresh. No event is appended.
     * @param stage The stage.
     * @param stale True when the stage output is out of date with its inputs.
     * @throws InvalidArgumentError When the stage is unknown.
     * @throws IoError When the store is closed or read-only, or when SQLite fails.
     */
    void setStale(StageId stage, bool stale);
    /**
     * @brief Replaces a stage configuration. No event is appended.
     * @param stage The stage.
     * @param configurationJson The configuration as JSON; null stores "".
     * @throws InvalidArgumentError When the stage is unknown.
     * @throws IoError When the store is closed or read-only, or when SQLite fails.
     * @throws std::bad_alloc When memory is exhausted.
     */
    void setConfiguration(StageId stage, const char* configurationJson);
    /**
     * @brief Records that a stage depends on another. Repeating it is a no-op.
     * @param stage The dependent stage.
     * @param dependsOn The stage it depends on.
     * @throws InvalidArgumentError When either stage is unknown or they are the same.
     * @throws IoError When the store is closed or read-only, or when SQLite fails.
     */
    void addDependency(StageId stage, StageId dependsOn);
    /**
     * @brief Removes every dependency of a stage.
     * @param stage The stage.
     * @throws InvalidArgumentError When the stage is unknown.
     * @throws IoError When the store is closed or read-only, or when SQLite fails.
     */
    void clearDependencies(StageId stage);
    /**
     * @brief Records that a stage wrote to a layer. Repeating it is a no-op.
     * @param stage The stage.
     * @param layer An existing layer other than the pixel layer.
     * @throws InvalidArgumentError When the stage is unknown, or the layer is unknown
     * or is the pixel layer.
     * @throws IoError When the store is closed or read-only, or when SQLite fails.
     */
    void addWrittenLayer(StageId stage, LayerId layer);
    /**
     * @brief Returns how many changes of a stage's status, completion time or stale flag
     * this writer has committed since the store opened.
     *
     * Held in memory, not stored. setStatus() and setStale() advance it once each, after
     * their transaction commits, so a reader that notes it before reading stage state and
     * sees it unchanged later knows no such change committed in between. The store's
     * exclusive writer lock makes this the only writer of that state while it is open, so
     * StageRegistry keys the staleness causes it caches on it.
     * @return The count; 0 before any change.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadSafe.
     */
    uint64_t revision() const noexcept;

    /**
     * @brief Groups the writes one thread makes through the writers of one store into a
     * single transaction, which commits whole or not at all.
     *
     * The constructor takes the store's write lock and begins the transaction. Until
     * commit() or the destructor, every write the same thread makes to that store joins
     * the batch instead of committing on its own. A join is a savepoint: a write that
     * succeeds is kept in the batch; a write that throws undoes only itself, throws the
     * exception it throws outside a batch, and leaves the batch able to commit. Writes by
     * other threads wait for the batch to end, as they wait for any write.
     *
     * Use it for StageStateWriter writes. A rollback undoes only the SQL rows; it does
     * not undo what a writer keeps in memory about a write that joined. A writer that
     * buffers (ObjectWriter::flush, say, which moves its pending objects to its committed
     * count once its write returns) would, after a rolled-back batch, have lost those
     * objects and report a wrong count.
     *
     * Reads, including those on the batch's own thread, go through pooled read
     * connections: they see none of the batch's writes before commit() and all of them
     * after, as other processes do.
     *
     * Rules:
     * - One batch per thread and store: a second Batch on a thread that holds one throws.
     * - A durable write (a commit that is on disk before it returns, as
     *   RasterRegistry::discardRasterOfStage makes) cannot join and throws
     *   InvalidArgumentError, since the synchronous level cannot change inside a
     *   transaction. The batch is unaffected.
     * - SQLite may abort the whole transaction on an error such as a full disk or an
     *   I/O failure, not only the failing statement. The failing write throws; every
     *   later write in the batch and commit() then throw IoError, and nothing of the
     *   batch is stored.
     * - Create, commit() and destroy a Batch on one thread, and close the store on
     *   another or after the batch has ended: Store::close() waits for a batch of another
     *   thread, and deadlocks against one held by the thread that calls it.
     * - Keep a batch short. It holds the write lock, so every other writer of the store
     *   waits for it.
     * - A setStatus() in a batch posts its commit notification when the batch commits,
     *   and never when it rolls back.
     * - revision() counts every setStatus() and setStale() call, as it does outside a
     *   batch. It may therefore count a change that a rolled-back batch then discarded;
     *   it never misses a change that committed. Callers that key a cache on it
     *   re-read, so a spurious count costs only a re-read.
     *
     * Not copyable or movable.
     * @par Thread-safety
     * NotThreadSafe: use it only on the thread that created it. Other threads' writes to
     * the store are safe and wait for it.
     * @par Requirements
     * UR-STG-4
     */
    class SLIDEIO_PROCESSOR_DATA_EXPORTS Batch {
    public:
        /**
         * @brief Begins the batch.
         * @param writer A writer of the store to group writes in.
         * @throws IoError When the store is closed or read-only, or when SQLite fails.
         * @throws InvalidArgumentError When this thread already holds a batch on the store.
         */
        explicit Batch(StageStateWriter& writer);
        /**
         * @brief Rolls the batch back unless commit() ran. Never throws.
         *
         * Destroyed on a thread other than the one that created it, it cannot release
         * the write lock: it logs an Error, does not roll back, and the lock stays held.
         */
        ~Batch();
        Batch(const Batch&) = delete;             ///< Not copyable.
        Batch& operator=(const Batch&) = delete;  ///< Not copyable.

        /**
         * @brief Commits every write of the batch at once, then posts the notification
         * a setStatus() in it deferred.
         *
         * The batch has ended afterwards, whether or not the commit succeeded.
         * @throws InvalidArgumentError When the batch has already been committed, or when
         * called on a thread other than the one that created the batch.
         * @throws IoError When the commit fails; the batch is then rolled back.
         */
        void commit();

    private:
        detail::StoreImpl* store_;
        bool open_;
    };

private:
    friend class Store;
    explicit StageStateWriter(detail::StoreImpl& store);
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace slideio::processor
