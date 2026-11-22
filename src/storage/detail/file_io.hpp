// Requirements: UR-RES-7, UR-STG-9
#pragma once

// File helpers shared by the raster files' code: the spool, the libtiff wrapper and the
// writer's publish (rulings S46, S47 and S52). The platform code lives here alone; the
// POSIX side is compiled on Linux and macOS only, and is unverified on this project's
// Windows build machine.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

namespace slideio::processor::detail {

// The text of an errno value. std::strerror is deprecated on MSVC and not thread-safe.
std::string errnoText(int error);

// Throws IoError `message`, with `cause` as its cause; no cause when `cause` is empty.
[[noreturn]] void throwIo(const std::string& message, const std::string& cause);

// fseek and ftell with 64-bit offsets: _fseeki64 and _ftelli64 on Windows, fseeko and
// ftello elsewhere. seekFile returns 0 on success, as fseek does; tellFile returns -1 on
// failure.
int seekFile(std::FILE* file, int64_t offset, int whence);
int64_t tellFile(std::FILE* file);

// Forces the file's written bytes to stable storage: FlushFileBuffers on Windows,
// F_FULLFSYNC (falling back to fsync) on macOS, fsync elsewhere. On POSIX the directory
// holding it is synced too, so a new file's name is as durable as its bytes; where the
// filesystem cannot sync a directory (EINVAL, EBADF or ENOTSUP) that step is skipped and
// logged at Debug. Throws IoError "cannot sync the file" with the system's text as the
// cause.
void syncFile(const std::filesystem::path& path);

// Deletes the file and returns true; returns false when it was already gone, which is not
// an error. Throws IoError "cannot delete the file" with the system's text as the cause.
bool removeFile(const std::filesystem::path& path);

} // namespace slideio::processor::detail
