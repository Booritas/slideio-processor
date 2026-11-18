// Requirements: UR-PIX-3, UR-PIX-4, UR-IMG-11
#pragma once

#include <slideio/processor/core/block_view.hpp>

#include <opencv2/core.hpp>

#include <cstdint>

namespace slideio::processor::detail {

/// A single-channel cv::Mat header over @p plane of @p view: no copy, the
/// plane's own memory and row stride, view.height() rows of view.width()
/// samples. Its depth is the one the OpenCV backend maps the plane's type to:
/// Byte CV_8U, Int8 CV_8S, UInt16 CV_16U, Int16 CV_16S, Int32 CV_32S, Float16
/// CV_16F, Float32 CV_32F, Float64 CV_64F, and from OpenCV 5 on UInt32
/// CV_32U, UInt64 CV_64U, Int64 CV_64S. Writing through the header writes the
/// plane. Throws UnsupportedDataTypeError for a type OpenCV cannot hold, and
/// InternalError for a plane index out of range or dimensions beyond int.
cv::Mat planeAsMat(const BlockView& view, int32_t plane);

/// Translates the cv::Exception being handled into an InternalError naming
/// @p operationName, with OpenCV's message as its cause(). Call it only from
/// a `catch (const cv::Exception&)` handler; any other exception being
/// handled propagates unchanged. Every argument passed to OpenCV has been
/// checked beforehand, so a failure inside OpenCV is a defect.
[[noreturn]] void translateCv(const char* operationName);

} // namespace slideio::processor::detail
