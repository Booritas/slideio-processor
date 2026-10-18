#pragma once

#include <slideio/processor/core/data_type.hpp>

#include <cstdint>

namespace slideio::processor::detail {

/// The data types an OpenCV of the given major version computes natively.
///
/// From major 5 on that is all eleven types; before it, every type except
/// UInt32, Int64 and UInt64 (UR-BLD-4). A pure function, so that both
/// branches are testable under whichever OpenCV the build resolved.
DataTypeSet backendTypesForOpenCvMajor(int32_t major) noexcept;

} // namespace slideio::processor::detail
