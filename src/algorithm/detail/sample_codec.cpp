// Requirements: UR-IMG-10, UR-IMG-11
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

Sample signedSample(int64_t v) { return Sample{Sample::Kind::Signed, v, 0, 0.0}; }
Sample floatSample(double v) { return Sample{Sample::Kind::Float, 0, 0, v}; }

} // namespace

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
