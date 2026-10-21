#include "storage/detail/writer_lock.hpp"

#include <slideio/processor/core/error.hpp>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace slideio::processor::detail {

namespace {

std::string lockPathOf(const std::string& storePath) { return storePath + "-writer.lock"; }

// How often acquire reopens the lock file after locking an orphan. A holder removes the
// lock file only while it holds the lock (removeFileAndRelease), so a handle opened just
// before the removal can lock the removed file while a later opener creates and locks a
// new one. A lock therefore counts only when the path still names the locked file, and
// while a counted lock is held the path keeps naming its file: only its holder removes
// it. So no two acquires succeed at once, and each retry follows a fresh removal. A
// refusal counts the same way: a lock found held on a file the path no longer names
// belongs to a holder removing it, so acquire retries rather than reports a writer.
// On Windows without POSIX delete semantics (FAT, Windows before 10 1809) a removed
// lock file stays delete-pending while open; reopening it fails, so acquire throws
// "cannot open", which fails safe and never yields two writers. The check relies on
// local-filesystem file identity (inode, volume serial and file index): the writer
// lock does not support stores on network shares.
constexpr int kLockAttempts = 8;

// The holder's pid from the lock file's text, or "" when it does not parse.
std::string parsePid(const std::string& text) {
    size_t end = 0;
    while (end < text.size() && text[end] >= '0' && text[end] <= '9') ++end;
    if (end == 0 || end > 19) return std::string();
    return text.substr(0, end);
}

[[noreturn]] void throwHeld(const std::string& storePath, const std::string& lockText) {
    const std::string pid = parsePid(lockText);
    std::string message = "Store::open: '" + storePath + "' is already open for writing";
    if (!pid.empty()) message += " by process " + pid;
    message += "; concurrent writers are not supported";
    throw IoError(message.c_str());
}

[[noreturn]] void throwLockFileError(const std::string& lockPath, const char* step, const std::string& cause) {
    const std::string message = "Store::open: cannot " + std::string(step) + " the writer lock file '" + lockPath + "'";
    ErrorContext context;
    context.cause = cause.c_str();
    throw IoError(message.c_str(), context);
}

#ifdef _WIN32
int currentPid() { return static_cast<int>(GetCurrentProcessId()); }
#else
int currentPid() { return static_cast<int>(getpid()); }
#endif

} // namespace

void WriterLock::removeFileAndRelease(const std::string& storePath) noexcept {
    if (held()) {
        // On Windows the handle shares delete access, so the file can go while it is open.
        std::error_code ec;
        std::filesystem::remove(std::filesystem::u8path(lockPathOf(storePath)), ec);
    }
    release();
}

WriterLock::WriterLock(WriterLock&& other) noexcept {
#ifdef _WIN32
    handle_ = std::exchange(other.handle_, nullptr);
#else
    fd_ = std::exchange(other.fd_, -1);
#endif
}

WriterLock& WriterLock::operator=(WriterLock&& other) noexcept {
    if (this != &other) {
        release();
#ifdef _WIN32
        handle_ = std::exchange(other.handle_, nullptr);
#else
        fd_ = std::exchange(other.fd_, -1);
#endif
    }
    return *this;
}

WriterLock::~WriterLock() { release(); }

WriterLock WriterLock::acquire(const std::string& storePath) { return acquire(storePath, std::function<void()>()); }

#ifdef _WIN32

namespace {
// The locked byte lies far beyond the pid text, so a refused opener can still read it.
void lockRegion(OVERLAPPED& region) {
    region = OVERLAPPED{};
    region.Offset = 0;
    region.OffsetHigh = 1;
}

std::string lastErrorText() { return std::system_category().message(static_cast<int>(GetLastError())); }

// True when `h` is the file `path` names now; false when the path is missing or names
// another file, because the file `h` holds was removed after it was opened.
bool namesFile(HANDLE h, const std::filesystem::path& path) {
    BY_HANDLE_FILE_INFORMATION held{};
    if (!GetFileInformationByHandle(h, &held)) return false;
    // No access rights and full sharing: this open never conflicts with another handle.
    HANDLE named = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (named == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION current{};
    const BOOL ok = GetFileInformationByHandle(named, &current);
    CloseHandle(named);
    return ok && held.dwVolumeSerialNumber == current.dwVolumeSerialNumber &&
           held.nFileIndexHigh == current.nFileIndexHigh && held.nFileIndexLow == current.nFileIndexLow;
}
} // namespace

WriterLock WriterLock::acquire(const std::string& storePath, const std::function<void()>& afterOpen) {
    const std::string lockPath = lockPathOf(storePath);
    const std::filesystem::path wide = std::filesystem::u8path(lockPath);
    for (int attempt = 0; attempt < kLockAttempts; ++attempt) {
        HANDLE h = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) throwLockFileError(lockPath, "open", lastErrorText());
        WriterLock lock;
        lock.handle_ = h;  // closed by ~WriterLock on every failure and retry below
        if (attempt == 0 && afterOpen) afterOpen();

        OVERLAPPED region;
        lockRegion(region);
        if (!LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &region)) {
            const DWORD error = GetLastError();
            lock.handle_ = nullptr;
            if (error != ERROR_LOCK_VIOLATION && error != ERROR_IO_PENDING) {
                CloseHandle(h);
                throwLockFileError(lockPath, "lock", std::system_category().message(static_cast<int>(error)));
            }
            if (!namesFile(h, wide)) {  // locked by a holder that is removing it; no writer's
                CloseHandle(h);
                continue;
            }
            char buffer[32] = {};
            DWORD read = 0;
            const BOOL ok = ReadFile(h, buffer, sizeof(buffer) - 1, &read, nullptr);
            CloseHandle(h);
            throwHeld(storePath, ok ? std::string(buffer, read) : std::string());
        }
        if (!namesFile(h, wide)) continue;  // an orphan (see kLockAttempts); released here
        // Lock held: replace the text with this process's id.
        const std::string pid = std::to_string(currentPid());
        LARGE_INTEGER zero{};
        DWORD written = 0;
        if (!SetFilePointerEx(h, zero, nullptr, FILE_BEGIN) || !SetEndOfFile(h) ||
            !WriteFile(h, pid.data(), static_cast<DWORD>(pid.size()), &written, nullptr) || written != pid.size()) {
            throwLockFileError(lockPath, "write", lastErrorText());
        }
        return lock;
    }
    throwLockFileError(lockPath, "lock", "the lock file was replaced while it was being locked");
}

bool WriterLock::held() const noexcept { return handle_ != nullptr; }

void WriterLock::release() noexcept {
    if (handle_ != nullptr) {
        OVERLAPPED region;
        lockRegion(region);
        UnlockFileEx(handle_, 0, 1, 0, &region);
        CloseHandle(handle_);
        handle_ = nullptr;
    }
}

#else

namespace {
// True when `fd` is the file `path` names now; false when the path is missing or names
// another file, because the file `fd` holds was removed after it was opened.
bool namesFile(int fd, const std::string& path) {
    struct stat held {};
    struct stat current {};
    if (::fstat(fd, &held) != 0 || ::stat(path.c_str(), &current) != 0) return false;
    return held.st_dev == current.st_dev && held.st_ino == current.st_ino;
}
} // namespace

WriterLock WriterLock::acquire(const std::string& storePath, const std::function<void()>& afterOpen) {
    const std::string lockPath = lockPathOf(storePath);
    for (int attempt = 0; attempt < kLockAttempts; ++attempt) {
        const int fd = ::open(lockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd < 0) throwLockFileError(lockPath, "open", std::strerror(errno));
        WriterLock lock;
        lock.fd_ = fd;  // closed by ~WriterLock on every failure and retry below
        if (attempt == 0 && afterOpen) afterOpen();

        if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
            const int error = errno;
            lock.fd_ = -1;
            if (error != EWOULDBLOCK) {
                ::close(fd);
                throwLockFileError(lockPath, "lock", std::strerror(error));
            }
            if (!namesFile(fd, lockPath)) {  // locked by a holder that is removing it; no writer's
                ::close(fd);
                continue;
            }
            char buffer[32] = {};
            const ssize_t n = ::pread(fd, buffer, sizeof(buffer) - 1, 0);
            ::close(fd);
            throwHeld(storePath, n > 0 ? std::string(buffer, static_cast<size_t>(n)) : std::string());
        }
        if (!namesFile(fd, lockPath)) continue;  // an orphan (see kLockAttempts); released here
        const std::string pid = std::to_string(currentPid());
        if (::ftruncate(fd, 0) != 0 ||
            ::pwrite(fd, pid.data(), pid.size(), 0) != static_cast<ssize_t>(pid.size())) {
            throwLockFileError(lockPath, "write", std::strerror(errno));
        }
        return lock;
    }
    throwLockFileError(lockPath, "lock", "the lock file was replaced while it was being locked");
}

bool WriterLock::held() const noexcept { return fd_ >= 0; }

void WriterLock::release() noexcept {
    if (fd_ >= 0) {
        ::flock(fd_, LOCK_UN);
        ::close(fd_);
        fd_ = -1;
    }
}

#endif

} // namespace slideio::processor::detail
