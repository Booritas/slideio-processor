#include <slideio/processor/core/raster_mask.hpp>

#include <slideio/processor/core/error.hpp>

#include "core/detail/coverage.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
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

} // namespace

RasterMask::RasterMask() noexcept = default;

RasterMask::RasterMask(const Rect64& regionAtLevel, int32_t level, double levelScale) {
    if (regionAtLevel.isEmpty()) {
        throw InvalidArgumentError("RasterMask: region is empty");
    }
    if (level < 0) {
        throw InvalidArgumentError("RasterMask: level is negative");
    }
    if (!std::isfinite(levelScale) || !(levelScale > 0.0)) {
        throw InvalidArgumentError("RasterMask: levelScale must be positive and finite");
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
        impl_.swap(other.impl_);
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
    const detail::PixelGrid grid{static_cast<double>(region.x) * scale,
                                 static_cast<double>(region.y) * scale,
                                 scale,
                                 scale,
                                 region.width,
                                 region.height};
    detail::forEachCoveredSpan(polygon, grid, [&mask](int64_t y, int64_t x0, int64_t x1) {
        std::memset(mask.row(y) + x0, 255, static_cast<size_t>(x1 - x0));
    });
}

} // namespace slideio::processor
