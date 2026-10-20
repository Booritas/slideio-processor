#include <slideio/processor/storage/stage_state_reader.hpp>

#include "storage/detail/sqlite.hpp"
#include "storage/detail/store_impl.hpp"

#include <slideio/processor/core/error.hpp>

#include <mutex>
#include <set>
#include <string>

namespace slideio::processor {

class StageStateReader::Impl {
public:
    explicit Impl(detail::StoreImpl& s) : store(s) {}

    // Keeps a copy of `text` alive for the life of the reader and returns it. Equal
    // values share one node; std::set nodes never move, so every pointer handed out
    // stays valid.
    const char* intern(std::string text) {
        std::lock_guard<std::mutex> guard(mutex);
        return pool.insert(std::move(text)).first->c_str();
    }

    [[noreturn]] static void throwUnknown(const char* operation, StageId stage) {
        const std::string message =
            std::string(operation) + ": there is no stage " + std::to_string(stage.value());
        throw InvalidArgumentError(message.c_str());
    }

    [[noreturn]] static void throwOutOfRange(const char* operation, StageId stage, int32_t index) {
        const std::string message = std::string(operation) + ": index " + std::to_string(index) +
                                    " is out of range for stage " + std::to_string(stage.value());
        throw InvalidArgumentError(message.c_str());
    }

    // Runs `select` (which must return one row for an existing stage, binding the stage
    // id as parameter 1) and hands the row to `read`. Throws InvalidArgumentError when no
    // row comes back.
    template <class Read>
    auto readStage(const char* operation, StageId stage, const char* sql, Read&& read) const {
        return store.withReadConnection(operation, [&](sqlite3* db) {
            detail::Statement select(db, sql);
            select.bindInt64(1, stage.value());
            if (!select.step()) throwUnknown(operation, stage);
            return read(select);
        });
    }

    detail::StoreImpl& store;
    std::mutex mutex;
    std::set<std::string> pool;
};

StageStateReader::StageStateReader(detail::StoreImpl& store) : impl_(std::make_unique<Impl>(store)) {}

StageStateReader::~StageStateReader() = default;

int32_t StageStateReader::count() const noexcept {
    try {
        return impl_->store.withReadConnection("StageStateReader::count", [](sqlite3* db) {
            detail::Statement select(db, "SELECT COUNT(*) FROM stages");
            return select.step() ? static_cast<int32_t>(select.columnInt64(0)) : 0;
        });
    } catch (...) {
        return 0;
    }
}

StageId StageStateReader::at(int32_t index) const {
    constexpr const char* kOperation = "StageStateReader::at";
    return impl_->store.withReadConnection(kOperation, [&](sqlite3* db) {
        detail::Statement select(db, "SELECT id FROM stages ORDER BY id LIMIT 1 OFFSET ?");
        select.bindInt64(1, index);
        if (index < 0 || !select.step()) {
            const std::string message =
                std::string(kOperation) + ": index " + std::to_string(index) + " is out of range";
            throw InvalidArgumentError(message.c_str());
        }
        return StageId(static_cast<int32_t>(select.columnInt64(0)));
    });
}

bool StageStateReader::find(const char* name, StageId& out) const noexcept {
    if (name == nullptr || *name == '\0') return false;
    try {
        const std::string wanted = name;
        return impl_->store.withReadConnection("StageStateReader::find", [&](sqlite3* db) {
            detail::Statement select(db, "SELECT id FROM stages WHERE name = ?");
            select.bindText(1, wanted);
            if (!select.step()) return false;
            out = StageId(static_cast<int32_t>(select.columnInt64(0)));
            return true;
        });
    } catch (...) {
        return false;
    }
}

const char* StageStateReader::name(StageId stage) const {
    std::string text = impl_->readStage("StageStateReader::name", stage, "SELECT name FROM stages WHERE id = ?",
                                        [](detail::Statement& s) { return s.columnText(0); });
    return impl_->intern(std::move(text));
}

StageStatus StageStateReader::status(StageId stage) const {
    return impl_->readStage("StageStateReader::status", stage, "SELECT status FROM stages WHERE id = ?",
                            [](detail::Statement& s) { return static_cast<StageStatus>(s.columnInt64(0)); });
}

bool StageStateReader::isStale(StageId stage) const {
    return impl_->readStage("StageStateReader::isStale", stage, "SELECT stale FROM stages WHERE id = ?",
                            [](detail::Statement& s) { return s.columnInt64(0) != 0; });
}

const char* StageStateReader::configuration(StageId stage) const {
    std::string text =
        impl_->readStage("StageStateReader::configuration", stage, "SELECT configuration FROM stages WHERE id = ?",
                         [](detail::Statement& s) { return s.columnText(0); });
    return impl_->intern(std::move(text));
}

int64_t StageStateReader::completedAtUnixMs(StageId stage) const {
    return impl_->readStage("StageStateReader::completedAtUnixMs", stage,
                            "SELECT completed_at_ms FROM stages WHERE id = ?",
                            [](detail::Statement& s) { return s.columnInt64(0); });
}

int32_t StageStateReader::dependencyCount(StageId stage) const {
    return impl_->readStage(
        "StageStateReader::dependencyCount", stage,
        "SELECT (SELECT COUNT(*) FROM stage_deps WHERE stage_id = stages.id) FROM stages WHERE id = ?",
        [](detail::Statement& s) { return static_cast<int32_t>(s.columnInt64(0)); });
}

StageId StageStateReader::dependencyAt(StageId stage, int32_t index) const {
    constexpr const char* kOperation = "StageStateReader::dependencyAt";
    // One statement, so the stage and the dependency are read from one snapshot.
    return impl_->store.withReadConnection(kOperation, [&](sqlite3* db) {
        detail::Statement select(
            db,
            "SELECT EXISTS(SELECT 1 FROM stages WHERE id = ?1), (SELECT depends_on_id FROM stage_deps "
            "WHERE stage_id = ?1 ORDER BY depends_on_id LIMIT 1 OFFSET ?2)");
        select.bindInt64(1, stage.value());
        select.bindInt64(2, index);
        if (!select.step() || select.columnInt64(0) == 0) Impl::throwUnknown(kOperation, stage);
        if (index < 0 || select.columnIsNull(1)) Impl::throwOutOfRange(kOperation, stage, index);
        return StageId(static_cast<int32_t>(select.columnInt64(1)));
    });
}

int32_t StageStateReader::writtenLayerCount(StageId stage) const {
    return impl_->readStage(
        "StageStateReader::writtenLayerCount", stage,
        "SELECT (SELECT COUNT(*) FROM stage_layers WHERE stage_id = stages.id) FROM stages WHERE id = ?",
        [](detail::Statement& s) { return static_cast<int32_t>(s.columnInt64(0)); });
}

LayerId StageStateReader::writtenLayerAt(StageId stage, int32_t index) const {
    constexpr const char* kOperation = "StageStateReader::writtenLayerAt";
    return impl_->store.withReadConnection(kOperation, [&](sqlite3* db) {
        detail::Statement select(
            db,
            "SELECT EXISTS(SELECT 1 FROM stages WHERE id = ?1), (SELECT layer_id FROM stage_layers "
            "WHERE stage_id = ?1 ORDER BY layer_id LIMIT 1 OFFSET ?2)");
        select.bindInt64(1, stage.value());
        select.bindInt64(2, index);
        if (!select.step() || select.columnInt64(0) == 0) Impl::throwUnknown(kOperation, stage);
        if (index < 0 || select.columnIsNull(1)) Impl::throwOutOfRange(kOperation, stage, index);
        return LayerId(static_cast<int32_t>(select.columnInt64(1)));
    });
}

} // namespace slideio::processor
