// Requirements: UR-RES-7, UR-STG-10
#pragma once

// Reads a published raster file at any scale and region (ruling S50), and the two
// TileRasterSource implementations behind RasterRegistry::sourceFor: one over such a file,
// one that reads zero for a raster that is not populated.

#include "storage/detail/tiff_io.hpp"
#include "storage/detail/tile_spool.hpp"

#include <slideio/processor/core/block_view.hpp>
#include <slideio/processor/core/flow.hpp>

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace slideio::processor::detail {

// One page of a raster file: its size and its TIFF tile size, in page pixels.
struct RasterFilePage {
    int64_t width = 0;
    int64_t height = 0;
    int64_t tileWidth = 0;
    int64_t tileHeight = 0;
};

// The pyramidal BigTIFF of one published raster (rulings S41 and S50). The constructor
// opens a first read handle and checks the file against what its row says, so the file
// is held from then on: on POSIX a later unlink leaves it readable, and on Windows it
// cannot be deleted while this object lives. Further handles are opened on demand, one
// per concurrent fill(), and pooled for reuse.
class TiffRasterReader {
public:
    // `path` is the file; `format` the declared sample type and plane count; `scaleX` and
    // `scaleY` the level-0 pixels per page-0 pixel and `width` x `height` page 0's size,
    // all from the row. Throws IoError naming the file by its role, never its path, when it cannot be opened, or when
    // its pages, samples or planes are not the ones the row and S41 describe; and
    // std::bad_alloc.
    TiffRasterReader(std::filesystem::path path, RasterFormat format, double scaleX, double scaleY, int64_t width,
                     int64_t height);
    TiffRasterReader(const TiffRasterReader&) = delete;
    TiffRasterReader& operator=(const TiffRasterReader&) = delete;
    ~TiffRasterReader();

    // Fills `out`, which the caller has validated: exactly format.planes planes of
    // format.type, positive width and height, and a non-empty slideRect. Let t be the
    // smaller of the view's two level-0-per-pixel ratios; the page read is the coarsest k
    // with scaleX * 2^k <= t * (1 + 1e-9), else page 0. View pixel (i, j) takes page pixel
    // (floor((x0 + (i + 0.5) * tx) / (scaleX * 2^k)), likewise for y); pixels outside the
    // page are 0. Each TIFF tile it touches is decoded once per plane. ThreadSafe. Throws
    // IoError naming the file by its role, never its path, when libtiff fails, or when no handle is open and the file
    // can no longer be opened; and std::bad_alloc.
    void fill(BlockView& out);

private:
    // Drives acquire() and discard() directly, to pin the pool's wake-ups (test only).
    friend struct TiffRasterReaderTestSeam;

    // A read handle, the page its directory is set to, and its tile buffer.
    struct Handle {
        TiffHandle tiff;
        int64_t page = -1;  // the current directory; -1 before the first TIFFSetDirectory
        std::vector<uint8_t> tile;
    };

    // Takes an idle handle, else opens one. When opening fails while other handles are
    // open, waits for one of them to come back instead, since a file unlinked after the
    // constructor is still readable through them (POSIX). Rethrows the open failure when
    // no handle is open, after waking every waiter, which then tries an open of its own.
    std::unique_ptr<Handle> acquire();
    // Returns a handle to the pool after a fill that succeeded.
    void release(std::unique_ptr<Handle> handle) noexcept;
    // Forgets a handle whose fill failed; it is closed, not reused.
    void discard(std::unique_ptr<Handle> handle) noexcept;
    // Opens a handle, running openHook_ first when it is set.
    std::unique_ptr<Handle> open() const;

    const std::filesystem::path path_;
    const std::string pathUtf8_;
    const RasterFormat format_;
    const double scaleX_;
    const double scaleY_;
    std::vector<RasterFilePage> pages_;  // page 0 first; fixed by the constructor

    std::mutex mutex_;  // guards idle_ and open_
    std::condition_variable returned_;
    std::vector<std::unique_ptr<Handle>> idle_;
    int32_t open_ = 0;  // handles open: idle plus in use
    std::function<void()> openHook_;  // test seam (TiffRasterReaderTestSeam); empty in the library
};

// A source over `reader` (defined in raster_source.cpp). planeType() and planeCount() are
// `format`'s. fillTile throws InvalidArgumentError unless the view has exactly
// format.planes planes of format.type and a derivable scale (positive width, height and
// slideRect extent), then fills it through the reader. ThreadSafe. Throws std::bad_alloc.
TileRasterSourcePtr makeFileRasterSource(std::shared_ptr<TiffRasterReader> reader, RasterFormat format);

// A source that validates as makeFileRasterSource's does and fills zeros: an unpopulated
// raster. Throws std::bad_alloc.
TileRasterSourcePtr makeZeroRasterSource(RasterFormat format);

} // namespace slideio::processor::detail
