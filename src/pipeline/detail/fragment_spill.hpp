// Requirements: UR-EXE-2, UR-SEG-3
#pragma once

#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/pipeline/operation.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace slideio::processor::detail {

/// 32 random lowercase hex digits from std::random_device, for scratch file names.
/// Throws std::bad_alloc, or what std::random_device throws.
std::string randomHexName();

/// One scratch file of seam fragments written out of memory: created as
/// `<directory>/seam-<32 random lowercase hex>.bin`, appended to, read back at
/// recorded offsets, and removed when the object is destroyed.
///
/// A record is, in native byte order: int32 ring count; per ring a uint8 hole
/// flag, an int64 point count and the points as int64 x, y pairs; int32 partial
/// count; per partial int64 count, double sum, sum of squares, minimum and
/// maximum, int32 channel.
///
/// Every failure to create, write or read the file throws IoError naming the
/// file. Thread-safe: an internal mutex serialises the file.
class FragmentSpill {
public:
    /// Creates the file in `directory` (UTF-8). Throws IoError when it cannot be
    /// created, std::bad_alloc.
    explicit FragmentSpill(const std::string& directory);
    /// Closes and removes the file. Throws nothing.
    ~FragmentSpill();
    FragmentSpill(const FragmentSpill&) = delete;
    FragmentSpill& operator=(const FragmentSpill&) = delete;

    /// Appends one fragment and returns the offset of its record; `bytes`
    /// receives the record's size. Throws IoError, std::bad_alloc; on failure
    /// nothing is recorded and the next write goes to the same offset.
    int64_t write(const Polygon& polygon, const std::vector<IntensityPartial>& partials, int64_t& bytes);
    /// Reads back the record at `offset`. Throws IoError, std::bad_alloc.
    void read(int64_t offset, Polygon& polygon, std::vector<IntensityPartial>& partials);

    /// The file's path, UTF-8.
    const std::string& path() const noexcept { return pathText_; }

private:
    [[noreturn]] void fail(const char* what);

    std::filesystem::path path_;
    std::string pathText_;
    std::mutex mutex_;
    std::fstream file_;
    int64_t size_ = 0;
};

} // namespace slideio::processor::detail
