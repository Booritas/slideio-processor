// Requirements: UR-BLD-5
#pragma once

// RAII wrappers over the SQLite C API. Every SQLite failure is reported through
// throwSqliteError, which throws IoError naming the store path with sqlite3_errmsg as
// the cause (C-6).

#include <sqlite3.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace slideio::processor::detail {

// Throws IoError "<operation>: SQLite failed on '<path>'" with sqlite3_errmsg(db) as the
// cause. `storePath` null takes the path from the connection; a null `db` gives the
// cause for `code`.
[[noreturn]] void throwSqliteError(sqlite3* db, int code, const char* operation, const char* storePath = nullptr);

// One owned sqlite3 connection, closed (sqlite3_close_v2) on destruction. Move-only.
class Connection {
public:
    Connection() noexcept = default;
    Connection(Connection&& other) noexcept;
    Connection& operator=(Connection&& other) noexcept;
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    ~Connection();

    // Opens `path` (UTF-8) with sqlite3_open_v2 `flags` and sets busy_timeout to 5000 ms.
    // Throws IoError naming the path.
    static Connection open(const std::string& path, int flags);

    sqlite3* get() const noexcept { return db_; }
    explicit operator bool() const noexcept { return db_ != nullptr; }
    // Closes the connection; a no-op when it is already closed.
    void close() noexcept;

private:
    explicit Connection(sqlite3* db) noexcept : db_(db) {}
    sqlite3* db_ = nullptr;
};

// Runs one or more statements with sqlite3_exec. Throws IoError.
void exec(sqlite3* db, const char* sql);

// One prepared statement, finalised on destruction. Move-only. Indices are 1-based for
// bind and 0-based for column, as in SQLite.
class Statement {
public:
    // Prepares `sql` on `db`. Throws IoError.
    Statement(sqlite3* db, const char* sql);
    Statement(Statement&& other) noexcept;
    Statement& operator=(Statement&&) = delete;
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    ~Statement();

    void bindInt64(int index, int64_t value);
    void bindDouble(int index, double value);
    void bindText(int index, const std::string& value);  // copied by SQLite
    void bindBlob(int index, const void* data, size_t size);  // copied by SQLite
    void bindNull(int index);

    // Steps once: true when a row is available, false when the statement is done.
    // Throws IoError.
    bool step();
    // Steps a statement that returns no row. Throws IoError.
    void run();
    // Resets the statement and clears its bindings, ready to bind again.
    void reset() noexcept;

    bool columnIsNull(int column) const noexcept;
    int64_t columnInt64(int column) const noexcept;
    double columnDouble(int column) const noexcept;
    std::string columnText(int column) const;
    // The blob's bytes, valid until the next step, reset or destruction; null when empty.
    const uint8_t* columnBlob(int column) const noexcept;
    size_t columnBytes(int column) const noexcept;

    sqlite3_stmt* get() const noexcept { return stmt_; }

private:
    void check(int code, const char* operation) const;
    sqlite3* db_;
    sqlite3_stmt* stmt_ = nullptr;
};

} // namespace slideio::processor::detail
