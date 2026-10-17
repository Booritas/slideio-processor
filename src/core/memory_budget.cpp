#include <slideio/processor/core/memory_budget.hpp>

#include <slideio/processor/core/error.hpp>

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace slideio::processor {

MemoryBudget::~MemoryBudget() = default;

namespace {

class MemoryBudgetImpl final : public MemoryBudget {
public:
    explicit MemoryBudgetImpl(int64_t ceiling) : ceiling_(ceiling) {}

    int64_t ceilingBytes() const override { return ceiling_; }

    int64_t reservedBytes() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return reserved_;
    }

    int64_t availableBytes() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return ceiling_ - reserved_;
    }

    bool tryReserve(int64_t bytes) override {
        if (bytes < 0) {
            throw InvalidArgumentError("bytes must not be negative");
        }
        std::lock_guard<std::mutex> lock(mutex_);
        return reserveLocked(bytes);
    }

    void reserveOrWait(int64_t bytes, const CancellationToken& token) override {
        if (bytes < 0) {
            throw InvalidArgumentError("bytes must not be negative");
        }
        if (bytes > ceiling_) {
            throw InvalidArgumentError("bytes exceeds the memory budget ceiling");
        }
        std::unique_lock<std::mutex> lock(mutex_);
        while (!reserveLocked(bytes)) {
            token.throwIfCancelled();
            released_.wait_for(lock, std::chrono::milliseconds(10));
        }
    }

    void release(int64_t bytes) override {
        if (bytes < 0) {
            throw InvalidArgumentError("bytes must not be negative");
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (bytes > reserved_) {
                throw InvalidArgumentError("bytes exceeds the reserved amount");
            }
            reserved_ -= bytes;
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
