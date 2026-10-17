// Requirements: UR-RES-2, UR-RES-5, UR-RES-6, UR-EXT-4
#pragma once

#include <slideio/processor/core/block_view.hpp>
#include <slideio/processor/core/data_type.hpp>
#include <slideio/processor/core/export.hpp>
#include <slideio/processor/core/ids.hpp>
#include <slideio/processor/core/measurement.hpp>
#include <slideio/processor/core/object_query.hpp>
#include <slideio/processor/core/object_record.hpp>

#include <cstdint>
#include <memory>

namespace slideio::processor {

class RasterMask;

/**
 * @brief Receives the objects a stage produces.
 * @par Thread-safety
 * Each implementation declares its own.
 * @par Requirements
 * UR-RES-2, UR-EXT-4
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS ObjectSink {
public:
    /** @brief Destroys the sink. */
    virtual ~ObjectSink();
    /**
     * @brief Emits one object.
     * @param record The object; copied if retained.
     * @throws ProcessorError Implementation-defined.
     */
    virtual void emit(const ObjectRecord& record) = 0;
    /**
     * @brief Makes everything emitted so far durable or visible.
     * @throws ProcessorError Implementation-defined.
     */
    virtual void flush() = 0;
    /**
     * @brief Returns the number of objects emitted.
     * @return The count.
     */
    virtual int64_t emittedCount() const = 0;
};

/**
 * @brief Receives containment and derivation edges between existing objects.
 * @par Thread-safety
 * Each implementation declares its own.
 * @par Requirements
 * UR-RES-5, UR-EXT-4
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS EdgeSink {
public:
    /** @brief Destroys the sink. */
    virtual ~EdgeSink();
    /**
     * @brief Asserts that an object is contained in a parent.
     * @param child The child object.
     * @param parent The parent object.
     * @throws ProcessorError Implementation-defined.
     */
    virtual void assertParent(ObjectId child, ObjectId parent) = 0;
    /**
     * @brief Asserts that an object was derived from a primitive.
     * @param derived The derived object.
     * @param primitive The primitive it came from.
     * @throws ProcessorError Implementation-defined.
     */
    virtual void assertDerivedFrom(ObjectId derived, ObjectId primitive) = 0;
    /**
     * @brief Makes every assertion so far durable or visible.
     * @throws ProcessorError Implementation-defined.
     */
    virtual void flush() = 0;
    /**
     * @brief Returns the number of assertions made.
     * @return The count.
     */
    virtual int64_t assertedCount() const = 0;
};

/**
 * @brief Receives the tiles of a run's final raster output.
 * @par Thread-safety
 * Implementations must be ThreadSafe: writeTile() is called concurrently from
 * workers.
 * @par Requirements
 * UR-RES-2, UR-EXT-4
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS RasterSink {
public:
    /** @brief Destroys the sink. */
    virtual ~RasterSink();
    /**
     * @brief Writes one tile, in the geometry the view carries. Called once per tile.
     * @param tile The tile.
     * @throws ProcessorError Implementation-defined.
     */
    virtual void writeTile(const BlockView& tile) = 0;
    /**
     * @brief Makes every tile so far durable or visible.
     * @throws ProcessorError Implementation-defined.
     */
    virtual void flush() = 0;
    /**
     * @brief Returns the number of tiles written.
     * @return The count.
     */
    virtual int64_t tilesWritten() const = 0;
};

/**
 * @brief Receives additive claims about objects that already exist.
 *
 * An object may carry any number of labels from any number of stages.
 * @par Thread-safety
 * Each implementation declares its own.
 * @par Requirements
 * UR-RES-2, UR-EXT-4
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS AnnotationSink {
public:
    /** @brief Destroys the sink. */
    virtual ~AnnotationSink();
    /**
     * @brief Attaches a label to an existing object without modifying it.
     * @param object The object.
     * @param label The label.
     * @throws ProcessorError Implementation-defined.
     */
    virtual void assertLabel(ObjectId object, LabelId label) = 0;
    /**
     * @brief Attaches a named measurement to an existing object. One stage may
     * assert a given name on a given object once.
     * @param object The object.
     * @param name UTF-8 measurement name.
     * @param value The value.
     * @param unit The unit.
     * @throws ProcessorError Implementation-defined.
     */
    virtual void assertMeasurement(ObjectId object, const char* name, double value,
                                   MeasurementUnit unit) = 0;
    /**
     * @brief Makes every assertion so far durable or visible.
     * @throws ProcessorError Implementation-defined.
     */
    virtual void flush() = 0;
    /**
     * @brief Returns the number of assertions made.
     * @return The count.
     */
    virtual int64_t assertedCount() const = 0;
};

/**
 * @brief Iterates the result of an ObjectSource query.
 * @par Thread-safety
 * ThreadConfined.
 * @par Requirements
 * UR-RES-2, UR-RES-6
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS Cursor {
public:
    /** @brief Destroys the cursor. */
    virtual ~Cursor();
    /**
     * @brief Advances to the next object.
     * @return False when the result is exhausted.
     * @throws ProcessorError Implementation-defined.
     */
    virtual bool next() = 0;
    /**
     * @brief Returns the current object.
     * @return The record; valid until the next call to next() or close().
     * @throws ProcessorError When there is no current object.
     */
    virtual const ObjectRecord& current() const = 0;
    /**
     * @brief Returns the zero-based position of the current object.
     * @return The position.
     */
    virtual int64_t position() const = 0;
    /**
     * @brief Releases the cursor's resources.
     * @throws ProcessorError Implementation-defined.
     */
    virtual void close() = 0;
};
/** @brief Shared handle to a Cursor. */
using CursorPtr = std::shared_ptr<Cursor>;

/**
 * @brief Read access to stored objects, selected by an ObjectQuery.
 *
 * No method materialises a result set (UR-RES-6).
 * @par Thread-safety
 * Each implementation declares its own.
 * @par Requirements
 * UR-RES-2, UR-RES-6, UR-EXT-4
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS ObjectSource {
public:
    /** @brief Destroys the source. */
    virtual ~ObjectSource();
    /**
     * @brief Opens a cursor over the matching objects.
     * @param query The selection.
     * @return The cursor.
     * @throws InvalidArgumentError When the query cannot be honoured.
     */
    virtual CursorPtr query(const ObjectQuery& query) = 0;
    /**
     * @brief Counts the matching objects.
     * @param query The selection.
     * @return The count.
     * @throws InvalidArgumentError When the query cannot be honoured.
     */
    virtual int64_t count(const ObjectQuery& query) const = 0;
    /**
     * @brief Fetches one object by id.
     * @param id The object.
     * @param out Receives the record when found.
     * @return True when the object exists.
     * @throws ProcessorError Implementation-defined.
     */
    virtual bool get(ObjectId id, ObjectRecord& out) const = 0;
};

/**
 * @brief Resolves a region mask one tile at a time.
 * @par Thread-safety
 * Implementations must be ThreadSafe: fillMask() is called concurrently from
 * worker threads.
 * @par Requirements
 * UR-RES-5, UR-EXT-4
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS TileMaskSource {
public:
    /** @brief Destroys the source. */
    virtual ~TileMaskSource();
    /**
     * @brief Fills a mask over the region, level and scale it already carries.
     * Called once per tile.
     * @param out The mask to fill.
     * @throws ProcessorError Implementation-defined.
     */
    virtual void fillMask(RasterMask& out) = 0;
};
/** @brief Shared handle to a TileMaskSource. */
using TileMaskSourcePtr = std::shared_ptr<TileMaskSource>;

/**
 * @brief Makes a mask source backed by the objects of one layer.
 *
 * For each tile it queries `source` for the objects of `layer` intersecting the
 * tile (grown by the dilation) and rasterises them, then widens the result by
 * `dilationLevel0Pixels`. The query carries StageVisibility::CompletedOnly, so
 * a restriction never sees a concurrently running stage's partial output.
 * @param source The objects; borrowed, must outlive the returned object (C-8).
 * @param layer The layer to read; must be valid.
 * @param dilationLevel0Pixels Widening in level-0 pixels; finite and >= 0.
 * @return The mask source.
 * @throws InvalidArgumentError When `layer` is invalid or the dilation is not
 * finite and >= 0.
 * @par Thread-safety
 * The result is ThreadSafe when `source` is.
 * @par Requirements
 * UR-STG-3, UR-STG-5, UR-EXT-4
 */
SLIDEIO_PROCESSOR_DATA_EXPORTS TileMaskSourcePtr makeLayerMaskSource(ObjectSource& source,
                                                                     LayerId layer,
                                                                     double dilationLevel0Pixels);

/**
 * @brief What a layer-backed raster source writes into its plane.
 * @par Requirements
 * UR-RES-5
 */
enum class LayerRasterContent : int32_t {
    Label,        ///< The covering object's own label id (R11); 0 where none covers or it has none.
    Membership,   ///< 1 inside any object of the layer, 0 outside.
    ObjectAnchor  ///< ObjectId::anchorPacked() of the covering object; 0 where none.
};

/**
 * @brief Resolves a raster of values one tile at a time.
 * @par Thread-safety
 * Implementations must be ThreadSafe: fillTile() is called concurrently from
 * worker threads.
 * @par Requirements
 * UR-RES-5, UR-EXT-4
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS TileRasterSource {
public:
    /** @brief Destroys the source. */
    virtual ~TileRasterSource();
    /**
     * @brief Fills a view over the region, level and geometry it already carries.
     * Called once per tile.
     * @param out The view to fill.
     * @throws ProcessorError Implementation-defined.
     */
    virtual void fillTile(BlockView& out) = 0;
    /**
     * @brief Returns the plane type the framework must allocate before fillTile().
     * @return The data type.
     */
    virtual DataType planeType() const = 0;
    /**
     * @brief Returns the plane count the framework must allocate before fillTile().
     * @return The number of planes.
     */
    virtual int32_t planeCount() const = 0;
};
/** @brief Shared handle to a TileRasterSource. */
using TileRasterSourcePtr = std::shared_ptr<TileRasterSource>;

/**
 * @brief Makes a raster source backed by the objects of one layer.
 *
 * For each tile it queries `source` for the objects of `layer` intersecting the
 * tile and rasterises the requested content into plane 0, at the tile's own
 * level. Where objects overlap, the smallest ObjectId wins. The query carries
 * StageVisibility::CompletedOnly. planeCount() is 1; planeType() is Int32 for
 * Label, Byte for Membership and Int64 for ObjectAnchor. Label writes the
 * record's own label, or 0 when it has none.
 * @param source The objects; borrowed, must outlive the returned object (C-8).
 * @param layer The layer to read; must be valid.
 * @param content What to write.
 * @return The raster source.
 * @throws InvalidArgumentError When `layer` is invalid or `content` is unknown.
 * @par Thread-safety
 * The result is ThreadSafe when `source` is.
 * @par Requirements
 * UR-STG-3, UR-STG-5, UR-EXT-4
 */
SLIDEIO_PROCESSOR_DATA_EXPORTS TileRasterSourcePtr makeLayerRasterSource(ObjectSource& source,
                                                                         LayerId layer,
                                                                         LayerRasterContent content);

} // namespace slideio::processor
