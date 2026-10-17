#include <slideio/processor/core/raster_mask.hpp>

#include <slideio/processor/core/error.hpp>

#include "core/detail/coverage.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace slideio::processor {

class RasterMask::Impl {
public:
    Rect64 region{0, 0, 0, 0};
    int32_t level = 0;
    double scale = 0.0;
    std::vector<uint8_t> pixels;
};

namespace {

// floor(v / scale) as int64, or false when it does not fit.
bool floorDiv(int64_t v, double scale, int64_t& out) noexcept {
    const double q = std::floor(static_cast<double>(v) / scale);
    // 2^63 is exactly representable; anything at or beyond it cannot be a valid index.
    constexpr double kLimit = 9223372036854775808.0;
    if (!(q >= -kLimit && q < kLimit)) {
        return false;
    }
    out = static_cast<int64_t>(q);
    return true;
}

// Smallest accepted scale: keeps x / scale finite for every int64 x.
constexpr double kMinScale = 1e-6;

} // namespace

RasterMask::RasterMask() noexcept = default;

RasterMask::RasterMask(const Rect64& regionAtLevel, int32_t level, double levelScale) {
    if (regionAtLevel.isEmpty()) {
        throw InvalidArgumentError("RasterMask: region is empty");
    }
    if (level < 0) {
        throw InvalidArgumentError("RasterMask: level is negative");
    }
    if (!std::isfinite(levelScale) || !(levelScale >= kMinScale)) {
        throw InvalidArgumentError("RasterMask: levelScale must be finite and at least 1e-6");
    }
    // The region is non-empty, so width and height are positive here.
    if (regionAtLevel.x > std::numeric_limits<int64_t>::max() - regionAtLevel.width ||
        regionAtLevel.y > std::numeric_limits<int64_t>::max() - regionAtLevel.height) {
        throw InvalidArgumentError("RasterMask: region extent overflows int64");
    }
    const int64_t maxBytes = static_cast<int64_t>(
        std::min<uint64_t>(std::numeric_limits<size_t>::max() / 2,
                           static_cast<uint64_t>(std::numeric_limits<int64_t>::max())));
    if (regionAtLevel.width > maxBytes / regionAtLevel.height) {
        throw InvalidArgumentError("RasterMask: region area overflows");
    }
    auto impl = std::make_unique<Impl>();
    impl->region = regionAtLevel;
    impl->level = level;
    impl->scale = levelScale;
    impl->pixels.assign(static_cast<size_t>(regionAtLevel.width * regionAtLevel.height), 0);
    impl_ = std::move(impl);
}

RasterMask::RasterMask(const RasterMask& other)
    : impl_(other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr) {}

RasterMask::RasterMask(RasterMask&& other) noexcept : impl_(std::move(other.impl_)) {}

RasterMask& RasterMask::operator=(const RasterMask& other) {
    if (this != &other) {
        std::unique_ptr<Impl> copy = other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr;
        impl_ = std::move(copy);
    }
    return *this;
}

RasterMask& RasterMask::operator=(RasterMask&& other) noexcept {
    if (this != &other) {
        impl_ = std::move(other.impl_); // the source becomes null; never a swap
    }
    return *this;
}

RasterMask::~RasterMask() = default;

bool RasterMask::isNull() const noexcept { return !impl_; }

Rect64 RasterMask::regionAtLevel() const noexcept {
    return impl_ ? impl_->region : Rect64{0, 0, 0, 0};
}

int32_t RasterMask::level() const noexcept { return impl_ ? impl_->level : 0; }

double RasterMask::levelScale() const noexcept { return impl_ ? impl_->scale : 0.0; }

const uint8_t* RasterMask::row(int64_t y) const {
    if (!impl_ || y < 0 || y >= impl_->region.height) {
        throw InvalidArgumentError("RasterMask::row: row out of range");
    }
    return impl_->pixels.data() + static_cast<size_t>(y * impl_->region.width);
}

uint8_t* RasterMask::row(int64_t y) {
    return const_cast<uint8_t*>(static_cast<const RasterMask&>(*this).row(y));
}

bool RasterMask::test(Point64 levelPoint) const noexcept {
    if (!impl_ || !impl_->region.contains(levelPoint)) {
        return false;
    }
    const int64_t x = levelPoint.x - impl_->region.x;
    const int64_t y = levelPoint.y - impl_->region.y;
    return impl_->pixels[static_cast<size_t>(y * impl_->region.width + x)] != 0;
}

bool RasterMask::testLevel0(Point64 slidePoint) const noexcept {
    if (!impl_) {
        return false;
    }
    Point64 p{0, 0};
    if (!floorDiv(slidePoint.x, impl_->scale, p.x) || !floorDiv(slidePoint.y, impl_->scale, p.y)) {
        return false;
    }
    return test(p);
}

void RasterMask::clear() noexcept {
    if (impl_) {
        std::fill(impl_->pixels.begin(), impl_->pixels.end(), uint8_t{0});
    }
}

bool RasterMask::isEmpty() const noexcept {
    if (!impl_) {
        return true;
    }
    return std::all_of(impl_->pixels.begin(), impl_->pixels.end(), [](uint8_t v) { return v == 0; });
}

int64_t RasterMask::setPixelCount() const noexcept {
    if (!impl_) {
        return 0;
    }
    return static_cast<int64_t>(
        std::count_if(impl_->pixels.begin(), impl_->pixels.end(), [](uint8_t v) { return v != 0; }));
}

void rasteriseInto(RasterMask& mask, const Polygon& polygon) {
    if (mask.isNull()) {
        throw InvalidArgumentError("rasteriseInto: mask is null");
    }
    const Rect64 region = mask.regionAtLevel();
    const double scale = mask.levelScale();
    const detail::PixelGrid grid{region.x, region.y, scale, scale, region.width, region.height};
    detail::forEachCoveredSpan(polygon, grid, [&mask](int64_t y, int64_t x0, int64_t x1) {
        std::memset(mask.row(y) + x0, 255, static_cast<size_t>(x1 - x0));
    });
}

namespace {

// One sample of the plane's type, as native-endian bytes.
struct Sample {
    unsigned char bytes[8];
    int32_t size;
};

// IEEE half bit pattern of an integer with |v| <= 2048 (exact).
uint16_t halfFromInteger(int64_t v) {
    if (v == 0) {
        return 0;
    }
    const uint32_t sign = v < 0 ? 0x8000u : 0u;
    const uint32_t a = static_cast<uint32_t>(v < 0 ? -v : v);
    uint32_t e = 0;
    while ((a >> (e + 1)) != 0) {
        ++e;
    }
    const uint32_t frac = a - (1u << e);
    const uint32_t mant = e <= 10 ? frac << (10 - e) : frac >> (e - 10);
    return static_cast<uint16_t>(sign | ((e + 15) << 10) | mant);
}

template <typename T>
Sample sampleOf(T v) {
    Sample s{};
    s.size = static_cast<int32_t>(sizeof(T));
    std::memcpy(s.bytes, &v, sizeof(T));
    return s;
}

template <typename T>
bool fitsIn(int64_t value) {
    return value >= static_cast<int64_t>(std::numeric_limits<T>::min()) &&
           value <= static_cast<int64_t>(std::numeric_limits<T>::max());
}

// Encodes value for the type, or throws InvalidArgumentError naming both.
Sample makeSample(DataType type, int64_t value) {
    constexpr int64_t k24 = int64_t(1) << 24, k53 = int64_t(1) << 53;
    switch (type) {
    case DataType::Byte:
        if (fitsIn<uint8_t>(value)) return sampleOf(static_cast<uint8_t>(value));
        break;
    case DataType::Int8:
        if (fitsIn<int8_t>(value)) return sampleOf(static_cast<int8_t>(value));
        break;
    case DataType::UInt16:
        if (fitsIn<uint16_t>(value)) return sampleOf(static_cast<uint16_t>(value));
        break;
    case DataType::Int16:
        if (fitsIn<int16_t>(value)) return sampleOf(static_cast<int16_t>(value));
        break;
    case DataType::UInt32:
        if (fitsIn<uint32_t>(value)) return sampleOf(static_cast<uint32_t>(value));
        break;
    case DataType::Int32:
        if (fitsIn<int32_t>(value)) return sampleOf(static_cast<int32_t>(value));
        break;
    case DataType::UInt64:
        if (value >= 0) return sampleOf(static_cast<uint64_t>(value));
        break;
    case DataType::Int64:
        return sampleOf(value);
    case DataType::Float16:
        if (value >= -2048 && value <= 2048) return sampleOf(halfFromInteger(value));
        break;
    case DataType::Float32:
        if (value >= -k24 && value <= k24) return sampleOf(static_cast<float>(value));
        break;
    case DataType::Float64:
        if (value >= -k53 && value <= k53) return sampleOf(static_cast<double>(value));
        break;
    default:
        break;
    }
    throw InvalidArgumentError(("rasteriseInto: value " + std::to_string(value) +
                                " does not fit plane type " + dataTypeName(type))
                                   .c_str());
}

} // namespace

void dilateMask(RasterMask& mask, int32_t radius) {
    if (mask.isNull()) {
        throw InvalidArgumentError("dilateMask: mask is null");
    }
    if (radius < 0) {
        throw InvalidArgumentError("dilateMask: radius is negative");
    }
    if (radius == 0) {
        return;
    }
    const int64_t w = mask.regionAtLevel().width, h = mask.regionAtLevel().height;
    const int64_t r = radius;
    // All working memory first, so a bad_alloc leaves the mask unchanged.
    std::vector<uint8_t> horizontal(static_cast<size_t>(w * h), 0);
    std::vector<int64_t> counts(static_cast<size_t>(w), 0);
    std::vector<uint8_t> out(static_cast<size_t>(w), 0);
    // Horizontal pass: a sliding count of set pixels in [x - r, x + r].
    for (int64_t y = 0; y < h; ++y) {
        const uint8_t* src = mask.row(y);
        uint8_t* dst = horizontal.data() + static_cast<size_t>(y * w);
        int64_t cnt = 0;
        for (int64_t x = 0; x <= std::min(r, w - 1); ++x) {
            cnt += src[x] != 0;
        }
        for (int64_t x = 0; x < w; ++x) {
            if (x > 0) {
                if (x + r < w) {
                    cnt += src[x + r] != 0;
                }
                if (x - r - 1 >= 0) {
                    cnt -= src[x - r - 1] != 0;
                }
            }
            dst[x] = cnt > 0 ? 255 : 0;
        }
    }
    // Vertical pass: per-column counts of set pixels in rows [y - r, y + r].
    auto addRow = [&](int64_t y, int64_t sign) {
        const uint8_t* src = horizontal.data() + static_cast<size_t>(y * w);
        for (int64_t x = 0; x < w; ++x) {
            counts[static_cast<size_t>(x)] += sign * (src[x] != 0);
        }
    };
    for (int64_t y = 0; y <= std::min(r, h - 1); ++y) {
        addRow(y, 1);
    }
    for (int64_t y = 0; y < h; ++y) {
        if (y > 0) {
            if (y + r < h) {
                addRow(y + r, 1);
            }
            if (y - r - 1 >= 0) {
                addRow(y - r - 1, -1);
            }
        }
        for (int64_t x = 0; x < w; ++x) {
            out[static_cast<size_t>(x)] = counts[static_cast<size_t>(x)] > 0 ? 255 : 0;
        }
        std::memcpy(mask.row(y), out.data(), static_cast<size_t>(w));
    }
}

void rasteriseInto(BlockView& plane, int32_t planeIndex, const Polygon& polygon, int64_t value) {
    if (planeIndex < 0 || planeIndex >= plane.planeCount()) {
        throw InvalidArgumentError("rasteriseInto: planeIndex out of range");
    }
    const Sample sample = makeSample(plane.planeType(planeIndex), value);
    const detail::PixelGrid grid = detail::gridOfView(plane);
    unsigned char* base = static_cast<unsigned char*>(plane.planeData(planeIndex));
    const int64_t stride = plane.planeRowStrideBytes(planeIndex);
    detail::forEachCoveredSpan(polygon, grid, [&](int64_t y, int64_t x0, int64_t x1) {
        unsigned char* p = base + y * stride + x0 * sample.size;
        for (int64_t x = x0; x < x1; ++x, p += sample.size) {
            std::memcpy(p, sample.bytes, static_cast<size_t>(sample.size));
        }
    });
}

} // namespace slideio::processor
