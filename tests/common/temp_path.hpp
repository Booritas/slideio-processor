// Test-only: unique temporary file paths and whole-file reads and writes.
#pragma once

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace slideio::processor {

// A path in the temp directory, unique per process and per call. The file is not created.
// `stem` is UTF-8.
inline std::filesystem::path uniqueTempPath(const char* stem, const char* ext) {
    static std::atomic<int> counter{0};
#ifdef _WIN32
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(::getpid());
#endif
    const std::string name = std::string("slideio_proc_") + stem + "_" + std::to_string(pid) + "_" +
                             std::to_string(counter.fetch_add(1)) + ext;
    return std::filesystem::temp_directory_path() / std::filesystem::u8path(name);
}

// Reads a whole file in binary mode; empty string when it cannot be opened.
inline std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Creates or truncates the file and writes `content` in binary mode.
inline void writeFile(const std::filesystem::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

// Removes the file when it goes out of scope, so a failing test leaves nothing behind.
class TempFileGuard {
public:
    explicit TempFileGuard(std::filesystem::path path) : path_(std::move(path)) {}
    TempFileGuard(const TempFileGuard&) = delete;
    TempFileGuard& operator=(const TempFileGuard&) = delete;
    ~TempFileGuard() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

private:
    std::filesystem::path path_;
};

} // namespace slideio::processor
