// Requirements: UR-IMG-10, UR-IMG-11
#pragma once

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

/// The exact value of an IEEE 754 binary16 bit pattern: subnormals, signed
/// zeros, infinities and NaN included.
double decodeBinary16(uint16_t bits) noexcept;

/// The DataType whose dataTypeName() is @p name. Throws InvalidArgumentError,
/// without echoing the text, when @p name is null or no type's name.
DataType dataTypeFromName(const char* name);

} // namespace slideio::processor::detail
