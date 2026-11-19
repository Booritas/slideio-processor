// Requirements: UR-STG-2, UR-STG-3, UR-STG-4, UR-STG-10, UR-IMG-5
#pragma once

/**
 * @file stage_definition.hpp
 * @brief StageDefinition: the durable identity of a stage, a PipelineSpec plus the
 * layers, rasters and stage names only a workspace can supply.
 */

#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/core/ids.hpp>
#include <slideio/processor/pipeline/export.hpp>
#include <slideio/processor/pipeline/pipeline_spec.hpp>

#include <cstdint>
#include <memory>

namespace slideio::processor {

namespace detail {
struct StageDefinitionAccess;
}

/**
 * @brief A named stage: what it computes (a PipelineSpec) and what it reads, writes,
 * annotates and depends on, declared in the vocabulary of one workspace's store.
 *
 * Builder members return the definition so calls chain. Nothing is validated against a
 * store here; StageRegistry::declare() resolves the ids and the stage names. Layers a
 * stage reads, annotates or is restricted to, and layers it reads as rasters, appear
 * once each in readLayerAt() in the order they were first declared. Declaring a
 * layer, raster or explicit dependency twice with the same value keeps one entry.
 * Strings in are UTF-8 and copied; strings out are owned by the definition.
 * @par Thread-safety
 * ThreadConfined.
 * @par Requirements
 * UR-STG-2, UR-STG-3, UR-STG-4, UR-STG-10, UR-IMG-5
 */
class SLIDEIO_PROCESSOR_ENGINE_EXPORTS StageDefinition {
public:
    /**
     * @brief Starts a definition with an empty pipeline and no declarations.
     * @param name UTF-8 stage name; copied.
     * @throws InvalidArgumentError When the name is null or empty.
     * @throws std::bad_alloc When memory is exhausted.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-STG-2
     */
    explicit StageDefinition(const char* name);
    /**
     * @brief Copies a definition; the copy is independent of the original.
     * @param other The definition to copy.
     * @throws std::bad_alloc When memory is exhausted.
     * @par Thread-safety
     * ThreadConfined: @p other must not change during the call.
     * @par Requirements
     * UR-STG-2
     */
    StageDefinition(const StageDefinition& other);
    /**
     * @brief Replaces this definition with a copy of another.
     * @param other The definition to copy.
     * @return This definition.
     * @throws std::bad_alloc When memory is exhausted; this definition is then unchanged.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-STG-2
     */
    StageDefinition& operator=(const StageDefinition& other);
    ~StageDefinition();

    /**
     * @brief Declares a layer this stage reads, by spatial query or to assert edges
     * against. Callable more than once. A stage depends on every stage that writes a
     * layer it reads (SDS 04 4.3).
     * @param layer The layer.
     * @return This definition.
     * @throws std::bad_alloc When memory is exhausted.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-STG-3
     */
    StageDefinition& readsLayer(LayerId layer);

    /**
     * @brief Declares a layer this stage may write. Callable more than once: one stage
     * may emit several layers in one pass.
     * @param layer The layer.
     * @return This definition.
     * @throws std::bad_alloc When memory is exhausted.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-STG-2
     */
    StageDefinition& writesLayer(LayerId layer);

    /**
     * @brief Declares a layer whose objects this stage may annotate. Implies
     * readsLayer() for it, never writesLayer(): an annotator is not a writer of the
     * layer (SDS 04 4.3). Callable more than once.
     * @param layer The layer.
     * @return This definition.
     * @throws std::bad_alloc When memory is exhausted.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-STG-3
     */
    StageDefinition& annotatesLayer(LayerId layer);

    /**
     * @brief Declares an ordering edge that is not a data dependency. Prefer
     * readsLayer().
     * @param stageName UTF-8 name of the earlier stage; copied.
     * @return This definition.
     * @throws InvalidArgumentError When the name is null or empty.
     * @throws std::bad_alloc When memory is exhausted.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-STG-2
     */
    StageDefinition& dependsOn(const char* stageName);

    /**
     * @brief Sets what the stage computes: channels, ROI or polygon, and the operation
     * chain.
     * @param spec The pipeline; copied. A later call replaces an earlier one.
     * @return This definition.
     * @throws std::bad_alloc When memory is exhausted.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-IMG-5, UR-STG-4
     */
    StageDefinition& pipeline(const PipelineSpec& spec);

    /**
     * @brief Declares the persisted raster this stage writes. At most one: a raster has
     * a single producing stage.
     * @param raster The raster.
     * @return This definition.
     * @throws InvalidArgumentError When a raster was already declared as written.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-STG-10
     */
    StageDefinition& writesRaster(RasterId raster);

    /**
     * @brief Binds a persisted raster as an auxiliary input under a name. The
     * dependency on its writer is derived (SDS 04 4.3).
     * @param bindingName UTF-8 binding name; copied.
     * @param raster The raster.
     * @return This definition.
     * @throws InvalidArgumentError When the binding name is null or empty.
     * @throws std::bad_alloc When memory is exhausted.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-STG-10
     */
    StageDefinition& readsRaster(const char* bindingName, RasterId raster);

    /**
     * @brief Binds an earlier stage's layer as an auxiliary raster input under a name.
     * Implies readsLayer(). Callable more than once with different names.
     * @param bindingName UTF-8 binding name; copied.
     * @param layer The layer.
     * @param content What the raster holds.
     * @return This definition.
     * @throws InvalidArgumentError When the binding name is null or empty.
     * @throws std::bad_alloc When memory is exhausted.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-STG-3
     */
    StageDefinition& readsLayerAsRaster(const char* bindingName, LayerId layer, LayerRasterContent content);

    /**
     * @brief Restricts processing to the objects of an earlier stage's layer. Implies
     * readsLayer(). A later call replaces an earlier one.
     * @param layer The layer.
     * @return This definition.
     * @throws std::bad_alloc When memory is exhausted.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-STG-3
     */
    StageDefinition& restrictToLayer(LayerId layer);

    /**
     * @brief Returns the stage name.
     * @return A UTF-8 string valid for the life of the definition.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-STG-2
     */
    const char* name() const noexcept;

    /**
     * @brief Returns the pipeline.
     * @return The spec; empty until pipeline() is called.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-IMG-5
     */
    const PipelineSpec& pipeline() const noexcept;

    /**
     * @brief Counts the layers read: explicit reads, annotated layers, the restriction
     * and the layers read as rasters, each once.
     * @return The count.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-STG-3
     */
    int32_t readLayerCount() const noexcept;
    /**
     * @brief Returns a layer read, in first-declaration order.
     * @param index Position, 0 to readLayerCount() - 1.
     * @return The layer.
     * @throws InvalidArgumentError When the index is out of range.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-STG-3
     */
    LayerId readLayerAt(int32_t index) const;

    /**
     * @brief Counts the layers this stage may write.
     * @return The count.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-STG-2
     */
    int32_t writtenLayerCount() const noexcept;
    /**
     * @brief Returns a layer this stage may write.
     * @param index Position, 0 to writtenLayerCount() - 1.
     * @return The layer.
     * @throws InvalidArgumentError When the index is out of range.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-STG-2
     */
    LayerId writtenLayerAt(int32_t index) const;

    /**
     * @brief Counts the layers this stage may annotate.
     * @return The count.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-STG-3
     */
    int32_t annotatedLayerCount() const noexcept;
    /**
     * @brief Returns a layer this stage may annotate.
     * @param index Position, 0 to annotatedLayerCount() - 1.
     * @return The layer.
     * @throws InvalidArgumentError When the index is out of range.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-STG-3
     */
    LayerId annotatedLayerAt(int32_t index) const;

    /**
     * @brief Tells whether a written raster was declared.
     * @return True after writesRaster().
     * @throws Nothing.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-STG-10
     */
    bool writesAnyRaster() const noexcept;
    /**
     * @brief Returns the written raster.
     * @return The raster; invalid when writesAnyRaster() is false.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-STG-10
     */
    RasterId writtenRaster() const noexcept;

    /**
     * @brief Counts the persisted rasters bound as inputs.
     * @return The count.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-STG-10
     */
    int32_t readRasterCount() const noexcept;
    /**
     * @brief Returns the binding name of a raster input.
     * @param index Position, 0 to readRasterCount() - 1.
     * @return A UTF-8 string valid for the life of the definition.
     * @throws InvalidArgumentError When the index is out of range.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-STG-10
     */
    const char* readRasterBindingAt(int32_t index) const;
    /**
     * @brief Returns a raster bound as an input.
     * @param index Position, 0 to readRasterCount() - 1.
     * @return The raster.
     * @throws InvalidArgumentError When the index is out of range.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-STG-10
     */
    RasterId readRasterAt(int32_t index) const;

    /**
     * @brief Counts the layers bound as raster inputs.
     * @return The count.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-STG-3
     */
    int32_t rasterLayerCount() const noexcept;
    /**
     * @brief Returns the binding name of a layer read as a raster.
     * @param index Position, 0 to rasterLayerCount() - 1.
     * @return A UTF-8 string valid for the life of the definition.
     * @throws InvalidArgumentError When the index is out of range.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-STG-3
     */
    const char* rasterBindingAt(int32_t index) const;
    /**
     * @brief Returns a layer read as a raster.
     * @param index Position, 0 to rasterLayerCount() - 1.
     * @return The layer.
     * @throws InvalidArgumentError When the index is out of range.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-STG-3
     */
    LayerId rasterLayerAt(int32_t index) const;
    /**
     * @brief Returns what a layer read as a raster holds.
     * @param index Position, 0 to rasterLayerCount() - 1.
     * @return The content.
     * @throws InvalidArgumentError When the index is out of range.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-STG-3
     */
    LayerRasterContent rasterContentAt(int32_t index) const;

    /**
     * @brief Counts the explicit stage dependencies.
     * @return The count.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-STG-2
     */
    int32_t explicitDependencyCount() const noexcept;
    /**
     * @brief Returns the name of an explicit dependency.
     * @param index Position, 0 to explicitDependencyCount() - 1.
     * @return A UTF-8 string valid for the life of the definition.
     * @throws InvalidArgumentError When the index is out of range.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-STG-2
     */
    const char* explicitDependencyAt(int32_t index) const;

    /**
     * @brief Tells whether the stage is restricted to a layer.
     * @return True after restrictToLayer().
     * @throws Nothing.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-STG-3
     */
    bool hasLayerRestriction() const noexcept;
    /**
     * @brief Returns the layer the stage is restricted to.
     * @return The layer; invalid when hasLayerRestriction() is false.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-STG-3
     */
    LayerId layerRestriction() const noexcept;

private:
    friend struct detail::StageDefinitionAccess;
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace slideio::processor
