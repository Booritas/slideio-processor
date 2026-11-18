// Requirements: UR-PIX-5, UR-IMG-11, UR-ALG-6, UR-QA-3, UR-EXE-1
#pragma once

/**
 * @file threshold.hpp
 * @brief Thresholding operations: ThresholdDirection, the fixed threshold and
 * the streamed Otsu threshold.
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

/**
 * @brief Creates `pixel.threshold_otsu`: PerChannel, halo none, global
 * context, every input type, output Byte.
 *
 * Runs in two passes over the slide or its restriction. The first accumulates
 * one 64-bit histogram per source channel from the samples of each tile rect
 * that the run's mask admits; tiles combine by adding their integer bins, so
 * the result does not depend on the worker count or the tile order. Once all
 * tiles are counted, each channel's threshold bin k* is the smallest k in
 * [0, binCount - 2] maximising w0 * w1 * (m0 - m1)^2 over splits whose two
 * classes are both non-empty, the class means taken over bin indices. When no
 * split has two non-empty classes, k* is the highest non-empty bin. The second
 * pass writes 255 (foreground) or 0 (background) per sample: `Above` makes a
 * bin greater than k* foreground, `Below` a bin less than or equal to it. A
 * sample below the histogram's range counts as bin -1 and one above it as bin
 * binCount; a NaN sample is background, and so is every sample of a channel
 * that counted nothing.
 *
 * Its parameters are `direction` (`"Above"` or `"Below"`; `"Above"` by
 * default) and, only when created by name with a histogram range, the doubles
 * `histogramMinimum` and `histogramMaximum` (finite, the minimum less than the
 * maximum, given together) and the integer `binCount` (2 to 1048576; 256 by
 * default). With a range, the bins divide [histogramMinimum, histogramMaximum]
 * evenly, as Histogram64 does, a sample equal to the maximum falling in the
 * last bin. Without one, `Byte`, `Int8`, `UInt16` and `Int16` get one bin per
 * value of the type, and binCount must not be given; any other type then fails
 * the run, reported as a TileError whose cause is an InvalidArgumentError
 * naming the operation, the type and the missing `histogramMinimum` and
 * `histogramMaximum`. This factory creates the operation without a range, so
 * Operation::parameters() holds `direction` alone; created by name with a
 * range, the bag holds all four keys. By name, an unknown key, a value of the
 * wrong type or a value out of range throws InvalidArgumentError naming the
 * key, never the value.
 * @param direction Which side of the threshold is foreground.
 * @return The operation, never null.
 * @throws InvalidArgumentError When @p direction is not an enumerator; the
 * message names the operation and the parameter.
 * @throws std::bad_alloc When memory is exhausted.
 * @par Thread-safety
 * Safe to call concurrently. Each instance accumulates from one thread at a
 * time; once finalised, its apply() may run concurrently on different
 * contexts.
 * @par Requirements
 * UR-PIX-5, UR-QA-3, UR-EXE-1, UR-ALG-6
 */
SLIDEIO_PROCESSOR_ENGINE_EXPORTS OperationPtr makeOtsuThreshold(ThresholdDirection direction);

} // namespace slideio::processor
