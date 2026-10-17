#include <slideio/processor/core/memory_budget.hpp>

#include <slideio/processor/core/error.hpp>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <system_error>

namespace slideio::processor {

MemoryBudget::~MemoryBudget() = default;

namespace {

// C-6: a std::system_error from the mutex or condition variable is translated.
[[noreturn]] void synchronisationFailed(const char* operation, const std::system_error& e) {
    ErrorContext ctx;
    ctx.operationName = operation;
    ctx.cause = e.what();
    throw InternalError("MemoryBudget: synchronisation failed", ctx);
}

class MemoryBudgetImpl final : public MemoryBudget {
public:
    explicit MemoryBudgetImpl(int64_t ceiling) : ceiling_(ceiling) {}

    int64_t ceilingBytes() const override { return ceiling_; }

    int64_t reservedBytes() const override {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            return reserved_;
        } catch (const std::system_error& e) {
            synchronisationFailed("reservedBytes", e);
        }
    }

    int64_t availableBytes() const override {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            return ceiling_ - reserved_;
        } catch (const std::system_error& e) {
            synchronisationFailed("availableBytes", e);
        }
    }

    bool tryReserve(int64_t bytes) override {
        if (bytes < 0) {
            throw InvalidArgumentError("bytes must not be negative");
        }
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            return reserveLocked(bytes);
        } catch (const std::system_error& e) {
            synchronisationFailed("tryReserve", e);
        }
    }

    void reserveOrWait(int64_t bytes, const CancellationToken& token) override {
        if (bytes < 0) {
            throw InvalidArgumentError("bytes must not be negative");
        }
        if (bytes > ceiling_) {
            throw InvalidArgumentError("bytes exceeds the memory budget ceiling");
        }
        try {
            std::unique_lock<std::mutex> lock(mutex_);
            while (!reserveLocked(bytes)) {
                token.throwIfCancelled();
                released_.wait_for(lock, std::chrono::milliseconds(10));
            }
        } catch (const std::system_error& e) {
            synchronisationFailed("reserveOrWait", e);
        }
    }

    void release(int64_t bytes) override {
        if (bytes < 0) {
            throw InvalidArgumentError("bytes must not be negative");
        }
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if (bytes > reserved_) {
                throw InvalidArgumentError("bytes exceeds the reserved amount");
            }
            reserved_ -= bytes;
        } catch (const std::system_error& e) {
            synchronisationFailed("release", e);
        }
        released_.notify_all();
    }

private:
    // Caller holds mutex_. Compares against the remaining space so that a huge
    // request cannot overflow reserved_ + bytes.
    bool reserveLocked(int64_t bytes) {
        if (bytes > ceiling_ - reserved_) {
            return false;
        }
        reserved_ += bytes;
        return true;
    }

    const int64_t ceiling_;
    mutable std::mutex mutex_;
    std::condition_variable released_;
    int64_t reserved_ = 0;
};

} // namespace

MemoryBudgetPtr makeMemoryBudget(int64_t ceilingBytes) {
    if (ceilingBytes <= 0) {
        throw InvalidArgumentError("ceilingBytes must be greater than 0");
    }
    return std::make_shared<MemoryBudgetImpl>(ceilingBytes);
}

} // namespace slideio::processor
