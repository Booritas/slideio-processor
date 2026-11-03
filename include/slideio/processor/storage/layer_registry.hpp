// Requirements: UR-STG-10, UR-RES-13, UR-RES-14, UR-STG-12, UR-SEG-4, UR-IMG-4
#pragma once

/**
 * @file layer_registry.hpp
 * @brief LayerRegistry: the layers declared in a store and their hierarchy.
 */

#include <slideio/processor/core/export.hpp>
#include <slideio/processor/core/ids.hpp>
#include <slideio/processor/storage/types.hpp>

#include <cstdint>
#include <memory>

namespace slideio::processor {

namespace detail {
class StoreImpl;
} // namespace detail

/**
 * @brief The layers of a store: the pixel layer, a branching tree of semantic layers
 * beneath it, and any number of primitive layers outside the hierarchy.
 *
 * The pixel layer is LayerId(1), named "pixel", semantic, depth 0, and exists from the
 * moment the store is created. Declared layers number from 2 upward, in declaration
 * order. Names are unique across all layers and compared exactly. A semantic layer
 * lies beneath one parent; `vessels` and `glands` may both sit above `tissue` without
 * relating to each other. Layers are never removed in this version, so ids and
 * positions are stable. A ReadOnly store answers every query, and sees layers a
 * writer in another process declares later.
 *
 * Every member throws IoError on a closed store (the noexcept ones answer as
 * documented instead), and every declaring member also on a ReadOnly store. A
 * declaration appends a LayerDeclared event in the same transaction, so a subscribed
 * viewer learns of the layer as it is declared (UR-OBS-5).
 * @par Thread-safety
 * ThreadSafe: every member may be called from any thread with no external locking.
 * Declarations are serialised with every other write.
 * @par Requirements
 * UR-STG-10, UR-RES-13, UR-RES-14, UR-STG-12, UR-SEG-4, UR-IMG-4
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS LayerRegistry {
public:
    /** @brief Releases the registry. */
    ~LayerRegistry();
    LayerRegistry(const LayerRegistry&) = delete;             ///< Not copyable.
    LayerRegistry& operator=(const LayerRegistry&) = delete;  ///< Not copyable.

    /**
     * @brief Declares a semantic layer beneath a parent.
     * @param name UTF-8 layer name, unique across all layers and not "pixel".
     * @param parent An existing, semantic, undropped layer; pass pixelLayer() for a
     * layer rooted directly at the pixel layer. The new layer's depth is the parent's
     * plus one.
     * @return The new layer id.
     * @throws InvalidArgumentError When name is null, empty, "pixel" or already
     * declared (the message names it), or when parent is unknown, primitive or
     * dropped.
     * @throws IoError When the store is closed or read-only, or when SQLite fails.
     * @throws std::bad_alloc When memory is exhausted.
     */
    LayerId declareSemantic(const char* name, LayerId parent);
    /**
     * @brief Declares a primitive layer, outside the hierarchy.
     * @param name UTF-8 layer name, unique across all layers and not "pixel".
     * @return The new layer id.
     * @throws InvalidArgumentError When name is null, empty, "pixel" or already
     * declared (the message names it).
     * @throws IoError When the store is closed or read-only, or when SQLite fails.
     * @throws std::bad_alloc When memory is exhausted.
     */
    LayerId declarePrimitive(const char* name);

    /**
     * @brief Returns the pixel layer.
     * @return LayerId(1).
     * @throws Nothing.
     */
    LayerId pixelLayer() const noexcept;

    /**
     * @brief Returns the number of layers, the pixel layer included.
     * @return The count; 0 when the store is closed or SQLite fails.
     * @throws Nothing.
     */
    int32_t count() const noexcept;
    /**
     * @brief Returns the layer at a position in ascending id order.
     * @param index Zero-based position, below count(); position 0 is the pixel layer.
     * @return The layer id.
     * @throws InvalidArgumentError When index is negative or not below count().
     * @throws IoError When the store is closed, or when SQLite fails.
     */
    LayerId at(int32_t index) const;
    /**
     * @brief Finds a layer by name; the comparison is exact, with no case folding.
     * @param name UTF-8 layer name.
     * @param out Receives the id when found; unchanged otherwise.
     * @return True when the layer exists; false for a null name, an unknown name, a
     * closed store or a SQLite failure.
     * @throws Nothing.
     */
    bool find(const char* name, LayerId& out) const noexcept;

    /**
     * @brief Returns the name of a layer.
     * @param layer The layer.
     * @return A null-terminated UTF-8 string, valid for as long as this registry
     * lives; layers are never removed or renamed.
     * @throws InvalidArgumentError When the layer is unknown.
     * @throws IoError When the store is closed, or when SQLite fails.
     * @throws std::bad_alloc When memory is exhausted.
     */
    const char* name(LayerId layer) const;
    /**
     * @brief Returns the kind of a layer.
     * @param layer The layer.
     * @return Semantic or Primitive.
     * @throws InvalidArgumentError When the layer is unknown.
     * @throws IoError When the store is closed, or when SQLite fails.
     */
    LayerKind kind(LayerId layer) const;
    /**
     * @brief Tells whether a layer is declared.
     * @param layer The layer.
     * @return True when declared; false for an unknown layer, a closed store or a
     * SQLite failure.
     * @throws Nothing.
     */
    bool exists(LayerId layer) const noexcept;

    /**
     * @brief Returns the parent of a semantic layer.
     * @param layer The layer.
     * @return The parent; an invalid LayerId for the pixel layer.
     * @throws InvalidArgumentError When the layer is unknown or primitive.
     * @throws IoError When the store is closed, or when SQLite fails.
     */
    LayerId parent(LayerId layer) const;
    /**
     * @brief Returns the distance of a semantic layer from the pixel layer.
     * @param layer The layer.
     * @return 0 for the pixel layer, the parent's depth plus one otherwise.
     * @throws InvalidArgumentError When the layer is unknown or primitive.
     * @throws IoError When the store is closed, or when SQLite fails.
     */
    int32_t depth(LayerId layer) const;
    /**
     * @brief Returns the number of semantic layers declared directly beneath a layer.
     * @param layer The layer.
     * @return The count.
     * @throws InvalidArgumentError When the layer is unknown or primitive.
     * @throws IoError When the store is closed, or when SQLite fails.
     */
    int32_t childCount(LayerId layer) const;
    /**
     * @brief Returns one child of a layer, in declaration order.
     * @param layer The layer.
     * @param index Zero-based position, below childCount().
     * @return The child.
     * @throws InvalidArgumentError When the layer is unknown or primitive, or index is
     * out of range.
     * @throws IoError When the store is closed, or when SQLite fails.
     */
    LayerId childAt(LayerId layer, int32_t index) const;

    /**
     * @brief Tells whether one layer lies strictly above another, at any distance.
     *
     * The predicate edges are validated against (UR-RES-14). A layer is not above
     * itself, and siblings are not above one another.
     * @param descendant The candidate lower layer.
     * @param ancestor The candidate higher layer.
     * @return True when ancestor is a proper ancestor of descendant.
     * @throws InvalidArgumentError When either layer is unknown or primitive.
     * @throws IoError When the store is closed, or when SQLite fails.
     */
    bool isAbove(LayerId descendant, LayerId ancestor) const;

    /**
     * @brief Tells whether a layer has been dropped.
     * @param layer The layer.
     * @return True when dropped; false for an unknown layer, a closed store or a
     * SQLite failure. Nothing in this version drops a layer.
     * @throws Nothing.
     */
    bool isDropped(LayerId layer) const noexcept;

    /**
     * @brief Returns the number of stages declared to write a layer, before any runs.
     * @param layer The layer.
     * @return The count.
     * @throws InvalidArgumentError When the layer is unknown.
     * @throws IoError When the store is closed, or when SQLite fails.
     */
    int32_t writerStageCount(LayerId layer) const;
    /**
     * @brief Returns one stage declared to write a layer, in ascending stage id order.
     * @param layer The layer.
     * @param index Zero-based position, below writerStageCount().
     * @return The stage.
     * @throws InvalidArgumentError When the layer is unknown or index is out of range.
     * @throws IoError When the store is closed, or when SQLite fails.
     */
    StageId writerStageAt(LayerId layer, int32_t index) const;

    /**
     * @brief Returns the coarsest pyramid level any stage used to write a layer.
     *
     * Geometry in a layer carries level-0 coordinates whose precision is that of this
     * level. It is the maximum over all writers, never the last one.
     * @param layer The layer.
     * @return The level; 0 for a layer written only at full resolution, by import, or
     * not yet written, for an unknown layer, a closed store or a SQLite failure.
     * @throws Nothing.
     */
    int32_t coarsestAnalysisLevel(LayerId layer) const noexcept;
    /**
     * @brief Records the level a stage analysed at, keeping the maximum.
     *
     * Called by the framework when a writing stage completes.
     * @param layer The layer written.
     * @param level The pyramid level, 0 or more.
     * @throws InvalidArgumentError When level is negative or the layer is unknown.
     * @throws IoError When the store is closed or read-only, or when SQLite fails.
     */
    void recordAnalysisLevel(LayerId layer, int32_t level);

private:
    friend class Store;
    explicit LayerRegistry(detail::StoreImpl& store);
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace slideio::processor
