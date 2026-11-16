// Requirements: UR-EXE-1, UR-IMG-5, UR-STG-3
#pragma once

#include <pipeline/detail/level_table.hpp>
#include <pipeline/detail/run_plan.hpp>
#include <pipeline/detail/scene_reader.hpp>

#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/core/ids.hpp>
#include <slideio/processor/core/memory_budget.hpp>
#include <slideio/processor/pipeline/pipeline.hpp>
#include <slideio/processor/pipeline/pipeline_spec.hpp>
#include <slideio/processor/pipeline/tile_grid.hpp>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace slideio::processor {

/// The state behind a Pipeline: the bindings, and what validate() resolved.
class Pipeline::Impl {
public:
    Impl(std::shared_ptr<slideio::Scene> sceneIn, const Configuration& configIn, const PipelineSpec& specIn)
        : scene(std::move(sceneIn)), config(configIn), spec(specIn) {}

    std::shared_ptr<slideio::Scene> scene;
    Configuration config;
    PipelineSpec spec;

    // Bindings. Every pointer is borrowed from the caller.
    MemoryBudgetPtr budget;                                        ///< Never null.
    EdgeSink* edgeSink = nullptr;
    AnnotationSink* annotationSink = nullptr;
    RasterSink* rasterSink = nullptr;
    std::vector<std::pair<std::string, TileRasterSource*>> auxiliaries; ///< In binding order, names unique.
    TileMaskSource* maskSource = nullptr;
    std::vector<std::pair<std::string, LayerId>> layers;          ///< In binding order, names unique.
    std::string stageName = "pipeline";

    // Scene metadata, read at the first validate() and kept: it never changes.
    std::unique_ptr<detail::SceneReader> reader;
    std::unique_ptr<LevelSelector> selector;
    std::vector<detail::LevelDims> levels;

    /// What the last successful validate() resolved; dropped by every binding call.
    std::unique_ptr<detail::RunPlan> plan;
};

namespace detail {

/// The engine's way into a Pipeline's state (the executor and tests use it), so
/// no public signature grows.
struct PipelineAccess {
    static Pipeline::Impl& impl(const Pipeline& pipeline) noexcept { return *pipeline.impl_; }

    /// The plan of the last successful validate(); InternalError when there is none.
    static const RunPlan& plan(const Pipeline& pipeline) {
        if (!pipeline.impl_->plan) {
            throw InternalError("PipelineAccess::plan: the pipeline has not been validated");
        }
        return *pipeline.impl_->plan;
    }
};

} // namespace detail

} // namespace slideio::processor
