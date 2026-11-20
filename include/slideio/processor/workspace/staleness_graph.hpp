// Requirements: UR-STG-8, UR-STG-11
#pragma once

/**
 * @file staleness_graph.hpp
 * @brief StalenessGraph: what re-running a stage invalidates, and why.
 */

#include <slideio/processor/core/ids.hpp>
#include <slideio/processor/pipeline/export.hpp>

#include <cstdint>
#include <memory>

namespace slideio::processor {

/**
 * @brief Why one stage invalidates another: the path of one edge of the staleness graph.
 */
enum class StalenessReason : int32_t {
    None,            ///< Never reported; kept for the layout the SDS declares.
    LayerRead,       ///< The affected stage reads a layer the source stage writes.
    RasterRead,      ///< The affected stage reads the raster the source stage writes.
    LayerOrdering,   ///< The affected stage writes a semantic layer above one the source writes.
    DerivedFrom,     ///< The affected stage reads a primitive layer the source writes and writes a semantic layer.
    StageDependency, ///< The affected stage names the source in dependsOn (or the edge is pinned).
    Reconfigured     ///< The stage was redeclared or marked stale directly.
};

class StageRegistry;

/**
 * @brief A read-only view over the staleness graph a StageRegistry derives.
 *
 * Obtained from StageRegistry::staleness(); it lives as long as the registry and
 * changes nothing (StageRegistry::markStale acts on the answers). For two stages U and V
 * there is an edge U to V, one per reason, when:
 * - LayerRead: V reads a layer U writes;
 * - RasterRead: V reads the raster U writes;
 * - LayerOrdering: V writes a semantic layer strictly above a layer U writes (the
 *   writer of `tissue` reaches the writers of `cells` and `nuclei` below it in the
 *   hierarchy, whether or not they read anything);
 * - DerivedFrom: V reads a primitive layer U writes and V writes a semantic layer; the
 *   pair is reported as DerivedFrom instead of LayerRead;
 * - StageDependency: V's explicit dependsOn names U, or the edge is pinned around a stage
 *   whose record could not be restored.
 *
 * The edges come from the registry's held definitions, never from the store's writer
 * rows, which keep a writer a redeclaration dropped. The graph may contain cycles (a
 * `cells` stage reading `nuclei` reaches the `nuclei` writer by LayerOrdering and is
 * reached from it by LayerRead); only the effective dependencies, which gate running,
 * must stay acyclic.
 *
 * The forward answer lists one entry per edge U to V in the subgraph reachable from the
 * stage S, with V as the affected stage: breadth first from S, the edges of one source
 * ordered by ascending V and then by reason. A stage reached down two paths, or by two
 * reasons, appears twice. S itself appears only when a cycle leads back to it.
 *
 * The strings returned are copied into storage the graph owns. They stay valid until
 * the next declare() on this registry, or a read-only refresh that re-reads a writer's
 * changes; a caller keeps its own copy to hold one longer. Positions are into one
 * answer, so a declaration between two calls can change it.
 * @par Thread-safety
 * ThreadSafe: every member takes the registry's mutex; the view is not an independent
 * object. The reference StageRegistry::staleness() returns stays valid across declare().
 * @par Requirements
 * UR-STG-8, UR-STG-11
 */
class SLIDEIO_PROCESSOR_ENGINE_EXPORTS StalenessGraph {
public:
    StalenessGraph(const StalenessGraph&) = delete;             ///< Not copyable.
    StalenessGraph& operator=(const StalenessGraph&) = delete;  ///< Not copyable.
    ~StalenessGraph();

    /**
     * @brief Counts the stages re-running a stage would invalidate, one entry per edge.
     *
     * A read-only registry first follows a live writer's changes.
     * @param stage The stage re-run.
     * @return The number of entries.
     * @throws InvalidArgumentError When the stage is unknown.
     * @throws IoError In a read-only workspace, when re-reading the changed stages fails.
     * Never for a closed store, which answers from memory.
     * @throws std::bad_alloc When memory is exhausted.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-8, UR-STG-11
     */
    int32_t affectedCount(StageId stage) const;
    /**
     * @brief Returns the affected stage of one entry (see the class for the order).
     * @param stage The stage re-run.
     * @param index Zero-based position, below affectedCount().
     * @return The affected stage.
     * @throws InvalidArgumentError When the stage is unknown or index is out of range.
     * @throws IoError As affectedCount().
     * @throws std::bad_alloc When memory is exhausted.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-8, UR-STG-11
     */
    StageId affectedAt(StageId stage, int32_t index) const;
    /**
     * @brief Returns the reason of one entry; it belongs to the pair, not to the stage.
     * @param stage The stage re-run.
     * @param index Zero-based position, below affectedCount().
     * @return The reason; never None.
     * @throws InvalidArgumentError When the stage is unknown or index is out of range.
     * @throws IoError As affectedCount().
     * @throws std::bad_alloc When memory is exhausted.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-8, UR-STG-11
     */
    StalenessReason reasonAt(StageId stage, int32_t index) const;
    /**
     * @brief Returns the detail of one entry, naming the source stage and the artifact,
     * for example `reads layer "tissue" written by stage "background"`.
     * @param stage The stage re-run.
     * @param index Zero-based position, below affectedCount().
     * @return A null-terminated UTF-8 string; stays valid as the class describes.
     * @throws InvalidArgumentError When the stage is unknown or index is out of range.
     * @throws IoError As affectedCount().
     * @throws std::bad_alloc When memory is exhausted.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-8, UR-STG-11
     */
    const char* reasonDetailAt(StageId stage, int32_t index) const;

    /**
     * @brief Counts the causes of a stage's staleness.
     *
     * A stage that is not stale has none. For a stale stage the causes are its incoming
     * edges U to S whose source U changed since S: U is stale, or is not Complete, or
     * completed after S did (StageStateReader::completedAtUnixMs); one entry per edge, so a
     * source joined to S by several reasons appears once per reason, as in the forward
     * answer. When no edge qualifies, the stage is its own single cause, reported as
     * Reconfigured: it was redeclared or marked stale directly. The causes come from the
     * stored flags, status
     * and completion times, so they are the same after a reopen. They are ordered by
     * source stage, then reason.
     * @param stage The stage.
     * @return The number of causes; 0 for a stage that is not stale.
     * @throws InvalidArgumentError When the stage is unknown.
     * @throws IoError When the store is closed, or in a read-only workspace when
     * re-reading the changed stages fails.
     * @throws std::bad_alloc When memory is exhausted.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-8, UR-STG-11
     */
    int32_t causeCount(StageId stage) const;
    /**
     * @brief Returns the stage that caused one entry of a stage's staleness.
     * @param stage The stale stage.
     * @param index Zero-based position, below causeCount().
     * @return The source stage; the stage itself for a Reconfigured cause.
     * @throws InvalidArgumentError When the stage is unknown or index is out of range.
     * @throws IoError As causeCount().
     * @throws std::bad_alloc When memory is exhausted.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-8, UR-STG-11
     */
    StageId causeAt(StageId stage, int32_t index) const;
    /**
     * @brief Returns the reason of one cause; never None.
     * @param stage The stale stage.
     * @param index Zero-based position, below causeCount().
     * @return The reason of the edge, or Reconfigured.
     * @throws InvalidArgumentError When the stage is unknown or index is out of range.
     * @throws IoError As causeCount().
     * @throws std::bad_alloc When memory is exhausted.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-8, UR-STG-11
     */
    StalenessReason causeReasonAt(StageId stage, int32_t index) const;
    /**
     * @brief Returns the detail of one cause: the edge's detail (see reasonDetailAt()), or
     * `the stage was redeclared or marked stale directly`.
     * @param stage The stale stage.
     * @param index Zero-based position, below causeCount().
     * @return A null-terminated UTF-8 string; stays valid as the class describes.
     * @throws InvalidArgumentError When the stage is unknown or index is out of range.
     * @throws IoError As causeCount().
     * @throws std::bad_alloc When memory is exhausted.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-8, UR-STG-11
     */
    const char* causeReasonDetailAt(StageId stage, int32_t index) const;

private:
    friend class StageRegistry;
    class Impl;
    explicit StalenessGraph(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace slideio::processor
