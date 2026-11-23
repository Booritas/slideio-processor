// Requirements: UR-RES-12, UR-BLD-5, UR-OBS-1, UR-RES-11
#pragma once

/**
 * @file store.hpp
 * @brief Store and StorePtr: the store file, its open modes and its provenance.
 */

#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/export.hpp>
#include <slideio/processor/core/provenance.hpp>
#include <slideio/processor/storage/annotation_store.hpp>
#include <slideio/processor/storage/edge_store.hpp>
#include <slideio/processor/storage/event_journal.hpp>
#include <slideio/processor/storage/layer_registry.hpp>
#include <slideio/processor/storage/object_store.hpp>
#include <slideio/processor/storage/raster_store.hpp>
#include <slideio/processor/storage/stage_state_reader.hpp>
#include <slideio/processor/storage/stage_state_writer.hpp>
#include <slideio/processor/storage/store_format.hpp>
#include <slideio/processor/storage/types.hpp>
#include <slideio/processor/storage/vocabulary_registry.hpp>

#include <cstdint>
#include <memory>

namespace slideio::processor {

class Store;

namespace detail {
/** @brief Library-internal; not API. */
class GateClient;
#if defined(SLIDEIO_PROCESSOR_TEST_SEAMS)
/** @brief Test seam, not API: library-internal test access to Store::close(). */
SLIDEIO_PROCESSOR_DATA_EXPORTS void trackForCloseTesting(Store& store, const std::shared_ptr<GateClient>& client);
#endif
/** @brief Library-internal; not API. */
class StoreImpl;
/** @brief Library-internal, not API and not exported: the shared core of an open Store. */
StoreImpl& storeImplOf(Store& store) noexcept;
} // namespace detail
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
 * member of a ReadOnly store throws IoError naming the store (by its role and its path) and the word
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
     * The store's format version is read before anything else. Versions 1 and 2
     * (storeFormatVersion()) are accepted, and a store of any other version is refused
     * rather than misread. A ReadWrite open of a version 1 store migrates it to version 2
     * in place, in one durable transaction, before the store is returned; a ReadOnly open
     * leaves it at version 1.
     * @param path UTF-8 path of the store file.
     * @param mode CreateNew creates the file, which must not exist. ReadWrite and
     * ReadOnly open an existing file; ReadWrite takes the exclusive writer lock,
     * ReadOnly takes none.
     * @param configuration The settings the store uses; copied.
     * @return The open store.
     * @throws InvalidArgumentError When path is null or empty, or mode is not an
     * OpenMode.
     * @throws IoError When CreateNew finds the path existing; when ReadWrite or
     * ReadOnly finds it missing (the message names the store by its role and its
     * path); when another handle, in this process or another, holds the writer lock
     * (the message says so and, where it is known, "process <pid>"); when the migration of a version 1
     * store fails, which leaves the file at version 1; or when SQLite fails (the cause
     * is SQLite's message).
     * @throws StoreFormatError When the file is not a store (storeVersion() 0) or has
     * a format version other than 1 or storeFormatVersion(); the message names both
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
     * @return The version of the file as opened: storeFormatVersion() (2), including
     * for a version 1 store a writable open has migrated; 1 for a version 1 store opened
     * ReadOnly. Answers after close().
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
     * @brief Tells whether the store is still open.
     * @return False once close() has run; a caller that caches answers read from the
     * store uses it to stop answering from the cache.
     * @throws Nothing.
     */
    bool isOpen() const noexcept;

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
     * @brief Returns the raster registry of this store.
     * @return The registry, valid while this store lives. Usable until close(); a
     * ReadOnly store answers every query and refuses declarations, writers and
     * discards.
     * @throws Nothing.
     */
    RasterRegistry& rasters() noexcept;
    /** @copydoc rasters() */
    const RasterRegistry& rasters() const noexcept;
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
     * @brief Returns the edge store of this store.
     * @return The edge store, valid while this store lives. Usable until close(); a
     * ReadOnly store answers every read and refuses writers and discards. A writer it
     * returned throws IoError once the store is closed or destroyed.
     * @throws Nothing.
     */
    EdgeStore& edges() noexcept;
    /**
     * @brief Returns the annotation store of this store.
     * @return The annotation store, valid while this store lives. Usable until close();
     * a ReadOnly store answers every read and refuses writers and discards. A writer it
     * returned throws IoError once the store is closed or destroyed.
     * @throws Nothing.
     */
    AnnotationStore& annotations() noexcept;
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
     * Every live ObjectWriter, EdgeWriter and AnnotationWriter first commits what it
     * holds pending, as by
     * flush(); a writer used afterwards throws IoError, and an emit() or assert racing
     * close() is refused with IoError "store is closed" rather than accepted and lost. The
     * store is closed even when a flush fails: every writer is tried, and the first
     * failure is then reported. Errors during teardown are reported here rather than
     * in the destructor, which logs them.
     *
     * A live RasterWriter is abandoned, never flushed: its spool and any partial file are
     * deleted and nothing is published, because closing must not build a pyramid. Its
     * raster keeps the state it had.
     * @throws InvalidArgumentError When a writer's pending batch collides with a
     * committed object's anchor, holds an edge whose endpoint does not exist or that
     * conflicts with a committed parent edge, or holds an annotation whose object does
     * not exist or that conflicts with a committed measurement; that batch is rolled
     * back and dropped.
     * @throws IoError When a writer's commit or releasing the store fails.
     */
    void close();

private:
#if defined(SLIDEIO_PROCESSOR_TEST_SEAMS)
    // Test seam, not API: library-internal test access only.
    friend void detail::trackForCloseTesting(Store& store, const std::shared_ptr<detail::GateClient>& client);
#endif
    // Library-internal: GeoJsonImporter reaches the store core through it.
    friend detail::StoreImpl& detail::storeImplOf(Store& store) noexcept;
    class Impl;
    explicit Store(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace slideio::processor
