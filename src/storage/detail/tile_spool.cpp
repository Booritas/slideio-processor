#include "storage/detail/tile_spool.hpp"

#include "storage/detail/file_io.hpp"

#include <slideio/processor/core/error.hpp>

#include <zlib.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <share.h>
#endif

namespace slideio::processor::detail {

namespace {

constexpr int kSpoolLevel = 1;                  // zlib level of spooled calls (ruling S46)
constexpr size_t kOutputChunk = 64 * 1024;      // deflate output grows by this much
constexpr size_t kReadChunk = 64 * 1024;        // compressed bytes are read back this much at a time
constexpr size_t kMaxPiece = size_t(1) << 30;   // zlib's uInt counts are fed at most this much at once
constexpr int64_t kMaxExtent = std::numeric_limits<int32_t>::max();

// Read and write share the file, so every switch of direction goes through a seek, as
// ISO C requires; both directions here always seek first.
std::FILE* openSpool(const std::filesystem::path& path) {
#ifdef _WIN32
    return _wfsopen(path.c_str(), L"w+b", _SH_DENYNO);
#else
    return std::fopen(path.c_str(), "w+b");
#endif
}

// Throws for a zlib status that is neither success nor an expected end.
[[noreturn]] void throwZlib(const char* operation, int status) {
    if (status == Z_MEM_ERROR) throw std::bad_alloc();
    throw InternalError((std::string("TileSpool: ") + operation + " failed with zlib status " +
                         std::to_string(status))
                            .c_str());
}

// Owns a z_stream for deflate or inflate.
class ZStream {
public:
    explicit ZStream(bool deflating) : deflating_(deflating) {
        const int status = deflating ? deflateInit(&stream, kSpoolLevel) : inflateInit(&stream);
        if (status != Z_OK) throwZlib(deflating ? "deflateInit" : "inflateInit", status);
    }
    ~ZStream() {
        if (deflating_)
            deflateEnd(&stream);
        else
            inflateEnd(&stream);
    }
    ZStream(const ZStream&) = delete;
    ZStream& operator=(const ZStream&) = delete;

    z_stream stream{};

private:
    bool deflating_;
};

// Appends the `height` rows of `rowBytes` bytes at `base`, `stride` bytes apart, to `out`
// as one zlib stream, row by row, so no packed copy of the plane is made. Returns the
// stream's size.
int64_t compressPlane(const uint8_t* base, int64_t stride, size_t rowBytes, int64_t height, std::vector<uint8_t>& out) {
    ZStream zs(true);
    const size_t start = out.size();
    size_t used = start;
    for (int64_t row = 0; row < height; ++row) {
        const uint8_t* in = base + row * stride;
        size_t left = rowBytes;
        do {
            const size_t piece = std::min(left, kMaxPiece);
            zs.stream.next_in = const_cast<Bytef*>(in);
            zs.stream.avail_in = static_cast<uInt>(piece);
            in += piece;
            left -= piece;
            const int flush = row + 1 == height && left == 0 ? Z_FINISH : Z_NO_FLUSH;
            for (;;) {
                if (out.size() - used < kOutputChunk) out.resize(used + kOutputChunk);
                const size_t room = std::min(out.size() - used, kMaxPiece);
                zs.stream.next_out = out.data() + used;
                zs.stream.avail_out = static_cast<uInt>(room);
                const int status = deflate(&zs.stream, flush);
                if (status == Z_STREAM_ERROR) throwZlib("deflate", status);
                used += room - zs.stream.avail_out;
                if (flush == Z_FINISH ? status == Z_STREAM_END : zs.stream.avail_in == 0) break;
            }
        } while (left > 0);
    }
    out.resize(used);
    return static_cast<int64_t>(used - start);
}

// One grid tile a placed call touches.
struct Touch {
    int32_t gridX;
    int32_t gridY;
    size_t entry;
};

} // namespace

double levelOrigin(int64_t coordinate, double scale) noexcept {
    return std::round(static_cast<double>(coordinate) / scale);
}

TileSpool::TileSpool(std::filesystem::path spoolFile, RasterFormat format)
    : path_(std::move(spoolFile)), format_(format), sampleBytes_(dataTypeSizeBytes(format.type)) {
    if (sampleBytes_ <= 0 || format_.planes < 1) throw InternalError("TileSpool: not a raster format");
    errno = 0;
    file_ = openSpool(path_);
    if (file_ == nullptr) {
        const int error = errno;
        throwIo("cannot create spool '" + path_.u8string() + "'",
                error != 0 ? errnoText(error) : std::string("the file cannot be opened"));
    }
}

TileSpool::~TileSpool() {
    if (file_ != nullptr) std::fclose(file_);
}

const RasterFormat& TileSpool::format() const noexcept { return format_; }

void TileSpool::append(const Rect64& slideRect, int64_t width, int64_t height, const void* const* planeData,
                       const int64_t* rowStrides) {
    if (width < 0 || height < 0) throw InternalError("TileSpool::append: negative size");
    if (width == 0 || height == 0) return;

    // Compressed outside the lock, plane by plane.
    const size_t rowBytes = static_cast<size_t>(width) * sampleBytes_;
    std::vector<uint8_t> bytes;
    std::vector<int64_t> sizes(static_cast<size_t>(format_.planes));
    for (int32_t p = 0; p < format_.planes; ++p)
        sizes[p] = compressPlane(static_cast<const uint8_t*>(planeData[p]), rowStrides[p], rowBytes, height, bytes);

    std::lock_guard<std::mutex> lock(mutex_);
    if (file_ == nullptr) throw InternalError("TileSpool::append: the spool was removed");
    if (seekFile(file_, end_, SEEK_SET) != 0) throwIo("cannot seek in spool '" + path_.u8string() + "'", errnoText(errno));
    errno = 0;
    if (std::fwrite(bytes.data(), 1, bytes.size(), file_) != bytes.size()) {
        const int error = errno != 0 ? errno : EIO;
        throwIo("cannot write spool '" + path_.u8string() + "'", errnoText(error));
    }
    SpoolEntry entry;
    entry.slideRect = slideRect;
    entry.width = width;
    entry.height = height;
    entry.offset = end_;
    entry.firstPlane = planeBytes_.size();
    entries_.reserve(entries_.size() + 1);
    planeBytes_.insert(planeBytes_.end(), sizes.begin(), sizes.end());
    entries_.push_back(entry);
    end_ += static_cast<int64_t>(bytes.size());
    placed_ = false;  // a new call has no origin yet
}

void TileSpool::forEachCall(const std::function<void(const Rect64& slideRect, int64_t width, int64_t height)>& fn) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const SpoolEntry& entry : entries_) fn(entry.slideRect, entry.width, entry.height);
}

void TileSpool::place(double scaleX, double scaleY) {
    if (!(scaleX > 0.0) || !(scaleY > 0.0) || !std::isfinite(scaleX) || !std::isfinite(scaleY))
        throw InternalError("TileSpool::place: the scale is not positive");
    std::lock_guard<std::mutex> lock(mutex_);
    placed_ = false;
    int64_t extentWidth = 0, extentHeight = 0;
    for (SpoolEntry& entry : entries_) {
        const double x = levelOrigin(entry.slideRect.x, scaleX);
        const double y = levelOrigin(entry.slideRect.y, scaleY);
        if (x < 0 || y < 0) throw InternalError("TileSpool::place: a negative origin");
        if (x + static_cast<double>(entry.width) > static_cast<double>(kMaxExtent) ||
            y + static_cast<double>(entry.height) > static_cast<double>(kMaxExtent))
            throw InternalError("TileSpool::place: an extent beyond 2^31-1");
        entry.originX = static_cast<int64_t>(x);
        entry.originY = static_cast<int64_t>(y);
        extentWidth = std::max(extentWidth, entry.originX + entry.width);
        extentHeight = std::max(extentHeight, entry.originY + entry.height);
    }
    extentWidth_ = extentWidth;
    extentHeight_ = extentHeight;
    placed_ = true;
}

int64_t TileSpool::extentWidth() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return placed_ ? extentWidth_ : 0;
}

int64_t TileSpool::extentHeight() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return placed_ ? extentHeight_ : 0;
}

void TileSpool::decodeInto(const SpoolEntry& entry, int32_t plane, int64_t rowBegin, int64_t rowEnd,
                           int64_t columnBegin, int64_t columnEnd, std::vector<uint8_t>& target, int64_t targetX,
                           int64_t targetY, std::vector<uint8_t>& row, std::vector<uint8_t>& chunk) const {
    int64_t offset = entry.offset;
    for (int32_t p = 0; p < plane; ++p) offset += planeBytes_[entry.firstPlane + p];
    int64_t remaining = planeBytes_[entry.firstPlane + plane];
    const std::string where = "spool '" + path_.u8string() + "'";
    if (seekFile(file_, offset, SEEK_SET) != 0) throwIo("cannot read " + where, errnoText(errno));

    const size_t rowBytes = static_cast<size_t>(entry.width) * sampleBytes_;
    const size_t copyBytes = static_cast<size_t>(columnEnd - columnBegin) * sampleBytes_;
    row.resize(rowBytes);
    chunk.resize(kReadChunk);
    ZStream zs(false);
    int status = Z_OK;
    // Rows are decoded in order up to the last one the tile needs; the stream's rest is
    // never read.
    for (int64_t r = 0; r < rowEnd; ++r) {
        size_t filled = 0;
        while (filled < rowBytes) {
            if (status == Z_STREAM_END) throw InternalError("TileSpool: a call decodes short");
            if (zs.stream.avail_in == 0) {
                if (remaining == 0) throw InternalError("TileSpool: a call decodes short");
                const size_t n = static_cast<size_t>(std::min<int64_t>(remaining, static_cast<int64_t>(chunk.size())));
                errno = 0;
                if (std::fread(chunk.data(), 1, n, file_) != n) {
                    const int error = errno != 0 ? errno : EIO;
                    throwIo("cannot read " + where, errnoText(error));
                }
                zs.stream.next_in = chunk.data();
                zs.stream.avail_in = static_cast<uInt>(n);
                remaining -= static_cast<int64_t>(n);
            }
            const size_t room = std::min(rowBytes - filled, kMaxPiece);
            zs.stream.next_out = row.data() + filled;
            zs.stream.avail_out = static_cast<uInt>(room);
            status = inflate(&zs.stream, Z_NO_FLUSH);
            if (status != Z_OK && status != Z_STREAM_END && status != Z_BUF_ERROR) throwZlib("inflate", status);
            filled += room - zs.stream.avail_out;
        }
        if (r >= rowBegin) {
            std::memcpy(target.data() + (static_cast<size_t>(targetY + r - rowBegin) * kRasterTileSize +
                                         static_cast<size_t>(targetX)) *
                                            sampleBytes_,
                        row.data() + static_cast<size_t>(columnBegin) * sampleBytes_, copyBytes);
        }
    }
}

void TileSpool::forEachGridTile(
    const std::function<void(int32_t gridX, int32_t gridY, const std::vector<std::vector<uint8_t>>& planes)>& fn)
    const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_ == nullptr) throw InternalError("TileSpool::forEachGridTile: the spool was removed");
    if (!placed_) throw InternalError("TileSpool::forEachGridTile: the spool is not placed");

    // Every (grid tile, call) pair, in row-major tile order; within a tile, append order
    // (calls are listed in append order and the sort is stable).
    std::vector<Touch> touches;
    for (size_t i = 0; i < entries_.size(); ++i) {
        const SpoolEntry& e = entries_[i];
        for (int64_t gy = e.originY / kRasterTileSize; gy * kRasterTileSize < e.originY + e.height; ++gy)
            for (int64_t gx = e.originX / kRasterTileSize; gx * kRasterTileSize < e.originX + e.width; ++gx)
                touches.push_back({static_cast<int32_t>(gx), static_cast<int32_t>(gy), i});
    }
    std::stable_sort(touches.begin(), touches.end(), [](const Touch& a, const Touch& b) {
        return a.gridY != b.gridY ? a.gridY < b.gridY : a.gridX < b.gridX;
    });

    const size_t tileBytes = static_cast<size_t>(kRasterTileSize * kRasterTileSize) * sampleBytes_;
    std::vector<std::vector<uint8_t>> planes(static_cast<size_t>(format_.planes), std::vector<uint8_t>(tileBytes));
    std::vector<uint8_t> row;
    std::vector<uint8_t> chunk;
    size_t next = 0;
    while (next < touches.size()) {
        const Touch first = touches[next];
        for (std::vector<uint8_t>& plane : planes) std::fill(plane.begin(), plane.end(), uint8_t(0));
        const int64_t tileX = int64_t(first.gridX) * kRasterTileSize;
        const int64_t tileY = int64_t(first.gridY) * kRasterTileSize;
        for (; next < touches.size(); ++next) {
            const Touch& touch = touches[next];
            if (touch.gridX != first.gridX || touch.gridY != first.gridY) break;
            const SpoolEntry& e = entries_[touch.entry];
            const int64_t x0 = std::max(e.originX, tileX), x1 = std::min(e.originX + e.width, tileX + kRasterTileSize);
            const int64_t y0 = std::max(e.originY, tileY), y1 = std::min(e.originY + e.height, tileY + kRasterTileSize);
            for (int32_t p = 0; p < format_.planes; ++p)
                decodeInto(e, p, y0 - e.originY, y1 - e.originY, x0 - e.originX, x1 - e.originX, planes[p],
                           x0 - tileX, y0 - tileY, row, chunk);
        }
        fn(first.gridX, first.gridY, planes);
    }
}

void TileSpool::remove() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }
    std::error_code error;
    std::filesystem::remove(path_, error);
    if (error) throwIo("cannot delete spool '" + path_.u8string() + "'", error.message());
}

} // namespace slideio::processor::detail
