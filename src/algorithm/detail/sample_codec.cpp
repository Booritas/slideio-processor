// Requirements: UR-IMG-10, UR-IMG-11, UR-IMG-13
#include "detail/sample_codec.hpp"

#include <slideio/processor/core/error.hpp>

#include <cmath>
#include <cstring>
#include <limits>

namespace slideio::processor::detail {

namespace {

template <typename T>
T load(const void* row, int64_t x) {
    T value;
    std::memcpy(&value, static_cast<const unsigned char*>(row) + x * static_cast<int64_t>(sizeof(T)), sizeof(T));
    return value;
}

template <typename T>
void store(void* row, int64_t x, T value) noexcept {
    std::memcpy(static_cast<unsigned char*>(row) + x * static_cast<int64_t>(sizeof(T)), &value, sizeof(T));
}

Sample signedSample(int64_t v) { return Sample{Sample::Kind::Signed, v, 0, 0.0}; }
Sample floatSample(double v) { return Sample{Sample::Kind::Float, 0, 0, v}; }

// The integer nearest @p v, ties to even, independent of the floating-point
// environment. v - floor(v) is exact for every double.
double roundHalfEven(double v) noexcept {
    const double r = std::floor(v);
    const double fraction = v - r;
    if (fraction > 0.5 || (fraction == 0.5 && std::fmod(r, 2.0) != 0.0)) {
        return r + 1.0;
    }
    return r;
}

double roundBy(double v, RoundingMode rounding) noexcept {
    switch (rounding) {
    case RoundingMode::TowardZero: return std::trunc(v);
    case RoundingMode::Floor: return std::floor(v);
    case RoundingMode::Ceil: return std::ceil(v);
    case RoundingMode::Nearest:
    default: return roundHalfEven(v);
    }
}

// @p magnitude rounded to @p bits significant bits, ties to even, in integer
// arithmetic, as a double. The result is exact in any float type whose
// significand has at least @p bits bits (and whose range reaches 2^64), so
// converting it on involves no further rounding. The standard leaves the choice
// between the two neighbours of an integer-to-float conversion to the
// implementation, so the rounding is done here rather than by a cast.
double roundToSignificand(uint64_t magnitude, int bits) noexcept {
    int length = 0;
    for (uint64_t v = magnitude; v != 0; v >>= 1) {
        ++length;
    }
    if (length <= bits) {
        return static_cast<double>(magnitude); // fits: exact
    }
    const int shift = length - bits;
    uint64_t kept = magnitude >> shift;
    const uint64_t rest = magnitude & ((uint64_t{1} << shift) - 1);
    const uint64_t half = uint64_t{1} << (shift - 1);
    if (rest > half || (rest == half && (kept & 1u) != 0)) {
        ++kept; // at most 2^bits: still exact
    }
    return std::ldexp(static_cast<double>(kept), shift);
}

// An integer sample rounded to @p bits significant bits, ties to even, with its sign.
double integerAsFloat(const Sample& sample, int bits) noexcept {
    if (sample.kind == Sample::Kind::Unsigned) {
        return roundToSignificand(sample.u, bits);
    }
    // 0 - u is the magnitude of a negative value, INT64_MIN included.
    const bool negative = sample.s < 0;
    const uint64_t magnitude = negative ? uint64_t{0} - static_cast<uint64_t>(sample.s) : static_cast<uint64_t>(sample.s);
    const double rounded = roundToSignificand(magnitude, bits);
    return negative ? -rounded : rounded;
}

// The value of a sample as a double: exact for floats and for integers up to
// 2^53 in magnitude, the nearest double (ties to even) beyond.
double asDouble(const Sample& sample) noexcept {
    return sample.kind == Sample::Kind::Float ? sample.f
                                              : integerAsFloat(sample, std::numeric_limits<double>::digits);
}

template <typename T>
void writeInteger(void* row, int64_t x, const Sample& sample, RoundingMode rounding, bool& overflowed) noexcept {
    constexpr T lowest = std::numeric_limits<T>::lowest();
    constexpr T highest = std::numeric_limits<T>::max();
    constexpr auto lo = static_cast<int64_t>(lowest);
    constexpr auto hi = static_cast<uint64_t>(highest);
    T out = 0;
    switch (sample.kind) {
    case Sample::Kind::Signed:
        if (sample.s < lo) {
            out = lowest;
            overflowed = true;
        } else if (sample.s >= 0 && static_cast<uint64_t>(sample.s) > hi) {
            out = highest;
            overflowed = true;
        } else {
            out = static_cast<T>(sample.s);
        }
        break;
    case Sample::Kind::Unsigned:
        if (sample.u > hi) {
            out = highest;
            overflowed = true;
        } else {
            out = static_cast<T>(sample.u);
        }
        break;
    case Sample::Kind::Float: {
        if (std::isnan(sample.f)) {
            overflowed = true;
            break;
        }
        // Both bounds are powers of two (or 0), so exact as doubles; ±infinity
        // falls outside them.
        const double r = roundBy(sample.f, rounding);
        const double hiPlusOne = std::ldexp(1.0, std::numeric_limits<T>::digits);
        if (r < static_cast<double>(lo)) {
            out = lowest;
            overflowed = true;
        } else if (r >= hiPlusOne) {
            out = highest;
            overflowed = true;
        } else {
            out = static_cast<T>(r);
        }
        break;
    }
    }
    store(row, x, out);
}

// The smallest magnitude a double rounds (ties to even) to infinity as a float:
// the midpoint between FLT_MAX and 2^128.
constexpr double kFloat32Overflow = 0x1.ffffffp+127;

void writeFloat32(void* row, int64_t x, const Sample& sample, bool& overflowed) noexcept {
    float out;
    switch (sample.kind) {
    case Sample::Kind::Signed:
    case Sample::Kind::Unsigned:
        // Rounded once, to 24 bits, so the cast to float is exact.
        out = static_cast<float>(integerAsFloat(sample, std::numeric_limits<float>::digits));
        break;
    case Sample::Kind::Float:
    default:
        if (std::isfinite(sample.f) && std::fabs(sample.f) >= kFloat32Overflow) {
            out = std::signbit(sample.f) ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max();
            overflowed = true;
        } else {
            out = static_cast<float>(sample.f);
        }
        break;
    }
    store(row, x, out);
}

void writeFloat16(void* row, int64_t x, const Sample& sample, bool& overflowed) noexcept {
    // An integer beyond 2^53 is rounded once on the way to double, but every
    // such value overflows binary16 whatever its rounding.
    const double value = asDouble(sample);
    uint16_t bits = encodeBinary16(value);
    if (std::isfinite(value) && (bits & 0x7FFFu) == 0x7C00u) {
        bits = static_cast<uint16_t>((bits & 0x8000u) | 0x7BFFu);
        overflowed = true;
    }
    store(row, x, bits);
}

} // namespace

uint16_t encodeBinary16(double value) noexcept {
    const auto sign = static_cast<uint16_t>(std::signbit(value) ? 0x8000u : 0u);
    if (std::isnan(value)) {
        return static_cast<uint16_t>(sign | 0x7E00u);
    }
    const double magnitude = std::fabs(value);
    if (magnitude >= 65520.0) {
        return static_cast<uint16_t>(sign | 0x7C00u);
    }
    if (magnitude < 0x1p-14) {
        // Subnormal: units of 2^-24. A result of 1024 is the smallest normal's pattern.
        return static_cast<uint16_t>(sign | static_cast<uint16_t>(roundHalfEven(magnitude * 0x1p24)));
    }
    int exponent2 = 0;
    std::frexp(magnitude, &exponent2);
    int exponent = exponent2 - 1; // magnitude in [2^exponent, 2^(exponent+1))
    double mantissa = roundHalfEven(std::ldexp(magnitude, 10 - exponent)); // in [1024, 2048]
    if (mantissa == 2048.0) {
        mantissa = 1024.0;
        ++exponent;
    }
    return static_cast<uint16_t>(sign | static_cast<uint16_t>((exponent + 15) << 10) |
                                 static_cast<uint16_t>(static_cast<int>(mantissa) - 1024));
}

void writeSample(void* row, int64_t x, DataType type, const Sample& sample, RoundingMode rounding,
                 OverflowMode /*overflow*/, bool& overflowed) noexcept {
    switch (type) {
    case DataType::Byte: writeInteger<uint8_t>(row, x, sample, rounding, overflowed); return;
    case DataType::Int8: writeInteger<int8_t>(row, x, sample, rounding, overflowed); return;
    case DataType::UInt16: writeInteger<uint16_t>(row, x, sample, rounding, overflowed); return;
    case DataType::Int16: writeInteger<int16_t>(row, x, sample, rounding, overflowed); return;
    case DataType::UInt32: writeInteger<uint32_t>(row, x, sample, rounding, overflowed); return;
    case DataType::Int32: writeInteger<int32_t>(row, x, sample, rounding, overflowed); return;
    case DataType::UInt64: writeInteger<uint64_t>(row, x, sample, rounding, overflowed); return;
    case DataType::Int64: writeInteger<int64_t>(row, x, sample, rounding, overflowed); return;
    case DataType::Float16: writeFloat16(row, x, sample, overflowed); return;
    case DataType::Float32: writeFloat32(row, x, sample, overflowed); return;
    case DataType::Float64: store(row, x, asDouble(sample)); return;
    default: return;
    }
}

double decodeBinary16(uint16_t bits) noexcept {
    const bool negative = (bits & 0x8000u) != 0;
    const int exponent = (bits >> 10) & 0x1F;
    const int mantissa = bits & 0x3FF;
    double magnitude;
    if (exponent == 0) {
        magnitude = std::ldexp(static_cast<double>(mantissa), -24);
    } else if (exponent == 0x1F) {
        magnitude = mantissa == 0 ? std::numeric_limits<double>::infinity() : std::numeric_limits<double>::quiet_NaN();
    } else {
        magnitude = std::ldexp(static_cast<double>(mantissa | 0x400), exponent - 25);
    }
    return negative ? -magnitude : magnitude;
}

Sample readSample(const void* row, int64_t x, DataType type) {
    switch (type) {
    case DataType::Byte: return signedSample(load<uint8_t>(row, x));
    case DataType::Int8: return signedSample(load<int8_t>(row, x));
    case DataType::UInt16: return signedSample(load<uint16_t>(row, x));
    case DataType::Int16: return signedSample(load<int16_t>(row, x));
    case DataType::UInt32: return signedSample(load<uint32_t>(row, x));
    case DataType::Int32: return signedSample(load<int32_t>(row, x));
    case DataType::UInt64: return Sample{Sample::Kind::Unsigned, 0, load<uint64_t>(row, x), 0.0};
    case DataType::Int64: return signedSample(load<int64_t>(row, x));
    case DataType::Float16: return floatSample(decodeBinary16(load<uint16_t>(row, x)));
    case DataType::Float32: return floatSample(load<float>(row, x));
    case DataType::Float64: return floatSample(load<double>(row, x));
    default: break;
    }
    throw UnsupportedDataTypeError("readSample: not a sample type", type, DataTypeSet::all());
}

DataType dataTypeFromName(const char* name) {
    if (name != nullptr) {
        for (int32_t i = 0; i < static_cast<int32_t>(DataType::Unknown); ++i) {
            const auto type = static_cast<DataType>(i);
            if (std::strcmp(name, dataTypeName(type)) == 0) {
                return type;
            }
        }
    }
    throw InvalidArgumentError("dataTypeFromName: not the name of a data type");
}

} // namespace slideio::processor::detail
