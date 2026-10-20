#include <slideio/processor/storage/event_journal.hpp>

#include "storage/detail/schema.hpp"
#include "storage/detail/sqlite.hpp"
#include "storage/detail/store_impl.hpp"

#include <slideio/processor/core/error.hpp>

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace slideio::processor {

// ---- Event ---------------------------------------------------------------------------

struct Event::Impl {
    int64_t sequence = 0;
    EventKind kind = EventKind::LayerDeclared;
    int64_t timestampUnixMs = 0;
    StageId stage;
    LayerId layer;
    LabelId label;
    int64_t objectCount = 0;
    Rect64 boundingBox{0, 0, 0, 0};
    StageStatus stageStatus = StageStatus::Pending;
};

namespace detail {

// Builds an Event from a journal row; the only way a library event gets its fields.
struct EventAccess {
    // Decodes the row `select` is on: seq, kind, stage_id, layer_id, label_id,
    // object_count, min_x, min_y, max_x, max_y, stage_status, timestamp_ms.
    static Event fromRow(const Statement& select) {
        auto fields = std::make_unique<Event::Impl>();
        fields->sequence = select.columnInt64(0);
        fields->kind = static_cast<EventKind>(select.columnInt64(1));
        fields->stage = StageId(static_cast<int32_t>(select.columnInt64(2)));  // NULL reads 0: invalid
        fields->layer = LayerId(static_cast<int32_t>(select.columnInt64(3)));
        fields->label = LabelId(static_cast<int32_t>(select.columnInt64(4)));
        fields->objectCount = select.columnInt64(5);
        const int64_t minX = select.columnInt64(6);
        const int64_t minY = select.columnInt64(7);
        fields->boundingBox = Rect64{minX, minY, select.columnInt64(8) - minX, select.columnInt64(9) - minY};
        if (!select.columnIsNull(10)) fields->stageStatus = static_cast<StageStatus>(select.columnInt64(10));
        fields->timestampUnixMs = select.columnInt64(11);
        Event event;
        event.impl_ = std::move(fields);
        return event;
    }
};

} // namespace detail

Event::Event() noexcept = default;

Event::Event(const Event& other) : impl_(other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr) {}

Event::Event(Event&& other) noexcept : impl_(std::move(other.impl_)) { other.impl_.reset(); }

Event& Event::operator=(const Event& other) {
    if (this != &other) {
        auto copy = other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr;
        impl_ = std::move(copy);
    }
    return *this;
}

Event& Event::operator=(Event&& other) noexcept {
    if (this != &other) {
        impl_ = std::move(other.impl_);
        other.impl_.reset();
    }
    return *this;
}

Event::~Event() = default;

int64_t Event::sequence() const noexcept { return impl_ ? impl_->sequence : 0; }
EventKind Event::kind() const noexcept { return impl_ ? impl_->kind : EventKind::LayerDeclared; }
int64_t Event::timestampUnixMs() const noexcept { return impl_ ? impl_->timestampUnixMs : 0; }
StageId Event::stage() const noexcept { return impl_ ? impl_->stage : StageId(); }
LayerId Event::layer() const noexcept { return impl_ ? impl_->layer : LayerId(); }
LabelId Event::label() const noexcept { return impl_ ? impl_->label : LabelId(); }
int64_t Event::objectCount() const noexcept { return impl_ ? impl_->objectCount : 0; }
Rect64 Event::boundingBox() const noexcept { return impl_ ? impl_->boundingBox : Rect64{0, 0, 0, 0}; }
StageStatus Event::stageStatus() const noexcept { return impl_ ? impl_->stageStatus : StageStatus::Pending; }

EventCursor::~EventCursor() = default;

const int64_t EventJournal::kFromBeginning = -1;
const int64_t EventJournal::kFromNow = -2;

namespace {

constexpr int kPageRows = 256;

// One read transaction on a connection: BEGIN now, COMMIT on commit(), ROLLBACK when
// destroyed first. Each journal page, and each multi-statement read, is one of these.
class ReadTransaction {
public:
    explicit ReadTransaction(sqlite3* db) : db_(db) {
        detail::exec(db_, "BEGIN");
        active_ = true;
    }
    ReadTransaction(const ReadTransaction&) = delete;
    ReadTransaction& operator=(const ReadTransaction&) = delete;
    ~ReadTransaction() {
        if (active_) sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    void commit() {
        detail::exec(db_, "COMMIT");
        active_ = false;
    }

private:
    sqlite3* db_;
    bool active_ = false;
};

// The last sequence ever assigned (AUTOINCREMENT keeps it past truncation), or 0.
int64_t newestOn(sqlite3* db) {
    detail::Statement select(db, "SELECT seq FROM sqlite_sequence WHERE name = 'events'");
    return select.step() ? select.columnInt64(0) : 0;
}

// The first retained sequence: no event below it survives.
int64_t watermarkOn(sqlite3* db) {
    return std::max<int64_t>(1, detail::StoreImpl::metaInt64(db, detail::kMetaJournalTruncatedBelow).value_or(1));
}

int64_t oldestOn(sqlite3* db, int64_t newest) {
    detail::Statement select(db, "SELECT MIN(seq) FROM events");
    if (select.step() && !select.columnIsNull(0)) return select.columnInt64(0);
    return newest + 1;
}

// Throws InvalidArgumentError when events above `position` were truncated away: a
// subscriber is told so, never silently moved past the gap (UR-OBS-7).
void throwIfTruncated(sqlite3* db, int64_t position) {
    const int64_t watermark = watermarkOn(db);
    if (position + 1 < watermark) {
        const std::string message = "EventJournal: position " + std::to_string(position) +
                                    " has been truncated away; the journal now begins at sequence " +
                                    std::to_string(watermark);
        throw InvalidArgumentError(message.c_str());
    }
}

class EventCursorImpl final : public EventCursor {
public:
    EventCursorImpl(std::string path, detail::ReadLease&& lease, int64_t start, int32_t pollIntervalMs,
                    std::shared_ptr<detail::CommitWaiter> waiter)
        : path_(std::move(path)),
          lease_(std::move(lease)),
          position_(start),
          pollIntervalMs_(std::max<int32_t>(1, pollIntervalMs)),
          waiter_(std::move(waiter)) {}

    bool next(int32_t timeoutMs) override {
        if (timeoutMs < -1) {
            const std::string message =
                "EventCursor::next: timeoutMs is " + std::to_string(timeoutMs) + "; use -1, 0 or a positive wait";
            throw InvalidArgumentError(message.c_str());
        }
        using Clock = std::chrono::steady_clock;
        const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(timeoutMs > 0 ? timeoutMs : 0);
        for (;;) {
            checkUsable();
            // A buffered page was read before any truncation; the watermark is checked anyway,
            // so a cursor behind it is told rather than served rows the journal has dropped.
            if (index_ < page_.size()) throwIfTruncated(lease_->get(), position_);
            if (index_ >= page_.size() && !fetchPage()) {
                if (timeoutMs == 0) return false;
                int32_t slice = pollIntervalMs_;
                if (timeoutMs > 0) {
                    const auto remaining =
                        std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now()).count();
                    if (remaining <= 0) return false;
                    slice = static_cast<int32_t>(std::min<int64_t>(remaining, pollIntervalMs_));
                }
                waiter_->wait(slice);
                continue;
            }
            current_ = std::move(page_[index_++]);
            position_ = current_.sequence();
            return true;
        }
    }

    const Event& current() const override { return current_; }
    int64_t position() const override { return position_; }

    void close() override {
        page_.clear();
        index_ = 0;
        lease_.reset();  // the pool rolls back anything still open
    }

private:
    void checkUsable() const {
        if (!lease_) {
            const std::string message = std::string("EventCursor::next: the cursor is closed ('") + path_ + "')";
            throw IoError(message.c_str());
        }
        lease_->checkStoreOpen("EventCursor::next", path_);
    }

    // Reads the next page above the last row buffered, in its own read transaction.
    // Returns false when nothing is waiting.
    bool fetchPage() {
        page_.clear();
        index_ = 0;
        sqlite3* db = lease_->get();
        ReadTransaction transaction(db);
        throwIfTruncated(db, position_);
        detail::Statement select(db,
                                 "SELECT seq, kind, stage_id, layer_id, label_id, object_count, min_x, min_y, "
                                 "max_x, max_y, stage_status, timestamp_ms FROM events WHERE seq > ? "
                                 "ORDER BY seq LIMIT ?");
        select.bindInt64(1, position_);
        select.bindInt64(2, kPageRows);
        while (select.step()) page_.push_back(detail::EventAccess::fromRow(select));
        transaction.commit();
        return !page_.empty();
    }

    std::string path_;
    std::optional<detail::ReadLease> lease_;
    int64_t position_;  // the sequence last returned, or the resolved start
    int32_t pollIntervalMs_;
    std::shared_ptr<detail::CommitWaiter> waiter_;
    std::vector<Event> page_;
    size_t index_ = 0;
    Event current_;
};

} // namespace

// ---- EventJournal --------------------------------------------------------------------

class EventJournal::Impl {
public:
    explicit Impl(detail::StoreImpl& s) : store(s) {}
    detail::StoreImpl& store;
};

EventJournal::EventJournal(detail::StoreImpl& store) : impl_(std::make_unique<Impl>(store)) {}

EventJournal::~EventJournal() = default;

int64_t EventJournal::newestSequence() const {
    return impl_->store.withReadConnection("EventJournal::newestSequence", [](sqlite3* db) { return newestOn(db); });
}

int64_t EventJournal::oldestSequence() const {
    return impl_->store.withReadConnection("EventJournal::oldestSequence", [](sqlite3* db) {
        ReadTransaction transaction(db);
        const int64_t newest = newestOn(db);
        const int64_t oldest = oldestOn(db, newest);
        transaction.commit();
        return oldest;
    });
}

int64_t EventJournal::eventCount() const {
    return impl_->store.withReadConnection("EventJournal::eventCount", [](sqlite3* db) {
        detail::Statement count(db, "SELECT COUNT(*) FROM events");
        return count.step() ? count.columnInt64(0) : int64_t(0);
    });
}

EventCursorPtr EventJournal::subscribe(int64_t fromSequence) {
    detail::StoreImpl& store = impl_->store;
    if (fromSequence < 0 && fromSequence != kFromBeginning && fromSequence != kFromNow) {
        const std::string message = "EventJournal::subscribe: position " + std::to_string(fromSequence) +
                                    " is not a sequence; use a sequence from position(), kFromBeginning (" +
                                    std::to_string(kFromBeginning) + ") or kFromNow (" +
                                    std::to_string(kFromNow) + ")";
        throw InvalidArgumentError(message.c_str());
    }
    store.checkOpen("EventJournal::subscribe");
    detail::ReadLease lease = store.acquireRead("EventJournal::subscribe");
    int64_t start = 0;
    {
        sqlite3* db = lease.get();
        ReadTransaction transaction(db);
        const int64_t watermark = watermarkOn(db);
        if (fromSequence == kFromBeginning) {
            start = watermark - 1;
        } else if (fromSequence == kFromNow) {
            start = newestOn(db);
        } else {
            throwIfTruncated(db, fromSequence);
            start = fromSequence;
        }
        transaction.commit();
    }
    return std::make_shared<EventCursorImpl>(store.path(), std::move(lease), start,
                                             store.configuration().eventPollIntervalMs(), store.commitWaiter());
}

void EventJournal::truncateBelow(int64_t sequence) {
    impl_->store.withWriteTransaction("EventJournal::truncateBelow", [sequence](sqlite3* db) {
        const int64_t newest = newestOn(db);
        const int64_t watermark = std::max(watermarkOn(db), std::min(sequence, newest + 1));
        detail::Statement remove(db, "DELETE FROM events WHERE seq < ?");
        remove.bindInt64(1, watermark);
        remove.run();
        detail::StoreImpl::setMetaInt64(db, detail::kMetaJournalTruncatedBelow, watermark);
    });
}

} // namespace slideio::processor
