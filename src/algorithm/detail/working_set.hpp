// Requirements: UR-EXE-2, UR-EXT-4
#pragma once

#include <cstdint>

namespace slideio::processor::detail {

/// The bytes a standard-library allocator may add to one block it takes from
/// operator new, beyond the bytes the container asked for. MSVC's
/// std::allocator aligns a block of 4 KiB or more to 32 bytes by
/// over-allocating 39 bytes (47 with debug checks) and keeping the original
/// pointer in front of the aligned one; 64 bounds that. An operation's working-set declaration counts
/// it once per std::vector or std::string buffer that is live at the peak.
constexpr int64_t kAllocatorBlockOverhead = 64;

/// The bytes OpenCV takes through operator new for each image it allocates:
/// the cv::UMatData record that tracks the pixels (104 bytes under MSVC);
/// 128 bounds that. The pixels themselves come from cv::fastMalloc.
constexpr int64_t kCvMatBookkeeping = 128;

} // namespace slideio::processor::detail
