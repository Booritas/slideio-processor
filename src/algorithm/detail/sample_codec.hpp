// Requirements: UR-IMG-10, UR-IMG-11, UR-IMG-13
#pragma once

#include <slideio/processor/algorithm/pixel/convert.hpp>
#include <slideio/processor/core/data_type.hpp>

#include <cstdint>

namespace slideio::processor::detail {

/// One sample of any DataType, held without loss: integer types other than
/// UInt64 as Signed in `s`, UInt64 as Unsigned in `u`, float types (Float16
/// decoded exactly) as Float in `f`. The members not selected by `kind` are 0.
struct Sample {
    enum class Kind : int32_t { Signed, Unsigned, Float } kind;
    int64_t s;
    uint64_t u;
    double f;
};

/// The sample at column @p x of a row of @p type samples. Reads by memcpy, so
/// @p row need not be aligned. Throws UnsupportedDataTypeError for a type that
/// is not one of the eleven sample types.
Sample readSample(const void* row, int64_t x, DataType type);

/// The value of @p sample as a double: exact for every float sample (Float16
/// included) and for integers up to 2^53 in magnitude, which covers every
/// sample of every type but Int64 and UInt64; beyond that, the nearest double,
/// ties to even. Never throws.
double sampleAsDouble(const Sample& sample) noexcept;

/// Writes @p sample as a @p type sample at column @p x of a row, by memcpy, so
/// @p row need not be aligned, under the rules of pixel.convert_type: integer
/// to integer exact; to a float type the nearest value, ties to even; float to
/// integer rounded by @p rounding (Nearest is ties to even). A value the type
/// cannot hold sets @p overflowed (which is otherwise left alone) and is
/// written saturated: an integer type's minimum or maximum, a float type's
/// largest finite value with the sign; NaN to an integer type writes 0. NaN
/// and infinity to a float type are kept and are no overflow. The written
/// value does not depend on @p overflow: under OverflowMode::Error the caller
/// fails instead of keeping it. Never throws; a type that is not one of the
/// eleven sample types writes nothing, so the caller checks the type first.
void writeSample(void* row, int64_t x, DataType type, const Sample& sample, RoundingMode rounding,
                 OverflowMode overflow, bool& overflowed) noexcept;

/// The exact value of an IEEE 754 binary16 bit pattern: subnormals, signed
/// zeros, infinities and NaN included.
double decodeBinary16(uint16_t bits) noexcept;

/// The binary16 bit pattern nearest @p value, ties to even, subnormals
/// included. A magnitude of 65520 or more gives infinity with the sign; NaN
/// gives a quiet NaN with the sign.
uint16_t encodeBinary16(double value) noexcept;

/// The DataType whose dataTypeName() is @p name. Throws InvalidArgumentError,
/// without echoing the text, when @p name is null or no type's name.
DataType dataTypeFromName(const char* name);

} // namespace slideio::processor::detail
