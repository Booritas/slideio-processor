#include "storage/detail/object_lookup.hpp"

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
