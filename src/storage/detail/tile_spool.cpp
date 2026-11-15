#include "storage/detail/tile_spool.hpp"

#include <slideio/processor/core/error.hpp>

#include <zlib.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <new>
#include <numeric>
#include <string>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <share.h>
#endif

namespace slideio::processor::detail {

namespace {

constexpr int kSpoolLevel = 1;               // zlib level of spooled fragments (ruling S46)
constexpr size_t kOutputChunk = 64 * 1024;   // deflate output grows by this much
constexpr size_t kInputChunk = size_t(1) << 30; // inflate input is fed at most this much at once

std::string errnoText(int error) { return std::error_code(error, std::generic_category()).message(); }

[[noreturn]] void throwIo(const std::string& message, const std::string& cause) {
    ErrorContext context;
    context.cause = cause.empty() ? nullptr : cause.c_str();
    throw IoError(message.c_str(), context);
}

#ifdef _WIN32
int seekFile(std::FILE* file, int64_t offset) { return _fseeki64(file, offset, SEEK_SET); }
#else
// Unverified here: the POSIX side is compiled on Linux and macOS only.
int seekFile(std::FILE* file, int64_t offset) { return fseeko(file, static_cast<off_t>(offset), SEEK_SET); }
#endif

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

// Compresses the rectangle (x, y, width, height) of a block, every plane, into one zlib
// stream: plane after plane, rows packed. `packed` is scratch.
std::vector<uint8_t> compressFragment(const void* const* planeData, const int64_t* rowStrides, int32_t planes,
                                      int32_t sampleBytes, int64_t x, int64_t y, int64_t width, int64_t height,
                                      std::vector<uint8_t>& packed) {
    const size_t rowBytes = static_cast<size_t>(width) * sampleBytes;
    packed.resize(rowBytes * static_cast<size_t>(height));
    std::vector<uint8_t> out;
    size_t used = 0;
    ZStream zs(true);
    for (int32_t p = 0; p < planes; ++p) {
        const auto* base = static_cast<const uint8_t*>(planeData[p]);
        for (int64_t row = 0; row < height; ++row)
            std::memcpy(packed.data() + static_cast<size_t>(row) * rowBytes,
                        base + (y + row) * rowStrides[p] + x * sampleBytes, rowBytes);
        const int flush = p + 1 == planes ? Z_FINISH : Z_NO_FLUSH;
        zs.stream.next_in = packed.data();
        zs.stream.avail_in = static_cast<uInt>(packed.size()); // at most 512 x 512 x 8 bytes
        for (;;) {
            if (out.size() - used < kOutputChunk) out.resize(used + kOutputChunk);
            const auto room = static_cast<uInt>(out.size() - used);
            zs.stream.next_out = out.data() + used;
            zs.stream.avail_out = room;
            const int status = deflate(&zs.stream, flush);
            if (status == Z_STREAM_ERROR) throwZlib("deflate", status);
            used += room - zs.stream.avail_out;
            if (flush == Z_FINISH ? status == Z_STREAM_END : zs.stream.avail_in == 0) break;
        }
    }
    out.resize(used);
    return out;
}

// The fragment as spooled: its index entry and its compressed bytes.
struct Fragment {
    SpoolEntry entry;
    std::vector<uint8_t> bytes;
};

} // namespace

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

void TileSpool::append(int64_t originX, int64_t originY, int64_t width, int64_t height, const void* const* planeData,
                       const int64_t* rowStrides) {
    if (originX < 0 || originY < 0 || width < 0 || height < 0)
        throw InternalError("TileSpool::append: negative origin or size");
    constexpr int64_t kMaxExtent = std::numeric_limits<int32_t>::max();
    if (width > kMaxExtent - originX || height > kMaxExtent - originY)
        throw InternalError("TileSpool::append: extent beyond 2^31-1");
    if (width == 0 || height == 0) return;

    // Cut along the grid and compress, outside the lock.
    std::vector<Fragment> fragments;
    std::vector<uint8_t> packed;
    const int64_t endX = originX + width, endY = originY + height;
    for (int64_t gy = originY / kRasterTileSize; gy * kRasterTileSize < endY; ++gy) {
        for (int64_t gx = originX / kRasterTileSize; gx * kRasterTileSize < endX; ++gx) {
            const int64_t x0 = std::max(originX, gx * kRasterTileSize);
            const int64_t y0 = std::max(originY, gy * kRasterTileSize);
            const int64_t x1 = std::min(endX, (gx + 1) * kRasterTileSize);
            const int64_t y1 = std::min(endY, (gy + 1) * kRasterTileSize);
            Fragment fragment;
            fragment.entry.gridX = static_cast<int32_t>(gx);
            fragment.entry.gridY = static_cast<int32_t>(gy);
            fragment.entry.x = static_cast<int32_t>(x0 - gx * kRasterTileSize);
            fragment.entry.y = static_cast<int32_t>(y0 - gy * kRasterTileSize);
            fragment.entry.width = static_cast<int32_t>(x1 - x0);
            fragment.entry.height = static_cast<int32_t>(y1 - y0);
            fragment.bytes = compressFragment(planeData, rowStrides, format_.planes, sampleBytes_, x0 - originX,
                                              y0 - originY, x1 - x0, y1 - y0, packed);
            fragment.entry.bytes = static_cast<int64_t>(fragment.bytes.size());
            fragments.push_back(std::move(fragment));
        }
    }

    // Append under the lock: the block's fragments are adjacent in the index, so a block is
    // all earlier or all later than any other.
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_ == nullptr) throw InternalError("TileSpool::append: the spool was removed");
    if (seekFile(file_, end_) != 0) throwIo("cannot seek in spool '" + path_.u8string() + "'", errnoText(errno));
    int64_t offset = end_;
    for (Fragment& fragment : fragments) {
        errno = 0;
        if (std::fwrite(fragment.bytes.data(), 1, fragment.bytes.size(), file_) != fragment.bytes.size()) {
            const int error = errno != 0 ? errno : EIO;
            throwIo("cannot write spool '" + path_.u8string() + "'", errnoText(error));
        }
        fragment.entry.offset = offset;
        offset += fragment.entry.bytes;
    }
    entries_.reserve(entries_.size() + fragments.size());
    for (const Fragment& fragment : fragments) entries_.push_back(fragment.entry);
    end_ = offset;
    extentWidth_ = std::max(extentWidth_, endX);
    extentHeight_ = std::max(extentHeight_, endY);
}

int64_t TileSpool::extentWidth() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return extentWidth_;
}

int64_t TileSpool::extentHeight() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return extentHeight_;
}

void TileSpool::forEachGridTile(
    const std::function<void(int32_t gridX, int32_t gridY, const std::vector<std::vector<uint8_t>>& planes)>& fn)
    const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_ == nullptr) throw InternalError("TileSpool::forEachGridTile: the spool was removed");

    // Row-major tile order; within a tile, append order (a stable sort keeps it).
    std::vector<size_t> order(entries_.size());
    std::iota(order.begin(), order.end(), size_t(0));
    std::stable_sort(order.begin(), order.end(), [this](size_t a, size_t b) {
        const SpoolEntry& ea = entries_[a];
        const SpoolEntry& eb = entries_[b];
        return ea.gridY != eb.gridY ? ea.gridY < eb.gridY : ea.gridX < eb.gridX;
    });

    const size_t tileRowBytes = static_cast<size_t>(kRasterTileSize) * sampleBytes_;
    std::vector<std::vector<uint8_t>> planes(static_cast<size_t>(format_.planes),
                                             std::vector<uint8_t>(tileRowBytes * kRasterTileSize));
    std::vector<uint8_t> compressed;
    std::vector<uint8_t> decoded;
    const std::string where = "spool '" + path_.u8string() + "'";

    size_t next = 0;
    while (next < order.size()) {
        const SpoolEntry& first = entries_[order[next]];
        for (std::vector<uint8_t>& plane : planes) std::fill(plane.begin(), plane.end(), uint8_t(0));
        for (; next < order.size(); ++next) {
            const SpoolEntry& entry = entries_[order[next]];
            if (entry.gridX != first.gridX || entry.gridY != first.gridY) break;

            compressed.resize(static_cast<size_t>(entry.bytes));
            errno = 0;
            if (seekFile(file_, entry.offset) != 0 ||
                std::fread(compressed.data(), 1, compressed.size(), file_) != compressed.size()) {
                const int error = errno != 0 ? errno : EIO;
                throwIo("cannot read " + where, errnoText(error));
            }

            const size_t rowBytes = static_cast<size_t>(entry.width) * sampleBytes_;
            decoded.resize(rowBytes * static_cast<size_t>(entry.height));
            ZStream zs(false);
            size_t fed = 0;
            int status = Z_OK;
            for (std::vector<uint8_t>& plane : planes) {
                zs.stream.next_out = decoded.data();
                zs.stream.avail_out = static_cast<uInt>(decoded.size());
                while (zs.stream.avail_out > 0) {
                    if (status == Z_STREAM_END) throw InternalError("TileSpool: a fragment decodes short");
                    if (zs.stream.avail_in == 0 && fed < compressed.size()) {
                        const size_t chunk = std::min(compressed.size() - fed, kInputChunk);
                        zs.stream.next_in = compressed.data() + fed;
                        zs.stream.avail_in = static_cast<uInt>(chunk);
                        fed += chunk;
                    }
                    status = inflate(&zs.stream, Z_NO_FLUSH);
                    if (status == Z_BUF_ERROR && zs.stream.avail_in == 0 && fed == compressed.size())
                        throw InternalError("TileSpool: a fragment decodes short");
                    if (status != Z_OK && status != Z_STREAM_END && status != Z_BUF_ERROR)
                        throwZlib("inflate", status);
                }
                for (int32_t row = 0; row < entry.height; ++row)
                    std::memcpy(plane.data() + static_cast<size_t>(entry.y + row) * tileRowBytes +
                                    static_cast<size_t>(entry.x) * sampleBytes_,
                                decoded.data() + static_cast<size_t>(row) * rowBytes, rowBytes);
            }
            if (status != Z_STREAM_END) {
                // Everything was decoded; only the stream's end may remain.
                uint8_t spare = 0;
                zs.stream.next_out = &spare;
                zs.stream.avail_out = 1;
                if (inflate(&zs.stream, Z_FINISH) != Z_STREAM_END || zs.stream.avail_out != 1)
                    throw InternalError("TileSpool: a fragment decodes long");
            }
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
