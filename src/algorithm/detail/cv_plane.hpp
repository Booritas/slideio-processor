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
/// plane. The header is writable even when @p view is a const input view:
/// a header built over an input must only be read, never written through.
/// Throws UnsupportedDataTypeError for a type OpenCV cannot hold, and
/// InternalError for a plane index out of range or dimensions beyond int.
/// Building the header may also throw cv::Exception, so call it inside the
/// caller's `catch (const cv::Exception&)` handler that calls translateCv, as
/// the OpenCV call itself is.
cv::Mat planeAsMat(const BlockView& view, int32_t plane);

/// Translates the cv::Exception being handled into an InternalError naming
/// @p operationName, whose cause() names only OpenCV's error code and the
/// function that raised it, as "OpenCV error -215 in cv::Canny": OpenCV's own
/// message may print the values it checked, so it is not copied. OpenCV's
/// out-of-memory error (cv::Error::StsNoMem) becomes std::bad_alloc instead.
/// Call it only from a `catch (const cv::Exception&)` handler; any other
/// exception being handled propagates unchanged. Every argument passed to
/// OpenCV has been checked beforehand, so any other failure inside OpenCV is
/// a defect.
[[noreturn]] void translateCv(const char* operationName);

/// Makes OpenCV run every call on the calling thread (cv::setNumThreads(0)),
/// once per process. The tile workers are the engine's parallelism: OpenCV's
/// own threads would oversubscribe the cores, make an operation's working set
/// depend on the host's core count, and allocate on threads the memory
/// accounting does not see. OpenCV is linked privately into the engine, so
/// nothing else changes its thread count. Call it from the constructor of
/// every operation whose apply() calls OpenCV, never from a static
/// initialiser: OpenCV's own statics may not exist yet. A cv::Exception is
/// translated by translateCv into an InternalError (std::bad_alloc when
/// OpenCV is out of memory), and a later call tries again.
/// @par Thread-safety
/// Safe to call from any thread, any number of times.
void useSequentialOpenCv();

} // namespace slideio::processor::detail
