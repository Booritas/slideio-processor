// Requirements: UR-PIX-3, UR-PIX-4, UR-IMG-11
#include "detail/cv_plane.hpp"

#include <slideio/processor/core/error.hpp>

#include <limits>
#include <string>

namespace slideio::processor::detail {

namespace {

// The OpenCV depth of each type OpenCV can hold, or -1. Mirrors the OpenCV
// backend's mapping; the unsigned 32-bit and 64-bit integer depths exist from
// OpenCV 5 on.
int depthOf(DataType type) {
    switch (type) {
    case DataType::Byte:
        return CV_8U;
    case DataType::Int8:
        return CV_8S;
    case DataType::UInt16:
        return CV_16U;
    case DataType::Int16:
        return CV_16S;
    case DataType::Int32:
        return CV_32S;
    case DataType::Float16:
        return CV_16F;
    case DataType::Float32:
        return CV_32F;
    case DataType::Float64:
        return CV_64F;
#if CV_VERSION_MAJOR >= 5
    case DataType::UInt32:
        return CV_32U;
    case DataType::UInt64:
        return CV_64U;
    case DataType::Int64:
        return CV_64S;
#endif
    default:
        return -1;
    }
}

DataTypeSet cvTypes() {
    DataTypeSet types;
    for (int32_t i = 0; i < 11; ++i) {
        if (depthOf(static_cast<DataType>(i)) >= 0) {
            types.add(static_cast<DataType>(i));
        }
    }
    return types;
}

} // namespace

cv::Mat planeAsMat(const BlockView& view, int32_t plane) {
    if (plane < 0 || plane >= view.planeCount()) {
        throw InternalError("planeAsMat: plane index out of range");
    }
    const DataType type = view.planeType(plane);
    const int depth = depthOf(type);
    if (depth < 0) {
        throw UnsupportedDataTypeError("planeAsMat: OpenCV cannot hold the plane's type", type, cvTypes());
    }
    constexpr int64_t kIntMax = std::numeric_limits<int>::max();
    if (view.width() > kIntMax || view.height() > kIntMax) {
        throw InternalError("planeAsMat: the plane is too large for a cv::Mat");
    }
    return cv::Mat(static_cast<int>(view.height()), static_cast<int>(view.width()), CV_MAKETYPE(depth, 1),
                   view.planeData(plane), static_cast<size_t>(view.planeRowStrideBytes(plane)));
}

void translateCv(const char* operationName) {
    try {
        throw;
    } catch (const cv::Exception& e) {
        ErrorContext context;
        context.operationName = operationName;
        context.cause = e.what();
        throw InternalError((std::string(operationName) + ": OpenCV failed").c_str(), context);
    }
}

} // namespace slideio::processor::detail
