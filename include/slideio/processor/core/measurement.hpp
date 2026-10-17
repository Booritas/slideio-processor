// Requirements: UR-SEG-5, UR-SEG-6
#pragma once

#include <slideio/processor/core/export.hpp>

#include <cstdint>
#include <memory>

namespace slideio::processor {

/**
 * @brief The unit of a measurement.
 *
 * Pixels and SquarePixels mean no resolution calibration was available.
 * @par Requirements
 * UR-SEG-5, UR-SEG-6
 */
enum class MeasurementUnit : int32_t {
    Pixels,            ///< Length in pixels (uncalibrated).
    Micrometres,       ///< Length in micrometres (calibrated).
    SquarePixels,      ///< Area in square pixels (uncalibrated).
    SquareMicrometres, ///< Area in square micrometres (calibrated).
    Intensity,         ///< Pixel intensity.
    Dimensionless      ///< A unitless quantity.
};

/**
 * @brief One named, typed value held by a MeasurementSet.
 *
 * A small value class. Populated instances come from MeasurementSet; a
 * default-built one has name "", value 0 and unit Dimensionless.
 * @par Thread-safety
 * ThreadCompatible: const members may run concurrently.
 * @par Requirements
 * UR-SEG-5, UR-SEG-6
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS Measurement {
public:
    /**
     * @brief Builds an empty measurement: name "", value 0, Dimensionless.
     * @throws Nothing.
     */
    Measurement() noexcept;

    /**
     * @brief Returns the measurement name.
     * @return A UTF-8 string, never null. It stays valid until the owning
     * MeasurementSet is next modified or destroyed.
     * @throws Nothing.
     */
    const char* name() const noexcept;
    /**
     * @brief Returns the value.
     * @return The value, in unit().
     * @throws Nothing.
     */
    double value() const noexcept;
    /**
     * @brief Returns the unit.
     * @return The unit.
     * @throws Nothing.
     */
    MeasurementUnit unit() const noexcept;
    /**
     * @brief Tells whether the value is in calibrated physical units.
     * @return False iff the unit is Pixels or SquarePixels, which tells
     * "3.2 um^2" from "3.2 px^2 with no calibration available" (UR-SEG-6).
     * @throws Nothing.
     */
    bool isCalibrated() const noexcept;

private:
    friend class MeasurementSet;
    Measurement(const char* name, double value, MeasurementUnit unit) noexcept;

    const char* name_;
    double value_;
    MeasurementUnit unit_;
};

/**
 * @brief An ordered set of uniquely named measurements.
 *
 * Insertion order is kept. A moved-from set is valid and empty.
 * @par Thread-safety
 * ThreadCompatible: const members may run concurrently; any mutation needs
 * exclusive access.
 * @par Requirements
 * UR-SEG-5, UR-SEG-6
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS MeasurementSet {
public:
    /**
     * @brief Builds an empty set. Does not allocate.
     * @throws Nothing.
     */
    MeasurementSet();
    /**
     * @brief Copies a set.
     * @param other The set to copy.
     * @throws std::bad_alloc When memory is exhausted.
     */
    MeasurementSet(const MeasurementSet& other);
    /**
     * @brief Moves a set. The source is left empty.
     * @param other The set to move from.
     * @throws Nothing.
     */
    MeasurementSet(MeasurementSet&& other) noexcept;
    /**
     * @brief Copy-assigns a set.
     * @param other The set to copy.
     * @return This object.
     * @throws std::bad_alloc When memory is exhausted; this object is unchanged.
     */
    MeasurementSet& operator=(const MeasurementSet& other);
    /**
     * @brief Move-assigns a set. The source is left empty.
     * @param other The set to move from.
     * @return This object.
     * @throws Nothing.
     */
    MeasurementSet& operator=(MeasurementSet&& other) noexcept;
    /** @brief Destroys the set. */
    ~MeasurementSet();

    /**
     * @brief Returns the number of measurements.
     * @return The count.
     * @throws Nothing.
     */
    int32_t count() const noexcept;
    /**
     * @brief Returns a measurement by position.
     * @param index Position, from 0 to count() - 1, in insertion order.
     * @return The measurement; its name() is valid until this set is next
     * modified or destroyed.
     * @throws InvalidArgumentError When index is out of range.
     */
    Measurement at(int32_t index) const;
    /**
     * @brief Looks a measurement up by name.
     * @param name The UTF-8 name; null is never found.
     * @param out Receives the measurement when found; otherwise unchanged.
     * @return True when found.
     * @throws Nothing.
     */
    bool find(const char* name, Measurement& out) const noexcept;

    /**
     * @brief Adds a measurement, or replaces the value and unit of an
     * existing one in place, keeping its position.
     * @param name The UTF-8 name; not null and not empty.
     * @param value The value.
     * @param unit The unit.
     * @throws InvalidArgumentError When name is null or empty.
     * @throws std::bad_alloc When memory is exhausted; the set is unchanged.
     */
    void set(const char* name, double value, MeasurementUnit unit);
    /**
     * @brief Removes a measurement. Absent or null names are ignored.
     * @param name The UTF-8 name.
     * @throws Nothing.
     */
    void remove(const char* name) noexcept;
    /**
     * @brief Removes every measurement.
     * @throws Nothing.
     */
    void clear() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_; // null until a measurement is set
};

} // namespace slideio::processor
