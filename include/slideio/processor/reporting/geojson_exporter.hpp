// Requirements: UR-RES-8, UR-RES-11, UR-RES-15, UR-RES-18, UR-SEG-6, UR-QA-3
#pragma once

/**
 * @file geojson_exporter.hpp
 * @brief GeoJsonExporter: exports a query's objects to a QuPath-shaped GeoJSON file.
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
 * @brief Writes the objects a query selects to a GeoJSON FeatureCollection, one Feature
 * per object, through GeoJsonWriter.
 *
 * Features come in the query cursor's order. The document is GeoJsonWriter's: a
 * top-level "provenance" object (Store::provenance()), then one Feature per line. Every
 * feature has the shape QuPath reads: a Polygon, or a MultiPolygon for several outer
 * rings, with closed rings in level-0 integer coordinates, and properties "objectId"
 * (ObjectId::toString()), "objectType": "detection", "layer" (the layer name),
 * "stageId" (for an object with a producing stage), "classification": {"name": ...}
 * (the first label, absent for an object without labels), "labels" (an array of
 * {"name", "workspaceLocal"}, workspaceLocal true for a label that is not a standard
 * label) and "measurements" (an object of name to value).
 *
 * The ColumnSchema decides what a feature carries:
 * - Default: without a schema() call the schema is
 *   ColumnSchema().allMeasurements().units(UnitPolicy::BothColumns). A default export
 *   carries every finite value an object holds and every visible value a later stage
 *   asserted on it, including names like the CSV fixed columns ("layer", "labels")
 *   and twins whose base the store lacks ("len.um" alone). The exceptions: non-finite
 *   values (below), and a base name with both a ".um" and a ".um2" twin, of which only
 *   ".um2" is read (one twin per base, as everywhere in reporting).
 * - Measurements: the schema's measurement columns, resolved against the store as
 *   CsvWriter resolves them (except that names like the CSV fixed columns are kept),
 *   are the "measurements" keys, in column order and under
 *   the column names: "area", the calibrated twin "area.um2", and a later stage's
 *   assertion "<stage>/<name>" (ColumnSchema::assertedMeasurement()). A key the object
 *   lacks, or whose assertion is not visible, is absent. Two columns with the same name
 *   are refused before any file is created.
 * - Labels: LabelPolicy::EffectiveSet writes the whole effective label set
 *   (AnnotationStore order); LabelPolicy::ProducerOnly writes the producing stage's
 *   label alone, or none. The first label written is the "classification".
 * - Parent: includeParent(true) adds "parentId", the parent through a visible parent
 *   edge (EdgeStore::parentOf()), right after "objectId"; a root has none.
 * - Ignored: idFormat, includeLayer, includeStage, includeLabel(false),
 *   includeBoundingBox and includeGeometryWkt. A feature always carries its objectId,
 *   layer, geometry and labels.
 *
 * JSON has no NaN or infinity: a measurement whose value is not finite is left out of
 * that feature's "measurements", and the rest of the feature is written.
 *
 * Nothing else is written, in particular no export time, so two exports of an unchanged
 * store with the same query and schema are byte-identical (UR-QA-3).
 *
 * The provenance's slideId is the slide path as the store holds it. A path may contain
 * identifiers, such as an accession number, a medical record number, a patient's name
 * or a user name, that count as PHI or personal data under HIPAA and GDPR; consumers
 * who share an export must treat it accordingly. Redacting it is an open follow-up
 * (docs/project.md, reporting plan A).
 *
 * The export goes to "<path>.partial", renamed onto path, replacing any file there,
 * only when it completes. A cancelled or failed export removes the partial file and
 * leaves any file already at path untouched.
 *
 * GeoJsonImporter reads the file back into one layer, with geometry, classification and
 * measurements; it does not yet restore layers or parent edges from "layer" and
 * "parentId" (docs/project.md, reporting plan B).
 * @par Thread-safety
 * ThreadConfined: one thread at a time; the store is used as its own contract allows.
 * Two exporters on the same path share "<path>.partial" and must not run at once.
 * @par Requirements
 * UR-RES-8, UR-RES-11, UR-RES-15, UR-RES-18, UR-SEG-6, UR-QA-3 (SDS 07 section 7.5).
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS GeoJsonExporter {
public:
    /**
     * @brief Creates an exporter for a file; nothing is opened until write().
     * @param path UTF-8 path of the GeoJSON file; not null, not empty.
     * @throws InvalidArgumentError When path is null or empty.
     * @throws std::bad_alloc When memory is exhausted.
     */
    explicit GeoJsonExporter(const char* path);
    /** @brief Destroys the exporter. */
    ~GeoJsonExporter();
    GeoJsonExporter(const GeoJsonExporter&) = delete;             ///< Not copyable.
    GeoJsonExporter& operator=(const GeoJsonExporter&) = delete;  ///< Not copyable.

    /**
     * @brief Sets the measurements, labels and parent the features carry (default
     * ColumnSchema().allMeasurements().units(UnitPolicy::BothColumns)).
     * @param schema The schema; copied.
     * @return This exporter.
     * @throws std::bad_alloc When memory is exhausted; the exporter is unchanged.
     */
    GeoJsonExporter& schema(const ColumnSchema& schema);

    /**
     * @brief Exports the objects the query selects, with no observer and no cancellation.
     * @param store The store to read; must be open.
     * @param query The selection.
     * @return What was written.
     * @throws IoError When the store is closed, SQLite fails, or the file cannot be
     * written or renamed; the partial file is removed. Messages carry no paths.
     * @throws InvalidArgumentError As ObjectStore::query() throws for a bad query, or
     * when two resolved columns share a name; the latter before any file is created.
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
     * @return What was written; bytesWritten() is the finished file's size.
     * wasCancelled() is true after a cancellation, and then bytesWritten() is 0.
     * @throws IoError When the store is closed, SQLite fails, or the file cannot be
     * written or renamed; the partial file is removed. Messages carry no paths.
     * @throws InvalidArgumentError As ObjectStore::query() throws for a bad query, or
     * when two resolved columns share a name; the latter before any file is created.
     * @throws std::bad_alloc When memory is exhausted; the partial file is removed.
     */
    ExportResult write(Store& store, const ObjectQuery& query, ProgressObserver* observer,
                       const CancellationToken& token);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace slideio::processor
