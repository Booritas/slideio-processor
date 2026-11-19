// Requirements: UR-STG-1
// Test-only: a slide file SlideIO opens, and a temporary directory that removes itself.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace slideio::processor::testing_support {

// Writes an 8-bit, 3-sample (RGB), tiled TIFF of width x height to `file`, through
// libtiff, and returns `file`. Pixel values are a deterministic gradient. Throws
// std::runtime_error when libtiff cannot write the file.
std::filesystem::path writeTestSlide(const std::filesystem::path& file, int32_t width, int32_t height);

// A unique path in the temp directory, not created; remove_all on destruction, so a
// failing test leaves nothing behind.
class TempDirectory {
public:
    TempDirectory();
    ~TempDirectory();
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    const std::filesystem::path& path() const noexcept { return path_; }
    // The path as UTF-8, as the library's `const char*` arguments take it.
    std::string utf8() const { return path_.u8string(); }

private:
    std::filesystem::path path_;
};

} // namespace slideio::processor::testing_support
