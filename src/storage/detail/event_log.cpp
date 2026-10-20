#include "storage/detail/event_log.hpp"

#include <chrono>

namespace slideio::processor::detail {

namespace {

void bindOptionalId(Statement& statement, int index, int32_t id) {
    if (id > 0) {
        statement.bindInt64(index, id);
    } else {
        statement.bindNull(index);
    }
}

} // namespace

int64_t appendEvent(sqlite3* writeConnection, const EventRow& row) {
    Statement insert(writeConnection,
                     "INSERT INTO events(kind, stage_id, layer_id, label_id, object_count, min_x, min_y, max_x, "
                     "max_y, stage_status, timestamp_ms) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    insert.bindInt64(1, static_cast<int64_t>(row.kind));
    bindOptionalId(insert, 2, row.stageId);
    bindOptionalId(insert, 3, row.layerId);
    bindOptionalId(insert, 4, row.labelId);
    insert.bindInt64(5, row.objectCount);
    insert.bindInt64(6, row.minX);
    insert.bindInt64(7, row.minY);
    insert.bindInt64(8, row.maxX);
    insert.bindInt64(9, row.maxY);
    if (row.hasStageStatus) {
        insert.bindInt64(10, static_cast<int64_t>(row.stageStatus));
    } else {
        insert.bindNull(10);
    }
    int64_t timestamp = row.timestampMs;
    if (timestamp == 0) {
        timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    }
    insert.bindInt64(11, timestamp);
    insert.run();
    return sqlite3_last_insert_rowid(writeConnection);
}

} // namespace slideio::processor::detail
