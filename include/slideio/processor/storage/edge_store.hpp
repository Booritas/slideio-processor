// Requirements: UR-RES-5, UR-RES-14, UR-RES-16, UR-STG-4, UR-STG-6, UR-OBS-2
#pragma once

/**
 * @file edge_store.hpp
 * @brief EdgeWriter and EdgeStore: parent and derived-from edges between objects.
 */

#include <slideio/processor/core/export.hpp>
#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/core/ids.hpp>

#include <cstdint>
#include <memory>

namespace slideio::processor {

class LayerRegistry;

namespace detail {
class StoreImpl;
class StoreGate;
} // namespace detail

class EdgeWriter;
/** @brief Shared ownership of an EdgeWriter. */
using EdgeWriterPtr = std::shared_ptr<EdgeWriter>;

/**
 * @brief Asserts edges between objects, attributing every one to the stage it was
 * created for.
 *
 * Edges are records of the form (child, parent, asserting stage) and (derived,
 * primitive, asserting stage); neither endpoint object is modified. A child has at
 * most one parent store-wide. Containment is asserted, never derived: only the layer
 * order is checked, never geometry.
 *
 * One assert inserts nothing by itself. Accepted edges accumulate until
 * batchSizeEdges() are pending; then one transaction inserts their rows and one
 * EdgesAsserted event, together or not at all (UR-OBS-2). The event carries this
 * writer's stage, the number of new rows, the union bounding box of the child and
 * derived objects of those rows, and their layer when they all share one (an invalid
 * layer otherwise). A batch that inserts no new row appends no event. flush() commits
 * what is pending.
 *
 * Layer checks run at the call. Endpoint existence is checked at commit, because an
 * endpoint may still be pending in an unflushed ObjectWriter: flush that writer first.
 * Every endpoint is looked up whatever its stage's status. A missing endpoint, or a
 * parent edge conflicting with a committed one, rolls the whole batch back; its
 * pending edges are dropped and the call that triggered the commit throws
 * InvalidArgumentError, "EdgeWriter::flush: <role> <layer>/<anchor> does not exist"
 * for a missing endpoint, where role is child, parent, derived or primitive.
 *
 * Store::close() (and so the Store's destruction) commits what a live writer holds
 * pending. A writer used after that, by an assert or by a flush() with something
 * pending, throws IoError containing "store is closed".
 * @par Thread-safety
 * assertParent(), assertDerivedFrom(), assertedCount(), batchSizeEdges(),
 * setBatchSizeEdges() and stage() are ThreadSafe. flush() requires exclusive access:
 * no assert may run concurrently with it on the same writer. Any number of writers
 * may be used concurrently; their commits are serialised.
 * @par Requirements
 * UR-RES-5, UR-RES-14, UR-RES-16, UR-STG-6, UR-OBS-2
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS EdgeWriter : public EdgeSink {
public:
    /**
     * @brief Flushes what is pending; a failure is logged at LogLevel::Error and
     * swallowed.
     */
    ~EdgeWriter() override;
    EdgeWriter(const EdgeWriter&) = delete;             ///< Not copyable.
    EdgeWriter& operator=(const EdgeWriter&) = delete;  ///< Not copyable.

    /**
     * @brief Asserts that `child` is contained by `parent`, committing the pending
     * batch once it reaches batchSizeEdges().
     *
     * Both must lie in semantic layers, with the parent's layer a proper ancestor of
     * the child's at any distance: a nucleus may attach directly to a tissue region.
     * Legal in either creation order. Re-asserting an edge already pending in this
     * writer adds no row, and re-asserting a committed edge from the same stage is a
     * no-op at commit; both still count as accepted calls.
     * @param child The contained object.
     * @param parent The containing object.
     * @throws InvalidArgumentError When either id is invalid; when either layer is
     * unknown or primitive, or the parent's layer is not below the child's in its
     * branch (the message names both layers); when the child already has another
     * parent pending in this writer (the message names the child, that parent and the
     * stage); or when the commit this call triggered failed (see the class
     * description).
     * @throws IoError When the store is closed or Store::close() has begun (the edge is
     * not accepted), or when SQLite fails during a commit; the batch is then dropped.
     * @throws std::bad_alloc When memory is exhausted.
     */
    void assertParent(ObjectId child, ObjectId parent) override;
    /**
     * @brief Asserts that `derived` was built from `primitive`, committing the pending
     * batch once it reaches batchSizeEdges().
     *
     * Many-to-many, optional, valid when empty. One primitive may contribute to
     * several objects and one object may be built from many primitives. The same pair
     * asserted by two stages is two rows and one read result.
     * @param derived An object in a semantic layer.
     * @param primitive An object in a primitive layer that is not dropped.
     * @throws InvalidArgumentError When either id is invalid; when derived is not in a
     * semantic layer or primitive is not in a primitive layer (the message names both
     * layers); when the primitive's layer is dropped; or when the commit this call
     * triggered failed.
     * @throws IoError As for assertParent().
     * @throws std::bad_alloc When memory is exhausted.
     */
    void assertDerivedFrom(ObjectId derived, ObjectId primitive) override;

    /**
     * @brief Commits every pending edge. A no-op when nothing is pending.
     * @throws InvalidArgumentError When an endpoint does not exist, or a parent edge
     * conflicts with a committed one (the message names the child, the existing parent
     * and its stage); the batch is rolled back and dropped.
     * @throws IoError When the store is closed, or when SQLite fails; the batch is
     * dropped.
     * @throws std::bad_alloc When memory is exhausted.
     */
    void flush() override;
    /**
     * @brief Returns the number of assert calls accepted and not dropped.
     * @return Committed plus pending calls. A batch dropped at commit no longer counts.
     * @throws Nothing in practice.
     */
    int64_t assertedCount() const override;

    /**
     * @brief Returns the number of pending edges that triggers a commit.
     * @return The batch size; Configuration::objectBatchSize() until changed.
     * @throws Nothing.
     */
    int64_t batchSizeEdges() const noexcept;
    /**
     * @brief Sets the number of pending edges that triggers a commit.
     *
     * Takes effect at the next assert; edges already pending are not committed by this
     * call.
     * @param size The batch size, at least 1.
     * @throws InvalidArgumentError When size is less than 1.
     */
    void setBatchSizeEdges(int64_t size);

    /**
     * @brief Returns the stage every edge asserted through this writer is attributed
     * to.
     * @return The stage.
     * @throws Nothing.
     */
    StageId stage() const noexcept;

private:
    friend class EdgeStore;
    EdgeWriter(std::shared_ptr<detail::StoreGate> gate, const LayerRegistry& layers, StageId stage, int64_t batchSize);
    class Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * @brief The edges of a store: writers, reads and per-stage discard.
 *
 * Reads use the default view, StageVisibility::CompletedOnly: an edge is visible when
 * its asserting stage is Complete, stale or not. parentOf() and isAttached() look at
 * the edge alone, which is authoritative; the parent object's own visibility is not
 * consulted. The cursors return records visible under CompletedOnly and joined
 * through a visible edge, once each, in ascending ObjectId order; each record's
 * stage() is the stage that produced it. The edge counts count every stored row,
 * whatever its visibility.
 *
 * Every member throws IoError on a closed store, and every mutating member also on a
 * ReadOnly store.
 * @par Thread-safety
 * ThreadSafe: every member may be called from any thread with no external locking.
 * A returned CursorPtr is ThreadConfined.
 * @par Requirements
 * UR-RES-5, UR-RES-14, UR-RES-16, UR-STG-4, UR-STG-6
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS EdgeStore {
public:
    /** @brief Releases the edge store. */
    ~EdgeStore();
    EdgeStore(const EdgeStore&) = delete;             ///< Not copyable.
    EdgeStore& operator=(const EdgeStore&) = delete;  ///< Not copyable.

    /**
     * @brief Returns a writer that attributes every edge it asserts to `stage`.
     *
     * Any number may be held concurrently, from any threads; writes are serialised
     * internally into batched transactions.
     * @param stage A declared stage.
     * @return The writer, with batch size Configuration::objectBatchSize().
     * @throws InvalidArgumentError When the stage is unknown.
     * @throws IoError When the store is closed or read-only, or when SQLite fails.
     * @throws std::bad_alloc When memory is exhausted.
     */
    EdgeWriterPtr writerForStage(StageId stage);

    /**
     * @brief Reads the parent of an object through a visible edge.
     * @param child The object.
     * @param out Receives the parent when found; unchanged otherwise.
     * @return True when a visible parent edge exists; false for an unknown or invalid
     * child.
     * @throws IoError When the store is closed, or when SQLite fails.
     */
    bool parentOf(ObjectId child, ObjectId& out) const;
    /**
     * @brief Opens a cursor over the visible children of an object.
     * @param parent The object.
     * @return The cursor, positioned before the first record; empty for an unknown or
     * invalid parent. It holds one read transaction for its lifetime and throws
     * IoError once the store is closed.
     * @throws IoError When the store is closed, or when SQLite fails.
     * @throws std::bad_alloc When memory is exhausted.
     */
    CursorPtr childrenOf(ObjectId parent) const;
    /**
     * @brief Tells whether an object has a visible parent edge.
     * @param child The object.
     * @return True when attached; false for an unknown or invalid child.
     * @throws IoError When the store is closed, or when SQLite fails.
     */
    bool isAttached(ObjectId child) const;
    /**
     * @brief Opens a cursor over the visible primitives an object was derived from.
     *
     * Primitives of a dropped layer no longer exist and are not returned.
     * @param derived The derived object.
     * @return The cursor, as for childrenOf().
     * @throws IoError When the store is closed, or when SQLite fails.
     * @throws std::bad_alloc When memory is exhausted.
     */
    CursorPtr originsOf(ObjectId derived) const;
    /**
     * @brief Opens a cursor over the visible objects derived from a primitive.
     * @param primitive The primitive.
     * @return The cursor, as for childrenOf().
     * @throws IoError When the store is closed, or when SQLite fails.
     * @throws std::bad_alloc When memory is exhausted.
     */
    CursorPtr derivedFrom(ObjectId primitive) const;

    /**
     * @brief Removes only the edges a stage asserted, and appends a StageDiscarded
     * event with count 0, in one transaction.
     *
     * Objects are untouched, including objects of other stages that this stage had
     * attached (UR-STG-6).
     * @param stage A declared stage.
     * @throws InvalidArgumentError When the stage is unknown.
     * @throws IoError When the store is closed or read-only, or when SQLite fails.
     */
    void discardEdgesOfStage(StageId stage);

    /**
     * @brief Returns the number of stored parent edges, whatever their visibility.
     * @return The count.
     * @throws IoError When the store is closed, or when SQLite fails.
     */
    int64_t parentEdgeCount() const;
    /**
     * @brief Returns the number of stored derived-from rows, whatever their
     * visibility, including rows whose primitive no longer exists.
     * @return The count.
     * @throws IoError When the store is closed, or when SQLite fails.
     */
    int64_t derivedFromEdgeCount() const;

private:
    friend class Store;
    EdgeStore(detail::StoreImpl& store, const LayerRegistry& layers);
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace slideio::processor
