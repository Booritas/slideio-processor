// Requirements: UR-OBS-6
#pragma once

// The local wake-up signal (ruling S13): a named, machine-local object that a store's
// writer posts after every commit that appends an event, so that event cursors in any
// process wake at once instead of at the end of a poll interval. The name derives from
// meta.store_uuid, never from the path.
//
//   Windows: a manual-reset named event, "Local\slideio-processor-<uuid>". post() sets
//            it; a waiter that wakes resets it.
//   Linux:   a POSIX named semaphore, "/slideio-processor-<uuid>", mode 0600. post()
//            posts it; a waiter uses sem_timedwait and drains it with sem_trywait. The
//            writer unlinks the name when its store closes.
//   macOS:   no signal (no sem_timedwait); isSupported() is false and cursors poll.
//
// The signal is best-effort. Correctness never depends on it (UR-OBS-6): a cursor
// re-reads the journal after every wait, and no wait exceeds the poll interval, so a
// missed or spurious wake costs at most one interval. Failures are logged at Warning
// and never thrown.

#include <cstdint>
#include <string>

namespace slideio::processor::detail {

class ChangeSignal {
public:
    // True where the platform has a signal (Windows, Linux); false on macOS.
    static bool isSupported() noexcept;

    // Opens the signal for the store whose meta.store_uuid is `storeUuid`, creating it
    // when no handle has yet. Never throws: on failure it logs a Warning naming
    // `storePath` and the cause, and the signal stays closed, so wait() just sleeps.
    // Where !isSupported() it stays closed and logs nothing.
    ChangeSignal(const std::string& storeUuid, const std::string& storePath) noexcept;
    ChangeSignal(const ChangeSignal&) = delete;
    ChangeSignal& operator=(const ChangeSignal&) = delete;
    ~ChangeSignal();

    // True when the signal opened.
    bool isOpen() const noexcept;

    // Wakes every waiter (Windows) or one waiter (Linux). A no-op when not open; a
    // failure is logged at Warning.
    void post() noexcept;

    // Blocks for at most `timeoutMs` milliseconds, or until post(). Returns true when
    // woken by a post, false on timeout. When not open it sleeps the whole timeout and
    // returns false. A failure is logged at Warning and treated as a timeout, after
    // sleeping the rest of the timeout, so a broken signal never spins a caller.
    bool wait(int32_t timeoutMs) noexcept;

    // Removes the signal's name (Linux; the writer, on close), so a crashed or finished
    // store leaves nothing behind once every handle is closed. Handles already open keep
    // working. A no-op elsewhere.
    void unlink() noexcept;

private:
    std::string name_;
    std::string storePath_;
#ifdef _WIN32
    void* handle_ = nullptr;
#else
    void* semaphore_ = nullptr;  // sem_t*, kept opaque to keep <semaphore.h> out of here
#endif
};

} // namespace slideio::processor::detail
