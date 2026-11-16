#pragma once

// Registration of a label a GeoJSON import brings in under ImportPolicy::Permissive
// (ruling S26). Defined in vocabulary_registry.cpp, beside addLocal, which it mirrors.

#include <slideio/processor/core/ids.hpp>

namespace slideio::processor::detail {

class StoreImpl;

// Returns the label VocabularyRegistry::find resolves `name` to (case folded, aliases
// included), setting `added` to false; otherwise inserts `name` as given with origin
// ImportRegistered and appends a LabelAdded event in one transaction, setting `added` to
// true. Throws InvalidArgumentError for a null or empty name; IoError when the store is
// closed or read-only, or on a SQLite failure; std::bad_alloc.
LabelId registerImportedLabel(StoreImpl& store, const char* name, bool& added);

} // namespace slideio::processor::detail
