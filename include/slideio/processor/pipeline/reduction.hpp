// Requirements: UR-PIX-5, UR-QA-3, UR-IMG-2
#pragma once

/**
 * @file reduction.hpp
 * @brief Reduction and Histogram64: whole-slide accumulators fed tile by tile.
 */

#include <slideio/processor/core/block_view.hpp>
#include <slideio/processor/pipeline/export.hpp>

#include <cstdint>
#include <memory>

namespace slideio::processor {

/**
 * @brief A value accumulated over every tile of a run and combined across
 * workers.
 *
 * Integer accumulators (histogram bins, pixel counts, areas) combine in any
 * order, so the worker count cannot affect them, and isOrderIndependent()
 * returns true. Floating-point accumulators must return false; the framework
 * then combines them in canonical tile order, because float addition is not
 * associative (UR-QA-3).
 * @par Thread-safety
 * See each member.
 * @par Requirements
 * UR-PIX-5, UR-QA-3, UR-IMG-2
 */
class SLIDEIO_PROCESSOR_ENGINE_EXPORTS Reduction {
public:
    /** @brief Destroys the reduction. */
    virtual ~Reduction();

    /**
     * @brief Accumulates one tile.
     * @param block The tile's pixels; read during this call only.
     * @param tileIndex The tile's row-major index in its grid; unused by
     * integer reductions, and the key order-dependent reductions are combined by.
     * @throws std::bad_alloc When memory is exhausted.
     * @par Thread-safety
     * ThreadSafe: may be called concurrently on one object.
     * @par Requirements
     * UR-PIX-5, UR-IMG-2
     */
    virtual void accumulate(const BlockView& block, int64_t tileIndex) = 0;
    /**
     * @brief Adds another reduction of the same kind and shape into this one.
     * @param other The reduction to add.
     * @throws InvalidArgumentError When @p other is not of this kind or does
     * not have the same shape.
     * @par Thread-safety
     * Requires exclusive access to this object; @p other is read.
     * @par Requirements
     * UR-PIX-5, UR-QA-3
     */
    virtual void combine(const Reduction& other) = 0;
    /**
     * @brief Tells whether combination order cannot change the result.
     * @return True for integer reductions, false for floating-point ones.
     * @throws Nothing.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-QA-3
     */
    virtual bool isOrderIndependent() const = 0;

protected:
    /** @brief Creates the base of a reduction. */
    Reduction();
    /**
     * @brief Copies the base of a reduction.
     * @param other The reduction to copy.
     */
    Reduction(const Reduction& other);
    /**
     * @brief Copy-assigns the base of a reduction.
     * @param other The reduction to copy.
     * @return This object.
     */
    Reduction& operator=(const Reduction& other);
};

/**
 * @brief A fixed-range histogram with 64-bit bins, accumulated across threads.
 *
 * Every plane of every accumulated view is read, each sample as a double by its
 * plane's DataType, and binned into floor((v - min) / (max - min) * binCount);
 * a value equal to the maximum goes in the last bin. A value outside
 * [min, max], and NaN, is ignored and not counted. Bins are atomic, so
 * accumulate() is ThreadSafe and the result is independent of thread count and
 * order (UR-PIX-5's streamed Otsu equals one computed from an in-memory
 * histogram). Copies copy the bin values. A moved-from histogram may only be
 * destroyed or assigned to; every other call throws InvalidArgumentError.
 * @par Thread-safety
 * accumulate() is ThreadSafe. Readers need the accumulation to have finished.
 * combine() and assignment need exclusive access to the target.
 * @par Requirements
 * UR-PIX-5, UR-QA-3, UR-IMG-2
 */
class SLIDEIO_PROCESSOR_ENGINE_EXPORTS Histogram64 : public Reduction {
public:
    /**
     * @brief Creates an empty histogram.
     * @param binCount Number of bins; at least 1.
     * @param minValue Lower bound, inclusive; finite.
     * @param maxValue Upper bound, inclusive; finite and greater than @p minValue.
     * @throws InvalidArgumentError When an argument is out of range.
     * @throws std::bad_alloc When memory is exhausted.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-PIX-5
     */
    Histogram64(int32_t binCount, double minValue, double maxValue);
    /**
     * @brief Copies a histogram, including its bin values.
     * @param other The histogram to copy; not being accumulated into.
     * @throws std::bad_alloc When memory is exhausted.
     */
    Histogram64(const Histogram64& other);
    /**
     * @brief Moves a histogram; the source is left destroyable and assignable only.
     * @param other The histogram to move from.
     * @throws Nothing.
     */
    Histogram64(Histogram64&& other) noexcept;
    /**
     * @brief Copy-assigns a histogram, including its bin values.
     * @param other The histogram to copy.
     * @return This object.
     * @throws std::bad_alloc When memory is exhausted; this object is unchanged.
     */
    Histogram64& operator=(const Histogram64& other);
    /**
     * @brief Move-assigns a histogram; the source is left destroyable and assignable only.
     * @param other The histogram to move from.
     * @return This object.
     * @throws Nothing.
     */
    Histogram64& operator=(Histogram64&& other) noexcept;
    /** @brief Destroys the histogram. */
    ~Histogram64() override;

    /**
     * @brief Bins every sample of every plane of @p block.
     * @param block The tile's pixels.
     * @param tileIndex Unused.
     * @throws InvalidArgumentError When @p block is null or has a plane of an
     * unsupported type.
     * @throws std::bad_alloc When memory is exhausted.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-PIX-5, UR-IMG-2
     */
    void accumulate(const BlockView& block, int64_t tileIndex) override;
    /**
     * @brief Adds the bins of another histogram into this one.
     * @param other A Histogram64 with the same binCount, minValue and maxValue.
     * @throws InvalidArgumentError When @p other is not a Histogram64 of the same shape.
     * @par Thread-safety
     * Requires exclusive access to this object.
     * @par Requirements
     * UR-PIX-5
     */
    void combine(const Reduction& other) override;
    /**
     * @brief Tells whether combination order cannot change the result.
     * @return Always true: the bins are integers.
     * @throws Nothing.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-QA-3
     */
    bool isOrderIndependent() const override;

    /**
     * @brief Returns the count in one bin.
     * @param index Bin index in [0, binCount).
     * @return The count.
     * @throws InvalidArgumentError When @p index is out of range.
     * @par Thread-safety
     * Safe once accumulation has finished.
     * @par Requirements
     * UR-PIX-5, UR-IMG-2
     */
    int64_t bin(int32_t index) const;
    /**
     * @brief Returns the sum of all bins: the number of samples counted.
     * @return The total; 64-bit (UR-IMG-2).
     * @throws InvalidArgumentError On a moved-from histogram.
     * @par Thread-safety
     * Safe once accumulation has finished.
     * @par Requirements
     * UR-IMG-2
     */
    int64_t totalCount() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace slideio::processor
