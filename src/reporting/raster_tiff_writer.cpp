// Requirements: UR-RES-7, UR-QA-3
#include <slideio/processor/reporting/raster_tiff_writer.hpp>

#include "core/detail/diagnostics.hpp"
#include "core/detail/utf8.hpp"
#include "reporting/detail/export_driver.hpp"
#include "storage/detail/pyramid_tiff_writer.hpp"
#include "storage/detail/tile_spool.hpp"

#include <slideio/processor/core/block_view.hpp>
#include <slideio/processor/core/data_type.hpp>
#include <slideio/processor/core/error.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace slideio::processor {
namespace {

constexpr const char* kProgressName = "raster export";
constexpr int64_t kMaxExtent = std::numeric_limits<int32_t>::max();

// Removes a file on every exit path unless released.
class FileGuard {
public:
    explicit FileGuard(std::filesystem::path path) : path_(std::move(path)) {}
    FileGuard(const FileGuard&) = delete;
    FileGuard& operator=(const FileGuard&) = delete;
    ~FileGuard() {
        if (armed_) {
            std::error_code ec;
            std::filesystem::remove(path_, ec);
        }
    }
    void release() noexcept { armed_ = false; }

private:
    std::filesystem::path path_;
    bool armed_ = true;
};

bool omeHolds(DataType type) {
    switch (type) {
        case DataType::Byte:
        case DataType::Int8:
        case DataType::UInt16:
        case DataType::Int16:
        case DataType::Int32:
        case DataType::Float32:
        case DataType::Float64:
            return true;
        default:
            return false;
    }
}

DataTypeSet omeTypes() {
    DataTypeSet types;
    for (int32_t i = 0; i < static_cast<int32_t>(DataType::Unknown); ++i) {
        if (omeHolds(static_cast<DataType>(i))) types.add(static_cast<DataType>(i));
    }
    return types;
}

// ceil(size / scale), exact: the quotient in doubles can land a hair above an exact integer
// (1100 / 1.1), so a result whose predecessor already covers the size is stepped down.
int64_t ceilDiv(int64_t size, double scale) {
    int64_t q = static_cast<int64_t>(std::ceil(static_cast<double>(size) / scale));
    while (q > 1 && static_cast<double>(q - 1) * scale >= static_cast<double>(size)) --q;
    return q;
}

} // namespace

class RasterTiffWriter::Impl {
public:
    std::filesystem::path path;
    RasterCompression compression = RasterCompression::Deflate;
    double micronsPerPixel = 0.0;
    std::string name;
};

RasterTiffWriter::RasterTiffWriter(const char* path) {
    if (path == nullptr || *path == '\0') throw InvalidArgumentError("RasterTiffWriter: the path is empty");
    impl_ = std::make_unique<Impl>();
    impl_->path = std::filesystem::u8path(path);
}

RasterTiffWriter::~RasterTiffWriter() = default;

RasterTiffWriter& RasterTiffWriter::compression(RasterCompression compression) {
    impl_->compression = compression;
    return *this;
}

RasterTiffWriter& RasterTiffWriter::resolution(double micronsPerPixel) {
    if (!(std::isfinite(micronsPerPixel) && micronsPerPixel >= 0))
        throw InvalidArgumentError("RasterTiffWriter::resolution: the pixel size must be finite and not negative");
    impl_->micronsPerPixel = micronsPerPixel;
    return *this;
}

RasterTiffWriter& RasterTiffWriter::name(const char* name) {
    std::string text = name == nullptr ? std::string() : std::string(name);
    if (!detail::isValidUtf8(text)) throw InvalidArgumentError("RasterTiffWriter::name: the name is not valid UTF-8");
    impl_->name = std::move(text);
    return *this;
}

ExportResult RasterTiffWriter::write(TileRasterSource& source, const Rect64& region, double scale,
                                     ProgressObserver* observer, const CancellationToken& token) {
    constexpr const char* who = "RasterTiffWriter::write";
    if (region.width <= 0 || region.height <= 0)
        throw InvalidArgumentError("RasterTiffWriter::write: the region is empty");
    if (!(std::isfinite(scale) && scale >= 1.0))
        throw InvalidArgumentError("RasterTiffWriter::write: the scale must be finite and at least 1");
    const int64_t outWidth = ceilDiv(region.width, scale);
    const int64_t outHeight = ceilDiv(region.height, scale);
    if (outWidth > kMaxExtent || outHeight > kMaxExtent)
        throw InvalidArgumentError("RasterTiffWriter::write: the output is too large");
    const DataType type = source.planeType();
    const int32_t planes = source.planeCount();
    if (!omeHolds(type))
        throw UnsupportedDataTypeError("RasterTiffWriter::write: the OME-TIFF layout does not take the source's type",
                                       type, omeTypes());
    if (planes < 1) throw InvalidArgumentError("RasterTiffWriter::write: the source has no planes");

    constexpr int64_t kTile = detail::kRasterTileSize;
    const int64_t tilesX = (outWidth + kTile - 1) / kTile;
    const int64_t tilesY = (outHeight + kTile - 1) / kTile;
    const int64_t total = tilesX * tilesY;
    const int64_t sampleBytes = dataTypeSizeBytes(type);

    std::filesystem::path partial = impl_->path;
    partial += ".partial";
    std::filesystem::path spoolPath = partial;
    spoolPath += ".spool";

    // Declared before the spool, so the spool's file is closed before it is removed.
    FileGuard spoolGuard(spoolPath);
    FileGuard partialGuard(partial);
    const detail::RasterFormat format{type, planes, impl_->compression};
    detail::TileSpool spool(spoolPath, format);

    std::vector<std::vector<uint8_t>> buffers(static_cast<size_t>(planes));
    std::vector<BlockPlane> viewPlanes(static_cast<size_t>(planes));
    const auto levelCoordinate = [&](int64_t origin, int64_t offset) {
        return origin + static_cast<int64_t>(std::llround(static_cast<double>(offset) * scale));
    };

    int64_t done = 0;
    for (int64_t ty = 0; ty < tilesY; ++ty) {
        for (int64_t tx = 0; tx < tilesX; ++tx) {
            if (token.isCancelled()) return ExportResult(done, 0, true);
            const int64_t ox = tx * kTile;
            const int64_t oy = ty * kTile;
            const int64_t w = std::min(kTile, outWidth - ox);
            const int64_t h = std::min(kTile, outHeight - oy);
            const int64_t stride = w * sampleBytes;
            // At most one tile of the widest sample (8 bytes). The bound is stated so the
            // compiler sees it too: GCC 14 otherwise takes the size to be possibly negative.
            const int64_t bytes = stride * h;
            if (bytes <= 0 || bytes > kTile * kTile * 8)
                throw InternalError("RasterTiffWriter: a tile's size is out of range");
            std::vector<const void*> data(static_cast<size_t>(planes));
            std::vector<int64_t> strides(static_cast<size_t>(planes), stride);
            for (int32_t p = 0; p < planes; ++p) {
                auto& buffer = buffers[static_cast<size_t>(p)];
                buffer.assign(static_cast<size_t>(bytes), 0);
                viewPlanes[static_cast<size_t>(p)] = BlockPlane{buffer.data(), type, stride, p};
                data[static_cast<size_t>(p)] = buffer.data();
            }
            const int64_t x0 = levelCoordinate(region.x, ox);
            const int64_t y0 = levelCoordinate(region.y, oy);
            const Rect64 slideRect{x0, y0, levelCoordinate(region.x, ox + w) - x0,
                                   levelCoordinate(region.y, oy + h) - y0};
            BlockView view(w, h, slideRect, 0, viewPlanes.data(), planes);
            source.fillTile(view);
            // Placed on the output grid: the spool's scale is 1.
            spool.append(Rect64{ox, oy, w, h}, w, h, data.data(), strides.data());
            ++done;
            if (observer != nullptr)
                observer->onProgress(kProgressName, static_cast<double>(done) / static_cast<double>(total), done,
                                     total);
        }
    }

    spool.place(1.0, 1.0);
    detail::PyramidMetadata metadata;
    metadata.name = impl_->name;
    metadata.micronsPerPixel = impl_->micronsPerPixel;
    detail::writePyramidTiff(partial, spool, format, metadata, detail::PyramidLayout::OmeSubIfds);
    spool.remove();

    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(partial, ec);
    if (ec) {
        const std::string cause = ec.message();
        ErrorContext context;
        context.cause = cause.c_str();
        const std::string message =
            detail::withPath(std::string(who) + ": reading the output file's size failed", partial.u8string());
        throw IoError(message.c_str(), context);
    }
    std::filesystem::rename(partial, impl_->path, ec);
    if (ec) detail::throwReplaceFailed(who, impl_->path, partial, ec);
    partialGuard.release();
    return ExportResult(done, static_cast<int64_t>(size), false);
}

} // namespace slideio::processor
