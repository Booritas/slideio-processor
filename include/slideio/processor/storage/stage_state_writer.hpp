// Requirements: UR-STG-4, UR-STG-2, UR-OBS-5
#pragma once

/**
 * @file stage_state_writer.hpp
 * @brief StageStateWriter: the write side of stage state.
 */

#include <slideio/processor/core/export.hpp>
#include <slideio/processor/core/ids.hpp>
#include <slideio/processor/storage/types.hpp>

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

private:
    friend class Store;
    explicit StageStateWriter(detail::StoreImpl& store);
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace slideio::processor
