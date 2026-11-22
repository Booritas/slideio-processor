// Requirements: UR-SEG-3, UR-SEG-4, UR-SEG-5, UR-EXT-2, UR-EXT-4
#pragma once

/**
 * @file label_image.hpp
 * @brief LabelImageOperation: the base of an operation whose model predicts a
 * label image per tile (a cell or nucleus segmentation network), which turns
 * each tile's tile-local labels into whole objects by anchor ownership.
 */

#include <slideio/processor/core/block_view.hpp>
#include <slideio/processor/core/data_type.hpp>
#include <slideio/processor/pipeline/export.hpp>
#include <slideio/processor/pipeline/operation.hpp>

#include <cstdint>

namespace slideio::processor {

/**
 * @brief An abstract operation that turns a label image predicted per tile into
 * whole objects.
 *
 * A subclass implements predictLabels(): over the tile's halo rect, it writes a
 * label per pixel, 0 for background. Labels need to be distinct only within the
 * call. A model numbers the objects it finds tile by tile, so one object can
 * carry different values in two tiles and one value can name two objects. The
 * base therefore never joins objects across tiles, as `segment.contours` does
 * through its seam fragments. Each object is delivered whole by the one tile
 * that owns it:
 * 1. predictLabels() fills a zero-filled `Int32` plane over the halo rect.
 *    Labels it writes beyond the analysis level are cleared, so an object at
 *    the level's edge is cut at the edge.
 * 2. Each maximal 4-connected set of pixels of one non-zero value is one
 *    region. Two disjoint regions of one value are two objects; two touching
 *    regions of different values are two objects sharing an edge.
 * 3. Each region is traced, holes included, in level coordinates.
 * 4. A region's anchor is its topmost-then-leftmost boundary vertex, the point
 *    its ObjectId is derived from. The tile whose half-open tile rect holds the
 *    anchor owns the region; every other tile that sees it ignores it.
 * 5. An owned region touching a side of the halo rect that lies strictly inside
 *    the analysis level may continue beyond what the tile saw. It is dropped
 *    and reported through OperationContext::countTruncatedObjects(), which the
 *    run sums as Pipeline::truncatedObjectCount(). A side on or beyond the
 *    level's edge never truncates. The count is one per region dropped in one
 *    tile's view, not per object: an object larger than the halo allows may
 *    show, in another tile's view, a part whose own anchor that tile owns, and
 *    that part is dropped and counted too. So each truncated object counts at
 *    least once, possibly more, and the figure depends on the tile size; 0
 *    means nothing was dropped.
 * 6. Every other owned region is emitted whole through the context's object
 *    sink on the target layer, with `area` and `perimeter` in square level-0
 *    pixels and level-0 pixels, and `area.um2` and `perimeter.um` when the
 *    scene is calibrated.
 * 7. The output plane receives the predicted labels over the tile rect, so a
 *    raster written from the operation holds the model's tile-local labels.
 *
 * An object is complete when its extent below and beside its anchor fits the
 * halo: a subclass declares a halo at least as large as the largest object it
 * expects, in level pixels. A larger halo costs memory, since the label plane
 * covers the halo rect; the run refuses, before any tile, a halo whose working
 * set exceeds the memory ceiling.
 *
 * The defaults: Joint arity, every input type and no uniform-type requirement,
 * one `Int32` output plane, producesObjects() true. The subclass declares
 * name(), halo() and parameters(), as every operation does, and may override
 * the other declarations; it must not change the output type or plane count.
 * @par Thread-safety
 * As Operation: one instance may be applied to several tiles concurrently, so
 * predictLabels() must be reentrant.
 * @par Requirements
 * UR-SEG-3, UR-SEG-4, UR-SEG-5, UR-EXT-2, UR-EXT-4
 */
class SLIDEIO_PROCESSOR_ENGINE_EXPORTS LabelImageOperation : public Operation {
public:
    /**
     * @brief Builds the base; it holds no state.
     * @throws Nothing.
     */
    LabelImageOperation() noexcept;
    /**
     * @brief Destroys the operation.
     * @throws Nothing.
     */
    ~LabelImageOperation() override;

    /**
     * @brief Predicts the labels of one tile.
     *
     * Called once per apply(), with the tile's context: its input() covers the
     * halo rect, haloRectAtLevel(), and so does @p labels. The subclass writes
     * one label per pixel: 0 for background, any other value for an object.
     * Values need to be distinct only within the call; negative values are
     * labels like any other.
     * @param context The tile's context, read-only.
     * @param labels One `Int32` plane over the halo rect, zero-filled, owned by
     * the base and valid during the call only; row y starts at
     * planeRowStrideBytes(0) * y bytes from planeData(0).
     * @throws Whatever the implementation throws; the run fails with it as a
     * TileError naming the operation.
     * @par Thread-safety
     * Must be reentrant: other tiles call it concurrently.
     * @par Requirements
     * UR-EXT-4
     */
    virtual void predictLabels(const OperationContext& context, BlockView& labels) = 0;

    /**
     * @brief Returns ChannelArity::Joint: the model sees every selected channel.
     * @return ChannelArity::Joint.
     * @throws Nothing.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-ALG-3
     */
    ChannelArity arity() const override;
    /**
     * @brief Returns false: the selected channels may differ in type.
     * @return False.
     * @throws Nothing.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-ALG-4
     */
    bool requiresUniformTypes() const override;
    /**
     * @brief Returns every type: what the model accepts is the subclass's to
     * check in predictLabels().
     * @return DataTypeSet::all().
     * @throws Nothing.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-ALG-4
     */
    DataTypeSet supportedTypes() const override;
    /**
     * @brief Returns DataType::Int32, the type of the label plane.
     * @param inputType Ignored.
     * @return DataType::Int32.
     * @throws Nothing.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-EXT-4
     */
    DataType outputType(DataType inputType) const override;
    /**
     * @brief Returns 1: the operation writes the label plane alone.
     * @param inputPlaneCount Ignored.
     * @return 1.
     * @throws Nothing.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-ALG-3
     */
    int32_t outputPlaneCount(int32_t inputPlaneCount) const override;
    /**
     * @brief Returns true: the operation emits objects to its target layer.
     * @return True.
     * @throws Nothing.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-EXT-4
     */
    bool producesObjects() const override;
    /**
     * @brief Returns the memory one apply() allocates per pixel of the halo
     * rect: the predicted label plane, the region labels, and the labelling,
     * tracing and emission figures `segment.contours` declares.
     * @return Bytes per level pixel of the halo rect.
     * @throws Nothing.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-EXE-2, UR-EXT-4
     */
    int64_t workingSetBytesPerLevelPixel() const override;
    /**
     * @brief Returns the memory one apply() allocates that does not scale with
     * the tile: one object being emitted and the bookkeeping of its buffers.
     * @return Bytes per call.
     * @throws Nothing.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-EXE-2, UR-EXT-4
     */
    int64_t workingSetBytesPerTile() const override;
    /**
     * @brief Keeps every object and its holes, and adds `area.um2` and
     * `perimeter.um` when the scene is calibrated.
     * @param record The object, in level-0 coordinates; finished in place.
     * @param context The run's pixel size and analysis level.
     * @return True.
     * @throws std::bad_alloc When memory is exhausted.
     * @par Thread-safety
     * Reentrant.
     * @par Requirements
     * UR-SEG-5
     */
    bool finishObject(ObjectRecord& record, const ObjectFinishContext& context) const override;
    /**
     * @brief Predicts the tile's labels and emits the objects the tile owns,
     * as the class describes.
     * @param context The tile's context.
     * @throws InternalError When the output is not one `Int32` plane over the
     * tile rect, or when the halo rect does not contain the tile rect.
     * @throws InvalidArgumentError When the halo rect has too many pixels to
     * label (2^31 or more).
     * @throws Whatever predictLabels() throws.
     * @throws std::bad_alloc When memory is exhausted.
     * @par Thread-safety
     * As predictLabels().
     * @par Requirements
     * UR-SEG-3, UR-SEG-4, UR-EXT-4
     */
    void apply(OperationContext& context) final;
};

} // namespace slideio::processor
