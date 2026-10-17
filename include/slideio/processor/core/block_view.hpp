// Requirements: UR-API-3, UR-IMG-9, UR-IMG-10
#pragma once

#include <slideio/processor/core/data_type.hpp>
#include <slideio/processor/core/export.hpp>
#include <slideio/processor/core/geometry.hpp>

#include <cstdint>
#include <memory>

namespace slideio::processor {

/**
 * @brief One plane of a BlockView: a contiguous, strided buffer holding one channel.
 *
 * A plain data structure; it owns nothing.
 * @par Thread-safety
 * ThreadCompatible.
 * @par Requirements
 * UR-API-3, UR-IMG-9, UR-IMG-10
 */
struct SLIDEIO_PROCESSOR_DATA_EXPORTS BlockPlane {
    void* data;              ///< First byte of row 0.
    DataType type;           ///< Sample type of this plane.
    int64_t rowStrideBytes;  ///< Bytes between rows; at least width * dataTypeSizeBytes(type).
    int32_t sourceChannel;   ///< The scene channel this plane holds.
};

/**
 * @brief A planar, non-owning view over raster memory the caller owns.
 *
 * One plane per selected channel, each with its own DataType. Nothing is
 * copied and nothing is freed: every plane's data pointer must outlive the
 * view. Copying a view copies pointers, not pixels.
 *
 * A moved-from view is a valid null view. Every accessor is safe on it.
 * @par Thread-safety
 * ThreadCompatible; the underlying buffer's safety is the owner's contract.
 * @par Requirements
 * UR-API-3, UR-IMG-9, UR-IMG-10
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS BlockView {
public:
    /**
     * @brief Builds a null view. Does not allocate.
     * @throws Nothing.
     */
    BlockView() noexcept;
    /**
     * @brief Builds a view over caller-owned memory.
     * @param width Width in level pixels; must be positive.
     * @param height Height in level pixels; must be positive.
     * @param slideRect The same block in level-0 coordinates; must not be empty.
     * @param level Pyramid level; must not be negative.
     * @param planes Plane array, read during this call only; must not be null.
     * @param planeCount Number of planes; must be at least 1.
     * @throws InvalidArgumentError Naming the offending field ("width", "height",
     * "level", "slideRect", "planes", "planeCount", or "planes[i].data",
     * "planes[i].type", "planes[i].rowStrideBytes", "planes[i].sourceChannel"),
     * checked in that order.
     * @throws std::bad_alloc When memory is exhausted.
     */
    BlockView(int64_t width, int64_t height,
              Rect64 slideRect, int32_t level,
              const BlockPlane* planes, int32_t planeCount);
    /**
     * @brief Copies a view (pointers, not pixels).
     * @param other The view to copy.
     * @throws std::bad_alloc When memory is exhausted.
     */
    BlockView(const BlockView& other);
    /**
     * @brief Moves a view. The source becomes a null view.
     * @param other The view to move from.
     * @throws Nothing.
     */
    BlockView(BlockView&& other) noexcept;
    /**
     * @brief Copy-assigns a view.
     * @param other The view to copy.
     * @return This object.
     * @throws std::bad_alloc When memory is exhausted; this object is unchanged.
     */
    BlockView& operator=(const BlockView& other);
    /**
     * @brief Move-assigns a view.
     * @param other The view to move from.
     * @return This object.
     * @throws Nothing.
     */
    BlockView& operator=(BlockView&& other) noexcept;
    /** @brief Destroys the view; frees no pixel memory. */
    ~BlockView();

    /**
     * @brief Tells whether the view is null.
     * @return True for a default-constructed or moved-from view.
     * @throws Nothing.
     */
    bool isNull() const noexcept;
    /**
     * @brief Returns the width in level pixels.
     * @return The width, or 0 on a null view.
     * @throws Nothing.
     */
    int64_t width() const noexcept;
    /**
     * @brief Returns the height in level pixels.
     * @return The height, or 0 on a null view.
     * @throws Nothing.
     */
    int64_t height() const noexcept;
    /**
     * @brief Returns the block's position and size in level-0 coordinates.
     * @return The rectangle, or {0, 0, 0, 0} on a null view.
     * @throws Nothing.
     */
    Rect64 slideRect() const noexcept;
    /**
     * @brief Returns the pyramid level.
     * @return The level, or 0 on a null view.
     * @throws Nothing.
     */
    int32_t level() const noexcept;

    /**
     * @brief Returns the number of planes.
     * @return The count, or 0 on a null view.
     * @throws Nothing.
     */
    int32_t planeCount() const noexcept;
    /**
     * @brief Returns a plane's first byte.
     * @param plane Plane index in [0, planeCount()).
     * @return Pointer to row 0 of the plane.
     * @throws InvalidArgumentError When the index is out of range or the view is null.
     */
    void* planeData(int32_t plane) const;
    /**
     * @brief Returns a plane's sample type.
     * @param plane Plane index in [0, planeCount()).
     * @return The type.
     * @throws InvalidArgumentError When the index is out of range or the view is null.
     */
    DataType planeType(int32_t plane) const;
    /**
     * @brief Returns a plane's row stride.
     * @param plane Plane index in [0, planeCount()).
     * @return Bytes between rows.
     * @throws InvalidArgumentError When the index is out of range or the view is null.
     */
    int64_t planeRowStrideBytes(int32_t plane) const;
    /**
     * @brief Returns the scene channel a plane holds.
     * @param plane Plane index in [0, planeCount()).
     * @return The channel index.
     * @throws InvalidArgumentError When the index is out of range or the view is null.
     */
    int32_t planeSourceChannel(int32_t plane) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_; // null for a null view
};

} // namespace slideio::processor
