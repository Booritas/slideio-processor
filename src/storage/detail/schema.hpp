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

// The pixel layer, created with the store (ruling S5); declared layers number from 2.
inline constexpr int32_t kPixelLayerId = 1;
inline constexpr const char* kPixelLayerName = "pixel";

// Workspace-local labels number from here upward; ids 1..65535 belong to the standard
// vocabulary (ruling S5).
inline constexpr int32_t kFirstLocalLabelId = 65536;

// Creates every plan-A table and index, the pixel layer (id 1) and the initial meta
// rows. Runs inside the caller's write transaction on `db`. `provenanceJson` is the
// initial meta.provenance, `storeUuid` the initial meta.store_uuid. Throws IoError.
void createSchema(sqlite3* db, const char* provenanceJson, const char* storeUuid);

} // namespace slideio::processor::detail
