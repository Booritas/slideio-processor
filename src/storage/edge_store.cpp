#include <slideio/processor/storage/edge_store.hpp>

#include "storage/detail/errors.hpp"
#include "storage/detail/event_log.hpp"
#include "storage/detail/object_lookup.hpp"
#include "storage/detail/query_builder.hpp"
#include "storage/detail/sqlite.hpp"
#include "storage/detail/store_impl.hpp"

#include <slideio/processor/core/error.hpp>

#include <string>
#include <utility>
#include <vector>

namespace slideio::processor {

namespace {

constexpr int64_t kComplete = static_cast<int64_t>(StageStatus::Complete);

detail::QueryValue int64Value(int64_t v) {
    detail::QueryValue value;
    value.integer = v;
    return value;
}

// The objects visible under CompletedOnly whose rowid or (layer, anchor) the subquery
// `selection` yields, in ascending ObjectId order (ruling S20). `values` are bound to the
// subquery's parameters, after the object's own status.
detail::BuiltQuery visibleObjects(const char* selection, std::vector<detail::QueryValue> values) {
    detail::BuiltQuery built;
    built.rowsSql = std::string("SELECT ") + detail::kObjectColumns +
                    " FROM objects o JOIN stages s ON s.id = o.stage_id WHERE s.status = ? AND " + selection +
                    " ORDER BY o.layer_id, o.anchor_id";
    built.values.push_back(int64Value(kComplete));
    for (detail::QueryValue& v : values) built.values.push_back(std::move(v));
    return built;
}

} // namespace

class EdgeStore::Impl {
public:
    Impl(detail::StoreImpl& s, const LayerRegistry& l) : store(s), layers(l) {}

    CursorPtr open(const char* operation, detail::BuiltQuery&& built) const {
        detail::ReadLease lease = store.acquireRead(operation);
        return detail::openObjectCursor(std::move(lease), std::move(built));
    }

    int64_t count(const char* operation, const char* sql) const {
        return store.withReadConnection(operation, [sql](sqlite3* db) {
            detail::Statement count(db, sql);
            return count.step() ? count.columnInt64(0) : int64_t(0);
        });
    }

    detail::StoreImpl& store;
    const LayerRegistry& layers;  // handed to writers, which reach it through the gate
};

EdgeStore::EdgeStore(detail::StoreImpl& store, const LayerRegistry& layers)
    : impl_(std::make_unique<Impl>(store, layers)) {}

EdgeStore::~EdgeStore() = default;

EdgeWriterPtr EdgeStore::writerForStage(StageId stage) {
    constexpr const char* kOperation = "EdgeStore::writerForStage";
    detail::StoreImpl& store = impl_->store;
    store.checkWritable(kOperation);
    const bool exists =
        store.withReadConnection(kOperation, [stage](sqlite3* db) { return detail::stageExists(db, stage); });
    if (!exists) detail::throwUnknownStage(kOperation, stage);
    return EdgeWriterPtr(
        new EdgeWriter(store.gate(), impl_->layers, stage, store.configuration().objectBatchSize()));
}

bool EdgeStore::parentOf(ObjectId child, ObjectId& out) const {
    constexpr const char* kOperation = "EdgeStore::parentOf";
    const detail::StoreImpl& store = impl_->store;
    if (!child.isValid()) {
        store.checkOpen(kOperation);
        return false;
    }
    return store.withReadConnection(kOperation, [&](sqlite3* db) {
        // The edge alone decides; the parent object's own visibility is not consulted.
        detail::Statement select(db,
                                 "SELECT p.layer_id, p.anchor_id FROM objects c "
                                 "JOIN parent_edges e ON e.child_rowid = c.rowid "
                                 "JOIN stages se ON se.id = e.stage_id "
                                 "JOIN objects p ON p.rowid = e.parent_rowid "
                                 "WHERE c.layer_id = ? AND c.anchor_id = ? AND se.status = ?");
        select.bindInt64(1, child.layer().value());
        select.bindInt64(2, child.anchorPacked());
        select.bindInt64(3, kComplete);
        if (!select.step()) return false;
        out = detail::objectIdOf(select.columnInt64(0), select.columnInt64(1));
        return true;
    });
}

CursorPtr EdgeStore::childrenOf(ObjectId parent) const {
    return impl_->open("EdgeStore::childrenOf",
                       visibleObjects("o.rowid IN (SELECT e.child_rowid FROM parent_edges e "
                                      "JOIN stages se ON se.id = e.stage_id "
                                      "JOIN objects p ON p.rowid = e.parent_rowid "
                                      "WHERE p.layer_id = ? AND p.anchor_id = ? AND se.status = ?)",
                                      {int64Value(parent.layer().value()), int64Value(parent.anchorPacked()),
                                       int64Value(kComplete)}));
}

bool EdgeStore::isAttached(ObjectId child) const {
    constexpr const char* kOperation = "EdgeStore::isAttached";
    const detail::StoreImpl& store = impl_->store;
    if (!child.isValid()) {
        store.checkOpen(kOperation);
        return false;
    }
    return store.withReadConnection(kOperation, [&](sqlite3* db) {
        detail::Statement select(db,
                                 "SELECT 1 FROM objects c JOIN parent_edges e ON e.child_rowid = c.rowid "
                                 "JOIN stages se ON se.id = e.stage_id "
                                 "WHERE c.layer_id = ? AND c.anchor_id = ? AND se.status = ?");
        select.bindInt64(1, child.layer().value());
        select.bindInt64(2, child.anchorPacked());
        select.bindInt64(3, kComplete);
        return select.step();
    });
}

CursorPtr EdgeStore::originsOf(ObjectId derived) const {
    // An inner join on objects: the primitives of a dropped layer yield nothing.
    return impl_->open("EdgeStore::originsOf",
                       visibleObjects("(o.layer_id, o.anchor_id) IN (SELECT d.primitive_layer, d.primitive_anchor "
                                      "FROM derived_from d JOIN stages sd ON sd.id = d.stage_id "
                                      "WHERE d.derived_layer = ? AND d.derived_anchor = ? AND sd.status = ?)",
                                      {int64Value(derived.layer().value()), int64Value(derived.anchorPacked()),
                                       int64Value(kComplete)}));
}

CursorPtr EdgeStore::derivedFrom(ObjectId primitive) const {
    // An inner join on the primitive, as originsOf: a dropped primitive yields nothing,
    // though its derived-from rows are kept (ruling S16).
    return impl_->open("EdgeStore::derivedFrom",
                       visibleObjects("(o.layer_id, o.anchor_id) IN (SELECT d.derived_layer, d.derived_anchor "
                                      "FROM derived_from d JOIN stages sd ON sd.id = d.stage_id "
                                      "JOIN objects p ON p.layer_id = d.primitive_layer "
                                      "AND p.anchor_id = d.primitive_anchor "
                                      "WHERE d.primitive_layer = ? AND d.primitive_anchor = ? AND sd.status = ?)",
                                      {int64Value(primitive.layer().value()), int64Value(primitive.anchorPacked()),
                                       int64Value(kComplete)}));
}

void EdgeStore::discardEdgesOfStage(StageId stage) {
    constexpr const char* kOperation = "EdgeStore::discardEdgesOfStage";
    detail::StoreImpl& store = impl_->store;
    store.withWriteTransaction(kOperation, [&](sqlite3* db) {
        if (!detail::stageExists(db, stage)) detail::throwUnknownStage(kOperation, stage);
        for (const char* sql : {"DELETE FROM parent_edges WHERE stage_id = ?", "DELETE FROM derived_from WHERE stage_id = ?"}) {
            detail::Statement remove(db, sql);
            remove.bindInt64(1, stage.value());
            remove.run();
        }
        // No object is removed; the event tells observers of the retraction (ruling S25).
        detail::EventRow event;
        event.kind = EventKind::StageDiscarded;
        event.stageId = stage.value();
        event.objectCount = 0;
        detail::appendEvent(db, event);
    });
    store.notifyCommitted();
}

int64_t EdgeStore::parentEdgeCount() const {
    return impl_->count("EdgeStore::parentEdgeCount", "SELECT COUNT(*) FROM parent_edges");
}

int64_t EdgeStore::derivedFromEdgeCount() const {
    return impl_->count("EdgeStore::derivedFromEdgeCount", "SELECT COUNT(*) FROM derived_from");
}

} // namespace slideio::processor
