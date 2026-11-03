#include <slideio/processor/storage/vocabulary_registry.hpp>

#include "storage/detail/event_log.hpp"
#include "storage/detail/label_aliases.hpp"
#include "storage/detail/schema.hpp"
#include "storage/detail/sqlite.hpp"
#include "storage/detail/store_impl.hpp"

#include <slideio/processor/config/standard_vocabulary.hpp>
#include <slideio/processor/core/error.hpp>

#include <algorithm>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace slideio::processor {

namespace {

[[noreturn]] void throwInvalid(const char* operation, const std::string& what) {
    const std::string message = std::string(operation) + ": " + what;
    throw InvalidArgumentError(message.c_str());
}

[[noreturn]] void throwUnknown(const char* operation, LabelId label) {
    throwInvalid(operation, "there is no label " + std::to_string(label.value()));
}

// The id of the lowest-numbered label whose name equals `folded` once ASCII case is
// folded (SQLite's lower() folds ASCII only), read on `db`.
std::optional<int32_t> lookupFolded(sqlite3* db, const std::string& folded) {
    detail::Statement select(db, "SELECT id FROM labels WHERE lower(name) = ? ORDER BY id LIMIT 1");
    select.bindText(1, folded);
    if (!select.step()) return std::nullopt;
    return static_cast<int32_t>(select.columnInt64(0));
}

// Resolves a name against the table itself: exact fold first, then the QuPath alias.
std::optional<int32_t> resolveInTable(sqlite3* db, const char* name) {
    const std::string folded = detail::foldAscii(name);
    if (const std::optional<int32_t> id = lookupFolded(db, folded)) return id;
    if (const char* canonical = detail::canonicalForAlias(folded)) return lookupFolded(db, canonical);
    return std::nullopt;
}

} // namespace

class VocabularyRegistry::Impl {
public:
    // A label row. Labels are append-only and never renamed, so the whole row is cached.
    struct Row {
        LabelId id;
        std::string name;
        LabelOrigin origin = LabelOrigin::Standard;
    };

    explicit Impl(detail::StoreImpl& s) : store(s) {}

    // Adds to the cache every row the table holds beyond it, when the table's row count
    // differs from the cache's. Labels are append-only, so the count suffices. A row
    // may arrive with an id below ones already cached (a newer standard label written
    // by another process after a local label), so every row is checked by id.
    // Caller holds `mutex`.
    void refresh(const char* operation) {
        store.withReadConnection(operation, [&](sqlite3* db) {
            int64_t stored = 0;
            {
                detail::Statement count(db, "SELECT COUNT(*) FROM labels");
                if (count.step()) stored = count.columnInt64(0);
            }
            if (stored == static_cast<int64_t>(rows.size())) return;
            detail::Statement select(db, "SELECT id, name, origin FROM labels ORDER BY id");
            bool added = false;
            while (select.step()) {
                const int32_t id = static_cast<int32_t>(select.columnInt64(0));
                if (byId.count(id) != 0) continue;
                Row row;
                row.id = LabelId(id);
                row.name = select.columnText(1);
                row.origin = static_cast<LabelOrigin>(select.columnInt64(2));
                rows.push_back(std::move(row));  // deque: earlier rows (and their names) never move
                Row& cached = rows.back();
                byId[id] = &cached;
                const auto folded = byFolded.emplace(detail::foldAscii(cached.name.c_str()), &cached);
                if (!folded.second && id < folded.first->second->id.value()) folded.first->second = &cached;
                added = true;
            }
            if (added) {
                order.clear();
                for (const auto& entry : byId) order.push_back(entry.second->id);
            }
        });
    }

    template <class Read>
    auto withRow(const char* operation, LabelId label, Read&& read) {
        std::lock_guard<std::mutex> guard(mutex);
        refresh(operation);
        const auto found = byId.find(label.value());
        if (found == byId.end()) throwUnknown(operation, label);
        return read(*found->second);
    }

    // The metadata value at `key`, read on a fresh connection call.
    std::optional<int64_t> readMeta(const char* operation, const char* key) const {
        return store.withReadConnection(operation, [&](sqlite3* db) { return detail::StoreImpl::metaInt64(db, key); });
    }

    detail::StoreImpl& store;
    std::mutex mutex;
    std::deque<Row> rows;
    std::map<int32_t, Row*> byId;                // ascending id
    std::map<std::string, Row*> byFolded;        // ASCII-folded name -> lowest-id row
    std::vector<LabelId> order;                  // ascending id, for at()
};

VocabularyRegistry::VocabularyRegistry(detail::StoreImpl& store) : impl_(std::make_unique<Impl>(store)) {}

VocabularyRegistry::~VocabularyRegistry() = default;

int32_t VocabularyRegistry::recordedStandardVersion() const noexcept {
    try {
        const std::optional<int64_t> version =
            impl_->readMeta("VocabularyRegistry::recordedStandardVersion", detail::kMetaVocabularyVersion);
        return version ? static_cast<int32_t>(*version) : 0;
    } catch (...) {
        return 0;
    }
}

LabelId VocabularyRegistry::addLocal(const char* name) {
    constexpr const char* kOperation = "VocabularyRegistry::addLocal";
    if (name == nullptr || *name == '\0') throwInvalid(kOperation, "the label name is null or empty");
    bool added = false;
    const LabelId id = impl_->store.withWriteTransaction(kOperation, [&](sqlite3* db) {
        // Resolve against the table inside the write transaction, so a label another
        // handle has just added is found rather than duplicated.
        if (const std::optional<int32_t> existing = resolveInTable(db, name)) return LabelId(*existing);
        int64_t next = detail::kFirstLocalLabelId;
        {
            detail::Statement max(db, "SELECT COALESCE(MAX(id), 0) FROM labels");
            if (max.step()) next = std::max<int64_t>(next, max.columnInt64(0) + 1);
        }
        {
            detail::Statement insert(db, "INSERT INTO labels(id, name, origin) VALUES (?, ?, ?)");
            insert.bindInt64(1, next);
            insert.bindText(2, name);
            insert.bindInt64(3, static_cast<int64_t>(LabelOrigin::WorkspaceLocal));
            insert.run();
        }
        detail::EventRow event;
        event.kind = EventKind::LabelAdded;
        event.labelId = static_cast<int32_t>(next);
        detail::appendEvent(db, event);
        added = true;
        return LabelId(static_cast<int32_t>(next));
    });
    if (added) impl_->store.notifyCommitted();
    return id;
}

bool VocabularyRegistry::find(const char* name, LabelId& out) const noexcept {
    if (name == nullptr || *name == '\0') return false;
    try {
        const std::string folded = detail::foldAscii(name);
        std::lock_guard<std::mutex> guard(impl_->mutex);
        impl_->refresh("VocabularyRegistry::find");
        auto found = impl_->byFolded.find(folded);
        if (found == impl_->byFolded.end()) {
            const char* canonical = detail::canonicalForAlias(folded);
            if (canonical != nullptr) found = impl_->byFolded.find(canonical);
        }
        if (found == impl_->byFolded.end()) return false;
        out = found->second->id;
        return true;
    } catch (...) {
        return false;
    }
}

const char* VocabularyRegistry::name(LabelId label) const {
    return impl_->withRow("VocabularyRegistry::name", label, [](const Impl::Row& row) { return row.name.c_str(); });
}

LabelOrigin VocabularyRegistry::origin(LabelId label) const {
    return impl_->withRow("VocabularyRegistry::origin", label, [](const Impl::Row& row) { return row.origin; });
}

bool VocabularyRegistry::isRecognised(LabelId label) const noexcept {
    try {
        std::lock_guard<std::mutex> guard(impl_->mutex);
        impl_->refresh("VocabularyRegistry::isRecognised");
        if (impl_->byId.count(label.value()) == 0) return false;
        // Ids below kFirstLocalLabelId are standard; one above the library's table is from
        // a newer vocabulary (ruling S7).
        return label.value() >= detail::kFirstLocalLabelId ||
               label.value() <= StandardVocabulary::instance().labelCount();
    } catch (...) {
        return false;
    }
}

int32_t VocabularyRegistry::count() const noexcept {
    try {
        std::lock_guard<std::mutex> guard(impl_->mutex);
        impl_->refresh("VocabularyRegistry::count");
        return static_cast<int32_t>(impl_->rows.size());
    } catch (...) {
        return 0;
    }
}

LabelId VocabularyRegistry::at(int32_t index) const {
    constexpr const char* kOperation = "VocabularyRegistry::at";
    std::lock_guard<std::mutex> guard(impl_->mutex);
    impl_->refresh(kOperation);
    if (index < 0 || static_cast<size_t>(index) >= impl_->order.size()) {
        throwInvalid(kOperation, "index " + std::to_string(index) + " is out of range");
    }
    return impl_->order[static_cast<size_t>(index)];
}

ImportPolicy VocabularyRegistry::importPolicy() const noexcept {
    try {
        const std::optional<int64_t> value =
            impl_->readMeta("VocabularyRegistry::importPolicy", detail::kMetaImportPolicy);
        if (value && *value == static_cast<int64_t>(ImportPolicy::Permissive)) return ImportPolicy::Permissive;
        return ImportPolicy::Strict;
    } catch (...) {
        return ImportPolicy::Strict;
    }
}

void VocabularyRegistry::setImportPolicy(ImportPolicy policy) {
    constexpr const char* kOperation = "VocabularyRegistry::setImportPolicy";
    impl_->store.withWriteTransaction(kOperation, [&](sqlite3* db) {
        if (policy != ImportPolicy::Strict && policy != ImportPolicy::Permissive) {
            throwInvalid(kOperation, "the import policy " + std::to_string(static_cast<int32_t>(policy)) +
                                         " is not an ImportPolicy");
        }
        detail::StoreImpl::setMetaInt64(db, detail::kMetaImportPolicy, static_cast<int64_t>(policy));
    });
}

} // namespace slideio::processor
