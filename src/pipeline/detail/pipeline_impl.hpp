// Requirements: UR-EXE-1, UR-IMG-5, UR-STG-3
#pragma once

#include <pipeline/detail/level_table.hpp>
#include <pipeline/detail/run_plan.hpp>
#include <pipeline/detail/scene_reader.hpp>
#include <pipeline/detail/tile_evaluator.hpp>

#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/core/ids.hpp>
#include <slideio/processor/core/memory_budget.hpp>
#include <slideio/processor/pipeline/operation.hpp>
#include <slideio/processor/pipeline/pipeline.hpp>
#include <slideio/processor/pipeline/pipeline_spec.hpp>
#include <slideio/processor/pipeline/tile_grid.hpp>

#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace slideio::processor {

namespace detail {
class PipelineSceneSource;
} // namespace detail

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
    // Shared, so a snapshot outlives the Pipeline.
    std::shared_ptr<const detail::SceneReader> reader;
    std::shared_ptr<const LevelSelector> selector;
    std::vector<detail::LevelDims> levels;

    /// What the last successful validate() resolved. Immutable once built: validate()
    /// swaps a new plan in and every binding call drops the pointer, so a run or a
    /// snapshot holding the old plan keeps it unchanged.
    std::shared_ptr<const detail::RunPlan> plan;

    /// What the last run counted; reset when a run starts.
    detail::RunCounters counters;

    /// The operations the last run applied: the plan's, with the run's own
    /// accumulated global-context instances in their positions. Set when a run
    /// starts; empty before the first. Never read by a run.
    std::vector<OperationPtr> lastRunOperations;

    /// What levelCount(), rect(), readBlock() and asScene() evaluate through: built
    /// under `lazyMutex` from a snapshot of the bindings and set only when the build
    /// succeeds, so concurrent readBlock() calls share one build and a failed build
    /// leaves nothing. Every binding call drops it; validate() leaves it, since the
    /// same bindings resolve to the same plan.
    std::mutex lazyMutex;
    std::shared_ptr<detail::PipelineSceneSource> lazy;  ///< Guarded by lazyMutex.

    /// Drops what the bindings resolved: the plan and the lazy source.
    void bindingsChanged() {
        plan.reset();
        std::lock_guard<std::mutex> lock(lazyMutex);
        lazy.reset();
    }
};

namespace detail {

/// What a lazy scene evaluates from: the plan and the bindings that matter to
/// pixels, taken at one moment. Later validate() or binding calls on the Pipeline
/// leave a snapshot unchanged. The mask source and the auxiliaries stay borrowed
/// from the caller; sinks are not part of it. The layers, the budget and the stage
/// name come along so the plan can be resolved again at other levels, tiles can
/// reserve their bytes, and a failure can name the stage.
struct PipelineSnapshot {
    std::shared_ptr<const RunPlan> plan;           ///< Never null.
    std::shared_ptr<slideio::Scene> scene;
    Configuration config;
    PipelineSpec spec;
    std::shared_ptr<const SceneReader> reader;     ///< Never null.
    std::shared_ptr<const LevelSelector> selector; ///< Never null.
    std::vector<LevelDims> levels;
    std::vector<std::pair<std::string, TileRasterSource*>> auxiliaries;
    TileMaskSource* maskSource = nullptr;
    std::vector<std::pair<std::string, LayerId>> layers;
    MemoryBudgetPtr budget;                        ///< Never null.
    std::string stageName;
};

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

    /// The current plan and pixel bindings, validating first when no plan is held.
    /// Throws what Pipeline::validate() throws.
    static PipelineSnapshot snapshot(const Pipeline& pipeline) {
        if (!pipeline.impl_->plan) {
            pipeline.validate();
        }
        const Pipeline::Impl& impl = *pipeline.impl_;
        return PipelineSnapshot{impl.plan,       impl.scene,  impl.config,      impl.spec,
                                impl.reader,     impl.selector, impl.levels,  impl.auxiliaries,
                                impl.maskSource, impl.layers, impl.budget,    impl.stageName};
    }
};

} // namespace detail

} // namespace slideio::processor
