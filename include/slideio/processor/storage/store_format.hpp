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
 * Version 2 is current: a new store is written as version 2, and Store::open accepts
 * versions 1 and 2 and refuses any other. Version 2 adds `stages.stale_since_ms` (SDS 03
 * §3.12). A version 1 store is migrated to version 2 in place by its first writable open,
 * in one durable transaction; a library that reads only version 1 then refuses it. A
 * read-only open leaves a version 1 store unchanged.
 *
 * Versions 1 and 2 are frozen. Checked-in fixtures under `tests/storage/data/` hold them.
 * `store-v2.schema.sql` is the schema snapshot a fresh store, and a migrated version 1
 * store, must match; `store-v2.golden.store` with its `store-v2.golden.store-rasters/`
 * directory is a store written by version 2 that must still open and read back.
 * `store-v1.schema.sql` is the schema of `store-v1.golden.store` (with its `-rasters`
 * directory), a store written by version 1 that must still open read-only, unchanged, and
 * is the input the migration is tested on. The v1 fixtures are never regenerated: this
 * library no longer writes version 1. The v2 fixtures are written once by their generator
 * and never regenerated, except the snapshot's rtree shadow-table lines after a SQLite
 * upgrade; any schema change, additive ones included, needs version 3 and a migration. A
 * SQLite upgrade may change the three rtree shadow-table `sql` lines of the v2 snapshot;
 * regenerate that snapshot then, deliberately, and review the diff.
 * @return The format version, currently 2.
 * @par Thread-safety
 * ThreadSafe.
 * @par Requirements
 * UR-RES-12 (format versioning)
 */
SLIDEIO_PROCESSOR_DATA_EXPORTS int32_t storeFormatVersion() noexcept;

}  // namespace slideio::processor
