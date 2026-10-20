#include <slideio/processor/storage/object_store.hpp>

#include "storage/detail/event_log.hpp"
#include "storage/detail/geometry_codec.hpp"
#include "storage/detail/query_builder.hpp"
#include "storage/detail/schema.hpp"
#include "storage/detail/sqlite.hpp"
#include "storage/detail/store_impl.hpp"

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/log.hpp>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace slideio::processor {

namespace {

[[noreturn]] void throwInvalid(const char* operation, const std::string& what) {
    const std::string message = std::string(operation) + ": " + what;
    throw InvalidArgumentError(message.c_str());
}

std::string anchorText(Point64 anchor) {
    return "(" + std::to_string(anchor.x) + ", " + std::to_string(anchor.y) + ")";
}

struct PendingMeasurement {
    std::string name;
    double value = 0.0;
    int64_t unit = 0;
};

// One accepted record, ready to insert.
struct PendingObject {
    int32_t layer = 0;
    int64_t anchor = 0;  // ObjectId::anchorPacked()
    Point64 anchorPoint{0, 0};
    int32_t label = 0;  // 0: no label
    Rect64 box{0, 0, 0, 0};
    std::vector<uint8_t> geometry;
    std::vector<PendingMeasurement> measurements;
};

// One layer's share of a batch: its ObjectsAdded count and union bounding box.
struct LayerSummary {
    int64_t count = 0;
    int64_t minX = 0, minY = 0, maxX = 0, maxY = 0;
};

// The state of one ObjectWriter, shared with the store gate so that Store::close() can
// commit what it holds pending (SDS 03 §3.2).
class WriterCore final : public detail::GateClient {
public:
    WriterCore(std::shared_ptr<detail::StoreGate> g, StageId s, int64_t batch)
        : gate(std::move(g)), stage(s), batchSize(std::max<int64_t>(1, batch)) {}

    // Returns the layer's name, after checking it may hold objects. Validated layers are
    // cached: a layer is never removed, and a drop is re-checked at commit. Caller holds
    // `mutex`.
    const std::string& requireLayer(const char* operation, LayerId layer) {
        const auto cached = layerNames.find(layer.value());
        if (cached != layerNames.end()) return cached->second;
        if (layer.value() == detail::kPixelLayerId) {
            throwInvalid(operation, "the pixel layer holds no objects; emit into a declared layer");
        }
        struct Row {
            bool found = false;
            std::string name;
            bool dropped = false;
        };
        const Row row = gate->with(operation, [&](detail::StoreImpl& store) {
            return store.withReadConnection(operation, [&](sqlite3* db) {
                detail::Statement select(db, "SELECT name, dropped FROM layers WHERE id = ?");
                select.bindInt64(1, layer.value());
                Row r;
                if (select.step()) {
                    r.found = true;
                    r.name = select.columnText(0);
                    r.dropped = select.columnInt64(1) != 0;
                }
                return r;
            });
        });
        if (!row.found) throwInvalid(operation, "there is no layer " + std::to_string(layer.value()));
        if (row.dropped) throwInvalid(operation, "layer '" + row.name + "' is dropped");
        return layerNames.emplace(layer.value(), row.name).first->second;
    }

    // Checks that the store holds the label. Labels are never removed, so held ones are
    // cached. Caller holds `mutex`.
    void requireLabel(const char* operation, LabelId label) {
        if (heldLabels.count(label.value()) != 0) return;
        const bool held = gate->with(operation, [&](detail::StoreImpl& store) {
            return store.withReadConnection(operation, [&](sqlite3* db) {
                detail::Statement select(db, "SELECT 1 FROM labels WHERE id = ?");
                select.bindInt64(1, label.value());
                return select.step();
            });
        });
        if (!held) throwInvalid(operation, "the store holds no label " + std::to_string(label.value()));
        heldLabels.insert(label.value());
    }

    // Commits every pending record in one transaction (SDS 03 §3.5, rulings S8, S9). The
    // batch leaves `pending` first, so a failed commit drops it. Caller holds `mutex`.
    void commitLocked(const char* operation) {
        if (pending.empty()) return;
        std::vector<PendingObject> batch;
        batch.swap(pending);
        pendingKeys.clear();
        gate->with(operation, [&](detail::StoreImpl& store) {
            store.withWriteTransaction(operation, [&](sqlite3* db) { insertBatch(db, operation, batch); });
            store.notifyCommitted();
        });
        committed += static_cast<int64_t>(batch.size());
    }

    void insertBatch(sqlite3* db, const char* operation, const std::vector<PendingObject>& batch) {
        detail::Statement insertObject(db,
                                       "INSERT INTO objects(layer_id, anchor_id, stage_id, label_id, min_x, min_y, "
                                       "max_x, max_y, geometry) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
        detail::Statement insertBox(db, "INSERT INTO object_rtree(rowid, min_x, max_x, min_y, max_y) VALUES (?, ?, ?, ?, ?)");
        detail::Statement internName(db, "INSERT OR IGNORE INTO measurement_names(name) VALUES (?)");
        detail::Statement selectName(db, "SELECT id FROM measurement_names WHERE name = ?");
        detail::Statement insertMeasurement(
            db, "INSERT INTO measurements(object_rowid, ordinal, name_id, value, unit) VALUES (?, ?, ?, ?, ?)");
        std::unordered_map<std::string, int64_t> nameIds;
        std::map<int32_t, LayerSummary> layers;  // ascending layer id: the event order

        for (const PendingObject& o : batch) {
            insertObject.bindInt64(1, o.layer);
            insertObject.bindInt64(2, o.anchor);
            insertObject.bindInt64(3, stage.value());
            if (o.label != 0) {
                insertObject.bindInt64(4, o.label);
            } else {
                insertObject.bindNull(4);
            }
            insertObject.bindInt64(5, o.box.x);
            insertObject.bindInt64(6, o.box.y);
            insertObject.bindInt64(7, o.box.right());
            insertObject.bindInt64(8, o.box.bottom());
            insertObject.bindBlob(9, o.geometry.data(), o.geometry.size());
            const int rc = sqlite3_step(insertObject.get());
            if (rc != SQLITE_DONE) {
                if (sqlite3_extended_errcode(db) == SQLITE_CONSTRAINT_UNIQUE) {
                    throwInvalid(operation, "layer '" + layerNames.at(o.layer) + "' already holds an object with anchor " +
                                                anchorText(o.anchorPoint) +
                                                "; objects in one layer may not share an anchor. The batch of " +
                                                std::to_string(batch.size()) + " objects was rolled back");
                }
                detail::throwSqliteError(db, rc, operation);
            }
            insertObject.reset();
            const int64_t rowid = sqlite3_last_insert_rowid(db);

            insertBox.bindInt64(1, rowid);
            insertBox.bindDouble(2, detail::floatDown(o.box.x));
            insertBox.bindDouble(3, detail::floatUp(o.box.right()));
            insertBox.bindDouble(4, detail::floatDown(o.box.y));
            insertBox.bindDouble(5, detail::floatUp(o.box.bottom()));
            insertBox.run();
            insertBox.reset();

            int64_t ordinal = 0;
            for (const PendingMeasurement& m : o.measurements) {
                auto id = nameIds.find(m.name);
                if (id == nameIds.end()) {
                    internName.bindText(1, m.name);
                    internName.run();
                    internName.reset();
                    selectName.bindText(1, m.name);
                    if (!selectName.step()) throw InternalError("ObjectWriter: an interned measurement name vanished");
                    id = nameIds.emplace(m.name, selectName.columnInt64(0)).first;
                    selectName.reset();
                }
                insertMeasurement.bindInt64(1, rowid);
                insertMeasurement.bindInt64(2, ordinal++);
                insertMeasurement.bindInt64(3, id->second);
                insertMeasurement.bindDouble(4, m.value);
                insertMeasurement.bindInt64(5, m.unit);
                insertMeasurement.run();
                insertMeasurement.reset();
            }

            LayerSummary& summary = layers[o.layer];
            if (summary.count == 0) {
                summary.minX = o.box.x;
                summary.minY = o.box.y;
                summary.maxX = o.box.right();
                summary.maxY = o.box.bottom();
            } else {
                summary.minX = std::min(summary.minX, o.box.x);
                summary.minY = std::min(summary.minY, o.box.y);
                summary.maxX = std::max(summary.maxX, o.box.right());
                summary.maxY = std::max(summary.maxY, o.box.bottom());
            }
            ++summary.count;
        }

        // A layer dropped after its records were accepted refuses the batch.
        detail::Statement dropped(db, "SELECT dropped FROM layers WHERE id = ?");
        for (const auto& entry : layers) {
            dropped.bindInt64(1, entry.first);
            if (dropped.step() && dropped.columnInt64(0) != 0) {
                throwInvalid(operation, "layer '" + layerNames.at(entry.first) + "' was dropped; the batch of " +
                                            std::to_string(batch.size()) + " objects was rolled back");
            }
            dropped.reset();
        }

        for (const auto& entry : layers) {
            detail::EventRow event;
            event.kind = EventKind::ObjectsAdded;
            event.stageId = stage.value();
            event.layerId = entry.first;
            event.objectCount = entry.second.count;
            event.minX = entry.second.minX;
            event.minY = entry.second.minY;
            event.maxX = entry.second.maxX;
            event.maxY = entry.second.maxY;
            detail::appendEvent(db, event);
        }
    }

    struct KeyHash {
        size_t operator()(const std::pair<int32_t, int64_t>& key) const noexcept {
            return std::hash<int64_t>()(key.second) * 31u + std::hash<int32_t>()(key.first);
        }
    };

    const std::shared_ptr<detail::StoreGate> gate;
    const StageId stage;
    std::atomic<int64_t> batchSize;

    mutable std::mutex mutex;  // guards everything below
    std::vector<PendingObject> pending;
    std::unordered_set<std::pair<int32_t, int64_t>, KeyHash> pendingKeys;  // (layer, anchor)
    int64_t committed = 0;
    std::unordered_map<int32_t, std::string> layerNames;  // validated layers
    std::unordered_set<int32_t> heldLabels;

    // Store::close() commits the pending batch while the store is still open.
    void flushBeforeClose() override {
        std::lock_guard<std::mutex> guard(mutex);
        commitLocked("Store::close");
    }
};

} // namespace

class ObjectWriter::Impl {
public:
    std::shared_ptr<WriterCore> core;
};

ObjectWriter::ObjectWriter(std::shared_ptr<detail::StoreGate> gate, StageId stage, int64_t batchSize)
    : impl_(std::make_unique<Impl>()) {
    impl_->core = std::make_shared<WriterCore>(gate, stage, batchSize);
    gate->track(impl_->core);
}

ObjectWriter::~ObjectWriter() {
    try {
        flush();
    } catch (const std::exception& e) {
        logMessage(LogLevel::Error, "ObjectWriter: flushing stage %d to '%s' failed: %s", impl_->core->stage.value(),
                   impl_->core->gate->path().c_str(), e.what());
    } catch (...) {
        logMessage(LogLevel::Error, "ObjectWriter: flushing stage %d to '%s' failed", impl_->core->stage.value(),
                   impl_->core->gate->path().c_str());
    }
}

void ObjectWriter::emit(const ObjectRecord& record) {
    constexpr const char* kOperation = "ObjectWriter::emit";
    const LayerId layer = record.layer();
    const Polygon& geometry = record.geometry();
    const ObjectId id = record.id();

    // Encoded before locking, so concurrent emitters encode in parallel.
    PendingObject object;
    if (!geometry.isEmpty() && id.isValid()) {
        object.layer = layer.value();
        object.anchor = id.anchorPacked();
        object.anchorPoint = id.anchorPoint();
        object.box = geometry.boundingBox();
        object.geometry = detail::encodePolygon(geometry);
        const MeasurementSet& measurements = record.measurements();
        object.measurements.reserve(static_cast<size_t>(measurements.count()));
        for (int32_t i = 0; i < measurements.count(); ++i) {
            const Measurement m = measurements.at(i);
            object.measurements.push_back({m.name(), m.value(), static_cast<int64_t>(m.unit())});
        }
    }

    std::lock_guard<std::mutex> guard(impl_->core->mutex);
    // A writer that outlived its store, or whose store closed, refuses at once.
    impl_->core->gate->with(kOperation, [&](detail::StoreImpl& store) { store.checkWritable(kOperation); });
    const std::string& layerName = impl_->core->requireLayer(kOperation, layer);
    if (geometry.isEmpty()) throwInvalid(kOperation, "an object in layer '" + layerName + "' has empty geometry");
    if (!id.isValid()) {
        throwInvalid(kOperation, "the anchor " + anchorText(geometry.anchor()) + " of an object in layer '" + layerName +
                                     "' is outside the identifiable range 0 <= x < 2^31, 0 <= y < 2^32");
    }
    if (record.hasLabel()) {
        object.label = record.label().value();
        impl_->core->requireLabel(kOperation, record.label());
    }
    if (!impl_->core->pendingKeys.insert({object.layer, object.anchor}).second) {
        throwInvalid(kOperation, "an object with anchor " + anchorText(object.anchorPoint) +
                                     " is already pending in layer '" + layerName +
                                     "'; objects in one layer may not share an anchor");
    }
    try {
        impl_->core->pending.push_back(std::move(object));
    } catch (...) {
        impl_->core->pendingKeys.erase({object.layer, object.anchor});
        throw;
    }
    if (static_cast<int64_t>(impl_->core->pending.size()) >= impl_->core->batchSize.load(std::memory_order_relaxed)) {
        impl_->core->commitLocked(kOperation);
    }
}

void ObjectWriter::flush() {
    std::lock_guard<std::mutex> guard(impl_->core->mutex);
    impl_->core->commitLocked("ObjectWriter::flush");
}

int64_t ObjectWriter::emittedCount() const {
    std::lock_guard<std::mutex> guard(impl_->core->mutex);
    return impl_->core->committed + static_cast<int64_t>(impl_->core->pending.size());
}

int64_t ObjectWriter::batchSizeObjects() const noexcept { return impl_->core->batchSize.load(std::memory_order_relaxed); }

void ObjectWriter::setBatchSizeObjects(int64_t size) {
    if (size < 1) {
        const std::string message =
            "ObjectWriter::setBatchSizeObjects: the batch size " + std::to_string(size) + " is less than 1";
        throw InvalidArgumentError(message.c_str());
    }
    impl_->core->batchSize.store(size, std::memory_order_relaxed);
}

StageId ObjectWriter::stage() const noexcept { return impl_->core->stage; }

} // namespace slideio::processor
