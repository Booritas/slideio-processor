// Requirements: UR-STG-4, UR-RES-11
#pragma once

/**
 * @file operation_parameters.hpp
 * @brief OperationParameters: the typed key-value bag an operation carries.
 */

#include <slideio/processor/pipeline/export.hpp>

#include <cstdint>
#include <memory>

namespace slideio::processor {

/**
 * @brief A typed key-value bag: the parameters an operation is built from and
 * the form a recorded stage replays from.
 *
 * Operations are resolved by name from the catalog, so their parameters must
 * round-trip through the stage configuration recorded in the store; toJson()
 * produces that record. A value is one of int64, double, bool, string or
 * array of double. Keys are UTF-8, non-null and non-empty, and are kept in
 * ascending bytewise order, so two bags with the same entries are equal and
 * serialise identically whatever order they were set in. Setting an existing
 * key replaces it, type included. Getters never coerce: getInt() on a
 * double-typed key reports false.
 *
 * In JSON an integer is written as an integer and a double always with a
 * fraction or exponent, so the type survives a round trip. Doubles must be
 * finite, because JSON cannot hold anything else and a recorded stage must
 * replay.
 * @par Thread-safety
 * ThreadCompatible.
 * @par Requirements
 * UR-STG-4, UR-RES-11
 */
class SLIDEIO_PROCESSOR_ENGINE_EXPORTS OperationParameters {
public:
    /**
     * @brief Creates an empty bag.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-STG-4
     */
    OperationParameters();
    /**
     * @brief Copies a bag; the copy shares nothing with the original.
     * @param other The bag to copy.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * Reads @p other; ThreadCompatible.
     * @par Requirements
     * UR-STG-4
     */
    OperationParameters(const OperationParameters& other);
    /**
     * @brief Moves a bag. The moved-from bag is empty and reusable.
     * @param other The bag to move from.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-STG-4
     */
    OperationParameters(OperationParameters&& other) noexcept;
    /**
     * @brief Replaces this bag's entries with a copy of another's.
     * @param other The bag to copy.
     * @return This bag.
     * @throws std::bad_alloc When allocation fails; this bag is then unchanged.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-STG-4
     */
    OperationParameters& operator=(const OperationParameters& other);
    /**
     * @brief Replaces this bag's entries by moving another's.
     * @param other The bag to move from; left empty and reusable.
     * @return This bag.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-STG-4
     */
    OperationParameters& operator=(OperationParameters&& other) noexcept;
    ~OperationParameters();

    /**
     * @brief Sets an integer value, replacing any value of the key.
     * @param key UTF-8 key, non-null and non-empty.
     * @param value The value.
     * @return This bag, for chaining.
     * @throws InvalidArgumentError When the key is null or empty; the message
     * names the setter.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-STG-4, UR-RES-11
     */
    OperationParameters& setInt(const char* key, int64_t value);
    /**
     * @brief Sets a floating-point value, replacing any value of the key.
     * @param key UTF-8 key, non-null and non-empty.
     * @param value The value; must be finite.
     * @return This bag, for chaining.
     * @throws InvalidArgumentError When the key is null or empty, or the
     * value is NaN or infinite; the message names the setter and the key.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-STG-4, UR-RES-11
     */
    OperationParameters& setDouble(const char* key, double value);
    /**
     * @brief Sets a boolean value, replacing any value of the key.
     * @param key UTF-8 key, non-null and non-empty.
     * @param value The value.
     * @return This bag, for chaining.
     * @throws InvalidArgumentError When the key is null or empty; the message
     * names the setter.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-STG-4, UR-RES-11
     */
    OperationParameters& setBool(const char* key, bool value);
    /**
     * @brief Sets a string value, replacing any value of the key.
     * @param key UTF-8 key, non-null and non-empty.
     * @param value UTF-8 text; null reads as empty.
     * @return This bag, for chaining.
     * @throws InvalidArgumentError When the key is null or empty; the message
     * names the setter.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-STG-4, UR-RES-11
     */
    OperationParameters& setString(const char* key, const char* value);
    /**
     * @brief Sets an array-of-double value, replacing any value of the key.
     * @param key UTF-8 key, non-null and non-empty.
     * @param values The elements, copied; may be null only when @p count is 0.
     * @param count The element count; 0 stores an empty array.
     * @return This bag, for chaining.
     * @throws InvalidArgumentError When the key is null or empty, @p count is
     * negative, @p values is null with a positive @p count, or an element is
     * NaN or infinite; the message names the setter and the key. The bag is
     * unchanged.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-STG-4, UR-RES-11
     */
    OperationParameters& setDoubleArray(const char* key, const double* values, int32_t count);

    /**
     * @brief Reads an integer value.
     * @param key The key; null reads as absent.
     * @param out Receives the value when the key holds an integer.
     * @return True when the key holds an integer; false when it is absent or
     * holds another type (no coercion). @p out is then untouched.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-STG-4
     */
    bool getInt(const char* key, int64_t& out) const noexcept;
    /**
     * @brief Reads a floating-point value.
     * @param key The key; null reads as absent.
     * @param out Receives the value when the key holds a double.
     * @return True when the key holds a double; false when it is absent or
     * holds another type (an integer is not coerced). @p out is then untouched.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-STG-4
     */
    bool getDouble(const char* key, double& out) const noexcept;
    /**
     * @brief Reads a boolean value.
     * @param key The key; null reads as absent.
     * @param out Receives the value when the key holds a boolean.
     * @return True when the key holds a boolean; false otherwise. @p out is
     * then untouched.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-STG-4
     */
    bool getBool(const char* key, bool& out) const noexcept;
    /**
     * @brief Reads a string value.
     * @param key The key; null reads as absent.
     * @param fallback Returned when the key is absent or not a string; may be
     * null.
     * @return The stored text, valid until the next non-const call on this
     * bag, or @p fallback.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-STG-4
     */
    const char* getString(const char* key, const char* fallback) const noexcept;
    /**
     * @brief Reads an array-of-double value, following the fill-caller-buffer
     * convention.
     * @param key The key; null reads as absent.
     * @param buffer Receives up to @p capacity elements; null queries the count.
     * @param capacity The buffer capacity in elements.
     * @return The stored element count, whatever @p capacity is; 0 when the key
     * is absent or holds another type.
     * @throws Nothing in practice; declared without noexcept to match the
     * convention.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-STG-4
     */
    int32_t getDoubleArray(const char* key, double* buffer, int32_t capacity) const;

    /**
     * @brief Returns the number of entries.
     * @return The count.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-STG-4
     */
    int32_t count() const noexcept;
    /**
     * @brief Returns the key at an index; keys ascend bytewise.
     * @param index Zero-based, in [0, count()).
     * @return The UTF-8 key, valid until the next non-const call.
     * @throws InvalidArgumentError When the index is out of range.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-STG-4
     */
    const char* keyAt(int32_t index) const;
    /**
     * @brief Writes the bag as one JSON object, keys in stored order.
     *
     * Integers are written as integers; doubles always with a fraction or
     * exponent (shortest form that round-trips); booleans, strings and arrays
     * of numbers as JSON does. UTF-8, no insignificant whitespace.
     * @param buffer Receives the text and a terminator; null queries the length.
     * @param capacity The buffer capacity in bytes, terminator included. A
     * shorter buffer receives a truncated, terminated text.
     * @return The text length in bytes excluding the terminator.
     * @throws InternalError When serialisation fails.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-STG-4, UR-RES-11
     */
    int32_t toJson(char* buffer, int32_t capacity) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace slideio::processor
