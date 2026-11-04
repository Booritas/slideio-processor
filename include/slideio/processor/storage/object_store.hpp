// Requirements: UR-RES-1, UR-RES-2, UR-RES-3, UR-EXE-7, UR-STG-5, UR-STG-6, UR-OBS-2, UR-OBS-4, UR-QA-3
#pragma once

/**
 * @file object_store.hpp
 * @brief ObjectWriter and ObjectStore: batched object writes, spatial queries and
 * stage discard.
 */

#include <slideio/processor/core/export.hpp>
#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/core/ids.hpp>
#include <slideio/processor/core/object_query.hpp>
#include <slideio/processor/core/object_record.hpp>

#include <cstdint>
#include <memory>

namespace slideio::processor {

namespace detail {
class StoreImpl;
class StoreGate;
} // namespace detail

class ObjectWriter;
/** @brief Shared ownership of an ObjectWriter. */
using ObjectWriterPtr = std::shared_ptr<ObjectWriter>;

/**
 * @brief Writes objects into a store, attributing every one to the stage it was
 * created for.
 *
 * One emit() inserts nothing by itself. Accepted records accumulate until
 * batchSizeObjects() are pending; then one transaction inserts their object rows,
 * their R*Tree rows, their measurements and one ObjectsAdded event per distinct
 * layer of the batch (ascending layer id, each with that layer's count and union
 * bounding box), together or not at all (UR-OBS-2). Objects are inserted in arrival
 * order. flush() commits what is pending.
 *
 * emit() validates each record and throws InvalidArgumentError, adding nothing, for:
 * an undeclared or dropped layer, or the pixel layer; empty geometry; an anchor
 * outside the identifiable range (the message names the anchor); a label the store
 * does not hold; or an anchor already pending in this writer for the same layer (the
 * message names the layer and the anchor). An anchor that collides with an object
 * already committed (by this writer, another writer or another stage) is caught at
 * commit: the whole batch is rolled back, its pending objects are dropped, and the
 * emit() or flush() that triggered the commit throws InvalidArgumentError naming the
 * layer and the anchor.
 *
 * Store::close() (and so the Store's destruction) commits what a live writer holds
 * pending. A writer used after that, by emit() or by a flush() with something pending,
 * throws IoError containing "store is closed".
 * @par Thread-safety
 * emit(), emittedCount(), batchSizeObjects(), setBatchSizeObjects() and stage() are
 * ThreadSafe. flush() requires exclusive access: no emit() may run concurrently with
 * it on the same writer. Any number of writers may be used concurrently; their
 * commits are serialised.
 * @par Requirements
 * UR-RES-1, UR-EXE-7, UR-STG-6, UR-OBS-2, UR-QA-3
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS ObjectWriter : public ObjectSink {
public:
    /**
     * @brief Flushes what is pending; a failure is logged at LogLevel::Error and
     * swallowed.
     */
    ~ObjectWriter() override;
    ObjectWriter(const ObjectWriter&) = delete;             ///< Not copyable.
    ObjectWriter& operator=(const ObjectWriter&) = delete;  ///< Not copyable.

    /**
     * @brief Accepts a record, committing the pending batch once it reaches
     * batchSizeObjects().
     *
     * Writes the record with this writer's stage() as its provenance. The record's
     * own stage() is not read and cannot override it; the layer, geometry, label and
     * measurements come from the record.
     * @param record The record; copied.
     * @throws InvalidArgumentError When the record is refused (see the class
     * description), or when the commit it triggered found an anchor collision; the
     * batch is then dropped.
     * @throws IoError When the store is closed or Store::close() has begun (the record
     * is not accepted), or when SQLite fails during a commit; the batch is then
     * dropped.
     * @throws std::bad_alloc When memory is exhausted.
     */
    void emit(const ObjectRecord& record) override;
    /**
     * @brief Commits every pending record. A no-op when nothing is pending.
     * @throws InvalidArgumentError When an anchor collides with a committed object;
     * the batch is rolled back and dropped.
     * @throws IoError When the store is closed, or when SQLite fails; the batch is
     * dropped.
     * @throws std::bad_alloc When memory is exhausted.
     */
    void flush() override;
    /**
     * @brief Returns the number of records accepted and not dropped.
     * @return Committed plus pending records. A batch dropped at commit no longer
     * counts.
     * @throws Nothing in practice.
     */
    int64_t emittedCount() const override;

    /**
     * @brief Returns the number of pending records that triggers a commit.
     * @return The batch size; Configuration::objectBatchSize() until changed.
     * @throws Nothing.
     */
    int64_t batchSizeObjects() const noexcept;
    /**
     * @brief Sets the number of pending records that triggers a commit.
     *
     * Takes effect at the next emit(); records already pending are not committed by
     * this call.
     * @param size The batch size, at least 1.
     * @throws InvalidArgumentError When size is less than 1.
     */
    void setBatchSizeObjects(int64_t size);

    /**
     * @brief Returns the stage every object emitted through this writer is
     * attributed to.
     * @return The stage.
     * @throws Nothing.
     */
    StageId stage() const noexcept;

private:
    friend class ObjectStore;
    ObjectWriter(std::shared_ptr<detail::StoreGate> gate, StageId stage, int64_t batchSize);
    class Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * @brief The objects of a store: writers, queries, point reads and stage discard.
 *
 * Query results are in ascending ObjectId order, by layer and then by packed anchor,
 * whatever the order the objects arrived in (UR-QA-3). A query's
 * StageVisibility::CompletedOnly (the default) admits only objects whose stage status
 * is Complete, stale or not, so a later stage never sees another's partial output
 * (UR-STG-5); IncludeInFlight is the observer's opt-in (UR-OBS-4). get() honours the
 * default view too.
 *
 * A rectangle filter admits an object whose bounding box shares at least one pixel
 * with the rectangle (half-open edges). A polygon filter tests the object's exact
 * geometry on 64-bit integer coordinates: Intersects admits an object whose closed
 * region meets the polygon's, touching included; ContainedBy admits an object whose
 * closed region lies inside or on the polygon's. A measurement bound is existential:
 * it admits an object when the producing stage's measurement or any measurement
 * asserted on it satisfies the bound; an object without the named measurement fails
 * it. Label filters test the effective label set: the object's row label plus every
 * asserted label. AttachmentFilter::AttachedOnly admits an object that has a parent
 * edge, whatever the parent's own visibility, and UnattachedOnly one that has none.
 * An asserted label or measurement, and a parent edge, count only when the stage
 * that asserted it is visible under the query's StageVisibility: Complete, stale or
 * not, under CompletedOnly; any status under IncludeInFlight. A limit applies after
 * every other filter.
 *
 * Every member throws IoError on a closed store, and every mutating member also on a
 * ReadOnly store.
 * @par Thread-safety
 * ThreadSafe: every member may be called from any thread with no external locking.
 * A returned CursorPtr is ThreadConfined.
 * @par Requirements
 * UR-RES-1, UR-RES-2, UR-RES-3, UR-EXE-7, UR-STG-5, UR-STG-6, UR-OBS-4, UR-QA-3
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS ObjectStore : public ObjectSource {
public:
    /** @brief Releases the object store. */
    ~ObjectStore() override;
    ObjectStore(const ObjectStore&) = delete;             ///< Not copyable.
    ObjectStore& operator=(const ObjectStore&) = delete;  ///< Not copyable.

    /**
     * @brief Returns a writer that attributes everything it emits to a stage.
     *
     * Any number may be held concurrently, from any threads; writes are serialised
     * internally into batched transactions.
     * @param stage A declared stage.
     * @return The writer, with batch size Configuration::objectBatchSize().
     * @throws InvalidArgumentError When the stage is unknown.
     * @throws IoError When the store is closed or read-only, or when SQLite fails.
     * @throws std::bad_alloc When memory is exhausted.
     */
    ObjectWriterPtr writerForStage(StageId stage);

    /**
     * @brief Opens a cursor over the objects a query selects.
     *
     * The cursor holds one read transaction for its lifetime, so it sees one snapshot
     * of the store, taken by this call: objects committed later are not returned, and
     * no batch is ever seen in part. position() is the zero-based index of the
     * current record. Each record's stage() is the stage that produced it. The cursor
     * throws IoError once the store is closed or destroyed, and after its own close().
     * @param query The filters.
     * @return The cursor, positioned before the first record.
     * @throws InvalidArgumentError When the query holds a negative limit or a value
     * outside its enumeration.
     * @throws IoError When the store is closed, or when SQLite fails.
     * @throws std::bad_alloc When memory is exhausted.
     */
    CursorPtr query(const ObjectQuery& query) override;
    /**
     * @brief Counts the objects a query selects.
     * @param query The filters.
     * @return The number of records query() would yield.
     * @throws InvalidArgumentError As for query().
     * @throws IoError When the store is closed, or when SQLite fails.
     * @throws InternalError When a stored geometry is corrupt.
     * @throws std::bad_alloc When memory is exhausted.
     */
    int64_t count(const ObjectQuery& query) const override;
    /**
     * @brief Reads one object, in the default view: its stage must be Complete.
     * @param id The object.
     * @param out Receives the record when found; unchanged otherwise. Its stage() is
     * the stage that produced it.
     * @return True when the object exists and its stage is Complete; false for an
     * invalid id.
     * @throws IoError When the store is closed, or when SQLite fails.
     * @throws InternalError When the stored geometry is corrupt.
     * @throws std::bad_alloc When memory is exhausted.
     */
    bool get(ObjectId id, ObjectRecord& out) const override;

    /**
     * @brief Removes every object a stage wrote, with everything about those objects,
     * and appends a StageDiscarded event carrying the number of objects removed, in
     * one transaction.
     *
     * Used by re-run and by failure handling (UR-STG-6). Besides the objects it
     * removes their measurements, the label and measurement assertions on them made
     * by any stage, and the parent edges and derived-from edges with either endpoint
     * among them, asserted by any stage, so nothing re-attaches to a re-run's objects.
     * Objects of other stages are untouched, and so are the stage's own assertions
     * and edges about them: EdgeStore::discardEdgesOfStage() and
     * AnnotationStore::discardAnnotationsOfStage() remove those.
     * @param stage A declared stage.
     * @throws InvalidArgumentError When the stage is unknown.
     * @throws IoError When the store is closed or read-only, or when SQLite fails.
     */
    void discardStage(StageId stage);

    /**
     * @brief Returns the number of stored objects, whatever their stage's status.
     * @return The count.
     * @throws IoError When the store is closed, or when SQLite fails.
     */
    int64_t totalObjectCount() const;
    /**
     * @brief Returns the number of stored objects in a layer, whatever their stage's
     * status.
     * @param layer The layer.
     * @return The count; 0 for an unknown layer.
     * @throws IoError When the store is closed, or when SQLite fails.
     */
    int64_t objectCountInLayer(LayerId layer) const;

private:
    friend class Store;
    explicit ObjectStore(detail::StoreImpl& store);
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace slideio::processor
