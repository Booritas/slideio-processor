#pragma once

/**
 * @file types.hpp
 * @brief Enumerations of the storage module.
 *
 * Every enumerator value is persisted in the store file: never renumber, and
 * append new enumerators only at the end.
 *
 * Thread-safety: not applicable (declarations only).
 */

#include <cstdint>

namespace slideio::processor {

/** @brief How Store::open treats the store file and the writer lock. */
enum class OpenMode : int32_t {
    ReadOnly,   ///< No writer; safe alongside another process's writer.
    ReadWrite,  ///< Exclusive writer; readers may attach concurrently.
    CreateNew   ///< Creates the store; fails if the path exists.
};

/** @brief Whether a layer holds semantic objects or primitives. */
enum class LayerKind : int32_t {
    Semantic,   ///< A layer in the semantic hierarchy.
    Primitive   ///< A layer outside the hierarchy.
};

/** @brief Where a vocabulary label came from. */
enum class LabelOrigin : int32_t {
    Standard,          ///< The library's standard vocabulary.
    WorkspaceLocal,    ///< Added to this workspace only.
    ImportRegistered   ///< Registered by a GeoJSON import.
};

/** @brief How a GeoJSON import treats labels the vocabulary lacks. */
enum class ImportPolicy : int32_t {
    Strict,      ///< Unknown labels are rejected.
    Permissive   ///< Unknown labels are registered.
};

/** @brief The lifecycle state of a stage. */
enum class StageStatus : int32_t {
    Pending,    ///< Declared, not started.
    Running,    ///< In progress.
    Complete,   ///< Finished successfully.
    Failed      ///< Finished unsuccessfully.
};

/** @brief The kind of a record in the event journal. */
enum class EventKind : int32_t {
    LayerDeclared,      ///< A layer was declared.
    ObjectsAdded,       ///< A batch of objects was committed.
    EdgesAsserted,      ///< A batch of parent edges was committed.
    ObjectsAnnotated,   ///< Label or measurement assertions were committed.
    LabelAdded,         ///< A label was added to the vocabulary.
    StageStateChanged,  ///< A stage changed status.
    StageDiscarded,     ///< A stage's output was discarded.
    LayerDropped,       ///< A layer was dropped.
    RasterDeclared,     ///< A raster was declared.
    RasterWritten,      ///< A raster block was written.
    RasterDropped       ///< A raster was dropped.
};

}  // namespace slideio::processor
