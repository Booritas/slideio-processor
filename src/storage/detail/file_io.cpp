#include "storage/detail/file_io.hpp"

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/log.hpp>

#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace slideio::processor::detail {

std::string errnoText(int error) { return std::error_code(error, std::generic_category()).message(); }

void throwIo(const std::string& message, const std::string& cause) {
    ErrorContext context;
    context.cause = cause.empty() ? nullptr : cause.c_str();
    throw IoError(message.c_str(), context);
}

#ifdef _WIN32

int seekFile(std::FILE* file, int64_t offset, int whence) { return _fseeki64(file, offset, whence); }

int64_t tellFile(std::FILE* file) { return _ftelli64(file); }

void syncFile(const std::filesystem::path& path) {
    // FlushFileBuffers needs a handle opened for writing; every share flag is given, so a
    // reader or a pending delete elsewhere never makes the sync fail.
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        throwIo("cannot sync the file",
                std::error_code(static_cast<int>(error), std::system_category()).message());
    }
    const BOOL flushed = FlushFileBuffers(handle);
    const DWORD error = flushed ? 0 : GetLastError();
    CloseHandle(handle);
    if (!flushed) {
        throwIo("cannot sync the file",
                std::error_code(static_cast<int>(error), std::system_category()).message());
    }
}

#else

// Unverified here: the POSIX side is compiled on Linux and macOS only.
static_assert(sizeof(off_t) == 8, "64-bit file offsets are required (_FILE_OFFSET_BITS=64 on 32-bit Linux)");

int seekFile(std::FILE* file, int64_t offset, int whence) { return fseeko(file, static_cast<off_t>(offset), whence); }

int64_t tellFile(std::FILE* file) { return static_cast<int64_t>(ftello(file)); }

namespace {

// Syncs the descriptor and returns 0, or the errno of the failure. On macOS fsync leaves
// the data in the drive's cache, so F_FULLFSYNC is asked first; a filesystem that does not
// support it (some network ones) falls back to fsync.
int syncDescriptor(int fd, bool full) {
#ifdef __APPLE__
    if (full && ::fcntl(fd, F_FULLFSYNC) == 0) return 0;
#else
    (void)full;
#endif
    return ::fsync(fd) == 0 ? 0 : errno;
}

// Syncs `path`, opened read-only, and returns 0, or the errno of the failed sync. Throws
// IoError when it cannot be opened.
int syncPath(const std::filesystem::path& path, bool full) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) throwIo("cannot sync the file", errnoText(errno));
    const int error = syncDescriptor(fd, full);
    ::close(fd);
    return error;
}

} // namespace

void syncFile(const std::filesystem::path& path) {
    const int error = syncPath(path, true);
    if (error != 0) throwIo("cannot sync the file", errnoText(error));
    // A new file's name lives in its directory, which has to reach the disk as well. Some
    // filesystems (FUSE, network) cannot sync a directory and say so with EINVAL, EBADF
    // or ENOTSUP; there that sync is best-effort, as it is all they offer.
    const std::filesystem::path parent = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
    const int directoryError = syncPath(parent, false);
    if (directoryError == EINVAL || directoryError == EBADF || directoryError == ENOTSUP) {
        logMessage(LogLevel::Debug, "cannot sync the directory of a file (%s); its filesystem does not support it",
                   errnoText(directoryError).c_str());
    } else if (directoryError != 0) {
        throwIo("cannot sync the directory of the file", errnoText(directoryError));
    }
}

#endif

bool removeFile(const std::filesystem::path& path) {
    std::error_code error;
    const bool removed = std::filesystem::remove(path, error);
    if (error) throwIo("cannot delete the file", error.message());
    return removed;
}

} // namespace slideio::processor::detail
