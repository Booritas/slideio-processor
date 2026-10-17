#include <slideio/processor/core/data_type.hpp>

#include <slideio/processor/core/error.hpp>

namespace slideio::processor {

namespace {

constexpr int32_t kValidCount = static_cast<int32_t>(DataType::Unknown); // 11
constexpr uint32_t kAllBits = (1u << kValidCount) - 1u;
constexpr int32_t kSlideioUnknown = 1024; // slideio::DataType::DT_Unknown

// Maps any out-of-range value to the Unknown slot.
constexpr int32_t slot(DataType t) {
    const int32_t i = static_cast<int32_t>(t);
    return (i >= 0 && i < kValidCount) ? i : kValidCount;
}

constexpr uint32_t bit(DataType t) { return 1u << static_cast<int32_t>(t); }

const char* const kNames[] = {"Byte", "Int8", "UInt16", "Int16", "UInt32", "Int32",
                              "UInt64", "Int64", "Float16", "Float32", "Float64", "Unknown"};
const int32_t kSizes[] = {1, 1, 2, 2, 4, 4, 8, 8, 2, 4, 8, 0};
// slideio::DataType value for each DataType, in enumerator order.
const int32_t kToSlideio[] = {0, 1, 2, 3, 8, 4, 10, 9, 7, 5, 6, kSlideioUnknown};

constexpr uint32_t kFloatBits = bit(DataType::Float16) | bit(DataType::Float32) | bit(DataType::Float64);
constexpr uint32_t kUnsignedBits = bit(DataType::Byte) | bit(DataType::UInt16) | bit(DataType::UInt32) |
                                   bit(DataType::UInt64);

} // namespace

const char* dataTypeName(DataType type) noexcept { return kNames[slot(type)]; }

int32_t dataTypeSizeBytes(DataType type) noexcept { return kSizes[slot(type)]; }

bool dataTypeIsFloat(DataType type) noexcept {
    return slot(type) < kValidCount && (bit(type) & kFloatBits) != 0;
}

bool dataTypeIsSigned(DataType type) noexcept {
    return slot(type) < kValidCount && (bit(type) & kUnsignedBits) == 0;
}

DataType dataTypeFromSlideio(int32_t slideioDataType) noexcept {
    for (int32_t i = 0; i < kValidCount; ++i) {
        if (kToSlideio[i] == slideioDataType) {
            return static_cast<DataType>(i);
        }
    }
    return DataType::Unknown;
}

int32_t dataTypeToSlideio(DataType type) noexcept { return kToSlideio[slot(type)]; }

DataTypeSet::DataTypeSet() noexcept : bits_(0) {}

DataTypeSet DataTypeSet::all() noexcept {
    DataTypeSet s;
    s.bits_ = kAllBits;
    return s;
}

DataTypeSet DataTypeSet::integerTypes() noexcept {
    DataTypeSet s;
    s.bits_ = kAllBits & ~kFloatBits;
    return s;
}

DataTypeSet DataTypeSet::floatTypes() noexcept {
    DataTypeSet s;
    s.bits_ = kFloatBits;
    return s;
}

DataTypeSet& DataTypeSet::add(DataType type) noexcept {
    if (slot(type) < kValidCount) {
        bits_ |= bit(type);
    }
    return *this;
}

DataTypeSet& DataTypeSet::remove(DataType type) noexcept {
    if (slot(type) < kValidCount) {
        bits_ &= ~bit(type);
    }
    return *this;
}

bool DataTypeSet::contains(DataType type) const noexcept {
    return slot(type) < kValidCount && (bits_ & bit(type)) != 0;
}

bool DataTypeSet::isEmpty() const noexcept { return bits_ == 0; }

int32_t DataTypeSet::count() const noexcept {
    int32_t n = 0;
    for (uint32_t b = bits_; b != 0; b &= b - 1) {
        ++n;
    }
    return n;
}

DataType DataTypeSet::at(int32_t index) const {
    if (index >= 0) {
        int32_t seen = 0;
        for (int32_t i = 0; i < kValidCount; ++i) {
            if ((bits_ & (1u << i)) != 0) {
                if (seen == index) {
                    return static_cast<DataType>(i);
                }
                ++seen;
            }
        }
    }
    throw InvalidArgumentError("DataTypeSet index out of range");
}

uint32_t DataTypeSet::bits() const noexcept { return bits_; }

DataTypeSet DataTypeSet::intersect(DataTypeSet other) const noexcept {
    DataTypeSet s;
    s.bits_ = bits_ & other.bits_;
    return s;
}

bool DataTypeSet::operator==(DataTypeSet other) const noexcept { return bits_ == other.bits_; }

} // namespace slideio::processor
