// Requirements: UR-RES-7, UR-QA-3
#include <slideio/processor/masks/mask_writer.hpp>

#include <slideio/processor/core/block_view.hpp>
#include <slideio/processor/core/data_type.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/reporting/raster_tiff_writer.hpp>

#include "core/detail/utf8.hpp"

#include <slideio/slideio/scene.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <new>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace slideio::processor {

namespace {

// Runs a SlideIO call, translating what it throws: ProcessorError and std::bad_alloc pass
// through (a scene's own source may throw them), anything else becomes IoError with the
// SlideIO text as its cause.
template <typename Call>
auto fromSlideio(const char* message, Call&& call) -> decltype(call()) {
    try {
        return call();
    } catch (const ProcessorError&) {
        throw;
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& e) {
        ErrorContext context;
        context.cause = e.what();
        throw IoError(message, context);
    }
}

// A whole scene, z slice 0 and time frame 0, as a tile raster source in its own pixels:
// level-0 pixel (x, y) of a view is scene pixel (x, y). Each tile is one
// readResampledBlock of the part of the view inside the scene; the rest is zero.
class SceneRasterSource final : public TileRasterSource {
public:
    SceneRasterSource(std::shared_ptr<slideio::Scene> scene, DataType type, int32_t planes, int64_t width,
                      int64_t height)
        : scene_(std::move(scene)), type_(type), planes_(planes), width_(width), height_(height) {}

    DataType planeType() const override { return type_; }
    int32_t planeCount() const override { return planes_; }

    void fillTile(BlockView& out) override {
        if (out.isNull() || out.planeCount() != planes_) {
            throw InvalidArgumentError("MaskWriter: the view does not match the scene's channels");
        }
        for (int32_t p = 0; p < planes_; ++p) {
            if (out.planeType(p) != type_) {
                throw InvalidArgumentError("MaskWriter: the view does not match the scene's channels");
            }
        }
        const int64_t sample = dataTypeSizeBytes(type_);
        const int64_t outW = out.width();
        const int64_t outH = out.height();
        for (int32_t p = 0; p < planes_; ++p) {
            auto* base = static_cast<uint8_t*>(out.planeData(p));
            for (int64_t y = 0; y < outH; ++y) {
                std::memset(base + y * out.planeRowStrideBytes(p), 0, static_cast<size_t>(outW * sample));
            }
        }

        // The output columns and rows whose centres lie inside the scene, and the scene
        // pixels they span. At scale 1, as MaskWriter writes, the two coincide.
        const Rect64 r = out.slideRect();
        int64_t ox0 = 0, ox1 = 0, sx0 = 0, sx1 = 0;
        int64_t oy0 = 0, oy1 = 0, sy0 = 0, sy1 = 0;
        if (!span(r.x, r.width, outW, width_, ox0, ox1, sx0, sx1) ||
            !span(r.y, r.height, outH, height_, oy0, oy1, sy0, sy1)) {
            return;
        }
        const int64_t w = ox1 - ox0;
        const int64_t h = oy1 - oy0;
        std::vector<uint8_t> block(static_cast<size_t>(w * h * planes_ * sample));
        const std::tuple<int, int, int, int> rect(static_cast<int>(sx0), static_cast<int>(sy0),
                                                  static_cast<int>(sx1 - sx0), static_cast<int>(sy1 - sy0));
        const std::tuple<int, int> size(static_cast<int>(w), static_cast<int>(h));
        fromSlideio("MaskWriter: SlideIO failed to read the scene", [&] {
            scene_->readResampledBlock(rect, size, block.data(), block.size());
        });

        // SlideIO delivers channels interleaved; the view is planar.
        const size_t pixelBytes = static_cast<size_t>(planes_ * sample);
        for (int32_t p = 0; p < planes_; ++p) {
            auto* base = static_cast<uint8_t*>(out.planeData(p));
            for (int64_t y = 0; y < h; ++y) {
                uint8_t* to = base + (oy0 + y) * out.planeRowStrideBytes(p) + ox0 * sample;
                const uint8_t* from = block.data() + static_cast<size_t>(y * w) * pixelBytes +
                                      static_cast<size_t>(p * sample);
                for (int64_t x = 0; x < w; ++x) {
                    std::memcpy(to, from, static_cast<size_t>(sample));
                    to += sample;
                    from += pixelBytes;
                }
            }
        }
    }

private:
    // On one axis: output indices [o0, o1) of `count`, spanning `extent` scene pixels from
    // `origin`, whose centres lie in [0, limit), and the scene pixels [s0, s1) they cover.
    // False when none does.
    static bool span(int64_t origin, int64_t extent, int64_t count, int64_t limit, int64_t& o0, int64_t& o1,
                     int64_t& s0, int64_t& s1) {
        const double step = static_cast<double>(extent) / static_cast<double>(count);
        const auto centre = [&](int64_t i) { return static_cast<double>(origin) + (static_cast<double>(i) + 0.5) * step; };
        o0 = 0;
        while (o0 < count && centre(o0) < 0.0) ++o0;
        o1 = o0;
        while (o1 < count && centre(o1) < static_cast<double>(limit)) ++o1;
        if (o1 <= o0) return false;
        s0 = std::clamp<int64_t>(static_cast<int64_t>(std::floor(static_cast<double>(origin) + o0 * step)), 0, limit);
        s1 = std::clamp<int64_t>(static_cast<int64_t>(std::ceil(static_cast<double>(origin) + o1 * step)), 0, limit);
        return s1 > s0;
    }

    std::shared_ptr<slideio::Scene> scene_;
    DataType type_;
    int32_t planes_;
    int64_t width_;
    int64_t height_;
};

} // namespace

class MaskWriter::Impl {
public:
    std::string path;
    RasterCompression compression = RasterCompression::Deflate;
    std::optional<double> micronsPerPixel;  // unset: the scene's own
    std::string name;
};

MaskWriter::MaskWriter(const char* path) : impl_(std::make_unique<Impl>()) {
    if (path == nullptr || *path == '\0') throw InvalidArgumentError("MaskWriter: the path is empty");
    impl_->path = path;
}

MaskWriter::~MaskWriter() = default;

MaskWriter& MaskWriter::compression(RasterCompression compression) {
    impl_->compression = compression;
    return *this;
}

MaskWriter& MaskWriter::resolution(double micronsPerPixel) {
    if (!std::isfinite(micronsPerPixel) || micronsPerPixel < 0.0) {
        throw InvalidArgumentError("MaskWriter::resolution: the pixel size must be finite and not negative");
    }
    impl_->micronsPerPixel = micronsPerPixel;
    return *this;
}

MaskWriter& MaskWriter::name(const char* name) {
    std::string text = name == nullptr ? std::string() : std::string(name);
    if (!detail::isValidUtf8(text)) throw InvalidArgumentError("MaskWriter::name: the name is not valid UTF-8");
    impl_->name = std::move(text);
    return *this;
}

ExportResult MaskWriter::write(std::shared_ptr<slideio::Scene> scene, ProgressObserver* observer,
                               const CancellationToken& token) {
    if (!scene) throw InvalidArgumentError("MaskWriter::write: the scene is null");

    constexpr const char* describe = "MaskWriter::write: SlideIO failed to describe the scene";
    const int channels = fromSlideio(describe, [&] { return scene->getNumChannels(); });
    if (channels < 1) throw InvalidArgumentError("MaskWriter::write: the scene has no channel");
    const slideio::DataType first = fromSlideio(describe, [&] { return scene->getChannelDataType(0); });
    for (int c = 1; c < channels; ++c) {
        if (fromSlideio(describe, [&] { return scene->getChannelDataType(c); }) != first) {
            throw InvalidArgumentError("MaskWriter::write: the scene's channels differ in type");
        }
    }
    // A type DataType cannot hold maps to Unknown, which RasterTiffWriter refuses as
    // UnsupportedDataTypeError, as it refuses every type outside the OME-TIFF layout.
    const DataType type = dataTypeFromSlideio(static_cast<int32_t>(first));
    const std::tuple<int, int, int, int> rect = fromSlideio(describe, [&] { return scene->getRect(); });
    const int64_t width = std::get<2>(rect);
    const int64_t height = std::get<3>(rect);
    if (width <= 0 || height <= 0) throw InvalidArgumentError("MaskWriter::write: the scene is empty");

    double micronsPerPixel = 0.0;
    if (impl_->micronsPerPixel) {
        micronsPerPixel = *impl_->micronsPerPixel;
    } else {
        const std::tuple<double, double> metres = fromSlideio(describe, [&] { return scene->getResolution(); });
        const double own = std::get<0>(metres) * 1e6;
        if (std::isfinite(own) && own > 0.0) micronsPerPixel = own;
    }

    SceneRasterSource source(scene, type, static_cast<int32_t>(channels), width, height);
    RasterTiffWriter writer(impl_->path.c_str());
    writer.compression(impl_->compression).resolution(micronsPerPixel).name(impl_->name.c_str());
    return writer.write(source, Rect64{0, 0, width, height}, 1.0, observer, token);
}

} // namespace slideio::processor
