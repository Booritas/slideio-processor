// Requirements: UR-STG-2, UR-STG-4, UR-STG-8
#pragma once

/**
 * @file stage_registry.hpp
 * @brief StageRegistry: the stages of a workspace and the dependencies derived from them.
 */

#include <slideio/processor/core/ids.hpp>
#include <slideio/processor/pipeline/export.hpp>
#include <slideio/processor/storage/store.hpp>
#include <slideio/processor/workspace/stage_definition.hpp>

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
 * a row whose record does not parse is kept by name only, logged as a warning naming the
 * stage, never runnable, and has no reads, writes or edges. In this version staleness follows these
 * dependencies only; the layer-ordering and derived-from paths and the StalenessGraph
 * arrive later.
 * @par Thread-safety
 * ThreadSafe: one mutex serialises every member.
 * @par Requirements
 * UR-STG-2, UR-STG-4, UR-STG-8
 */
class SLIDEIO_PROCESSOR_ENGINE_EXPORTS StageRegistry {
public:
    StageRegistry(const StageRegistry&) = delete;             ///< Not copyable.
    StageRegistry& operator=(const StageRegistry&) = delete;  ///< Not copyable.
    ~StageRegistry();

    /**
     * @brief Declares a stage, derives its dependencies and persists it.
     *
     * Nothing is written unless the declaration is valid: ids are checked and the
     * candidate dependency graph is built and checked for cycles in memory first. The
     * record (stageRecordJson) becomes the stage's configuration, each written layer is
     * recorded against the stage, and every stage's dependency rows are brought up to
     * date.
     *
     * Declaring a name that already exists is a redeclaration: when it is the same
     * declaration (detail sameDeclaration: pipelines equal by value, lists compared
     * without regard to order) nothing changes and the same id returns, also in a
     * read-only workspace; otherwise the record is rewritten, the dependencies are
     * re-derived and the stage and its dependents are marked stale (see markStale()). A
     * redeclaration that would form a cycle is refused and changes nothing. Layers the
     * old declaration wrote stay recorded as written by the stage; storage has no way to
     * drop them.
     * An explicit dependsOn naming the stage itself throws StageError.
     *
     * Every call to StageStateWriter is its own transaction, so a storage failure in the
     * middle of a declare can leave the stage declared (and held in memory) with
     * incomplete written-layer or dependency rows. The next declare rewrites every
     * stage's dependency rows and so repairs the dependencies.
     * @param definition The stage; copied.
     * @return The stage's id; the existing id for a redeclaration.
     * @throws InvalidArgumentError When a layer or
     * raster the definition names is unknown, or it writes the pixel layer.
     * @throws StageError When an explicit dependency names an unknown stage (the message
     * names both stages) or the stage itself, or the effective graph would contain a
     * cycle (the message names the stages in it).
     * @throws IoError When the workspace is read-only or closed.
     * @throws std::bad_alloc When memory is exhausted.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-2, UR-STG-4
     */
    StageId declare(const StageDefinition& definition);

    /**
     * @brief Marks a stage and everything that depends on it, transitively, as stale.
     *
     * Staleness follows the effective dependencies (reads of layers and rasters and
     * explicit dependsOn) in reverse: the stage and every stage reachable from it through
     * the stages that depend on them. Each is set through StageStateWriter::setStale, so
     * StageStateReader::isStale agrees. A stage whose record could not be restored takes
     * part like any other, but has no edges.
     * @param stage The stage.
     * @throws InvalidArgumentError When the stage is unknown.
     * @throws IoError When the workspace is read-only or closed.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-8
     */
    void markStale(StageId stage);
    /**
     * @brief Clears the stale flag of one stage; its dependents are not touched.
     * @param stage The stage.
     * @throws InvalidArgumentError When the stage is unknown.
     * @throws IoError When the workspace is read-only or closed.
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
     * @param stage The stage.
     * @return The count.
     * @throws InvalidArgumentError When the stage is unknown.
     * @throws IoError When the store is closed.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-2, UR-STG-4
     */
    int32_t dependencyCount(StageId stage) const;
    /**
     * @brief Returns one effective dependency, in ascending StageId order.
     * @param stage The stage.
     * @param index Zero-based position, below dependencyCount().
     * @return The stage depended on.
     * @throws InvalidArgumentError When the stage is unknown or index is out of range.
     * @throws IoError When the store is closed.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-2, UR-STG-4
     */
    StageId dependencyAt(StageId stage, int32_t index) const;
    /**
     * @brief Tells whether a dependency came from an explicit dependsOn.
     * @param stage The stage.
     * @param index Zero-based position, below dependencyCount().
     * @return True when the dependency was declared with dependsOn, even if it is also
     * derived from reads.
     * @throws InvalidArgumentError When the stage is unknown or index is out of range.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-2
     */
    bool isExplicitDependency(StageId stage, int32_t index) const;
    /**
     * @brief Tells whether every effective dependency is Complete and not stale.
     * @param stage The stage.
     * @return True when so; true for a stage with no dependencies, except one whose
     * record could not be restored, which is never runnable.
     * @throws InvalidArgumentError When the stage is unknown.
     * @throws IoError When the store is closed.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-4, UR-STG-8
     */
    bool isRunnable(StageId stage) const;

    /**
     * @brief Returns the definition a stage was declared with.
     * @param stage The stage.
     * @return The definition; the reference stays valid until the next declare. For a
     * stage whose record could not be restored it is an empty definition holding only
     * the name.
     * @throws InvalidArgumentError When the stage is unknown.
     * @par Thread-safety
     * ThreadSafe to call; the returned definition is immutable.
     * @par Requirements
     * UR-STG-2
     */
    const StageDefinition& definition(StageId stage) const;

private:
    friend class Workspace;
    explicit StageRegistry(Store& store);
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace slideio::processor
