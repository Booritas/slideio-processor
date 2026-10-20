#pragma once

/**
 * @file store_format.hpp
 * @brief storeFormatVersion: the store file format this library reads and writes.
 */

#include <slideio/processor/core/export.hpp>

#include <cstdint>

namespace slideio::processor {

/**
 * @brief The store file format version this library reads and writes.
 *
 * Store::open refuses a file whose recorded version differs.
 * @return The format version, currently 1.
 * @par Thread-safety
 * ThreadSafe.
 * @par Requirements
 * UR-RES-12 (format versioning)
 */
SLIDEIO_PROCESSOR_DATA_EXPORTS int32_t storeFormatVersion() noexcept;

}  // namespace slideio::processor
