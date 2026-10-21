// Requirements: UR-RES-12, UR-BLD-5, UR-OBS-1, UR-RES-11
#pragma once

/**
 * @file store.hpp
 * @brief Store and StorePtr: the store file, its open modes and its provenance.
 */

#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/export.hpp>
#include <slideio/processor/core/provenance.hpp>
#include <slideio/processor/storage/event_journal.hpp>
#include <slideio/processor/storage/layer_registry.hpp>
#include <slideio/processor/storage/object_store.hpp>
#include <slideio/processor/storage/stage_state_reader.hpp>
#include <slideio/processor/storage/stage_state_writer.hpp>
#include <slideio/processor/storage/store_format.hpp>
#include <slideio/processor/storage/types.hpp>
#include <slideio/processor/storage/vocabulary_registry.hpp>

#include <cstdint>
#include <memory>

namespace slideio::processor {

namespace detail {
struct StoreAccess;
} // namespace detail

class Store;
/** @brief Shared ownership of an open Store. */
using StorePtr = std::shared_ptr<Store>;

/**
 * @brief An open store file: one slide's durable objects, layers, labels, stage
 * state, provenance and event journal.
 *
 * The file is a SQLite database in WAL mode. A ReadOnly store in one process reads
 * concurrently with a ReadWrite store in another, neither blocking the other, and
 * each read sees a consistent snapshot, never a partially applied transaction. A
 * ReadWrite or CreateNew store holds an exclusive writer lock, in a file
 * "<path>-writer.lock" beside the store, until close().
 *
 * After close(), path(), formatVersion(), mode() and isReadOnly() still answer;
 * every other member throws IoError containing "store is closed". Every mutating
 * member of a ReadOnly store throws IoError naming the path and the word
 * "read-only".
 * @par Thread-safety
 * ThreadSafe: every member may be called from any thread with no external locking.
 * Each concurrent reader uses its own connection; writes are serialised.
 * @par Requirements
 * UR-RES-12, UR-BLD-5, UR-OBS-1, UR-RES-11
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS Store {
public:
    /**
     * @brief Opens or creates a store.
     *
     * The store's format version is read before anything else, and a store of
     * another version is refused rather than misread.
     * @param path UTF-8 path of the store file.
     * @param mode CreateNew creates the file, which must not exist. ReadWrite and
     * ReadOnly open an existing file; ReadWrite takes the exclusive writer lock,
     * ReadOnly takes none.
     * @param configuration The settings the store uses; copied.
     * @return The open store.
     * @throws InvalidArgumentError When path is null or empty, or mode is not an
     * OpenMode.
     * @throws IoError When CreateNew finds the path existing; when ReadWrite or
     * ReadOnly finds it missing (the message names the path); when another handle,
     * in this process or another, holds the writer lock (the message names the path
     * and, where it is known, "process <pid>"); or when SQLite fails (the cause is
     * SQLite's message).
     * @throws StoreFormatError When the file is not a store (storeVersion() 0) or has
     * a format version other than storeFormatVersion(); the message names both
     * versions.
     * @throws std::bad_alloc When memory is exhausted.
     */
    static StorePtr open(const char* path, OpenMode mode, const Configuration& configuration);
    /** @brief Calls close(); an error is logged at LogLevel::Error and swallowed. */
    ~Store();
    Store(const Store&) = delete;             ///< Not copyable.
    Store& operator=(const Store&) = delete;  ///< Not copyable.

    /**
     * @brief Returns the path the store was opened with.
     * @return A null-terminated UTF-8 string, never null, valid while this object
     * lives. Answers after close().
     * @throws Nothing.
     */
    const char* path() const noexcept;
    /**
     * @brief Returns the store's format version.
     * @return The version read from the file, equal to storeFormatVersion(). Answers
     * after close().
     * @throws Nothing.
     */
    int32_t formatVersion() const noexcept;
    /**
     * @brief Returns the mode the store was opened with.
     * @return The mode passed to open(). Answers after close().
     * @throws Nothing.
     */
    OpenMode mode() const noexcept;
    /**
     * @brief Tells whether the store refuses writes.
     * @return True only for OpenMode::ReadOnly. Answers after close().
     * @throws Nothing.
     */
    bool isReadOnly() const noexcept;

    /**
     * @brief Returns the stored provenance record.
     *
     * The library version and creation time are the stored ones, not the reading
     * library's or the time of the read. A new store holds a default Provenance
     * built when it was created.
     * @return The record.
     * @throws IoError When the store is closed, or when SQLite fails.
     * @throws InternalError When the stored record is unreadable.
     * @throws std::bad_alloc When memory is exhausted.
     */
    Provenance provenance() const;
    /**
     * @brief Replaces the stored provenance record.
     * @param provenance The record to store, including its library version and
     * creation time.
     * @throws IoError When the store is closed or read-only, or when SQLite fails;
     * the stored record is then unchanged.
     * @throws std::bad_alloc When memory is exhausted.
     */
    void setProvenance(const Provenance& provenance);

    /**
     * @brief Returns the event journal of this store.
     * @return The journal, valid while this store lives. Usable until close().
     * @throws Nothing.
     */
    EventJournal& events() noexcept;
    /**
     * @brief Returns the layer registry of this store.
     * @return The registry, valid while this store lives. Usable until close(); a
     * ReadOnly store answers every query and refuses declarations.
     * @throws Nothing.
     */
    LayerRegistry& layers() noexcept;
    /** @copydoc layers() */
    const LayerRegistry& layers() const noexcept;
    /**
     * @brief Returns the vocabulary registry of this store.
     * @return The registry, valid while this store lives. Usable until close(); a
     * ReadOnly store answers every query and refuses additions.
     * @throws Nothing.
     */
    VocabularyRegistry& vocabulary() noexcept;
    /** @copydoc vocabulary() */
    const VocabularyRegistry& vocabulary() const noexcept;
    /**
     * @brief Returns the object store of this store.
     * @return The object store, valid while this store lives. Usable until close(); a
     * ReadOnly store answers every query and refuses writers and discards. A writer
     * it returned throws IoError once the store is closed or destroyed.
     * @throws Nothing.
     */
    ObjectStore& objects() noexcept;
    /**
     * @brief Returns the stage state reader of this store.
     * @return The reader, valid while this store lives. Usable until close(); a
     * ReadOnly store answers every member.
     * @throws Nothing.
     */
    StageStateReader& stageStates() noexcept;
    /**
     * @brief Returns the stage state writer of this store.
     *
     * The workspace records stage rows only through it. On a ReadOnly store the
     * writer exists and every member throws IoError.
     * @return The writer, valid while this store lives. Usable until close().
     * @throws Nothing.
     */
    StageStateWriter& stageStateWriter() noexcept;

    /**
     * @brief Flushes pending batches and releases every connection and the writer
     * lock. Idempotent.
     *
     * Every live ObjectWriter first commits what it holds pending, as by flush(); a
     * writer used afterwards throws IoError. The store is closed even when a flush
     * fails: every writer is tried, and the first failure is then reported. Errors
     * during teardown are reported here rather than in the destructor, which logs them.
     * @throws InvalidArgumentError When a writer's pending batch collides with a
     * committed object's anchor; that batch is rolled back and dropped.
     * @throws IoError When a writer's commit or releasing the store fails.
     */
    void close();

private:
    friend struct detail::StoreAccess;  // library-internal test access
    class Impl;
    explicit Store(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace slideio::processor
