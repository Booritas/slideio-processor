// Requirements: UR-RES-9, UR-RES-11, UR-RES-15, UR-RES-18, UR-SEG-6, UR-QA-3
#pragma once

/**
 * @file csv_writer.hpp
 * @brief CsvWriter: exports a query's objects to a CSV file, one row per object.
 */

#include <slideio/processor/core/control.hpp>
#include <slideio/processor/core/export.hpp>
#include <slideio/processor/core/object_query.hpp>
#include <slideio/processor/reporting/column_schema.hpp>
#include <slideio/processor/reporting/export_result.hpp>

#include <memory>

namespace slideio::processor {

class Store;

/**
 * @brief Writes the objects a query selects to a CSV file, one row per object.
 *
 * The file is UTF-8 without a byte-order mark, lines end in LF, and fields follow
 * RFC 4180: a field holding the delimiter, a double quote, CR, LF or '#' is quoted,
 * with quotes doubled. '#' is quoted because pandas.read_csv(comment='#') cuts a line
 * at an unquoted '#'. Rows come in the query cursor's order.
 *
 * Unless provenanceHeader(false) is set, the file starts with '#' comment lines that
 * pandas.read_csv(comment='#') skips:
 * @code
 * # slideio-processor export
 * # slide_id: <slideId>
 * # scene_index: <n>
 * # library_version: <v>
 * # created_at_unix_ms: <n>
 * # stage: <name> <configuration>
 * @endcode
 * with one stage line per stage of the store, in StageStateReader order, and no
 * trailing space for an empty configuration. CR and LF in any value are written as
 * a backslash followed by r or n, so each stays on one line. Nothing else is
 * written, in particular no export time, so two exports of an unchanged store with the
 * same query and schema are byte-identical (UR-QA-3).
 *
 * slide_id is the slide path as the store holds it. A path may contain identifiers,
 * such as an accession number, a medical record number, a patient's name or a user
 * name, that count as PHI or personal data under HIPAA and GDPR; consumers who share
 * an export must treat it accordingly. provenanceHeader(false) drops every provenance
 * line (and with it the provenance UR-RES-11 asks for).
 *
 * The header line follows, then the rows. Cell contents, per ColumnSchema column:
 * - layer_id, anchor_id: the decimal LayerId value and the decimal packed anchor
 *   (ObjectId::anchorPacked()); object_id: ObjectId::toString().
 * - layer: the layer name. stage: the stage name, empty for an object with no stage.
 * - labels, labels_local: the effective label set (AnnotationStore order), names joined
 *   with ';', and the parallel list of 1 (not a standard label) or 0 (standard). Inside
 *   a name, a backslash is written as two backslashes and ';' as a backslash and ';'.
 *   label, label_local: the same for the producing stage's label alone. Empty for an
 *   object without labels.
 * - bbox_x, bbox_y, bbox_width, bbox_height: ObjectRecord::boundingBox(), level-0.
 * - geometry_wkt: POLYGON for one outer ring, MULTIPOLYGON for several, in level-0
 *   integer coordinates with closed rings; a hole joins the first outer ring, in ring
 *   order, that contains its first vertex.
 * - parent_layer_id, parent_anchor_id or parent_id: the parent through a visible
 *   parent edge; empty for an object without one.
 * - measurements: the value as printf's "%.17g" in the C locale, so it round-trips;
 *   NaN as "nan", +Inf as "inf" and -Inf as "-inf", which pandas parses; empty when
 *   the object lacks it. A stage-qualified column ("<stage>/<name>",
 *   ColumnSchema::assertedMeasurement()) holds the value that stage asserted on the
 *   object, empty when it asserted none or its assertion is not visible.
 *
 * The export goes to "<path>.partial", renamed onto path, replacing any file there,
 * only when it completes. A cancelled or failed export removes the partial file and
 * leaves any file already at path untouched.
 * @par Thread-safety
 * ThreadConfined: one thread at a time; the store is used as its own contract allows.
 * Two writers on the same path share "<path>.partial" and must not run at once.
 * @par Requirements
 * UR-RES-9, UR-RES-11, UR-RES-15, UR-RES-18, UR-SEG-6, UR-QA-3 (SDS 07 section 7.3).
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS CsvWriter {
public:
    /**
     * @brief Creates a writer for a file; nothing is opened until write().
     * @param path UTF-8 path of the CSV file; not null, not empty.
     * @throws InvalidArgumentError When path is null or empty.
     * @throws std::bad_alloc When memory is exhausted.
     */
    explicit CsvWriter(const char* path);
    /** @brief Destroys the writer. */
    ~CsvWriter();
    CsvWriter(const CsvWriter&) = delete;             ///< Not copyable.
    CsvWriter& operator=(const CsvWriter&) = delete;  ///< Not copyable.

    /**
     * @brief Sets the columns (default ColumnSchema::standard()).
     * @param schema The schema; copied.
     * @return This writer.
     * @throws std::bad_alloc When memory is exhausted; the writer is unchanged.
     */
    CsvWriter& schema(const ColumnSchema& schema);
    /**
     * @brief Sets the field delimiter (default ',').
     * @param delimiter The delimiter; not '"', CR, LF or '#'.
     * @return This writer.
     * @throws InvalidArgumentError When the delimiter is refused; the writer is unchanged.
     */
    CsvWriter& delimiter(char delimiter);
    /**
     * @brief Writes or omits the '#' provenance lines (default true).
     * @param include True to write them.
     * @return This writer.
     * @throws Nothing in practice.
     */
    CsvWriter& provenanceHeader(bool include);

    /**
     * @brief Exports the objects the query selects, with no observer and no cancellation.
     * @param store The store to read; must be open.
     * @param query The selection.
     * @return What was written.
     * @throws IoError When the store is closed, SQLite fails, or the file cannot be
     * written or renamed; the partial file is removed. Messages carry no paths.
     * @throws InvalidArgumentError As ObjectStore::query() throws for a bad query, or
     * when two resolved columns share a name (ColumnSchema::assertedMeasurement()); the
     * latter before any file is created.
     * @throws std::bad_alloc When memory is exhausted; the partial file is removed.
     */
    ExportResult write(Store& store, const ObjectQuery& query);
    /**
     * @brief Exports the objects the query selects.
     *
     * Calls observer->onProgress("export", fraction, completed, total) after every 1000
     * objects and once at the end, where total is ObjectStore::count(query). The token
     * is checked before each object; once it is cancelled, the partial file is removed
     * and the result reports the objects written before the check.
     * @param store The store to read; must be open.
     * @param query The selection.
     * @param observer Receives progress on this thread; may be null.
     * @param token Cancels the export.
     * @return What was written; wasCancelled() is true after a cancellation, and then
     * bytesWritten() is 0.
     * @throws IoError When the store is closed, SQLite fails, or the file cannot be
     * written or renamed; the partial file is removed. Messages carry no paths.
     * @throws InvalidArgumentError As ObjectStore::query() throws for a bad query, or
     * when two resolved columns share a name (ColumnSchema::assertedMeasurement()); the
     * latter before any file is created.
     * @throws std::bad_alloc When memory is exhausted; the partial file is removed.
     */
    ExportResult write(Store& store, const ObjectQuery& query, ProgressObserver* observer,
                       const CancellationToken& token);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace slideio::processor
