// Requirements: UR-OBS-6
#include "storage/detail/change_signal.hpp"

#include <slideio/processor/core/log.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>


namespace slideio::processor::detail {

namespace {

// Logs a failed signal call with the Win32 error; the caller falls back to polling.
void warnFailure(const char* step, const std::string& name, const std::string& storePath) noexcept {
    logMessage(LogLevel::Warning, "ChangeSignal: %s '%s' for '%s' failed (Win32 error %lu); event cursors poll",
               step, name.c_str(), storePath.c_str(), static_cast<unsigned long>(GetLastError()));
}

} // namespace

bool ChangeSignal::isSupported() noexcept { return true; }

ChangeSignal::ChangeSignal(const std::string& storeUuid, const std::string& storePath) noexcept {
    try {
        name_ = "Local\\slideio-processor-" + storeUuid;
        storePath_ = storePath;
    } catch (...) {
        return;  // out of memory: no signal, so cursors poll
    }
    // Manual reset, initially clear. Opens the event when another handle created it.
    handle_ = CreateEventA(nullptr, TRUE, FALSE, name_.c_str());
    if (handle_ == nullptr) warnFailure("creating the event", name_, storePath_);
}

ChangeSignal::~ChangeSignal() {
    if (handle_ != nullptr) CloseHandle(handle_);
}

bool ChangeSignal::isOpen() const noexcept { return handle_ != nullptr; }

void ChangeSignal::post() noexcept {
    if (handle_ != nullptr && !SetEvent(handle_)) warnFailure("setting the event", name_, storePath_);
}

bool ChangeSignal::wait(int32_t timeoutMs) noexcept {
    const DWORD timeout = timeoutMs > 0 ? static_cast<DWORD>(timeoutMs) : 0;
    if (handle_ == nullptr) {
        Sleep(timeout);
        return false;
    }
    const DWORD result = WaitForSingleObject(handle_, timeout);
    if (result == WAIT_OBJECT_0) {
        // Every waiter blocked at SetEvent has been released; clear it for the next commit.
        // A waiter that was not yet blocked and arrives after this reset waits out its slice:
        // at most one poll interval late, never wrong (S13).
        if (ResetEvent(handle_)) return true;
        warnFailure("resetting the event", name_, storePath_);
        Sleep(timeout);  // the event may stay set: never spin on it
        return false;
    }
    if (result != WAIT_TIMEOUT) {
        warnFailure("waiting on the event", name_, storePath_);
        Sleep(timeout);
    }
    return false;
}

void ChangeSignal::unlink() noexcept {}  // a named event goes with its last handle

} // namespace slideio::processor::detail
