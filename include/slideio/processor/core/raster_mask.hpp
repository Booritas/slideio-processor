// Requirements: UR-IMG-5, UR-STG-3, UR-QA-2, UR-QA-3
#pragma once

#include <slideio/processor/core/export.hpp>
#include <slideio/processor/core/geometry.hpp>

#include <cstdint>
#include <memory>

namespace slideio::processor {

/**
 * @brief A binary mask over a rectangular region of one pyramid level.
 *
 * One byte per pixel, 0 outside and 255 inside, row-major with rows contiguous
 * and no padding. Allocation is `regionAtLevel.area()` bytes and is the
 * caller's to budget (see MemoryBudget). A default-constructed mask is null:
 * it has no region and no pixels.
 * @par Thread-safety
 * ThreadCompatible: one mask per tile, built and read by the worker that owns
 * it. A const mask may be read from many threads.
 * @par Requirements
 * UR-IMG-5, UR-STG-3, UR-QA-2, UR-QA-3
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS RasterMask {
public:
    /**
     * @brief Builds a null mask. Allocates nothing.
     * @throws Nothing.
     */
    RasterMask() noexcept;

    /**
     * @brief Builds a mask with every pixel clear.
     * @param regionAtLevel The region, in pixels of `level`.
     * @param level The pyramid level, at least 0.
     * @param levelScale The factor mapping level pixels to level-0 pixels;
     * finite and greater than 0.
     * @throws InvalidArgumentError For an empty region, a region whose area
     * overflows, a negative level, or a scale that is not positive and finite.
     * @throws std::bad_alloc When the pixels cannot be allocated.
     */
    RasterMask(const Rect64& regionAtLevel, int32_t level, double levelScale);

    /**
     * @brief Copies a mask, pixels included.
     * @param other The mask to copy; a null mask copies as null.
     * @throws std::bad_alloc When the pixels cannot be allocated.
     */
    RasterMask(const RasterMask& other);
    /**
     * @brief Moves a mask; the source becomes null.
     * @param other The mask to move from.
     * @throws Nothing.
     */
    RasterMask(RasterMask&& other) noexcept;
    /**
     * @brief Replaces this mask with a copy of another. Strong guarantee.
     * @param other The mask to copy.
     * @return This mask.
     * @throws std::bad_alloc When the pixels cannot be allocated; this mask is
     * unchanged.
     */
    RasterMask& operator=(const RasterMask& other);
    /**
     * @brief Replaces this mask with another by move.
     * @param other The mask to move from; it receives this mask's old state.
     * @return This mask.
     * @throws Nothing.
     */
    RasterMask& operator=(RasterMask&& other) noexcept;
    /** @brief Destroys the mask. */
    ~RasterMask();

    /**
     * @brief Tells whether the mask is null.
     * @return True for a default-constructed or moved-from mask.
     * @throws Nothing.
     */
    bool isNull() const noexcept;
    /**
     * @brief Returns the region the mask covers.
     * @return The region in level pixels; {0, 0, 0, 0} for a null mask.
     * @throws Nothing.
     */
    Rect64 regionAtLevel() const noexcept;
    /**
     * @brief Returns the pyramid level.
     * @return The level; 0 for a null mask.
     * @throws Nothing.
     */
    int32_t level() const noexcept;
    /**
     * @brief Returns the level-to-level-0 scale.
     * @return The scale; 0.0 for a null mask.
     * @throws Nothing.
     */
    double levelScale() const noexcept;

    /**
     * @brief Returns row `y` of the region, indexed from its top edge.
     * @param y The row, from 0 to regionAtLevel().height - 1.
     * @return Pointer to `regionAtLevel().width` bytes.
     * @throws InvalidArgumentError When y is out of range; always for a null
     * mask.
     */
    const uint8_t* row(int64_t y) const;
    /**
     * @brief Returns row `y` of the region for writing.
     * @param y The row, from 0 to regionAtLevel().height - 1.
     * @return Pointer to `regionAtLevel().width` bytes. Write 0 or 255.
     * @throws InvalidArgumentError When y is out of range; always for a null
     * mask.
     */
    uint8_t* row(int64_t y);

    /**
     * @brief Tests a pixel given in absolute level coordinates.
     * @param levelPoint The pixel, in level coordinates (not relative to the
     * region).
     * @return True when the pixel is in the region and set; false outside the
     * region, never an error.
     * @throws Nothing.
     */
    bool test(Point64 levelPoint) const noexcept;

    /**
     * @brief Tests the level pixel containing a level-0 slide point.
     * @param slidePoint The point, in level-0 coordinates.
     * @return test({floor(x / levelScale), floor(y / levelScale)}); false for
     * a null mask.
     * @throws Nothing.
     */
    bool testLevel0(Point64 slidePoint) const noexcept;

    /**
     * @brief Clears every pixel.
     * @throws Nothing.
     */
    void clear() noexcept;
    /**
     * @brief Tells whether no pixel is set.
     * @return True when no pixel is set; true for a null mask.
     * @throws Nothing.
     */
    bool isEmpty() const noexcept;
    /**
     * @brief Counts the set pixels.
     * @return The number of non-zero pixels; 0 for a null mask.
     * @throws Nothing.
     */
    int64_t setPixelCount() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * @brief Sets every pixel of `mask` that `polygon` covers any part of.
 *
 * Additive: pixels already set stay set, so a mask is built by calling this
 * once per object. `polygon` is in level-0 slide coordinates and is mapped
 * inside against the mask's own levelScale(); no intermediate integer polygon
 * exists. The fill rule is conservative even-odd: a pixel is set when the
 * even-odd region has positive-area overlap with the pixel's square, so a hole
 * clears a pixel only when it covers all of it. The result does not depend on
 * the mask's origin, so two tiles sharing a seam agree on it. An empty polygon
 * sets nothing.
 * @param mask The mask to add to; must not be null.
 * @param polygon The polygon, in level-0 coordinates.
 * @throws InvalidArgumentError When the mask is null.
 * @throws std::bad_alloc When working memory cannot be allocated; the mask
 * is then unchanged.
 * @par Thread-safety
 * ThreadCompatible: concurrent calls on distinct masks are safe.
 * @par Requirements
 * UR-IMG-5, UR-STG-3, UR-QA-2, UR-QA-3
 */
SLIDEIO_PROCESSOR_DATA_EXPORTS void rasteriseInto(RasterMask& mask, const Polygon& polygon);

} // namespace slideio::processor
