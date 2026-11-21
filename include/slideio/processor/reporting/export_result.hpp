// Requirements: UR-RES-9, UR-QA-3
#pragma once

/**
 * @file export_result.hpp
 * @brief ExportResult: what a report writer returns.
 */

#include <slideio/processor/core/export.hpp>

#include <cstdint>

namespace slideio::processor {

/**
 * @brief The outcome of one export: how many objects and bytes were written, and
 * whether the export was cancelled.
 *
 * A value type, returned by every writer of the reporting module.
 * @par Thread-safety
 * ThreadCompatible: an immutable value; concurrent reads are safe.
 * @par Requirements
 * UR-RES-9, UR-QA-3
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS ExportResult {
public:
    /**
     * @brief Creates a result.
     * @param objectsWritten The number of objects written (up to the cancellation, if any).
     * @param bytesWritten The size of the finished output in bytes.
     * @param wasCancelled True when the export was cancelled.
     * @throws Nothing.
     */
    ExportResult(int64_t objectsWritten, int64_t bytesWritten, bool wasCancelled) noexcept;

    /**
     * @brief Returns the number of objects written.
     * @return The count.
     * @throws Nothing.
     */
    int64_t objectsWritten() const noexcept;
    /**
     * @brief Returns the size of the output in bytes.
     * @return The byte count.
     * @throws Nothing.
     */
    int64_t bytesWritten() const noexcept;
    /**
     * @brief Tells whether the export was cancelled.
     * @return True when cancelled.
     * @throws Nothing.
     */
    bool wasCancelled() const noexcept;

private:
    int64_t objectsWritten_;
    int64_t bytesWritten_;
    bool wasCancelled_;
};

} // namespace slideio::processor
