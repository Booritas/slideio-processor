// Requirements: UR-RES-7
#pragma once

/**
 * @file label_mask.hpp
 * @brief makeLabelMaskSource(): a tile raster source of label values driven by an object query.
 */

#include <slideio/processor/core/export.hpp>
#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/core/object_query.hpp>
#include <slideio/processor/storage/store.hpp>

namespace slideio::processor {

/**
 * @brief Makes a raster source whose pixels encode the objects a query admits.
 *
 * The source has one Int32 plane. For each fillTile() it reads the store with the
 * caller's query, except that the query's intersecting() box (if any) is intersected
 * with the tile's own level-0 extent, grown by one level-0 pixel on each side so that
 * rounding never drops a sliver, as makeLayerRasterSource() queries. The query's
 * limit() is not applied: a limit per tile would drop objects at random. Every other
 * filter (stage, label, label filter, attachment, polygon, measurement bounds,
 * visibility) applies as the query states it.
 *
 * Where an admitted object covers a pixel (conservative rasterisation, as core's layer
 * sources) the pixel's value is the query's label() filter's LabelId when it has one;
 * otherwise the first member of the object's effective label set (see
 * AnnotationStore::labelCount()), or 0 when the set is empty. Where objects overlap the
 * smallest ObjectId wins. Every other pixel is 0.
 *
 * A query selects objects; it does not clip them. An object the intersecting() box
 * admits is drawn whole, also where it runs past the box edge. To clip, choose the
 * region that is rasterised: rasteriseLabels() clips its scene to its region, and a
 * caller of fillTile() asks only for the tiles it wants.
 *
 * Effective labels are read through AnnotationStore with storage's own visibility, not
 * the query's. An object the query admits only because its visibility() is wider than
 * AnnotationStore's (an object of an incomplete stage, say) has no effective label there
 * and, without a label() filter, rasterises as 0.
 *
 * Rasterisation is core's: this source drives makeLayerRasterSource() over an
 * ObjectSource that merges the query into each tile query.
 * @param store The store to read. Borrowed: it must outlive the source, and the source's
 * fillTile() throws IoError once the store is closed.
 * @param query The selection; it must have a layer(). Copied.
 * @return The raster source; planeType() is Int32 and planeCount() is 1.
 * @throws InvalidArgumentError When the query has no layer, or its layer is invalid.
 * @throws std::bad_alloc When memory is exhausted.
 * @par Thread-safety
 * ThreadConfined: the returned source's fillTile() is not safe to call concurrently.
 * @par Requirements
 * UR-RES-7 (SDS 07 section 7.6).
 */
SLIDEIO_PROCESSOR_DATA_EXPORTS TileRasterSourcePtr makeLabelMaskSource(Store& store, const ObjectQuery& query);

} // namespace slideio::processor
