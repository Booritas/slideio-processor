// Requirements: UR-RES-7, UR-QA-3
#pragma once

/**
 * @file mask_writer.hpp
 * @brief MaskWriter: writes a label mask or processing map, as a SlideIO scene, to a lossless
 * pyramidal OME-TIFF.
 */

#include <slideio/processor/core/control.hpp>
#include <slideio/processor/pipeline/export.hpp>
#include <slideio/processor/reporting/export_result.hpp>
#include <slideio/processor/storage/raster_store.hpp>

#include <memory>

namespace slideio {
class Scene;
} // namespace slideio

namespace slideio::processor {

/**
 * @brief Writes a label mask or a processing map, given as a SlideIO scene, at full
 * resolution as a lossless pyramidal OME-TIFF that SlideIO opens with all its levels.
 *
 * The scene is read through Scene::readResampledBlock, one 512 x 512 tile at a time, and
 * written by RasterTiffWriter at scale 1 over the whole scene: the file's layout, its
 * reduced levels, its compression (lossless only), its OME-XML and its partial-file and
 * cancellation behaviour are RasterTiffWriter's. Reduced levels of an integer type take
 * the top-left sample of each 2 x 2 block, so a label value is never blended. Each channel
 * becomes one plane, in order.
 *
 * Every channel must share one data type, and that type must be one the OME-TIFF layout
 * holds (Byte, Int8, UInt16, Int16, Int32, Float32, Float64). The scene is a label mask
 * or processing map: rasteriseLabels()'s, or a pipeline's (Pipeline::asScene()).
 * Converting whole-slide image files is not its purpose: general or lossy conversion
 * belongs to slideio-converter (SDS 07 section 7.8). Only z slice 0 and time frame 0 are
 * written.
 *
 * The pixel size written is the one set with resolution(); when none was set it is the
 * scene's own x resolution, converted from metres to micrometres, when that is greater
 * than 0, and none otherwise. Only the x resolution is used: RasterTiffWriter takes one
 * pixel size and writes it for both axes, so a scene with non-square pixels is written
 * as square at its x size. No path or other identifier is written.
 * @par Thread-safety
 * ThreadConfined: one thread at a time. The scene is read from the calling thread only.
 * @par Requirements
 * UR-RES-7, UR-QA-3 (SDS 07 section 7.6).
 */
class SLIDEIO_PROCESSOR_ENGINE_EXPORTS MaskWriter {
public:
    /**
     * @brief Creates a writer for a file; nothing is created until write().
     * @param path UTF-8 path of the TIFF file; not null, not empty. Name it with the
     * ".ome.tif" extension: Bio-Formats and QuPath read the OME-XML (and so the levels and
     * the pixel size) only for that extension.
     * @throws InvalidArgumentError When path is null or empty.
     * @throws std::bad_alloc When memory is exhausted.
     */
    explicit MaskWriter(const char* path);
    /** @brief Destroys the writer. */
    ~MaskWriter();
    MaskWriter(const MaskWriter&) = delete;             ///< Not copyable.
    MaskWriter& operator=(const MaskWriter&) = delete;  ///< Not copyable.

    /**
     * @brief Sets the compression (default RasterCompression::Deflate).
     * @param compression The compression; every value is lossless.
     * @return This writer.
     * @throws Nothing.
     */
    MaskWriter& compression(RasterCompression compression);
    /**
     * @brief Sets the pixel size of the output in micrometres, replacing the scene's own.
     * @param micronsPerPixel The size; 0 writes none.
     * @return This writer.
     * @throws InvalidArgumentError When the value is negative or not finite; the writer is
     * unchanged.
     */
    MaskWriter& resolution(double micronsPerPixel);
    /**
     * @brief Sets the image name in the OME-XML (default empty).
     * @param name UTF-8 name; null reads as empty. Copied.
     * @return This writer.
     * @throws InvalidArgumentError When the name is not well-formed UTF-8; the writer is
     * unchanged.
     * @throws std::bad_alloc When memory is exhausted; the writer is unchanged.
     */
    MaskWriter& name(const char* name);

    /**
     * @brief Reads the whole scene and writes the file.
     *
     * Progress and cancellation are RasterTiffWriter::write()'s: one progress call per
     * tile, stage name "raster export".
     * @param scene The scene; not null. Shared for the call.
     * @param observer Receives progress on this thread; may be null.
     * @param token Cancels the write.
     * @return objectsWritten() is the number of tiles, bytesWritten() the file's size;
     * wasCancelled() is true after a cancellation.
     * @throws InvalidArgumentError For a null scene, a scene with no channel or whose
     * channels differ in type; before any file is created.
     * @throws UnsupportedDataTypeError When the channels' type is not one the OME-TIFF
     * layout holds; before any file is created.
     * @throws IoError When SlideIO fails to describe or read the scene (its text is the
     * error's cause), or a file cannot be written or renamed; the partial files are
     * removed. Messages name the files they are about by their paths (a SlideIO failure, the
     * slide the scene reports; a write, the partial file; a rename, both files), never
     * names or values.
     * @throws ProcessorError Whatever the scene's own source or the observer throws; the
     * partial files are removed.
     * @throws std::bad_alloc When memory is exhausted; the partial files are removed.
     */
    ExportResult write(std::shared_ptr<slideio::Scene> scene, ProgressObserver* observer,
                       const CancellationToken& token);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace slideio::processor
