#include <slideio/processor/storage/stage_state_writer.hpp>

#include "storage/detail/event_log.hpp"
#include "storage/detail/sqlite.hpp"
#include "storage/detail/store_impl.hpp"

#include <slideio/processor/core/error.hpp>

#include <chrono>
#include <string>

namespace slideio::processor {

namespace {

constexpr int32_t kPixelLayerId = 1;

[[noreturn]] void throwInvalid(const char* operation, const std::string& what) {
    const std::string message = std::string(operation) + ": " + what;
    throw InvalidArgumentError(message.c_str());
}

void requireStage(sqlite3* db, const char* operation, StageId stage) {
    detail::Statement select(db, "SELECT 1 FROM stages WHERE id = ?");
    select.bindInt64(1, stage.value());
    if (!select.step()) throwInvalid(operation, "there is no stage " + std::to_string(stage.value()));
}

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

class StageStateWriter::Impl {
public:
    explicit Impl(detail::StoreImpl& s) : store(s) {}
    detail::StoreImpl& store;
};

StageStateWriter::StageStateWriter(detail::StoreImpl& store) : impl_(std::make_unique<Impl>(store)) {}

StageStateWriter::~StageStateWriter() = default;

StageId StageStateWriter::declare(const char* name, const char* configurationJson) {
    constexpr const char* kOperation = "StageStateWriter::declare";
    return impl_->store.withWriteTransaction(kOperation, [&](sqlite3* db) {
        if (name == nullptr || *name == '\0') throwInvalid(kOperation, "the stage name is null or empty");
        const std::string stageName = name;
        {
            detail::Statement existing(db, "SELECT 1 FROM stages WHERE name = ?");
            existing.bindText(1, stageName);
            if (existing.step()) throwInvalid(kOperation, "a stage named '" + stageName + "' already exists");
        }
        detail::Statement insert(db, "INSERT INTO stages(name, configuration) VALUES (?, ?)");
        insert.bindText(1, stageName);
        insert.bindText(2, configurationJson != nullptr ? configurationJson : "");
        insert.run();
        return StageId(static_cast<int32_t>(sqlite3_last_insert_rowid(db)));
    });
}

void StageStateWriter::setStatus(StageId stage, StageStatus status) {
    constexpr const char* kOperation = "StageStateWriter::setStatus";
    bool appended = false;
    impl_->store.withWriteTransaction(kOperation, [&](sqlite3* db) {
        requireStage(db, kOperation, stage);
        const int32_t raw = static_cast<int32_t>(status);
        if (raw < static_cast<int32_t>(StageStatus::Pending) || raw > static_cast<int32_t>(StageStatus::Failed)) {
            throwInvalid(kOperation, "status " + std::to_string(raw) + " is not a StageStatus");
        }
        if (status == StageStatus::Complete) {
            detail::Statement update(db, "UPDATE stages SET status = ?, completed_at_ms = ? WHERE id = ?");
            update.bindInt64(1, raw);
            update.bindInt64(2, nowMs());
            update.bindInt64(3, stage.value());
            update.run();
        } else {
            detail::Statement update(db, "UPDATE stages SET status = ? WHERE id = ?");
            update.bindInt64(1, raw);
            update.bindInt64(2, stage.value());
            update.run();
        }
        detail::EventRow event;
        event.kind = EventKind::StageStateChanged;
        event.stageId = stage.value();
        event.hasStageStatus = true;
        event.stageStatus = status;
        detail::appendEvent(db, event);
        appended = true;
    });
    if (appended) impl_->store.notifyCommitted();
}

void StageStateWriter::setStale(StageId stage, bool stale) {
    constexpr const char* kOperation = "StageStateWriter::setStale";
    impl_->store.withWriteTransaction(kOperation, [&](sqlite3* db) {
        requireStage(db, kOperation, stage);
        detail::Statement update(db, "UPDATE stages SET stale = ? WHERE id = ?");
        update.bindInt64(1, stale ? 1 : 0);
        update.bindInt64(2, stage.value());
        update.run();
    });
}

void StageStateWriter::setConfiguration(StageId stage, const char* configurationJson) {
    constexpr const char* kOperation = "StageStateWriter::setConfiguration";
    impl_->store.withWriteTransaction(kOperation, [&](sqlite3* db) {
        requireStage(db, kOperation, stage);
        detail::Statement update(db, "UPDATE stages SET configuration = ? WHERE id = ?");
        update.bindText(1, configurationJson != nullptr ? configurationJson : "");
        update.bindInt64(2, stage.value());
        update.run();
    });
}

void StageStateWriter::addDependency(StageId stage, StageId dependsOn) {
    constexpr const char* kOperation = "StageStateWriter::addDependency";
    impl_->store.withWriteTransaction(kOperation, [&](sqlite3* db) {
        requireStage(db, kOperation, stage);
        requireStage(db, kOperation, dependsOn);
        if (stage == dependsOn) {
            throwInvalid(kOperation, "stage " + std::to_string(stage.value()) + " cannot depend on itself");
        }
        detail::Statement insert(db, "INSERT OR IGNORE INTO stage_deps(stage_id, depends_on_id) VALUES (?, ?)");
        insert.bindInt64(1, stage.value());
        insert.bindInt64(2, dependsOn.value());
        insert.run();
    });
}

void StageStateWriter::clearDependencies(StageId stage) {
    constexpr const char* kOperation = "StageStateWriter::clearDependencies";
    impl_->store.withWriteTransaction(kOperation, [&](sqlite3* db) {
        requireStage(db, kOperation, stage);
        detail::Statement remove(db, "DELETE FROM stage_deps WHERE stage_id = ?");
        remove.bindInt64(1, stage.value());
        remove.run();
    });
}

void StageStateWriter::addWrittenLayer(StageId stage, LayerId layer) {
    constexpr const char* kOperation = "StageStateWriter::addWrittenLayer";
    impl_->store.withWriteTransaction(kOperation, [&](sqlite3* db) {
        requireStage(db, kOperation, stage);
        if (layer.value() == kPixelLayerId) throwInvalid(kOperation, "the pixel layer is not written by a stage");
        {
            detail::Statement existing(db, "SELECT 1 FROM layers WHERE id = ?");
            existing.bindInt64(1, layer.value());
            if (!existing.step()) throwInvalid(kOperation, "there is no layer " + std::to_string(layer.value()));
        }
        detail::Statement insert(db, "INSERT OR IGNORE INTO stage_layers(layer_id, stage_id) VALUES (?, ?)");
        insert.bindInt64(1, layer.value());
        insert.bindInt64(2, stage.value());
        insert.run();
    });
}

} // namespace slideio::processor
