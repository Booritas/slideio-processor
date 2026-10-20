// Requirements: UR-OBS-2, UR-OBS-3, UR-OBS-7, UR-OBS-6
#pragma once

/**
 * @file event_journal.hpp
 * @brief Event, EventCursor, EventCursorPtr and EventJournal: the store's event journal.
 */

#include <slideio/processor/core/export.hpp>
#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/core/ids.hpp>
#include <slideio/processor/storage/types.hpp>

#include <cstdint>
#include <memory>

namespace slideio::processor {

namespace detail {
class StoreImpl;
struct EventAccess;
} // namespace detail

/**
 * @brief One record of the event journal: what happened, where, and how much.
 *
 * An event carries enough to decide whether to look further and never geometry.
 * A default-constructed event, and a moved-from one, reads as no event: sequence 0,
 * kind LayerDeclared, invalid ids, count 0, an empty bounding box and status Pending.
 * @par Thread-safety
 * ThreadCompatible: const members may run concurrently; assignment needs exclusive
 * access.
 * @par Requirements
 * UR-OBS-2, UR-OBS-3
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS Event {
public:
    /**
     * @brief Builds an event that reads as no event.
     * @throws Nothing.
     */
    Event() noexcept;
    /**
     * @brief Copies an event.
     * @param other The event to copy.
     * @throws std::bad_alloc When memory is exhausted.
     */
    Event(const Event& other);
    /**
     * @brief Moves an event; `other` then reads as a default event.
     * @param other The event to move from.
     * @throws Nothing.
     */
    Event(Event&& other) noexcept;
    /**
     * @brief Copies an event.
     * @param other The event to copy.
     * @return This event.
     * @throws std::bad_alloc When memory is exhausted; this event is then unchanged.
     */
    Event& operator=(const Event& other);
    /**
     * @brief Moves an event; `other` then reads as a default event.
     * @param other The event to move from.
     * @return This event.
     * @throws Nothing.
     */
    Event& operator=(Event&& other) noexcept;
    /** @brief Releases the event. */
    ~Event();

    /**
     * @brief Returns the position of the event in the journal.
     * @return The sequence, from 1 and never reused; 0 for a default event.
     * @throws Nothing.
     */
    int64_t sequence() const noexcept;
    /**
     * @brief Returns what happened.
     * @return The kind.
     * @throws Nothing.
     */
    EventKind kind() const noexcept;
    /**
     * @brief Returns when the event was recorded.
     * @return Milliseconds since the Unix epoch.
     * @throws Nothing.
     */
    int64_t timestampUnixMs() const noexcept;

    /**
     * @brief Returns the stage concerned.
     * @return The stage, or an invalid id where not applicable.
     * @throws Nothing.
     */
    StageId stage() const noexcept;
    /**
     * @brief Returns the layer concerned.
     * @return The layer, or an invalid id where not applicable.
     * @throws Nothing.
     */
    LayerId layer() const noexcept;
    /**
     * @brief Returns the label concerned.
     * @return The label, or an invalid id where not applicable.
     * @throws Nothing.
     */
    LabelId label() const noexcept;

    /**
     * @brief Returns the size of the batch.
     * @return The object or edge count of an ObjectsAdded or EdgesAsserted event, the
     * number of objects removed for StageDiscarded, otherwise 0.
     * @throws Nothing.
     */
    int64_t objectCount() const noexcept;
    /**
     * @brief Returns the union bounding box of the batch.
     * @return The box, empty for an event of a kind that carries none.
     * @throws Nothing.
     */
    Rect64 boundingBox() const noexcept;
    /**
     * @brief Returns the status a StageStateChanged event entered.
     * @return The status; Pending for any other kind.
     * @throws Nothing.
     */
    StageStatus stageStatus() const noexcept;

private:
    friend struct detail::EventAccess;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * @brief A subscriber's position in the event journal.
 *
 * A cursor holds one store connection for its lifetime and reads the journal in
 * pages, each page in its own read transaction, so events committed meanwhile become
 * visible. It may outlive the Store that produced it: next() then throws IoError.
 * @par Thread-safety
 * ThreadConfined: one thread at a time uses a cursor; distinct cursors are
 * independent.
 * @par Requirements
 * UR-OBS-2, UR-OBS-3, UR-OBS-6, UR-OBS-7
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS EventCursor {
public:
    /** @brief Releases the cursor's connection. */
    virtual ~EventCursor();

    /**
     * @brief Waits up to `timeoutMs` for the next event.
     *
     * Checks the journal, and while nothing is waiting sleeps at most one poll
     * interval (Configuration::eventPollIntervalMs) and checks again. Correctness never
     * depends on a wake-up signal: a polling subscriber observes the identical
     * sequence.
     * @param timeoutMs 0 polls without blocking, -1 waits indefinitely, a positive
     * value waits at most that many milliseconds.
     * @return True when current() is the next event; false on timeout.
     * @throws InvalidArgumentError When timeoutMs is below -1.
     * @throws IoError When the cursor or the store is closed, or when SQLite fails.
     * @throws std::bad_alloc When memory is exhausted.
     */
    virtual bool next(int32_t timeoutMs) = 0;
    /**
     * @brief Returns the event last returned by next().
     * @return The event; a default event before the first successful next(). Valid
     * until the next call to next().
     * @throws Nothing.
     */
    virtual const Event& current() const = 0;

    /**
     * @brief Returns the sequence last returned.
     *
     * Persist it to resume in a later process with EventJournal::subscribe.
     * @return The sequence of current(); before the first next(), the position the
     * subscription started from.
     * @throws Nothing.
     */
    virtual int64_t position() const = 0;
    /**
     * @brief Releases the cursor's connection. Idempotent; next() then throws IoError.
     * @throws Nothing.
     */
    virtual void close() = 0;
};

/** @brief Shared ownership of an EventCursor. */
using EventCursorPtr = std::shared_ptr<EventCursor>;

/**
 * @brief The store's event journal: rows ordered by sequence, read by cursors.
 *
 * A subscriber holds one integer and asks for events above it, which gives late join,
 * replay from any point and resumption after a restart, with no subscriber list on
 * the writer's side. A subscriber that falls behind causes no backpressure and no
 * drops.
 * @par Thread-safety
 * ThreadSafe: every member may be called from any thread with no external locking.
 * @par Requirements
 * UR-OBS-2, UR-OBS-3, UR-OBS-6, UR-OBS-7
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS EventJournal {
public:
    /** @brief Subscribe position meaning "replay everything retained" (-1). */
    static const int64_t kFromBeginning;
    /** @brief Subscribe position meaning "only events after subscribing" (-2). */
    static const int64_t kFromNow;

    /** @brief Releases the journal handle. */
    ~EventJournal();
    EventJournal(const EventJournal&) = delete;             ///< Not copyable.
    EventJournal& operator=(const EventJournal&) = delete;  ///< Not copyable.

    /**
     * @brief Returns the smallest retained sequence.
     * @return The sequence, or newestSequence() + 1 when none is retained (1 for a new
     * store).
     * @throws IoError When the store is closed, or when SQLite fails.
     */
    int64_t oldestSequence() const;
    /**
     * @brief Returns the last sequence ever assigned.
     * @return The sequence, or 0 when none was assigned. Truncation does not lower it.
     * @throws IoError When the store is closed, or when SQLite fails.
     */
    int64_t newestSequence() const;

    /**
     * @brief Begins a cursor at the first event with sequence above `fromSequence`.
     * @param fromSequence A sequence previously returned by position(), or
     * kFromBeginning, or kFromNow.
     * @return The cursor, positioned before its first event.
     * @throws InvalidArgumentError When `fromSequence` is negative and neither
     * kFromBeginning nor kFromNow; or when events above it were truncated away (the
     * message names both positions).
     * @throws IoError When the store is closed, or when SQLite fails.
     * @throws std::bad_alloc When memory is exhausted.
     */
    EventCursorPtr subscribe(int64_t fromSequence);

    /**
     * @brief Discards events below `sequence`.
     *
     * Objects, edges and stage state are unaffected. The watermark only rises, and a
     * value beyond newestSequence() + 1 is taken as newestSequence() + 1, so sequences
     * assigned later stay retained.
     * @param sequence The smallest sequence to keep.
     * @throws IoError When the store is closed or read-only, or when SQLite fails.
     */
    void truncateBelow(int64_t sequence);
    /**
     * @brief Returns the number of retained events.
     * @return The count.
     * @throws IoError When the store is closed, or when SQLite fails.
     */
    int64_t eventCount() const;

private:
    friend class Store;
    explicit EventJournal(detail::StoreImpl& store);
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace slideio::processor
