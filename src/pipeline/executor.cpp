// Requirements: UR-EXE-2, UR-EXE-3, UR-EXE-4, UR-EXE-5
#include <slideio/processor/pipeline/executor.hpp>

#include <pipeline/detail/pipeline_impl.hpp>

#include <slideio/processor/core/error.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
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

// What the workers of one run share. Lives on run()'s stack; run() returns only
// after every job has signalled done.
struct RunState {
    std::atomic<int64_t> next{0};       ///< The next tile index to hand out.
    std::atomic<bool> stop{false};      ///< Set by the first failure: hand out no more tiles.

    std::mutex failureMutex;
    std::exception_ptr failure;         ///< The first failure; later ones are dropped.
    int64_t failedTile = -1;
    bool hasOperation = false;
    std::string failedOperation;

    std::mutex progressMutex;           ///< Serialises the completed count and the observer.

    std::mutex doneMutex;
    std::condition_variable done;
    int32_t jobsLeft = 0;

    void recordFailure(std::exception_ptr error, int64_t tile, const char* operation) {
        {
            std::lock_guard<std::mutex> lock(failureMutex);
            if (!failure) {
                failure = std::move(error);
                failedTile = tile;
                hasOperation = operation != nullptr;
                failedOperation = operation != nullptr ? operation : "";
            }
        }
        stop = true;
    }

    // The last thing a job does. noexcept: a failure here would leave run() waiting
    // forever, so it terminates instead.
    void jobDone() noexcept {
        std::lock_guard<std::mutex> lock(doneMutex);
        --jobsLeft;
        done.notify_all();
    }
};

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
        : grid(gridIn), pool(poolIn), config(configIn) {}

    const TileGrid grid;
    WorkerPool& pool;
    const Configuration config;
    std::atomic<int64_t> completed{0};
    std::mutex runMutex;                ///< Serialises run().
};

TileExecutor::TileExecutor(const TileGrid& grid, WorkerPool& pool, const Configuration& config)
    : impl_(std::make_unique<Impl>(grid, pool, config)) {}

TileExecutor::~TileExecutor() = default;

int64_t TileExecutor::tilesCompleted() const noexcept { return impl_->completed.load(); }

int64_t TileExecutor::tilesTotal() const noexcept { return impl_->grid.tileCount(); }

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
    const detail::RunPlan& plan = *state.plan;
    if (!sameGrid(plan.grid, impl_->grid)) {
        throw InvalidArgumentError("TileExecutor::run: the pipeline's tile grid differs from the executor's");
    }
    if (!plan.globalContextPositions.empty()) {
        throw InternalError("TileExecutor::run: global-context operations are not run yet");
    }
    WorkerPool::Impl& pool = *impl_->pool.impl_;
    MemoryBudget& budget = *pool.budget;
    // The plan's tileBytes bounds every tile's peak buffers (P26); reserve it whole.
    const int64_t tileBytes = plan.tileBytes;
    if (tileBytes > budget.ceilingBytes()) {
        throw InvalidArgumentError(("TileExecutor::run: one tile needs " + std::to_string(tileBytes) +
                                    " bytes, above the pool's memory ceiling of " +
                                    std::to_string(budget.ceilingBytes()) + " bytes")
                                       .c_str());
    }

    impl_->completed = 0;
    state.counters.reset();
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
    const detail::TileEvaluator evaluator(plan, *state.reader, std::move(bindings));

    const int64_t total = impl_->grid.tileCount();
    const std::string& stageName = state.stageName;
    std::atomic<int64_t>& completed = impl_->completed;
    // Shared with every job, so a job that signals done after run() has seen
    // jobsLeft reach 0 and returned still touches a live object.
    const auto shared = std::make_shared<RunState>();
    RunState& run = *shared;

    auto work = [&, shared]() {
        RunState& own = *shared;
        for (;;) {
            if (own.stop) {
                break;
            }
            const int64_t tile = own.next.fetch_add(1);
            if (tile >= total) {
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
                    evaluator.evaluate(tile, token, &operation);
                }
                std::lock_guard<std::mutex> lock(own.progressMutex);
                const int64_t done = ++completed;
                if (progress != nullptr) {
                    progress->onProgress(stageName.c_str(), static_cast<double>(done) / static_cast<double>(total),
                                         done, total);
                }
            } catch (...) {
                own.recordFailure(std::current_exception(), tile, operation);
                break;
            }
        }
        own.jobDone();
    };

    // One job per worker; each pulls tile indices until none is left or the run fails.
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
    if (!run.failure) {
        return;
    }

    // The first failure: a CancelledError as is, anything else as a TileError (P27).
    std::string cause;
    try {
        std::rethrow_exception(run.failure);
    } catch (const CancelledError&) {
        throw;
    } catch (const std::exception& e) {
        cause = e.what();
    } catch (...) {
        cause = "an exception that is not a std::exception";
    }
    const int64_t columns = impl_->grid.columns();
    ErrorContext context;
    context.stageName = stageName.c_str();
    context.operationName = run.hasOperation ? run.failedOperation.c_str() : nullptr;
    context.cause = cause.c_str();
    context.hasTile = true;
    context.tileX = run.failedTile % columns;
    context.tileY = run.failedTile / columns;
    throw TileError("a tile failed", context);
}

} // namespace slideio::processor
