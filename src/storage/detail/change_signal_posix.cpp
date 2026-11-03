// Requirements: UR-OBS-6
#include "storage/detail/change_signal.hpp"

#include <slideio/processor/core/log.hpp>

#include <chrono>
#include <thread>

#if !defined(__APPLE__)
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <semaphore.h>
#endif

namespace slideio::processor::detail {

namespace {

void sleepFor(int32_t timeoutMs) noexcept {
    if (timeoutMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs));
}

} // namespace

#if defined(__APPLE__)

// macOS has no sem_timedwait: no signal, and event cursors poll (ruling S13).

bool ChangeSignal::isSupported() noexcept { return false; }

ChangeSignal::ChangeSignal(const std::string& storeUuid, const std::string& storePath, Role role) noexcept {
    (void)storeUuid;
    (void)storePath;
    (void)role;
}

ChangeSignal::~ChangeSignal() = default;

bool ChangeSignal::isOpen() const noexcept { return false; }

void ChangeSignal::post() noexcept {}

bool ChangeSignal::wait(int32_t timeoutMs) noexcept {
    sleepFor(timeoutMs);
    return false;
}

void ChangeSignal::unlink() noexcept {}

#else

namespace {

sem_t* semaphoreOf(void* handle) noexcept { return static_cast<sem_t*>(handle); }

// Logs a failed signal call with errno; the caller falls back to polling.
void warnFailure(const char* step, const std::string& name, const std::string& storePath, int error) noexcept {
    logMessage(LogLevel::Warning, "ChangeSignal: %s '%s' for '%s' failed (%s); event cursors poll", step,
               name.c_str(), storePath.c_str(), std::strerror(error));
}

} // namespace

bool ChangeSignal::isSupported() noexcept { return true; }

ChangeSignal::ChangeSignal(const std::string& storeUuid, const std::string& storePath, Role role) noexcept
    : role_(role) {
    try {
        name_ = "/slideio-processor-" + storeUuid;
        storePath_ = storePath;
        replaced_.reserve(4);
    } catch (...) {
        name_.clear();  // out of memory: no signal, so cursors poll
        return;
    }
    if (role_ == Role::Reader) {
        // Never creates the name: only the writer does, and only the writer removes it.
        tryOpenByName();
        return;
    }
    // Mode 0600 keeps the semaphore to this user. Opens it when a writer before this one
    // crashed and left the name behind.
    sem_t* semaphore = sem_open(name_.c_str(), O_CREAT, 0600, 0);
    if (semaphore == SEM_FAILED) {
        warnOnce("creating the semaphore", errno);
        return;
    }
    current_.semaphore = semaphore;
}

ChangeSignal::~ChangeSignal() {
    if (current_.semaphore != nullptr) sem_close(semaphoreOf(current_.semaphore));
    for (const Held& held : replaced_) sem_close(semaphoreOf(held.semaphore));
}

bool ChangeSignal::isOpen() const noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    return current_.semaphore != nullptr;
}

void* ChangeSignal::acquire() noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    if (current_.semaphore != nullptr) ++current_.waiters;
    return current_.semaphore;
}

void ChangeSignal::release(void* semaphore) noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    if (semaphore == current_.semaphore) {
        --current_.waiters;
        return;
    }
    const auto held = std::find_if(replaced_.begin(), replaced_.end(),
                                   [semaphore](const Held& h) { return h.semaphore == semaphore; });
    if (held != replaced_.end() && --held->waiters == 0) {
        sem_close(semaphoreOf(held->semaphore));
        replaced_.erase(held);
    }
}

void ChangeSignal::tryOpenByName() noexcept {
    if (name_.empty()) return;
    sem_t* fresh = sem_open(name_.c_str(), 0);
    if (fresh == SEM_FAILED) {
        const int error = errno;
        // No name: no writer yet, or it has closed. Keep any semaphore held and try later.
        if (error != ENOENT) warnOnce("opening the semaphore", error);
        return;
    }
    std::lock_guard<std::mutex> guard(mutex_);
    if (fresh == current_.semaphore) {
        sem_close(fresh);  // the same semaphore, opened again: drop the extra reference
        return;
    }
    // A new semaphore under the name (a writer recreated it). The old one closes now, or
    // once the waits still on it end.
    if (current_.semaphore != nullptr) {
        if (current_.waiters == 0) {
            sem_close(semaphoreOf(current_.semaphore));
        } else {
            try {
                replaced_.push_back(current_);
            } catch (...) {
                sem_close(fresh);  // out of memory: keep waiting on the old one
                return;
            }
        }
    }
    current_ = Held{fresh, 0};
}

void ChangeSignal::warnOnce(const char* step, int error) noexcept {
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (warned_) return;
        warned_ = true;
    }
    warnFailure(step, name_, storePath_, error);
}

void ChangeSignal::post() noexcept {
    void* semaphore = acquire();
    if (semaphore == nullptr) return;
    // EOVERFLOW means posts are piling up with nobody waiting: a waiter wakes all the same.
    if (sem_post(semaphoreOf(semaphore)) != 0 && errno != EOVERFLOW) {
        warnFailure("posting the semaphore", name_, storePath_, errno);
    }
    release(semaphore);
}

bool ChangeSignal::wait(int32_t timeoutMs) noexcept {
    bool reopened = false;
    if (role_ == Role::Reader && !isOpen()) {
        tryOpenByName();  // a writer may have appeared since the last wait
        reopened = true;
    }
    void* handle = acquire();
    if (handle == nullptr) {
        sleepFor(timeoutMs);
        return false;
    }
    sem_t* semaphore = semaphoreOf(handle);
    timespec deadline{};
    clock_gettime(CLOCK_REALTIME, &deadline);  // sem_timedwait takes an absolute CLOCK_REALTIME time
    const int64_t nanoseconds =
        static_cast<int64_t>(deadline.tv_nsec) + static_cast<int64_t>(timeoutMs > 0 ? timeoutMs : 0) * 1000000;
    deadline.tv_sec += static_cast<time_t>(nanoseconds / 1000000000);
    deadline.tv_nsec = static_cast<long>(nanoseconds % 1000000000);
    int rc = 0;
    do {
        rc = sem_timedwait(semaphore, &deadline);
    } while (rc != 0 && errno == EINTR);
    const int error = rc == 0 ? 0 : errno;
    if (rc == 0) {
        while (sem_trywait(semaphore) == 0) {
            // Drain the posts of earlier commits: one wake covers them all.
        }
    }
    release(handle);
    if (rc == 0) return true;
    if (error != ETIMEDOUT) {
        warnFailure("waiting on the semaphore", name_, storePath_, error);
        sleepFor(timeoutMs);
    } else if (role_ == Role::Reader && !reopened) {
        // Nothing woke us: the writer may have closed and a new one recreated the name.
        tryOpenByName();
    }
    return false;
}

void ChangeSignal::unlink() noexcept {
    if (role_ == Role::Writer && isOpen() && sem_unlink(name_.c_str()) != 0 && errno != ENOENT) {
        warnFailure("unlinking the semaphore", name_, storePath_, errno);
    }
}

#endif

} // namespace slideio::processor::detail
