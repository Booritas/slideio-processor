// Requirements: UR-RES-9, UR-RES-15, UR-SEG-6
#pragma once

/**
 * @file column_schema.hpp
 * @brief ColumnSchema: which columns a tabular report holds, and under which names.
 */

#include <slideio/processor/core/export.hpp>

#include <cstdint>
#include <memory>

namespace slideio::processor {

/** @brief How an object identifier is written. */
enum class IdFormat : int32_t {
    TwoColumns,   ///< Columns layer_id and anchor_id (default).
    PackedString  ///< One column, object_id, in ObjectId::toString() form.
};

/** @brief Which labels an object's label columns carry. */
enum class LabelPolicy : int32_t {
    /// Every label, ';'-separated, with a parallel workspace-local flag list (default).
    EffectiveSet,
    ProducerOnly   ///< The label of the producing stage alone.
};

/** @brief Which unit a measurement column holds. */
enum class UnitPolicy : int32_t {
    Calibrated,   ///< The calibrated twin where the store has one, pixels otherwise (default).
    PixelsOnly,   ///< The pixel value only.
    BothColumns   ///< The pixel column and, where the store has one, the calibrated twin.
};

namespace detail {
class ColumnSchemaAccess;
}

/**
 * @brief Describes the columns of a tabular report.
 *
 * Fixed columns come first, in this order and under these names: layer_id and
 * anchor_id (or object_id), layer, stage, labels and labels_local (or label and
 * label_local), bbox_x, bbox_y, bbox_width, bbox_height, geometry_wkt, and
 * parent_layer_id and parent_anchor_id (or parent_id). Measurement columns follow.
 *
 * A measurement is named by its base name, the store's pixel-unit name. Under
 * UnitPolicy::Calibrated a base name whose store holds a twin ("n.um" or "n.um2")
 * is written as the twin column, so the header says which unit a column holds.
 * columnCount() and columnNameAt() report the fixed columns and the explicit
 * measurement names as UnitPolicy::PixelsOnly would; the full list, with twins and
 * allMeasurements() expanded, is resolved against a store when a writer starts.
 *
 * A value type: copies are independent. Column-name pointers stay valid until the
 * schema is next modified or destroyed.
 * @par Thread-safety
 * ThreadCompatible: concurrent const calls are safe; modification needs exclusive access.
 * @par Requirements
 * UR-RES-9, UR-RES-15, UR-SEG-6, decision 8 (SDS 07 section 7.2).
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS ColumnSchema {
public:
    /**
     * @brief Builds the default schema: TwoColumns, Calibrated, layer, stage, labels
     * (EffectiveSet) and bounding box on; geometry and parent off; no measurements.
     * @throws std::bad_alloc When memory is exhausted.
     */
    ColumnSchema();
    /**
     * @brief Copies a schema.
     * @param other The schema to copy.
     * @throws std::bad_alloc When memory is exhausted.
     */
    ColumnSchema(const ColumnSchema& other);
    /**
     * @brief Moves a schema. The source is left valid and holding the default schema.
     * @param other The schema to move from.
     * @throws std::bad_alloc When memory is exhausted.
     */
    ColumnSchema(ColumnSchema&& other);
    /**
     * @brief Copy-assigns a schema.
     * @param other The schema to copy.
     * @return This schema.
     * @throws std::bad_alloc When memory is exhausted; this schema is unchanged.
     */
    ColumnSchema& operator=(const ColumnSchema& other);
    /**
     * @brief Move-assigns a schema. The source is left valid and holding the default schema.
     * @param other The schema to move from.
     * @return This schema.
     * @throws std::bad_alloc When memory is exhausted.
     */
    ColumnSchema& operator=(ColumnSchema&& other);
    /** @brief Destroys the schema. */
    ~ColumnSchema();

    /**
     * @brief Returns the default schema.
     * @return A schema equal to ColumnSchema().
     * @throws std::bad_alloc When memory is exhausted.
     */
    static ColumnSchema standard();

    /**
     * @brief Sets how identifiers are written.
     * @param format The format.
     * @return This schema.
     * @throws std::bad_alloc When memory is exhausted.
     */
    ColumnSchema& idFormat(IdFormat format);
    /**
     * @brief Sets the unit policy.
     * @param policy The policy.
     * @return This schema.
     * @throws std::bad_alloc When memory is exhausted.
     */
    ColumnSchema& units(UnitPolicy policy);
    /**
     * @brief Includes or drops the layer column (default true).
     * @param include True to include.
     * @return This schema.
     * @throws std::bad_alloc When memory is exhausted.
     */
    ColumnSchema& includeLayer(bool include);
    /**
     * @brief Includes or drops the stage column (default true).
     * @param include True to include.
     * @return This schema.
     * @throws std::bad_alloc When memory is exhausted.
     */
    ColumnSchema& includeStage(bool include);
    /**
     * @brief Includes or drops the label columns (default true).
     * @param include True to include.
     * @return This schema.
     * @throws std::bad_alloc When memory is exhausted.
     */
    ColumnSchema& includeLabel(bool include);
    /**
     * @brief Sets which labels the label columns carry.
     * @param policy The policy.
     * @return This schema.
     * @throws std::bad_alloc When memory is exhausted.
     */
    ColumnSchema& labelPolicy(LabelPolicy policy);
    /**
     * @brief Includes or drops the bounding-box columns (default true).
     * @param include True to include.
     * @return This schema.
     * @throws std::bad_alloc When memory is exhausted.
     */
    ColumnSchema& includeBoundingBox(bool include);
    /**
     * @brief Includes or drops the geometry_wkt column (default false).
     * @param include True to include.
     * @return This schema.
     * @throws std::bad_alloc When memory is exhausted.
     */
    ColumnSchema& includeGeometryWkt(bool include);
    /**
     * @brief Includes or drops the parent columns (default false).
     * @param include True to include.
     * @return This schema.
     * @throws std::bad_alloc When memory is exhausted.
     */
    ColumnSchema& includeParent(bool include);
    /**
     * @brief Adds a measurement by its base name; columns follow call order.
     *
     * Give the base name ("area"); the unit is chosen by units(), so a name ending in
     * ".um" or ".um2" is refused. Cells come from the object row's own measurements
     * only: values asserted by later stages (AnnotationStore::assertedMeasurementAt())
     * are not exported yet, so an object whose value was only asserted gets an empty
     * cell. A name equal to a fixed column name (layer_id, anchor_id, object_id, layer,
     * stage, labels, labels_local, label, label_local, bbox_x, bbox_y, bbox_width,
     * bbox_height, geometry_wkt, parent_layer_id, parent_anchor_id, parent_id) is
     * refused, whether or not that column is included, so a header never holds two
     * columns of one name.
     * @param name The base name; not null, not empty, not ending in ".um" or ".um2",
     * not a fixed column name.
     * @return This schema.
     * @throws InvalidArgumentError When name is null, empty, a calibrated twin name or a
     * fixed column name, or was already added; the schema is unchanged. Messages carry
     * no names.
     * @throws std::bad_alloc When memory is exhausted.
     */
    ColumnSchema& measurement(const char* name);
    /**
     * @brief Adds, when a writer starts, every measurement the store holds, after the
     * explicit names, in ascending byte order and without duplicates.
     *
     * Store names ending in ".um" or ".um2" are skipped: they are calibrated twins, and
     * units() decides whether a twin column is written beside its base name. Store
     * names equal to a fixed column name (see measurement()) are skipped too. Calling it
     * twice is the same as once. Asserted measurement names share the store's name
     * list, but asserted values are not exported yet (cells come from the object row's
     * own measurements only), so a name that was only ever asserted gives a column
     * that is empty in every row.
     * @return This schema.
     * @throws std::bad_alloc When memory is exhausted.
     */
    ColumnSchema& allMeasurements();

    /**
     * @brief Returns the number of columns without a store: the fixed columns plus the
     * explicit measurement names, as UnitPolicy::PixelsOnly would.
     * @return The count.
     * @throws Nothing.
     */
    int32_t columnCount() const noexcept;
    /**
     * @brief Returns a column name, as columnCount() counts.
     * @param index Zero-based, below columnCount().
     * @return The name; valid until the schema is next modified or destroyed.
     * @throws InvalidArgumentError When index is negative or not below columnCount().
     */
    const char* columnNameAt(int32_t index) const;
    /**
     * @brief Writes the comma-separated header line, without a line terminator, for the
     * columns columnCount() counts.
     *
     * A name containing a comma, a quote, CR, LF or '#' is quoted as RFC 4180 does
     * ('#' because pandas.read_csv(comment='#') cuts a line at an unquoted '#').
     * @param buffer Receives the null-terminated UTF-8 text; null queries the length.
     * @param capacity The buffer size in bytes including the terminator. When too small,
     * the text is truncated and still terminated; when 0, nothing is written.
     * @return The required length excluding the terminator.
     * @throws std::bad_alloc When memory is exhausted.
     */
    int32_t toHeaderLine(char* buffer, int32_t capacity) const;

    /** @brief Implementation detail; incomplete outside the library. */
    class Impl;

private:
    friend class detail::ColumnSchemaAccess;
    std::unique_ptr<Impl> impl_;
};

} // namespace slideio::processor
