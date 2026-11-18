// Requirements: UR-EXE-2, UR-EXT-4
#pragma once

#include <cstdint>

namespace slideio::processor::detail {

/// The bytes a standard-library allocator may add to one block it takes from
/// operator new, beyond the bytes the container asked for. MSVC's
/// std::allocator aligns a block of 4 KiB or more to 32 bytes by
/// over-allocating 39 bytes and keeping the original pointer in front of the
/// aligned one; 64 bounds that. An operation's working-set declaration counts
/// it once per std::vector or std::string buffer that is live at the peak.
constexpr int64_t kAllocatorBlockOverhead = 64;

} // namespace slideio::processor::detail
