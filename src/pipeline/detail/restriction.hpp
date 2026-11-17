// Requirements: UR-IMG-5, UR-STG-3
#pragma once

#include <pipeline/detail/run_plan.hpp>

#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/core/raster_mask.hpp>
#include <slideio/processor/pipeline/pipeline_spec.hpp>
#include <slideio/processor/pipeline/tile_grid.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace slideio::processor::detail {

/// The run's restriction: the per-tile mask, built before any pixel
/// is read, and the anchor test for objects emitted outside a tile. Built once
/// per run and shared by the workers: every member function may be called from
/// several threads at once. Every pointer and reference is borrowed and must
/// outlive the filter.
///
/// The ROI is rasterised by its edges: a level pixel is set when it lies inside
/// LevelSelector::mapToLevel(roi), the outward mapping the grid region uses.
/// The polygon is rasterised with rasteriseInto (conservative even-odd) and the
/// TileMaskSource fills a second mask over the same region; both are intersected
/// with the tile mask. The mask source may be called from several workers at
/// once, so it must be thread-safe.
///
/// admits() answers from a cache of masks, one per tile of the grid, each over the
/// tile rect (no halo) and built on the first admits() in that tile, so a tile
/// holding many anchors costs one build, one TileMaskSource::fillMask call
/// included. A mutex guards the cache; a tile's mask is built outside it, once,
/// however many threads ask for it together. A cached mask stays until
/// releaseTileMask(), which the caller invokes when no anchor in that tile can
/// arrive any more; a later admits() in a released tile builds its mask again.
class RestrictionFilter {
public:
    RestrictionFilter(const RunPlan& plan, const PipelineSpec& spec, const LevelSelector& selector,
                      TileMaskSource* maskSource);

    RestrictionFilter(const RestrictionFilter&) = delete;
    RestrictionFilter& operator=(const RestrictionFilter&) = delete;

    /// True when the run is restricted, so tiles carry a mask.
    bool restricted() const noexcept;

    /// The tile's mask over its halo rect at the level (the tile grown by the
    /// chain's halo); a null mask when the run is unrestricted. Not cached.
    RasterMask buildTileMask(int64_t tileIndex) const;

    /// True when the tile's mask has no set pixel inside the tile rect, so the
    /// tile is skipped. False for a null mask.
    bool skips(const RasterMask& mask, int64_t tileIndex) const;

    /// Whether an object anchored at a level-0 point passes the restriction: the
    /// pixel of the anchor's level pixel in the cached mask of the tile holding it.
    /// An anchor in no tile of the grid is tested on a one-pixel mask built
    /// through the same sources, uncached. True when the run is unrestricted.
    bool admits(Point64 anchorLevel0) const;

    /// Drops the cached mask of tile `tileIndex`; nothing when none is cached.
    void releaseTileMask(int64_t tileIndex) const;

    /// How many tile masks the cache holds.
    int64_t cachedTileMaskCount() const;

private:
    // One tile's cached mask; `built` guards its single build.
    struct CachedMask {
        std::once_flag built;
        RasterMask mask;
    };

    // A mask over `regionAtLevel` holding exactly the restriction.
    RasterMask buildMask(const Rect64& regionAtLevel) const;
    // Clears every pixel of `mask` outside the restriction: the ROI, then the polygon,
    // then the mask source.
    void intersect(RasterMask& mask) const;
    void intersectRoi(RasterMask& mask) const;
    // The grid tile whose rect holds the level pixel; -1 when none does.
    int64_t tileHolding(Point64 levelPixel) const;

    const RunPlan& plan_;
    bool restricted_;
    bool hasRoi_;
    Rect64 roiAtLevel_;
    const Polygon* polygon_;        // Null without a polygon restriction; borrowed from the spec.
    TileMaskSource* maskSource_;    // Null without a bound source; borrowed.

    mutable std::mutex cacheMutex_; // Guards cache_ (the map, not the masks it points to).
    mutable std::unordered_map<int64_t, std::shared_ptr<CachedMask>> cache_;
};

} // namespace slideio::processor::detail
