// Requirements: UR-OBS-6
#pragma once

// The local wake-up signal (ruling S13): a named, machine-local object that a store's
// writer posts after every commit that appends an event, so that event cursors in any
// process wake at once instead of at the end of a poll interval. The name derives from
// meta.store_uuid, never from the path.
//
//   Windows: a manual-reset named event, "Local\slideio-processor-<uuid>". post() sets
//            it; a waiter that wakes resets it. Every handle creates or opens it; the
//            system destroys it with its last handle, so nothing is left behind.
//   Linux:   a POSIX named semaphore, "/slideio-processor-<uuid>", mode 0600. post()
//            posts it; a waiter uses sem_timedwait and drains it with sem_trywait. Only
//            a writer creates the name, and it unlinks the name when its store closes. A
//            reader only opens an existing name, so it never leaves one behind; while
//            there is none (no writer yet), and after each wait that times out, it tries
//            to open the name again, at most once per wait, so it follows a writer that
//            appears later or recreates the name.
//   macOS:   no signal (no sem_timedwait); isSupported() is false and cursors poll.
//
// The signal is best-effort. Correctness never depends on it (UR-OBS-6): a cursor
// re-reads the journal after every wait, and no wait exceeds the poll interval, so a
// missed or spurious wake costs at most one interval. Failures are logged at Warning
// and never thrown.

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace slideio::processor::detail {

class ChangeSignal {
public:
    // Who holds the signal: the store's writer creates the name; a reader only opens it.
    enum class Role : int32_t { Writer, Reader };

    // True where the platform has a signal (Windows, Linux); false on macOS.
    static bool isSupported() noexcept;

    // Opens the signal for the store whose meta.store_uuid is `storeUuid`. Never throws:
    // on failure it logs a Warning naming `storePath` and the cause, and the signal stays
    // closed, so wait() just sleeps. A Linux reader that finds no name stays closed
    // silently and tries again in wait(). Where !isSupported() it stays closed and logs
    // nothing.
    ChangeSignal(const std::string& storeUuid, const std::string& storePath, Role role) noexcept;
    ChangeSignal(const ChangeSignal&) = delete;
    ChangeSignal& operator=(const ChangeSignal&) = delete;
    ~ChangeSignal();

    // True when the signal is open now.
    bool isOpen() const noexcept;

    // Wakes every waiter (Windows) or one waiter (Linux). A no-op when not open; a
    // failure is logged at Warning.
    void post() noexcept;

    // Blocks for at most `timeoutMs` milliseconds, or until post(). Returns true when
    // woken by a post, false on timeout. When not open it sleeps the whole timeout and
    // returns false. A failure is logged at Warning and treated as a timeout, after
    // sleeping the rest of the timeout, so a broken signal never spins a caller.
    // ThreadSafe: several cursors may wait at once.
    bool wait(int32_t timeoutMs) noexcept;

    // Removes the signal's name (Linux; the writer, on close), so a finished store
    // leaves nothing behind once every handle is closed. Handles already open keep
    // working. A no-op elsewhere.
    void unlink() noexcept;

private:
#if defined(_WIN32)
    std::string name_;
    std::string storePath_;
    void* handle_ = nullptr;
#elif !defined(__APPLE__)
    // A semaphore this handle has open, and how many wait() calls are on it now.
    struct Held {
        void* semaphore;  // sem_t*, kept opaque to keep <semaphore.h> out of here
        int32_t waiters;
    };
    // The current semaphore, with its waiter count raised, or null; give it back with
    // release().
    void* acquire() noexcept;
    // Lowers the waiter count; closes a replaced semaphore once nobody waits on it.
    void release(void* semaphore) noexcept;
    // Reader: opens the name if it exists, replacing a semaphore the writer unlinked.
    void tryOpenByName() noexcept;
    void warnOnce(const char* step, int error) noexcept;

    std::string name_;
    std::string storePath_;
    Role role_ = Role::Reader;
    mutable std::mutex mutex_;   // guards current_, replaced_ and warned_
    Held current_{nullptr, 0};   // the semaphore to wait on and post
    std::vector<Held> replaced_; // replaced by a re-open while a wait() was still on them
    bool warned_ = false;        // a failure to open is logged once
#endif
};

} // namespace slideio::processor::detail
