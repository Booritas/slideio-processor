// Requirements: UR-EXE-2, UR-EXE-3, UR-EXE-4, UR-EXE-5
#pragma once

/**
 * @file executor.hpp
 * @brief WorkerPool and TileExecutor: the threads a run evaluates its tiles on,
 * and the dispatch of one run's tiles over them.
 */

#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/control.hpp>
#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/core/memory_budget.hpp>
#include <slideio/processor/pipeline/export.hpp>
#include <slideio/processor/pipeline/pipeline.hpp>
#include <slideio/processor/pipeline/tile_grid.hpp>

#include <cstdint>
#include <memory>

namespace slideio::processor {

class TileExecutor;

/**
 * @brief A fixed set of worker threads and the memory budget their tiles
 * reserve from.
 *
 * The threads start in the constructor and stop in shutdown(), which the
 * destructor calls, so no thread outlives the pool. A TileExecutor runs its
 * tiles on them; before evaluating a tile a worker reserves the tile's buffer
 * bytes from the budget, waiting while they do not fit, and releases them when
 * the tile is done, so the budget bounds how many tiles are in memory at once.
 * @par Thread-safety
 * ThreadSafe.
 * @par Requirements
 * UR-EXE-2, UR-EXE-3, UR-EXE-4
 */
class SLIDEIO_PROCESSOR_ENGINE_EXPORTS WorkerPool {
public:
    /**
     * @brief Starts @p workerCount threads.
     * @param workerCount The number of threads, at least 1; 1 runs a single
     * worker (UR-EXE-3).
     * @param budget The budget tiles reserve their buffers from; never null.
     * @throws InvalidArgumentError When @p workerCount is below 1 or @p budget is null.
     * @throws InternalError When a thread cannot be started; the threads already
     * started are stopped first.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-EXE-3, UR-EXE-4
     */
    WorkerPool(int32_t workerCount, MemoryBudgetPtr budget);
    /**
     * @brief Calls shutdown(): finishes the queued work and joins every thread.
     * @throws Nothing.
     * @par Thread-safety
     * Not concurrently with any other member; not from a worker thread of this pool.
     * @par Requirements
     * UR-EXE-4
     */
    ~WorkerPool();

    /**
     * @brief The number of threads the pool was built with.
     * @return The count, at least 1.
     * @throws Nothing.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-EXE-3
     */
    int32_t workerCount() const noexcept;
    /**
     * @brief Blocks until no work is queued or running. Returns at once after
     * shutdown().
     * @throws InternalError When a synchronisation primitive fails.
     * @par Thread-safety
     * Safe to call concurrently, but not from a worker thread of this pool.
     * @par Requirements
     * UR-EXE-4
     */
    void waitIdle();
    /**
     * @brief Stops the pool: the work already queued finishes, then every thread
     * is joined. Idempotent; later runs on the pool throw InvalidArgumentError.
     * @throws InternalError When a synchronisation primitive fails.
     * @par Thread-safety
     * Safe to call concurrently, but not from a worker thread of this pool.
     * @par Requirements
     * UR-EXE-4
     */
    void shutdown();

private:
    friend class TileExecutor;
    class Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * @brief Runs one Pipeline's tiles on a WorkerPool.
 *
 * Tile indices are handed out in ascending order; tiles complete in any order,
 * and nothing downstream depends on it. Before each tile the cancellation token
 * is checked, then the tile's buffer bytes are reserved from the pool's budget
 * (waiting while they do not fit) and released when the tile is done.
 *
 * The first tile failure stops the dispatch; the tiles already running finish,
 * and the failure is rethrown as a TileError naming the stage, the operation
 * when one threw, the tile's column and row, and the original text as the
 * cause. A CancelledError, from the token or from an operation, is rethrown as
 * is. No sink is flushed by run(): flushing on success is Pipeline::runInto's.
 *
 * Progress is reported after every tile, skipped tiles included, as
 * `onProgress(stageName, completed / total, completed, total)`; the calls never
 * overlap and `completed` increases by one each time.
 * @par Thread-safety
 * ThreadSafe: concurrent run() calls on one executor are serialised, and
 * tilesCompleted() and tilesTotal() may be read while a run is in progress.
 * run() updates the pipeline's per-run state (its validation result, its
 * global-context instances and its counters), so no other thread may use that
 * pipeline, through this executor or any other, while run() is in progress.
 * @par Requirements
 * UR-EXE-2, UR-EXE-3, UR-EXE-4, UR-EXE-5
 */
class SLIDEIO_PROCESSOR_ENGINE_EXPORTS TileExecutor {
public:
    /**
     * @brief Binds a grid to a pool. Starts nothing.
     * @param grid The tiles to run; copied. Must be the grid of the pipeline
     * passed to run().
     * @param pool The pool to run on; borrowed, must outlive the executor.
     * @param config The configuration; copied.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-EXE-4
     */
    TileExecutor(const TileGrid& grid, WorkerPool& pool, const Configuration& config);
    /**
     * @brief Releases the executor. A run in progress must have returned.
     * @throws Nothing.
     * @par Thread-safety
     * Not concurrently with any other member.
     * @par Requirements
     * UR-EXE-4
     */
    ~TileExecutor();

    /**
     * @brief Evaluates every tile of @p pipeline on the pool and returns when all
     * of them are done, or when the run has failed and the tiles in flight have
     * finished. Validates the pipeline first when it has not been validated
     * since its last binding call.
     *
     * A chain with global-context operations runs one accumulation pass over
     * every tile per such operation, in chain order, then the final pass. An
     * accumulation pass applies the operations before it, accumulates each tile
     * into a fresh instance of the operation, combines those instances in
     * ascending tile index (a skipped tile combining nothing), and finalises;
     * it writes no sink. Results therefore do not depend on the worker count.
     *
     * A global-context operation's instances keep the state its
     * Operation::accumulatedStateBytes() declares. Before the first pass, the run
     * reserves from the pool's budget, in one wait, the state of every such
     * operation's run instance plus the most the tile instances of any one
     * accumulation pass may hold, and releases it when run() returns, on every
     * path. A pass holds at most 2 x workers tile instances, fewer when what the
     * ceiling leaves beside one tile and the run instances' state holds fewer;
     * a worker that finds a pass at that bound waits until one is combined.
     *
     * When an operation of the chain produces objects, the run joins the
     * fragments of objects cut by tile seams with a SeamMerger, which works in
     * a directory of the run's own, `pipeline-<32 random lowercase hex digits>`
     * under the configuration's scratch directory (the platform's temporary
     * directory when that is empty). Every tile of the final pass, a skipped one
     * included, is reported to it as it completes, so merged objects reach
     * @p sink during the pass; whatever remains is resolved after the last tile.
     * A merged object is stored only when the restriction admits its anchor.
     * The directory is removed before run() returns or throws, on every path; a
     * chain producing no objects creates none.
     *
     * The object sink and the pipeline's edge, annotation and raster sinks are
     * called concurrently from the pool's worker threads, so each must be
     * ThreadSafe. run() flushes none of them; Pipeline::runInto flushes each
     * once, after run() has returned and every worker has stopped, and only on
     * success.
     * @param pipeline The pipeline; its bindings (sinks, auxiliaries, mask
     * source, layers, stage name) are used as they stand. No other thread may
     * use it while run() is in progress.
     * @param sink Receives the objects the chain emits; called concurrently,
     * not flushed.
     * @param progress Receives one call per tile of every pass; may be null.
     * @param token Cancels the run.
     * @throws InvalidArgumentError As Pipeline::validate(); when the pipeline's
     * grid differs from this executor's; when one tile's bytes exceed the
     * pool's budget ceiling; when one tile's bytes with the global-context
     * operations' accumulated state and one more instance of the largest exceed
     * the pool's budget ceiling; when the pool has been shut down.
     * @throws UnsupportedDataTypeError As Pipeline::validate().
     * @throws ChannelArityError As Pipeline::validate().
     * @throws IoError As Pipeline::validate(); when the run's scratch
     * directory cannot be created, its path not being formable (not UTF-8,
     * say) included, with the reason as the cause; as SeamMerger::resolve().
     * @throws TileError When a tile fails, merging the objects its completion
     * closes included; hasTile() is true, tileX() and tileY() are its column
     * and row. When creating or finalising a global-context operation fails, or
     * when the seam merge after the last tile (SeamMerger::resolve()) throws
     * anything but a ProcessorError or std::bad_alloc, such as a standard
     * exception from @p sink, with its what() as the cause; hasTile() is then
     * false.
     * @throws CancelledError When @p token is cancelled or an operation throws it.
     * @throws InternalError When a tile failed and its failure could not be
     * recorded, or as Pipeline::validate() or SeamMerger::resolve().
     * @throws std::bad_alloc When allocation fails outside a tile.
     * @par Thread-safety
     * Safe to call concurrently; runs on one executor are serialised. The
     * pipeline must not be used by any other thread during the call. Concurrent
     * runs with global-context operations whose executors share one budget,
     * through one pool, whose ceiling is below the sum of their run reservations
     * plus one tile may wait on each other until cancelled.
     * @par Requirements
     * UR-EXE-1, UR-EXE-2, UR-EXE-4, UR-EXE-5, UR-QA-3, UR-SEG-3
     */
    void run(Pipeline& pipeline, ObjectSink& sink, ProgressObserver* progress, const CancellationToken& token);

    /**
     * @brief The tiles the current or last run has completed, skipped ones included.
     * @return The count; 0 before the first run.
     * @throws Nothing.
     * @par Thread-safety
     * Safe to call concurrently, also during run().
     * @par Requirements
     * UR-EXE-4
     */
    int64_t tilesCompleted() const noexcept;
    /**
     * @brief The tile evaluations of the current or last run: the grid's tile
     * count times the passes (one per global-context operation, plus the final
     * pass).
     * @return The count; the grid's tile count before the first run.
     * @throws Nothing.
     * @par Thread-safety
     * Safe to call concurrently, also during run().
     * @par Requirements
     * UR-EXE-4
     */
    int64_t tilesTotal() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace slideio::processor
