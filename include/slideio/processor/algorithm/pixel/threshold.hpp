// Requirements: UR-PIX-5, UR-IMG-11, UR-ALG-6, UR-QA-2, UR-QA-3, UR-EXE-1
#pragma once

/**
 * @file threshold.hpp
 * @brief Thresholding operations: ThresholdDirection, AdaptiveMethod, the
 * fixed threshold, the streamed Otsu threshold and the adaptive threshold.
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
 * @brief How the adaptive threshold averages a pixel's window.
 *
 * Stored in an operation's parameters as its enumerator spelling, `"Mean"` or
 * `"Gaussian"`.
 * @par Requirements
 * UR-PIX-5
 */
enum class AdaptiveMethod : int32_t {
    Mean,    ///< Every pixel of the window has equal weight.
    Gaussian ///< Separable Gaussian weights centred on the pixel.
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

/**
 * @brief Creates `pixel.threshold_adaptive`: PerChannel, halo blockSize / 2 on
 * every side, every input type except `Int64` and `UInt64`, output Byte.
 *
 * Each pixel's threshold is T = W - @p offset, where W is the average of the
 * blockSize x blockSize window centred on it. `Mean` gives every window pixel
 * equal weight: the window sum divided by blockSize^2. `Gaussian` uses the
 * separable weights exp(-d^2 / (2 sigma^2)) for d from -blockSize / 2 to
 * blockSize / 2, with sigma = 0.3 * ((blockSize - 1) / 2 - 1) + 0.8,
 * normalised to sum 1. The output sample is 255 (foreground) when the input
 * sample is strictly greater than T (`Above`) or strictly less (`Below`), else
 * 0 (background). A NaN sample, or a NaN anywhere in the window, makes the
 * pixel background. Window pixels beyond the level read as zero.
 *
 * Every pixel is computed from its own window alone, in double and in one fixed
 * order: for each window row top to bottom, its weighted sum left to right,
 * then those row sums weighted top to bottom. No running sum is carried from a
 * neighbouring pixel, so the result does not depend on the tiling. The 64-bit
 * integer types are refused because their window sums are not exact in double;
 * every other type's samples are, so integer comparisons are exact.
 *
 * Its parameters are `method` (`"Mean"` or `"Gaussian"`; `"Mean"` by default),
 * `blockSize` (an odd integer from 3 to 1025; 11 by default), `offset` (a
 * finite double; 2.0 by default) and `direction` (`"Above"` or `"Below"`;
 * `"Above"` by default), and Operation::parameters() always holds all four.
 * Creating it by name through OperationCatalog::create with the same values
 * gives equal parameters. By name, an unknown key, a value of the wrong type or
 * a value out of range throws InvalidArgumentError naming the key, never the
 * value.
 * @param method How the window is averaged.
 * @param blockSize The window's width and height; odd, from 3 to 1025.
 * @param offset Subtracted from the window average; must be finite.
 * @param direction Which side of the threshold is foreground.
 * @return The operation, never null.
 * @throws InvalidArgumentError When @p method or @p direction is not an
 * enumerator, @p blockSize is even or outside 3 to 1025, or @p offset is not
 * finite; the message names the operation and the parameter, never the value.
 * @throws std::bad_alloc When memory is exhausted.
 * @par Thread-safety
 * Safe to call concurrently. The operation's apply() may run concurrently on
 * different contexts.
 * @par Requirements
 * UR-PIX-5, UR-QA-2, UR-ALG-6
 */
SLIDEIO_PROCESSOR_ENGINE_EXPORTS OperationPtr makeAdaptiveThreshold(AdaptiveMethod method, int32_t blockSize,
                                                                    double offset, ThresholdDirection direction);

} // namespace slideio::processor
