// Requirements: UR-RES-7
#pragma once

// The spool behind a raster writer (ruling S46). writeTile cuts each block along the
// 512-pixel grid into fragments; each fragment is compressed with zlib level 1 outside any
// lock and appended to the spool file under the spool's mutex, beside a small in-memory
// index entry. At flush, forEachGridTile composes each grid tile from its fragments, in
// append order, so a later overlapping fragment wins. The file format is private to one
// process lifetime and carries no version: nothing but this spool ever reads it.

#include <slideio/processor/core/data_type.hpp>
#include <slideio/processor/storage/raster_store.hpp>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <mutex>
#include <vector>

namespace slideio::processor::detail {

// The edge of a grid tile in pixels, in the spool and in the TIFF (ruling S41).
constexpr int64_t kRasterTileSize = 512;

// What a raster's samples are and how its TIFF is compressed.
struct RasterFormat {
    DataType type = DataType::Unknown;
    int32_t planes = 0;
    RasterCompression compression = RasterCompression::Deflate;
};

// One spooled fragment: a rectangle within one grid tile, every plane of it, compressed
// as one zlib stream (planes one after another, rows packed) at `offset` in the file.
struct SpoolEntry {
    int32_t gridX = 0;  // grid column of the tile
    int32_t gridY = 0;  // grid row of the tile
    int32_t x = 0;      // the rectangle within the tile
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;
    int64_t offset = 0; // where its compressed bytes start in the spool file
    int64_t bytes = 0;  // how many compressed bytes it has
};

// A raster's spool file and the index of its fragments. Not copyable or movable.
// Thread-safety: append, extentWidth and extentHeight are safe from any thread at once.
// forEachGridTile and remove must not run beside any other call.
class TileSpool {
public:
    // Creates or truncates the spool file at `spoolFile`. Throws IoError naming the file
    // when it cannot be created, and InternalError when `format` has no sample size or
    // a plane count below 1.
    TileSpool(std::filesystem::path spoolFile, RasterFormat format);
    // Closes the file and leaves it on disk: removing it is remove()'s job.
    ~TileSpool();
    TileSpool(const TileSpool&) = delete;
    TileSpool& operator=(const TileSpool&) = delete;

    // Spools the block of `width` x `height` pixels whose top-left is (originX, originY),
    // in level pixels. Plane p's top-left sample is at planeData[p], and its rows are
    // rowStrides[p] bytes apart; both arrays have format.planes entries. Extends the
    // extent to cover the block. An empty block is a no-op. Throws InternalError for a
    // negative origin or size, or an extent beyond 2^31-1 (the writer validates first),
    // and IoError naming the file when the append fails.
    void append(int64_t originX, int64_t originY, int64_t width, int64_t height, const void* const* planeData,
                const int64_t* rowStrides);

    // The format the spool was created with.
    const RasterFormat& format() const noexcept;

    // The largest originX + width (originY + height) appended; 0 before any append.
    int64_t extentWidth() const;
    int64_t extentHeight() const;

    // Calls fn(gridX, gridY, planes) once for every grid tile that has a fragment, in
    // row-major order (row by row, columns ascending). `planes` holds one vector per plane
    // of 512 x 512 samples, row-major: zero where no fragment reached, otherwise the
    // fragments applied in append order. The vectors are reused between calls. Throws
    // IoError naming the file when it cannot be read back, InternalError when a fragment
    // does not decode to its size, and whatever fn throws.
    void forEachGridTile(
        const std::function<void(int32_t gridX, int32_t gridY, const std::vector<std::vector<uint8_t>>& planes)>&
            fn) const;

    // Closes the file and deletes it. A file already gone is not an error. Throws IoError
    // naming the file when it cannot be deleted. Further appends are an InternalError.
    void remove();

private:
    std::filesystem::path path_;
    RasterFormat format_;
    int32_t sampleBytes_ = 0;
    mutable std::mutex mutex_;  // guards everything below
    mutable std::FILE* file_ = nullptr;
    int64_t end_ = 0;           // bytes written so far
    int64_t extentWidth_ = 0;
    int64_t extentHeight_ = 0;
    std::vector<SpoolEntry> entries_; // in append order
};

} // namespace slideio::processor::detail
