#include "storage/detail/measurement_names.hpp"

#include <slideio/processor/core/error.hpp>

#include <string>

namespace slideio::processor::detail {

MeasurementNameInterner::MeasurementNameInterner(sqlite3* writeDb, const char* operation)
    : operation_(operation),
      insert_(writeDb, "INSERT OR IGNORE INTO measurement_names(name) VALUES (?)"),
      select_(writeDb, "SELECT id FROM measurement_names WHERE name = ?") {}

int64_t MeasurementNameInterner::idOf(const std::string& name) {
    const auto known = ids_.find(name);
    if (known != ids_.end()) return known->second;
    insert_.bindText(1, name);
    insert_.run();
    insert_.reset();
    select_.bindText(1, name);
    if (!select_.step()) throw InternalError((std::string(operation_) + ": an interned measurement name vanished").c_str());
    const int64_t id = select_.columnInt64(0);
    select_.reset();
    return ids_.emplace(name, id).first->second;
}

} // namespace slideio::processor::detail
