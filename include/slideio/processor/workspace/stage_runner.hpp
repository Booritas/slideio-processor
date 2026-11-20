// Requirements: UR-STG-3, UR-STG-5, UR-STG-6, UR-STG-7, UR-STG-10, UR-EXE-1, UR-EXE-4, UR-EXE-5
#pragma once

/**
 * @file stage_runner.hpp
 * @brief StageRunner: runs one declared stage of a workspace to completion.
 */

#include <slideio/processor/core/control.hpp>
#include <slideio/processor/core/ids.hpp>
#include <slideio/processor/pipeline/export.hpp>
#include <slideio/processor/workspace/workspace.hpp>

#include <cstdint>
#include <memory>

namespace slideio::processor {

/**
 * @brief Runs one stage of a workspace: checks it may run, builds its Pipeline from the
 * stored definition, wires the stage's writers as the sinks, executes across the tile
 * grid, flushes the writers (objects, then edges, then annotations) and records the
 * outcome in the store.
 *
 * run() refuses, before changing anything, a read-only workspace (IoError) and then,
 * with a StageError naming the stage, a stage whose record could not be read, a stage
 * with a dependency that is not Complete or is stale (the message names the first such
 * dependency, in StageRegistry::dependencyAt() order), and a workspace in which another
 * StageRunner is inside run(). A stale stage whose dependencies are current is
 * runnable: running it is how it becomes current.
 *
 * Once running, the stage is set Running. On success every written layer records the
 * run's analysis level, the stage is set Complete and its stale flag is cleared, in
 * that order, after the flushes. An exception raised after the stage was set Running
 * leaves it Failed and propagates.
 *
 * Errors the runner raises itself carry the stage name in ErrorContext::stageName and
 * name the stage in the message. Messages never carry the slide path.
 * @par Thread-safety
 * ThreadConfined; one runner at a time per workspace.
 * @par Requirements
 * UR-STG-3, UR-STG-5, UR-STG-6, UR-STG-7, UR-STG-10, UR-EXE-1, UR-EXE-4, UR-EXE-5
 */
class SLIDEIO_PROCESSOR_ENGINE_EXPORTS StageRunner {
public:
    /**
     * @brief Binds a runner to a stage of a workspace. Runs nothing.
     * @param workspace The workspace; borrowed, must outlive the runner.
     * @param stage The stage, as declared in the workspace's StageRegistry.
     * @throws InvalidArgumentError When the registry does not know @p stage.
     * @throws IoError In a read-only workspace, when re-reading the changed stages fails.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-STG-3
     */
    StageRunner(Workspace& workspace, StageId stage);
    /**
     * @brief Releases the runner. Does not touch the workspace.
     * @throws Nothing.
     */
    ~StageRunner();

    StageRunner(const StageRunner&) = delete;             ///< Not copyable.
    StageRunner& operator=(const StageRunner&) = delete;  ///< Not copyable.

    /**
     * @brief Sets the observer that receives the run's progress, each report naming the
     * stage. Borrowed, may be null, must outlive run(). The observer is called from the
     * run's worker threads.
     * @param observer The observer, or null for none.
     * @return This runner, for chaining.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-EXE-4
     */
    StageRunner& progress(ProgressObserver* observer) noexcept;
    /**
     * @brief Sets the token that cancels the run. The token is copied and shares its flag
     * with the original.
     * @param token The token.
     * @return This runner, for chaining.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-EXE-4
     */
    StageRunner& cancellation(const CancellationToken& token) noexcept;

    /**
     * @brief Runs the stage to completion.
     * @throws IoError When the workspace is read-only, or as the pipeline and the store
     * raise it.
     * @throws StageError When the stage's record is unreadable, a dependency is not
     * Complete or is stale, or another runner of the workspace is inside run(); it names
     * the stage in its context and message.
     * @throws InvalidArgumentError, UnsupportedDataTypeError, ChannelArityError,
     * TileError, CancelledError, InternalError As Pipeline::validate() and
     * Pipeline::runInto() raise them; the stage is left Failed.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-STG-3, UR-STG-5, UR-STG-10, UR-EXE-1, UR-EXE-4, UR-EXE-5
     */
    void run();

    /**
     * @brief Tells whether this run replaces an earlier one: the stage's status at the
     * start of run() was not Pending (Complete, Failed, or Running left by a process that
     * died mid-run). Before run() it answers for the status at construction.
     * @return True for a re-run.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-STG-6
     */
    bool isRerun() const noexcept;
    /**
     * @brief The objects the last run committed, taken after the flushes; 0 before run()
     * and after a failed run.
     * @return The count.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-STG-3
     */
    int64_t objectsWritten() const noexcept;
    /**
     * @brief The edges the last run asserted, taken after the flushes; 0 before run() and
     * after a failed run.
     * @return The count.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-STG-3
     */
    int64_t edgesAsserted() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace slideio::processor
