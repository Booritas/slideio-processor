// Requirements: UR-QA-3, UR-EXE-1, UR-PIX-5
#pragma once

#include <slideio/processor/core/error.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>

namespace slideio::processor::detail {

/// Combines per-tile results in canonical order (ruling P15): workers hand in
/// (tileIndex, value) in any order, and the fold calls `combine` strictly in
/// ascending tile index, holding a result that arrives early until every
/// predecessor has arrived. A skipped tile counts as arrived and combines nothing.
///
/// Every tile index in [0, tileCount) must arrive exactly once; complete() tells
/// whether all have. `combine` runs under the fold's lock, so it is never called
/// concurrently with itself, on whichever worker's arrival unblocks it.
/// Thread-safety: submit() and skip() are ThreadSafe; the rest is ThreadSafe too.
template <typename T>
class OrderedFold {
public:
    using Combine = std::function<void(T&)>;

    /// Throws InvalidArgumentError when tileCount is negative or combine is empty.
    OrderedFold(int64_t tileCount, Combine combine) : tileCount_(tileCount), combine_(std::move(combine)) {
        if (tileCount_ < 0) {
            throw InvalidArgumentError("OrderedFold: the tile count must not be negative");
        }
        if (!combine_) {
            throw InvalidArgumentError("OrderedFold: the combine function is empty");
        }
    }

    /// Hands in tile `tileIndex`'s result. Throws InvalidArgumentError when the
    /// index is out of range or has already arrived, and whatever `combine`
    /// throws (the fold then stops: no later result is combined).
    void submit(int64_t tileIndex, T value) { arrive(tileIndex, Slot{true, std::move(value)}); }

    /// Marks tile `tileIndex` as arrived with nothing to combine. Throws as submit().
    void skip(int64_t tileIndex) { arrive(tileIndex, Slot{false, T()}); }

    /// The lowest tile index not yet combined (tileCount when complete).
    int64_t nextIndex() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return next_;
    }

    /// Whether every tile has arrived and been combined.
    bool complete() const { return nextIndex() == tileCount_; }

    /// Results held because a predecessor has not arrived yet.
    int64_t heldCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<int64_t>(held_.size());
    }

private:
    struct Slot {
        bool hasValue;
        T value;
    };

    void arrive(int64_t tileIndex, Slot slot) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (failed_) {
            throw InternalError("OrderedFold: an earlier combine failed");
        }
        if (tileIndex < next_ || tileIndex >= tileCount_ || held_.count(tileIndex) != 0) {
            throw InvalidArgumentError(
                ("OrderedFold: tile " + std::to_string(tileIndex) + " is out of range or arrived twice").c_str());
        }
        held_.emplace(tileIndex, std::move(slot));
        while (!held_.empty() && held_.begin()->first == next_) {
            Slot ready = std::move(held_.begin()->second);
            held_.erase(held_.begin());
            if (ready.hasValue) {
                try {
                    combine_(ready.value);
                } catch (...) {
                    failed_ = true;
                    throw;
                }
            }
            ++next_;
        }
    }

    const int64_t tileCount_;
    const Combine combine_;
    mutable std::mutex mutex_;
    std::map<int64_t, Slot> held_;
    int64_t next_ = 0;
    bool failed_ = false;
};

} // namespace slideio::processor::detail
