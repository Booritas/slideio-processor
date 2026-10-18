// Requirements: UR-IMG-2, UR-SEG-4, UR-SEG-5
#pragma once

/**
 * @file geometry.hpp
 * @brief Point64, Rect64 and Polygon: integer geometry on the pixel-corner lattice.
 */

#include <slideio/processor/core/export.hpp>

#include <cstdint>
#include <memory>

namespace slideio::processor {

/**
 * @brief A point with 64-bit integer coordinates.
 *
 * Vertices lie on the pixel-corner lattice: the unit square (0,0)-(1,1) is
 * pixel (0,0). Used as a pixel index when passed to Polygon::contains().
 * @par Thread-safety
 * ThreadCompatible: concurrent reads are safe; writing a member needs
 * exclusive access.
 * @par Requirements
 * UR-IMG-2, UR-SEG-4
 */
struct SLIDEIO_PROCESSOR_DATA_EXPORTS Point64 {
    int64_t x; ///< Horizontal coordinate.
    int64_t y; ///< Vertical coordinate.
};

/**
 * @brief Compares two points.
 * @param a First point.
 * @param b Second point.
 * @return True when both coordinates are equal.
 * @throws Nothing.
 * @par Thread-safety
 * Safe from any thread.
 * @par Requirements
 * UR-SEG-4
 */
SLIDEIO_PROCESSOR_DATA_EXPORTS bool operator==(Point64 a, Point64 b) noexcept;

/**
 * @brief An axis-aligned rectangle with 64-bit integer coordinates, half-open:
 * it covers x <= px < right() and y <= py < bottom().
 *
 * A rectangle is empty when width <= 0 or height <= 0.
 * @par Thread-safety
 * ThreadCompatible: const members may run concurrently; writing a member
 * needs exclusive access.
 * @par Requirements
 * UR-IMG-2, UR-SEG-4
 */
struct SLIDEIO_PROCESSOR_DATA_EXPORTS Rect64 {
    int64_t x; ///< Left edge.
    int64_t y; ///< Top edge.
    int64_t width; ///< Horizontal extent.
    int64_t height; ///< Vertical extent.

    /**
     * @brief Returns the exclusive right edge.
     * @return x + width.
     * @throws Nothing.
     */
    int64_t right() const noexcept;
    /**
     * @brief Returns the exclusive bottom edge.
     * @return y + height.
     * @throws Nothing.
     */
    int64_t bottom() const noexcept;
    /**
     * @brief Returns the number of pixels covered.
     * @return width * height, computed in 64 bits (UR-IMG-2). Not meaningful
     * for an empty rectangle with a negative side.
     * @throws Nothing.
     */
    int64_t area() const noexcept;
    /**
     * @brief Tells whether the rectangle covers no pixel.
     * @return True when width <= 0 or height <= 0.
     * @throws Nothing.
     */
    bool isEmpty() const noexcept;
    /**
     * @brief Tests a pixel against the half-open extent.
     * @param p The pixel.
     * @return True when x <= p.x < right() and y <= p.y < bottom().
     * @throws Nothing.
     */
    bool contains(Point64 p) const noexcept;
    /**
     * @brief Tells whether two rectangles share at least one pixel.
     * @param other The other rectangle.
     * @return False for rectangles that only touch, and for any empty one.
     * @throws Nothing.
     */
    bool intersects(const Rect64& other) const noexcept;
    /**
     * @brief Returns the overlap of two rectangles.
     * @param other The other rectangle.
     * @return The common region, or {0, 0, 0, 0} when they do not intersect.
     * @throws Nothing.
     */
    Rect64 intersected(const Rect64& other) const noexcept;
    /**
     * @brief Returns the smallest rectangle covering both.
     * @param other The other rectangle.
     * @return The bounding rectangle. An empty operand is ignored; two empty
     * operands give {0, 0, 0, 0}.
     * @throws Nothing.
     */
    Rect64 united(const Rect64& other) const noexcept;
};

/**
 * @brief Compares two rectangles.
 * @param a First rectangle.
 * @param b Second rectangle.
 * @return True when all four fields are equal.
 * @throws Nothing.
 * @par Thread-safety
 * Safe from any thread.
 * @par Requirements
 * UR-SEG-4
 */
SLIDEIO_PROCESSOR_DATA_EXPORTS bool operator==(const Rect64& a, const Rect64& b) noexcept;

/**
 * @brief A region made of closed rings, with even-odd membership.
 *
 * Ring 0 is always an outer ring. A polygon may hold several outer rings and
 * any number of holes. Holes have no owner: membership is the even-odd rule
 * over every ring edge, whatever the ring order. Vertices lie on the
 * pixel-corner lattice (see Point64). Rings are stored without a repeated
 * closing vertex. Every polygon that crosses a public boundary is in level-0
 * slide coordinates.
 *
 * A moved-from polygon is valid and empty.
 * @par Thread-safety
 * ThreadCompatible: const members may run concurrently; any mutation needs
 * exclusive access.
 * @par Requirements
 * UR-IMG-2, UR-SEG-4, UR-SEG-5
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS Polygon {
public:
    /**
     * @brief Builds an empty polygon.
     * @throws std::bad_alloc When memory is exhausted.
     */
    Polygon();
    /**
     * @brief Copies a polygon.
     * @param other The polygon to copy.
     * @throws std::bad_alloc When memory is exhausted.
     */
    Polygon(const Polygon& other);
    /**
     * @brief Moves a polygon. The source is left empty.
     * @param other The polygon to move from.
     * @throws Nothing.
     */
    Polygon(Polygon&& other) noexcept;
    /**
     * @brief Copy-assigns a polygon.
     * @param other The polygon to copy.
     * @return This object.
     * @throws std::bad_alloc When memory is exhausted; this object is unchanged.
     */
    Polygon& operator=(const Polygon& other);
    /**
     * @brief Move-assigns a polygon. The source is left empty; never swaps.
     * @param other The polygon to move from.
     * @return This object.
     * @throws Nothing.
     */
    Polygon& operator=(Polygon&& other) noexcept;
    /** @brief Destroys the polygon. */
    ~Polygon();

    /**
     * @brief Returns the number of rings, outer and hole.
     * @return The count.
     * @throws Nothing.
     */
    int32_t ringCount() const noexcept;
    /**
     * @brief Tells whether a ring is a hole.
     * @param ring Ring index, from 0 to ringCount() - 1.
     * @return True for a hole; always false for ring 0.
     * @throws InvalidArgumentError When ring is out of range.
     */
    bool ringIsHole(int32_t ring) const;
    /**
     * @brief Returns the vertex count of a ring.
     * @param ring Ring index, from 0 to ringCount() - 1.
     * @return The count, at least 3, without a closing vertex.
     * @throws InvalidArgumentError When ring is out of range.
     */
    int64_t ringPointCount(int32_t ring) const;
    /**
     * @brief Copies a ring's vertices into a caller buffer.
     * @param ring Ring index, from 0 to ringCount() - 1.
     * @param buffer Destination, or null to query the count.
     * @param capacity Number of points the buffer holds. At most that many
     * are written; a larger ring is truncated.
     * @return The full vertex count of the ring, whatever was written.
     * @throws InvalidArgumentError When ring is out of range.
     */
    int64_t ringPoints(int32_t ring, Point64* buffer, int64_t capacity) const;

    /**
     * @brief Appends a ring.
     *
     * A last point equal to the first is dropped.
     * @param points The vertices; must not be null.
     * @param count Number of points in the array.
     * @param isHole True to add a hole. The first ring must be an outer ring.
     * @throws InvalidArgumentError When points is null, when fewer than 3
     * points remain after dropping a closing point, when the remaining points
     * all lie on one line (a zero-area ring), when the polygon's extent with
     * this ring would not fit a 64-bit integer (maxX - minX or maxY - minY
     * overflows), or when isHole is true and the polygon is empty. The polygon
     * is unchanged on failure.
     * @throws std::bad_alloc When memory is exhausted; the polygon is unchanged.
     */
    void addRing(const Point64* points, int64_t count, bool isHole);
    /**
     * @brief Removes every ring.
     * @throws Nothing.
     */
    void clear() noexcept;
    /**
     * @brief Tells whether the polygon has no ring.
     * @return True when ringCount() is 0.
     * @throws Nothing.
     */
    bool isEmpty() const noexcept;

    /**
     * @brief Returns the extent of all vertices.
     * @return {minX, minY, maxX - minX, maxY - minY}, or {0, 0, 0, 0} when empty.
     * It never overflows, because addRing() keeps the extent within int64, and
     * it is non-empty for a non-empty polygon, because no ring is collinear.
     * @throws Nothing.
     */
    Rect64 boundingBox() const noexcept;
    /**
     * @brief Returns the vertex that anchors object identity.
     *
     * The vertex with the smallest y over all rings, ties broken by the
     * smallest x. Part of the store format's stability contract.
     * @return The vertex, or {0, 0} when empty.
     * @throws Nothing.
     */
    Point64 anchor() const noexcept;
    /**
     * @brief Returns the area under the even-odd rule.
     * @return The sum of outer-ring areas minus the sum of hole areas, by the
     * shoelace formula in double precision; 0 when empty.
     * @throws Nothing.
     */
    double area() const noexcept;
    /**
     * @brief Returns the total ring length.
     * @return The length of every ring, holes included, summed; 0 when empty.
     * @throws Nothing.
     */
    double perimeter() const noexcept;
    /**
     * @brief Returns the area-weighted centroid.
     *
     * Outer rings weigh positively and holes negatively. When area() is 0 the
     * result is the mean of the vertices of ring 0.
     * @return The centroid rounded to the nearest integer, or {0, 0} when empty.
     * @throws Nothing.
     */
    Point64 centroid() const noexcept;
    /**
     * @brief Tests a pixel by the even-odd rule.
     *
     * `p` is a PIXEL INDEX, not a lattice point: the test point is the centre
     * of pixel p, (p.x + 0.5, p.y + 0.5) (plan ruling R8). Contrast
     * RasterMask::testLevel0(), which takes a corner-lattice POINT.
     * @param p The pixel index.
     * @return True when the centre lies inside.
     * @throws Nothing.
     */
    bool contains(Point64 p) const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace slideio::processor
