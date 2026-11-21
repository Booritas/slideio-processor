// Requirements: UR-RES-7
#pragma once

/**
 * @file label_scene.hpp
 * @brief rasteriseLabels(): a lazy SlideIO scene whose pixels are the label values of the
 * objects an object query admits.
 */

#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/core/object_query.hpp>
#include <slideio/processor/pipeline/export.hpp>
#include <slideio/processor/storage/store.hpp>

#include <memory>

namespace slideio {
class Scene;
} // namespace slideio

namespace slideio::processor {

/**
 * @brief Presents the objects a query admits, over a region, as a lazy label-mask scene.
 *
 * The scene is built with slideio::makeScene over a source backed by
 * makeLabelMaskSource(), so its pixels follow that function's rules: the query's label()
 * filter's LabelId where it has one, otherwise the first member of the object's effective
 * label set (read with storage's visibility, so an object admitted only by a wider
 * visibility() rasterises as 0); where objects overlap the smallest ObjectId wins; 0
 * elsewhere.
 *
 * The scene has one Int32 channel, named "labels", and its rect is (0, 0, width, height)
 * of the region: scene pixel (x, y) is level-0 pixel (region.x + x, region.y + y). Its
 * resolution and magnification are 0 (unknown); its name is empty. It has a single zoom
 * level: a block read at any output size (Scene::readResampledBlock) goes to the source
 * with that size and is rasterised at it directly, never resampled from a finer raster,
 * so every zoom is exact and no label value is blended.
 *
 * An output pixel whose centre lies outside the region reads 0. One whose centre lies
 * inside is rasterised over its whole footprint, so on a reduced read (each output pixel
 * covering several level-0 pixels) an edge pixel whose footprint crosses the region edge
 * can carry the label of an object lying only outside the region. At full resolution
 * every pixel is a single level-0 pixel and the clip is exact.
 *
 * Nothing is computed when the scene is created: the store is first queried when a block
 * is read, so a read sees the store as it is then. Each read queries only the objects that
 * intersect the block.
 * @param store The store to read. Borrowed: it must outlive the scene, and reads throw once
 * it is closed.
 * @param query The selection; it must have a layer(). Copied.
 * @param regionLevel0 The region, in level-0 pixels; not empty, each side at most
 * 2^31 - 1 pixels.
 * @return The scene.
 * @throws InvalidArgumentError When the query has no valid layer, the region is empty, or
 * a side exceeds 2^31 - 1 or the region's far edge overflows. Messages carry no values.
 * @throws InternalError When SlideIO refuses the scene's description.
 * @throws std::bad_alloc When memory is exhausted.
 * @par Thread-safety
 * The returned scene may be read from several threads at once: each read rasterises
 * through a source of its own, and the store is ThreadSafe.
 * @par Requirements
 * UR-RES-7 (SDS 07 section 7.6).
 */
SLIDEIO_PROCESSOR_ENGINE_EXPORTS std::shared_ptr<slideio::Scene> rasteriseLabels(Store& store,
                                                                                const ObjectQuery& query,
                                                                                const Rect64& regionLevel0);

} // namespace slideio::processor
