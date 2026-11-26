// Requirements: UR-SEG-1, UR-ALG-6
#pragma once

#include <cstdint>
#include <vector>

namespace slideio::processor::detail {

/// The triangle method's cut-off bin of a histogram: tissue is a bin above it.
///
/// Zack GW, Rogers WE, Latt SA. Automatic measurement of sister chromatid
/// exchange frequency. J Histochem Cytochem 1977;25:741-753. A port of OpenCV
/// 4.14's getThreshVal_Triangle_8u (modules/imgproc/src/thresh.cpp),
/// generalised from 256 bins of int counts to bins.size() bins of int64_t
/// counts, step for step: the bounds widened by one, the histogram flipped
/// when its peak is nearer the left bound, the distance to the line from the
/// bound to the peak maximised (the first maximum kept), OpenCV's final
/// decrement, and the flip undone. On 256 bins it equals what cv::threshold
/// returns with THRESH_TRIANGLE for the same pixels. The flip is read through
/// the index, so nothing is allocated.
/// @return The cut-off bin. -1 has two meanings: an empty or all-zero
/// histogram, which has no cut-off; and a non-empty histogram that rises
/// straight to its peak, as OpenCV's does, where the cut-off is below bin 0 and
/// every counted value is above it. A caller tells them apart by whether
/// anything was counted.
inline int64_t triangleThresholdBin(const std::vector<int64_t>& bins) {
    const auto n = static_cast<int64_t>(bins.size());
    bool counted = false;
    for (int64_t i = 0; i < n && !counted; ++i) {
        counted = bins[static_cast<size_t>(i)] > 0;
    }
    if (!counted) {
        return -1;
    }
    int64_t leftBound = 0;
    int64_t rightBound = 0;
    int64_t maxInd = 0;
    int64_t maxCount = 0;
    bool isFlipped = false;
    const auto h = [&](int64_t i) {
        return bins[static_cast<size_t>(isFlipped ? n - 1 - i : i)];
    };

    for (int64_t i = 0; i < n; ++i) {
        if (h(i) > 0) {
            leftBound = i;
            break;
        }
    }
    if (leftBound > 0) {
        --leftBound;
    }
    // OpenCV stops before bin 0 here.
    for (int64_t i = n - 1; i > 0; --i) {
        if (h(i) > 0) {
            rightBound = i;
            break;
        }
    }
    if (rightBound < n - 1) {
        ++rightBound;
    }
    for (int64_t i = 0; i < n; ++i) {
        if (h(i) > maxCount) {
            maxCount = h(i);
            maxInd = i;
        }
    }

    if (maxInd - leftBound < rightBound - maxInd) {
        isFlipped = true;
        leftBound = n - 1 - rightBound;
        maxInd = n - 1 - maxInd;
    }

    double thresh = static_cast<double>(leftBound);
    // The distance up to a constant factor, as OpenCV computes it.
    const auto a = static_cast<double>(maxCount);
    const auto b = static_cast<double>(leftBound - maxInd);
    double dist = 0.0;
    for (int64_t i = leftBound + 1; i <= maxInd; ++i) {
        const double tempDist = a * static_cast<double>(i) + b * static_cast<double>(h(i));
        if (tempDist > dist) {
            dist = tempDist;
            thresh = static_cast<double>(i);
        }
    }
    thresh -= 1.0;

    if (isFlipped) {
        thresh = static_cast<double>(n - 1) - thresh;
    }
    return static_cast<int64_t>(thresh);
}

} // namespace slideio::processor::detail
