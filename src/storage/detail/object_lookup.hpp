// Requirements: UR-RES-5, UR-RES-16
#pragma once

// Resolving an ObjectId to its stored row, for the writers that reference objects
// committed by others (edges, annotations): they check existence inside their batch
// transaction (ruling S19), whatever the object's stage status.

#include "storage/detail/sqlite.hpp"

#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/core/ids.hpp>

#include <cstdint>
#include <string>

namespace slideio::processor::detail {

// Resolves an object through the UNIQUE(layer_id, anchor_id) index. findObjectRow binds
// it; prepare it once per batch.
inline constexpr const char* kObjectLookupSql =
    "SELECT rowid, min_x, min_y, max_x, max_y FROM objects WHERE layer_id = ? AND anchor_id = ?";

// One stored object: its rowid and its bounding box.
struct ObjectRow {
    int64_t rowid;
    Rect64 box;
};

// Looks `id` up through `lookup` (kObjectLookupSql) and fills `out` when found; `out`
// is unchanged otherwise. Resets `lookup` either way. Returns false for an invalid id.
// Throws IoError.
bool findObjectRow(Statement& lookup, ObjectId id, ObjectRow& out);

// Returns "<layer name>/(<x>, <y>)" for messages: the layer's name (or "layer <n>"
// when the store holds no such layer) and the anchor. Never names file content.
// Throws IoError, std::bad_alloc.
std::string describeObject(sqlite3* db, ObjectId id);

// Rebuilds the ObjectId of a stored (layer_id, anchor_id) pair.
ObjectId objectIdOf(int64_t layer, int64_t anchorPacked) noexcept;

} // namespace slideio::processor::detail
