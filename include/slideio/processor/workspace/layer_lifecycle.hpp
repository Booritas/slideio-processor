// Requirements: UR-STG-10, UR-STG-12, UR-RES-16
#pragma once

/**
 * @file layer_lifecycle.hpp
 * @brief LayerLifecycle: declaring layers and dropping primitive layers of a workspace.
 */

#include <slideio/processor/pipeline/export.hpp>
#include <slideio/processor/storage/layer_registry.hpp>

#include <cstdint>
#include <memory>

namespace slideio::processor {

/**
 * @brief The layer operations of a workspace, delegating to the store's LayerRegistry.
 *
 * Obtained from Workspace::layerLifecycle(); it lives as long as the workspace. The
 * store's own LayerDeclared events record every declaration and drop, so the workspace
 * keeps nothing of its own. Dropping a primitive layer discards its objects, leaves
 * the objects derived from it complete and queryable, and marks no stage stale
 * (UR-STG-12).
 * @par Thread-safety
 * ThreadSafe.
 * @par Requirements
 * UR-STG-10, UR-STG-12, UR-RES-16
 */
class SLIDEIO_PROCESSOR_ENGINE_EXPORTS LayerLifecycle {
public:
    LayerLifecycle(const LayerLifecycle&) = delete;             ///< Not copyable.
    LayerLifecycle& operator=(const LayerLifecycle&) = delete;  ///< Not copyable.
    ~LayerLifecycle();

    /**
     * @brief Declares a semantic layer below an existing layer.
     * @param name UTF-8 layer name; copied.
     * @param parent The layer it sits directly below.
     * @return The new layer's id.
     * @throws InvalidArgumentError As LayerRegistry::declareSemantic(): null, empty or
     * duplicate name, unknown or primitive parent.
     * @throws IoError When the workspace is read-only or closed.
     * @throws std::bad_alloc When memory is exhausted.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-10
     */
    LayerId declareSemantic(const char* name, LayerId parent);

    /**
     * @brief Declares a primitive layer, outside the containment hierarchy.
     * @param name UTF-8 layer name; copied.
     * @return The new layer's id.
     * @throws InvalidArgumentError As LayerRegistry::declarePrimitive().
     * @throws IoError When the workspace is read-only or closed.
     * @throws std::bad_alloc When memory is exhausted.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-10
     */
    LayerId declarePrimitive(const char* name);

    /**
     * @brief Discards every object of a primitive layer.
     *
     * Objects derived from it remain complete and queryable; nothing is marked stale.
     * Dropping a layer already dropped does nothing.
     * @param primitiveLayer The layer to drop.
     * @throws InvalidArgumentError When the layer is semantic or the pixel layer, or is
     * not a layer of this workspace.
     * @throws IoError When the workspace is read-only or closed.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-12, UR-RES-16
     */
    void drop(LayerId primitiveLayer);

    /**
     * @brief Tells whether a layer was dropped.
     * @param layer The layer.
     * @return True after drop(); false for any other layer, including an unknown one.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-12
     */
    bool isDropped(LayerId layer) const noexcept;

    /**
     * @brief Tells whether a layer ever held objects, which tells a dropped layer from
     * one that never ran.
     * @param layer The layer.
     * @return True when a stage wrote objects to it, also after a drop.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-12
     */
    bool wasEverPopulated(LayerId layer) const noexcept;

    /**
     * @brief Returns how many objects a drop discarded.
     * @param layer The layer.
     * @return The count; 0 when the layer was not dropped.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-12
     */
    int64_t droppedObjectCount(LayerId layer) const noexcept;

private:
    friend class Workspace;
    explicit LayerLifecycle(LayerRegistry& registry) noexcept;
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace slideio::processor
