#pragma once

// Interning of measurement names into the measurement_names table, shared by the writers
// that store measurements.

#include "storage/detail/sqlite.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace slideio::processor::detail {

// Maps names to measurement_names ids inside one write transaction. The two statements are
// prepared once, and each name is looked up in the database once. Not thread-safe; it
// borrows `writeDb`, which must outlive it, and `operation`, a string literal naming the
// writer in its InternalError text.
class MeasurementNameInterner {
public:
    MeasurementNameInterner(sqlite3* writeDb, const char* operation);

    // Returns the id of `name`, inserting it when new. Throws InternalError
    // "<operation>: an interned measurement name vanished" when the row cannot be read
    // back, IoError on a SQLite failure.
    int64_t idOf(const std::string& name);

private:
    const char* operation_;
    Statement insert_;
    Statement select_;
    std::unordered_map<std::string, int64_t> ids_;
};

} // namespace slideio::processor::detail
