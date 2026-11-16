#pragma once

// Error helpers shared by the storage translation units. Every message keeps the form
// "<operation>: <what>", which the tests match on.

#include "storage/detail/sqlite.hpp"

#include <slideio/processor/core/ids.hpp>

#include <string>

namespace slideio::processor::detail {

// Throws InvalidArgumentError "<operation>: <what>".
[[noreturn]] void throwInvalid(const char* operation, const std::string& what);

// Returns whether the stages table holds `stage`.
bool stageExists(sqlite3* db, StageId stage);

// Throws InvalidArgumentError "<operation>: there is no stage <id>".
[[noreturn]] void throwUnknownStage(const char* operation, StageId stage);

} // namespace slideio::processor::detail
