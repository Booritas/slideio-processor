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
 * grid, flushes the writers (objects, then edges, then annotations, then the raster,
 * last) and records the outcome in the store.
 *
 * run() refuses, before changing anything, a read-only workspace (IoError) and then,
 * with a StageError naming the stage, a workspace in which another StageRunner is inside
 * run(), a stage whose record could not be read, a stage with a dependency that is not
 * Complete or is stale (the message names the first such dependency, in
 * StageRegistry::dependencyAt() order), and a stage whose definition has an operation
 * producing objects that names no layer the stage writes (or none, with the stage
 * writing other than one layer). A stale stage whose dependencies are current is
 * runnable: running it is how it becomes current.
 *
 * Once past those refusals, a re-run first marks every direct dependent stale; then the
 * stage is set Running; then a re-run discards the stage's prior objects, edges,
 * annotations and raster. On success every written layer records the run's analysis
 * level, the stage is set Complete and its stale flag is cleared, in that order, after
 * the flushes. Any failure from the re-run preparation on leaves the stage Failed, best
 * effort, and propagates.
 *
 * Errors the runner raises itself carry the stage name in ErrorContext::stageName and
 * name the stage in the message. The runner adds no slide path to any message; a
 * wrapped error's ErrorContext::cause carries the original error's text unchanged.
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
     * @throws IoError When the workspace is read-only; nothing is changed.
     * @throws StageError When another runner of the workspace is inside run(), the
     * stage's record is unreadable, a dependency is not Complete or is stale, or an
     * operation that produces objects names no layer the stage writes (or none, with the
     * stage writing other than one layer); nothing is changed, and for the last case the
     * error names the operation too. Also for every failure of the run itself, from the
     * re-run preparation to the final status write: the stage is left Failed (best
     * effort) and the error names the stage, carries the original error's text as its
     * cause, and copies the operation and tile when the original names them. A
     * StageError raised inside the run (a nested runner's refusal) propagates unchanged.
     * @throws CancelledError When the run's token is cancelled; it names the stage and
     * the stage is left Failed.
     * @throws std::bad_alloc When allocation fails, unchanged; once the run has started
     * the stage is left Failed.
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
