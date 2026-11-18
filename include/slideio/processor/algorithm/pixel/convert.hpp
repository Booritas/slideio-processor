// Requirements: UR-IMG-13, UR-IMG-10, UR-IMG-11
#pragma once

/**
 * @file convert.hpp
 * @brief Explicit sample type conversion: RoundingMode, OverflowMode and
 * `pixel.convert_type`.
 */

#include <slideio/processor/core/data_type.hpp>
#include <slideio/processor/pipeline/export.hpp>
#include <slideio/processor/pipeline/operation.hpp>

#include <cstdint>

namespace slideio::processor {

/**
 * @brief How a floating-point sample is rounded to an integer type.
 *
 * Affects only conversions to integer types. Stored in an operation's
 * parameters as its enumerator spelling, e.g. `"TowardZero"`.
 * @par Requirements
 * UR-IMG-13
 */
enum class RoundingMode : int32_t {
    Nearest,    ///< To the nearest integer, ties to even.
    TowardZero, ///< Truncation.
    Floor,      ///< Toward negative infinity.
    Ceil        ///< Toward positive infinity.
};

/**
 * @brief What happens to a sample the target type cannot hold.
 *
 * Stored in an operation's parameters as its enumerator spelling, `"Saturate"`
 * or `"Error"`.
 * @par Requirements
 * UR-IMG-13, UR-IMG-11
 */
enum class OverflowMode : int32_t {
    Saturate, ///< Clamp to the nearest value the target holds (NaN to an integer type gives 0).
    Error     ///< Fail the tile.
};

/**
 * @brief Creates `pixel.convert_type`: PerChannel, halo none, every input type,
 * output @p target.
 *
 * The one operation that narrows, saturates or loses precision, and only as
 * the caller asks. Each sample is converted by these rules:
 * - integer to integer: exact when in range; out of range it overflows.
 * - integer to float: the nearest representable value, ties to even. A
 *   `Float16` result beyond ±65504 overflows.
 * - float to integer: rounded by the RoundingMode (`Nearest` is ties to
 *   even), then out of range it overflows. NaN and ±infinity overflow.
 * - float to a wider or equal float: exact.
 * - float to a narrower float: the nearest representable value, ties to even.
 *   A finite value whose rounded result would be infinite overflows. NaN and
 *   ±infinity are kept.
 * - a sample of the target type itself is copied bit for bit.
 *
 * An overflowing sample, under OverflowMode::Saturate, becomes the target's
 * minimum or maximum (the largest finite value, with the sign, for a float
 * target), and NaN converted to an integer type becomes 0. Under
 * OverflowMode::Error the tile fails with InvalidArgumentError
 * `"pixel.convert_type: <n> samples of <source type> do not fit <target type>"`,
 * counting every overflowing sample of the tile; it never names a value. When
 * planes of several types overflow, the source types are listed in plane
 * order, each once and separated by ", ", e.g.
 * `"pixel.convert_type: 4 samples of Int16, Int32 do not fit Int8"`.
 *
 * Its parameters are `target` (a dataTypeName() spelling; `"Float32"` when
 * created by name without it), `rounding` (`"Nearest"` by default) and
 * `overflow` (`"Saturate"` by default), and Operation::parameters() always
 * holds all three. Creating it by name through OperationCatalog::create with
 * the same values gives equal parameters. By name, an unknown key, a value of
 * the wrong type or an unknown spelling throws InvalidArgumentError naming the
 * key.
 *
 * The RoundingMode argument says how a float sample is rounded to an integer
 * target, and the OverflowMode argument what happens to a sample the target
 * cannot hold.
 * @param target The output type; one of the eleven sample types.
 * @return The operation, never null.
 * @throws InvalidArgumentError When @p target is DataType::Unknown or out of
 * range, or the RoundingMode or the OverflowMode is not an enumerator; the
 * message names the operation and the parameter, never the value.
 * @throws std::bad_alloc When memory is exhausted.
 * @par Thread-safety
 * Safe to call concurrently. The operation's apply() may run concurrently on
 * different contexts.
 * @par Requirements
 * UR-IMG-13, UR-IMG-10, UR-IMG-11
 */
SLIDEIO_PROCESSOR_ENGINE_EXPORTS OperationPtr makeTypeConvert(DataType target, RoundingMode, OverflowMode);

} // namespace slideio::processor
