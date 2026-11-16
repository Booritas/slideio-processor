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

namespace slideio {
class Scene;
} // namespace slideio

namespace slideio::processor::detail {

/// Everything Pipeline::validate() resolved, kept for the run (rulings P13, P19,
/// P21, P23, P26). Built only by buildRunPlan, which reads no pixel.
struct RunPlan {
    int32_t level;                                 ///< The analysis level.
    double levelScale;                             ///< LevelSelector::scaleOfLevel(level).
    TileGrid grid;                                 ///< One tile when the chain has a WholeLevel operation.
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
    bool restricted;                               ///< A ROI, a polygon or a mask source is bound.
    /// Upper bound on one tile's buffers: the largest tile grown by chainHalo,
    /// times the bytes per pixel of every step's planes, every auxiliary's planes
    /// and the restriction masks (one byte, two with a mask source).
    int64_t tileBytes;
};

/// What buildRunPlan validates against. Every pointer and reference is borrowed
/// for the call.
struct RunPlanInputs {
    const PipelineSpec* spec;
    const Configuration* config;
    std::shared_ptr<slideio::Scene> scene;
    const std::vector<DataType>* channelTypes;     ///< SceneReader::channelTypes(); Unknown allowed.
    const LevelSelector* selector;
    const std::vector<LevelDims>* levels;
    const std::vector<std::pair<std::string, LayerId>>* layers;
    const std::vector<std::pair<std::string, TileRasterSource*>>* auxiliaries;
    bool hasMaskSource;
    int64_t ceilingBytes;                          ///< The budget's ceilingBytes().
};

/// Runs the checks of ruling P19 in its order, then P13 and P26, and returns the
/// plan. Throws InvalidArgumentError, UnsupportedDataTypeError or
/// ChannelArityError as Pipeline::validate() documents. Never reads a pixel.
RunPlan buildRunPlan(const RunPlanInputs& inputs);

} // namespace slideio::processor::detail
