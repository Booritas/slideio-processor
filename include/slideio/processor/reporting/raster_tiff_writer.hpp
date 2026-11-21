// Requirements: UR-RES-7, UR-QA-3
#pragma once

/**
 * @file raster_tiff_writer.hpp
 * @brief RasterTiffWriter: writes any tile raster source as a lossless pyramidal OME-TIFF.
 */

#include <slideio/processor/core/control.hpp>
#include <slideio/processor/core/export.hpp>
#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/reporting/export_result.hpp>
#include <slideio/processor/storage/raster_store.hpp>

#include <memory>

namespace slideio::processor {

/**
 * @brief Rasterises a region of a TileRasterSource at a scale and writes it as a
 * pyramidal OME-TIFF that SlideIO opens with all its levels.
 *
 * The file is tiled 512 x 512 with one sample per plane and planes stored separately.
 * Page 0 is the full-resolution image; its reduced levels, each half the size of the one
 * before, rounded up, until both sides are at most 512, are SubIFDs of page 0. Page 0's
 * ImageDescription is minimal OME-XML: the image name (empty by default) and, when
 * resolution() was set, the pixel size in micrometres. No path or other identifier is
 * written. Compression is lossless (None, LZW or Deflate). Reduced levels keep storage's
 * rules: integer types take the top-left sample of each 2 x 2 block, so a label value is
 * never blended; floating types take the mean.
 *
 * The data type is the source's planeType() and the planes its planeCount(). The OME-TIFF
 * layout holds Byte, Int8, UInt16, Int16, Int32, Float32 and Float64; any other type is
 * refused before a file is created.
 *
 * The output size is ceil(region size / scale). The source is asked for each 512 x 512
 * output tile (edge tiles are clipped to the output) through a BlockView whose slideRect
 * is the tile's level-0 extent. The file goes to "<path>.partial", renamed onto path,
 * replacing any file there, only when it completes; a spool file "<path>.partial.spool"
 * exists meanwhile. A cancelled or failed write removes both and leaves any file already
 * at path untouched. Two writes of the same source with the same settings give
 * byte-identical files.
 * @par Thread-safety
 * ThreadConfined: one thread at a time. The source's fillTile() is called from the
 * calling thread only. Two writers on the same path share "<path>.partial" and must not
 * run at once.
 * @par Requirements
 * UR-RES-7, UR-QA-3 (SDS 07 section 7.6).
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS RasterTiffWriter {
public:
    /**
     * @brief Creates a writer for a file; nothing is created until write().
     * @param path UTF-8 path of the TIFF file; not null, not empty.
     * @throws InvalidArgumentError When path is null or empty.
     * @throws std::bad_alloc When memory is exhausted.
     */
    explicit RasterTiffWriter(const char* path);
    /** @brief Destroys the writer. */
    ~RasterTiffWriter();
    RasterTiffWriter(const RasterTiffWriter&) = delete;             ///< Not copyable.
    RasterTiffWriter& operator=(const RasterTiffWriter&) = delete;  ///< Not copyable.

    /**
     * @brief Sets the compression (default RasterCompression::Deflate).
     * @param compression The compression; every value is lossless.
     * @return This writer.
     * @throws Nothing.
     */
    RasterTiffWriter& compression(RasterCompression compression);
    /**
     * @brief Sets the pixel size of the output in micrometres (default 0, unknown).
     * @param micronsPerPixel The size; 0 writes none.
     * @return This writer.
     * @throws InvalidArgumentError When the value is negative or not finite; the writer is
     * unchanged.
     */
    RasterTiffWriter& resolution(double micronsPerPixel);
    /**
     * @brief Sets the image name in the OME-XML (default empty).
     * @param name UTF-8 name; null reads as empty. Copied.
     * @return This writer.
     * @throws std::bad_alloc When memory is exhausted; the writer is unchanged.
     */
    RasterTiffWriter& name(const char* name);

    /**
     * @brief Rasterises the region and writes the file.
     *
     * Calls observer->onProgress("raster export", fraction, completed, total) after each
     * tile, where total is the tile count. The token is checked before each tile; once it
     * is cancelled the partial files are removed and the result reports the tiles written
     * so far and 0 bytes.
     * @param source The raster to read; borrowed for the call.
     * @param regionLevel0 The region in level-0 pixels; not empty.
     * @param scale Level-0 pixels per output pixel; finite and at least 1.
     * @param observer Receives progress on this thread; may be null.
     * @param token Cancels the write.
     * @return objectsWritten() is the number of tiles, bytesWritten() the file's size;
     * wasCancelled() is true after a cancellation.
     * @throws InvalidArgumentError For an empty region, a scale below 1 or not finite, an
     * output beyond 2^31-1 pixels on a side, or a source with no planes; before any file
     * is created.
     * @throws UnsupportedDataTypeError When the source's type is not one the OME-TIFF
     * layout holds; before any file is created.
     * @throws IoError When a file cannot be written or renamed; the partial files are
     * removed. Messages carry no paths, names or values.
     * @throws ProcessorError Whatever the source or the observer throws; the partial files
     * are removed.
     * @throws std::bad_alloc When memory is exhausted; the partial files are removed.
     */
    ExportResult write(TileRasterSource& source, const Rect64& regionLevel0, double scale,
                       ProgressObserver* observer, const CancellationToken& token);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace slideio::processor
