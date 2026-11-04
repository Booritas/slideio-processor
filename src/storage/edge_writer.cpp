#include <slideio/processor/storage/edge_store.hpp>

#include "storage/detail/errors.hpp"
#include "storage/detail/event_log.hpp"
#include "storage/detail/object_lookup.hpp"
#include "storage/detail/sqlite.hpp"
#include "storage/detail/store_impl.hpp"

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/log.hpp>
#include <slideio/processor/storage/layer_registry.hpp>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace slideio::processor {

namespace {

// Missing endpoints are reported under this name whichever call triggered the commit
// (ruling S19).
constexpr const char* kFlushOperation = "EdgeWriter::flush";

struct PendingEdge {
    ObjectId from;  // the child, or the derived object
    ObjectId to;    // the parent, or the primitive
};

// What a writer knows of a layer it validated: layers are never removed, renamed or
// re-kinded, so this is cached. A drop is re-read at each call that cares.
struct LayerInfo {
    std::string name;
    LayerKind kind = LayerKind::Semantic;
};

// The EdgesAsserted summary of a batch (ruling S21).
struct BatchSummary {
    int64_t count = 0;
    int32_t layer = 0;  // 0: the rows span more than one layer
    int64_t minX = 0, minY = 0, maxX = 0, maxY = 0;

    void add(LayerId objectLayer, const Rect64& box) {
        if (count == 0) {
            layer = objectLayer.value();
            minX = box.x;
            minY = box.y;
            maxX = box.right();
            maxY = box.bottom();
        } else {
            if (layer != objectLayer.value()) layer = 0;
            minX = std::min(minX, box.x);
            minY = std::min(minY, box.y);
            maxX = std::max(maxX, box.right());
            maxY = std::max(maxY, box.bottom());
        }
        ++count;
    }
};

// "'<name>' (id <n>)", or "<n>" when the store holds no such stage.
std::string describeStage(sqlite3* db, int64_t stage) {
    detail::Statement select(db, "SELECT name FROM stages WHERE id = ?");
    select.bindInt64(1, stage);
    const std::string id = std::to_string(stage);
    return select.step() ? "'" + select.columnText(0) + "' (id " + id + ")" : id;
}

// The state of one EdgeWriter, shared with the store gate so that Store::close() can
// commit what it holds pending (SDS 03 §3.2).
class EdgeWriterCore final : public detail::GateClient {
public:
    EdgeWriterCore(std::shared_ptr<detail::StoreGate> g, const LayerRegistry& l, StageId s, int64_t batch)
        : gate(std::move(g)), layers(l), stage(s), batchSize(std::max<int64_t>(1, batch)) {}

    // Returns what the registry says of a layer, or null for an unknown one. The
    // registry is reached only inside the gate, so it is never used after the Store
    // that owns it has closed. Caller holds `mutex`.
    const LayerInfo* layerInfo(const char* operation, LayerId layer) {
        const auto cached = layerInfos.find(layer.value());
        if (cached != layerInfos.end()) return &cached->second;
        bool known = false;
        LayerInfo info;
        gate->with(operation, [&](detail::StoreImpl&) {
            known = layers.exists(layer);
            if (known) {
                info.name = layers.name(layer);
                info.kind = layers.kind(layer);
            }
        });
        if (!known) return nullptr;
        return &layerInfos.emplace(layer.value(), std::move(info)).first->second;
    }

    // "the child's layer 'cells' and the parent's layer 'tissue'", for messages naming
    // both layers.
    static std::string bothLayers(const char* firstRole, LayerId first, const LayerInfo* firstInfo,
                                  const char* secondRole, LayerId second, const LayerInfo* secondInfo) {
        const auto text = [](const char* role, LayerId layer, const LayerInfo* info) {
            return std::string("the ") + role + "'s layer " +
                   (info != nullptr ? "'" + info->name + "'" : std::to_string(layer.value()) + " (unknown)");
        };
        return text(firstRole, first, firstInfo) + " and " + text(secondRole, second, secondInfo);
    }

    // Refuses new edges once the store is closed or closing; see ObjectWriter::emit for
    // why this runs under `mutex`. Caller holds `mutex`.
    void checkAccepting(const char* operation) {
        gate->with(operation, [&](detail::StoreImpl& store) { store.checkAcceptingObjects(operation); });
    }

    // Accepts one more call, committing once the pending edges reach the batch size.
    // Caller holds `mutex`.
    void accepted() {
        ++pendingCalls;
        if (static_cast<int64_t>(parents.size() + derived.size()) >= batchSize.load(std::memory_order_relaxed)) {
            commitLocked(kFlushOperation);
        }
    }

    // Commits every pending edge in one transaction (rulings S18, S19, S21). The batch
    // leaves the pending state first, so a failed commit drops it. Caller holds `mutex`.
    void commitLocked(const char* operation) {
        if (parents.empty() && derived.empty()) return;
        std::vector<PendingEdge> parentBatch;
        std::vector<PendingEdge> derivedBatch;
        parentBatch.swap(parents);
        derivedBatch.swap(derived);
        pendingParents.clear();
        const int64_t calls = pendingCalls;
        pendingCalls = 0;
        gate->with(operation, [&](detail::StoreImpl& store) {
            const bool appended = store.withWriteTransaction(
                operation, [&](sqlite3* db) { return insertBatch(db, operation, parentBatch, derivedBatch); });
            if (appended) store.notifyCommitted();
        });
        committed += calls;
    }

    // Returns whether an event was appended.
    bool insertBatch(sqlite3* db, const char* operation, const std::vector<PendingEdge>& parentBatch,
                     const std::vector<PendingEdge>& derivedBatch) {
        detail::Statement lookup(db, detail::kObjectLookupSql);
        const auto resolve = [&](const char* role, ObjectId id) {
            detail::ObjectRow row{};
            if (!detail::findObjectRow(lookup, id, row)) {
                detail::throwInvalid(operation, std::string(role) + " " + detail::describeObject(db, id) + " does not exist");
            }
            return row;
        };
        BatchSummary summary;

        detail::Statement insertParent(
            db, "INSERT INTO parent_edges(child_rowid, parent_rowid, stage_id) VALUES (?, ?, ?) ON CONFLICT DO NOTHING");
        detail::Statement existing(db, "SELECT parent_rowid, stage_id FROM parent_edges WHERE child_rowid = ?");
        for (const PendingEdge& e : parentBatch) {
            const detail::ObjectRow child = resolve("child", e.from);
            const detail::ObjectRow parent = resolve("parent", e.to);
            insertParent.bindInt64(1, child.rowid);
            insertParent.bindInt64(2, parent.rowid);
            insertParent.bindInt64(3, stage.value());
            insertParent.run();
            insertParent.reset();
            if (sqlite3_changes(db) > 0) {
                summary.add(e.from.layer(), child.box);
                continue;
            }
            existing.bindInt64(1, child.rowid);
            if (!existing.step()) {
                throw InternalError((std::string(operation) + ": a parent edge was neither inserted nor found").c_str());
            }
            const int64_t existingParent = existing.columnInt64(0);
            const int64_t existingStage = existing.columnInt64(1);
            existing.reset();
            if (existingParent == parent.rowid && existingStage == stage.value()) continue;  // the same edge
            detail::throwInvalid(operation, "child " + detail::describeObject(db, e.from) + " already has parent " +
                                                describeRow(db, existingParent) + ", asserted by stage " +
                                                describeStage(db, existingStage) +
                                                "; a child has one parent. The batch was rolled back");
        }

        detail::Statement insertDerived(db,
                                        "INSERT INTO derived_from(derived_layer, derived_anchor, primitive_layer, "
                                        "primitive_anchor, stage_id) VALUES (?, ?, ?, ?, ?) ON CONFLICT DO NOTHING");
        for (const PendingEdge& e : derivedBatch) {
            const detail::ObjectRow object = resolve("derived", e.from);
            resolve("primitive", e.to);
            insertDerived.bindInt64(1, e.from.layer().value());
            insertDerived.bindInt64(2, e.from.anchorPacked());
            insertDerived.bindInt64(3, e.to.layer().value());
            insertDerived.bindInt64(4, e.to.anchorPacked());
            insertDerived.bindInt64(5, stage.value());
            insertDerived.run();
            insertDerived.reset();
            if (sqlite3_changes(db) > 0) summary.add(e.from.layer(), object.box);
        }

        if (summary.count == 0) return false;
        detail::EventRow event;
        event.kind = EventKind::EdgesAsserted;
        event.stageId = stage.value();
        event.layerId = summary.layer;
        event.objectCount = summary.count;
        event.minX = summary.minX;
        event.minY = summary.minY;
        event.maxX = summary.maxX;
        event.maxY = summary.maxY;
        detail::appendEvent(db, event);
        return true;
    }

    // describeObject for an object known by rowid.
    static std::string describeRow(sqlite3* db, int64_t rowid) {
        detail::Statement select(db, "SELECT layer_id, anchor_id FROM objects WHERE rowid = ?");
        select.bindInt64(1, rowid);
        if (!select.step()) return "object row " + std::to_string(rowid);
        return detail::describeObject(db, detail::objectIdOf(select.columnInt64(0), select.columnInt64(1)));
    }

    // Runs fn(sqlite3*) on a read connection, for a message. Caller holds `mutex`.
    template <class Fn>
    std::string readForMessage(const char* operation, Fn&& fn) {
        return gate->with(operation, [&](detail::StoreImpl& store) { return store.withReadConnection(operation, fn); });
    }

    const std::shared_ptr<detail::StoreGate> gate;
    const LayerRegistry& layers;  // reached only inside gate->with
    const StageId stage;
    std::atomic<int64_t> batchSize;

    mutable std::mutex mutex;  // guards everything below
    std::vector<PendingEdge> parents;
    std::vector<PendingEdge> derived;
    std::map<ObjectId, ObjectId> pendingParents;  // child -> parent, for the in-writer conflict check
    int64_t pendingCalls = 0;
    int64_t committed = 0;
    std::unordered_map<int32_t, LayerInfo> layerInfos;

    // Store::close() commits the pending batch while the store is still open.
    void flushBeforeClose() override {
        std::lock_guard<std::mutex> guard(mutex);
        commitLocked("Store::close");
    }
};

} // namespace

class EdgeWriter::Impl {
public:
    std::shared_ptr<EdgeWriterCore> core;
};

EdgeWriter::EdgeWriter(std::shared_ptr<detail::StoreGate> gate, const LayerRegistry& layers, StageId stage,
                       int64_t batchSize)
    : impl_(std::make_unique<Impl>()) {
    impl_->core = std::make_shared<EdgeWriterCore>(gate, layers, stage, batchSize);
    gate->track(impl_->core);
}

EdgeWriter::~EdgeWriter() {
    try {
        flush();
    } catch (const std::exception& e) {
        logMessage(LogLevel::Error, "EdgeWriter: flushing stage %d to '%s' failed: %s", impl_->core->stage.value(),
                   impl_->core->gate->path().c_str(), e.what());
    } catch (...) {
        logMessage(LogLevel::Error, "EdgeWriter: flushing stage %d to '%s' failed", impl_->core->stage.value(),
                   impl_->core->gate->path().c_str());
    }
}

void EdgeWriter::assertParent(ObjectId child, ObjectId parent) {
    constexpr const char* kOperation = "EdgeWriter::assertParent";
    EdgeWriterCore& core = *impl_->core;
    std::lock_guard<std::mutex> guard(core.mutex);
    core.checkAccepting(kOperation);
    if (!child.isValid()) detail::throwInvalid(kOperation, "the child id is invalid");
    if (!parent.isValid()) detail::throwInvalid(kOperation, "the parent id is invalid");
    const LayerInfo* childInfo = core.layerInfo(kOperation, child.layer());
    const LayerInfo* parentInfo = core.layerInfo(kOperation, parent.layer());
    const std::string layers =
        EdgeWriterCore::bothLayers("child", child.layer(), childInfo, "parent", parent.layer(), parentInfo);
    if (childInfo == nullptr || parentInfo == nullptr) detail::throwInvalid(kOperation, layers + ": a layer is unknown");
    if (childInfo->kind == LayerKind::Primitive || parentInfo->kind == LayerKind::Primitive) {
        detail::throwInvalid(kOperation, layers + ": a primitive layer has no position in the hierarchy");
    }
    const bool above = core.gate->with(
        kOperation, [&](detail::StoreImpl&) { return core.layers.isAbove(child.layer(), parent.layer()); });
    if (!above) {
        detail::throwInvalid(kOperation, layers + ": the parent's layer must lie below the child's, in its branch");
    }

    const auto pending = core.pendingParents.find(child);
    if (pending != core.pendingParents.end()) {
        if (pending->second == parent) {  // the same edge: accepted, no new row
            core.accepted();
            return;
        }
        const ObjectId other = pending->second;
        const std::string message = core.readForMessage(kOperation, [&](sqlite3* db) {
            return "child " + detail::describeObject(db, child) + " already has parent " +
                   detail::describeObject(db, other) + " pending, asserted by stage " +
                   describeStage(db, core.stage.value()) + "; a child has one parent";
        });
        detail::throwInvalid(kOperation, message);
    }
    core.pendingParents.emplace(child, parent);
    try {
        core.parents.push_back({child, parent});
    } catch (...) {
        core.pendingParents.erase(child);
        throw;
    }
    core.accepted();
}

void EdgeWriter::assertDerivedFrom(ObjectId derived, ObjectId primitive) {
    constexpr const char* kOperation = "EdgeWriter::assertDerivedFrom";
    EdgeWriterCore& core = *impl_->core;
    std::lock_guard<std::mutex> guard(core.mutex);
    core.checkAccepting(kOperation);
    if (!derived.isValid()) detail::throwInvalid(kOperation, "the derived id is invalid");
    if (!primitive.isValid()) detail::throwInvalid(kOperation, "the primitive id is invalid");
    const LayerInfo* derivedInfo = core.layerInfo(kOperation, derived.layer());
    const LayerInfo* primitiveInfo = core.layerInfo(kOperation, primitive.layer());
    if (derivedInfo == nullptr || primitiveInfo == nullptr || derivedInfo->kind != LayerKind::Semantic ||
        primitiveInfo->kind != LayerKind::Primitive) {
        detail::throwInvalid(kOperation, EdgeWriterCore::bothLayers("derived object", derived.layer(), derivedInfo,
                                                                    "primitive", primitive.layer(), primitiveInfo) +
                                             ": the derived object must lie in a semantic layer and the primitive "
                                             "in a primitive layer");
    }
    const bool dropped =
        core.gate->with(kOperation, [&](detail::StoreImpl&) { return core.layers.isDropped(primitive.layer()); });
    if (dropped) detail::throwInvalid(kOperation, "the primitive layer '" + primitiveInfo->name + "' is dropped");
    core.derived.push_back({derived, primitive});
    core.accepted();
}

void EdgeWriter::flush() {
    std::lock_guard<std::mutex> guard(impl_->core->mutex);
    impl_->core->commitLocked(kFlushOperation);
}

int64_t EdgeWriter::assertedCount() const {
    std::lock_guard<std::mutex> guard(impl_->core->mutex);
    return impl_->core->committed + impl_->core->pendingCalls;
}

int64_t EdgeWriter::batchSizeEdges() const noexcept { return impl_->core->batchSize.load(std::memory_order_relaxed); }

void EdgeWriter::setBatchSizeEdges(int64_t size) {
    if (size < 1) {
        detail::throwInvalid("EdgeWriter::setBatchSizeEdges", "the batch size " + std::to_string(size) + " is less than 1");
    }
    impl_->core->batchSize.store(size, std::memory_order_relaxed);
}

StageId EdgeWriter::stage() const noexcept { return impl_->core->stage; }

} // namespace slideio::processor
