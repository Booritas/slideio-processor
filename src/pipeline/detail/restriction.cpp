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

RasterMask RestrictionFilter::buildTileMask(int64_t tileIndex) const {
    if (!restricted_) {
        return RasterMask();
    }
    RasterMask mask(plan_.grid.tileRectAtLevelWithHalo(tileIndex, plan_.chainHalo), plan_.level, plan_.levelScale);
    const Rect64 region = mask.regionAtLevel();
    for (int64_t y = 0; y < region.height; ++y) {
        std::memset(mask.row(y), 255, static_cast<size_t>(region.width));
    }
    intersect(mask);
    return mask;
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
    RasterMask mask(Rect64{pixel.x, pixel.y, 1, 1}, plan_.level, plan_.levelScale);
    mask.row(0)[0] = 255;
    intersect(mask);
    return mask.testLevel0(anchorLevel0);
}

} // namespace slideio::processor::detail
