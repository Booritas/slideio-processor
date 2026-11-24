// Requirements: UR-API-2, UR-RES-7, UR-RES-8, UR-RES-9, UR-RES-11
#pragma once

/**
 * @file workspace_export.hpp
 * @brief writeExport(): writes one export of an open workspace to a file.
 */

#include <slideio/processor/core/control.hpp>
#include <slideio/processor/pipeline/export.hpp>
#include <slideio/processor/reporting/export_result.hpp>
#include <slideio/processor/run/export_spec.hpp>
#include <slideio/processor/workspace/workspace.hpp>

namespace slideio::processor {

/**
 * @brief Writes one export of `workspace` to `path`, as step 5 of SlideRun does: through the
 * export's writer, which writes "<path>.partial" and renames it onto `path` only when
 * complete (CsvWriter, GeoJsonExporter, rasteriseLabels() with MaskWriter, or
 * RasterTiffWriter, by ExportSpec::kind(); see SlideRun for what each writes).
 *
 * Reads the workspace only, so it works on a read-only workspace. The mask and raster
 * carry the slide's pixel size when it has one. The CSV and GeoJSON provenance carries
 * `slideId` in place of the slide path.
 * @param workspace An open workspace.
 * @param spec The export; its file name is not used, `path` is.
 * @param path UTF-8 path of the file to write, the full path. Its parent directory must
 * exist: the function makes no directory.
 * @param slideId Written in place of the slide path in CSV and GeoJSON provenance; null is "".
 * @param observer Progress observer, or null.
 * @param token Cancellation token.
 * @return The writer's result. When cancelled, wasCancelled() is true and no file is left
 * ("<path>.partial" included); the caller decides what to raise.
 * @throws InvalidArgumentError With a message starting "writeExport: " when `path` is null
 * or empty; when the layer of an object export is not in the workspace, or the raster of a
 * raster export is not in it or no stage has written it (the message names it).
 * @throws InternalError When SlideIO fails to describe the scene, or a raster was written at
 * a level the scene does not have.
 * @throws IoError As the export writers.
 * @par Thread-safety
 * ThreadConfined to the workspace's use: no other thread may use the workspace meanwhile.
 * @par Requirements
 * UR-API-2, UR-RES-7, UR-RES-8, UR-RES-9, UR-RES-11
 */
SLIDEIO_PROCESSOR_ENGINE_EXPORTS ExportResult writeExport(Workspace& workspace, const ExportSpec& spec,
                                                          const char* path, const char* slideId,
                                                          ProgressObserver* observer,
                                                          const CancellationToken& token);

} // namespace slideio::processor
