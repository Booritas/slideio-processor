// Requirements: UR-STG-2, UR-STG-4, UR-STG-8, UR-STG-10, UR-STG-11, UR-OBS-1
#pragma once

/**
 * @file stage_registry.hpp
 * @brief StageRegistry: the stages of a workspace and the dependencies derived from them.
 */

#include <slideio/processor/core/ids.hpp>
#include <slideio/processor/pipeline/export.hpp>
#include <slideio/processor/storage/store.hpp>
#include <slideio/processor/workspace/stage_definition.hpp>
#include <slideio/processor/workspace/staleness_graph.hpp>

#include <cstdint>
#include <memory>

namespace slideio::processor {

/**
 * @brief Declares stages, derives their dependencies and reports their state.
 *
 * Obtained from Workspace::stages(); it lives as long as the workspace. The registry
 * holds each declared StageDefinition in memory and writes the durable record, the
 * written layers and the derived dependency rows through the store's StageStateWriter,
 * so StageStateReader and LayerRegistry::writerStageAt see the same graph without
 * linking this module. The read accessors duplicate StageStateReader on purpose: a
 * producer gets them beside declaration.
 *
 * The effective dependencies of a stage are every stage writing a layer it reads, every
 * stage whose written raster it reads, and its explicit dependsOn names. They are
 * recomputed on every declare, so a reader declared before its writer gains the
 * dependency when the writer is declared. The constructor restores every stored stage
 * (definition and dependencies) so a reopened workspace is complete before any declare;
 * nothing is written, so a read-only open works.
 *
 * A row whose record does not parse, or whose explicit dependsOn names a stage that is
 * not stored (or the stage itself), is kept by name only: a warning naming the stage is
 * logged and the stage has an empty definition, which derives no reads or writes. It is
 * never runnable. Its stored edges, its own dependencies and the dependencies of other
 * stages on it, are pinned into the graph as non-explicit edges and carried by every
 * later declare, so a reader of its layers stays blocked rather than becoming runnable.
 * Redeclaring the stage with a real definition drops the pins and normal derivation
 * resumes. The dependency accessors, isExplicitDependency and isRunnable all read this
 * one graph. Staleness follows a wider graph, the StalenessGraph of staleness(): the
 * layer-ordering and derived-from paths join the dependencies, and an unreadable stage
 * contributes its pinned edges as StageDependency edges. It is rebuilt from the held
 * definitions with the dependency graph, and may contain cycles the dependencies cannot.
 *
 * In a read-only workspace (Workspace::openReadOnly) a writer in another process may
 * declare stages while the registry is open. Before answering, declare(),
 * dependencyCount(), dependencyAt(), isExplicitDependency(), isRunnable() and
 * definition() compare the stored stage count and each stage's stored configuration
 * with those last restored, and restore again when either changed, so they agree with
 * count(), at() and configuration(), which read the store directly. A closed store is
 * not re-read: those members answer from the last restore.
 * @par Thread-safety
 * ThreadSafe: one mutex serialises every member.
 * @par Requirements
 * UR-STG-2, UR-STG-4, UR-STG-8, UR-OBS-1
 */
class SLIDEIO_PROCESSOR_ENGINE_EXPORTS StageRegistry {
public:
    StageRegistry(const StageRegistry&) = delete;             ///< Not copyable.
    StageRegistry& operator=(const StageRegistry&) = delete;  ///< Not copyable.
    ~StageRegistry();

    /**
     * @brief Declares a stage, derives its dependencies and persists it.
     *
     * The store is checked first: a closed one throws IoError before anything else. A
     * read-only registry then catches up with a live writer (see the class). Nothing is
     * written unless the declaration is valid: ids are checked and the candidate
     * dependency graph is built and checked for cycles in memory first. The record
     * (stageRecordJson) becomes the stage's configuration, each written layer is recorded
     * against the stage, and every stage's dependency rows are brought up to date.
     *
     * Declaring a name that already exists is a redeclaration. When it is the same
     * declaration (detail sameDeclaration: pipelines equal by value, lists compared
     * without regard to order) the same id returns and nothing is marked stale; in a
     * writable workspace its written layers are recorded again and every dependency row
     * rewritten (both repeat harmlessly), and in a read-only one nothing is written.
     * Otherwise the record is rewritten, the dependencies are re-derived and the stage
     * and its dependents are marked stale (see markStale()). A redeclaration that would
     * form a cycle is refused and changes nothing. Layers the old declaration wrote stay
     * recorded as written by the stage; storage has no way to drop them.
     * An explicit dependsOn naming the stage itself throws StageError.
     *
     * Every call to StageStateWriter is its own transaction, so a storage failure in the
     * middle of a declare can leave the stage declared (and held in memory) with
     * incomplete written-layer or dependency rows. Declaring the stage again, changed or
     * not, repairs both; any other declare repairs the dependency rows.
     * @param definition The stage; copied.
     * @return The stage's id; the existing id for a redeclaration.
     * @throws InvalidArgumentError When a layer or raster the definition names is
     * unknown, or it writes the pixel layer.
     * @throws StageError When an explicit dependency names an unknown stage (the message
     * names both stages) or the stage itself, or the effective graph would contain a
     * cycle (the message names the stages in it).
     * @throws IoError When the workspace is closed, or is read-only and the declaration
     * is new or changed, or when SQLite fails.
     * @throws std::bad_alloc When memory is exhausted.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-2, UR-STG-4, UR-OBS-1
     */
    StageId declare(const StageDefinition& definition);

    /**
     * @brief Marks a stage and everything its re-run would invalidate, transitively, as stale.
     *
     * Staleness follows the staleness() graph forward along all five paths (layer reads,
     * raster reads, layer ordering, derived-from and explicit dependsOn): the stage and
     * every stage in its forward answer. A cycle is walked once, each stage set once.
     * Each is set through StageStateWriter::setStale, so StageStateReader::isStale
     * agrees. A stage whose record could not be restored takes part like any other,
     * through its pinned edges.
     * @param stage The stage.
     * @throws InvalidArgumentError When the stage is unknown.
     * @throws IoError When the workspace is read-only or closed, or when SQLite fails.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-8, UR-STG-11
     */
    void markStale(StageId stage);
    /**
     * @brief Marks every stage a re-run of the stage would invalidate as stale, not the
     * stage itself.
     *
     * The stages are those of the stage's forward answer in staleness(), along all five
     * paths, each set once in one locked pass; the stage itself is set only when a cycle
     * leads back to it. This is what StageRunner does first when it re-runs a stage.
     * @param stage The stage being re-run.
     * @throws InvalidArgumentError When the stage is unknown.
     * @throws IoError When the workspace is read-only (before the stage is resolved) or
     * closed, or when SQLite fails.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-8, UR-STG-11
     */
    void markAffectedStale(StageId stage);
    /**
     * @brief Returns the staleness graph: what re-running a stage invalidates, and why.
     *
     * The view lives as long as the registry and its members follow every declaration;
     * the reference stays valid across declare(). See StalenessGraph for how long the
     * strings it returns stay valid.
     * @return The graph.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-8, UR-STG-11
     */
    const StalenessGraph& staleness() const noexcept;
    /**
     * @brief Clears the stale flag of one stage; its dependents are not touched.
     * @param stage The stage.
     * @throws InvalidArgumentError When the stage is unknown.
     * @throws IoError When the workspace is read-only or closed, or when SQLite fails.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-8
     */
    void clearStale(StageId stage);

    /**
     * @brief Returns the number of stages.
     * @return The count.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-4
     */
    int32_t count() const noexcept;
    /**
     * @brief Returns the stage at a position in ascending id order.
     * @param index Zero-based position, below count().
     * @return The stage id.
     * @throws InvalidArgumentError When index is out of range.
     * @throws IoError When the store is closed.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-4
     */
    StageId at(int32_t index) const;
    /**
     * @brief Finds a stage by exact name.
     * @param name UTF-8 stage name.
     * @param out Receives the id when found; unchanged otherwise.
     * @return True when the stage exists.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-4
     */
    bool find(const char* name, StageId& out) const noexcept;

    /**
     * @brief Returns the name of a stage.
     * @param stage The stage.
     * @return A null-terminated UTF-8 string, valid while the workspace lives.
     * @throws InvalidArgumentError When the stage is unknown.
     * @throws IoError When the store is closed.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-4
     */
    const char* name(StageId stage) const;
    /**
     * @brief Returns the status of a stage.
     * @param stage The stage.
     * @return The status; Pending for a stage never run.
     * @throws InvalidArgumentError When the stage is unknown.
     * @throws IoError When the store is closed.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-4
     */
    StageStatus status(StageId stage) const;
    /**
     * @brief Tells whether a stage's output is out of date with its inputs.
     * @param stage The stage.
     * @return True when the stage is marked stale.
     * @throws InvalidArgumentError When the stage is unknown.
     * @throws IoError When the store is closed.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-4, UR-STG-8
     */
    bool isStale(StageId stage) const;
    /**
     * @brief Returns the stage record JSON (stageRecordVersion 1).
     * @param stage The stage.
     * @return A null-terminated UTF-8 string, valid while the workspace lives.
     * @throws InvalidArgumentError When the stage is unknown.
     * @throws IoError When the store is closed.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-2, UR-STG-4
     */
    const char* configuration(StageId stage) const;
    /**
     * @brief Returns when a stage last entered Complete.
     * @param stage The stage.
     * @return Milliseconds since the Unix epoch; 0 if it never completed.
     * @throws InvalidArgumentError When the stage is unknown.
     * @throws IoError When the store is closed.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-4
     */
    int64_t completedAtUnixMs(StageId stage) const;

    /**
     * @brief Returns the number of effective dependencies of a stage.
     *
     * Read from the registry's graph in memory, not from the stored rows; a read-only
     * registry re-reads the store first when a writer changed the stages (see the class).
     * @param stage The stage.
     * @return The count.
     * @throws InvalidArgumentError When the stage is unknown.
     * @throws IoError In a read-only workspace, when re-reading the changed stages fails.
     * Never for a closed store, which answers from memory.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-2, UR-STG-4, UR-OBS-1
     */
    int32_t dependencyCount(StageId stage) const;
    /**
     * @brief Returns one effective dependency, in ascending StageId order. Read from the
     * registry's graph, like dependencyCount() and isExplicitDependency().
     * @param stage The stage.
     * @param index Zero-based position, below dependencyCount().
     * @return The stage depended on.
     * @throws InvalidArgumentError When the stage is unknown or index is out of range.
     * @throws IoError In a read-only workspace, when re-reading the changed stages fails.
     * Never for a closed store, which answers from memory.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-2, UR-STG-4, UR-OBS-1
     */
    StageId dependencyAt(StageId stage, int32_t index) const;
    /**
     * @brief Tells whether a dependency came from an explicit dependsOn. Read from the
     * registry's graph, like dependencyCount().
     * @param stage The stage.
     * @param index Zero-based position, below dependencyCount().
     * @return True when the dependency was declared with dependsOn, even if it is also
     * derived from reads.
     * @throws InvalidArgumentError When the stage is unknown or index is out of range.
     * @throws IoError In a read-only workspace, when re-reading the changed stages fails.
     * Never for a closed store, which answers from memory.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-2, UR-OBS-1
     */
    bool isExplicitDependency(StageId stage, int32_t index) const;
    /**
     * @brief Tells whether every effective dependency is Complete and not stale.
     * @param stage The stage.
     * @return True when so; true for a stage with no dependencies, except one whose
     * record could not be restored, which is never runnable, so a stage depending on it
     * stays blocked (the dependencies are read from the registry's graph, pinned edges
     * included; their status and stale flag from the store).
     * @throws InvalidArgumentError When the stage is unknown.
     * @throws IoError When the store is closed and the stage has a dependency, when
     * SQLite fails, or in a read-only workspace when re-reading the changed stages fails.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-4, UR-STG-8, UR-OBS-1
     */
    bool isRunnable(StageId stage) const;
    /**
     * @brief Tells whether a stage was kept by name only at restore, because its stored
     * record could not be read (see the class description). Such a stage is never
     * runnable and StageRunner refuses it.
     * @param stage The stage.
     * @return True for a stage kept by name only; false once it is redeclared with a real
     * definition.
     * @throws InvalidArgumentError When the stage is unknown.
     * @throws IoError In a read-only workspace, when re-reading the changed stages fails.
     * Never for a closed store, which answers from memory.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-4, UR-STG-10
     */
    bool isUnreadable(StageId stage) const;

    /**
     * @brief Returns the definition a stage was declared with.
     * @param stage The stage.
     * @return The definition. In a writable workspace the reference stays valid until
     * the stage is redeclared with a changed declaration; in a read-only one, for the
     * life of the registry (a definition a writer's change replaces is kept). For a stage
     * restored by name only it is an empty definition holding only the name.
     * @throws InvalidArgumentError When the stage is unknown.
     * @throws IoError In a read-only workspace, when re-reading the changed stages fails.
     * Never for a closed store, which answers from memory.
     * @par Thread-safety
     * ThreadSafe to call; the returned definition is immutable.
     * @par Requirements
     * UR-STG-2, UR-OBS-1
     */
    const StageDefinition& definition(StageId stage) const;

private:
    friend class Workspace;
    explicit StageRegistry(Store& store);
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace slideio::processor
