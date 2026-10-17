// Requirements: UR-RES-2, UR-RES-3, UR-RES-4, UR-RES-5, UR-STG-3, UR-STG-5
#pragma once

/**
 * @file object_query.hpp
 * @brief ObjectQuery, its filter enumerations and MeasurementBound.
 */

#include <slideio/processor/core/export.hpp>
#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/core/ids.hpp>

#include <cstdint>
#include <memory>

namespace slideio::processor {

/**
 * @brief Which stages' objects a query sees.
 * @par Requirements
 * UR-STG-5
 */
enum class StageVisibility : int32_t {
    CompletedOnly,   ///< Only objects of completed stages (the default).
    IncludeInFlight  ///< Also objects of a stage still running; the reader accepts retraction.
};

/**
 * @brief How a polygon filter relates to an object's geometry.
 * @par Requirements
 * UR-RES-3
 */
enum class SpatialPredicate : int32_t {
    Intersects,   ///< The object overlaps the polygon.
    ContainedBy   ///< The object lies inside the polygon.
};

/**
 * @brief Filters objects by whether they have a parent.
 * @par Requirements
 * UR-RES-5, UR-STG-3
 */
enum class AttachmentFilter : int32_t {
    Any,             ///< No filtering.
    AttachedOnly,    ///< Only objects that have a parent.
    UnattachedOnly   ///< Only objects that have no parent.
};

/**
 * @brief Filters objects by whether they have an effective label.
 * @par Requirements
 * UR-RES-4
 */
enum class LabelFilter : int32_t {
    Any,             ///< No filtering.
    LabelledOnly,    ///< Only objects whose effective label set is non-empty.
    UnlabelledOnly   ///< Only objects whose effective label set is empty.
};

/**
 * @brief One measurement bound of an ObjectQuery, as read back by
 * ObjectQuery::measurementBoundAt().
 *
 * A POD value. The name pointer is owned by the query it came from.
 * @par Thread-safety
 * ThreadCompatible: concurrent reads are safe; writing a member needs
 * exclusive access.
 * @par Requirements
 * UR-RES-2
 */
struct SLIDEIO_PROCESSOR_DATA_EXPORTS MeasurementBound {
    const char* name;   ///< UTF-8 measurement name; never null; may be empty.
    double value;       ///< The bound.
    bool isLowerBound;  ///< True for "at least", false for "at most".
};

/**
 * @brief A fluent description of which objects to select from an ObjectSource.
 *
 * Every filter is optional and filters combine conjunctively. Setters do not
 * validate their input: a consumer rejects what it cannot use. After move
 * construction or move assignment the source is a default query.
 * @par Thread-safety
 * ThreadConfined (builder).
 * @par Requirements
 * UR-RES-2, UR-RES-3, UR-RES-4, UR-RES-5, UR-STG-3, UR-STG-5
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS ObjectQuery {
public:
    /**
     * @brief Builds an unfiltered query: CompletedOnly, no limit.
     * @throws std::bad_alloc When memory is exhausted.
     */
    ObjectQuery();
    /**
     * @brief Copies a query.
     * @param other The query to copy.
     * @throws std::bad_alloc When memory is exhausted.
     */
    ObjectQuery(const ObjectQuery& other);
    /**
     * @brief Moves a query. The source is left a default query.
     * @param other The query to move from.
     * @throws Nothing.
     */
    ObjectQuery(ObjectQuery&& other) noexcept;
    /**
     * @brief Copy-assigns a query.
     * @param other The query to copy.
     * @return This object.
     * @throws std::bad_alloc When memory is exhausted; this object is unchanged.
     */
    ObjectQuery& operator=(const ObjectQuery& other);
    /**
     * @brief Move-assigns a query. The source is left a default query; never
     * swaps.
     * @param other The query to move from.
     * @return This object.
     * @throws Nothing.
     */
    ObjectQuery& operator=(ObjectQuery&& other) noexcept;
    /** @brief Destroys the query. */
    ~ObjectQuery();

    /**
     * @brief Restricts to one layer. A repeated call keeps the last value.
     * @param layer The layer.
     * @return This query.
     * @throws std::bad_alloc When memory is exhausted.
     */
    ObjectQuery& layer(LayerId layer);
    /**
     * @brief Restricts to objects produced by one stage. A repeated call keeps the last value.
     * @param stage The stage.
     * @return This query.
     * @throws std::bad_alloc When memory is exhausted.
     */
    ObjectQuery& stage(StageId stage);
    /**
     * @brief Restricts to objects intersecting a rectangle, in level-0 pixels.
     * A repeated call keeps the last value.
     *
     * Contract for an ObjectSource honouring it: the result must include at
     * least every object whose geometry has positive-area overlap with `rect`.
     * It may include more, for example every object whose bounding box
     * intersects `rect`. Objects that only touch `rect` (share an edge or a
     * corner, with zero-area overlap) need not be returned. Consumers that need
     * an exact answer filter the result themselves.
     * @param rect The rectangle.
     * @return This query.
     * @throws std::bad_alloc When memory is exhausted.
     */
    ObjectQuery& intersecting(const Rect64& rect);
    /**
     * @brief Restricts to objects related to a polygon. The polygon is copied.
     * A repeated call keeps the last value.
     * @param polygon The polygon, in level-0 pixels.
     * @param predicate How an object must relate to it.
     * @return This query.
     * @throws std::bad_alloc When memory is exhausted; the query is unchanged.
     */
    ObjectQuery& withinPolygon(const Polygon& polygon, SpatialPredicate predicate);
    /**
     * @brief Matches an object whose effective label set contains this label.
     *
     * The effective set is the producing stage's label on the object row plus
     * every label any stage has since asserted. Existential: one match
     * suffices. A repeated call keeps the last value.
     * @param label The label.
     * @return This query.
     * @throws std::bad_alloc When memory is exhausted.
     */
    ObjectQuery& label(LabelId label);
    /**
     * @brief Filters on whether the effective label set is empty.
     * @param filter The filter; Any (the default) removes the restriction.
     * @return This query.
     * @throws std::bad_alloc When memory is exhausted.
     */
    ObjectQuery& labelFilter(LabelFilter filter);
    /**
     * @brief Filters on whether the object has a parent.
     * @param filter The filter; Any (the default) removes the restriction.
     * @return This query.
     * @throws std::bad_alloc When memory is exhausted.
     */
    ObjectQuery& attachment(AttachmentFilter filter);
    /**
     * @brief Appends a lower bound on a named measurement.
     *
     * Existential across stages: where two stages asserted the same name, one
     * satisfying value is a match. Each call appends a bound.
     * @param name UTF-8 measurement name; null is stored as empty and rejected
     * by the consumer.
     * @param value The inclusive lower bound.
     * @return This query.
     * @throws std::bad_alloc When memory is exhausted.
     */
    ObjectQuery& measurementAtLeast(const char* name, double value);
    /**
     * @brief Appends an upper bound on a named measurement.
     * @param name UTF-8 measurement name; null is stored as empty and rejected
     * by the consumer.
     * @param value The inclusive upper bound.
     * @return This query.
     * @throws std::bad_alloc When memory is exhausted.
     * @see measurementAtLeast
     */
    ObjectQuery& measurementAtMost(const char* name, double value);
    /**
     * @brief Sets stage visibility; the default is CompletedOnly.
     * @param visibility The visibility.
     * @return This query.
     * @throws std::bad_alloc When memory is exhausted.
     */
    ObjectQuery& visibility(StageVisibility visibility);
    /**
     * @brief Caps the number of objects returned. A repeated call keeps the last value.
     * @param maxObjects The cap; the consumer rejects a negative value.
     * @return This query.
     * @throws std::bad_alloc When memory is exhausted.
     */
    ObjectQuery& limit(int64_t maxObjects);

    /**
     * @brief Tells whether a spatial filter is set.
     * @return hasIntersecting() || hasWithinPolygon().
     * @throws Nothing.
     */
    bool hasSpatialFilter() const noexcept;
    /**
     * @brief Tells whether the query selects every visible object.
     * @return True iff none of layer, stage, a spatial filter, label, a non-Any
     * label filter, a non-Any attachment filter or a measurement bound is set.
     * Visibility and limit are not filters.
     * @throws Nothing.
     */
    bool isUnfiltered() const noexcept;

    /**
     * @brief Tells whether a layer is set.
     * @return True iff layer() was called.
     * @throws Nothing.
     */
    bool hasLayer() const noexcept;
    /**
     * @brief Returns the layer.
     * @return The layer; invalid if !hasLayer().
     * @throws Nothing.
     */
    LayerId layerValue() const noexcept;
    /**
     * @brief Tells whether a stage is set.
     * @return True iff stage() was called.
     * @throws Nothing.
     */
    bool hasStage() const noexcept;
    /**
     * @brief Returns the stage.
     * @return The stage; invalid if !hasStage().
     * @throws Nothing.
     */
    StageId stageValue() const noexcept;
    /**
     * @brief Tells whether a rectangle filter is set.
     * @return True iff intersecting() was called.
     * @throws Nothing.
     */
    bool hasIntersecting() const noexcept;
    /**
     * @brief Returns the rectangle filter.
     * @return The rectangle; all zero if !hasIntersecting().
     * @throws Nothing.
     */
    Rect64 intersectingValue() const noexcept;
    /**
     * @brief Tells whether a polygon filter is set.
     * @return True iff withinPolygon() was called.
     * @throws Nothing.
     */
    bool hasWithinPolygon() const noexcept;
    /**
     * @brief Returns the polygon filter.
     * @return The polygon; empty if !hasWithinPolygon(). The reference is valid
     * until the next non-const call on this query or its destruction.
     * @throws Nothing.
     */
    const Polygon& withinPolygonValue() const noexcept;
    /**
     * @brief Returns the polygon filter's predicate.
     * @return The predicate; Intersects if !hasWithinPolygon().
     * @throws Nothing.
     */
    SpatialPredicate withinPolygonPredicate() const noexcept;
    /**
     * @brief Tells whether a label is set.
     * @return True iff label() was called.
     * @throws Nothing.
     */
    bool hasLabel() const noexcept;
    /**
     * @brief Returns the label.
     * @return The label; invalid if !hasLabel().
     * @throws Nothing.
     */
    LabelId labelValue() const noexcept;
    /**
     * @brief Returns the label filter.
     * @return The filter; Any by default.
     * @throws Nothing.
     */
    LabelFilter labelFilterValue() const noexcept;
    /**
     * @brief Returns the attachment filter.
     * @return The filter; Any by default.
     * @throws Nothing.
     */
    AttachmentFilter attachmentValue() const noexcept;
    /**
     * @brief Returns the stage visibility.
     * @return The visibility; CompletedOnly by default.
     * @throws Nothing.
     */
    StageVisibility visibilityValue() const noexcept;
    /**
     * @brief Tells whether a limit is set.
     * @return True iff limit() was called.
     * @throws Nothing.
     */
    bool hasLimit() const noexcept;
    /**
     * @brief Returns the limit.
     * @return The cap; 0 if !hasLimit().
     * @throws Nothing.
     */
    int64_t limitValue() const noexcept;
    /**
     * @brief Returns the number of measurement bounds.
     * @return The count, in the order the bounds were added.
     * @throws Nothing.
     */
    int32_t measurementBoundCount() const noexcept;
    /**
     * @brief Returns one measurement bound.
     * @param index Zero-based index, below measurementBoundCount().
     * @return The bound. Its name pointer stays valid until the next non-const
     * call on this query or its destruction.
     * @throws InvalidArgumentError When index is out of range.
     */
    MeasurementBound measurementBoundAt(int32_t index) const;

private:
    class Impl;
    Impl& ensure();
    const Impl& view() const noexcept;
    std::unique_ptr<Impl> impl_; // null only after a move construction
};

} // namespace slideio::processor
