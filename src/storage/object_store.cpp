#include <slideio/processor/storage/object_store.hpp>

#include "storage/detail/errors.hpp"
#include "storage/detail/event_log.hpp"
#include "storage/detail/object_lookup.hpp"
#include "storage/detail/query_builder.hpp"
#include "storage/detail/sqlite.hpp"
#include "storage/detail/store_impl.hpp"

#include <slideio/processor/core/error.hpp>

#include <mutex>
#include <set>
#include <string>
#include <utility>

namespace slideio::processor {

class ObjectStore::Impl {
public:
    explicit Impl(detail::StoreImpl& s) : store(s) {}
    // Keeps a copy of `text` alive for the life of the store and returns it. Equal values
    // share one node; std::set nodes never move, so every pointer handed out stays valid.
    const char* intern(std::string text) {
        std::lock_guard<std::mutex> guard(mutex);
        return pool.insert(std::move(text)).first->c_str();
    }

    detail::StoreImpl& store;
    std::mutex mutex;
    std::set<std::string> pool;
};

ObjectStore::ObjectStore(detail::StoreImpl& store) : impl_(std::make_unique<Impl>(store)) {}

ObjectStore::~ObjectStore() = default;

ObjectWriterPtr ObjectStore::writerForStage(StageId stage) {
    constexpr const char* kOperation = "ObjectStore::writerForStage";
    detail::StoreImpl& store = impl_->store;
    store.checkWritable(kOperation);
    const bool exists =
        store.withReadConnection(kOperation, [stage](sqlite3* db) { return detail::stageExists(db, stage); });
    if (!exists) detail::throwUnknownStage(kOperation, stage);
    return ObjectWriterPtr(new ObjectWriter(store.gate(), stage, store.configuration().objectBatchSize()));
}

CursorPtr ObjectStore::query(const ObjectQuery& query) {
    constexpr const char* kOperation = "ObjectStore::query";
    detail::StoreImpl& store = impl_->store;
    detail::BuiltQuery built = detail::buildObjectQuery(query, kOperation);
    detail::ReadLease lease = store.acquireRead(kOperation);
    return detail::openObjectCursor(std::move(lease), std::move(built));
}

int64_t ObjectStore::count(const ObjectQuery& query) const {
    constexpr const char* kOperation = "ObjectStore::count";
    const detail::StoreImpl& store = impl_->store;
    detail::BuiltQuery built = detail::buildObjectQuery(query, kOperation);
    if (built.hasPolygon) {
        // The exact predicate runs in C++, so every candidate is read.
        return detail::countWithPolygon(store.acquireRead(kOperation), std::move(built));
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
        if (!detail::stageExists(db, stage)) detail::throwUnknownStage(kOperation, stage);
        // Everything about the stage's objects, by any stage, goes with them (ruling S25).
        const int64_t removed = detail::deleteObjects(db, detail::ObjectsOf::Stage, stage.value());
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

int32_t ObjectStore::measurementNameCount() const {
    return impl_->store.withReadConnection("ObjectStore::measurementNameCount", [](sqlite3* db) {
        detail::Statement count(db, "SELECT COUNT(*) FROM measurement_names");
        // Safe: measurement names are far fewer than 2^31 (one row per distinct name).
        return count.step() ? static_cast<int32_t>(count.columnInt64(0)) : 0;
    });
}

const char* ObjectStore::measurementNameAt(int32_t index) const {
    constexpr const char* kOperation = "ObjectStore::measurementNameAt";
    std::string name;
    const bool found = impl_->store.withReadConnection(kOperation, [&](sqlite3* db) {
        if (index < 0) return false;
        detail::Statement select(db, "SELECT name FROM measurement_names ORDER BY name LIMIT 1 OFFSET ?");
        select.bindInt64(1, index);
        if (!select.step()) return false;
        name = select.columnText(0);
        return true;
    });
    if (!found) {
        const std::string message =
            std::string(kOperation) + ": index " + std::to_string(index) + " is out of range";
        throw InvalidArgumentError(message.c_str());
    }
    return impl_->intern(std::move(name));
}

} // namespace slideio::processor
