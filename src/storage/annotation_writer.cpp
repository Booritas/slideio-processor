#include <slideio/processor/storage/annotation_store.hpp>

#include "storage/detail/errors.hpp"
#include "storage/detail/event_log.hpp"
#include "storage/detail/measurement_names.hpp"
#include "storage/detail/object_lookup.hpp"
#include "storage/detail/sqlite.hpp"
#include "storage/detail/store_impl.hpp"

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/log.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace slideio::processor {

namespace {

// Missing objects and committed conflicts are reported under this name whichever call
// triggered the commit (ruling S22).
constexpr const char* kFlushOperation = "AnnotationWriter::flush";

struct PendingLabel {
    ObjectId object;
    LabelId label;
};

struct PendingValue {
    double value;
    MeasurementUnit unit;
};

// The ObjectsAnnotated summary of a batch (ruling S21): one entry per distinct object
// that gained a row.
struct BatchSummary {
    std::set<ObjectId> objects;
    int32_t layer = 0;  // 0: the objects span more than one layer
    int64_t minX = 0, minY = 0, maxX = 0, maxY = 0;

    void add(ObjectId object, const Rect64& box) {
        if (!objects.insert(object).second) return;
        if (objects.size() == 1) {
            layer = object.layer().value();
            minX = box.x;
            minY = box.y;
            maxX = box.right();
            maxY = box.bottom();
        } else {
            if (layer != object.layer().value()) layer = 0;
            minX = std::min(minX, box.x);
            minY = std::min(minY, box.y);
            maxX = std::max(maxX, box.right());
            maxY = std::max(maxY, box.bottom());
        }
    }
};

bool unitInRange(MeasurementUnit unit) {
    const auto value = static_cast<int32_t>(unit);
    return value >= static_cast<int32_t>(MeasurementUnit::Pixels) &&
           value <= static_cast<int32_t>(MeasurementUnit::Dimensionless);
}

// The state of one AnnotationWriter, shared with the store gate so that Store::close()
// can commit what it holds pending (SDS 03 §3.2).
class AnnotationWriterCore final : public detail::GateClient {
public:
    AnnotationWriterCore(std::shared_ptr<detail::StoreGate> g, StageId s, int64_t batch)
        : gate(std::move(g)), stage(s), batchSize(std::max<int64_t>(1, batch)) {}

    // Refuses new annotations once the store is closed or closing; see ObjectWriter::emit
    // for why this runs under `mutex`. Caller holds `mutex`.
    void checkAccepting(const char* operation) {
        gate->with(operation, [&](detail::StoreImpl& store) { store.checkAcceptingObjects(operation); });
    }

    // Returns whether the store holds `label`. Labels are never removed, so a known one is
    // cached. Caller holds `mutex`.
    bool labelExists(const char* operation, LabelId label) {
        if (knownLabels.count(label.value()) != 0) return true;
        const bool exists = gate->with(operation, [&](detail::StoreImpl& store) {
            return store.withReadConnection(operation, [&](sqlite3* db) {
                detail::Statement select(db, "SELECT 1 FROM labels WHERE id = ?");
                select.bindInt64(1, label.value());
                return select.step();
            });
        });
        if (exists) knownLabels.insert(label.value());
        return exists;
    }

    // describeObject on a read connection, for a message raised at the call. Caller holds
    // `mutex`.
    std::string describeForMessage(const char* operation, ObjectId object) {
        return gate->with(operation, [&](detail::StoreImpl& store) {
            return store.withReadConnection(operation,
                                            [&](sqlite3* db) { return detail::describeObject(db, object); });
        });
    }

    // Accepts one more call, committing once the pending annotations reach the batch
    // size. Caller holds `mutex`.
    void accepted() {
        ++pendingCalls;
        if (static_cast<int64_t>(labels.size() + measurements.size()) >= batchSize.load(std::memory_order_relaxed)) {
            commitLocked(kFlushOperation);
        }
    }

    // Commits every pending annotation in one transaction (rulings S21, S22). The batch
    // leaves the pending state first, so a failed commit drops it. Caller holds `mutex`.
    void commitLocked(const char* operation) {
        if (labels.empty() && measurements.empty()) return;
        std::vector<PendingLabel> labelBatch;
        std::map<std::pair<ObjectId, std::string>, PendingValue> measurementBatch;
        labelBatch.swap(labels);
        measurementBatch.swap(measurements);
        pendingLabels.clear();
        const int64_t calls = pendingCalls;
        pendingCalls = 0;
        gate->with(operation, [&](detail::StoreImpl& store) {
            const bool appended = store.withWriteTransaction(
                operation, [&](sqlite3* db) { return insertBatch(db, operation, labelBatch, measurementBatch); });
            if (appended) store.notifyCommitted();
        });
        committed += calls;
    }

    // Returns whether an event was appended.
    bool insertBatch(sqlite3* db, const char* operation, const std::vector<PendingLabel>& labelBatch,
                     const std::map<std::pair<ObjectId, std::string>, PendingValue>& measurementBatch) {
        detail::Statement lookup(db, detail::kObjectLookupSql);
        const auto resolve = [&](ObjectId id) {
            detail::ObjectRow row{};
            if (!detail::findObjectRow(lookup, id, row)) {
                detail::throwInvalid(operation, "object " + detail::describeObject(db, id) + " does not exist");
            }
            return row;
        };
        BatchSummary summary;

        detail::Statement insertLabel(
            db, "INSERT INTO label_assertions(object_rowid, stage_id, label_id) VALUES (?, ?, ?) ON CONFLICT DO NOTHING");
        for (const PendingLabel& l : labelBatch) {
            const detail::ObjectRow object = resolve(l.object);
            insertLabel.bindInt64(1, object.rowid);
            insertLabel.bindInt64(2, stage.value());
            insertLabel.bindInt64(3, l.label.value());
            insertLabel.run();
            insertLabel.reset();
            if (sqlite3_changes(db) > 0) summary.add(l.object, object.box);
        }

        detail::MeasurementNameInterner nameIds(db, "AnnotationWriter");
        detail::Statement insertMeasurement(db,
                                            "INSERT INTO measurement_assertions(object_rowid, stage_id, name_id, value, "
                                            "unit) VALUES (?, ?, ?, ?, ?) ON CONFLICT DO NOTHING");
        detail::Statement existing(db,
                                   "SELECT value, unit FROM measurement_assertions "
                                   "WHERE object_rowid = ? AND stage_id = ? AND name_id = ?");
        for (const auto& entry : measurementBatch) {
            const ObjectId id = entry.first.first;
            const std::string& name = entry.first.second;
            const PendingValue& v = entry.second;
            const detail::ObjectRow object = resolve(id);
            const int64_t nameId = nameIds.idOf(name);
            insertMeasurement.bindInt64(1, object.rowid);
            insertMeasurement.bindInt64(2, stage.value());
            insertMeasurement.bindInt64(3, nameId);
            insertMeasurement.bindDouble(4, v.value);
            insertMeasurement.bindInt64(5, static_cast<int64_t>(v.unit));
            insertMeasurement.run();
            insertMeasurement.reset();
            if (sqlite3_changes(db) > 0) {
                summary.add(id, object.box);
                continue;
            }
            existing.bindInt64(1, object.rowid);
            existing.bindInt64(2, stage.value());
            existing.bindInt64(3, nameId);
            if (!existing.step()) {
                throw InternalError(
                    (std::string(operation) + ": a measurement assertion was neither inserted nor found").c_str());
            }
            const double storedValue = existing.columnDouble(0);
            const int64_t storedUnit = existing.columnInt64(1);
            existing.reset();
            if (storedValue == v.value && storedUnit == static_cast<int64_t>(v.unit)) continue;  // the same claim
            // Values are not named: they may come from file content.
            detail::throwInvalid(operation, "object " + detail::describeObject(db, id) + " already has measurement '" +
                                                name + "' with another value or unit from stage " +
                                                std::to_string(stage.value()) + "; the batch was rolled back");
        }

        if (summary.objects.empty()) return false;
        detail::EventRow event;
        event.kind = EventKind::ObjectsAnnotated;
        event.stageId = stage.value();
        event.layerId = summary.layer;
        event.objectCount = static_cast<int64_t>(summary.objects.size());
        event.minX = summary.minX;
        event.minY = summary.minY;
        event.maxX = summary.maxX;
        event.maxY = summary.maxY;
        detail::appendEvent(db, event);
        return true;
    }

    const std::shared_ptr<detail::StoreGate> gate;
    const StageId stage;
    std::atomic<int64_t> batchSize;

    mutable std::mutex mutex;  // guards everything below
    std::vector<PendingLabel> labels;
    std::set<std::pair<ObjectId, LabelId>> pendingLabels;  // dedupes repeats within a batch
    // Keyed by (object, name), which is also the in-writer conflict check (ruling S22).
    std::map<std::pair<ObjectId, std::string>, PendingValue> measurements;
    int64_t pendingCalls = 0;
    int64_t committed = 0;
    std::unordered_set<int32_t> knownLabels;

    // Store::close() commits the pending batch while the store is still open.
    void flushBeforeClose() override {
        std::lock_guard<std::mutex> guard(mutex);
        commitLocked("Store::close");
    }
    // After the object writers: the pending rows may reference objects they hold.
    int flushPhase() const noexcept override { return 1; }
};

} // namespace

class AnnotationWriter::Impl {
public:
    std::shared_ptr<AnnotationWriterCore> core;
};

AnnotationWriter::AnnotationWriter(std::shared_ptr<detail::StoreGate> gate, StageId stage, int64_t batchSize)
    : impl_(std::make_unique<Impl>()) {
    impl_->core = std::make_shared<AnnotationWriterCore>(gate, stage, batchSize);
    gate->track(impl_->core);
}

AnnotationWriter::~AnnotationWriter() {
    try {
        flush();
    } catch (const std::exception& e) {
        logMessage(LogLevel::Error, "AnnotationWriter: flushing stage %d to '%s' failed: %s",
                   impl_->core->stage.value(), impl_->core->gate->path().c_str(), e.what());
    } catch (...) {
        logMessage(LogLevel::Error, "AnnotationWriter: flushing stage %d to '%s' failed", impl_->core->stage.value(),
                   impl_->core->gate->path().c_str());
    }
}

void AnnotationWriter::assertLabel(ObjectId object, LabelId label) {
    constexpr const char* kOperation = "AnnotationWriter::assertLabel";
    AnnotationWriterCore& core = *impl_->core;
    std::lock_guard<std::mutex> guard(core.mutex);
    core.checkAccepting(kOperation);
    if (!object.isValid()) detail::throwInvalid(kOperation, "the object id is invalid");
    if (!label.isValid()) detail::throwInvalid(kOperation, "the label id is invalid");
    if (!core.labelExists(kOperation, label)) {
        detail::throwInvalid(kOperation, "the store holds no label " + std::to_string(label.value()));
    }
    if (core.pendingLabels.emplace(object, label).second) {
        try {
            core.labels.push_back({object, label});
        } catch (...) {
            core.pendingLabels.erase({object, label});
            throw;
        }
    }
    core.accepted();
}

void AnnotationWriter::assertMeasurement(ObjectId object, const char* name, double value, MeasurementUnit unit) {
    constexpr const char* kOperation = "AnnotationWriter::assertMeasurement";
    AnnotationWriterCore& core = *impl_->core;
    std::lock_guard<std::mutex> guard(core.mutex);
    core.checkAccepting(kOperation);
    if (!object.isValid()) detail::throwInvalid(kOperation, "the object id is invalid");
    if (name == nullptr || *name == '\0') detail::throwInvalid(kOperation, "the measurement name is null or empty");
    if (!std::isfinite(value)) {
        detail::throwInvalid(kOperation, std::string("the value of measurement '") + name + "' is not finite");
    }
    if (!unitInRange(unit)) {
        detail::throwInvalid(kOperation, "the unit " + std::to_string(static_cast<int32_t>(unit)) +
                                             " of measurement '" + name + "' is out of range");
    }
    const auto inserted = core.measurements.emplace(std::make_pair(object, std::string(name)), PendingValue{value, unit});
    if (!inserted.second) {
        const PendingValue& pending = inserted.first->second;
        if (pending.value != value || pending.unit != unit) {
            detail::throwInvalid(kOperation, "object " + core.describeForMessage(kOperation, object) +
                                                 " already has measurement '" + name +
                                                 "' pending with another value or unit; one stage asserts one value");
        }
    }
    core.accepted();
}

void AnnotationWriter::flush() {
    std::lock_guard<std::mutex> guard(impl_->core->mutex);
    impl_->core->commitLocked(kFlushOperation);
}

int64_t AnnotationWriter::assertedCount() const {
    std::lock_guard<std::mutex> guard(impl_->core->mutex);
    return impl_->core->committed + impl_->core->pendingCalls;
}

int64_t AnnotationWriter::batchSizeAnnotations() const noexcept {
    return impl_->core->batchSize.load(std::memory_order_relaxed);
}

void AnnotationWriter::setBatchSizeAnnotations(int64_t size) {
    if (size < 1) {
        detail::throwInvalid("AnnotationWriter::setBatchSizeAnnotations",
                             "the batch size " + std::to_string(size) + " is less than 1");
    }
    impl_->core->batchSize.store(size, std::memory_order_relaxed);
}

StageId AnnotationWriter::stage() const noexcept { return impl_->core->stage; }

} // namespace slideio::processor
