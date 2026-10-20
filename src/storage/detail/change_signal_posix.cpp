// Requirements: UR-OBS-6
#include "storage/detail/change_signal.hpp"

#include <slideio/processor/core/log.hpp>

#include <chrono>
#include <thread>

#if !defined(__APPLE__)
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

ChangeSignal::ChangeSignal(const std::string& storeUuid, const std::string& storePath) noexcept {
    (void)storeUuid;
    (void)storePath;
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

ChangeSignal::ChangeSignal(const std::string& storeUuid, const std::string& storePath) noexcept {
    try {
        name_ = "/slideio-processor-" + storeUuid;
        storePath_ = storePath;
    } catch (...) {
        return;  // out of memory: no signal, so cursors poll
    }
    // Opens the semaphore when another handle created it; mode 0600 keeps it to this user.
    sem_t* semaphore = sem_open(name_.c_str(), O_CREAT, 0600, 0);
    if (semaphore == SEM_FAILED) {
        warnFailure("opening the semaphore", name_, storePath_, errno);
        return;
    }
    semaphore_ = semaphore;
}

ChangeSignal::~ChangeSignal() {
    if (semaphore_ != nullptr) sem_close(semaphoreOf(semaphore_));
}

bool ChangeSignal::isOpen() const noexcept { return semaphore_ != nullptr; }

void ChangeSignal::post() noexcept {
    // EOVERFLOW means posts are piling up with nobody waiting: a waiter wakes all the same.
    if (semaphore_ != nullptr && sem_post(semaphoreOf(semaphore_)) != 0 && errno != EOVERFLOW) {
        warnFailure("posting the semaphore", name_, storePath_, errno);
    }
}

bool ChangeSignal::wait(int32_t timeoutMs) noexcept {
    if (semaphore_ == nullptr) {
        sleepFor(timeoutMs);
        return false;
    }
    sem_t* semaphore = semaphoreOf(semaphore_);
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
    if (rc == 0) {
        while (sem_trywait(semaphore) == 0) {
            // Drain the posts of earlier commits: one wake covers them all.
        }
        return true;
    }
    if (errno != ETIMEDOUT) {
        warnFailure("waiting on the semaphore", name_, storePath_, errno);
        sleepFor(timeoutMs);
    }
    return false;
}

void ChangeSignal::unlink() noexcept {
    if (semaphore_ != nullptr && sem_unlink(name_.c_str()) != 0 && errno != ENOENT) {
        warnFailure("unlinking the semaphore", name_, storePath_, errno);
    }
}

#endif

} // namespace slideio::processor::detail
