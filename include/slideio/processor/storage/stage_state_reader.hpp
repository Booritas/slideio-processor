// Requirements: UR-STG-4, UR-STG-2, UR-STG-10
#pragma once

/**
 * @file stage_state_reader.hpp
 * @brief StageStateReader: the read side of stage state.
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
 * @brief Reads the state of the stages recorded in a store: status, staleness,
 * configuration, dependencies and the layers each stage wrote.
 *
 * Declaring, running and invalidating stages belong to the workspace, which writes
 * through StageStateWriter; this reader lets an observer report "nuclei: complete,
 * cells: running" without linking the engine. A ReadOnly store answers every member.
 * Stages are ordered by ascending StageId, dependencies by ascending dependency id and
 * written layers by ascending LayerId.
 * @par Thread-safety
 * ThreadSafe: every member may be called from any thread with no external locking.
 * @par Requirements
 * UR-STG-4, UR-STG-2, UR-STG-10
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS StageStateReader {
public:
    /** @brief Releases the reader. */
    ~StageStateReader();
    StageStateReader(const StageStateReader&) = delete;             ///< Not copyable.
    StageStateReader& operator=(const StageStateReader&) = delete;  ///< Not copyable.

    /**
     * @brief Returns the number of stages.
     * @return The count; 0 when the store is closed or SQLite fails.
     * @throws Nothing.
     */
    int32_t count() const noexcept;
    /**
     * @brief Returns the stage at a position in ascending id order.
     * @param index Zero-based position, below count().
     * @return The stage id.
     * @throws InvalidArgumentError When index is negative or not below count().
     * @throws IoError When the store is closed, or when SQLite fails.
     */
    StageId at(int32_t index) const;
    /**
     * @brief Finds a stage by name; the comparison is exact, with no case folding.
     * @param name UTF-8 stage name.
     * @param out Receives the id when found; unchanged otherwise.
     * @return True when the stage exists; false for a null name, an unknown name, a
     * closed store or a SQLite failure.
     * @throws Nothing.
     */
    bool find(const char* name, StageId& out) const noexcept;

    /**
     * @brief Returns the name of a stage.
     * @param stage The stage.
     * @return A null-terminated UTF-8 string, valid for as long as this reader lives;
     * the name never changes.
     * @throws InvalidArgumentError When the stage is unknown.
     * @throws IoError When the store is closed, or when SQLite fails.
     * @throws std::bad_alloc When memory is exhausted.
     */
    const char* name(StageId stage) const;
    /**
     * @brief Returns the status of a stage.
     * @param stage The stage.
     * @return The status.
     * @throws InvalidArgumentError When the stage is unknown.
     * @throws IoError When the store is closed, or when SQLite fails.
     */
    StageStatus status(StageId stage) const;
    /**
     * @brief Tells whether a stage's output is out of date with its inputs.
     * @param stage The stage.
     * @return True when the stage is marked stale.
     * @throws InvalidArgumentError When the stage is unknown.
     * @throws IoError When the store is closed, or when SQLite fails.
     */
    bool isStale(StageId stage) const;
    /**
     * @brief Returns the configuration JSON recorded for a stage.
     * @param stage The stage.
     * @return A null-terminated UTF-8 string, "" when none was recorded. The pointer
     * stays valid for as long as this reader lives and always holds the value read by
     * this call: a later change to the configuration is seen by a later call, which
     * returns a different pointer. Distinct values read are retained until the reader
     * is destroyed.
     * @throws InvalidArgumentError When the stage is unknown.
     * @throws IoError When the store is closed, or when SQLite fails.
     * @throws std::bad_alloc When memory is exhausted.
     */
    const char* configuration(StageId stage) const;
    /**
     * @brief Returns when a stage last entered Complete.
     * @param stage The stage.
     * @return Milliseconds since the Unix epoch; 0 if the stage never completed.
     * @throws InvalidArgumentError When the stage is unknown.
     * @throws IoError When the store is closed, or when SQLite fails.
     */
    int64_t completedAtUnixMs(StageId stage) const;

    /**
     * @brief Returns the number of stages a stage depends on.
     * @param stage The stage.
     * @return The count.
     * @throws InvalidArgumentError When the stage is unknown.
     * @throws IoError When the store is closed, or when SQLite fails.
     */
    int32_t dependencyCount(StageId stage) const;
    /**
     * @brief Returns one dependency of a stage, in ascending id order.
     * @param stage The stage.
     * @param index Zero-based position, below dependencyCount().
     * @return The stage depended on.
     * @throws InvalidArgumentError When the stage is unknown or index is out of range.
     * @throws IoError When the store is closed, or when SQLite fails.
     */
    StageId dependencyAt(StageId stage, int32_t index) const;
    /**
     * @brief Returns the number of layers a stage has written to.
     * @param stage The stage.
     * @return The count.
     * @throws InvalidArgumentError When the stage is unknown.
     * @throws IoError When the store is closed, or when SQLite fails.
     */
    int32_t writtenLayerCount(StageId stage) const;
    /**
     * @brief Returns one layer a stage has written to, in ascending id order.
     * @param stage The stage.
     * @param index Zero-based position, below writtenLayerCount().
     * @return The layer.
     * @throws InvalidArgumentError When the stage is unknown or index is out of range.
     * @throws IoError When the store is closed, or when SQLite fails.
     */
    LayerId writtenLayerAt(StageId stage, int32_t index) const;

private:
    friend class Store;
    explicit StageStateReader(detail::StoreImpl& store);
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace slideio::processor
