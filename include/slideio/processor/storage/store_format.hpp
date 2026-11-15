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
 *
 * Version 1 is frozen. Two checked-in fixtures under `tests/storage/data/` hold it:
 * `store-v1.schema.sql`, the schema snapshot a fresh store must match, and
 * `store-v1.golden.store` with its `store-v1.golden.store-rasters/` directory, a
 * store written by version 1 that must still open and read back. Any schema change,
 * additive ones included, needs version 2 and a migration (SDS 03 §3.12). The v1 fixtures
 * are never regenerated: a version 2 gets `store-v2.*` fixtures of its own and keeps the v1
 * golden store as the input its migration is tested on. A SQLite upgrade may change the
 * three rtree shadow-table `sql` lines of the schema snapshot; regenerate the snapshot then,
 * deliberately, and review the diff.
 * @return The format version, currently 1.
 * @par Thread-safety
 * ThreadSafe.
 * @par Requirements
 * UR-RES-12 (format versioning)
 */
SLIDEIO_PROCESSOR_DATA_EXPORTS int32_t storeFormatVersion() noexcept;

}  // namespace slideio::processor
