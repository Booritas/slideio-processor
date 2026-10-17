// Requirements: UR-EXE-4
#pragma once

/**
 * @file control.hpp
 * @brief Cancellation (CancellationToken, CancellationSource) and progress
 * reporting (ProgressObserver).
 */

#include <slideio/processor/core/export.hpp>

#include <cstdint>
#include <memory>

namespace slideio::processor {

class CancellationSource;

/**
 * @brief A cheap, copyable handle that reports whether an operation has been
 * cancelled.
 *
 * A default-built token holds no state and is never cancelled. Tokens made by
 * a CancellationSource share its flag and stay valid after the source is
 * destroyed. A moved-from token holds no state, so it is never cancelled.
 * @par Thread-safety
 * ThreadCompatible: concurrent const calls on one token object are safe; a
 * non-const call (assignment) needs exclusive access to that object.
 * @note Copies of a token share one flag, which is atomic: distinct copies may
 * be queried from any number of threads while CancellationSource::cancel()
 * runs concurrently.
 * @par Requirements
 * UR-EXE-4
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS CancellationToken {
public:
    /**
     * @brief Builds a token that is never cancelled. Does not allocate.
     * @throws Nothing.
     */
    CancellationToken() noexcept;
    /**
     * @brief Copies a token; both share the same flag.
     * @param other The token to copy.
     * @throws Nothing.
     */
    CancellationToken(const CancellationToken& other) noexcept;
    /**
     * @brief Moves a token. The source is left never-cancelled.
     * @param other The token to move from.
     * @throws Nothing.
     */
    CancellationToken(CancellationToken&& other) noexcept;
    /**
     * @brief Copy-assigns a token.
     * @param other The token to copy.
     * @return This object.
     * @throws Nothing.
     */
    CancellationToken& operator=(const CancellationToken& other) noexcept;
    /**
     * @brief Move-assigns a token. The source is left never-cancelled.
     * @param other The token to move from.
     * @return This object.
     * @throws Nothing.
     */
    CancellationToken& operator=(CancellationToken&& other) noexcept;
    /** @brief Destroys the token. */
    ~CancellationToken();

    /**
     * @brief Tells whether cancellation was requested.
     * @return True once the owning source has been cancelled.
     * @throws Nothing.
     */
    bool isCancelled() const noexcept;
    /**
     * @brief Throws when cancellation was requested.
     * @throws CancelledError With the message "operation cancelled".
     */
    void throwIfCancelled() const;

private:
    friend class CancellationSource;
    class State;
    explicit CancellationToken(std::shared_ptr<State> state) noexcept;
    void cancel() const noexcept;

    std::shared_ptr<State> state_; // null: never cancelled
};

/**
 * @brief Owns a cancellation flag and hands out tokens that observe it.
 *
 * Non-copyable and non-movable: the source is the one place that may cancel.
 * Cancellation is permanent and idempotent.
 * @par Thread-safety
 * ThreadSafe.
 * @par Requirements
 * UR-EXE-4
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS CancellationSource {
public:
    /**
     * @brief Builds a source that is not cancelled.
     * @throws std::bad_alloc When memory is exhausted.
     */
    CancellationSource();
    /** @brief Destroys the source. Tokens already issued remain valid. */
    ~CancellationSource();
    /** @brief Deleted: a source is not copyable (nor movable). */
    CancellationSource(const CancellationSource&) = delete;
    /**
     * @brief Deleted: a source is not copy-assignable (nor move-assignable).
     * @return Not applicable.
     */
    CancellationSource& operator=(const CancellationSource&) = delete;

    /**
     * @brief Makes a token observing this source.
     * @return A token that reports cancelled once cancel() is called.
     * @throws Nothing.
     */
    CancellationToken token() const noexcept;
    /**
     * @brief Requests cancellation. Idempotent.
     * @throws Nothing.
     */
    void cancel() noexcept;
    /**
     * @brief Tells whether cancel() has been called.
     * @return True after cancel().
     * @throws Nothing.
     */
    bool isCancelled() const noexcept;

private:
    CancellationToken token_;
};

/**
 * @brief Receives progress notifications from a running stage.
 *
 * Implementations are called from worker threads.
 * @par Thread-safety
 * Implementations must be ThreadSafe.
 * @par Requirements
 * UR-EXE-4
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS ProgressObserver {
public:
    /** @brief Destroys the observer. */
    virtual ~ProgressObserver();
    /**
     * @brief Reports progress of a stage.
     * @param stageName UTF-8 name of the reporting stage.
     * @param fraction Completed fraction, from 0.0 to 1.0.
     * @param completed Units of work done so far.
     * @param total Total units of work.
     * @throws Implementations should not throw.
     */
    virtual void onProgress(const char* stageName, double fraction,
                            int64_t completed, int64_t total) = 0;
};

} // namespace slideio::processor
