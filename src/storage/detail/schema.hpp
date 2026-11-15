// Requirements: UR-RES-12
#pragma once

// The v1 schema (SDS 03 §3.12) and the meta keys.

#include <sqlite3.h>

#include <cstdint>

namespace slideio::processor::detail {

// meta keys.
inline constexpr const char* kMetaFormatVersion = "format_version";
inline constexpr const char* kMetaVocabularyVersion = "vocabulary_version";
inline constexpr const char* kMetaProvenance = "provenance";
inline constexpr const char* kMetaImportPolicy = "import_policy";
inline constexpr const char* kMetaJournalTruncatedBelow = "journal_truncated_below";
inline constexpr const char* kMetaStoreUuid = "store_uuid";
// Bumped by every change to the layers table, in its transaction, so a LayerRegistry
// cache sees declarations and drops alike (ruling S28).
inline constexpr const char* kMetaLayersRevision = "layers_revision";
// Bumped by every change to the rasters table, in its transaction, so a RasterRegistry
// cache sees declarations, publications and discards alike; a raster's row changes
// without the row count changing.
inline constexpr const char* kMetaRastersRevision = "rasters_revision";

// The pixel layer, created with the store (ruling S5); declared layers number from 2.
inline constexpr int32_t kPixelLayerId = 1;
inline constexpr const char* kPixelLayerName = "pixel";

// Workspace-local labels number from here upward; ids 1..65535 belong to the standard
// vocabulary (ruling S5).
inline constexpr int32_t kFirstLocalLabelId = 65536;

// Creates every table and index, the pixel layer (id 1) and the initial meta
// rows. Runs inside the caller's write transaction on `db`. `provenanceJson` is the
// initial meta.provenance, `storeUuid` the initial meta.store_uuid. Throws IoError.
void createSchema(sqlite3* db, const char* provenanceJson, const char* storeUuid);

// Adds one to meta.layers_revision. Runs inside the caller's write transaction, which
// has just changed the layers table. Throws IoError.
void bumpLayersRevision(sqlite3* db);

// Adds one to meta.rasters_revision. Runs inside the caller's write transaction, which
// has just changed the rasters table. Throws IoError.
void bumpRastersRevision(sqlite3* db);

} // namespace slideio::processor::detail
