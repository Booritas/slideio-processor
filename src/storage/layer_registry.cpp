#include <slideio/processor/storage/layer_registry.hpp>

#include "storage/detail/event_log.hpp"
#include "storage/detail/schema.hpp"
#include "storage/detail/sqlite.hpp"
#include "storage/detail/store_impl.hpp"

#include <slideio/processor/core/error.hpp>

#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace slideio::processor {

namespace {

[[noreturn]] void throwInvalid(const char* operation, const std::string& what) {
    const std::string message = std::string(operation) + ": " + what;
    throw InvalidArgumentError(message.c_str());
}

[[noreturn]] void throwUnknown(const char* operation, LayerId layer) {
    throwInvalid(operation, "there is no layer " + std::to_string(layer.value()));
}

void validateName(const char* operation, const char* name) {
    if (name == nullptr || *name == '\0') throwInvalid(operation, "the layer name is null or empty");
    if (std::string(name) == detail::kPixelLayerName) {
        throwInvalid(operation, "the layer name 'pixel' is reserved for the pixel layer");
    }
}

} // namespace

class LayerRegistry::Impl {
public:
    // The immutable part of a layer row. dropped and analysis_level can change, so they
    // are read from the table on every call and never cached.
    struct Row {
        LayerId id;
        std::string name;
        LayerKind kind = LayerKind::Semantic;
        LayerId parent;
        int32_t depth = 0;
        std::vector<LayerId> children;  // semantic layers directly beneath, ascending id
    };

    explicit Impl(detail::StoreImpl& s) : store(s) {}

    // Appends to the cache every row the table holds beyond it, when the table's row
    // count differs from the cache's. Layers are append-only, so the count suffices.
    // Caller holds `mutex`.
    void refresh(const char* operation) {
        store.withReadConnection(operation, [&](sqlite3* db) {
            int64_t stored = 0;
            {
                detail::Statement count(db, "SELECT COUNT(*) FROM layers");
                if (count.step()) stored = count.columnInt64(0);
            }
            if (stored == static_cast<int64_t>(rows.size())) return;
            const int64_t last = rows.empty() ? 0 : rows.back().id.value();
            detail::Statement select(db,
                                     "SELECT id, name, kind, parent_id, depth FROM layers WHERE id > ? ORDER BY id");
            select.bindInt64(1, last);
            while (select.step()) {
                Row row;
                row.id = LayerId(static_cast<int32_t>(select.columnInt64(0)));
                row.name = select.columnText(1);
                row.kind = static_cast<LayerKind>(select.columnInt64(2));
                if (!select.columnIsNull(3)) row.parent = LayerId(static_cast<int32_t>(select.columnInt64(3)));
                row.depth = static_cast<int32_t>(select.columnInt64(4));
                rows.push_back(std::move(row));  // deque: earlier rows (and their names) never move
                Row& added = rows.back();
                byId[added.id.value()] = &added;
                byName[added.name] = &added;
                if (added.parent.value() != 0) {
                    const auto parent = byId.find(added.parent.value());
                    if (parent != byId.end()) parent->second->children.push_back(added.id);
                }
            }
        });
    }

    // Locks, refreshes, then hands the row to `read`; throws InvalidArgumentError for an
    // unknown layer.
    template <class Read>
    auto withRow(const char* operation, LayerId layer, Read&& read) {
        std::lock_guard<std::mutex> guard(mutex);
        refresh(operation);
        return read(requireRow(operation, layer));
    }

    // As withRow, for the hierarchy members: a primitive layer has no position in it.
    template <class Read>
    auto withSemanticRow(const char* operation, LayerId layer, Read&& read) {
        return withRow(operation, layer, [&](const Row& row) {
            requireSemantic(operation, row);
            return read(row);
        });
    }

    const Row& requireRow(const char* operation, LayerId layer) const {
        const auto found = byId.find(layer.value());
        if (found == byId.end()) throwUnknown(operation, layer);
        return *found->second;
    }

    static void requireSemantic(const char* operation, const Row& row) {
        if (row.kind != LayerKind::Semantic) {
            throwInvalid(operation, "layer '" + row.name + "' is primitive and has no place in the hierarchy");
        }
    }

    // Inserts a layer inside the caller's write transaction and appends LayerDeclared.
    static LayerId insertLayer(sqlite3* db, const char* operation, const char* name, LayerKind kind, LayerId parent,
                               int32_t depth) {
        const std::string layerName = name;
        {
            detail::Statement existing(db, "SELECT 1 FROM layers WHERE name = ?");
            existing.bindText(1, layerName);
            if (existing.step()) throwInvalid(operation, "a layer named '" + layerName + "' already exists");
        }
        detail::Statement insert(db, "INSERT INTO layers(name, kind, parent_id, depth) VALUES (?, ?, ?, ?)");
        insert.bindText(1, layerName);
        insert.bindInt64(2, static_cast<int64_t>(kind));
        if (parent.value() != 0) {
            insert.bindInt64(3, parent.value());
        } else {
            insert.bindNull(3);
        }
        insert.bindInt64(4, depth);
        insert.run();
        const LayerId id(static_cast<int32_t>(sqlite3_last_insert_rowid(db)));
        detail::EventRow event;
        event.kind = EventKind::LayerDeclared;
        event.layerId = id.value();
        detail::appendEvent(db, event);
        return id;
    }

    detail::StoreImpl& store;
    std::mutex mutex;
    std::deque<Row> rows;
    std::map<int32_t, Row*> byId;
    std::map<std::string, Row*> byName;
};

LayerRegistry::LayerRegistry(detail::StoreImpl& store) : impl_(std::make_unique<Impl>(store)) {}

LayerRegistry::~LayerRegistry() = default;

LayerId LayerRegistry::declareSemantic(const char* name, LayerId parent) {
    constexpr const char* kOperation = "LayerRegistry::declareSemantic";
    const LayerId id = impl_->store.withWriteTransaction(kOperation, [&](sqlite3* db) {
        validateName(kOperation, name);
        const std::string parentText = std::to_string(parent.value());
        detail::Statement select(db, "SELECT kind, depth, dropped FROM layers WHERE id = ?");
        select.bindInt64(1, parent.value());
        if (!select.step()) throwInvalid(kOperation, "the parent layer " + parentText + " does not exist");
        if (static_cast<LayerKind>(select.columnInt64(0)) != LayerKind::Semantic) {
            throwInvalid(kOperation, "the parent layer " + parentText + " is primitive");
        }
        if (select.columnInt64(2) != 0) throwInvalid(kOperation, "the parent layer " + parentText + " is dropped");
        const int32_t parentDepth = static_cast<int32_t>(select.columnInt64(1));
        return Impl::insertLayer(db, kOperation, name, LayerKind::Semantic, parent, parentDepth + 1);
    });
    impl_->store.notifyCommitted();
    return id;
}

LayerId LayerRegistry::declarePrimitive(const char* name) {
    constexpr const char* kOperation = "LayerRegistry::declarePrimitive";
    const LayerId id = impl_->store.withWriteTransaction(kOperation, [&](sqlite3* db) {
        validateName(kOperation, name);
        return Impl::insertLayer(db, kOperation, name, LayerKind::Primitive, LayerId(), 0);
    });
    impl_->store.notifyCommitted();
    return id;
}

LayerId LayerRegistry::pixelLayer() const noexcept { return LayerId(detail::kPixelLayerId); }

int32_t LayerRegistry::count() const noexcept {
    try {
        std::lock_guard<std::mutex> guard(impl_->mutex);
        impl_->refresh("LayerRegistry::count");
        return static_cast<int32_t>(impl_->rows.size());
    } catch (...) {
        return 0;
    }
}

LayerId LayerRegistry::at(int32_t index) const {
    constexpr const char* kOperation = "LayerRegistry::at";
    std::lock_guard<std::mutex> guard(impl_->mutex);
    impl_->refresh(kOperation);
    if (index < 0 || static_cast<size_t>(index) >= impl_->rows.size()) {
        throwInvalid(kOperation, "index " + std::to_string(index) + " is out of range");
    }
    return impl_->rows[static_cast<size_t>(index)].id;
}

bool LayerRegistry::find(const char* name, LayerId& out) const noexcept {
    if (name == nullptr || *name == '\0') return false;
    try {
        std::lock_guard<std::mutex> guard(impl_->mutex);
        impl_->refresh("LayerRegistry::find");
        const auto found = impl_->byName.find(name);
        if (found == impl_->byName.end()) return false;
        out = found->second->id;
        return true;
    } catch (...) {
        return false;
    }
}

const char* LayerRegistry::name(LayerId layer) const {
    return impl_->withRow("LayerRegistry::name", layer, [](const Impl::Row& row) { return row.name.c_str(); });
}

LayerKind LayerRegistry::kind(LayerId layer) const {
    return impl_->withRow("LayerRegistry::kind", layer, [](const Impl::Row& row) { return row.kind; });
}

bool LayerRegistry::exists(LayerId layer) const noexcept {
    try {
        std::lock_guard<std::mutex> guard(impl_->mutex);
        impl_->refresh("LayerRegistry::exists");
        return impl_->byId.count(layer.value()) != 0;
    } catch (...) {
        return false;
    }
}

LayerId LayerRegistry::parent(LayerId layer) const {
    return impl_->withSemanticRow("LayerRegistry::parent", layer, [](const Impl::Row& row) { return row.parent; });
}

int32_t LayerRegistry::depth(LayerId layer) const {
    return impl_->withSemanticRow("LayerRegistry::depth", layer, [](const Impl::Row& row) { return row.depth; });
}

int32_t LayerRegistry::childCount(LayerId layer) const {
    return impl_->withSemanticRow("LayerRegistry::childCount", layer,
                                  [](const Impl::Row& row) { return static_cast<int32_t>(row.children.size()); });
}

LayerId LayerRegistry::childAt(LayerId layer, int32_t index) const {
    constexpr const char* kOperation = "LayerRegistry::childAt";
    return impl_->withSemanticRow(kOperation, layer, [&](const Impl::Row& row) {
        if (index < 0 || static_cast<size_t>(index) >= row.children.size()) {
            throwInvalid(kOperation,
                         "index " + std::to_string(index) + " is out of range for layer '" + row.name + "'");
        }
        return row.children[static_cast<size_t>(index)];
    });
}

bool LayerRegistry::isAbove(LayerId descendant, LayerId ancestor) const {
    constexpr const char* kOperation = "LayerRegistry::isAbove";
    std::lock_guard<std::mutex> guard(impl_->mutex);
    impl_->refresh(kOperation);
    const Impl::Row& low = impl_->requireRow(kOperation, descendant);
    const Impl::Row& high = impl_->requireRow(kOperation, ancestor);
    Impl::requireSemantic(kOperation, low);
    Impl::requireSemantic(kOperation, high);
    // Walk up from the descendant's parent; depth strictly falls, so the walk ends.
    for (LayerId at = low.parent; at.value() != 0;) {
        if (at == ancestor) return true;
        at = impl_->requireRow(kOperation, at).parent;
    }
    return false;
}

bool LayerRegistry::isDropped(LayerId layer) const noexcept {
    try {
        return impl_->store.withReadConnection("LayerRegistry::isDropped", [&](sqlite3* db) {
            detail::Statement select(db, "SELECT dropped FROM layers WHERE id = ?");
            select.bindInt64(1, layer.value());
            return select.step() && select.columnInt64(0) != 0;
        });
    } catch (...) {
        return false;
    }
}

int32_t LayerRegistry::writerStageCount(LayerId layer) const {
    constexpr const char* kOperation = "LayerRegistry::writerStageCount";
    return impl_->store.withReadConnection(kOperation, [&](sqlite3* db) {
        detail::Statement select(
            db, "SELECT (SELECT COUNT(*) FROM stage_layers WHERE layer_id = layers.id) FROM layers WHERE id = ?");
        select.bindInt64(1, layer.value());
        if (!select.step()) throwUnknown(kOperation, layer);
        return static_cast<int32_t>(select.columnInt64(0));
    });
}

StageId LayerRegistry::writerStageAt(LayerId layer, int32_t index) const {
    constexpr const char* kOperation = "LayerRegistry::writerStageAt";
    return impl_->store.withReadConnection(kOperation, [&](sqlite3* db) {
        // One statement, so the layer and the stage are read from one snapshot.
        detail::Statement select(
            db,
            "SELECT EXISTS(SELECT 1 FROM layers WHERE id = ?1), (SELECT stage_id FROM stage_layers "
            "WHERE layer_id = ?1 ORDER BY stage_id LIMIT 1 OFFSET ?2)");
        select.bindInt64(1, layer.value());
        select.bindInt64(2, index);
        if (!select.step() || select.columnInt64(0) == 0) throwUnknown(kOperation, layer);
        if (index < 0 || select.columnIsNull(1)) {
            throwInvalid(kOperation,
                         "index " + std::to_string(index) + " is out of range for layer " + std::to_string(layer.value()));
        }
        return StageId(static_cast<int32_t>(select.columnInt64(1)));
    });
}

int32_t LayerRegistry::coarsestAnalysisLevel(LayerId layer) const noexcept {
    try {
        return impl_->store.withReadConnection("LayerRegistry::coarsestAnalysisLevel", [&](sqlite3* db) {
            detail::Statement select(db, "SELECT analysis_level FROM layers WHERE id = ?");
            select.bindInt64(1, layer.value());
            return select.step() ? static_cast<int32_t>(select.columnInt64(0)) : 0;
        });
    } catch (...) {
        return 0;
    }
}

void LayerRegistry::recordAnalysisLevel(LayerId layer, int32_t level) {
    constexpr const char* kOperation = "LayerRegistry::recordAnalysisLevel";
    impl_->store.withWriteTransaction(kOperation, [&](sqlite3* db) {
        if (level < 0) throwInvalid(kOperation, "the analysis level " + std::to_string(level) + " is negative");
        detail::Statement update(db, "UPDATE layers SET analysis_level = MAX(analysis_level, ?) WHERE id = ?");
        update.bindInt64(1, level);
        update.bindInt64(2, layer.value());
        update.run();
        if (sqlite3_changes(db) == 0) throwUnknown(kOperation, layer);
    });
}

} // namespace slideio::processor
