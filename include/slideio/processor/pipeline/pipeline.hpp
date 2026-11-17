// Requirements: UR-EXE-1, UR-ALG-1, UR-ALG-5, UR-PIX-6, UR-IMG-5, UR-STG-3
#pragma once

/**
 * @file pipeline.hpp
 * @brief Pipeline: a PipelineSpec bound to a scene, a configuration, a budget
 * and the sinks of one run.
 */

#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/block_view.hpp>
#include <slideio/processor/core/control.hpp>
#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/core/ids.hpp>
#include <slideio/processor/core/memory_budget.hpp>
#include <slideio/processor/pipeline/export.hpp>
#include <slideio/processor/pipeline/pipeline_spec.hpp>

#include <cstdint>
#include <memory>

namespace slideio {
class Scene;
} // namespace slideio

namespace slideio::processor {

namespace detail {
struct PipelineAccess;
} // namespace detail

/**
 * @brief A spec bound to a scene, a configuration and a budget, with the
 * runtime bindings of one run.
 *
 * Everything about what to compute comes from the PipelineSpec, copied at
 * construction; everything set here is about where and with what resources:
 * the memory budget, the sinks, the auxiliary rasters, the mask source, the
 * layer bindings and the stage name. validate() checks the whole run against
 * the scene's metadata, the catalog's declarations and the bindings before any
 * pixel is read, so a mis-specified stage fails in seconds rather than hours.
 *
 * The scene type is spelled `std::shared_ptr<slideio::Scene>` because SlideIO
 * defines `ScenePtr` as a preprocessor macro.
 * @par Thread-safety
 * ThreadConfined for configuration; the scene returned by asScene() is
 * ThreadSafe for reads.
 * @par Requirements
 * UR-EXE-1, UR-ALG-1, UR-ALG-5, UR-PIX-6, UR-IMG-5, UR-STG-3
 */
class SLIDEIO_PROCESSOR_ENGINE_EXPORTS Pipeline {
public:
    /**
     * @brief Binds a spec to a scene and a configuration. Reads no pixels and
     * resolves nothing; validate() does that.
     * @param scene The source scene; shared, never null.
     * @param config The configuration; copied. Supplies the tile size and,
     * until memoryBudget() is called, the memory ceiling.
     * @param spec The spec; copied.
     * @throws InvalidArgumentError When @p scene is null.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-EXE-1
     */
    Pipeline(std::shared_ptr<slideio::Scene> scene, const Configuration& config, const PipelineSpec& spec);
    /**
     * @brief Releases the pipeline. Borrowed sinks and sources are not touched.
     * @throws Nothing.
     */
    ~Pipeline();

    /**
     * @brief Sets the memory budget the run reserves its buffers from, and
     * whose ceiling validate() checks the run against.
     * @param budget The budget; null restores the default, a budget whose
     * ceiling is the configuration's memoryCeilingBytes().
     * @return This pipeline, for chaining.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-ALG-5
     */
    Pipeline& memoryBudget(MemoryBudgetPtr budget);

    /**
     * @brief Where operations assert containment. Borrowed, may be null, must
     * outlive the run. `StageRunner` passes the stage's `EdgeWriter`.
     *
     * Operations call the sink concurrently from the run's worker threads, so
     * it must be ThreadSafe. Its flush() is called once, after every worker
     * has stopped, and only when the run succeeds.
     * @param sink The sink, or null for none.
     * @return This pipeline, for chaining.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-STG-3
     */
    Pipeline& edgeSink(EdgeSink* sink);

    /**
     * @brief Where operations attach labels and measurements to objects that
     * already exist. Borrowed, may be null, must outlive the run.
     * `StageRunner` passes the stage's `AnnotationWriter`.
     *
     * Operations call the sink concurrently from the run's worker threads, so
     * it must be ThreadSafe. Its flush() is called once, after every worker
     * has stopped, and only when the run succeeds.
     * @param sink The sink, or null for none.
     * @return This pipeline, for chaining.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-STG-3
     */
    Pipeline& annotationSink(AnnotationSink* sink);

    /**
     * @brief Where the chain's final raster output goes, one tile at a time.
     * Borrowed, may be null, must outlive the run. `StageRunner` passes the
     * stage's `RasterWriter` when it declared `writesRaster`; a run with
     * none discards its final raster.
     * @param sink The sink, or null for none.
     * @return This pipeline, for chaining.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-EXE-1
     */
    Pipeline& rasterSink(RasterSink* sink);

    /**
     * @brief A second raster input, addressed by operations as `name` and read at
     * the run's analysis level. Borrowed, may be null, must outlive the run.
     * `StageRunner` passes one per `readsLayerAsRaster` declaration;
     * a direct caller passes its own or none. Callable more than once with
     * different names; the same name twice replaces.
     * @param name UTF-8 binding name, non-null and non-empty; copied.
     * @param source The source, or null to remove the binding of @p name.
     * @return This pipeline, for chaining.
     * @throws InvalidArgumentError When @p name is null or empty.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-EXE-1
     */
    Pipeline& auxiliaryRaster(const char* name, TileRasterSource* source);

    /**
     * @brief The run's restriction to the objects of some region, resolved one
     * tile at a time. Borrowed, may be null, must outlive the run.
     * `StageRunner` passes a source over the stage's restriction layer; a
     * direct caller passes null or its own. It never narrows the tile grid.
     * @param source The source, or null for none.
     * @return This pipeline, for chaining.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-IMG-5, UR-STG-3
     */
    Pipeline& maskSource(TileMaskSource* source);

    /**
     * @brief Binds a layer name to an identifier for this run. Object-producing
     * operations name their target layer as a string; this is the only
     * mapping the pipeline has. `StageRunner` calls it once per declared
     * layer; a direct caller calls it itself. The same name twice replaces.
     * @param name UTF-8 layer name, non-null and non-empty; copied.
     * @param layer The identifier; must be valid.
     * @return This pipeline, for chaining.
     * @throws InvalidArgumentError When @p name is null or empty, or @p layer is invalid.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-STG-3
     */
    Pipeline& bindLayer(const char* name, LayerId layer);

    /**
     * @brief Names the run for progress reports and errors: the `stageName`
     * passed to ProgressObserver::onProgress and put in every TileError's
     * ErrorContext. Defaults to "pipeline"; `StageRunner` sets the stage's name.
     * @param name UTF-8 name, non-null and non-empty; copied.
     * @return This pipeline, for chaining.
     * @throws InvalidArgumentError When @p name is null or empty.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-EXE-1
     */
    Pipeline& stageName(const char* name);

    /**
     * @brief The spec this pipeline was constructed from, copied at construction.
     * @return The spec; valid for the pipeline's lifetime.
     * @throws Nothing.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-EXE-1
     */
    const PipelineSpec& spec() const noexcept;

    /**
     * @brief The pyramid level this run reads from, chosen by `LevelSelector`.
     * Valid after validate(); recorded against every layer the run
     * writes, so a consumer can tell what the geometry's precision is.
     *
     * The spec's level when it names one. An automatic level (-1) resolves to
     * level 0 for a chain of Halo operations, and for a chain containing a
     * WholeLevel operation to the finest level whose whole-level memory need
     * fits the budget.
     * @return The level.
     * @throws InvalidArgumentError When validate() has not succeeded since
     * construction or the last binding call.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-EXE-1
     */
    int32_t analysisLevel() const;

    /**
     * @brief Lazy: evaluated per requested block. Construction computes no pixels,
     * and the result is a valid input to another Pipeline.
     * @return The scene.
     * @throws InternalError Always, until the lazy mode is implemented.
     * @par Thread-safety
     * ThreadConfined; the returned scene is ThreadSafe for reads.
     * @par Requirements
     * UR-EXE-1, UR-PIX-6
     */
    std::shared_ptr<slideio::Scene> asScene() const;

    /**
     * @brief Eager: streams the whole slide or ROI, sending emitted objects to
     * @p sink. Calls validate() first.
     *
     * Tiles run on a WorkerPool of the configuration's workerCount() threads,
     * each reserving its buffers from the memory budget while it is evaluated, so
     * the budget bounds how many tiles are in memory at once. The object, edge,
     * annotation and raster sinks are called concurrently from those worker
     * threads, so each must be ThreadSafe. On success, once every worker has stopped,
     * the object, edge, annotation and raster sinks are flushed, once each, in
     * that order; on failure or cancellation none is, and no worker thread
     * outlives the call.
     *
     * Objects cut by tile seams are merged during the run, as
     * TileExecutor::run() describes, so each reaches @p sink once and whole,
     * before the sinks are flushed. The run's scratch directory,
     * `pipeline-<32 random lowercase hex digits>` under the configuration's
     * scratch directory, exists only while a chain producing objects runs and is
     * removed on every path.
     * @param sink Receives the objects the chain emits; called concurrently.
     * @param progress Receives progress reports; may be null.
     * @param token Cancels the run.
     * @throws InvalidArgumentError As validate().
     * @throws UnsupportedDataTypeError As validate().
     * @throws ChannelArityError As validate().
     * @throws IoError As validate(); when the run's scratch directory cannot
     * be created; when merged fragments cannot be read back from it.
     * @throws TileError When a tile fails: the stage, the operation when one
     * threw, the tile's column and row (hasTile() is true), and the original
     * text as the cause. Also when creating or finalising a global-context
     * operation fails; hasTile() is then false. Nothing is flushed then.
     * @throws CancelledError When @p token is cancelled or an operation throws
     * it; nothing is flushed.
     * @throws InternalError When a tile failed and its failure could not be
     * recorded, when seam fragments do not merge, or as validate(); nothing is
     * flushed.
     * @throws std::bad_alloc When allocation fails outside a tile.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-EXE-1, UR-ALG-5, UR-IMG-5, UR-STG-3, UR-SEG-3
     */
    void runInto(ObjectSink& sink, ProgressObserver* progress, const CancellationToken& token);
    /**
     * @brief Eager, discarding emitted objects: runInto() with a sink that counts and drops.
     * @param progress Receives progress reports; may be null.
     * @param token Cancels the run.
     * @throws InvalidArgumentError As validate().
     * @throws UnsupportedDataTypeError As validate().
     * @throws ChannelArityError As validate().
     * @throws IoError As validate().
     * @throws TileError As runInto(): hasTile() is true for a tile failure and
     * false for a failure creating or finalising a global-context operation.
     * Nothing is flushed then.
     * @throws CancelledError When @p token is cancelled or an operation throws
     * it; nothing is flushed.
     * @throws InternalError When a tile failed and its failure could not be
     * recorded, or as validate(); nothing is flushed.
     * @throws std::bad_alloc When allocation fails outside a tile.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-EXE-1, UR-ALG-5
     */
    void run(ProgressObserver* progress, const CancellationToken& token);

    /**
     * @brief The number of pyramid levels of the output scene.
     * @return The count.
     * @throws InternalError Always, until the lazy mode is implemented.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-PIX-6
     */
    int32_t levelCount() const;
    /**
     * @brief The extent of one output level.
     * @param level The level.
     * @return The rectangle, in that level's pixels.
     * @throws InternalError Always, until the lazy mode is implemented.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-PIX-6
     */
    Rect64 rect(int32_t level) const;
    /**
     * @brief Computes one block of the output.
     * @param level The level.
     * @param region The block, in that level's pixels.
     * @param out Receives the block.
     * @throws InternalError Always, until the lazy mode is implemented.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-PIX-6
     */
    void readBlock(int32_t level, const Rect64& region, BlockView& out) const;

    /**
     * @brief Validates declared types, arity and halo against the scene. Called by
     * run(); exposed so a caller can check before committing to a long run.
     *
     * Reads no pixels. In order: the spec has an operation; every operation
     * name is registered; explicit channels and an explicit level are in range;
     * the region (scene, ROI and polygon bounding box intersected) is not
     * empty; every operation accepts the plane types it receives, the scene's
     * channel types for the first and each predecessor's output types after;
     * every object-producing operation's target layer resolves against
     * bindLayer(); every bound auxiliary raster declares at least one plane of
     * a type with a size; a chain with a WholeLevel operation fits the memory
     * ceiling as one tile; and one tile's buffers fit the ceiling. The result
     * is kept until the next binding call or validate().
     * @throws InvalidArgumentError When the spec is empty, an operation is
     * unknown or its parameters are refused, a channel or the level is out of
     * range, the restriction does not intersect the scene, a target layer is
     * unnamed, unbound or ambiguous, an auxiliary raster declares no plane or
     * a plane type without a size (the message names its binding), a level of
     * the scene has a non-positive size, or the run does not fit the memory
     * ceiling.
     * @throws UnsupportedDataTypeError When an operation does not accept a type
     * it would receive, or a selected channel has a type the pipeline cannot
     * read; the message names the operation or the channel, and the type.
     * @throws ChannelArityError When a Joint operation requiring uniform types
     * would receive mixed types.
     * @throws IoError When SlideIO fails to describe the scene or to report a
     * level; the cause is preserved.
     * @throws InternalError When an operation's factory misbehaves, as
     * OperationCatalog::create() reports it.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-EXE-1, UR-IMG-5, UR-STG-3
     */
    void validate() const;

private:
    friend struct detail::PipelineAccess;
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace slideio::processor
