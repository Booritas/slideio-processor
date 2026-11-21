// Requirements: UR-RES-6
#pragma once

/**
 * @file materialise.hpp
 * @brief Bounded materialisation: reads a filtered query into memory as a set.
 */

#include <slideio/processor/core/export.hpp>
#include <slideio/processor/core/object_query.hpp>
#include <slideio/processor/core/object_record.hpp>

#include <cstdint>
#include <memory>

namespace slideio::processor {

class Store;
class ObjectArray;
/** @brief Shared ownership of an ObjectArray. */
using ObjectArrayPtr = std::shared_ptr<ObjectArray>;

/**
 * @brief Reads a filtered query into memory.
 *
 * The only call of the library that brings objects into memory as a set, and
 * deliberately awkward: it demands a filter and an explicit cap, and throws rather
 * than truncating. There is no overload without a cap (UR-RES-6). Every refusal
 * happens before any record is read. Records are in the cursor's order.
 * @param store The store to read; must be open.
 * @param query A filtered query (ObjectQuery::isUnfiltered() false).
 * @param maxObjects The most records to accept, at least 1.
 * @return The records.
 * @throws InvalidArgumentError When maxObjects is below 1, the query is unfiltered,
 * or it selects more than maxObjects objects; the message carries no counts.
 * @throws IoError When the store is closed, or when SQLite fails.
 * @throws std::bad_alloc When memory is exhausted.
 * @par Thread-safety
 * ThreadSafe with respect to distinct stores; on one store it is as ObjectStore::query().
 * @par Requirements
 * UR-RES-6
 */
SLIDEIO_PROCESSOR_DATA_EXPORTS ObjectArrayPtr materialise(Store& store, const ObjectQuery& query,
                                                          int64_t maxObjects);

/**
 * @brief An immutable set of object records read by materialise().
 *
 * Not constructible by users: materialise() is the only producer.
 * @par Thread-safety
 * ThreadCompatible: concurrent const calls are safe.
 * @par Requirements
 * UR-RES-6
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS ObjectArray {
public:
    /** @brief Releases the records. */
    ~ObjectArray();
    ObjectArray(const ObjectArray&) = delete;             ///< Not copyable.
    ObjectArray& operator=(const ObjectArray&) = delete;  ///< Not copyable.

    /**
     * @brief Returns the number of records.
     * @return The count.
     * @throws Nothing.
     */
    int64_t count() const noexcept;
    /**
     * @brief Returns one record, in the order the query's cursor yielded them.
     * @param index Zero-based, below count().
     * @return The record; valid as long as this array.
     * @throws InvalidArgumentError When index is negative or at least count().
     */
    const ObjectRecord& at(int64_t index) const;

private:
    friend ObjectArrayPtr materialise(Store&, const ObjectQuery&, int64_t);
    ObjectArray();
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace slideio::processor
