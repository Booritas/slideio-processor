// Requirements: UR-API-2, UR-STG-1, UR-STG-4, UR-EXE-4, UR-QA-3, UR-RES-7, UR-RES-8, UR-RES-9
#pragma once

/**
 * @file slide_run.hpp
 * @brief SlideRun: runs a pipeline file against one slide scene and writes its exports.
 */

#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/control.hpp>
#include <slideio/processor/pipeline/export.hpp>
#include <slideio/processor/run/pipeline_file.hpp>
#include <slideio/processor/run/slide_run_result.hpp>

#include <cstdint>
#include <memory>

namespace slideio::processor {

/**
 * @brief Runs one pipeline file against one slide scene: the single-slide entry point
 * that a command-line tool, the Python bindings and later batch processing wrap.
 *
 * The pipeline file names no slide: the slide, the scene, the workspace directory and the
 * export directory are the run's arguments. run() works in five steps, in order:
 * 1. **Validate operations.** Every operation of every stage, in file order, must be
 *    registered in the OperationCatalog; the first that is not is refused, naming it.
 *    Then each is built with OperationCatalog::create() from its parameters, whose
 *    errors propagate. Nothing has been created or written yet. Validation precedes
 *    declaration so that a stage's record lists its nondeterministic operations.
 * 2. **Open or create the workspace.** A workspace directory holding a store
 *    ("<workspace>/workspace.store", Workspace's layout) is first opened read-only to
 *    read its binding: when its slide is not the same file as the slide given
 *    (std::filesystem::equivalent) or its scene index differs, the run is refused and
 *    nothing is written. Otherwise it is opened for writing (Workspace::open). Any other
 *    path is created (Workspace::create).
 * 3. **Declare.** Each layer of the file is declared unless the workspace has one of
 *    that name, which is reused when it has the same kind and parent and refused
 *    otherwise; each raster likewise, compared by plane type and plane count. Then every
 *    stage is declared from its record. An identical declaration changes nothing; a
 *    changed one makes the stage and its dependents stale (StageRegistry::declare()).
 * 4. **Run stages.** Repeatedly, the first stage in file order that is not Complete, or
 *    is stale, and is runnable, runs with a StageRunner. When stages still need to run
 *    but none is runnable, the first of them is refused with its RunBlocker. Workspace
 *    stages the file does not name are left alone.
 * 5. **Write exports.** The export directory is made when missing. Each export is written
 *    to "<file>.partial" in it and renamed onto "<file>" only when complete, by its writer:
 *    CSV with CsvWriter, GeoJSON with GeoJsonExporter, a label mask with rasteriseLabels()
 *    over the scene's whole level-0 rect and MaskWriter, and a raster with
 *    RasterTiffWriter over RasterRegistry::sourceFor(), for the whole scene, at the
 *    scale of the level it was written at. The mask and raster carry the slide's pixel
 *    size when it has one.
 *
 * The progress observer and cancellation token are forwarded to every StageRunner and
 * export writer, and the token is also checked before each export. A cancelled run throws
 * CancelledError and leaves no partial file; an export file a previous run completed is
 * left as it was. The workspace is closed when run() returns or throws.
 *
 * Messages name stages, layers, rasters, operations and export kinds of the file, never
 * paths, slide names or values.
 * @par Thread-safety
 * ThreadConfined. One run at a time per workspace: the store's writer lock refuses a second.
 * @par Requirements
 * UR-API-2, UR-STG-1, UR-STG-4, UR-EXE-4, UR-QA-3, UR-RES-7, UR-RES-8, UR-RES-9
 */
class SLIDEIO_PROCESSOR_ENGINE_EXPORTS SlideRun {
public:
    /**
     * @brief Binds a run to its file and arguments. Opens and writes nothing.
     * @param file The pipeline file; copied.
     * @param slidePath UTF-8 path of the slide; copied.
     * @param sceneIndex The scene of the slide; not negative.
     * @param workspacePath UTF-8 path of the workspace directory; copied.
     * @param exportDirectory UTF-8 path of the directory the exports go to; copied.
     * @param configuration The settings for the workspace; copied.
     * @throws InvalidArgumentError When a path is null or empty, or sceneIndex is negative.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-API-2
     */
    SlideRun(const PipelineFile& file, const char* slidePath, int32_t sceneIndex, const char* workspacePath,
             const char* exportDirectory, const Configuration& configuration);
    /**
     * @brief Releases the run.
     * @throws Nothing.
     */
    ~SlideRun();
    SlideRun(const SlideRun&) = delete;             ///< Not copyable.
    SlideRun& operator=(const SlideRun&) = delete;  ///< Not copyable.

    /**
     * @brief Sets the observer of the stages' and exports' progress. Borrowed, may be
     * null, must outlive run(). It is called from the stages' worker threads.
     * @param observer The observer, or null for none.
     * @return This run, for chaining.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-EXE-4
     */
    SlideRun& progress(ProgressObserver* observer) noexcept;
    /**
     * @brief Sets the token that cancels the run. The token is copied and shares its flag
     * with the original.
     * @param token The token.
     * @return This run, for chaining.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-EXE-4
     */
    SlideRun& cancellation(const CancellationToken& token) noexcept;

    /**
     * @brief Runs the file against the slide: validates, opens or creates the workspace,
     * declares, runs what is not current and writes the exports (see the class).
     * @return Per stage, whether it ran and what it wrote; per export, its file and size.
     * @throws InvalidArgumentError When an operation is not registered (the message names
     * it and its stage) or its factory refuses its parameters, before anything is created;
     * when the workspace belongs to another slide or scene ("the workspace belongs to
     * another slide or scene"), before anything is written; when a layer or raster of the
     * workspace differs from the file's (the message names it); when a stage record does
     * not declare (as StageRegistry::declare()); or when an export names a layer or raster
     * the workspace does not hold, or a raster no stage has written.
     * @throws StageError When stages still need to run but none can (the message names
     * the first and why), or as StageRunner::run().
     * @throws CancelledError When the token is cancelled; no partial file is left.
     * @throws IoError As Workspace::create() and Workspace::open(), when the export
     * directory cannot be made, or as the export writers.
     * @throws StoreFormatError When the workspace's store has another format version.
     * @throws ProcessorError Any other error of a writer, propagated.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-STG-1, UR-STG-4, UR-EXE-4, UR-QA-3, UR-RES-7, UR-RES-8, UR-RES-9
     */
    SlideRunResult run();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace slideio::processor
