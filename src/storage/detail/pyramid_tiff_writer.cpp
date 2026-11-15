#include "storage/detail/pyramid_tiff_writer.hpp"

#include "storage/detail/half_float.hpp"
#include "storage/detail/tiff_io.hpp"

#include <slideio/processor/core/error.hpp>

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <tiffio.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace slideio::processor::detail {

namespace {

constexpr uint32_t kTile = static_cast<uint32_t>(kRasterTileSize);
constexpr int kDeflateLevel = 6; // ruling S42

struct PageSize {
    uint32_t width;
    uint32_t height;
};

// Page 0 is the extent; each next page halves the one above, rounding up, until both
// dimensions are at most one tile (ruling S43).
std::vector<PageSize> pageSizes(int64_t width, int64_t height) {
    std::vector<PageSize> pages{{static_cast<uint32_t>(width), static_cast<uint32_t>(height)}};
    while (pages.back().width > kTile || pages.back().height > kTile) {
        const PageSize& above = pages.back();
        pages.push_back({(above.width + 1) / 2, (above.height + 1) / 2});
    }
    return pages;
}

uint16_t tiffCompression(RasterCompression compression) {
    switch (compression) {
    case RasterCompression::None:
        return COMPRESSION_NONE;
    case RasterCompression::Lzw:
        return COMPRESSION_LZW;
    case RasterCompression::Deflate:
        return COMPRESSION_ADOBE_DEFLATE;
    }
    throw InternalError("writePyramidTiff: not a raster compression");
}

// The Predictor tag for `format`, or 0 for none written (ruling S42): horizontal for
// integers, floating point for Float32 and Float64, and PREDICTOR_NONE for Float16, whose
// floating predictor GDAL does not read reliably.
uint16_t tiffPredictor(const RasterFormat& format) {
    if (format.compression == RasterCompression::None) return 0;
    if (!dataTypeIsFloat(format.type)) return PREDICTOR_HORIZONTAL;
    return format.type == DataType::Float16 ? PREDICTOR_NONE : PREDICTOR_FLOATINGPOINT;
}

// Page 0's ImageDescription (ruling S41).
std::string imageDescription(const PyramidMetadata& metadata, DataType type) {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    writer.StartObject();
    writer.Key("slideioProcessorRaster");
    writer.Int(1);
    writer.Key("name");
    writer.String(metadata.name.c_str(), static_cast<rapidjson::SizeType>(metadata.name.size()));
    writer.Key("description");
    writer.String(metadata.description.c_str(), static_cast<rapidjson::SizeType>(metadata.description.size()));
    writer.Key("dataType");
    writer.String(dataTypeName(type));
    writer.Key("level");
    writer.Int(metadata.level);
    writer.Key("scaleX");
    writer.Double(metadata.scaleX);
    writer.Key("scaleY");
    writer.Double(metadata.scaleY);
    writer.EndObject();
    return std::string(buffer.GetString(), buffer.GetSize());
}

void setTags(const TiffHandle& handle, const RasterFormat& format, const PageSize& size, int page,
             const std::string& description) {
    TIFF* tif = handle.get();
    const auto check = [&handle](int status) {
        if (status != 1) handle.fail("TIFFSetField");
    };
    if (page > 0) check(TIFFSetField(tif, TIFFTAG_SUBFILETYPE, static_cast<uint32_t>(FILETYPE_REDUCEDIMAGE)));
    check(TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, size.width));
    check(TIFFSetField(tif, TIFFTAG_IMAGELENGTH, size.height));
    check(TIFFSetField(tif, TIFFTAG_TILEWIDTH, kTile));
    check(TIFFSetField(tif, TIFFTAG_TILELENGTH, kTile));
    check(TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, tiffBitsPerSample(format.type)));
    check(TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, tiffSampleFormat(format.type)));
    check(TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, static_cast<uint16_t>(format.planes)));
    if (format.planes > 1) {
        const std::vector<uint16_t> extra(static_cast<size_t>(format.planes - 1), EXTRASAMPLE_UNSPECIFIED);
        check(TIFFSetField(tif, TIFFTAG_EXTRASAMPLES, static_cast<uint16_t>(extra.size()), extra.data()));
    }
    check(TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK));
    check(TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_SEPARATE));
    check(TIFFSetField(tif, TIFFTAG_COMPRESSION, tiffCompression(format.compression)));
    if (format.compression == RasterCompression::Deflate) check(TIFFSetField(tif, TIFFTAG_ZIPQUALITY, kDeflateLevel));
    const uint16_t predictor = tiffPredictor(format);
    if (predictor != 0) check(TIFFSetField(tif, TIFFTAG_PREDICTOR, predictor));
    if (page == 0) check(TIFFSetField(tif, TIFFTAG_IMAGEDESCRIPTION, description.c_str()));
}

// Writes one page's tiles. An all-zero tile is encoded once, its compressed bytes read
// back, and every later one written raw from them (ruling S41).
class PageTileWriter {
public:
    PageTileWriter(const TiffHandle& handle, size_t tileBytes) : handle_(handle), tileBytes_(tileBytes) {}

    // Writes `data` (one tile of one plane, tileBytes) as tile `index`. libtiff may
    // rewrite `data` in place.
    void write(uint32_t index, std::vector<uint8_t>& data) {
        const auto bytes = static_cast<tmsize_t>(tileBytes_);
        if (TIFFWriteEncodedTile(handle_.get(), index, data.data(), bytes) != bytes)
            handle_.fail("TIFFWriteEncodedTile");
    }

    void writeZero(uint32_t index) {
        TIFF* tif = handle_.get();
        if (zero_.empty()) {
            std::vector<uint8_t> zeros(tileBytes_, 0);
            write(index, zeros);
            uint64_t* counts = nullptr;
            if (TIFFGetField(tif, TIFFTAG_TILEBYTECOUNTS, &counts) != 1 || counts == nullptr)
                handle_.fail("TIFFGetField");
            zero_.resize(static_cast<size_t>(counts[index]));
            const auto size = static_cast<tmsize_t>(zero_.size());
            if (zero_.empty() || TIFFReadRawTile(tif, index, zero_.data(), size) != size) {
                zero_.clear();
                handle_.fail("TIFFReadRawTile");
            }
            return;
        }
        const auto size = static_cast<tmsize_t>(zero_.size());
        if (TIFFWriteRawTile(tif, index, zero_.data(), size) != size) handle_.fail("TIFFWriteRawTile");
    }

private:
    const TiffHandle& handle_;
    size_t tileBytes_;
    std::vector<uint8_t> zero_; // the compressed zero tile, once encoded
};

bool allZero(const std::vector<uint8_t>& data) {
    return std::all_of(data.begin(), data.end(), [](uint8_t b) { return b == 0; });
}

// Page 0: every grid tile with fragments from the spool, then zeros for the rest.
void writeBasePage(const TiffHandle& handle, const TileSpool& spool, const RasterFormat& format,
                   const PageSize& size, size_t tileBytes) {
    TIFF* tif = handle.get();
    const uint32_t across = (size.width + kTile - 1) / kTile;
    const uint32_t down = (size.height + kTile - 1) / kTile;
    std::vector<bool> written(static_cast<size_t>(across) * down, false);
    PageTileWriter writer(handle, tileBytes);
    std::vector<uint8_t> scratch(tileBytes);
    spool.forEachGridTile([&](int32_t gridX, int32_t gridY, const std::vector<std::vector<uint8_t>>& planes) {
        written[static_cast<size_t>(gridY) * across + gridX] = true;
        for (int32_t p = 0; p < format.planes; ++p) {
            const uint32_t index = TIFFComputeTile(tif, gridX * kTile, gridY * kTile, 0, static_cast<uint16_t>(p));
            if (allZero(planes[p])) {
                writer.writeZero(index);
            } else {
                // libtiff takes a mutable buffer; the spool's stays untouched.
                std::copy(planes[p].begin(), planes[p].end(), scratch.begin());
                writer.write(index, scratch);
            }
        }
    });
    for (uint32_t gy = 0; gy < down; ++gy)
        for (uint32_t gx = 0; gx < across; ++gx)
            if (!written[static_cast<size_t>(gy) * across + gx])
                for (int32_t p = 0; p < format.planes; ++p)
                    writer.writeZero(TIFFComputeTile(tif, gx * kTile, gy * kTile, 0, static_cast<uint16_t>(p)));
}

// Loads and stores one float sample as a double.
struct Half {
    static double load(const uint8_t* p) {
        uint16_t bits;
        std::memcpy(&bits, p, sizeof bits);
        return halfToFloat(bits);
    }
    static void store(double value, uint8_t* p) {
        const uint16_t bits = floatToHalf(static_cast<float>(value));
        std::memcpy(p, &bits, sizeof bits);
    }
};

template <typename T>
struct Float {
    static double load(const uint8_t* p) {
        T value;
        std::memcpy(&value, p, sizeof value);
        return value;
    }
    static void store(double value, uint8_t* p) {
        const auto narrowed = static_cast<T>(value);
        std::memcpy(p, &narrowed, sizeof narrowed);
    }
};

// The four source tiles of an output tile, [row * 2 + column]; null where the source page
// has no such tile.
using SourceTiles = std::array<const uint8_t*, 4>;

// The mean of the samples present in each 2x2 block, in double, for a float type.
template <typename Codec>
void reduceMean(const SourceTiles& src, uint8_t* out, uint32_t gx, uint32_t gy, const PageSize& size,
                const PageSize& above, size_t sampleBytes) {
    for (uint32_t y = 0; y < kTile; ++y) {
        const uint32_t pageY = gy * kTile + y;
        if (pageY >= size.height) break;
        for (uint32_t x = 0; x < kTile; ++x) {
            const uint32_t pageX = gx * kTile + x;
            if (pageX >= size.width) break;
            const uint8_t* tile = src[(2 * y / kTile) * 2 + (2 * x / kTile)];
            const uint32_t sx = (2 * x) % kTile, sy = (2 * y) % kTile; // even, so +1 stays in the tile
            const bool right = 2 * pageX + 1 < above.width, below = 2 * pageY + 1 < above.height;
            double sum = Codec::load(tile + (static_cast<size_t>(sy) * kTile + sx) * sampleBytes);
            int count = 1;
            if (right) {
                sum += Codec::load(tile + (static_cast<size_t>(sy) * kTile + sx + 1) * sampleBytes);
                ++count;
            }
            if (below) {
                sum += Codec::load(tile + (static_cast<size_t>(sy + 1) * kTile + sx) * sampleBytes);
                ++count;
            }
            if (right && below) {
                sum += Codec::load(tile + (static_cast<size_t>(sy + 1) * kTile + sx + 1) * sampleBytes);
                ++count;
            }
            Codec::store(sum / count, out + (static_cast<size_t>(y) * kTile + x) * sampleBytes);
        }
    }
}

// The top-left sample of each 2x2 block, for an integer type.
void reduceTopLeft(const SourceTiles& src, uint8_t* out, uint32_t gx, uint32_t gy, const PageSize& size,
                   size_t sampleBytes) {
    for (uint32_t y = 0; y < kTile && gy * kTile + y < size.height; ++y) {
        for (uint32_t x = 0; x < kTile && gx * kTile + x < size.width; ++x) {
            const uint8_t* tile = src[(2 * y / kTile) * 2 + (2 * x / kTile)];
            const size_t from = (static_cast<size_t>((2 * y) % kTile) * kTile + (2 * x) % kTile) * sampleBytes;
            std::memcpy(out + (static_cast<size_t>(y) * kTile + x) * sampleBytes, tile + from, sampleBytes);
        }
    }
}

// Fills output tile (gx, gy) of a page of `size` from the page above, of `above`.
// Integers take the top-left sample of each 2x2 block, which is always present; floats
// the mean of the samples present (ruling S43). Pixels outside the page are left as they
// are: the caller zeroes `out`.
void reduceTile(DataType type, const SourceTiles& src, uint8_t* out, uint32_t gx, uint32_t gy, const PageSize& size,
                const PageSize& above, size_t sampleBytes) {
    switch (type) {
    case DataType::Float16:
        return reduceMean<Half>(src, out, gx, gy, size, above, sampleBytes);
    case DataType::Float32:
        return reduceMean<Float<float>>(src, out, gx, gy, size, above, sampleBytes);
    case DataType::Float64:
        return reduceMean<Float<double>>(src, out, gx, gy, size, above, sampleBytes);
    default:
        return reduceTopLeft(src, out, gx, gy, size, sampleBytes);
    }
}

// Page k > 0, built from page k - 1 read back through `reader`.
void writeReducedPage(const TiffHandle& handle, const TiffHandle& reader, const RasterFormat& format,
                      const PageSize& size, const PageSize& above, size_t tileBytes) {
    TIFF* tif = handle.get();
    TIFF* source = reader.get();
    const size_t sampleBytes = static_cast<size_t>(dataTypeSizeBytes(format.type));
    const uint32_t across = (size.width + kTile - 1) / kTile;
    const uint32_t down = (size.height + kTile - 1) / kTile;
    const uint32_t aboveAcross = (above.width + kTile - 1) / kTile;
    const uint32_t aboveDown = (above.height + kTile - 1) / kTile;
    PageTileWriter writer(handle, tileBytes);
    std::array<std::vector<uint8_t>, 4> buffers;
    std::vector<uint8_t> out(tileBytes);
    for (uint32_t gy = 0; gy < down; ++gy) {
        for (uint32_t gx = 0; gx < across; ++gx) {
            for (int32_t p = 0; p < format.planes; ++p) {
                SourceTiles src{};
                for (uint32_t i = 0; i < 4; ++i) {
                    const uint32_t sx = 2 * gx + i % 2, sy = 2 * gy + i / 2;
                    if (sx >= aboveAcross || sy >= aboveDown) continue;
                    buffers[i].resize(tileBytes);
                    const auto bytes = static_cast<tmsize_t>(tileBytes);
                    const uint32_t index = TIFFComputeTile(source, sx * kTile, sy * kTile, 0, static_cast<uint16_t>(p));
                    if (TIFFReadEncodedTile(source, index, buffers[i].data(), bytes) != bytes)
                        reader.fail("TIFFReadEncodedTile");
                    src[i] = buffers[i].data();
                }
                std::fill(out.begin(), out.end(), uint8_t(0));
                reduceTile(format.type, src, out.data(), gx, gy, size, above, sampleBytes);
                const uint32_t index = TIFFComputeTile(tif, gx * kTile, gy * kTile, 0, static_cast<uint16_t>(p));
                if (allZero(out))
                    writer.writeZero(index);
                else
                    writer.write(index, out);
            }
        }
    }
}

} // namespace

PyramidInfo writePyramidTiff(const std::filesystem::path& tif, const TileSpool& spool, const RasterFormat& format,
                             const PyramidMetadata& metadata) {
    const RasterFormat& spooled = spool.format();
    if (format.type != spooled.type || format.planes != spooled.planes)
        throw InternalError("writePyramidTiff: the format is not the spool's");
    const int64_t width = spool.extentWidth();
    const int64_t height = spool.extentHeight();
    if (width <= 0 || height <= 0) throw InternalError("writePyramidTiff: the spool is empty");

    const std::string path = tif.u8string();
    const std::vector<PageSize> pages = pageSizes(width, height);
    const size_t tileBytes = static_cast<size_t>(kTile) * kTile * static_cast<size_t>(dataTypeSizeBytes(format.type));
    const std::string description = imageDescription(metadata, format.type);

    TiffHandle handle = TiffHandle::openWrite(path);
    for (size_t page = 0; page < pages.size(); ++page) {
        setTags(handle, format, pages[page], static_cast<int>(page), description);
        if (page == 0) {
            writeBasePage(handle, spool, format, pages[0], tileBytes);
        } else {
            // The page above is complete on disk: read it back through a second handle.
            TiffHandle reader = TiffHandle::openRead(path);
            if (TIFFSetDirectory(reader.get(), static_cast<tdir_t>(page - 1)) != 1) reader.fail("TIFFSetDirectory");
            writeReducedPage(handle, reader, format, pages[page], pages[page - 1], tileBytes);
            reader.close();
        }
        if (TIFFWriteDirectory(handle.get()) != 1) handle.fail("TIFFWriteDirectory");
        handle.flush();
    }
    handle.close();
    return PyramidInfo{static_cast<int32_t>(pages.size()), width, height};
}

} // namespace slideio::processor::detail
