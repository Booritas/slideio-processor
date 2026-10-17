// Requirements: UR-RES-4, UR-RES-13, UR-STG-4, UR-STG-6
#pragma once

#include <slideio/processor/core/export.hpp>
#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/core/ids.hpp>
#include <slideio/processor/core/measurement.hpp>

namespace slideio::processor {

/**
 * @brief One segmented object: a layer, a polygon, an optional label and
 * measurements.
 *
 * The identifier is derived from the layer and the geometry anchor, so a
 * re-run producing the same geometry produces the same id (UR-STG-6). A
 * moved-from record is valid and in its default state.
 * @par Thread-safety
 * ThreadCompatible: const members may run concurrently; any mutation needs
 * exclusive access.
 * @par Requirements
 * UR-RES-4, UR-RES-13, UR-STG-4, UR-STG-6
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS ObjectRecord {
public:
    /**
     * @brief Builds a default record: empty geometry, invalid layer, stage and label.
     * @throws std::bad_alloc When memory is exhausted.
     */
    ObjectRecord();
    /**
     * @brief Copies a record.
     * @param other The record to copy.
     * @throws std::bad_alloc When memory is exhausted.
     */
    ObjectRecord(const ObjectRecord& other);
    /**
     * @brief Moves a record. The source is left in its default state.
     * @param other The record to move from.
     * @throws Nothing.
     */
    ObjectRecord(ObjectRecord&& other) noexcept;
    /**
     * @brief Copy-assigns a record.
     * @param other The record to copy.
     * @return This object.
     * @throws std::bad_alloc When memory is exhausted; this object is unchanged.
     */
    ObjectRecord& operator=(const ObjectRecord& other);
    /**
     * @brief Move-assigns a record. The source is left in its default state.
     * @param other The record to move from.
     * @return This object.
     * @throws Nothing.
     */
    ObjectRecord& operator=(ObjectRecord&& other) noexcept;
    /** @brief Destroys the record. */
    ~ObjectRecord();

    /**
     * @brief Returns the derived identifier.
     * @return ObjectId::fromAnchor(layer(), geometry().anchor()); invalid when
     * the geometry is empty or the layer is invalid.
     * @throws Nothing.
     */
    ObjectId id() const noexcept;
    /**
     * @brief Returns the layer.
     * @return The layer; invalid until setLayer().
     * @throws Nothing.
     */
    LayerId layer() const noexcept;
    /**
     * @brief Returns the stage that produced this object.
     *
     * Read-only: stamped on emission, never set by the producer. In this
     * release the stamping route does not exist yet (storage module), so the
     * id is always invalid.
     * @return An invalid id.
     * @throws Nothing.
     */
    StageId stage() const noexcept;

    /**
     * @brief Returns the geometry.
     * @return The polygon, in level-0 coordinates.
     * @throws Nothing.
     */
    const Polygon& geometry() const noexcept;
    /**
     * @brief Returns the bounding box of the geometry.
     * @return geometry().boundingBox().
     * @throws Nothing.
     */
    Rect64 boundingBox() const noexcept;

    /**
     * @brief Tells whether the record has a label.
     * @return True iff label() is valid.
     * @throws Nothing.
     */
    bool hasLabel() const noexcept;
    /**
     * @brief Returns the producing stage's own label.
     * @return The label; invalid if !hasLabel().
     * @throws Nothing.
     */
    LabelId label() const noexcept;

    /**
     * @brief Returns the measurements.
     * @return The measurement set.
     * @throws Nothing.
     */
    const MeasurementSet& measurements() const noexcept;
    /**
     * @brief Returns the measurements for modification.
     * @return The measurement set.
     * @throws Nothing.
     */
    MeasurementSet& measurements() noexcept;

    /**
     * @brief Replaces the geometry. This changes id().
     * @param geometry The polygon to copy.
     * @throws std::bad_alloc When memory is exhausted; the record is unchanged.
     */
    void setGeometry(const Polygon& geometry);
    /**
     * @brief Sets the layer. This changes id().
     * @param layer The layer.
     * @throws Nothing.
     */
    void setLayer(LayerId layer);
    /**
     * @brief Sets the producing stage's own classification of this object.
     *
     * Labels asserted later by other stages live in the annotation store, not
     * here.
     * @param label The label.
     * @throws Nothing.
     */
    void setLabel(LabelId label);
    /**
     * @brief Removes the label, so hasLabel() becomes false.
     * @throws Nothing.
     */
    void clearLabel() noexcept;
    /**
     * @brief Returns every field to its default.
     * @throws Nothing.
     */
    void reset() noexcept;

private:
    Polygon geometry_;
    MeasurementSet measurements_;
    LayerId layer_;
    StageId stage_;
    LabelId label_;
};

} // namespace slideio::processor
