// Requirements: UR-STG-1, UR-STG-9, UR-EXE-2, UR-STG-10, UR-STG-12, UR-STG-2, UR-STG-4, UR-EXE-1
#pragma once

/**
 * @file workspace.hpp
 * @brief Workspace and WorkspacePtr: the directory that binds one slide scene to a
 * store, a scratch area and a memory budget.
 */

#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/memory_budget.hpp>
#include <slideio/processor/pipeline/export.hpp>
#include <slideio/processor/storage/layer_registry.hpp>
#include <slideio/processor/storage/store.hpp>
#include <slideio/processor/workspace/layer_lifecycle.hpp>
#include <slideio/processor/workspace/stage_registry.hpp>

#include <cstdint>
#include <memory>

namespace slideio {
class Scene;
} // namespace slideio

namespace slideio::processor {

class Workspace;
/** @brief Shared ownership of an open Workspace. */
using WorkspacePtr = std::shared_ptr<Workspace>;

namespace detail {
/** @brief Library-internal, not API: the workspace's run slot (run_slot.hpp). Takes it; false when taken. */
bool takeRunSlot(Workspace& workspace) noexcept;
/** @brief Library-internal, not API: releases the run slot takeRunSlot() took. */
void releaseRunSlot(Workspace& workspace) noexcept;
} // namespace detail

/**
 * @brief A directory holding one slide scene's analysis: its store, the binding to
 * the slide and scene, and a scratch area.
 *
 * The layout is fixed: the store is "<workspace>/workspace.store", with its sidecar
 * files and its "workspace.store-rasters" directory beside it, and scratch is
 * "<workspace>/scratch". Everything lives under the one path, so a complete analysis
 * copies or archives as one unit (UR-STG-9). The slide binding is the store's
 * Provenance: its slideId holds the slide path, made absolute and normalised, and its
 * sceneIndex the scene; open() reopens the slide from it. A workspace holds exactly
 * one slide.
 *
 * A writable workspace (create(), open()) holds the store's exclusive writer lock
 * until close(); a second writer, in this process or another, is refused.
 * openReadOnly() takes no lock and runs alongside a writer.
 *
 * After close(), path(), slidePath(), sceneIndex(), isReadOnly(), configuration(),
 * scratchConfiguration() and memoryBudget() still answer; scene() returns null; store()
 * and layers() return the closed store's objects, whose members throw IoError.
 *
 * The scene type is spelled `std::shared_ptr<slideio::Scene>` because SlideIO
 * defines `ScenePtr` as a preprocessor macro.
 * @par Thread-safety
 * ThreadSafe for accessors; single-writer for execution (Assumption 6).
 * @par Requirements
 * UR-STG-1, UR-STG-9, UR-EXE-2
 */
class SLIDEIO_PROCESSOR_ENGINE_EXPORTS Workspace {
public:
    /**
     * @brief Creates a workspace binding a slide scene to a new store.
     *
     * The slide is opened and the scene checked before anything is made. Then the
     * directory is made, the store created in it with the slide path and scene index
     * as its provenance, and the scratch directory made. A failure removes what this
     * call made, and only that, which it tracks rather than infers: the store, its
     * sidecars, its raster directory and scratch when it created the store, and the
     * directory when it created the directory and nothing else is left in it. A create
     * racing on the same path therefore never removes the other's workspace.
     * @param workspacePath UTF-8 path of the workspace directory; copied. It must not
     * exist, or be an empty directory. Its parent must exist: only the workspace
     * directory itself is made.
     * @param slidePath UTF-8 path of the slide, opened with SlideIO's driver lookup. A
     * relative path is resolved against the current directory; the absolute,
     * lexically normalised form is what slidePath() returns and the store records.
     * @param sceneIndex The scene of the slide, from 0 below its scene count.
     * @param configuration The settings; copied.
     * @return The open, writable workspace.
     * @throws InvalidArgumentError When a path is null or empty, or sceneIndex is out of
     * range for the slide.
     * @throws IoError When workspacePath exists and is not an empty directory, or is
     * made by someone else while this call runs; when the slide does not open (the
     * message names its path); when the parent directory is missing or the directory,
     * the store or the scratch directory cannot be made (the message names
     * workspacePath); or when the store already exists because a racing create made it.
     * @throws std::bad_alloc When memory is exhausted.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-1, UR-STG-9
     */
    static WorkspacePtr create(const char* workspacePath, const char* slidePath, int32_t sceneIndex,
                               const Configuration& configuration);

    /**
     * @brief Creates a workspace over a scene the caller has already opened.
     *
     * Binds the given scene instead of opening the slide file again, so a caller that
     * holds the slide open (the Python bindings) shares it. Everything else is that of
     * the path overload: the same layout, provenance (the absolute, normalised
     * slidePath and sceneIndex), undo of a failed create and ownership rules, so the
     * workspace reopens with either open(). scene() returns the given object.
     *
     * Checked before anything is made: the scene is not null, sceneIndex is not
     * negative, and the scene's file path is the same file as slidePath
     * (std::filesystem::equivalent). The messages carry no path.
     *
     * @par What is trusted
     * sceneIndex: a slideio::Scene cannot say which scene of its slide it is, so the
     * caller's index is recorded as given and not confirmed, nor checked against the
     * slide's scene count.
     * @param workspacePath UTF-8 path of the workspace directory; as for the path overload.
     * @param scene The open scene; shared, never reset by the workspace. close() releases
     * only the workspace's reference: the slide stays open as long as the caller keeps it.
     * @param slidePath UTF-8 path of the slide the scene was opened from.
     * @param sceneIndex The scene's index in its slide, from 0.
     * @param configuration The settings; copied.
     * @return The open, writable workspace.
     * @throws InvalidArgumentError When a path is null or empty, the scene is null,
     * sceneIndex is negative, or the scene is not of the file at slidePath (also when
     * that file is missing).
     * @throws IoError As for the path overload, except that the slide is not opened.
     * @throws std::bad_alloc When memory is exhausted.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-1, UR-STG-9
     */
    static WorkspacePtr create(const char* workspacePath, std::shared_ptr<slideio::Scene> scene,
                               const char* slidePath, int32_t sceneIndex, const Configuration& configuration);

    /**
     * @brief Opens an existing workspace for writing, in this or any later process.
     *
     * Takes the store's writer lock, reopens the slide from the provenance, then
     * clears the scratch directory and deletes the raster files no raster refers to
     * (RasterRegistry::removeUnreferencedFiles()), logging the bytes reclaimed at
     * LogLevel::Info. Both reclaim what a process that died mid-stage left behind.
     * @param workspacePath UTF-8 path of the workspace directory; copied.
     * @param configuration The settings; copied.
     * @return The open, writable workspace.
     * @throws InvalidArgumentError When workspacePath is null or empty.
     * @throws IoError When the path holds no workspace store, when another handle holds
     * the writer lock, when the slide no longer opens or no longer has the scene (the
     * message names the slide path), or when the scratch directory cannot be cleared.
     * @throws StoreFormatError When the store has another format version.
     * @throws std::bad_alloc When memory is exhausted.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-1, UR-STG-9
     */
    static WorkspacePtr open(const char* workspacePath, const Configuration& configuration);

    /**
     * @brief Opens an existing workspace for writing over a scene the caller has
     * already opened.
     *
     * As the path overload, including the scratch clear and the raster-file sweep, but
     * the slide file is not opened: the given scene is bound. The scene's file path must
     * be the same file (std::filesystem::equivalent) as the slide path the workspace
     * recorded, and sceneIndex must equal the recorded scene index; otherwise nothing is
     * changed (the store is closed again, no scratch or raster file touched). The
     * messages carry no path.
     *
     * @par What is trusted
     * Only the sceneIndex match is checked, never that the scene truly is that scene of
     * its slide, which a slideio::Scene cannot confirm.
     * @param workspacePath UTF-8 path of the workspace directory; copied.
     * @param scene The open scene; shared. close() releases only the workspace's reference.
     * @param sceneIndex The scene's index in its slide; must be the recorded one.
     * @param configuration The settings; copied.
     * @return The open, writable workspace.
     * @throws InvalidArgumentError When workspacePath is null or empty, the scene is null,
     * sceneIndex is negative, the scene is not of the recorded slide, or sceneIndex
     * differs from the recorded one.
     * @throws IoError When the path holds no workspace store, when another handle holds
     * the writer lock, or when the scratch directory cannot be cleared.
     * @throws StoreFormatError When the store has another format version.
     * @throws std::bad_alloc When memory is exhausted.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-1, UR-STG-9
     */
    static WorkspacePtr open(const char* workspacePath, std::shared_ptr<slideio::Scene> scene, int32_t sceneIndex,
                             const Configuration& configuration);

    /**
     * @brief Opens an existing workspace without a writer lock.
     *
     * Stage state and objects are readable, nothing may be run or written. Safe
     * alongside another process's writer; the scratch directory and the raster files
     * are left alone, since that writer may own them.
     * @param workspacePath UTF-8 path of the workspace directory; copied.
     * @param configuration The settings; copied.
     * @return The open, read-only workspace.
     * @throws InvalidArgumentError When workspacePath is null or empty.
     * @throws IoError When the path holds no workspace store, or when the slide no
     * longer opens or no longer has the scene (the message names the slide path).
     * @throws StoreFormatError When the store has another format version.
     * @throws std::bad_alloc When memory is exhausted.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-1, UR-OBS-1
     */
    static WorkspacePtr openReadOnly(const char* workspacePath, const Configuration& configuration);

    /** @brief Calls close(); an error is logged at LogLevel::Error and swallowed. */
    ~Workspace();
    Workspace(const Workspace&) = delete;             ///< Not copyable.
    Workspace& operator=(const Workspace&) = delete;  ///< Not copyable.

    /**
     * @brief Returns the workspace directory's path.
     * @return A null-terminated UTF-8 string as given to create() or open(), never
     * null, valid while this object lives. Answers after close().
     * @throws Nothing.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-9
     */
    const char* path() const noexcept;
    /**
     * @brief Returns the bound slide's path.
     * @return A null-terminated UTF-8 string, the absolute path stored in the
     * provenance, never null,
     * valid while this object lives. Answers after close().
     * @throws Nothing.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-1
     */
    const char* slidePath() const noexcept;
    /**
     * @brief Returns the bound scene's index in the slide.
     * @return The index. Answers after close().
     * @throws Nothing.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-1
     */
    int32_t sceneIndex() const noexcept;
    /**
     * @brief Tells whether the workspace was opened with openReadOnly().
     * @return True for openReadOnly(), false for create() and open(). Answers after
     * close().
     * @throws Nothing.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-OBS-1
     */
    bool isReadOnly() const noexcept;

    /**
     * @brief Returns the workspace's store.
     * @return The store, never null. After close() it is the closed store.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-1
     */
    StorePtr store() noexcept;
    /**
     * @brief Returns the bound scene.
     *
     * The workspace keeps the slide open as long as the scene. close() releases the
     * slide, so a scene obtained here must not be used after close(), even though the
     * shared pointer keeps the object alive: drop it before closing.
     * @return The scene; null after close().
     * @throws Nothing.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-1
     */
    std::shared_ptr<slideio::Scene> scene() noexcept;
    /**
     * @brief Returns the configuration the workspace was opened with.
     * @return The configuration, valid while this object lives.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-EXE-2
     */
    const Configuration& configuration() const noexcept;
    /**
     * @brief Returns configuration() with its scratch directory pointed at
     * "<workspace>/scratch".
     *
     * What a stage run hands to Pipeline, so seam spill files live inside the
     * workspace rather than beside it (UR-STG-9). Every other setting equals
     * configuration()'s.
     * @return The configuration, valid while this object lives.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-9
     */
    const Configuration& scratchConfiguration() const noexcept;
    /**
     * @brief Returns the workspace's one memory budget.
     *
     * Made once, with configuration().memoryCeilingBytes() as its ceiling, and handed
     * to every pipeline the workspace runs, so UR-EXE-2's ceiling spans them together.
     * The storage module takes no budget.
     * @return The same budget on every call, never null.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadSafe; the budget is ThreadSafe.
     * @par Requirements
     * UR-EXE-2
     */
    MemoryBudgetPtr memoryBudget() noexcept;

    /**
     * @brief Returns the store's layer registry.
     * @return store()->layers(), valid while this object lives.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-1
     */
    LayerRegistry& layers() noexcept;

    /**
     * @brief Returns the layer lifecycle, which declares and drops layers.
     * @return The lifecycle, owned by this workspace and valid while it lives.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-10, UR-STG-12
     */
    LayerLifecycle& layerLifecycle() noexcept;

    /**
     * @brief Returns the stage registry, which declares stages and derives their
     * dependencies.
     * @return The registry, owned by this workspace and valid while it lives.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-2, UR-STG-4
     */
    StageRegistry& stages() noexcept;

    /**
     * @brief Closes the store, releasing the writer lock, and releases the slide.
     *
     * A second call does nothing. A scene obtained from scene() must not be used
     * afterwards: the slide that owns it is released.
     * @throws IoError When closing the store fails; the workspace is closed even so.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-STG-1
     */
    void close();

private:
    // Library-internal: the run slot (run_slot.hpp) reaches its flag through these.
    friend bool detail::takeRunSlot(Workspace& workspace) noexcept;
    friend void detail::releaseRunSlot(Workspace& workspace) noexcept;
    class Impl;
    explicit Workspace(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace slideio::processor
