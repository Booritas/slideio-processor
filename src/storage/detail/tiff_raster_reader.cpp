#include "storage/detail/tiff_raster_reader.hpp"

#include "storage/detail/file_io.hpp"
#include "storage/detail/tiff_io.hpp"

#include <slideio/processor/core/data_type.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/geometry.hpp>

#include <tiffio.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace slideio::processor::detail {

namespace {

// The tolerance of the page choice (ruling S50): a view whose scale is a page's, give or
// take rounding, reads that page.
constexpr double kScaleTolerance = 1e-9;

// A run of view pixels, along one axis, that all fall in one TIFF tile of the page.
struct Run {
    int64_t tile = 0;   // the tile's index along the axis
    int64_t begin = 0;  // the first view pixel
    int64_t end = 0;    // one past the last
};

// The page pixel of each of `count` view pixels along one axis, -1 outside the page:
// floor((origin + (i + 0.5) * step) / pageScale). Nondecreasing where it is not -1.
std::vector<int64_t> pagePixels(int64_t origin, double step, int64_t count, double pageScale, int64_t pageSize) {
    std::vector<int64_t> pixels(static_cast<size_t>(count));
    for (int64_t i = 0; i < count; ++i) {
        const double centre = static_cast<double>(origin) + (static_cast<double>(i) + 0.5) * step;
        const double pixel = std::floor(centre / pageScale);
        pixels[static_cast<size_t>(i)] =
            pixel >= 0.0 && pixel < static_cast<double>(pageSize) ? static_cast<int64_t>(pixel) : -1;
    }
    return pixels;
}

// The runs of `pixels` (from pagePixels) inside the page, split at the tile boundaries.
// The pixels inside the page are contiguous, since the mapping is monotonic.
std::vector<Run> runsOf(const std::vector<int64_t>& pixels, int64_t tileSize) {
    std::vector<Run> runs;
    for (int64_t i = 0; i < static_cast<int64_t>(pixels.size()); ++i) {
        const int64_t pixel = pixels[static_cast<size_t>(i)];
        if (pixel < 0) continue;
        const int64_t tile = pixel / tileSize;
        if (!runs.empty() && runs.back().tile == tile && runs.back().end == i) {
            runs.back().end = i + 1;
        } else {
            runs.push_back(Run{tile, i, i + 1});
        }
    }
    return runs;
}

// Reads a TIFF field of type T, or throws IoError naming the file by its role, never its path, and the field.
template <class T>
T field(const TiffHandle& handle, uint32_t tag, const char* name, bool defaulted = false) {
    T value{};
    const int found = defaulted ? TIFFGetFieldDefaulted(handle.get(), tag, &value)
                                : TIFFGetField(handle.get(), tag, &value);
    if (found != 1) throwIo(std::string("TIFFGetField: the raster file has no ") + name, std::string());
    return value;
}

} // namespace

TiffRasterReader::TiffRasterReader(std::filesystem::path path, RasterFormat format, double scaleX, double scaleY,
                                   int64_t width, int64_t height)
    : path_(std::move(path)), pathUtf8_(path_.u8string()), format_(format), scaleX_(scaleX), scaleY_(scaleY) {
    std::unique_ptr<Handle> first = open();
    TIFF* tif = first->tiff.get();
    const auto mismatch = [&](const std::string& what) {
        throwIo("TiffRasterReader: the raster file is not the raster its row describes", what);
    };
    const tdir_t count = TIFFNumberOfDirectories(tif);
    if (count < 1) mismatch("it has no page");
    int64_t expectedWidth = width;
    int64_t expectedHeight = height;
    for (tdir_t k = 0; k < count; ++k) {
        if (TIFFSetDirectory(tif, k) != 1) first->tiff.fail("TIFFSetDirectory");
        const std::string page = "page " + std::to_string(k);
        RasterFilePage info;
        info.width = field<uint32_t>(first->tiff, TIFFTAG_IMAGEWIDTH, "ImageWidth");
        info.height = field<uint32_t>(first->tiff, TIFFTAG_IMAGELENGTH, "ImageLength");
        if (TIFFIsTiled(tif) == 0) mismatch(page + " is not tiled");
        info.tileWidth = field<uint32_t>(first->tiff, TIFFTAG_TILEWIDTH, "TileWidth");
        info.tileHeight = field<uint32_t>(first->tiff, TIFFTAG_TILELENGTH, "TileLength");
        if (info.width != expectedWidth || info.height != expectedHeight) {
            mismatch(page + " is " + std::to_string(info.width) + " x " + std::to_string(info.height) + ", not " +
                     std::to_string(expectedWidth) + " x " + std::to_string(expectedHeight));
        }
        if (info.tileWidth < 1 || info.tileHeight < 1) mismatch(page + " has an empty tile size");
        const auto bits = field<uint16_t>(first->tiff, TIFFTAG_BITSPERSAMPLE, "BitsPerSample", true);
        const auto sampleFormat = field<uint16_t>(first->tiff, TIFFTAG_SAMPLEFORMAT, "SampleFormat", true);
        const auto samples = field<uint16_t>(first->tiff, TIFFTAG_SAMPLESPERPIXEL, "SamplesPerPixel", true);
        const auto planar = field<uint16_t>(first->tiff, TIFFTAG_PLANARCONFIG, "PlanarConfiguration", true);
        if (bits != tiffBitsPerSample(format_.type) || sampleFormat != tiffSampleFormat(format_.type)) {
            mismatch(page + "'s samples are not " + std::string(dataTypeName(format_.type)));
        }
        if (samples != format_.planes || planar != PLANARCONFIG_SEPARATE) {
            mismatch(page + " does not hold " + std::to_string(format_.planes) + " separate planes");
        }
        pages_.push_back(info);
        first->page = k;
        expectedWidth = (expectedWidth + 1) / 2;
        expectedHeight = (expectedHeight + 1) / 2;
    }
    idle_.push_back(std::move(first));
    open_ = 1;
}

TiffRasterReader::~TiffRasterReader() = default;

void TiffRasterReader::fill(BlockView& out) {
    const int64_t width = out.width();
    const int64_t height = out.height();
    const Rect64 rect = out.slideRect();
    const int64_t sampleBytes = dataTypeSizeBytes(format_.type);
    for (int32_t p = 0; p < format_.planes; ++p) {
        auto* base = static_cast<uint8_t*>(out.planeData(p));
        const int64_t stride = out.planeRowStrideBytes(p);
        for (int64_t y = 0; y < height; ++y) std::memset(base + y * stride, 0, static_cast<size_t>(width * sampleBytes));
    }

    // The coarsest page no coarser than the view; page 0 for a view finer than it.
    const double stepX = static_cast<double>(rect.width) / static_cast<double>(width);
    const double stepY = static_cast<double>(rect.height) / static_cast<double>(height);
    const double limit = std::min(stepX, stepY) * (1.0 + kScaleTolerance);
    size_t k = 0;
    while (k + 1 < pages_.size() && scaleX_ * std::ldexp(1.0, static_cast<int>(k + 1)) <= limit) ++k;
    const RasterFilePage& page = pages_[k];
    const double factor = std::ldexp(1.0, static_cast<int>(k));

    const std::vector<int64_t> columns = pagePixels(rect.x, stepX, width, scaleX_ * factor, page.width);
    const std::vector<int64_t> rows = pagePixels(rect.y, stepY, height, scaleY_ * factor, page.height);
    const std::vector<Run> columnRuns = runsOf(columns, page.tileWidth);
    const std::vector<Run> rowRuns = runsOf(rows, page.tileHeight);
    if (columnRuns.empty() || rowRuns.empty()) return;  // wholly outside the page: zero

    std::unique_ptr<Handle> handle = acquire();
    try {
        TIFF* tif = handle->tiff.get();
        if (handle->page != static_cast<int64_t>(k)) {
            handle->page = -1;
            if (TIFFSetDirectory(tif, static_cast<tdir_t>(k)) != 1) handle->tiff.fail("TIFFSetDirectory");
            handle->page = static_cast<int64_t>(k);
        }
        const tmsize_t tileBytes = TIFFTileSize(tif);
        if (tileBytes <= 0 || tileBytes < page.tileWidth * page.tileHeight * sampleBytes) {
            handle->tiff.fail("TIFFTileSize");
        }
        handle->tile.resize(static_cast<size_t>(tileBytes));
        const int64_t tileRowBytes = page.tileWidth * sampleBytes;
        for (int32_t p = 0; p < format_.planes; ++p) {
            auto* base = static_cast<uint8_t*>(out.planeData(p));
            const int64_t stride = out.planeRowStrideBytes(p);
            for (const Run& rowRun : rowRuns) {
                for (const Run& columnRun : columnRuns) {
                    const uint32_t tileIndex =
                        TIFFComputeTile(tif, static_cast<uint32_t>(columnRun.tile * page.tileWidth),
                                        static_cast<uint32_t>(rowRun.tile * page.tileHeight), 0,
                                        static_cast<uint16_t>(p));
                    if (TIFFReadEncodedTile(tif, tileIndex, handle->tile.data(), tileBytes) < 0) {
                        handle->tiff.fail("TIFFReadEncodedTile");
                    }
                    const int64_t tileX = columnRun.tile * page.tileWidth;
                    const int64_t tileY = rowRun.tile * page.tileHeight;
                    for (int64_t y = rowRun.begin; y < rowRun.end; ++y) {
                        const uint8_t* source =
                            handle->tile.data() + (rows[static_cast<size_t>(y)] - tileY) * tileRowBytes;
                        uint8_t* target = base + y * stride;
                        for (int64_t x = columnRun.begin; x < columnRun.end; ++x) {
                            std::memcpy(target + x * sampleBytes,
                                        source + (columns[static_cast<size_t>(x)] - tileX) * sampleBytes,
                                        static_cast<size_t>(sampleBytes));
                        }
                    }
                }
            }
        }
    } catch (...) {
        discard(std::move(handle));
        throw;
    }
    release(std::move(handle));
}

std::unique_ptr<TiffRasterReader::Handle> TiffRasterReader::acquire() {
    std::unique_lock<std::mutex> lock(mutex_);
    bool wait = false;
    for (;;) {
        if (!idle_.empty()) {
            std::unique_ptr<Handle> handle = std::move(idle_.back());
            idle_.pop_back();
            return handle;
        }
        if (wait) {
            returned_.wait(lock);
            wait = false;
            continue;
        }
        ++open_;
        lock.unlock();
        try {
            return open();
        } catch (const IoError&) {
            lock.lock();
            --open_;
            if (open_ == 0) {
                // This was the last handle a waiter counted on: wake them, to try an open of
                // their own. Not while others are open, or two failing callers would wake
                // each other round and round.
                returned_.notify_all();
                throw;
            }
            wait = true;  // the file may be gone, but the open handles still read it
        } catch (...) {
            lock.lock();
            --open_;
            if (open_ == 0) returned_.notify_all();
            throw;
        }
    }
}

void TiffRasterReader::release(std::unique_ptr<Handle> handle) noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    try {
        idle_.push_back(std::move(handle));
    } catch (...) {
        --open_;  // no room to pool it: it closes on return
    }
    returned_.notify_one();
}

void TiffRasterReader::discard(std::unique_ptr<Handle> handle) noexcept {
    {
        std::lock_guard<std::mutex> guard(mutex_);
        --open_;
        returned_.notify_all();
    }
    handle.reset();
}

std::unique_ptr<TiffRasterReader::Handle> TiffRasterReader::open() const {
    if (openHook_) openHook_();
    auto handle = std::make_unique<Handle>();
    handle->tiff = TiffHandle::openRead(pathUtf8_);
    return handle;
}

} // namespace slideio::processor::detail
