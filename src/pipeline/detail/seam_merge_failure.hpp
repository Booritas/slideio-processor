// Requirements: UR-SEG-3, UR-EXE-5
#pragma once

#include <slideio/processor/core/error.hpp>

#include <exception>
#include <new>
#include <string>

namespace slideio::processor::detail {

/// Rethrows a failure of the seam merge that follows a run's last tile
/// (SeamMerger::resolve(), the object sink it emits to included) as the run
/// reports failures: a ProcessorError, a CancelledError among them, and
/// std::bad_alloc as they are; anything else, a standard exception from a sink
/// or a failed lock, as a TileError with no tile (hasTile() false) naming the
/// stage, with the original what() as its cause.
[[noreturn]] inline void rethrowSeamMergeFailure(const std::exception_ptr& failure, const std::string& stageName) {
    std::string cause;
    try {
        std::rethrow_exception(failure);
    } catch (const ProcessorError&) {
        throw;
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& e) {
        cause = e.what();
    } catch (...) {
        cause = "an exception that is not a std::exception";
    }
    ErrorContext context;
    context.stageName = stageName.c_str();
    context.cause = cause.c_str();
    throw TileError("the seam merge after the last tile failed", context);
}

} // namespace slideio::processor::detail
