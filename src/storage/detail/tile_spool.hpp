// Requirements: UR-RES-7
#pragma once

// The spool behind a raster writer (rulings S45 and S46, as amended in Task 4's fix
// round). writeTile hands each call to append whole: its level-0 slideRect, its size in
// level pixels, and its planes. Each plane is cut into pieces of 512 x 512 samples on a
// grid local to the call (pieces on its right and bottom edges are smaller), and each
// piece is compressed as its own zlib stream (level 1) outside any lock, then appended to
// the spool file under the spool's mutex beside a small in-memory index entry. Where a
// call lies in level pixels depends on the raster's scale, which is final only at flush,
// so the pieces are not aligned to the level's grid: place() fixes every call's origin
// from the final scale, and forEachGridTile composes each 512-pixel grid tile from the
// calls that touch it, in append order, so a later overlapping call wins, decoding only
// the pieces the tile meets. A piece meets at most 4 grid tiles, so it is decoded at
// most 4 times, row by row and only as far as each tile needs; no more than one tile
// per plane is ever held. The file format is private to one process lifetime and
// carries no version: nothing but this spool ever reads it.

#include <slideio/processor/core/data_type.hpp>
#include <slideio/processor/core/geometry.hpp>
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

// A call's origin in level pixels from its level-0 coordinate: coordinate / scale, rounded
// to the nearest pixel, halves away from zero, as llround does (ruling S45). A double, so
// a caller can range-check it before converting.
double levelOrigin(int64_t coordinate, double scale) noexcept;

// Where one piece's zlib stream lies in the spool file.
struct SpoolPiece {
    int64_t offset = 0;
    int64_t bytes = 0;
};

// One spooled call: its geometry, and where its pieces lie in the spool's pieces_: from
// `firstPiece` on, plane by plane, each plane's pieces row by row, columns ascending.
struct SpoolEntry {
    Rect64 slideRect{0, 0, 0, 0};  // the call's level-0 rectangle, as given
    int64_t width = 0;             // its size in level pixels
    int64_t height = 0;
    size_t firstPiece = 0;   // index of its first plane's first piece in pieces_
    int64_t originX = 0;     // its top-left in level pixels; set by place()
    int64_t originY = 0;
};

// A raster's spool file and the index of its calls. Not copyable or movable.
// Thread-safety: append, forEachCall, extentWidth and extentHeight are safe from any
// thread at once. place, forEachGridTile and remove must not run beside any other call.
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

    // Spools one call: `width` x `height` level pixels covering `slideRect` at level 0.
    // Plane p's top-left sample is at planeData[p], and its rows are rowStrides[p] bytes
    // apart; both arrays have format.planes entries. An empty call is a no-op. Throws
    // InternalError for a negative size, and IoError naming the file when the append
    // fails.
    void append(const Rect64& slideRect, int64_t width, int64_t height, const void* const* planeData,
                const int64_t* rowStrides);

    // The format the spool was created with.
    const RasterFormat& format() const noexcept;

    // Calls fn(slideRect, width, height) for every spooled call, in append order, so the
    // writer can check each against the final scale. Throws what fn throws.
    void forEachCall(const std::function<void(const Rect64& slideRect, int64_t width, int64_t height)>& fn) const;

    // Fixes every call's origin in level pixels: llround(slideRect.x / scaleX), and
    // likewise for y, and the extent from them. May be called again with another scale.
    // Throws InternalError for a scale that is not positive, a negative origin, or an
    // extent beyond 2^31-1 (the writer validates first).
    void place(double scaleX, double scaleY);

    // The largest originX + width (originY + height) of the placed calls; 0 before
    // place() or any append.
    int64_t extentWidth() const;
    int64_t extentHeight() const;

    // Calls fn(gridX, gridY, planes) once for every grid tile that a placed call touches,
    // in row-major order (row by row, columns ascending). `planes` holds one vector per
    // plane of 512 x 512 samples, row-major: zero where no call reached, otherwise the
    // calls applied in append order. The vectors are reused between calls. Throws
    // InternalError before place() or when a piece does not decode to its size, IoError
    // naming the file when it cannot be read back, and whatever fn throws.
    void forEachGridTile(
        const std::function<void(int32_t gridX, int32_t gridY, const std::vector<std::vector<uint8_t>>& planes)>&
            fn) const;

    // Closes the file and deletes it. A file already gone is not an error. Throws IoError
    // naming the file when it cannot be deleted. Further appends are an InternalError.
    void remove();

    // Test seams. piecesStored: the pieces spooled so far, over every plane of every call.
    // piecesDecoded: how many times forEachGridTile has started decoding a piece since
    // the spool was created, counting a piece again each time a tile needs it, and
    // counting runs that threw part way; never reset.
    int64_t piecesStored() const;
    int64_t piecesDecoded() const;

private:
    // Decodes rows [0, rowEnd) of `piece`, `pieceWidth` samples wide, and copies columns
    // [columnBegin, columnEnd) of rows [rowBegin, rowEnd) into `target` at (targetX,
    // targetY + row - rowBegin), a 512 x 512 tile. Caller holds mutex_.
    void decodeInto(const SpoolPiece& piece, int64_t pieceWidth, int64_t rowBegin, int64_t rowEnd,
                    int64_t columnBegin, int64_t columnEnd, std::vector<uint8_t>& target, int64_t targetX,
                    int64_t targetY, std::vector<uint8_t>& row, std::vector<uint8_t>& chunk) const;

    std::filesystem::path path_;
    RasterFormat format_;
    int32_t sampleBytes_ = 0;
    mutable std::mutex mutex_;  // guards everything below
    mutable std::FILE* file_ = nullptr;
    int64_t end_ = 0;           // bytes written so far
    bool placed_ = false;
    int64_t extentWidth_ = 0;
    int64_t extentHeight_ = 0;
    std::vector<SpoolEntry> entries_;  // in append order
    std::vector<SpoolPiece> pieces_;   // every entry's pieces, in order
    mutable int64_t piecesDecoded_ = 0;  // see piecesDecoded()
};

} // namespace slideio::processor::detail
