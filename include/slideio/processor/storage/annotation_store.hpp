// Requirements: UR-RES-4, UR-RES-17, UR-RES-18, UR-STG-4, UR-STG-6, UR-QA-3
#pragma once

/**
 * @file annotation_store.hpp
 * @brief AnnotationWriter and AnnotationStore: labels and measurements asserted about
 * existing objects.
 */

#include <slideio/processor/core/export.hpp>
#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/core/ids.hpp>
#include <slideio/processor/core/measurement.hpp>

#include <cstdint>
#include <memory>

namespace slideio::processor {

namespace detail {
class StoreImpl;
class StoreGate;
} // namespace detail

class AnnotationWriter;
class LayerRegistry;
class ObjectStore;
/** @brief Shared ownership of an AnnotationWriter. */
using AnnotationWriterPtr = std::shared_ptr<AnnotationWriter>;

/**
 * @brief Asserts labels and measurements about existing objects, attributing every
 * one to the stage it was created for.
 *
 * An annotation is a record of the form (object, asserting stage, claim); the object
 * row is never modified, so the label it carries stays the producing stage's own
 * assertion. An identical label, or an identical (name, value, unit) measurement, from
 * one stage on one object is one row.
 *
 * One assert inserts nothing by itself. Accepted annotations accumulate until
 * batchSizeAnnotations() are pending; then one transaction inserts their rows and one
 * ObjectsAnnotated event, together or not at all (UR-OBS-2). The event carries this
 * writer's stage, the number of distinct objects that gained at least one new row,
 * their union bounding box, their layer when they all share one (an invalid layer
 * otherwise) and an invalid label. A batch that inserts no new row appends no event.
 * flush() commits what is pending.
 *
 * Arguments are checked at the call. Object existence is checked at commit, because
 * the object may still be pending in an unflushed ObjectWriter: flush that writer
 * first. Every object is looked up whatever its stage's status. A missing object, or a
 * measurement conflicting with a committed one from the same stage, rolls the whole
 * batch back; its pending annotations are dropped and the call that triggered the
 * commit throws InvalidArgumentError, "AnnotationWriter::flush: object <layer>/<anchor>
 * does not exist" for a missing object.
 *
 * Store::close() (and so the Store's destruction) commits what a live writer holds
 * pending. A writer used after that, by an assert or by a flush() with something
 * pending, throws IoError containing "store is closed".
 * @par Thread-safety
 * assertLabel(), assertMeasurement(), assertedCount(), batchSizeAnnotations(),
 * setBatchSizeAnnotations() and stage() are ThreadSafe. flush() requires exclusive
 * access: no assert may run concurrently with it on the same writer. Any number of
 * writers may be used concurrently; their commits are serialised.
 * @par Requirements
 * UR-RES-17, UR-RES-18, UR-STG-6, UR-QA-3, UR-OBS-2
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS AnnotationWriter : public AnnotationSink {
public:
    /**
     * @brief Flushes what is pending; a failure is logged at LogLevel::Error and
     * swallowed.
     */
    ~AnnotationWriter() override;
    AnnotationWriter(const AnnotationWriter&) = delete;             ///< Not copyable.
    AnnotationWriter& operator=(const AnnotationWriter&) = delete;  ///< Not copyable.

    /**
     * @brief Attaches `label` to an object that already exists, attributed to stage(),
     * committing the pending batch once it reaches batchSizeAnnotations().
     *
     * The object row is not touched: the label it carries, if any, remains the
     * producing stage's own assertion. Idempotent: the same stage asserting the same
     * label on the same object twice is one row; both calls count as accepted.
     * @param object The object.
     * @param label A label the store holds.
     * @throws InvalidArgumentError When either id is invalid, when the store holds no
     * such label, or when the commit this call triggered failed (see the class
     * description).
     * @throws IoError When the store is closed or Store::close() has begun (the label
     * is not accepted), or when SQLite fails during a commit; the batch is then
     * dropped.
     * @throws std::bad_alloc When memory is exhausted.
     */
    void assertLabel(ObjectId object, LabelId label) override;

    /**
     * @brief Attaches a named measurement to an object that already exists, attributed
     * to stage(), committing the pending batch once it reaches batchSizeAnnotations().
     *
     * Idempotent for an identical value and unit (compared with ==). The same name
     * asserted again on the same object with a different value or unit throws rather
     * than letting arrival order decide (UR-QA-3): at this call when the first is
     * pending in this writer, otherwise at commit.
     * @param object The object.
     * @param name UTF-8 measurement name; not null and not empty.
     * @param value A finite value.
     * @param unit A MeasurementUnit enumerator.
     * @throws InvalidArgumentError When the id is invalid, the name null or empty, the
     * value not finite or the unit out of range; when the same name is pending on the
     * object in this writer with a different value or unit (the message names the
     * object and the name); or when the commit this call triggered failed.
     * @throws IoError As for assertLabel().
     * @throws std::bad_alloc When memory is exhausted.
     */
    void assertMeasurement(ObjectId object, const char* name, double value, MeasurementUnit unit) override;

    /**
     * @brief Commits every pending annotation. A no-op when nothing is pending.
     * @throws InvalidArgumentError When an object does not exist, or a measurement
     * conflicts with one this stage committed earlier (the message names the object
     * and the name); the batch is rolled back and dropped.
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
     * @brief Returns the number of pending annotations that triggers a commit.
     * @return The batch size; Configuration::objectBatchSize() until changed.
     * @throws Nothing.
     */
    int64_t batchSizeAnnotations() const noexcept;
    /**
     * @brief Sets the number of pending annotations that triggers a commit.
     *
     * Takes effect at the next assert; annotations already pending are not committed
     * by this call.
     * @param size The batch size, at least 1.
     * @throws InvalidArgumentError When size is less than 1.
     */
    void setBatchSizeAnnotations(int64_t size);

    /**
     * @brief Returns the stage every annotation asserted through this writer is
     * attributed to.
     * @return The stage.
     * @throws Nothing.
     */
    StageId stage() const noexcept;

private:
    friend class AnnotationStore;
    AnnotationWriter(std::shared_ptr<detail::StoreGate> gate, StageId stage, int64_t batchSize);
    class Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * @brief The annotations of a store: writers, effective label sets, asserted
 * measurements and per-stage discard.
 *
 * Reads use the default view, StageVisibility::CompletedOnly. An object that does not
 * exist, or whose producing stage is not Complete, has no labels and no asserted
 * measurements. An asserted label or measurement is visible when its asserting stage
 * is Complete, stale or not. Each read is its own snapshot, so an index obtained from
 * one call may be out of range at the next when a writer committed in between.
 *
 * Every member throws IoError on a closed store, and every mutating member also on a
 * ReadOnly store.
 * @par Thread-safety
 * ThreadSafe: every member may be called from any thread with no external locking.
 * @par Requirements
 * UR-RES-4, UR-RES-17, UR-RES-18, UR-STG-4, UR-STG-6, UR-QA-3
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS AnnotationStore {
public:
    /** @brief Releases the annotation store. */
    ~AnnotationStore();
    AnnotationStore(const AnnotationStore&) = delete;             ///< Not copyable.
    AnnotationStore& operator=(const AnnotationStore&) = delete;  ///< Not copyable.

    /**
     * @brief Returns a writer that attributes every annotation it asserts to `stage`.
     *
     * Any number may be held concurrently, from any threads; writes are serialised
     * internally into batched transactions.
     * @param stage A declared stage.
     * @return The writer, with batch size Configuration::objectBatchSize().
     * @throws InvalidArgumentError When the stage is unknown.
     * @throws IoError When the store is closed or read-only, or when SQLite fails.
     * @throws std::bad_alloc When memory is exhausted.
     */
    AnnotationWriterPtr writerForStage(StageId stage);

    /**
     * @brief Returns the size of the object's effective label set.
     *
     * The effective set is the producing stage's label from the object row first,
     * when it has one, then every visible asserted label other than that one, in
     * ascending LabelId order, each once however many stages asserted it. The order is
     * fixed, not arrival order, so exports are identical across runs (UR-QA-3).
     * @param object The object.
     * @return The number of labels; 0 for an invalid, unknown or invisible object.
     * @throws IoError When the store is closed, or when SQLite fails.
     */
    int32_t labelCount(ObjectId object) const;
    /**
     * @brief Returns a label of the object's effective label set.
     * @param object The object.
     * @param index Position in the set, from 0 to labelCount() - 1.
     * @return The label.
     * @throws InvalidArgumentError When index is out of range.
     * @throws IoError When the store is closed, or when SQLite fails.
     */
    LabelId labelAt(ObjectId object, int32_t index) const;
    /**
     * @brief Returns who asserted the label at `index` of the effective label set.
     * @param object The object.
     * @param index Position in the set, from 0 to labelCount() - 1.
     * @return The producing stage for the row label; for an asserted label, the
     * smallest id among the visible stages that asserted it.
     * @throws InvalidArgumentError When index is out of range.
     * @throws IoError When the store is closed, or when SQLite fails.
     */
    StageId labelAssertedBy(ObjectId object, int32_t index) const;

    /**
     * @brief Returns the number of visible measurements other stages asserted on the
     * object.
     *
     * The producer's own measurements stay on the record; these are the additions,
     * ordered by name (bytewise UTF-8), then by asserting stage id.
     * @param object The object.
     * @return The number of asserted measurements; 0 for an invalid, unknown or
     * invisible object.
     * @throws IoError When the store is closed, or when SQLite fails.
     */
    int32_t assertedMeasurementCount(ObjectId object) const;
    /**
     * @brief Returns an asserted measurement of the object.
     * @param object The object.
     * @param index Position, from 0 to assertedMeasurementCount() - 1.
     * @return The measurement. Its name() stays valid until the Store is destroyed.
     * @throws InvalidArgumentError When index is out of range.
     * @throws IoError When the store is closed, or when SQLite fails.
     * @throws std::bad_alloc When memory is exhausted.
     */
    Measurement assertedMeasurementAt(ObjectId object, int32_t index) const;
    /**
     * @brief Returns the stage that asserted the measurement at `index`.
     * @param object The object.
     * @param index Position, from 0 to assertedMeasurementCount() - 1.
     * @return The asserting stage.
     * @throws InvalidArgumentError When index is out of range.
     * @throws IoError When the store is closed, or when SQLite fails.
     */
    StageId measurementAssertedBy(ObjectId object, int32_t index) const;

    /**
     * @brief Reads the measurement of one name that one stage asserted on the object.
     *
     * Resolves the one thing the effective sets leave ambiguous: two stages asserting
     * the same name on one object.
     * @param object The object.
     * @param name UTF-8 measurement name; null is never found.
     * @param stage The asserting stage.
     * @param out Receives the measurement when found, its name() valid until the Store
     * is destroyed; unchanged otherwise.
     * @return True when found; false when the stage asserted no measurement of that
     * name, or when the object or the stage's assertion is not visible.
     * @throws IoError When the store is closed, or when SQLite fails.
     * @throws std::bad_alloc When memory is exhausted.
     */
    bool assertedMeasurement(ObjectId object, const char* name, StageId stage, Measurement& out) const;

    /**
     * @brief Removes only the annotations a stage asserted, and appends a
     * StageDiscarded event with count 0, in one transaction.
     *
     * Objects are untouched, including the objects of other stages that this stage
     * had annotated (UR-STG-6).
     * @param stage A declared stage.
     * @throws InvalidArgumentError When the stage is unknown.
     * @throws IoError When the store is closed or read-only, or when SQLite fails.
     */
    void discardAnnotationsOfStage(StageId stage);

    /**
     * @brief Returns the number of stored label assertions, whatever their visibility.
     * @return The count.
     * @throws IoError When the store is closed, or when SQLite fails.
     */
    int64_t labelAnnotationCount() const;
    /**
     * @brief Returns the number of stored measurement assertions, whatever their
     * visibility.
     * @return The count.
     * @throws IoError When the store is closed, or when SQLite fails.
     */
    int64_t measurementAnnotationCount() const;

    /**
     * @brief Returns the number of distinct (asserting stage, measurement name) pairs
     * among the stored measurement assertions, whatever their visibility.
     *
     * A name and its calibrated twin (for example "area" and "area.um2") count
     * separately. assertedMeasurementColumnCount() and assertedMeasurementColumnAt()
     * each read the store afresh, so a write between two calls can change the count
     * and shift indices; read them while nothing writes, or tolerate the shift.
     * @return The count, 0 for a store without measurement assertions.
     * @throws IoError When the store is closed, or when SQLite fails.
     * @par Thread-safety
     * Safe to call from any thread.
     * @par Requirements
     * UR-RES-9, UR-RES-18.
     */
    int32_t assertedMeasurementColumnCount() const;
    /**
     * @brief Returns the pair at an index, ordered by stage id, then by name in
     * ascending byte order (SQLite's BINARY collation).
     * @param index Zero-based, below assertedMeasurementColumnCount().
     * @param stage Receives the asserting stage.
     * @param name Receives the measurement name, never null. The pointer stays valid for
     * the life of the store: names are interned like ObjectStore::measurementNameAt()'s.
     * @throws InvalidArgumentError When the index is negative or not below the count;
     * `stage` and `name` are unchanged.
     * @throws IoError When the store is closed, or when SQLite fails.
     * @throws std::bad_alloc When memory is exhausted.
     * @par Thread-safety
     * Safe to call from any thread.
     * @par Requirements
     * UR-RES-9, UR-RES-18.
     */
    void assertedMeasurementColumnAt(int32_t index, StageId& stage, const char*& name) const;

    /**
     * @brief Makes a raster source backed by the objects of one layer, whose Label
     * content is the effective label.
     *
     * Takes makeLayerRasterSource()'s parameters minus the ObjectSource, because the
     * store is the source; it takes every content, so a caller cannot reach core's
     * row-label-only Label by mistake. Membership and ObjectAnchor are exactly
     * makeLayerRasterSource(Store::objects(), layer, content).
     *
     * For Label, each fillTile() reads, in one read transaction, the objects of `layer`
     * visible under StageVisibility::CompletedOnly that intersect the tile's rasterised
     * grid, grown by one level-0 pixel on each side so that rounding never drops a
     * sliver, as makeLayerRasterSource() queries. Each object's value is the first
     * member of its effective label set (see labelCount()): its row label when it has
     * one, otherwise the smallest visible asserted label, otherwise 0. The values are
     * rasterised into plane 0 at the tile's own level, 0 where no object covers a
     * pixel; where objects overlap, the smallest ObjectId wins. planeType() is Int32
     * and planeCount() is 1. A dropped layer yields background.
     *
     * The Label source's fillTile() throws InvalidArgumentError when the view lacks
     * plane 0 of type Int32, or its scale, origin or level-0 query extent is unusable;
     * IoError once the Store is closed ("store is closed") or when SQLite fails;
     * InternalError when a stored geometry is corrupt; std::bad_alloc.
     * @param layer The layer to read: a declared layer other than the pixel layer.
     * @param content What to write.
     * @return The raster source. The store is borrowed (C-8), and the two kinds of
     * source differ in how. A Membership or ObjectAnchor source holds a reference to
     * Store::objects() and must not be used after the Store is destroyed. A Label
     * source holds the Store's shared gate instead, so after Store::close() or the
     * Store's destruction its fillTile() throws IoError ("store is closed").
     * @throws InvalidArgumentError When `layer` is invalid, unknown or the pixel layer,
     * or `content` is not a LayerRasterContent.
     * @throws IoError When the store is closed, or when SQLite fails.
     * @throws std::bad_alloc When memory is exhausted.
     * @par Thread-safety
     * ThreadSafe; the returned source is ThreadSafe.
     * @par Requirements
     * UR-RES-4, UR-STG-5, UR-EXT-4, UR-QA-3
     */
    TileRasterSourcePtr layerRasterSource(LayerId layer, LayerRasterContent content) const;

private:
    friend class Store;
    AnnotationStore(detail::StoreImpl& store, const LayerRegistry& layers, ObjectStore& objects);
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace slideio::processor
