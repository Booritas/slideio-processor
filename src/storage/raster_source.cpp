#include "storage/detail/errors.hpp"
#include "storage/detail/tiff_raster_reader.hpp"

#include <slideio/processor/core/block_view.hpp>
#include <slideio/processor/core/data_type.hpp>
#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/core/geometry.hpp>

#include <cstring>
#include <memory>
#include <string>
#include <utility>

namespace slideio::processor::detail {

namespace {

constexpr const char* kFillTile = "TileRasterSource::fillTile";

// Refuses a view a raster source cannot fill (ruling S50): it must hold exactly the
// declared planes, of the declared type, and have a derivable scale.
void validateView(const BlockView& out, const RasterFormat& format) {
    if (out.isNull()) throwInvalid(kFillTile, "the view is null");
    if (out.planeCount() != format.planes) {
        throwInvalid(kFillTile, "the view has " + std::to_string(out.planeCount()) + " planes; the raster has " +
                                    std::to_string(format.planes));
    }
    for (int32_t p = 0; p < format.planes; ++p) {
        if (out.planeType(p) != format.type) {
            throwInvalid(kFillTile, "plane " + std::to_string(p) + " of the view is " +
                                        dataTypeName(out.planeType(p)) + "; the raster's planes are " +
                                        dataTypeName(format.type));
        }
    }
    const Rect64 rect = out.slideRect();
    if (out.width() <= 0 || out.height() <= 0 || rect.width <= 0 || rect.height <= 0) {
        throwInvalid(kFillTile, "the view has no derivable scale: it is " + std::to_string(out.width()) + " x " +
                                    std::to_string(out.height()) + " pixels over " + std::to_string(rect.width) +
                                    " x " + std::to_string(rect.height) + " level-0 pixels");
    }
}

// A populated raster: its file, read through the reader bound to it.
class FileRasterSource final : public TileRasterSource {
public:
    FileRasterSource(std::shared_ptr<TiffRasterReader> reader, RasterFormat format)
        : reader_(std::move(reader)), format_(format) {}

    void fillTile(BlockView& out) override {
        validateView(out, format_);
        reader_->fill(out);
    }

    DataType planeType() const override { return format_.type; }

    int32_t planeCount() const override { return format_.planes; }

private:
    std::shared_ptr<TiffRasterReader> reader_;
    RasterFormat format_;
};

// An unpopulated raster reads as zero.
class ZeroRasterSource final : public TileRasterSource {
public:
    explicit ZeroRasterSource(RasterFormat format) : format_(format) {}

    void fillTile(BlockView& out) override {
        validateView(out, format_);
        const auto rowBytes = static_cast<size_t>(out.width() * dataTypeSizeBytes(format_.type));
        for (int32_t p = 0; p < format_.planes; ++p) {
            auto* base = static_cast<uint8_t*>(out.planeData(p));
            const int64_t stride = out.planeRowStrideBytes(p);
            for (int64_t y = 0; y < out.height(); ++y) std::memset(base + y * stride, 0, rowBytes);
        }
    }

    DataType planeType() const override { return format_.type; }

    int32_t planeCount() const override { return format_.planes; }

private:
    RasterFormat format_;
};

} // namespace

TileRasterSourcePtr makeFileRasterSource(std::shared_ptr<TiffRasterReader> reader, RasterFormat format) {
    return std::make_shared<FileRasterSource>(std::move(reader), format);
}

TileRasterSourcePtr makeZeroRasterSource(RasterFormat format) { return std::make_shared<ZeroRasterSource>(format); }

} // namespace slideio::processor::detail
