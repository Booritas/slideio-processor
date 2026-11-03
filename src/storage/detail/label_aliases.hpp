// Requirements: UR-RES-18
#pragma once

// The QuPath alias table (ruling S7) and the ASCII case fold lookup uses.

#include <string>

namespace slideio::processor::detail {

// Returns `name` with ASCII letters lower-cased; every other byte is kept, so UTF-8 is
// untouched. A null name folds to the empty string.
std::string foldAscii(const char* name);

// Maps a folded QuPath spelling to the canonical standard name, or returns nullptr when
// `folded` is not an alias. Keys are matched exactly: the `*` in "ignore*" is a literal
// character, not a wildcard.
const char* canonicalForAlias(const std::string& folded) noexcept;

} // namespace slideio::processor::detail
