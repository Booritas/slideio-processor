// Requirements: UR-API-2, UR-STG-4, UR-RES-9
#pragma once

/**
 * @file slide_run_result.hpp
 * @brief SlideRunResult: what one SlideRun::run() did, per stage and per export.
 */

#include <slideio/processor/pipeline/export.hpp>

#include <cstdint>
#include <memory>

namespace slideio::processor {

class SlideRun;

/**
 * @brief What a slide run did: for each stage of the pipeline file, in file order,
 * whether it ran and what it wrote; for each export, in file order, the file and its size.
 *
 * A stage that was Complete and current is reported as not run, with 0 objects and 0
 * truncated objects. A stage that ran reports StageRunner::objectsWritten() and
 * StageRunner::truncatedObjects() of its run. An export reports its plain file name, as
 * the pipeline file gives it, and the bytes of the file written.
 * @par Thread-safety
 * ThreadCompatible: const members are safe to call concurrently.
 * @par Requirements
 * UR-API-2, UR-STG-4, UR-RES-9
 */
class SLIDEIO_PROCESSOR_ENGINE_EXPORTS SlideRunResult {
public:
    /**
     * @brief Creates an empty result: no stages and no exports.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-API-2
     */
    SlideRunResult();
    /**
     * @brief Copies a result; the copy shares nothing with the original.
     * @param other The result to copy.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * Reads @p other; ThreadCompatible.
     * @par Requirements
     * UR-API-2
     */
    SlideRunResult(const SlideRunResult& other);
    /**
     * @brief Moves a result. The moved-from result reads as empty.
     * @param other The result to move from.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-API-2
     */
    SlideRunResult(SlideRunResult&& other) noexcept;
    /**
     * @brief Replaces this result with a copy of another.
     * @param other The result to copy.
     * @return This result.
     * @throws std::bad_alloc When allocation fails; this result is then unchanged.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-API-2
     */
    SlideRunResult& operator=(const SlideRunResult& other);
    /**
     * @brief Replaces this result by moving another.
     * @param other The result to move from; reads as empty afterwards.
     * @return This result.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-API-2
     */
    SlideRunResult& operator=(SlideRunResult&& other) noexcept;
    ~SlideRunResult();

    /**
     * @brief Returns the number of stages: those of the pipeline file.
     * @return The count.
     * @throws Nothing.
     * @par Thread-safety
     * Safe to call concurrently on a const result.
     * @par Requirements
     * UR-API-2
     */
    int32_t stageCount() const noexcept;
    /**
     * @brief Returns a stage's name.
     * @param index Position in file order, from 0 to stageCount() - 1.
     * @return The name; valid until the result is modified or destroyed.
     * @throws InvalidArgumentError When index is out of range.
     * @par Thread-safety
     * Safe to call concurrently on a const result.
     * @par Requirements
     * UR-API-2
     */
    const char* stageName(int32_t index) const;
    /**
     * @brief Tells whether a stage ran, rather than being skipped as Complete and current.
     * @param index Position in file order, from 0 to stageCount() - 1.
     * @return True when it ran.
     * @throws InvalidArgumentError When index is out of range.
     * @par Thread-safety
     * Safe to call concurrently on a const result.
     * @par Requirements
     * UR-API-2, UR-STG-4
     */
    bool stageRan(int32_t index) const;
    /**
     * @brief Returns the objects a stage wrote (StageRunner::objectsWritten()).
     * @param index Position in file order, from 0 to stageCount() - 1.
     * @return The count; 0 for a stage that did not run.
     * @throws InvalidArgumentError When index is out of range.
     * @par Thread-safety
     * Safe to call concurrently on a const result.
     * @par Requirements
     * UR-API-2
     */
    int64_t stageObjectsWritten(int32_t index) const;
    /**
     * @brief Returns the objects a stage's operations dropped as truncated
     * (StageRunner::truncatedObjects()).
     *
     * One per dropped tile view, not per distinct object: each truncated object
     * counts at least once, one dropped in several tiles' views counts once per
     * view, and the figure depends on the tile size. 0 means nothing was dropped.
     * @param index Position in file order, from 0 to stageCount() - 1.
     * @return The count; 0 for a stage that did not run.
     * @throws InvalidArgumentError When index is out of range.
     * @par Thread-safety
     * Safe to call concurrently on a const result.
     * @par Requirements
     * UR-API-2
     */
    int64_t stageTruncatedObjects(int32_t index) const;

    /**
     * @brief Returns the number of exports written: those of the pipeline file.
     * @return The count.
     * @throws Nothing.
     * @par Thread-safety
     * Safe to call concurrently on a const result.
     * @par Requirements
     * UR-API-2
     */
    int32_t exportCount() const noexcept;
    /**
     * @brief Returns an export's file name, the plain name the pipeline file gives.
     * @param index Position in file order, from 0 to exportCount() - 1.
     * @return The name; valid until the result is modified or destroyed.
     * @throws InvalidArgumentError When index is out of range.
     * @par Thread-safety
     * Safe to call concurrently on a const result.
     * @par Requirements
     * UR-API-2, UR-RES-9
     */
    const char* exportFile(int32_t index) const;
    /**
     * @brief Returns the size of an export's file.
     * @param index Position in file order, from 0 to exportCount() - 1.
     * @return The bytes written (ExportResult::bytesWritten()).
     * @throws InvalidArgumentError When index is out of range.
     * @par Thread-safety
     * Safe to call concurrently on a const result.
     * @par Requirements
     * UR-API-2, UR-RES-9
     */
    int64_t exportBytes(int32_t index) const;

private:
    friend class SlideRun;
    void addStage(const char* name, bool ran, int64_t objectsWritten, int64_t truncatedObjects);
    void addExport(const char* file, int64_t bytes);
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace slideio::processor
