#include "storage/detail/file_io.hpp"

#include <slideio/processor/core/error.hpp>

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
        throwIo("cannot sync '" + path.u8string() + "'",
                std::error_code(static_cast<int>(error), std::system_category()).message());
    }
    const BOOL flushed = FlushFileBuffers(handle);
    const DWORD error = flushed ? 0 : GetLastError();
    CloseHandle(handle);
    if (!flushed) {
        throwIo("cannot sync '" + path.u8string() + "'",
                std::error_code(static_cast<int>(error), std::system_category()).message());
    }
}

#else

// Unverified here: the POSIX side is compiled on Linux and macOS only.
int seekFile(std::FILE* file, int64_t offset, int whence) { return fseeko(file, static_cast<off_t>(offset), whence); }

int64_t tellFile(std::FILE* file) { return static_cast<int64_t>(ftello(file)); }

namespace {

// fsync on a descriptor opened with `flags`; throws naming `path`.
void syncPath(const std::filesystem::path& path, int flags) {
    const int fd = ::open(path.c_str(), flags);
    if (fd < 0) throwIo("cannot sync '" + path.u8string() + "'", errnoText(errno));
    const int error = ::fsync(fd) == 0 ? 0 : errno;
    ::close(fd);
    if (error != 0) throwIo("cannot sync '" + path.u8string() + "'", errnoText(error));
}

} // namespace

void syncFile(const std::filesystem::path& path) {
    syncPath(path, O_RDONLY);
    // A new file's name lives in its directory, which has to reach the disk as well.
    const std::filesystem::path parent = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
    syncPath(parent, O_RDONLY);
}

#endif

void removeFile(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::remove(path, error);
    if (error) throwIo("cannot delete '" + path.u8string() + "'", error.message());
}

} // namespace slideio::processor::detail
