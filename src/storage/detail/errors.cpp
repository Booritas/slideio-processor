#include "storage/detail/errors.hpp"

#include <slideio/processor/core/error.hpp>

namespace slideio::processor::detail {

void throwInvalid(const char* operation, const std::string& what) {
    const std::string message = std::string(operation) + ": " + what;
    throw InvalidArgumentError(message.c_str());
}

bool stageExists(sqlite3* db, StageId stage) {
    Statement select(db, "SELECT 1 FROM stages WHERE id = ?");
    select.bindInt64(1, stage.value());
    return select.step();
}

void throwUnknownStage(const char* operation, StageId stage) {
    throwInvalid(operation, "there is no stage " + std::to_string(stage.value()));
}

} // namespace slideio::processor::detail
