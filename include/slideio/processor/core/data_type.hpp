// Requirements: UR-IMG-8, UR-IMG-11, UR-IMG-12
#pragma once

#include <slideio/processor/core/export.hpp>

#include <cstdint>

namespace slideio::processor {

/**
 * @brief Pixel sample type. Enumerator order is fixed: DataTypeSet bit i means enumerator i.
 * @par Requirements
 * UR-IMG-8, UR-IMG-12
 */
enum class DataType : int32_t {
    Byte, ///< Unsigned 8-bit integer.
    Int8, ///< Signed 8-bit integer.
    UInt16, ///< Unsigned 16-bit integer.
    Int16, ///< Signed 16-bit integer.
    UInt32, ///< Unsigned 32-bit integer.
    Int32, ///< Signed 32-bit integer.
    UInt64, ///< Unsigned 64-bit integer.
    Int64, ///< Signed 64-bit integer.
    Float16, ///< 16-bit floating point.
    Float32, ///< 32-bit floating point.
    Float64, ///< 64-bit floating point.
    Unknown ///< Not a valid type. Also stands for any out-of-range value.
};

/**
 * @brief Returns the enumerator's name.
 * @param type The type.
 * @return The enumerator spelled exactly (for example "UInt16"), or "Unknown"
 * for Unknown and for any out-of-range value. Never null; static storage.
 * @throws Nothing.
 * @par Thread-safety
 * Safe from any thread.
 * @par Requirements
 * UR-IMG-11
 */
SLIDEIO_PROCESSOR_DATA_EXPORTS const char* dataTypeName(DataType type) noexcept;

/**
 * @brief Returns the size of one sample.
 * @param type The type.
 * @return Size in bytes, 0 for Unknown and for any out-of-range value.
 * @throws Nothing.
 * @par Thread-safety
 * Safe from any thread.
 * @par Requirements
 * UR-IMG-8
 */
SLIDEIO_PROCESSOR_DATA_EXPORTS int32_t dataTypeSizeBytes(DataType type) noexcept;

/**
 * @brief Tells whether a type is floating point.
 * @param type The type.
 * @return True for Float16, Float32 and Float64.
 * @throws Nothing.
 * @par Thread-safety
 * Safe from any thread.
 * @par Requirements
 * UR-IMG-8
 */
SLIDEIO_PROCESSOR_DATA_EXPORTS bool dataTypeIsFloat(DataType type) noexcept;

/**
 * @brief Tells whether a type can hold negative values.
 * @param type The type.
 * @return True for the signed integers and all floating-point types; false
 * for the unsigned integers, Unknown and out-of-range values.
 * @throws Nothing.
 * @par Thread-safety
 * Safe from any thread.
 * @par Requirements
 * UR-IMG-8
 */
SLIDEIO_PROCESSOR_DATA_EXPORTS bool dataTypeIsSigned(DataType type) noexcept;

/**
 * @brief Converts a SlideIO data type value to a DataType.
 * @param slideioDataType The integer value of slideio::DataType.
 * @return The matching type, Unknown for any value with no match.
 * @throws Nothing.
 * @par Thread-safety
 * Safe from any thread.
 * @par Requirements
 * UR-IMG-12
 */
SLIDEIO_PROCESSOR_DATA_EXPORTS DataType dataTypeFromSlideio(int32_t slideioDataType) noexcept;

/**
 * @brief Converts a DataType to the SlideIO data type value.
 * @param type The type.
 * @return The integer value of slideio::DataType; 1024 (DT_Unknown) for
 * Unknown and out-of-range values.
 * @throws Nothing.
 * @par Thread-safety
 * Safe from any thread.
 * @par Requirements
 * UR-IMG-12
 */
SLIDEIO_PROCESSOR_DATA_EXPORTS int32_t dataTypeToSlideio(DataType type) noexcept;

/**
 * @brief A set of valid data types, held as one bit mask.
 *
 * Bit i is set when the enumerator with value i is a member. Unknown is
 * never a member.
 * @par Thread-safety
 * A value type: distinct instances may be used concurrently; a const
 * instance may be read from many threads.
 * @par Requirements
 * UR-IMG-8, UR-IMG-11, UR-IMG-12
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS DataTypeSet {
public:
    /**
     * @brief Builds an empty set.
     * @throws Nothing.
     */
    DataTypeSet() noexcept;
    /**
     * @brief Returns the set of all eleven valid types.
     * @return The set.
     * @throws Nothing.
     */
    static DataTypeSet all() noexcept;
    /**
     * @brief Returns the set of the eight integer types.
     * @return The set.
     * @throws Nothing.
     */
    static DataTypeSet integerTypes() noexcept;
    /**
     * @brief Returns the set of the three floating-point types.
     * @return The set.
     * @throws Nothing.
     */
    static DataTypeSet floatTypes() noexcept;

    /**
     * @brief Adds a type. Unknown and out-of-range values are ignored.
     * @param type The type to add.
     * @return This set.
     * @throws Nothing.
     */
    DataTypeSet& add(DataType type) noexcept;
    /**
     * @brief Removes a type. Unknown and out-of-range values are ignored.
     * @param type The type to remove.
     * @return This set.
     * @throws Nothing.
     */
    DataTypeSet& remove(DataType type) noexcept;
    /**
     * @brief Tells whether a type is a member.
     * @param type The type to look for.
     * @return True when it is a member; false for Unknown and out-of-range values.
     * @throws Nothing.
     */
    bool contains(DataType type) const noexcept;
    /**
     * @brief Tells whether the set has no members.
     * @return True when empty.
     * @throws Nothing.
     */
    bool isEmpty() const noexcept;
    /**
     * @brief Returns the number of members.
     * @return The count, 0 to 11.
     * @throws Nothing.
     */
    int32_t count() const noexcept;
    /**
     * @brief Returns a member by position, in ascending enumerator order.
     * @param index Position, from 0 to count() - 1.
     * @return The member.
     * @throws InvalidArgumentError When index is out of range.
     */
    DataType at(int32_t index) const;
    /**
     * @brief Returns the bit mask. Bit i means enumerator i.
     * @return The mask.
     * @throws Nothing.
     */
    uint32_t bits() const noexcept;

    /**
     * @brief Returns the members common to this set and another.
     * @param other The other set.
     * @return The intersection.
     * @throws Nothing.
     */
    DataTypeSet intersect(DataTypeSet other) const noexcept;
    /**
     * @brief Compares two sets.
     * @param other The other set.
     * @return True when both have the same members.
     * @throws Nothing.
     */
    bool operator==(DataTypeSet other) const noexcept;

private:
    uint32_t bits_;
};

} // namespace slideio::processor
