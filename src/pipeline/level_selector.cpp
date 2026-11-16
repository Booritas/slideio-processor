// Requirements: UR-IMG-4, UR-SEG-3, UR-IMG-5
#include <slideio/processor/pipeline/tile_grid.hpp>

#include <slideio/processor/core/error.hpp>

#include <pipeline/detail/level_mapping.hpp>
#include <pipeline/detail/level_table.hpp>

#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace slideio::processor {

class LevelSelector::Impl {
public:
    std::vector<detail::LevelDims> levels;
    std::vector<double> scales;

    bool inRange(int32_t level) const noexcept {
        return level >= 0 && static_cast<size_t>(level) < levels.size();
    }

    double scale(int32_t level, const char* method) const {
        if (!inRange(level)) {
            throw InvalidArgumentError((std::string("LevelSelector::") + method + ": level " +
                                        std::to_string(level) + " is out of range [0, " +
                                        std::to_string(levels.size()) + ")")
                                           .c_str());
        }
        return scales[static_cast<size_t>(level)];
    }
};

namespace {

// The outward bound of one ordinate, checked to fit int64.
int64_t toInt64(long double v, const char* method) {
    // 2^63 is exact in long double; anything at or beyond it, or NaN, does not fit.
    constexpr long double limit = 9223372036854775808.0L;
    if (!(v >= -limit && v < limit)) {
        throw InvalidArgumentError(
            (std::string("LevelSelector::") + method + ": the mapped rectangle does not fit int64").c_str());
    }
    return static_cast<int64_t>(v);
}

} // namespace

LevelSelector::LevelSelector(std::shared_ptr<slideio::Scene> scene) {
    if (scene == nullptr) {
        throw InvalidArgumentError("LevelSelector: the scene is null");
    }
    auto impl = std::make_shared<Impl>();
    impl->levels = detail::levelTable(*scene);
    impl->scales.reserve(impl->levels.size());
    for (size_t level = 0; level < impl->levels.size(); ++level) {
        impl->scales.push_back(detail::scaleOfLevel(impl->levels, static_cast<int32_t>(level)));
    }
    impl_ = std::move(impl);
}

LevelSelector::LevelSelector(const LevelSelector& other) noexcept = default;
// The state is immutable and shared, so a move copies the pointer and the source keeps its value.
LevelSelector::LevelSelector(LevelSelector&& other) noexcept : impl_(other.impl_) {}
LevelSelector& LevelSelector::operator=(const LevelSelector& other) noexcept = default;
LevelSelector& LevelSelector::operator=(LevelSelector&& other) noexcept {
    impl_ = other.impl_;
    return *this;
}
LevelSelector::~LevelSelector() = default;

int32_t LevelSelector::levelForScale(double scale) const {
    if (!std::isfinite(scale) || scale <= 0.0) {
        throw InvalidArgumentError("LevelSelector::levelForScale: the scale must be positive and finite");
    }
    for (int32_t level = static_cast<int32_t>(impl_->levels.size()) - 1; level > 0; --level) {
        if (1.0 / impl_->scales[static_cast<size_t>(level)] >= scale * (1.0 - 1e-9)) {
            return level;
        }
    }
    return 0;
}

int32_t LevelSelector::levelForSize(int64_t targetWidth, int64_t targetHeight) const {
    if (targetWidth <= 0 || targetHeight <= 0) {
        throw InvalidArgumentError("LevelSelector::levelForSize: the target size must be positive");
    }
    for (int32_t level = static_cast<int32_t>(impl_->levels.size()) - 1; level > 0; --level) {
        const detail::LevelDims& dims = impl_->levels[static_cast<size_t>(level)];
        if (dims.width >= targetWidth && dims.height >= targetHeight) {
            return level;
        }
    }
    return 0;
}

double LevelSelector::scaleOfLevel(int32_t level) const {
    return impl_->scale(level, "scaleOfLevel");
}

Rect64 LevelSelector::mapToLevel(const Rect64& level0Rect, int32_t level) const {
    const long double s = impl_->scale(level, "mapToLevel");
    const int64_t x0 = toInt64(std::floor(static_cast<long double>(level0Rect.x) / s), "mapToLevel");
    const int64_t y0 = toInt64(std::floor(static_cast<long double>(level0Rect.y) / s), "mapToLevel");
    const int64_t x1 = toInt64(
        std::ceil((static_cast<long double>(level0Rect.x) + static_cast<long double>(level0Rect.width)) / s),
        "mapToLevel");
    const int64_t y1 = toInt64(
        std::ceil((static_cast<long double>(level0Rect.y) + static_cast<long double>(level0Rect.height)) / s),
        "mapToLevel");
    return Rect64{x0, y0, x1 - x0, y1 - y0};
}

Rect64 LevelSelector::mapToLevel0(const Rect64& levelRect, int32_t level) const {
    return detail::rectToLevel0(levelRect, impl_->scale(level, "mapToLevel0"));
}

Point64 LevelSelector::mapToLevel0(Point64 levelPoint, int32_t level) const noexcept {
    if (!impl_->inRange(level)) {
        return levelPoint;
    }
    const double s = impl_->scales[static_cast<size_t>(level)];
    return Point64{detail::levelToLevel0(levelPoint.x, s), detail::levelToLevel0(levelPoint.y, s)};
}

Polygon LevelSelector::mapToLevel0(const Polygon& levelPolygon, int32_t level) const {
    const double s = impl_->scale(level, "mapToLevel0");
    Polygon mapped;
    std::vector<Point64> points;
    for (int32_t ring = 0; ring < levelPolygon.ringCount(); ++ring) {
        points.resize(static_cast<size_t>(levelPolygon.ringPointCount(ring)));
        levelPolygon.ringPoints(ring, points.data(), static_cast<int64_t>(points.size()));
        for (Point64& p : points) {
            p = Point64{detail::levelToLevel0(p.x, s), detail::levelToLevel0(p.y, s)};
        }
        mapped.addRing(points.data(), static_cast<int64_t>(points.size()), levelPolygon.ringIsHole(ring));
    }
    return mapped;
}

void defaultTileSize(std::shared_ptr<slideio::Scene> scene, int32_t level, const Configuration& config,
                     int32_t& widthOut, int32_t& heightOut) {
    if (scene == nullptr) {
        throw InvalidArgumentError("defaultTileSize: the scene is null");
    }
    const std::vector<detail::LevelDims> levels = detail::levelTable(*scene);
    if (level < 0 || static_cast<size_t>(level) >= levels.size()) {
        throw InvalidArgumentError(("defaultTileSize: level " + std::to_string(level) + " is out of range [0, " +
                                    std::to_string(levels.size()) + ")")
                                       .c_str());
    }
    const detail::LevelDims& dims = levels[static_cast<size_t>(level)];
    if (config.tileSizeFromSlide() && dims.tileWidth > 0 && dims.tileHeight > 0) {
        widthOut = dims.tileWidth;
        heightOut = dims.tileHeight;
    } else {
        widthOut = config.tileWidth();
        heightOut = config.tileHeight();
    }
}

} // namespace slideio::processor
