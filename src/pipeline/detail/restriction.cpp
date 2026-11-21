// Requirements: UR-IMG-5, UR-STG-3
#include <pipeline/detail/restriction.hpp>

#include <slideio/processor/core/error.hpp>

#include <core/detail/level_mapping.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace slideio::processor::detail {

RestrictionFilter::RestrictionFilter(const RunPlan& plan, const PipelineSpec& spec, const LevelSelector& selector,
                                     TileMaskSource* maskSource, MemoryBudget* budget)
    : plan_(plan),
      restricted_(plan.restricted),
      hasRoi_(spec.hasRoi()),
      roi_(hasRoi_ ? spec.roi() : Rect64{0, 0, 0, 0}),
      roiAtLevel_{0, 0, 0, 0},
      polygon_(spec.hasPolygon() ? &spec.polygon() : nullptr),
      maskSource_(maskSource),
      budget_(budget) {
    if (hasRoi_) {
        // The outward mapping, which gives the run's region. It holds the level pixel of every ROI
        // anchor, where admits() tests it, at every scale.
        roiAtLevel_ = selector.mapToLevel(roi_, plan.level);
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
    // The tile's rect, which the mask built for it always covers.
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

bool RestrictionFilter::outsideRegion(int64_t tileIndex) const {
    if (!restricted_) {
        return false;
    }
    // At a non-integer scale the polygon's conservative raster can lie one level pixel outside
    // the bounding box's outward mapping, on any side: rasteriseInto sets a pixel whose real
    // square [P * s, (P + 1) * s) touches the polygon. The region is widened by one on every side.
    Rect64 region = plan_.region;
    if (plan_.levelScale != std::floor(plan_.levelScale)) {
        --region.x;
        --region.y;
        region.width += 2;
        region.height += 2;
    }
    return !plan_.grid.tileRectAtLevel(tileIndex).intersects(region);
}

Point64 RestrictionFilter::levelPixelOf(Point64 anchorLevel0) const {
    // The level pixel whose mapped corners hold the point, found as RasterMask::testLevel0
    // finds it.
    Point64 pixel{0, 0};
    if (!levelOrdinateAtOrBelow(anchorLevel0.x, plan_.levelScale, pixel.x) ||
        !levelOrdinateAtOrBelow(anchorLevel0.y, plan_.levelScale, pixel.y)) {
        throw InvalidArgumentError("RestrictionFilter: an anchor lies outside the level's coordinate range");
    }
    return pixel;
}

int64_t RestrictionFilter::tileOf(Point64 anchorLevel0) const { return tileHolding(levelPixelOf(anchorLevel0)); }

bool RestrictionFilter::inRoi(Point64 anchorLevel0) const noexcept {
    return !hasRoi_ || (anchorLevel0.x >= roi_.x && anchorLevel0.y >= roi_.y && anchorLevel0.x - roi_.x < roi_.width &&
                        anchorLevel0.y - roi_.y < roi_.height);
}

bool RestrictionFilter::admits(Point64 anchorLevel0) const {
    if (!restricted_) {
        return true;
    }
    // The ROI is tested exactly in level 0, before any mask: the mask's ROI part also sets the
    // pixels straddling the ROI's edge.
    if (!inRoi(anchorLevel0)) {
        return false;
    }
    const Point64 pixel = levelPixelOf(anchorLevel0);
    const int64_t tile = tileHolding(pixel);
    // In no tile of the grid (rare), or the tile's mask does not fit the budget:
    // a one-pixel mask, uncached.
    const auto uncached = [&]() { return buildMask(Rect64{pixel.x, pixel.y, 1, 1}).testLevel0(anchorLevel0); };
    if (tile < 0) {
        return uncached();
    }

    // The tile's entry is found or made under the cache lock, its bytes reserved
    // as it is made; its mask is built under the entry's own lock, once:
    // concurrent callers in the same tile wait for that build, and a build that
    // throws leaves the entry unbuilt, so the next caller builds it again.
    std::shared_ptr<CachedMask> entry;
    {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        const auto found = cache_.find(tile);
        if (found != cache_.end()) {
            entry = found->second;
        } else {
            const int64_t bytes = budget_ != nullptr ? plan_.grid.tileRectAtLevel(tile).area() : 0;
            if (budget_ == nullptr || budget_->tryReserve(bytes)) {
                try {
                    entry = std::make_shared<CachedMask>(budget_, bytes);
                } catch (...) {
                    if (budget_ != nullptr) {
                        budget_->release(bytes);
                    }
                    throw;
                }
                // Should this throw, the entry gives its bytes back as it goes.
                cache_.emplace(tile, entry);
            }
        }
    }
    if (!entry) {
        return uncached();
    }
    {
        std::lock_guard<std::mutex> lock(entry->mutex);
        if (!entry->built) {
            entry->mask = buildMask(plan_.grid.tileRectAtLevel(tile));
            entry->built = true;
        }
    }
    // Never written once built, and the lock above ordered the build before this read.
    return entry->mask.testLevel0(anchorLevel0);
}

RestrictionFilter::CachedMask::~CachedMask() {
    if (budget != nullptr && bytes > 0) {
        try {
            budget->release(bytes);
        } catch (...) {
            // A failing budget lock cannot be reported from a destructor.
        }
    }
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

TileMaskRelease::TileMaskRelease(const RestrictionFilter& filter, const TileGrid& grid)
    : filter_(filter),
      grid_(grid),
      completed_(std::make_unique<std::atomic<bool>[]>(static_cast<size_t>(grid.tileCount()))) {
    for (int64_t t = 0; t < grid.tileCount(); ++t) {
        completed_[static_cast<size_t>(t)] = false;
    }
}

bool TileMaskRelease::settled(int64_t tileIndex) const {
    const int64_t columns = grid_.columns();
    const int64_t column = tileIndex % columns;
    const int64_t row = tileIndex / columns;
    for (int64_t r = std::max<int64_t>(row - 1, 0); r <= std::min<int64_t>(row + 1, grid_.rows() - 1); ++r) {
        for (int64_t c = std::max<int64_t>(column - 1, 0); c <= std::min<int64_t>(column + 1, columns - 1); ++c) {
            if (!completed_[static_cast<size_t>(grid_.tileIndex(c, r))]) {
                return false;
            }
        }
    }
    return true;
}

bool TileMaskRelease::admits(Point64 anchorLevel0) const {
    const bool admitted = filter_.admits(anchorLevel0);
    // Checked after the build: when the tile settles in between, the completion
    // settling it releases the mask if it came after the build, and this note
    // does if it came before.
    const int64_t tile = filter_.tileOf(anchorLevel0);
    if (tile >= 0 && settled(tile)) {
        std::lock_guard<std::mutex> lock(lateMutex_);
        late_.push_back(tile);
    }
    return admitted;
}

void TileMaskRelease::tileCompleted(int64_t tileIndex) {
    if (tileIndex < 0 || tileIndex >= grid_.tileCount()) {
        throw InvalidArgumentError("TileMaskRelease::tileCompleted: the tile index is outside the grid");
    }
    completed_[static_cast<size_t>(tileIndex)] = true;
    // Only the tile and its neighbours can settle with it.
    const int64_t columns = grid_.columns();
    const int64_t column = tileIndex % columns;
    const int64_t row = tileIndex / columns;
    for (int64_t r = std::max<int64_t>(row - 1, 0); r <= std::min<int64_t>(row + 1, grid_.rows() - 1); ++r) {
        for (int64_t c = std::max<int64_t>(column - 1, 0); c <= std::min<int64_t>(column + 1, columns - 1); ++c) {
            const int64_t tile = grid_.tileIndex(c, r);
            if (settled(tile)) {
                filter_.releaseTileMask(tile);
            }
        }
    }
    std::vector<int64_t> late;
    {
        std::lock_guard<std::mutex> lock(lateMutex_);
        late.swap(late_);
    }
    for (const int64_t tile : late) {
        filter_.releaseTileMask(tile);
    }
}

} // namespace slideio::processor::detail
