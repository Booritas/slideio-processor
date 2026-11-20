// Requirements: UR-IMG-11, UR-ALG-3, UR-ALG-4, UR-EXE-1, UR-IMG-5, UR-STG-3
#pragma once

#include <pipeline/detail/level_table.hpp>

#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/data_type.hpp>
#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/core/ids.hpp>
#include <slideio/processor/pipeline/operation.hpp>
#include <slideio/processor/pipeline/pipeline_spec.hpp>
#include <slideio/processor/pipeline/tile_grid.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace slideio::processor::detail {

/// Everything Pipeline::validate() resolved, kept for the run: the whole-level layout, the level,
/// the grid and the tile bytes. Built only by buildRunPlan, which reads no pixel.
struct RunPlan {
    int32_t level;                                 ///< The analysis level.
    double levelScale;                             ///< LevelSelector::scaleOfLevel(level).
    /// One tile when the chain has a WholeLevel operation. A chain producing objects
    /// without one tiles levelRect, so a restriction's edge is no grid edge and its
    /// objects carry across it; every other chain tiles region.
    TileGrid grid;
    std::vector<OperationPtr> operations;          ///< Created from the spec through the catalog, in chain order.
    /// Plane types per step: [0] the source planes (the selected channels' types),
    /// [k + 1] the output of operation k. A PerChannel operation maps each plane
    /// through outputType(); a Joint one yields outputPlaneCount(n) planes, each
    /// of outputType(type of its first input plane).
    std::vector<std::vector<DataType>> planeTypes;
    std::vector<int32_t> channels;                 ///< The selected scene channels, in plane order.
    std::vector<LayerId> targetLayers;             ///< Per operation; invalid when it produces no objects.
    Halo chainHalo;                                ///< The operations' halos summed edge by edge.
    std::vector<size_t> globalContextPositions;    ///< Chain positions of needsGlobalContext() operations.
    /// Parallel to globalContextPositions: each operation's accumulatedStateBytes() for
    /// its input step's plane count, a negative figure as 0.
    std::vector<int64_t> accumulatedStateBytes;
    bool restricted;                               ///< A ROI, a polygon or a mask source is bound.
    /// Upper bound on one tile's buffers: the largest tile of the grid grown by chainHalo,
    /// times the bytes per pixel of every step's planes, the reader's interleave
    /// buffer, every auxiliary's planes and the restriction masks (one byte,
    /// two with a polygon or a mask source).
    int64_t tileBytes;
    /// The analysis level's extent, {0, 0, width, height}: what lies outside it reads
    /// as zeros at every step of the chain.
    Rect64 levelRect;
    /// The region at the level, in level pixels: the scene, the ROI and the polygon's
    /// bounding box intersected at level 0, mapped outward and clipped to the level. It
    /// bounds Pipeline::rect(), the raster tiles written and what an accumulation pass
    /// sees; it equals the grid's rect unless the grid tiles levelRect.
    Rect64 region;
};

/// What buildRunPlan validates against. Every pointer and reference is borrowed
/// for the call.
struct RunPlanInputs {
    const PipelineSpec* spec;
    const Configuration* config;
    const std::vector<DataType>* channelTypes;     ///< SceneReader::channelTypes(); Unknown allowed.
    const LevelSelector* selector;
    const std::vector<LevelDims>* levels;
    const std::vector<std::pair<std::string, LayerId>>* layers;
    const std::vector<std::pair<std::string, TileRasterSource*>>* auxiliaries;
    bool hasMaskSource;
    int64_t ceilingBytes;                          ///< The budget's ceilingBytes().
};

/// Runs the checks in the order SDS 05 5.5 lists them, and returns the
/// plan. Throws InvalidArgumentError, UnsupportedDataTypeError or
/// ChannelArityError as Pipeline::validate() documents. Never reads a pixel.
RunPlan buildRunPlan(const RunPlanInputs& inputs);

} // namespace slideio::processor::detail
