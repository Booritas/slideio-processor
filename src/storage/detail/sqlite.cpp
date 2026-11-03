#include "storage/detail/sqlite.hpp"

#include <slideio/processor/core/error.hpp>

#include <utility>

namespace slideio::processor::detail {

void throwSqliteError(sqlite3* db, int code, const char* operation, const char* storePath) {
    const char* path = storePath;
    if (path == nullptr && db != nullptr) path = sqlite3_db_filename(db, "main");
    if (path == nullptr) path = "";
    const std::string message = std::string(operation != nullptr ? operation : "SQLite") +
                                ": SQLite failed on '" + path + "'";
    ErrorContext context;
    context.cause = db != nullptr ? sqlite3_errmsg(db) : sqlite3_errstr(code);
    throw IoError(message.c_str(), context);
}

Connection::Connection(Connection&& other) noexcept : db_(std::exchange(other.db_, nullptr)) {}

Connection& Connection::operator=(Connection&& other) noexcept {
    if (this != &other) {
        close();
        db_ = std::exchange(other.db_, nullptr);
    }
    return *this;
}

Connection::~Connection() { close(); }

Connection Connection::open(const std::string& path, int flags) {
    sqlite3* db = nullptr;
    const int rc = sqlite3_open_v2(path.c_str(), &db, flags, nullptr);
    Connection connection(db);  // owns db even when the open failed
    if (rc != SQLITE_OK) throwSqliteError(db, rc, "Store::open", path.c_str());
    const int timeout = sqlite3_busy_timeout(db, 5000);
    if (timeout != SQLITE_OK) throwSqliteError(db, timeout, "Store::open", path.c_str());
    return connection;
}

void Connection::close() noexcept {
    if (db_ != nullptr) {
        sqlite3_close_v2(db_);
        db_ = nullptr;
    }
}

void exec(sqlite3* db, const char* sql) {
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) throwSqliteError(db, rc, "SQLite exec");
}

Statement::Statement(sqlite3* db, const char* sql) : db_(db) {
    check(sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr), "SQLite prepare");
}

Statement::Statement(Statement&& other) noexcept
    : db_(other.db_), stmt_(std::exchange(other.stmt_, nullptr)) {}

Statement::~Statement() { sqlite3_finalize(stmt_); }

void Statement::check(int code, const char* operation) const {
    if (code != SQLITE_OK) throwSqliteError(db_, code, operation);
}

void Statement::bindInt64(int index, int64_t value) {
    check(sqlite3_bind_int64(stmt_, index, value), "SQLite bind");
}

void Statement::bindDouble(int index, double value) {
    check(sqlite3_bind_double(stmt_, index, value), "SQLite bind");
}

void Statement::bindText(int index, const std::string& value) {
    check(sqlite3_bind_text64(stmt_, index, value.data(), value.size(), SQLITE_TRANSIENT, SQLITE_UTF8),
          "SQLite bind");
}

void Statement::bindBlob(int index, const void* data, size_t size) {
    // A zero-length blob is stored as an empty blob, not as NULL.
    static const uint8_t empty = 0;
    check(sqlite3_bind_blob64(stmt_, index, size != 0 ? data : &empty, size, SQLITE_TRANSIENT),
          "SQLite bind");
}

void Statement::bindNull(int index) { check(sqlite3_bind_null(stmt_, index), "SQLite bind"); }

bool Statement::step() {
    const int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    throwSqliteError(db_, rc, "SQLite step");
}

void Statement::run() {
    while (step()) {
    }
}

void Statement::reset() noexcept {
    sqlite3_reset(stmt_);
    sqlite3_clear_bindings(stmt_);
}

bool Statement::columnIsNull(int column) const noexcept {
    return sqlite3_column_type(stmt_, column) == SQLITE_NULL;
}

int64_t Statement::columnInt64(int column) const noexcept { return sqlite3_column_int64(stmt_, column); }

double Statement::columnDouble(int column) const noexcept { return sqlite3_column_double(stmt_, column); }

std::string Statement::columnText(int column) const {
    const unsigned char* text = sqlite3_column_text(stmt_, column);
    const int bytes = sqlite3_column_bytes(stmt_, column);
    return text != nullptr ? std::string(reinterpret_cast<const char*>(text), static_cast<size_t>(bytes))
                           : std::string();
}

const uint8_t* Statement::columnBlob(int column) const noexcept {
    return static_cast<const uint8_t*>(sqlite3_column_blob(stmt_, column));
}

size_t Statement::columnBytes(int column) const noexcept {
    return static_cast<size_t>(sqlite3_column_bytes(stmt_, column));
}

} // namespace slideio::processor::detail
