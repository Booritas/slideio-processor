// Requirements: UR-EXE-1, UR-EXE-2, UR-EXE-3, UR-EXE-4, UR-EXE-5, UR-QA-3, UR-SEG-3
#include <slideio/processor/pipeline/executor.hpp>

#include <pipeline/detail/fragment_spill.hpp>
#include <pipeline/detail/ordered_fold.hpp>
#include <pipeline/detail/pipeline_impl.hpp>
#include <pipeline/detail/restriction.hpp>
#include <pipeline/detail/seam_merger_access.hpp>

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/pipeline/operation_catalog.hpp>
#include <slideio/processor/pipeline/seam_merger.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace slideio::processor {

namespace {

[[noreturn]] void synchronisationFailed(const char* where, const std::system_error& e) {
    ErrorContext context;
    context.cause = e.what();
    throw InternalError((std::string(where) + ": a synchronisation primitive failed").c_str(), context);
}

// Two grids lay out the same tiles.
bool sameGrid(const TileGrid& a, const TileGrid& b) {
    if (a.level() != b.level() || a.columns() != b.columns() || a.rows() != b.rows() ||
        a.tileCount() != b.tileCount()) {
        return false;
    }
    if (a.tileCount() == 0) {
        return true;
    }
    const int64_t last = a.tileCount() - 1;
    return a.tileRectAtLevel(0) == b.tileRectAtLevel(0) && a.tileRectAtLevel(last) == b.tileRectAtLevel(last);
}

// Holds bytes reserved from a budget and releases them on scope exit.
class Reservation {
public:
    Reservation(MemoryBudget& budget, int64_t bytes) : budget_(budget), bytes_(bytes) {}
    ~Reservation() {
        try {
            budget_.release(bytes_);
        } catch (...) {
            // release() refuses only bytes it never reserved, which this scope cannot cause.
        }
    }
    Reservation(const Reservation&) = delete;
    Reservation& operator=(const Reservation&) = delete;

private:
    MemoryBudget& budget_;
    int64_t bytes_;
};

// A directory of one run's own, `<base>/pipeline-<32 random lowercase hex>`
// (the platform's temporary directory when `base` is empty), created on
// construction and removed with everything in it on destruction.
class RunScratchDirectory {
public:
    explicit RunScratchDirectory(const char* base) {
        std::error_code error;
        std::filesystem::path root;
        if (base == nullptr || base[0] == '\0') {
            root = std::filesystem::temp_directory_path(error);
        } else {
            root = std::filesystem::u8path(base);
        }
        if (!error) {
            path_ = root / ("pipeline-" + detail::randomHexName());
            std::filesystem::create_directories(path_, error);
        }
        if (error) {
            const std::string cause = error.message();
            ErrorContext context;
            context.cause = cause.c_str();
            throw IoError(("TileExecutor::run: cannot create a scratch directory under '" +
                           (base != nullptr ? std::string(base) : std::string()) + "'")
                              .c_str(),
                          context);
        }
        text_ = path_.u8string();
    }
    ~RunScratchDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    RunScratchDirectory(const RunScratchDirectory&) = delete;
    RunScratchDirectory& operator=(const RunScratchDirectory&) = delete;

    /// The directory, UTF-8.
    const char* path() const noexcept { return text_.c_str(); }

private:
    std::filesystem::path path_;
    std::string text_;
};

// What the workers of one pass share. Held by shared_ptr by run() and by every
// job, so a job that signals done after run() has seen jobsLeft reach 0 and
// returned still touches a live object.
struct RunState {
    std::atomic<int64_t> next{0};       ///< The next tile index to hand out.
    std::atomic<bool> stop{false};      ///< Set by the first failure: hand out no more tiles.
    /// Set when a failure could not be recorded (its lock or its copy failed): the
    /// pass failed, but `failure` may not say how.
    std::atomic<bool> failureLost{false};

    std::mutex failureMutex;
    std::exception_ptr failure;         ///< The first failure; later ones are dropped.
    int64_t failedTile = -1;
    bool hasOperation = false;
    std::string failedOperation;

    std::mutex progressMutex;           ///< Serialises the completed count and the observer.

    std::mutex doneMutex;
    std::condition_variable done;
    int32_t jobsLeft = 0;

    // Never throws: the run stops first, and a failure that cannot be stored is
    // flagged instead, so run() still fails.
    void recordFailure(std::exception_ptr error, int64_t tile, const char* operation) noexcept {
        stop = true;
        try {
            std::lock_guard<std::mutex> lock(failureMutex);
            if (!failure) {
                failedOperation = operation != nullptr ? operation : "";
                hasOperation = operation != nullptr;
                failedTile = tile;
                failure = std::move(error);
            }
        } catch (...) {
            failureLost = true;
        }
    }

    // The last thing a job does. noexcept: a failure here would leave run() waiting
    // forever, so it terminates instead.
    void jobDone() noexcept {
        std::lock_guard<std::mutex> lock(doneMutex);
        --jobsLeft;
        done.notify_all();
    }
};

// Signals a job done on every way out of it, an exception included.
class JobDoneGuard {
public:
    explicit JobDoneGuard(RunState& state) noexcept : state_(state) {}
    ~JobDoneGuard() { state_.jobDone(); }
    JobDoneGuard(const JobDoneGuard&) = delete;
    JobDoneGuard& operator=(const JobDoneGuard&) = delete;

private:
    RunState& state_;
};

// Rethrows `failure` as the run reports it: a CancelledError as is, anything
// else as a TileError naming the stage, the operation and, when there is one, the tile.
[[noreturn]] void reportFailure(const std::exception_ptr& failure, const std::string& stageName,
                                const char* operationName, const TileGrid& grid, int64_t tile) {
    std::string cause;
    try {
        std::rethrow_exception(failure);
    } catch (const CancelledError&) {
        throw;
    } catch (const std::exception& e) {
        cause = e.what();
    } catch (...) {
        cause = "an exception that is not a std::exception";
    }
    ErrorContext context;
    context.stageName = stageName.c_str();
    context.operationName = operationName;
    context.cause = cause.c_str();
    if (tile >= 0) {
        context.hasTile = true;
        context.tileX = tile % grid.columns();
        context.tileY = tile / grid.columns();
        throw TileError("a tile failed", context);
    }
    throw TileError("a global-context accumulation failed", context);
}

} // namespace

// --- WorkerPool ---------------------------------------------------------------------

class WorkerPool::Impl {
public:
    Impl(int32_t count, MemoryBudgetPtr budgetIn) : workerCount(count), budget(std::move(budgetIn)) {}

    const int32_t workerCount;
    const MemoryBudgetPtr budget;

    std::mutex mutex;
    std::condition_variable wake;       ///< Work queued, or stopping.
    std::condition_variable idle;       ///< Nothing queued or running.
    std::deque<std::function<void()>> queue;
    int32_t running = 0;
    bool stopping = false;

    std::mutex joinMutex;               ///< Serialises the joins of concurrent shutdown() calls.
    std::vector<std::thread> threads;

    // A worker: runs queued jobs until stopping and the queue is drained. Jobs
    // report their own failures; anything escaping one is dropped, never the thread.
    void loop() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [this] { return stopping || !queue.empty(); });
                if (queue.empty()) {
                    return;
                }
                job = std::move(queue.front());
                queue.pop_front();
                ++running;
            }
            try {
                job();
            } catch (...) {
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                --running;
                if (queue.empty() && running == 0) {
                    idle.notify_all();
                }
            }
        }
    }

    void submit(std::function<void()> job) {
        try {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (stopping) {
                    throw InvalidArgumentError("WorkerPool: the pool has been shut down");
                }
                queue.push_back(std::move(job));
            }
            wake.notify_one();
        } catch (const std::system_error& e) {
            synchronisationFailed("WorkerPool", e);
        }
    }

    void stop() {
        try {
            {
                std::lock_guard<std::mutex> lock(mutex);
                stopping = true;
            }
            wake.notify_all();
            std::lock_guard<std::mutex> joinLock(joinMutex);
            for (std::thread& thread : threads) {
                if (thread.joinable()) {
                    thread.join();
                }
            }
            threads.clear();
        } catch (const std::system_error& e) {
            synchronisationFailed("WorkerPool::shutdown", e);
        }
    }
};

WorkerPool::WorkerPool(int32_t workerCount, MemoryBudgetPtr budget) {
    if (workerCount < 1) {
        throw InvalidArgumentError(
            ("WorkerPool: workerCount must be at least 1 (got " + std::to_string(workerCount) + ")").c_str());
    }
    if (!budget) {
        throw InvalidArgumentError("WorkerPool: the memory budget is null");
    }
    impl_ = std::make_unique<Impl>(workerCount, std::move(budget));
    try {
        impl_->threads.reserve(static_cast<size_t>(workerCount));
        for (int32_t i = 0; i < workerCount; ++i) {
            impl_->threads.emplace_back([impl = impl_.get()] { impl->loop(); });
        }
    } catch (const std::system_error& e) {
        impl_->stop();
        ErrorContext context;
        context.cause = e.what();
        throw InternalError("WorkerPool: a worker thread could not be started", context);
    } catch (...) {
        impl_->stop();
        throw;
    }
}

WorkerPool::~WorkerPool() {
    try {
        shutdown();
    } catch (...) {
        // Only a failed synchronisation primitive gets here; nothing more can be done.
    }
}

int32_t WorkerPool::workerCount() const noexcept { return impl_->workerCount; }

void WorkerPool::waitIdle() {
    try {
        std::unique_lock<std::mutex> lock(impl_->mutex);
        impl_->idle.wait(lock, [this] { return impl_->queue.empty() && impl_->running == 0; });
    } catch (const std::system_error& e) {
        synchronisationFailed("WorkerPool::waitIdle", e);
    }
}

void WorkerPool::shutdown() { impl_->stop(); }

// --- TileExecutor -------------------------------------------------------------------

class TileExecutor::Impl {
public:
    Impl(const TileGrid& gridIn, WorkerPool& poolIn, const Configuration& configIn)
        : grid(gridIn), pool(poolIn), config(configIn), total(gridIn.tileCount()) {}

    const TileGrid grid;
    WorkerPool& pool;
    const Configuration config;
    std::atomic<int64_t> completed{0};
    std::atomic<int64_t> total;         ///< Passes × tiles of the current or last run.
    std::mutex runMutex;                ///< Serialises run().
};

TileExecutor::TileExecutor(const TileGrid& grid, WorkerPool& pool, const Configuration& config)
    : impl_(std::make_unique<Impl>(grid, pool, config)) {}

TileExecutor::~TileExecutor() = default;

int64_t TileExecutor::tilesCompleted() const noexcept { return impl_->completed.load(); }

int64_t TileExecutor::tilesTotal() const noexcept { return impl_->total.load(); }

void TileExecutor::run(Pipeline& pipeline, ObjectSink& sink, ProgressObserver* progress,
                       const CancellationToken& token) {
    std::unique_lock<std::mutex> runLock;
    try {
        runLock = std::unique_lock<std::mutex>(impl_->runMutex);
    } catch (const std::system_error& e) {
        synchronisationFailed("TileExecutor::run", e);
    }

    auto& state = detail::PipelineAccess::impl(pipeline);
    if (!state.plan) {
        pipeline.validate();
    }
    // The run holds the plan it started with; the plan is never written.
    const std::shared_ptr<const detail::RunPlan> heldPlan = state.plan;
    const detail::RunPlan& plan = *heldPlan;
    if (!sameGrid(plan.grid, impl_->grid)) {
        throw InvalidArgumentError("TileExecutor::run: the pipeline's tile grid differs from the executor's");
    }
    WorkerPool::Impl& pool = *impl_->pool.impl_;
    MemoryBudget& budget = *pool.budget;
    // The plan's tileBytes bounds every tile's peak buffers, in every pass; reserve it whole.
    const int64_t tileBytes = plan.tileBytes;
    if (tileBytes > budget.ceilingBytes()) {
        throw InvalidArgumentError(("TileExecutor::run: one tile needs " + std::to_string(tileBytes) +
                                    " bytes, above the pool's memory ceiling of " +
                                    std::to_string(budget.ceilingBytes()) + " bytes")
                                       .c_str());
    }

    // One accumulation pass per global-context operation, then the final pass.
    const std::vector<size_t>& globalPositions = plan.globalContextPositions;
    const int64_t tileCount = impl_->grid.tileCount();
    const int64_t total = static_cast<int64_t>(globalPositions.size() + 1) * tileCount;
    const OperationCatalog& catalog = OperationCatalog::instance();
    const std::string& stageName = state.stageName;
    impl_->completed = 0;
    impl_->total = total;
    state.counters.reset();

    // Each run accumulates into fresh instances of its own, never on top of an
    // earlier run's and never in the plan: the run applies the plan's operations
    // with its fresh instances in their positions.
    std::vector<OperationPtr> operations = plan.operations;
    for (size_t g : globalPositions) {
        const auto k = static_cast<int32_t>(g);
        const char* name = state.spec.operationName(k);
        try {
            operations[g] = catalog.create(name, state.spec.operationParameters(k));
        } catch (...) {
            reportFailure(std::current_exception(), stageName, name, impl_->grid, -1);
        }
    }
    state.lastRunOperations = operations;

    // A chain that produces objects gets a seam merger for the run, working in a
    // scratch directory of the run's own. Declared before the evaluator, whose
    // callback reaches the merger, so the evaluator goes first, then the merger,
    // removing its scratch file, then the directory: on every way out of run().
    const bool producesObjects = std::any_of(operations.begin(), operations.end(),
                                             [](const OperationPtr& op) { return op->producesObjects(); });
    std::unique_ptr<RunScratchDirectory> scratch;
    std::unique_ptr<SeamMerger> merger;
    if (producesObjects) {
        scratch = std::make_unique<RunScratchDirectory>(impl_->config.scratchDirectory());
        merger = std::make_unique<SeamMerger>(impl_->grid, pool.budget, scratch->path());
    }

    detail::Bindings bindings;
    bindings.objectSink = &sink;
    bindings.edgeSink = state.edgeSink;
    bindings.annotationSink = state.annotationSink;
    bindings.rasterSink = state.rasterSink;
    bindings.auxiliaries = state.auxiliaries;
    bindings.maskSource = state.maskSource;
    bindings.spec = &state.spec;
    bindings.selector = state.selector.get();
    bindings.counters = &state.counters;
    if (merger) {
        // Fragments are partial by definition, so they bypass the tile-ownership
        // and restriction filters; the merged object's anchor is tested instead.
        bindings.seamFragment = [target = merger.get()](const Polygon& fragment, LayerId layer,
                                                        const IntensityPartial* partials, int32_t count,
                                                        int64_t tileIndex) {
            target->addFragment(tileIndex, fragment, layer, partials, count);
        };
    }
    const detail::TileEvaluator evaluator(plan, operations, *state.reader, std::move(bindings));

    // A merged object is stored only when the restriction admits its anchor. The
    // tile masks admits() caches are released as tiles settle, which keeps the
    // cache to the tiles in progress. The merger calls the filter only from
    // notifyTileComplete() and resolve(), never after run() leaves them.
    std::unique_ptr<detail::TileMaskRelease> maskRelease;
    if (merger && evaluator.restriction().restricted()) {
        maskRelease = std::make_unique<detail::TileMaskRelease>(evaluator.restriction(), plan.grid);
        detail::SeamMergerAccess::setFilter(
            *merger, [release = maskRelease.get(), counters = &state.counters](Point64 anchor) {
                const bool admitted = release->admits(anchor);
                if (!admitted) {
                    ++counters->droppedByRestriction;
                }
                return admitted;
            });
    }

    std::atomic<int64_t>& completed = impl_->completed;

    // Runs `evaluateTile` on every tile, one job per worker, each pulling tile indices
    // until none is left or a tile fails; returns once every job is done, and throws
    // the pass's first failure as reportFailure() does.
    using TileFunction = std::function<void(int64_t tile, const char** operation)>;
    const auto runPass = [&](const TileFunction& evaluateTile) {
        const auto shared = std::make_shared<RunState>();
        RunState& run = *shared;
        auto work = [&, shared]() {
            RunState& own = *shared;
            const JobDoneGuard doneOnExit(own);
            for (;;) {
                if (own.stop) {
                    break;
                }
                const int64_t tile = own.next.fetch_add(1);
                if (tile >= tileCount) {
                    break;
                }
                const char* operation = nullptr;
                try {
                    token.throwIfCancelled();
                    {
                        budget.reserveOrWait(tileBytes, token);
                        const Reservation held(budget, tileBytes);
                        if (own.stop) {
                            break;
                        }
                        evaluateTile(tile, &operation);
                    }
                    std::lock_guard<std::mutex> lock(own.progressMutex);
                    const int64_t done = ++completed;
                    if (progress != nullptr) {
                        progress->onProgress(stageName.c_str(),
                                             static_cast<double>(done) / static_cast<double>(total), done, total);
                    }
                } catch (...) {
                    own.recordFailure(std::current_exception(), tile, operation);
                    break;
                }
            }
        };

        std::exception_ptr submitFailure;
        try {
            for (int32_t i = 0; i < pool.workerCount; ++i) {
                {
                    std::lock_guard<std::mutex> lock(run.doneMutex);
                    ++run.jobsLeft;
                }
                try {
                    pool.submit(work);
                } catch (...) {
                    std::lock_guard<std::mutex> lock(run.doneMutex);
                    --run.jobsLeft;
                    throw;
                }
            }
        } catch (...) {
            submitFailure = std::current_exception();
            run.stop = true;
        }
        try {
            std::unique_lock<std::mutex> lock(run.doneMutex);
            run.done.wait(lock, [&run] { return run.jobsLeft == 0; });
        } catch (...) {
            // Without the wait the jobs could outlive this frame; nothing safe remains.
            std::terminate();
        }
        if (submitFailure) {
            std::rethrow_exception(submitFailure);
        }
        if (run.failure) {
            reportFailure(run.failure, stageName, run.hasOperation ? run.failedOperation.c_str() : nullptr,
                          impl_->grid, run.failedTile);
        }
        if (run.failureLost) {
            throw InternalError("TileExecutor::run: a tile failed and its failure could not be recorded");
        }
    };

    for (size_t g : globalPositions) {
        // Pass for operation g: each tile accumulates into a fresh instance, and the
        // instances fold into the run's instance in ascending tile index.
        Operation& runInstance = *operations[g];
        const auto k = static_cast<int32_t>(g);
        const char* name = state.spec.operationName(k);
        const OperationParameters& parameters = state.spec.operationParameters(k);
        detail::OrderedFold<OperationPtr> fold(
            tileCount, [&runInstance](OperationPtr& tileInstance) { runInstance.combine(*tileInstance); });
        runPass([&](int64_t tile, const char** operation) {
            *operation = name;
            OperationPtr instance = catalog.create(name, parameters);
            detail::Accumulation accumulation{g, instance.get()};
            const detail::TileOutcome outcome = evaluator.evaluate(tile, token, operation, &accumulation);
            *operation = name;
            // combine() runs on whichever arrival unblocks the fold, so when it
            // throws, the TileError names this arriving tile, not necessarily
            // the tile whose instance failed to combine.
            if (outcome.skipped) {
                fold.skip(tile);
            } else {
                fold.submit(tile, std::move(instance));
            }
            *operation = nullptr;
        });
        if (!fold.complete()) {
            throw InternalError("TileExecutor::run: an accumulation pass ended with tiles not folded");
        }
        try {
            runInstance.finalizeAccumulation();
        } catch (...) {
            reportFailure(std::current_exception(), stageName, name, impl_->grid, -1);
        }
    }
    // The final pass. Every tile, a skipped one included, is reported to the
    // merger as it completes, so objects close during the pass; a failure there is
    // the tile's. After a failure or a cancellation nothing is resolved: the
    // merger is destroyed with whatever it still holds.
    runPass([&](int64_t tile, const char** operation) {
        evaluator.evaluate(tile, token, operation);
        if (merger) {
            merger->notifyTileComplete(tile, sink);
            if (maskRelease) {
                maskRelease->tileCompleted(tile);
            }
        }
    });
    if (merger) {
        merger->resolve(sink, progress, token);
    }
}

} // namespace slideio::processor
