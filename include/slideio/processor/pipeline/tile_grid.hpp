// Requirements: UR-IMG-4, UR-EXE-6, UR-EXT-3, UR-SEG-3, UR-QA-2, UR-QA-3, UR-IMG-5, UR-IMG-1
#pragma once

/**
 * @file tile_grid.hpp
 * @brief LevelSelector, TileGrid and defaultTileSize: level selection and a
 * tile grid laid out at the analysis level.
 */

#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/pipeline/export.hpp>
#include <slideio/processor/pipeline/operation.hpp>

#include <cstdint>
#include <memory>

namespace slideio {
class Scene;
} // namespace slideio

namespace slideio::processor {

/**
 * @brief The pyramid of a scene: which level to read, and the single mapping
 * between a level's coordinates and level 0's.
 *
 * Built from the scene's level sizes and nothing else; it keeps no reference to
 * the scene. A scene that reports no levels has one, level 0, the scene rect.
 *
 * **Two scale vocabularies.** scaleOfLevel() returns the level-to-level-0
 * factor, width(0) / width(level), which is 1 for level 0 and at least 1 for
 * every other level (4.0005 for a 2000-pixel level under an 8001-pixel level
 * 0). levelForScale() takes the opposite: a resolution fraction in (0, 1],
 * where 1 is full resolution and 1/16 is a sixteenth of it.
 * @par Thread-safety
 * Immutable: const members are safe to call concurrently.
 * @par Requirements
 * UR-IMG-4, UR-SEG-3, UR-IMG-5
 */
class SLIDEIO_PROCESSOR_ENGINE_EXPORTS LevelSelector {
public:
    /**
     * @brief Reads the pyramid of a scene.
     * @param scene The scene; its level sizes are copied.
     * @throws InvalidArgumentError When @p scene is null, or a level reports a
     * non-positive size.
     * @throws InternalError When SlideIO fails to report a level; the cause is preserved.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * Safe to call concurrently for scenes that may be queried concurrently.
     * @par Requirements
     * UR-IMG-4
     */
    explicit LevelSelector(std::shared_ptr<slideio::Scene> scene);
    /**
     * @brief Copies a selector; the copy shares only immutable state.
     * @param other The selector to copy.
     * @throws Nothing.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-IMG-4
     */
    LevelSelector(const LevelSelector& other) noexcept;
    /**
     * @brief Moves a selector. The moved-from selector keeps its value.
     * @param other The selector to move from.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-IMG-4
     */
    LevelSelector(LevelSelector&& other) noexcept;
    /**
     * @brief Replaces this selector with a copy of another.
     * @param other The selector to copy.
     * @return This selector.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-IMG-4
     */
    LevelSelector& operator=(const LevelSelector& other) noexcept;
    /**
     * @brief Replaces this selector with another. The moved-from selector keeps its value.
     * @param other The selector to move from.
     * @return This selector.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-IMG-4
     */
    LevelSelector& operator=(LevelSelector&& other) noexcept;
    /**
     * @brief Destroys the selector.
     * @throws Nothing.
     */
    ~LevelSelector();

    /**
     * @brief The coarsest level whose resolution still satisfies `scale`, so a
     * request at 1/16 never decodes level 0 and downsamples.
     * @param scale A resolution fraction, 0 < scale <= 1 (see the class notes).
     * @return The coarsest level L with 1 / scaleOfLevel(L) >= scale * (1 - 1e-9),
     * or 0 when no coarser level qualifies (including any scale above 1).
     * @throws InvalidArgumentError When @p scale is not positive or not finite.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-IMG-4
     */
    int32_t levelForScale(double scale) const;
    /**
     * @brief The coarsest level at least as large as a target size.
     * @param targetWidth Required width in level pixels, > 0.
     * @param targetHeight Required height in level pixels, > 0.
     * @return The coarsest level whose width >= @p targetWidth and height >=
     * @p targetHeight, or 0 when none is.
     * @throws InvalidArgumentError When either size is not positive.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-IMG-4
     */
    int32_t levelForSize(int64_t targetWidth, int64_t targetHeight) const;
    /**
     * @brief The level-to-level-0 factor of a level.
     * @param level A level, from 0 to the level count - 1.
     * @return double(width(0)) / double(width(level)): 1 for level 0, >= 1
     * otherwise, and not necessarily a power of two. One factor per level, the
     * one RasterMask carries.
     * @throws InvalidArgumentError When @p level is out of range.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-IMG-4, UR-SEG-3
     */
    double  scaleOfLevel(int32_t level) const;

    /**
     * @brief Level-0 to level. Rounds outward: the result covers every level-0
     * pixel of the input, so a mapped ROI never loses a boundary pixel.
     *
     * The inverse of mapToLevel0(), taken through that mapping in integers: x0
     * is the largest level ordinate whose level-0 image is at most x, and x1 the
     * smallest whose level-0 image is at least x + width; likewise y. The result
     * is therefore the same on every platform, and mapToLevel0() of it contains
     * @p level0Rect by construction.
     * @param level0Rect A rectangle in level-0 pixels; width and height not
     * negative.
     * @param level The target level.
     * @return The covering rectangle in level pixels; not clipped to the level.
     * @throws InvalidArgumentError When @p level is out of range, the width or
     * height is negative, or the far edge or the result does not fit int64.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-IMG-5
     */
    Rect64  mapToLevel(const Rect64& level0Rect, int32_t level) const;

    /**
     * @brief Level to level-0. The single mapping implementation in the library:
     * `llround(v * scaleOfLevel(level))` per ordinate, monotonic and
     * deterministic, so one level coordinate always yields one level-0
     * coordinate no matter which tile or thread asks.
     *
     * The rectangle maps by its edges: x0 = map(x), x1 = map(x + width), width
     * x1 - x0; likewise y. Two rectangles sharing a level edge share the level-0
     * edge exactly.
     * @param levelRect A rectangle in level pixels.
     * @param level Its level.
     * @return The rectangle in level-0 pixels.
     * @throws InvalidArgumentError When @p level is out of range.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-SEG-3
     */
    Rect64  mapToLevel0(const Rect64& levelRect, int32_t level) const;
    /**
     * @brief Level to level-0 for one point, through the same mapping.
     * @param levelPoint A point in level pixels.
     * @param level Its level. Precondition: in range. This overload cannot
     * throw, so a debug build asserts on an out-of-range level and a release
     * build returns @p levelPoint unchanged.
     * @return The point in level-0 pixels.
     * @throws Nothing.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-SEG-3
     */
    Point64 mapToLevel0(Point64 levelPoint, int32_t level) const noexcept;
    /**
     * @brief Level to level-0 for a polygon: every vertex through the same
     * mapping, ring order and hole flags kept.
     * @param levelPolygon A polygon in level pixels.
     * @param level Its level.
     * @return The polygon in level-0 pixels.
     * @throws InvalidArgumentError When @p level is out of range, or a mapped
     * ring is rejected by Polygon::addRing().
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-SEG-3
     */
    Polygon mapToLevel0(const Polygon& levelPolygon, int32_t level) const;

private:
    class Impl;
    std::shared_ptr<const Impl> impl_;
};

/**
 * @brief A row-major grid of tiles over a region of one pyramid level.
 *
 * The grid is laid out in LEVEL coordinates, so every tile boundary falls on an
 * exact level pixel edge, and the level-0 rectangles are derived from those
 * integers through LevelSelector's single mapping. Tile n's right edge and tile
 * n + 1's left edge are therefore the same level-0 number by construction, with
 * no gap and no overlap, on any pyramid (UR-SEG-3).
 *
 * Tiles are numbered row-major from the region's top-left: index = row *
 * columns() + column. The last column and row are clipped to the region. An
 * empty region (a non-positive side) has no tiles. Counts, rectangles and
 * indices are int64 throughout (UR-IMG-1).
 * @par Thread-safety
 * Immutable: const members are safe to call concurrently.
 * @par Requirements
 * UR-IMG-4, UR-EXE-6, UR-EXT-3, UR-SEG-3, UR-QA-2, UR-QA-3, UR-IMG-5, UR-IMG-1
 */
class SLIDEIO_PROCESSOR_ENGINE_EXPORTS TileGrid {
public:
    /**
     * @brief The grid is laid out in LEVEL coordinates. `regionAtLevel` and the
     * tile size are level-`level` pixels, so every tile boundary falls on an
     * exact level pixel edge.
     * @param regionAtLevel The region to cover, in level pixels; not checked
     * against the level's size.
     * @param level The level; its scale is copied from @p selector.
     * @param tileWidth Tile width in level pixels, >= 1.
     * @param tileHeight Tile height in level pixels, >= 1.
     * @param selector The scene's pyramid.
     * @throws InvalidArgumentError When a tile side is below 1, @p level is out
     * of range, or the tile count does not fit int64.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-IMG-4, UR-SEG-3
     */
    TileGrid(const Rect64& regionAtLevel, int32_t level,
             int32_t tileWidth, int32_t tileHeight,
             const LevelSelector& selector);
    /**
     * @brief Copies a grid; the copy shares only immutable state.
     * @param other The grid to copy.
     * @throws Nothing.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-IMG-4
     */
    TileGrid(const TileGrid& other) noexcept;
    /**
     * @brief Moves a grid. The moved-from grid keeps its value.
     * @param other The grid to move from.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-IMG-4
     */
    TileGrid(TileGrid&& other) noexcept;
    /**
     * @brief Replaces this grid with a copy of another.
     * @param other The grid to copy.
     * @return This grid.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-IMG-4
     */
    TileGrid& operator=(const TileGrid& other) noexcept;
    /**
     * @brief Replaces this grid with another. The moved-from grid keeps its value.
     * @param other The grid to move from.
     * @return This grid.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-IMG-4
     */
    TileGrid& operator=(TileGrid&& other) noexcept;
    /**
     * @brief Destroys the grid.
     * @throws Nothing.
     */
    ~TileGrid();

    /**
     * @brief The level the grid is laid out at.
     * @return The level.
     * @throws Nothing.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-IMG-4
     */
    int32_t level() const noexcept;

    /**
     * @brief The number of tiles.
     * @return columns() * rows(); 0 for an empty region.
     * @throws Nothing.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-IMG-1, UR-EXE-6
     */
    int64_t tileCount() const noexcept;
    /**
     * @brief The number of tile columns.
     * @return ceil(region width / tile width); 0 for an empty region.
     * @throws Nothing.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-IMG-1
     */
    int64_t columns() const noexcept;
    /**
     * @brief The number of tile rows.
     * @return ceil(region height / tile height); 0 for an empty region.
     * @throws Nothing.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-IMG-1
     */
    int64_t rows() const noexcept;

    /**
     * @brief Level coordinates: what an operation indexes, and what `BlockView`
     * covers. Halo is in level pixels too.
     * @param index A tile index, from 0 to tileCount() - 1.
     * @return The tile, clipped to the region on the last column and row.
     * @throws InvalidArgumentError When @p index is out of range.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-EXE-6
     */
    Rect64 tileRectAtLevel(int64_t index) const;
    /**
     * @brief The tile grown by a halo, in level pixels, and NOT clipped: halo
     * beyond the level reads as zeros, and halo beyond the region but inside the
     * level reads real pixels.
     * @param index A tile index, from 0 to tileCount() - 1.
     * @param halo The border, in level pixels; every side >= 0.
     * @return tileRectAtLevel(index) grown by @p halo on each side.
     * @throws InvalidArgumentError When @p index is out of range or a halo side is negative.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-EXT-3, UR-IMG-5
     */
    Rect64 tileRectAtLevelWithHalo(int64_t index, const Halo& halo) const;

    /**
     * @brief Level-0 coordinates, derived from the level rect through the same
     * mapping for every tile. Adjacent tiles therefore share an exact
     * level-0 boundary with no gap and no overlap.
     * @param index A tile index, from 0 to tileCount() - 1.
     * @return LevelSelector::mapToLevel0(tileRectAtLevel(index), level()).
     * @throws InvalidArgumentError When @p index is out of range.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-SEG-3, UR-QA-2, UR-QA-3
     */
    Rect64 tileRect(int64_t index) const;
    /**
     * @brief The halo'd tile in level-0 coordinates, through the same mapping.
     * @param index A tile index, from 0 to tileCount() - 1.
     * @param halo The border, in level pixels; every side >= 0.
     * @return LevelSelector::mapToLevel0(tileRectAtLevelWithHalo(index, halo), level()).
     * @throws InvalidArgumentError When @p index is out of range or a halo side is negative.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-SEG-3, UR-EXT-3
     */
    Rect64 tileRectWithHalo(int64_t index, const Halo& halo) const;

    /**
     * @brief The index of the tile at a column and row.
     * @param column From 0 to columns() - 1.
     * @param row From 0 to rows() - 1.
     * @return row * columns() + column.
     * @throws InvalidArgumentError When @p column or @p row is out of range.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-EXE-6
     */
    int64_t tileIndex(int64_t column, int64_t row) const;
    /**
     * @brief Tells whether a tile lies on the grid's border.
     * @param index A tile index, from 0 to tileCount() - 1.
     * @return True for the first or last column or row.
     * @throws InvalidArgumentError When @p index is out of range.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-SEG-3
     */
    bool    isEdgeTile(int64_t index) const;

private:
    class Impl;
    std::shared_ptr<const Impl> impl_;
};

/**
 * @brief The tile size, in level pixels: the slide's native tiling for that level
 * when available and tileSizeFromSlide is set, otherwise the configured size.
 *
 * The native tiling is LevelInfo::getTileSize(), used only when both of its
 * sides are positive.
 * @param scene The scene.
 * @param level A level of @p scene.
 * @param config Supplies tileSizeFromSlide(), tileWidth() and tileHeight().
 * @param widthOut Receives the tile width.
 * @param heightOut Receives the tile height.
 * @throws InvalidArgumentError When @p scene is null or @p level is out of
 * range; the outputs are then unchanged.
 * @throws InternalError When SlideIO fails to report a level; the cause is preserved.
 * @par Thread-safety
 * Safe to call concurrently for scenes that may be queried concurrently.
 * @par Requirements
 * UR-IMG-4, UR-EXE-6
 */
SLIDEIO_PROCESSOR_ENGINE_EXPORTS
void defaultTileSize(std::shared_ptr<slideio::Scene> scene, int32_t level, const Configuration& config,
                     int32_t& widthOut, int32_t& heightOut);

} // namespace slideio::processor
