// Requirements: UR-RES-12
#pragma once

// The v1 schema (SDS 03 §3.12) and the meta keys.

#include <sqlite3.h>

namespace slideio::processor::detail {

// meta keys.
inline constexpr const char* kMetaFormatVersion = "format_version";
inline constexpr const char* kMetaVocabularyVersion = "vocabulary_version";
inline constexpr const char* kMetaProvenance = "provenance";
inline constexpr const char* kMetaImportPolicy = "import_policy";
inline constexpr const char* kMetaJournalTruncatedBelow = "journal_truncated_below";
inline constexpr const char* kMetaStoreUuid = "store_uuid";

// Creates every plan-A table and index, the pixel layer (id 1) and the initial meta
// rows. Runs inside the caller's write transaction on `db`. `provenanceJson` is the
// initial meta.provenance, `storeUuid` the initial meta.store_uuid. Throws IoError.
void createSchema(sqlite3* db, const char* provenanceJson, const char* storeUuid);

} // namespace slideio::processor::detail
