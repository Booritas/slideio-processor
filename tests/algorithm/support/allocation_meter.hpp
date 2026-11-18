// Requirements: UR-EXE-2
#pragma once

// Measures the peak bytes the calling thread allocates through operator new while
// a meter is in scope. The executable allocation_meter.cpp is linked into
// replaces every form of the global operator new and operator delete with a
// counting pair; it must compile the code it measures into itself, because a
// shared library may allocate through its own operator new (under MSVC every
// module has its own). OpenCV is linked statically into that executable, so
// its operator-new allocations (a cv::AutoBuffer beyond its fixed size, its
// internal std::vectors) are counted; only the buffers it takes through
// cv::fastMalloc, which allocates through malloc, are not seen. Only the
// calling thread is counted: work an operation hands to other threads escapes
// the meter.

#include <slideio/processor/core/data_type.hpp>
#include <slideio/processor/pipeline/operation.hpp>

#include <cstdint>
#include <functional>
#include <vector>

namespace slideio::processor::testing_support {

class AllocationLedger;

/// Counts the live and peak bytes the constructing thread allocates through any
/// form of operator new while it is alive. Only blocks allocated in its scope
/// count; freeing one of them in its scope, on the same thread, lowers the live
/// count. Blocks allocated before it, after it or on another thread are not
/// counted. One meter per thread at a time: constructing a second one while the
/// first is alive throws std::logic_error.
class AllocationMeter {
public:
    AllocationMeter();
    ~AllocationMeter();
    AllocationMeter(const AllocationMeter&) = delete;
    AllocationMeter& operator=(const AllocationMeter&) = delete;

    /// The largest number of counted bytes live at once since construction.
    int64_t peakBytes() const;
    /// The counted bytes live now.
    int64_t liveBytes() const;

private:
    AllocationLedger* ledger_;
};

/// Applies @p op as applyDirect does and returns the peak bytes its apply()
/// calls allocate through operator new: the largest peak of any one call (one
/// call per plane for a PerChannel operation). The input, the output and the
/// context are built before the meter starts, so only apply() is measured.
int64_t measureApplyPeak(Operation& op, const std::vector<DataType>& inputTypes,
                         const std::vector<int32_t>& sourceChannels, int64_t width, int64_t height,
                         const std::function<double(int32_t plane, int64_t x, int64_t y)>& pixel);

} // namespace slideio::processor::testing_support
