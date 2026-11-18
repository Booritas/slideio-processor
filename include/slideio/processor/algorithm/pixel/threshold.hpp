// Requirements: UR-PIX-5, UR-IMG-11, UR-ALG-6
#pragma once

/**
 * @file threshold.hpp
 * @brief Thresholding operations: ThresholdDirection and the fixed threshold.
 */

#include <slideio/processor/pipeline/export.hpp>
#include <slideio/processor/pipeline/operation.hpp>

#include <cstdint>

namespace slideio::processor {

/**
 * @brief Which side of the threshold is foreground.
 *
 * Stored in an operation's parameters as its enumerator spelling, `"Above"` or
 * `"Below"`.
 * @par Requirements
 * UR-PIX-5
 */
enum class ThresholdDirection : int32_t {
    Above, ///< A sample strictly greater than the threshold is foreground.
    Below  ///< A sample strictly less than the threshold is foreground.
};

/**
 * @brief Creates `pixel.threshold_fixed`: PerChannel, halo none, every input
 * type, output Byte.
 *
 * Each output sample is 255 (foreground) when the input sample lies strictly
 * on the @p direction side of @p value, else 0 (background). A NaN sample is
 * always background. Integer samples are compared exactly, never through a
 * double, so an `Int64` sample of 2^53 + 1 is above a threshold of 2^53.
 *
 * Its parameters are `value` (double; 128.0 when created by name without it)
 * and `direction` (`"Above"` or `"Below"`; `"Above"` by default), and
 * Operation::parameters() always holds both. Creating it by name through
 * OperationCatalog::create with the same values gives equal parameters. By
 * name, an unknown key or a value of the wrong type throws
 * InvalidArgumentError naming the key.
 * @param value The threshold; must be finite.
 * @param direction Which side is foreground.
 * @return The operation, never null.
 * @throws InvalidArgumentError When @p value is not finite or @p direction is
 * not an enumerator; the message names the operation and the parameter, never
 * the value.
 * @throws std::bad_alloc When memory is exhausted.
 * @par Thread-safety
 * Safe to call concurrently. The operation's apply() may run concurrently on
 * different contexts.
 * @par Requirements
 * UR-PIX-5, UR-IMG-11, UR-ALG-6
 */
SLIDEIO_PROCESSOR_ENGINE_EXPORTS OperationPtr makeFixedThreshold(double value, ThresholdDirection direction);

} // namespace slideio::processor
