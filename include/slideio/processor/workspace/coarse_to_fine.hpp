// Requirements: UR-SEG-8, UR-IMG-4, UR-STG-10
#pragma once

/**
 * @file coarse_to_fine.hpp
 * @brief CoarseToFineSpec and declareCoarseToFine(): two wired stages, a coarse pass and
 * the fine pass that refines inside what it found.
 */

#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/core/ids.hpp>
#include <slideio/processor/pipeline/export.hpp>
#include <slideio/processor/pipeline/pipeline_spec.hpp>
#include <slideio/processor/workspace/stage_registry.hpp>

#include <memory>

namespace slideio::processor {

namespace detail {
struct CoarseToFineAccess;
}

/**
 * @brief What declareCoarseToFine() declares: the names of the two stages, what each
 * computes and the layer each writes, and how the fine one reads the coarse one.
 *
 * Builder members return the spec so calls chain. Nothing is validated here;
 * declareCoarseToFine() does that. Strings in are UTF-8 and copied.
 * @par Thread-safety
 * ThreadConfined.
 * @par Requirements
 * UR-SEG-8, UR-IMG-4, UR-STG-10
 */
class SLIDEIO_PROCESSOR_ENGINE_EXPORTS CoarseToFineSpec {
public:
    /**
     * @brief Starts a spec with no halves set, the binding "coarse" with
     * LayerRasterContent::ObjectAnchor, and the fine restriction on.
     * @param coarseStageName UTF-8 name of the coarse stage; copied.
     * @param fineStageName UTF-8 name of the fine stage; copied.
     * @throws InvalidArgumentError When a name is null or empty.
     * @throws std::bad_alloc When memory is exhausted.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-SEG-8
     */
    CoarseToFineSpec(const char* coarseStageName, const char* fineStageName);
    /**
     * @brief Copies a spec; the copy is independent of the original.
     * @param other The spec to copy.
     * @throws std::bad_alloc When memory is exhausted.
     * @par Thread-safety
     * ThreadConfined: @p other must not change during the call.
     * @par Requirements
     * UR-SEG-8
     */
    CoarseToFineSpec(const CoarseToFineSpec& other);
    /**
     * @brief Replaces this spec with a copy of another.
     * @param other The spec to copy.
     * @return This spec.
     * @throws std::bad_alloc When memory is exhausted; this spec is then unchanged.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-SEG-8
     */
    CoarseToFineSpec& operator=(const CoarseToFineSpec& other);
    ~CoarseToFineSpec();

    /**
     * @brief Sets the coarse half: what it computes, and the layer it writes. The
     * pipeline carries the coarse analysisLevel (SDS 05 5.4).
     * @param pipeline The pipeline; copied. A later call replaces an earlier one.
     * @param writes The layer the coarse stage writes.
     * @return This spec.
     * @throws std::bad_alloc When memory is exhausted.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-SEG-8, UR-IMG-4
     */
    CoarseToFineSpec& coarse(const PipelineSpec& pipeline, LayerId writes);

    /**
     * @brief Sets the fine half. Its analysisLevel may stay -1 for automatic selection;
     * when both are explicit the coarse one must be strictly coarser.
     * @param pipeline The pipeline; copied. A later call replaces an earlier one.
     * @param writes The layer the fine stage writes.
     * @return This spec.
     * @throws std::bad_alloc When memory is exhausted.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-SEG-8, UR-IMG-4
     */
    CoarseToFineSpec& fine(const PipelineSpec& pipeline, LayerId writes);

    /**
     * @brief Sets the name the fine stage's operations address the coarse raster by
     * (SDS 05 5.1), and what each pixel carries. Defaults to "coarse" and
     * LayerRasterContent::ObjectAnchor, the only content that distinguishes one coarse
     * region from another.
     * @param name UTF-8 binding name; copied.
     * @param content What the raster holds.
     * @return This spec.
     * @throws InvalidArgumentError When the name is null or empty.
     * @throws std::bad_alloc When memory is exhausted.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-STG-10
     */
    CoarseToFineSpec& binding(const char* name, LayerRasterContent content);

    /**
     * @brief Chooses whether the fine stage is also restricted to the coarse layer, so
     * it reads no tile the coarse pass found nothing in.
     * @param restrict True to restrict; the default.
     * @return This spec.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-IMG-4
     */
    CoarseToFineSpec& restrictFineToCoarse(bool restrict) noexcept;

private:
    friend struct detail::CoarseToFineAccess;
    class Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * @brief Declares the two stages, wired: the fine one reads the coarse one's layer as a
 * raster under the spec's binding (and so, through the implied readsLayer, depends on
 * it and is marked stale by it, SDS 04 4.3 and 4.5) and, unless turned off, is
 * restricted to that layer.
 *
 * Everything is validated before anything is declared. The coarse stage is declared
 * first and the fine one second. @p coarseOut and @p fineOut are assigned only when both
 * are declared.
 * @param registry The registry to declare in; must be writable.
 * @param spec The pair.
 * @param[out] coarseOut The coarse stage.
 * @param[out] fineOut The fine stage.
 * @throws StageError When the stage names match ("names"), the written layers match
 * ("layers"), both analysis levels are explicit and the coarse one is not strictly
 * greater ("levels"), or a half was never set ("coarse", "fine"); the message names the
 * field and nothing is declared.
 * @throws InvalidArgumentError, StageError, IoError From StageRegistry::declare() (an
 * unknown layer, a dependency cycle, a read-only or closed workspace and so on). When it
 * is the fine stage's declare that throws, the coarse stage stays declared, because the
 * store has no transaction across declarations; the error is rethrown unchanged and a
 * caller may redeclare the pair after fixing the cause.
 * @throws std::bad_alloc When memory is exhausted.
 * @par Thread-safety
 * ThreadSafe: each declare is serialised by the registry; the pair as a whole is not atomic.
 * @par Requirements
 * UR-SEG-8, UR-IMG-4, UR-STG-10
 */
SLIDEIO_PROCESSOR_ENGINE_EXPORTS void declareCoarseToFine(StageRegistry& registry, const CoarseToFineSpec& spec,
                                                         StageId& coarseOut, StageId& fineOut);

} // namespace slideio::processor
