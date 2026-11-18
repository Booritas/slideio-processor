// Requirements: UR-EXE-2
#pragma once

#include <slideio/processor/core/control.hpp>
#include <slideio/processor/core/error.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <system_error>

namespace slideio::processor::detail {

/// A counting semaphore bounding the operation instances an accumulation pass
/// holds at once: a worker takes a slot before it takes a tile, and the slot
/// comes back when the tile's instance has been combined and destroyed, or when
/// the tile is skipped.
///
/// A waiting worker re-checks the pass's stop flag and the cancellation token
/// every 10 ms, the interval MemoryBudget::reserveOrWait() polls at, so a failure
/// elsewhere or a cancellation never leaves it waiting for a slot nobody returns.
/// Thread-safety: ThreadSafe.
class InstanceSlots {
public:
    /// Throws InvalidArgumentError when `capacity` is below 1.
    explicit InstanceSlots(int64_t capacity) : capacity_(capacity) {
        if (capacity_ < 1) {
            throw InvalidArgumentError(
                ("InstanceSlots: the capacity must be at least 1 (got " + std::to_string(capacity_) + ")").c_str());
        }
    }
    InstanceSlots(const InstanceSlots&) = delete;
    InstanceSlots& operator=(const InstanceSlots&) = delete;

    /// Takes a slot, waiting until one is free. Returns false, holding none, when
    /// `stop` is set; throws CancelledError when `token` is cancelled, and
    /// InternalError when a synchronisation primitive fails.
    bool acquire(const CancellationToken& token, const std::atomic<bool>& stop) {
        try {
            std::unique_lock<std::mutex> lock(mutex_);
            for (;;) {
                if (stop) {
                    return false;
                }
                token.throwIfCancelled();
                if (inUse_ < capacity_) {
                    ++inUse_;
                    return true;
                }
                freed_.wait_for(lock, std::chrono::milliseconds(10));
            }
        } catch (const std::system_error& e) {
            ErrorContext context;
            context.cause = e.what();
            throw InternalError("InstanceSlots: a synchronisation primitive failed", context);
        }
    }

    /// Returns a slot taken by acquire().
    void release() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (inUse_ > 0) {
                --inUse_;
            }
        }
        freed_.notify_one();
    }

    /// The slots taken and not yet returned.
    int64_t inUse() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return inUse_;
    }

private:
    const int64_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable freed_;
    int64_t inUse_ = 0;
};

} // namespace slideio::processor::detail
