// Requirements: UR-EXE-2
#pragma once

/**
 * @file memory_budget.hpp
 * @brief MemoryBudget, a shared byte ceiling, and makeMemoryBudget().
 */

#include <slideio/processor/core/control.hpp>
#include <slideio/processor/core/export.hpp>

#include <cstdint>
#include <memory>

namespace slideio::processor {

/**
 * @brief A shared byte ceiling that raster tiles, object accumulation and
 * store buffers reserve against.
 *
 * Obtain one from makeMemoryBudget(). Reservations are accounting only: the
 * budget allocates nothing.
 * @par Thread-safety
 * ThreadSafe.
 * @par Requirements
 * UR-EXE-2
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS MemoryBudget {
public:
    /** @brief Destroys the budget. */
    virtual ~MemoryBudget();
    /**
     * @brief Returns the ceiling.
     * @return The ceiling in bytes, greater than 0.
     * @throws Nothing.
     */
    virtual int64_t ceilingBytes() const = 0;
    /**
     * @brief Returns the bytes currently reserved.
     * @return Reserved bytes, from 0 to ceilingBytes().
     * @throws InternalError When the budget's internal lock fails (a translated
     * std::system_error).
     */
    virtual int64_t reservedBytes() const = 0;
    /**
     * @brief Returns the bytes still available.
     * @return ceilingBytes() minus reservedBytes().
     * @throws InternalError When the budget's internal lock fails (a translated
     * std::system_error).
     */
    virtual int64_t availableBytes() const = 0;
    /**
     * @brief Reserves bytes if they fit, without waiting.
     * @param bytes The amount, at least 0. tryReserve(0) always succeeds.
     * @return True when reserved + bytes <= ceiling and the bytes were reserved.
     * @throws InvalidArgumentError When bytes is negative.
     * @throws InternalError When the budget's internal lock fails (a translated
     * std::system_error); nothing is reserved.
     */
    virtual bool tryReserve(int64_t bytes) = 0;
    /**
     * @brief Reserves bytes, blocking until they fit.
     *
     * The token is re-checked about every 10 ms while waiting.
     * @param bytes The amount, from 0 to ceilingBytes().
     * @param token Cancels the wait.
     * @throws InvalidArgumentError At once, when bytes is negative or exceeds
     * the ceiling, because such a request could never succeed.
     * @throws CancelledError When the token is cancelled while waiting; nothing
     * is reserved.
     * @throws InternalError When the budget's internal lock or condition
     * variable fails (a translated std::system_error); nothing is reserved.
     */
    virtual void reserveOrWait(int64_t bytes, const CancellationToken& token) = 0;
    /**
     * @brief Returns previously reserved bytes and wakes waiters.
     * @param bytes The amount, from 0 to reservedBytes().
     * @throws InvalidArgumentError When bytes is negative or exceeds the
     * reserved amount; nothing is released.
     * @throws InternalError When the budget's internal lock fails (a translated
     * std::system_error); nothing is released.
     */
    virtual void release(int64_t bytes) = 0;
};

/** @brief Shared ownership of a MemoryBudget. */
using MemoryBudgetPtr = std::shared_ptr<MemoryBudget>;

/**
 * @brief Creates a memory budget.
 * @param ceilingBytes The ceiling in bytes, greater than 0.
 * @return A new budget with nothing reserved.
 * @throws InvalidArgumentError When ceilingBytes <= 0.
 * @throws std::bad_alloc When memory is exhausted.
 * @par Thread-safety
 * ThreadSafe; the returned budget is ThreadSafe.
 * @par Requirements
 * UR-EXE-2
 */
SLIDEIO_PROCESSOR_DATA_EXPORTS MemoryBudgetPtr makeMemoryBudget(int64_t ceilingBytes);

} // namespace slideio::processor
