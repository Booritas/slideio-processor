// Requirements: UR-IMG-5, UR-STG-3
#include <pipeline/detail/restriction.hpp>

#include <slideio/processor/core/error.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace slideio::processor::detail {

RestrictionFilter::RestrictionFilter(const RunPlan& plan, const PipelineSpec& spec, const LevelSelector& selector,
                                     TileMaskSource* maskSource)
    : plan_(plan),
      restricted_(plan.restricted),
      hasRoi_(spec.hasRoi()),
      roiAtLevel_{0, 0, 0, 0},
      polygon_(spec.hasPolygon() ? &spec.polygon() : nullptr),
      maskSource_(maskSource) {
    if (hasRoi_) {
        roiAtLevel_ = selector.mapToLevel(spec.roi(), plan.level);
    }
}

bool RestrictionFilter::restricted() const noexcept { return restricted_; }

void RestrictionFilter::intersectRoi(RasterMask& mask) const {
    const Rect64 region = mask.regionAtLevel();
    const Rect64 keep = region.intersected(roiAtLevel_);
    for (int64_t y = 0; y < region.height; ++y) {
        uint8_t* row = mask.row(y);
        const int64_t levelY = region.y + y;
        if (keep.isEmpty() || levelY < keep.y || levelY >= keep.y + keep.height) {
            std::memset(row, 0, static_cast<size_t>(region.width));
            continue;
        }
        const int64_t from = keep.x - region.x;
        const int64_t to = from + keep.width;
        std::memset(row, 0, static_cast<size_t>(from));
        std::memset(row + to, 0, static_cast<size_t>(region.width - to));
    }
}

// Clears every pixel of `mask` that `keep` (the same region) leaves clear.
static void andInto(RasterMask& mask, const RasterMask& keep) {
    const Rect64 region = mask.regionAtLevel();
    for (int64_t y = 0; y < region.height; ++y) {
        uint8_t* row = mask.row(y);
        const uint8_t* other = keep.row(y);
        for (int64_t x = 0; x < region.width; ++x) {
            row[x] = static_cast<uint8_t>(row[x] & other[x]);
        }
    }
}

void RestrictionFilter::intersect(RasterMask& mask) const {
    if (hasRoi_) {
        intersectRoi(mask);
    }
    const Rect64 region = mask.regionAtLevel();
    if (polygon_ != nullptr) {
        RasterMask covered(region, mask.level(), mask.levelScale());
        rasteriseInto(covered, *polygon_);
        andInto(mask, covered);
    }
    if (maskSource_ != nullptr) {
        // The source fills a mask of its own: it may write every pixel, and the tile mask
        // keeps what the earlier restrictions allowed.
        RasterMask fromSource(region, mask.level(), mask.levelScale());
        maskSource_->fillMask(fromSource);
        andInto(mask, fromSource);
    }
}

RasterMask RestrictionFilter::buildMask(const Rect64& regionAtLevel) const {
    RasterMask mask(regionAtLevel, plan_.level, plan_.levelScale);
    const Rect64 region = mask.regionAtLevel();
    for (int64_t y = 0; y < region.height; ++y) {
        std::memset(mask.row(y), 255, static_cast<size_t>(region.width));
    }
    intersect(mask);
    return mask;
}

RasterMask RestrictionFilter::buildTileMask(int64_t tileIndex) const {
    if (!restricted_) {
        return RasterMask();
    }
    return buildMask(plan_.grid.tileRectAtLevelWithHalo(tileIndex, plan_.chainHalo));
}

bool RestrictionFilter::skips(const RasterMask& mask, int64_t tileIndex) const {
    if (mask.isNull()) {
        return false;
    }
    const Rect64 tile = plan_.grid.tileRectAtLevel(tileIndex);
    const Rect64 region = mask.regionAtLevel();
    for (int64_t y = tile.y; y < tile.y + tile.height; ++y) {
        const uint8_t* row = mask.row(y - region.y) + (tile.x - region.x);
        if (std::any_of(row, row + tile.width, [](uint8_t v) { return v != 0; })) {
            return false;
        }
    }
    return true;
}

bool RestrictionFilter::admits(Point64 anchorLevel0) const {
    if (!restricted_) {
        return true;
    }
    // The level pixel whose square holds the point, computed as RasterMask::testLevel0
    // computes it (double division, then floor).
    const double scale = plan_.levelScale;
    const Point64 pixel{static_cast<int64_t>(std::floor(static_cast<double>(anchorLevel0.x) / scale)),
                        static_cast<int64_t>(std::floor(static_cast<double>(anchorLevel0.y) / scale))};
    const int64_t tile = tileHolding(pixel);
    if (tile < 0) {
        // In no tile of the grid: rare, so a one-pixel mask, uncached.
        return buildMask(Rect64{pixel.x, pixel.y, 1, 1}).testLevel0(anchorLevel0);
    }

    // The tile's entry is found or made under the lock; its mask is built outside
    // it, once: concurrent callers in the same tile wait on the entry's once_flag,
    // and a build that throws leaves the flag unset for the next caller.
    std::shared_ptr<CachedMask> entry;
    {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        std::shared_ptr<CachedMask>& slot = cache_[tile];
        if (!slot) {
            slot = std::make_shared<CachedMask>();
        }
        entry = slot;
    }
    std::call_once(entry->built,
                   [this, tile, &entry]() { entry->mask = buildMask(plan_.grid.tileRectAtLevel(tile)); });
    return entry->mask.testLevel0(anchorLevel0);
}

void RestrictionFilter::releaseTileMask(int64_t tileIndex) const {
    // A caller still holding the entry keeps it alive until it is done.
    std::lock_guard<std::mutex> lock(cacheMutex_);
    cache_.erase(tileIndex);
}

int64_t RestrictionFilter::cachedTileMaskCount() const {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    return static_cast<int64_t>(cache_.size());
}

int64_t RestrictionFilter::tileHolding(Point64 levelPixel) const {
    const TileGrid& grid = plan_.grid;
    if (grid.tileCount() == 0) {
        return -1;
    }
    // Tile 0 has the grid's origin and, unless the grid has a single column or row,
    // its full tile size; tiles are laid out row-major from it.
    const Rect64 first = grid.tileRectAtLevel(0);
    if (levelPixel.x < first.x || levelPixel.y < first.y) {
        return -1;
    }
    const int64_t column = (levelPixel.x - first.x) / first.width;
    const int64_t row = (levelPixel.y - first.y) / first.height;
    if (column >= grid.columns() || row >= grid.rows()) {
        return -1;
    }
    const int64_t tile = grid.tileIndex(column, row);
    return grid.tileRectAtLevel(tile).contains(levelPixel) ? tile : -1;
}

} // namespace slideio::processor::detail
