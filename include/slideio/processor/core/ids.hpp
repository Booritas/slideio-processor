// Requirements: UR-RES-4, UR-QA-3
#pragma once

#include <slideio/processor/core/export.hpp>
#include <slideio/processor/core/geometry.hpp>

#include <cstdint>

namespace slideio::processor {

/**
 * @brief Identifies a layer.
 *
 * Valid iff value() > 0; the default value is 0, which means "none".
 * @par Thread-safety
 * A value type: distinct instances may be used concurrently; a const
 * instance may be read from many threads.
 * @par Requirements
 * UR-RES-4, UR-QA-3
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS LayerId {
public:
    /**
     * @brief Builds an invalid id (value 0).
     * @throws Nothing.
     */
    LayerId() noexcept;
    /**
     * @brief Builds an id from a raw value.
     * @param value The raw value; valid iff positive.
     * @throws Nothing.
     */
    explicit LayerId(int32_t value) noexcept;
    /**
     * @brief Returns the raw value.
     * @return The value given at construction, or 0.
     * @throws Nothing.
     */
    int32_t value() const noexcept;
    /**
     * @brief Tells whether the id designates something.
     * @return True iff value() > 0.
     * @throws Nothing.
     */
    bool isValid() const noexcept;

private:
    int32_t value_;
};

/**
 * @brief Compares two layer ids by raw value.
 * @param a First id.
 * @param b Second id.
 * @return True when the raw values are equal.
 * @throws Nothing.
 */
SLIDEIO_PROCESSOR_DATA_EXPORTS bool operator==(LayerId a, LayerId b) noexcept;
/**
 * @brief Compares two layer ids by raw value.
 * @param a First id.
 * @param b Second id.
 * @return True when the raw values differ.
 * @throws Nothing.
 */
SLIDEIO_PROCESSOR_DATA_EXPORTS bool operator!=(LayerId a, LayerId b) noexcept;
/**
 * @brief Orders two layer ids by raw value.
 * @param a First id.
 * @param b Second id.
 * @return True when a's raw value is smaller.
 * @throws Nothing.
 */
SLIDEIO_PROCESSOR_DATA_EXPORTS bool operator<(LayerId a, LayerId b) noexcept;

/**
 * @brief Identifies a pipeline stage.
 *
 * Valid iff value() > 0; the default value is 0, which means "none".
 * @par Thread-safety
 * A value type: distinct instances may be used concurrently; a const
 * instance may be read from many threads.
 * @par Requirements
 * UR-RES-4, UR-QA-3
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS StageId {
public:
    /**
     * @brief Builds an invalid id (value 0).
     * @throws Nothing.
     */
    StageId() noexcept;
    /**
     * @brief Builds an id from a raw value.
     * @param value The raw value; valid iff positive.
     * @throws Nothing.
     */
    explicit StageId(int32_t value) noexcept;
    /**
     * @brief Returns the raw value.
     * @return The value given at construction, or 0.
     * @throws Nothing.
     */
    int32_t value() const noexcept;
    /**
     * @brief Tells whether the id designates something.
     * @return True iff value() > 0.
     * @throws Nothing.
     */
    bool isValid() const noexcept;

private:
    int32_t value_;
};

/**
 * @brief Compares two stage ids by raw value.
 * @param a First id.
 * @param b Second id.
 * @return True when the raw values are equal.
 * @throws Nothing.
 */
SLIDEIO_PROCESSOR_DATA_EXPORTS bool operator==(StageId a, StageId b) noexcept;
/**
 * @brief Compares two stage ids by raw value.
 * @param a First id.
 * @param b Second id.
 * @return True when the raw values differ.
 * @throws Nothing.
 */
SLIDEIO_PROCESSOR_DATA_EXPORTS bool operator!=(StageId a, StageId b) noexcept;
/**
 * @brief Orders two stage ids by raw value.
 * @param a First id.
 * @param b Second id.
 * @return True when a's raw value is smaller.
 * @throws Nothing.
 */
SLIDEIO_PROCESSOR_DATA_EXPORTS bool operator<(StageId a, StageId b) noexcept;

/**
 * @brief Identifies a label.
 *
 * Valid iff value() > 0; the default value is 0, which means "none" (it is
 * the background value of a label raster).
 * @par Thread-safety
 * A value type: distinct instances may be used concurrently; a const
 * instance may be read from many threads.
 * @par Requirements
 * UR-RES-4, UR-QA-3
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS LabelId {
public:
    /**
     * @brief Builds an invalid id (value 0).
     * @throws Nothing.
     */
    LabelId() noexcept;
    /**
     * @brief Builds an id from a raw value.
     * @param value The raw value; valid iff positive.
     * @throws Nothing.
     */
    explicit LabelId(int32_t value) noexcept;
    /**
     * @brief Returns the raw value.
     * @return The value given at construction, or 0.
     * @throws Nothing.
     */
    int32_t value() const noexcept;
    /**
     * @brief Tells whether the id designates something.
     * @return True iff value() > 0.
     * @throws Nothing.
     */
    bool isValid() const noexcept;

private:
    int32_t value_;
};

/**
 * @brief Compares two label ids by raw value.
 * @param a First id.
 * @param b Second id.
 * @return True when the raw values are equal.
 * @throws Nothing.
 */
SLIDEIO_PROCESSOR_DATA_EXPORTS bool operator==(LabelId a, LabelId b) noexcept;
/**
 * @brief Compares two label ids by raw value.
 * @param a First id.
 * @param b Second id.
 * @return True when the raw values differ.
 * @throws Nothing.
 */
SLIDEIO_PROCESSOR_DATA_EXPORTS bool operator!=(LabelId a, LabelId b) noexcept;
/**
 * @brief Orders two label ids by raw value.
 * @param a First id.
 * @param b Second id.
 * @return True when a's raw value is smaller.
 * @throws Nothing.
 */
SLIDEIO_PROCESSOR_DATA_EXPORTS bool operator<(LabelId a, LabelId b) noexcept;

/**
 * @brief Identifies a raster.
 *
 * Valid iff value() > 0; the default value is 0, which means "none".
 * @par Thread-safety
 * A value type: distinct instances may be used concurrently; a const
 * instance may be read from many threads.
 * @par Requirements
 * UR-RES-4, UR-QA-3
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS RasterId {
public:
    /**
     * @brief Builds an invalid id (value 0).
     * @throws Nothing.
     */
    RasterId() noexcept;
    /**
     * @brief Builds an id from a raw value.
     * @param value The raw value; valid iff positive.
     * @throws Nothing.
     */
    explicit RasterId(int32_t value) noexcept;
    /**
     * @brief Returns the raw value.
     * @return The value given at construction, or 0.
     * @throws Nothing.
     */
    int32_t value() const noexcept;
    /**
     * @brief Tells whether the id designates something.
     * @return True iff value() > 0.
     * @throws Nothing.
     */
    bool isValid() const noexcept;

private:
    int32_t value_;
};

/**
 * @brief Compares two raster ids by raw value.
 * @param a First id.
 * @param b Second id.
 * @return True when the raw values are equal.
 * @throws Nothing.
 */
SLIDEIO_PROCESSOR_DATA_EXPORTS bool operator==(RasterId a, RasterId b) noexcept;
/**
 * @brief Compares two raster ids by raw value.
 * @param a First id.
 * @param b Second id.
 * @return True when the raw values differ.
 * @throws Nothing.
 */
SLIDEIO_PROCESSOR_DATA_EXPORTS bool operator!=(RasterId a, RasterId b) noexcept;
/**
 * @brief Orders two raster ids by raw value.
 * @param a First id.
 * @param b Second id.
 * @return True when a's raw value is smaller.
 * @throws Nothing.
 */
SLIDEIO_PROCESSOR_DATA_EXPORTS bool operator<(RasterId a, RasterId b) noexcept;

/**
 * @brief Identifies an object by layer and anchor position.
 *
 * The anchor is the object's polygon anchor() in level-0 coordinates, packed
 * as (y << 31) | x. That only fits 0 <= x < 2^31 and 0 <= y < 2^32: an anchor
 * outside that range, or an invalid layer, gives an invalid ObjectId, never a
 * wrapped one. An invalid id has layer 0 and anchorPacked() -1. Part of the
 * store format's stability contract.
 * @par Thread-safety
 * A value type: distinct instances may be used concurrently; a const
 * instance may be read from many threads.
 * @par Requirements
 * UR-RES-4, UR-QA-3
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS ObjectId {
public:
    /**
     * @brief Builds an invalid id.
     * @throws Nothing.
     */
    ObjectId() noexcept;
    /**
     * @brief Builds an id from a layer and an anchor.
     * @param layer The owning layer; must be valid.
     * @param anchor The anchor vertex, level-0 coordinates.
     * @return The id, or an invalid id when layer is invalid, x is outside
     * [0, 2^31) or y is outside [0, 2^32).
     * @throws Nothing.
     */
    static ObjectId fromAnchor(LayerId layer, Point64 anchor) noexcept;
    /**
     * @brief Parses the text form written by toString().
     *
     * Accepts exactly "<layer>:<x>:<y>" in decimal: no sign, no spaces, no
     * trailing characters, layer at least 1 and the anchor in range.
     * @param text UTF-8 text; must not be null.
     * @param out Receives the id on success; unchanged on failure.
     * @return True when text was parsed.
     * @throws InvalidArgumentError When text is null.
     */
    static bool fromString(const char* text, ObjectId& out);

    /**
     * @brief Tells whether the id designates an object.
     * @return False for a default-built id and for an out-of-range anchor.
     * @throws Nothing.
     */
    bool isValid() const noexcept;
    /**
     * @brief Returns the owning layer.
     * @return The layer, or an invalid LayerId (value 0) for an invalid id.
     * @throws Nothing.
     */
    LayerId layer() const noexcept;
    /**
     * @brief Returns the packed anchor.
     * @return (y << 31) | x, or -1 for an invalid id.
     * @throws Nothing.
     */
    int64_t anchorPacked() const noexcept;
    /**
     * @brief Returns the anchor vertex.
     * @return The unpacked anchor, or {0, 0} for an invalid id.
     * @throws Nothing.
     */
    Point64 anchorPoint() const noexcept;
    /**
     * @brief Writes the text form "<layer>:<x>:<y>", or "invalid".
     * @param buffer Destination, or null to query the length.
     * @param capacity Buffer size in bytes, terminator included.
     * @return The required length, excluding the terminator. The buffer is
     * filled and terminated when capacity allows, and truncated but still
     * terminated when it does not.
     * @throws Nothing.
     */
    int64_t toString(char* buffer, int64_t capacity) const noexcept;

private:
    LayerId layer_;
    int64_t packed_;
};

/**
 * @brief Compares two object ids.
 * @param a First id.
 * @param b Second id.
 * @return True when layer and packed anchor are equal.
 * @throws Nothing.
 */
SLIDEIO_PROCESSOR_DATA_EXPORTS bool operator==(const ObjectId& a, const ObjectId& b) noexcept;
/**
 * @brief Compares two object ids.
 * @param a First id.
 * @param b Second id.
 * @return True when they differ.
 * @throws Nothing.
 */
SLIDEIO_PROCESSOR_DATA_EXPORTS bool operator!=(const ObjectId& a, const ObjectId& b) noexcept;
/**
 * @brief Orders object ids by layer value, then by packed anchor (scan order).
 * @param a First id.
 * @param b Second id.
 * @return True when a sorts before b.
 * @throws Nothing.
 */
SLIDEIO_PROCESSOR_DATA_EXPORTS bool operator<(const ObjectId& a, const ObjectId& b) noexcept;

} // namespace slideio::processor
