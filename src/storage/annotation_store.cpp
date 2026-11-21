#include <slideio/processor/storage/annotation_store.hpp>

#include "storage/detail/errors.hpp"
#include "storage/detail/event_log.hpp"
#include "storage/detail/object_lookup.hpp"
#include "storage/detail/query_builder.hpp"
#include "storage/detail/sqlite.hpp"
#include "storage/detail/store_impl.hpp"

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/storage/layer_registry.hpp>
#include <slideio/processor/storage/object_store.hpp>

#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace slideio::processor {

namespace {

constexpr int64_t kComplete = static_cast<int64_t>(StageStatus::Complete);

// The effective label set of the object (?1 layer, ?2 anchor) when its producing stage
// has status ?3 (ruling S23): the row label at rank 0, then each label asserted by a
// stage of status ?3 at rank 1, once, attributed to the smallest such stage. An asserted
// label equal to the row label is the row label's entry.
constexpr const char* kEffectiveLabels =
    "WITH obj AS (SELECT o.rowid AS object_rowid, o.label_id AS label_id, o.stage_id AS stage_id "
    "FROM objects o JOIN stages s ON s.id = o.stage_id "
    "WHERE o.layer_id = ?1 AND o.anchor_id = ?2 AND s.status = ?3), "
    "effective AS (SELECT 0 AS rank, label_id, stage_id FROM obj WHERE label_id IS NOT NULL "
    "UNION ALL SELECT 1, a.label_id, MIN(a.stage_id) FROM obj "
    "JOIN label_assertions a ON a.object_rowid = obj.object_rowid JOIN stages sa ON sa.id = a.stage_id "
    "WHERE sa.status = ?3 AND a.label_id IS NOT obj.label_id GROUP BY a.label_id) ";

// The measurements asserted on the object (?1 layer, ?2 anchor) when both its producing
// stage and the asserting stage have status ?3.
constexpr const char* kAssertedMeasurements =
    "WITH asserted AS (SELECT n.name AS name, m.value AS value, m.unit AS unit, m.stage_id AS stage_id "
    "FROM objects o JOIN stages s ON s.id = o.stage_id "
    "JOIN measurement_assertions m ON m.object_rowid = o.rowid JOIN stages sa ON sa.id = m.stage_id "
    "JOIN measurement_names n ON n.id = m.name_id "
    "WHERE o.layer_id = ?1 AND o.anchor_id = ?2 AND s.status = ?3 AND sa.status = ?3) ";

} // namespace

class AnnotationStore::Impl {
public:
    Impl(detail::StoreImpl& s, const LayerRegistry& l, ObjectStore& o) : store(s), layers(l), objects(o) {}

    // Runs `sql` (one of the WITH prefixes above plus a SELECT) for `object`, binding
    // ?1..?3, then `bindMore(statement)`, and returns read(db, statement). An invalid id
    // never matches: it only checks that the store is open, and read() sees no row.
    template <class BindMore, class Read>
    auto query(const char* operation, ObjectId object, const std::string& sql, BindMore&& bindMore,
               Read&& read) const {
        return store.withReadConnection(operation, [&](sqlite3* db) {
            detail::Statement select(db, sql.c_str());
            select.bindInt64(1, object.isValid() ? object.layer().value() : 0);
            select.bindInt64(2, object.anchorPacked());
            select.bindInt64(3, kComplete);
            bindMore(select);
            return read(db, select);
        });
    }

    // Runs `prefix` + "SELECT COUNT(*) FROM <table>" for `object`.
    int32_t count(const char* operation, ObjectId object, const char* prefix, const char* table) const {
        return query(operation, object, std::string(prefix) + "SELECT COUNT(*) FROM " + table,
                     [](detail::Statement&) {},
                     [](sqlite3*, detail::Statement& select) {
                         return select.step() ? static_cast<int32_t>(select.columnInt64(0)) : 0;
                     });
    }

    // Steps to the row at `index` of `prefix` + `select` (which ends in "LIMIT 1 OFFSET
    // ?4") and returns read(statement), or throws InvalidArgumentError for an index out
    // of range.
    template <class Read>
    auto at(const char* operation, ObjectId object, int32_t index, const char* prefix, const char* select,
            Read&& read) const {
        if (index < 0) throwOutOfRange(operation, object, index, nullptr);
        return query(operation, object, std::string(prefix) + select,
                     [index](detail::Statement& s) { s.bindInt64(4, index); },
                     [&](sqlite3* db, detail::Statement& s) {
                         if (!s.step()) throwOutOfRange(operation, object, index, db);
                         return read(s);
                     });
    }

    [[noreturn]] static void throwOutOfRange(const char* operation, ObjectId object, int32_t index, sqlite3* db) {
        const std::string what = object.isValid() && db != nullptr ? "object " + detail::describeObject(db, object)
                                                                   : std::string("the object");
        detail::throwInvalid(operation, "index " + std::to_string(index) + " is out of range for " + what);
    }

    // A copy of `name` that stays valid until the Store is destroyed, for a returned
    // Measurement. Names are pooled once each; the store's measurement names are
    // append-only, so the pool is bounded by them.
    const char* pooledName(std::string name) const {
        std::lock_guard<std::mutex> guard(poolMutex);
        return namePool.insert(std::move(name)).first->c_str();
    }

    int64_t total(const char* operation, const char* sql) const {
        return store.withReadConnection(operation, [sql](sqlite3* db) {
            detail::Statement count(db, sql);
            return count.step() ? count.columnInt64(0) : int64_t(0);
        });
    }

    detail::StoreImpl& store;
    const LayerRegistry& layers;
    ObjectStore& objects;  // the source of core's Membership and ObjectAnchor rasters
    mutable std::mutex poolMutex;
    mutable std::set<std::string> namePool;
    // The (stage, name) pairs the most recent assertedMeasurementColumnCount() read.
    mutable std::mutex columnsMutex;
    mutable std::vector<std::pair<StageId, const char*>> assertedColumns;
};

AnnotationStore::AnnotationStore(detail::StoreImpl& store, const LayerRegistry& layers, ObjectStore& objects)
    : impl_(std::make_unique<Impl>(store, layers, objects)) {}

AnnotationStore::~AnnotationStore() = default;

AnnotationWriterPtr AnnotationStore::writerForStage(StageId stage) {
    constexpr const char* kOperation = "AnnotationStore::writerForStage";
    detail::StoreImpl& store = impl_->store;
    store.checkWritable(kOperation);
    const bool exists =
        store.withReadConnection(kOperation, [stage](sqlite3* db) { return detail::stageExists(db, stage); });
    if (!exists) detail::throwUnknownStage(kOperation, stage);
    return AnnotationWriterPtr(new AnnotationWriter(store.gate(), stage, store.configuration().objectBatchSize()));
}

int32_t AnnotationStore::labelCount(ObjectId object) const {
    return impl_->count("AnnotationStore::labelCount", object, kEffectiveLabels, "effective");
}

LabelId AnnotationStore::labelAt(ObjectId object, int32_t index) const {
    return impl_->at("AnnotationStore::labelAt", object, index, kEffectiveLabels,
                     "SELECT label_id FROM effective ORDER BY rank, label_id LIMIT 1 OFFSET ?4",
                     [](detail::Statement& s) { return LabelId(static_cast<int32_t>(s.columnInt64(0))); });
}

StageId AnnotationStore::labelAssertedBy(ObjectId object, int32_t index) const {
    return impl_->at("AnnotationStore::labelAssertedBy", object, index, kEffectiveLabels,
                     "SELECT stage_id FROM effective ORDER BY rank, label_id LIMIT 1 OFFSET ?4",
                     [](detail::Statement& s) { return StageId(static_cast<int32_t>(s.columnInt64(0))); });
}

int32_t AnnotationStore::assertedMeasurementCount(ObjectId object) const {
    return impl_->count("AnnotationStore::assertedMeasurementCount", object, kAssertedMeasurements, "asserted");
}

Measurement AnnotationStore::assertedMeasurementAt(ObjectId object, int32_t index) const {
    return impl_->at("AnnotationStore::assertedMeasurementAt", object, index, kAssertedMeasurements,
                     "SELECT name, value, unit FROM asserted ORDER BY name, stage_id LIMIT 1 OFFSET ?4",
                     [this](detail::Statement& s) {
                         return Measurement::restored(impl_->pooledName(s.columnText(0)), s.columnDouble(1),
                                                      static_cast<MeasurementUnit>(s.columnInt64(2)));
                     });
}

StageId AnnotationStore::measurementAssertedBy(ObjectId object, int32_t index) const {
    return impl_->at("AnnotationStore::measurementAssertedBy", object, index, kAssertedMeasurements,
                     "SELECT stage_id FROM asserted ORDER BY name, stage_id LIMIT 1 OFFSET ?4",
                     [](detail::Statement& s) { return StageId(static_cast<int32_t>(s.columnInt64(0))); });
}

bool AnnotationStore::assertedMeasurement(ObjectId object, const char* name, StageId stage, Measurement& out) const {
    constexpr const char* kOperation = "AnnotationStore::assertedMeasurement";
    if (name == nullptr) {
        impl_->store.checkOpen(kOperation);
        return false;
    }
    return impl_->query(
        kOperation, object,
        std::string(kAssertedMeasurements) + "SELECT value, unit FROM asserted WHERE name = ?4 AND stage_id = ?5",
        [&](detail::Statement& s) {
            s.bindText(4, name);
            s.bindInt64(5, stage.value());
        },
        [&](sqlite3*, detail::Statement& s) {
            if (!s.step()) return false;
            out = Measurement::restored(impl_->pooledName(name), s.columnDouble(0),
                                        static_cast<MeasurementUnit>(s.columnInt64(1)));
            return true;
        });
}

void AnnotationStore::discardAnnotationsOfStage(StageId stage) {
    constexpr const char* kOperation = "AnnotationStore::discardAnnotationsOfStage";
    detail::StoreImpl& store = impl_->store;
    store.withWriteTransaction(kOperation, [&](sqlite3* db) {
        if (!detail::stageExists(db, stage)) detail::throwUnknownStage(kOperation, stage);
        for (const char* sql :
             {"DELETE FROM label_assertions WHERE stage_id = ?", "DELETE FROM measurement_assertions WHERE stage_id = ?"}) {
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

int64_t AnnotationStore::labelAnnotationCount() const {
    return impl_->total("AnnotationStore::labelAnnotationCount", "SELECT COUNT(*) FROM label_assertions");
}

int64_t AnnotationStore::measurementAnnotationCount() const {
    return impl_->total("AnnotationStore::measurementAnnotationCount", "SELECT COUNT(*) FROM measurement_assertions");
}

int32_t AnnotationStore::assertedMeasurementColumnCount() const {
    constexpr const char* kOperation = "AnnotationStore::assertedMeasurementColumnCount";
    // One scan of measurement_assertions gives every pair; At() reads the snapshot.
    std::vector<std::pair<StageId, std::string>> rows;
    impl_->store.withReadConnection(kOperation, [&](sqlite3* db) {
        detail::Statement select(db,
                                 "SELECT m.stage_id, n.name FROM (SELECT DISTINCT stage_id, name_id "
                                 "FROM measurement_assertions) m JOIN measurement_names n ON n.id = m.name_id "
                                 "ORDER BY m.stage_id, n.name");
        while (select.step())
            rows.emplace_back(StageId(static_cast<int32_t>(select.columnInt64(0))), select.columnText(1));
        return true;
    });
    std::vector<std::pair<StageId, const char*>> snapshot;
    snapshot.reserve(rows.size());
    for (auto& [stage, name] : rows) snapshot.emplace_back(stage, impl_->pooledName(std::move(name)));
    std::lock_guard<std::mutex> guard(impl_->columnsMutex);
    impl_->assertedColumns = std::move(snapshot);
    // Safe: the pairs are far fewer than 2^31 (stages times distinct names).
    return static_cast<int32_t>(impl_->assertedColumns.size());
}

void AnnotationStore::assertedMeasurementColumnAt(int32_t index, StageId& stage, const char*& name) const {
    constexpr const char* kOperation = "AnnotationStore::assertedMeasurementColumnAt";
    impl_->store.checkOpen(kOperation);
    std::lock_guard<std::mutex> guard(impl_->columnsMutex);
    if (index < 0 || static_cast<size_t>(index) >= impl_->assertedColumns.size()) {
        const std::string message =
            std::string(kOperation) + ": index " + std::to_string(index) + " is out of range";
        throw InvalidArgumentError(message.c_str());
    }
    stage = impl_->assertedColumns[static_cast<size_t>(index)].first;
    name = impl_->assertedColumns[static_cast<size_t>(index)].second;
}

TileRasterSourcePtr AnnotationStore::layerRasterSource(LayerId layer, LayerRasterContent content) const {
    constexpr const char* kOperation = "AnnotationStore::layerRasterSource";
    detail::StoreImpl& store = impl_->store;
    store.checkOpen(kOperation);
    if (!layer.isValid() || !impl_->layers.exists(layer)) {
        detail::throwInvalid(kOperation, "layer " + std::to_string(layer.value()) + " is unknown");
    }
    if (layer == impl_->layers.pixelLayer()) {
        detail::throwInvalid(kOperation, "the pixel layer holds no objects");
    }
    switch (content) {
    case LayerRasterContent::Label:
        return detail::makeLabelRasterSource(store.gate(), layer);
    case LayerRasterContent::Membership:
    case LayerRasterContent::ObjectAnchor:
        // Neither reads a label, so core's source over the object store is the answer (S15).
        return makeLayerRasterSource(impl_->objects, layer, content);
    }
    detail::throwInvalid(kOperation,
                         "content " + std::to_string(static_cast<int32_t>(content)) + " is not a LayerRasterContent");
}

} // namespace slideio::processor
