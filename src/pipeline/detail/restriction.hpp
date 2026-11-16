// Requirements: UR-IMG-5, UR-STG-3
#pragma once

#include <pipeline/detail/run_plan.hpp>

#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/core/raster_mask.hpp>
#include <slideio/processor/pipeline/pipeline_spec.hpp>
#include <slideio/processor/pipeline/tile_grid.hpp>

#include <cstdint>

namespace slideio::processor::detail {

/// The run's restriction (ruling P24): the per-tile mask, built before any pixel
/// is read, and the anchor test for objects emitted outside a tile. Built once
/// per run; every member is const, so workers share it. Every pointer and
/// reference is borrowed and must outlive the filter.
///
/// The ROI is rasterised by its edges: a level pixel is set when it lies inside
/// LevelSelector::mapToLevel(roi), the outward mapping the grid region uses.
/// Polygon and TileMaskSource restrictions are not implemented yet: the
/// constructor throws InternalError for them.
class RestrictionFilter {
public:
    RestrictionFilter(const RunPlan& plan, const PipelineSpec& spec, const LevelSelector& selector,
                      TileMaskSource* maskSource);

    /// True when the run is restricted, so tiles carry a mask.
    bool restricted() const noexcept;

    /// The tile's mask over its halo rect at the level (the tile grown by the
    /// chain's halo); a null mask when the run is unrestricted.
    RasterMask buildTileMask(int64_t tileIndex) const;

    /// True when the tile's mask has no set pixel inside the tile rect, so the
    /// tile is skipped. False for a null mask.
    bool skips(const RasterMask& mask, int64_t tileIndex) const;

    /// Whether an object anchored at a level-0 point outside any tile's emission
    /// passes the restriction: a one-pixel mask at the anchor's level pixel, built
    /// through the same sources. True when the run is unrestricted.
    bool admits(Point64 anchorLevel0) const;

private:
    // Clears every pixel of `mask` outside the restriction.
    void intersect(RasterMask& mask) const;

    const RunPlan& plan_;
    bool restricted_;
    bool hasRoi_;
    Rect64 roiAtLevel_;
};

} // namespace slideio::processor::detail
