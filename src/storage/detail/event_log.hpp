// Requirements: UR-OBS-2, UR-OBS-3
#pragma once

// The internal journal writer (ruling S12). Every storage write appends its event
// through appendEvent, inside the caller's own write transaction.

#include "storage/detail/sqlite.hpp"

#include <slideio/processor/storage/types.hpp>

#include <cstdint>

namespace slideio::processor::detail {

// One journal row: every column but seq. A zero stage, layer or label is stored as NULL,
// as is a status when hasStageStatus is false. A zero timestampMs means "now".
struct EventRow {
    EventKind kind = EventKind::LayerDeclared;
    int32_t stageId = 0;
    int32_t layerId = 0;
    int32_t labelId = 0;
    int64_t objectCount = 0;
    // Half-open box [minX, maxX) x [minY, maxY); all zero for a kind that carries none.
    int64_t minX = 0;
    int64_t minY = 0;
    int64_t maxX = 0;
    int64_t maxY = 0;
    bool hasStageStatus = false;
    StageStatus stageStatus = StageStatus::Pending;
    int64_t timestampMs = 0;
};

// Inserts `row` into events on `writeConnection` and returns its sequence. It opens and
// commits no transaction: call it inside StoreImpl::withWriteTransaction, then call
// StoreImpl::notifyCommitted once the transaction has committed. Throws IoError.
int64_t appendEvent(sqlite3* writeConnection, const EventRow& row);

} // namespace slideio::processor::detail
