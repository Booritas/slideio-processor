// Requirements: UR-API-2, UR-STG-4, UR-RES-11
#pragma once

/**
 * @file pipeline_file.hpp
 * @brief PipelineFile: the JSON file that describes a whole slide run.
 */

#include <slideio/processor/pipeline/export.hpp>
#include <slideio/processor/run/export_spec.hpp>
#include <slideio/processor/storage/raster_store.hpp>
#include <slideio/processor/storage/types.hpp>

#include <cstdint>
#include <memory>

namespace slideio::processor {

class Workspace;

/**
 * @brief What a slide run does, as a value with a JSON form: the Python modules it
 * requires, the layers and rasters it declares, its stages, and its exports.
 *
 * A researcher builds the stages on a few slides in a workspace, takes the file from it
 * with fromWorkspace(), adds exports, and reuses the file unchanged for other slides.
 *
 * The JSON form, formatVersion 1, is one object:
 * `{"formatVersion":1,"requires":["module",...],
 * "layers":[{"name":"n","parent":"p","kind":"semantic"|"primitive"},...],
 * "rasters":[{"name":"r","type":"Float32","planes":1,"compression":"Deflate"},...],
 * "stages":[<stage record, stageRecordVersion 1>,...],
 * "exports":[{"kind":"csv"|"geojson"|"labelMask"|"rasterTiff","file":"f",
 * "layer":"n"|"raster":"r","measurements":["area",...]},...]}`.
 * A layer's `parent` defaults to the pixel layer and its `kind` to semantic; a raster's
 * `compression` to Deflate (`None`, `Lzw` and `Deflate` are the spellings); a CSV export
 * without `measurements` exports every measurement. `requires`, `layers`, `rasters`,
 * `stages` and `exports` are required members, and may be empty. A stage entry is
 * kept as the record it is, including keys this version does not read; layer names in it
 * are resolved when the file is run, not here.
 *
 * An export file is a plain name (see ExportSpec); names compare case-insensitively
 * for duplicates. An export may name a layer or raster the file declares or one a stage
 * of the file writes (its record's `writesLayers` or `writesRaster`).
 * Messages name stages, layers, rasters, export kinds and members, never values or paths.
 * @par Thread-safety
 * ThreadConfined to build; const members are safe to call concurrently.
 * @par Requirements
 * UR-API-2, UR-STG-4, UR-RES-11
 */
class SLIDEIO_PROCESSOR_ENGINE_EXPORTS PipelineFile {
public:
    /**
     * @brief Creates an empty file: no requirements, layers, rasters, stages or exports.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-API-2
     */
    PipelineFile();
    /**
     * @brief Copies a file; the copy shares nothing with the original. A moved-from file
     * copies as an empty file.
     * @param other The file to copy.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * Reads @p other; ThreadCompatible.
     * @par Requirements
     * UR-API-2
     */
    PipelineFile(const PipelineFile& other);
    /**
     * @brief Moves a file. The moved-from file reads as an empty file and is reusable.
     * @param other The file to move from.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-API-2
     */
    PipelineFile(PipelineFile&& other) noexcept;
    /**
     * @brief Replaces this file with a copy of another.
     * @param other The file to copy.
     * @return This file.
     * @throws std::bad_alloc When allocation fails; this file is then unchanged.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-API-2
     */
    PipelineFile& operator=(const PipelineFile& other);
    /**
     * @brief Replaces this file by moving another.
     * @param other The file to move from; reads as an empty file afterwards.
     * @return This file.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-API-2
     */
    PipelineFile& operator=(PipelineFile&& other) noexcept;
    ~PipelineFile();

    /**
     * @brief Reads a file from its JSON text.
     * @param json UTF-8 JSON text.
     * @return The file.
     * @throws InvalidArgumentError When the text is null or does not parse (the message
     * carries the offset); when formatVersion is not 1; when a member is missing or of
     * the wrong type, or a string holding a NUL (`\u0000`); when an export kind is unknown
     * or an export file name is unsafe (ExportSpec);
     * when layer, raster, stage or export-file names repeat (export files compared
     * case-insensitively); when a layer's parent is not an earlier layer; when a stage's
     * pipeline does not parse; when a stage's record names, in readsLayers, writesLayers,
     * annotatesLayers, restrictToLayer, readsLayersAsRaster, writesRaster or readsRasters,
     * a layer or raster the file does not declare (the pixel layer needs no declaration);
     * or when an export names a layer or raster the file does not declare.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-API-2, UR-STG-4
     */
    static PipelineFile fromJson(const char* json);
    /**
     * @brief Reads a file from a JSON file on disk.
     * @param path UTF-8 path of the file.
     * @return The file.
     * @throws InvalidArgumentError When the path is null, or as fromJson().
     * @throws IoError When the file cannot be read (the message does not name the path).
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-API-2
     */
    static PipelineFile load(const char* path);
    /**
     * @brief Takes the layers, rasters and stages of a development workspace.
     *
     * Every layer but the pixel layer (parent null when its parent is the pixel layer),
     * every raster, and every stage's record in declaration order. A raster's
     * compression is not written: the raster registry cannot report it, so the default
     * applies when the file is loaded. Requirements and exports are not taken.
     * @param workspace The workspace; it is only read.
     * @return The file.
     * @throws InvalidArgumentError When a stage record does not read back as a valid entry.
     * @throws IoError When the workspace is closed.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * ThreadSafe on the workspace's own terms; the result is a new value.
     * @par Requirements
     * UR-API-2, UR-STG-4
     */
    static PipelineFile fromWorkspace(Workspace& workspace);

    /**
     * @brief The JSON form; fromJson() of it gives an equal file.
     * @param buffer Caller buffer for UTF-8 text with a terminator; null queries the length.
     * @param capacity Size of @p buffer in bytes; the text is truncated to fit.
     * @return The length of the full text, excluding the terminator.
     * @throws InternalError When serialisation fails.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * Safe to call concurrently on a const file.
     * @par Requirements
     * UR-API-2
     */
    int32_t toJson(char* buffer, int32_t capacity) const;

    /**
     * @brief Adds a required Python module. A name already listed is ignored.
     * @param module Importable module name, non-null and non-empty.
     * @return This file, for chaining.
     * @throws InvalidArgumentError When the name is null or empty.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-API-2
     */
    PipelineFile& require(const char* module);
    /**
     * @brief Adds an export.
     * @param exportSpec The export.
     * @return This file, for chaining.
     * @throws InvalidArgumentError When its file name repeats an earlier export's
     * (case-insensitively), or it names a layer or raster the file does not declare. The
     * file is then unchanged.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-API-2
     */
    PipelineFile& addExport(const ExportSpec& exportSpec);

    /**
     * @brief Returns the number of required modules.
     * @return The count.
     * @throws Nothing.
     * @par Thread-safety
     * Safe to call concurrently on a const file.
     * @par Requirements
     * UR-API-2
     */
    int32_t requireCount() const noexcept;
    /**
     * @brief Returns a required module name.
     * @param index Position, from 0 to requireCount() - 1.
     * @return The name; valid until the file is modified or destroyed.
     * @throws InvalidArgumentError When index is out of range.
     * @par Thread-safety
     * Safe to call concurrently on a const file.
     * @par Requirements
     * UR-API-2
     */
    const char* requireAt(int32_t index) const;

    /**
     * @brief Returns the number of declared layers, the pixel layer not included.
     * @return The count.
     * @throws Nothing.
     * @par Thread-safety
     * Safe to call concurrently on a const file.
     * @par Requirements
     * UR-API-2
     */
    int32_t layerCount() const noexcept;
    /**
     * @brief Returns a layer's name.
     * @param index Position, from 0 to layerCount() - 1.
     * @return The name; valid until the file is modified or destroyed.
     * @throws InvalidArgumentError When index is out of range.
     * @par Thread-safety
     * Safe to call concurrently on a const file.
     * @par Requirements
     * UR-API-2
     */
    const char* layerName(int32_t index) const;
    /**
     * @brief Returns a layer's parent.
     * @param index Position, from 0 to layerCount() - 1.
     * @return The parent's name, or null when the parent is the pixel layer (and for a
     * primitive layer); valid until the file is modified or destroyed.
     * @throws InvalidArgumentError When index is out of range.
     * @par Thread-safety
     * Safe to call concurrently on a const file.
     * @par Requirements
     * UR-API-2
     */
    const char* layerParent(int32_t index) const;
    /**
     * @brief Returns a layer's kind.
     * @param index Position, from 0 to layerCount() - 1.
     * @return Semantic or Primitive.
     * @throws InvalidArgumentError When index is out of range.
     * @par Thread-safety
     * Safe to call concurrently on a const file.
     * @par Requirements
     * UR-API-2
     */
    LayerKind layerKind(int32_t index) const;

    /**
     * @brief Returns the number of declared rasters.
     * @return The count.
     * @throws Nothing.
     * @par Thread-safety
     * Safe to call concurrently on a const file.
     * @par Requirements
     * UR-API-2
     */
    int32_t rasterCount() const noexcept;
    /**
     * @brief Returns a raster declaration.
     * @param index Position, from 0 to rasterCount() - 1.
     * @return The declaration; valid until the file is modified or destroyed.
     * @throws InvalidArgumentError When index is out of range.
     * @par Thread-safety
     * Safe to call concurrently on a const file.
     * @par Requirements
     * UR-API-2
     */
    const RasterDeclaration& rasterAt(int32_t index) const;

    /**
     * @brief Returns the number of stages.
     * @return The count.
     * @throws Nothing.
     * @par Thread-safety
     * Safe to call concurrently on a const file.
     * @par Requirements
     * UR-API-2
     */
    int32_t stageCount() const noexcept;
    /**
     * @brief Returns a stage's name.
     * @param index Position, from 0 to stageCount() - 1.
     * @return The name; valid until the file is modified or destroyed.
     * @throws InvalidArgumentError When index is out of range.
     * @par Thread-safety
     * Safe to call concurrently on a const file.
     * @par Requirements
     * UR-API-2
     */
    const char* stageName(int32_t index) const;
    /**
     * @brief Returns a stage's record, the stageRecordVersion 1 JSON, compact.
     * @param index Position, from 0 to stageCount() - 1.
     * @return The record text; valid until the file is modified or destroyed.
     * @throws InvalidArgumentError When index is out of range.
     * @par Thread-safety
     * Safe to call concurrently on a const file.
     * @par Requirements
     * UR-API-2, UR-STG-4
     */
    const char* stageRecord(int32_t index) const;

    /**
     * @brief Returns the number of exports.
     * @return The count.
     * @throws Nothing.
     * @par Thread-safety
     * Safe to call concurrently on a const file.
     * @par Requirements
     * UR-API-2
     */
    int32_t exportCount() const noexcept;
    /**
     * @brief Returns an export.
     * @param index Position, from 0 to exportCount() - 1.
     * @return The export; valid until the file is modified or destroyed.
     * @throws InvalidArgumentError When index is out of range.
     * @par Thread-safety
     * Safe to call concurrently on a const file.
     * @par Requirements
     * UR-API-2
     */
    const ExportSpec& exportAt(int32_t index) const;

    /**
     * @brief Compares two files by value: requirements, layers, rasters (name, type,
     * planes, compression), stage records and exports, each in order.
     * @param other The file to compare with.
     * @return True when equal.
     * @throws Nothing.
     * @par Thread-safety
     * Safe to call concurrently on const files.
     * @par Requirements
     * UR-API-2
     */
    bool equals(const PipelineFile& other) const noexcept;

private:
    class Impl;
    static const Impl& read(const std::unique_ptr<Impl>& impl) noexcept;
    static Impl& write(std::unique_ptr<Impl>& impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace slideio::processor
