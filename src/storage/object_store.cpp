#include <slideio/processor/storage/object_store.hpp>

#include "storage/detail/event_log.hpp"
#include "storage/detail/query_builder.hpp"
#include "storage/detail/sqlite.hpp"
#include "storage/detail/store_impl.hpp"

#include <slideio/processor/core/error.hpp>

#include <string>
#include <utility>

namespace slideio::processor {

namespace {

[[noreturn]] void throwUnknownStage(const char* operation, StageId stage) {
    const std::string message = std::string(operation) + ": there is no stage " + std::to_string(stage.value());
    throw InvalidArgumentError(message.c_str());
}

bool stageExists(sqlite3* db, StageId stage) {
    detail::Statement select(db, "SELECT 1 FROM stages WHERE id = ?");
    select.bindInt64(1, stage.value());
    return select.step();
}

} // namespace

class ObjectStore::Impl {
public:
    explicit Impl(detail::StoreImpl& s) : store(s) {}
    detail::StoreImpl& store;
};

ObjectStore::ObjectStore(detail::StoreImpl& store) : impl_(std::make_unique<Impl>(store)) {}

ObjectStore::~ObjectStore() = default;

ObjectWriterPtr ObjectStore::writerForStage(StageId stage) {
    constexpr const char* kOperation = "ObjectStore::writerForStage";
    detail::StoreImpl& store = impl_->store;
    store.checkWritable(kOperation);
    const bool exists =
        store.withReadConnection(kOperation, [stage](sqlite3* db) { return stageExists(db, stage); });
    if (!exists) throwUnknownStage(kOperation, stage);
    return ObjectWriterPtr(new ObjectWriter(store.gate(), stage, store.configuration().objectBatchSize()));
}

CursorPtr ObjectStore::query(const ObjectQuery& query) {
    constexpr const char* kOperation = "ObjectStore::query";
    detail::StoreImpl& store = impl_->store;
    detail::BuiltQuery built = detail::buildObjectQuery(query, kOperation);
    detail::ReadLease lease = store.acquireRead(kOperation);
    return detail::openObjectCursor(store.path(), std::move(lease), std::move(built));
}

int64_t ObjectStore::count(const ObjectQuery& query) const {
    constexpr const char* kOperation = "ObjectStore::count";
    const detail::StoreImpl& store = impl_->store;
    detail::BuiltQuery built = detail::buildObjectQuery(query, kOperation);
    if (built.hasPolygon) {
        // The exact predicate runs in C++, so every candidate is read.
        return detail::countWithPolygon(store.path(), store.acquireRead(kOperation), std::move(built));
    }
    return store.withReadConnection(kOperation, [&built](sqlite3* db) {
        detail::Statement select(db, built.countSql.c_str());
        detail::bindValues(select, built.values);
        return select.step() ? select.columnInt64(0) : int64_t(0);
    });
}

bool ObjectStore::get(ObjectId id, ObjectRecord& out) const {
    constexpr const char* kOperation = "ObjectStore::get";
    const detail::StoreImpl& store = impl_->store;
    if (!id.isValid()) {
        store.checkOpen(kOperation);
        return false;
    }
    return store.withReadConnection(kOperation, [&](sqlite3* db) {
        // The object and its measurements from one snapshot; the pool rolls the
        // transaction back if anything below throws.
        detail::exec(db, "BEGIN");
        bool found = false;
        ObjectRecord record;
        {
            const std::string sql = std::string("SELECT ") + detail::kObjectColumns +
                                    " FROM objects o JOIN stages s ON s.id = o.stage_id "
                                    "WHERE o.layer_id = ? AND o.anchor_id = ? AND s.status = ?";
            detail::Statement select(db, sql.c_str());
            select.bindInt64(1, id.layer().value());
            select.bindInt64(2, id.anchorPacked());
            select.bindInt64(3, static_cast<int64_t>(StageStatus::Complete));
            if (select.step()) {
                Polygon geometry;
                detail::decodeRowGeometry(select, geometry);
                detail::Statement measurements(db, detail::kMeasurementSelect);
                record = detail::recordFromRow(select, geometry, measurements);
                found = true;
            }
        }
        detail::exec(db, "COMMIT");
        if (found) out = std::move(record);
        return found;
    });
}

void ObjectStore::discardStage(StageId stage) {
    constexpr const char* kOperation = "ObjectStore::discardStage";
    detail::StoreImpl& store = impl_->store;
    store.withWriteTransaction(kOperation, [&](sqlite3* db) {
        if (!stageExists(db, stage)) throwUnknownStage(kOperation, stage);
        int64_t removed = 0;
        {
            detail::Statement count(db, "SELECT COUNT(*) FROM objects WHERE stage_id = ?");
            count.bindInt64(1, stage.value());
            if (count.step()) removed = count.columnInt64(0);
        }
        // Measurements reference objects, so they go first; plan B adds edges and
        // annotations to the cascade.
        for (const char* sql : {"DELETE FROM measurements WHERE object_rowid IN "
                                "(SELECT rowid FROM objects WHERE stage_id = ?)",
                                "DELETE FROM object_rtree WHERE rowid IN (SELECT rowid FROM objects WHERE stage_id = ?)",
                                "DELETE FROM objects WHERE stage_id = ?"}) {
            detail::Statement remove(db, sql);
            remove.bindInt64(1, stage.value());
            remove.run();
        }
        detail::EventRow event;
        event.kind = EventKind::StageDiscarded;
        event.stageId = stage.value();
        event.objectCount = removed;
        detail::appendEvent(db, event);
    });
    store.notifyCommitted();
}

int64_t ObjectStore::totalObjectCount() const {
    return impl_->store.withReadConnection("ObjectStore::totalObjectCount", [](sqlite3* db) {
        detail::Statement count(db, "SELECT COUNT(*) FROM objects");
        return count.step() ? count.columnInt64(0) : int64_t(0);
    });
}

int64_t ObjectStore::objectCountInLayer(LayerId layer) const {
    return impl_->store.withReadConnection("ObjectStore::objectCountInLayer", [layer](sqlite3* db) {
        detail::Statement count(db, "SELECT COUNT(*) FROM objects WHERE layer_id = ?");
        count.bindInt64(1, layer.value());
        return count.step() ? count.columnInt64(0) : int64_t(0);
    });
}

} // namespace slideio::processor
