#include "storage/detail/object_lookup.hpp"

#include <vector>

namespace slideio::processor::detail {

bool findObjectRow(Statement& lookup, ObjectId id, ObjectRow& out) {
    if (!id.isValid()) return false;
    lookup.bindInt64(1, id.layer().value());
    lookup.bindInt64(2, id.anchorPacked());
    const bool found = lookup.step();
    if (found) {
        const int64_t minX = lookup.columnInt64(1);
        const int64_t minY = lookup.columnInt64(2);
        out.rowid = lookup.columnInt64(0);
        out.box = Rect64{minX, minY, lookup.columnInt64(3) - minX, lookup.columnInt64(4) - minY};
    }
    lookup.reset();
    return found;
}

int64_t deleteObjects(sqlite3* db, ObjectsOf which, int64_t key) {
    const std::string filter = which == ObjectsOf::Stage ? "stage_id = ?1" : "layer_id = ?1";
    const std::string rowids = "(SELECT rowid FROM objects WHERE " + filter + ")";
    int64_t removed = 0;
    {
        Statement count(db, ("SELECT COUNT(*) FROM objects WHERE " + filter).c_str());
        count.bindInt64(1, key);
        if (count.step()) removed = count.columnInt64(0);
    }
    std::vector<std::string> deletes = {"DELETE FROM label_assertions WHERE object_rowid IN " + rowids,
                                        "DELETE FROM measurement_assertions WHERE object_rowid IN " + rowids};
    if (which == ObjectsOf::Stage) {
        const std::string anchors = "(SELECT layer_id, anchor_id FROM objects WHERE " + filter + ")";
        deletes.push_back("DELETE FROM parent_edges WHERE child_rowid IN " + rowids + " OR parent_rowid IN " + rowids);
        // Keyed by ObjectId: a kept row would re-attach to a re-run's object at a
        // coincident anchor.
        deletes.push_back("DELETE FROM derived_from WHERE (derived_layer, derived_anchor) IN " + anchors +
                          " OR (primitive_layer, primitive_anchor) IN " + anchors);
    }
    deletes.push_back("DELETE FROM measurements WHERE object_rowid IN " + rowids);
    deletes.push_back("DELETE FROM object_rtree WHERE rowid IN " + rowids);
    deletes.push_back("DELETE FROM objects WHERE " + filter);
    for (const std::string& sql : deletes) {
        Statement remove(db, sql.c_str());
        remove.bindInt64(1, key);
        remove.run();
    }
    return removed;
}

std::string describeObject(sqlite3* db, ObjectId id) {
    std::string layer;
    {
        Statement select(db, "SELECT name FROM layers WHERE id = ?");
        select.bindInt64(1, id.layer().value());
        layer = select.step() ? select.columnText(0) : "layer " + std::to_string(id.layer().value());
    }
    const Point64 anchor = id.anchorPoint();
    return layer + "/(" + std::to_string(anchor.x) + ", " + std::to_string(anchor.y) + ")";
}

ObjectId objectIdOf(int64_t layer, int64_t anchorPacked) noexcept {
    return ObjectId::fromAnchor(LayerId(static_cast<int32_t>(layer)), Point64{anchorPacked & 0x7FFFFFFF, anchorPacked >> 31});
}

} // namespace slideio::processor::detail
