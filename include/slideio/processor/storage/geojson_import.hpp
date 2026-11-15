// Requirements: UR-RES-8, UR-RES-13, UR-RES-19, UR-IMG-5
#pragma once

/**
 * @file geojson_import.hpp
 * @brief ImportResult, GeoJsonImporter and importRoiPolygon: GeoJSON files into a store,
 * and a GeoJSON polygon as a processing ROI.
 */

#include <slideio/processor/core/control.hpp>
#include <slideio/processor/core/export.hpp>
#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/core/ids.hpp>
#include <slideio/processor/core/provenance.hpp>
#include <slideio/processor/storage/types.hpp>

#include <cstdint>
#include <memory>

namespace slideio::processor {

class Store;

/**
 * @brief What one GeoJsonImporter::import() call did.
 *
 * A small value class. A default-constructed result, and a moved-from one, reads as an
 * import of nothing: zero counts, policy Strict and no source provenance.
 * @par Thread-safety
 * ThreadCompatible: const members may run concurrently; assignment needs exclusive
 * access.
 * @par Requirements
 * UR-RES-19
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS ImportResult {
public:
    /**
     * @brief Builds a result that reads as an import of nothing. Does not allocate.
     * @throws Nothing.
     */
    ImportResult() noexcept;
    /**
     * @brief Copies a result.
     * @param other The result to copy.
     * @throws std::bad_alloc When memory is exhausted.
     */
    ImportResult(const ImportResult& other);
    /**
     * @brief Moves a result; `other` then reads as a default result.
     * @param other The result to move from.
     * @throws Nothing.
     */
    ImportResult(ImportResult&& other) noexcept;
    /**
     * @brief Copies a result.
     * @param other The result to copy.
     * @return This result.
     * @throws std::bad_alloc When memory is exhausted; this result is then unchanged.
     */
    ImportResult& operator=(const ImportResult& other);
    /**
     * @brief Moves a result; `other` then reads as a default result. Never swaps.
     * @param other The result to move from.
     * @return This result.
     * @throws Nothing.
     */
    ImportResult& operator=(ImportResult&& other) noexcept;
    /** @brief Releases the result. */
    ~ImportResult();

    /**
     * @brief Returns the number of objects the import committed.
     * @return The count.
     * @throws Nothing.
     */
    int64_t objectsImported() const noexcept;
    /**
     * @brief Returns the number of labels the import registered, with origin
     * LabelOrigin::ImportRegistered; always 0 under ImportPolicy::Strict.
     * @return The count.
     * @throws Nothing.
     */
    int32_t labelsRegistered() const noexcept;
    /**
     * @brief Returns the number of features skipped for their geometry: null, missing,
     * empty or unsupported (GeoJsonReader::featuresSkipped()), or invalid (the reader
     * threw a geometry error).
     * @return The count, saturated at the int32_t maximum.
     * @throws Nothing.
     */
    int32_t featuresSkipped() const noexcept;
    /**
     * @brief Returns the import policy in force when the import began.
     * @return VocabularyRegistry::importPolicy() at call time.
     * @throws Nothing.
     */
    ImportPolicy policyApplied() const noexcept;
    /**
     * @brief Tells whether the file carried a provenance record.
     * @return True when the file was a FeatureCollection with a "provenance" object.
     * @throws Nothing.
     */
    bool hasSourceProvenance() const noexcept;
    /**
     * @brief Returns the file's provenance record.
     *
     * The import never writes it into the store, whose provenance describes the slide the
     * store analyses; the file's may describe another slide or scene, and only the caller
     * can tell.
     * @return The record, restored through Provenance::restored(); a default-constructed
     * Provenance when hasSourceProvenance() is false. Valid as long as this result lives
     * and is not assigned to.
     * @throws Nothing.
     */
    const Provenance& sourceProvenance() const noexcept;

private:
    friend class GeoJsonImporter;
    class Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * @brief Imports the features of a GeoJSON file (QuPath-shaped or written by
 * GeoJsonWriter) into one layer of a store, attributed to one stage.
 *
 * Every feature with supported geometry becomes one object of the target layer, whatever
 * the feature's own "layer" says, so a layer query returns imported objects alongside
 * those a stage wrote (UR-RES-13). The object's label is the feature's classification (an
 * empty one leaves it unlabelled), resolved through VocabularyRegistry::find(), so QuPath
 * spellings such as `Tumor` resolve to the standard labels. Each "measurements.<name>"
 * property, object or array form, becomes measurement `<name>` with unit
 * MeasurementUnit::Dimensionless, its text parsed independently of the locale as a finite
 * number; a value that is not one skips that measurement only, with a LogLevel::Warning
 * naming the feature index and the measurement name. Other properties and the "labels"
 * array are not imported.
 *
 * A classification the vocabulary lacks is governed by VocabularyRegistry::importPolicy():
 * under ImportPolicy::Strict the import fails naming the label before anything is written;
 * under ImportPolicy::Permissive the label is registered with origin
 * LabelOrigin::ImportRegistered and a LabelAdded event (UR-RES-19).
 *
 * A feature with null, missing, empty or unsupported geometry, or whose geometry the
 * reader rejects, is skipped with a LogLevel::Warning naming its index. Feature indices
 * are 0-based positions among the feature objects of the file. No diagnostic copies a
 * property value or a provenance field.
 *
 * The importer borrows the store: the Store must outlive it.
 * @par Thread-safety
 * ThreadConfined.
 * @par Requirements
 * UR-RES-8, UR-RES-13, UR-RES-19
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS GeoJsonImporter {
public:
    /**
     * @brief Prepares imports into a layer, attributed to a stage.
     * @param store The store; borrowed, it must outlive the importer.
     * @param targetLayer A declared, undropped layer other than the pixel layer, semantic
     * or primitive; every imported object lands in it.
     * @param assertingStage A declared stage; every imported object is attributed to it.
     * @throws InvalidArgumentError When the target layer is unknown, the pixel layer or
     * dropped, or when the stage is unknown.
     * @throws IoError When the store is read-only or closed, or when SQLite fails.
     * @throws std::bad_alloc When memory is exhausted.
     */
    GeoJsonImporter(Store& store, LayerId targetLayer, StageId assertingStage);
    /** @brief Releases the importer. */
    ~GeoJsonImporter();
    GeoJsonImporter(const GeoJsonImporter&) = delete;             ///< Not copyable.
    GeoJsonImporter& operator=(const GeoJsonImporter&) = delete;  ///< Not copyable.

    /**
     * @brief Imports a file, without progress reporting or cancellation.
     * @param path UTF-8 path of the GeoJSON file.
     * @return What the import did.
     * @throws As import(const char*, ProgressObserver*, const CancellationToken&), except
     * CancelledError.
     */
    ImportResult import(const char* path);
    /**
     * @brief Imports a file.
     *
     * The file is read twice. The first pass counts the features, for progress, and under
     * ImportPolicy::Strict resolves every classification, writing nothing. The second
     * emits the objects through ObjectStore::writerForStage(), whose batches and checks
     * apply. Progress is reported after each committed batch and at the end, in features,
     * as onProgress(stage name, done / total, done, total). Cancellation is checked
     * before each feature of both passes.
     *
     * A failure in the second pass, cancellation included, leaves the batches already
     * committed attributed to the stage; the import never changes the stage's state, and
     * ObjectStore::discardStage() removes them. The store's own provenance is never
     * changed.
     * @param path UTF-8 path of the GeoJSON file.
     * @param observer Receives progress; may be null.
     * @param token Cancels the import.
     * @return What the import did.
     * @throws InvalidArgumentError When path is null; when the file is not GeoJSON
     * (GeoJsonReader::next()); under ImportPolicy::Strict, when a classification is not
     * in the vocabulary (the message names the label and the feature index; nothing is
     * written); or when an object is refused or its anchor collides with a stored object
     * (ObjectWriter::emit(); the batch is dropped).
     * @throws CancelledError When the token is cancelled; pending objects are committed
     * first.
     * @throws IoError When the file cannot be opened or read, or when the store is closed
     * or read-only, or SQLite fails.
     * @throws InternalError When the parser fails for any other reason.
     * @throws std::bad_alloc When memory is exhausted.
     */
    ImportResult import(const char* path,
                        ProgressObserver* observer,
                        const CancellationToken& token);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * @brief Reads the first polygon of a GeoJSON file as a processing ROI.
 *
 * No store is involved; the result is passed to PipelineSpec::restrictToPolygon. It is
 * the full geometry, every ring and hole, of the first feature with supported geometry
 * (Polygon or MultiPolygon); features before it whose geometry is null, missing, empty or
 * unsupported are skipped (UR-IMG-5).
 * @param path UTF-8 path of the GeoJSON file.
 * @return The polygon.
 * @throws InvalidArgumentError When path is null; when the file holds no feature with
 * supported geometry (the message is "importRoiPolygon: '<path>' holds no polygon"); when
 * that feature's geometry is invalid (the error is not passed over to a later feature);
 * or when the file is not GeoJSON.
 * @throws IoError When the file cannot be opened or read.
 * @throws InternalError When the parser fails for any other reason.
 * @throws std::bad_alloc When memory is exhausted.
 * @par Thread-safety
 * ThreadSafe: it shares no state.
 * @par Requirements
 * UR-IMG-5
 */
SLIDEIO_PROCESSOR_DATA_EXPORTS Polygon importRoiPolygon(const char* path);

} // namespace slideio::processor
