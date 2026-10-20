#include "storage/detail/store_impl.hpp"

#include "storage/detail/schema.hpp"

#include <slideio/processor/storage/store_format.hpp>

#include <rapidjson/document.h>

#include <cstdio>
#include <filesystem>
#include <random>
#include <system_error>

namespace slideio::processor::detail {

// Idle read connections, opened on demand and kept for reuse until the store closes.
class ConnectionPool {
public:
    explicit ConnectionPool(std::string path) : path_(std::move(path)) {}

    Connection acquire(const char* operation) {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (closed_) throwClosed(operation, path_);
            if (!idle_.empty()) {
                Connection c = std::move(idle_.back());
                idle_.pop_back();
                return c;
            }
        }
        return Connection::open(path_, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX);
    }

    // Keeps the connection for reuse, or closes it once the pool has closed.
    void release(Connection&& connection) noexcept {
        Connection c = std::move(connection);
        std::lock_guard<std::mutex> guard(mutex_);
        if (closed_) return;
        try {
            idle_.push_back(std::move(c));
        } catch (...) {
            // Out of memory: c still owns the connection and closes it.
        }
    }

    void close() noexcept {
        std::vector<Connection> idle;
        std::lock_guard<std::mutex> guard(mutex_);
        closed_ = true;
        idle.swap(idle_);
    }

    [[noreturn]] static void throwClosed(const char* operation, const std::string& path) {
        const std::string message = std::string(operation) + ": store is closed ('" + path + "')";
        throw IoError(message.c_str());
    }

private:
    std::string path_;
    std::mutex mutex_;
    std::vector<Connection> idle_;
    bool closed_ = false;
};

ReadLease::ReadLease(std::shared_ptr<ConnectionPool> pool, Connection connection) noexcept
    : pool_(std::move(pool)), connection_(std::move(connection)) {}

ReadLease::~ReadLease() {
    if (pool_ && connection_) pool_->release(std::move(connection_));
}

namespace {

std::string newStoreUuid() {
    std::random_device device;
    std::string uuid;
    for (int i = 0; i < 4; ++i) {
        char word[9];
        std::snprintf(word, sizeof(word), "%08x", static_cast<unsigned>(device()));
        uuid += word;
    }
    return uuid;
}

[[noreturn]] void throwIo(const std::string& message, const char* cause = nullptr) {
    ErrorContext context;
    context.cause = cause;
    throw IoError(message.c_str(), context);
}

void removeStoreFiles(const std::string& path) noexcept {
    std::error_code ec;
    const std::filesystem::path base = std::filesystem::u8path(path);
    std::filesystem::remove(base, ec);
    for (const char* suffix : {"-wal", "-shm", "-journal"}) {
        std::filesystem::path side = base;
        side += suffix;
        std::filesystem::remove(side, ec);
    }
}

} // namespace

std::string provenanceToJson(const Provenance& provenance) {
    const int32_t length = provenance.toJson(nullptr, 0);
    std::vector<char> buffer(static_cast<size_t>(length) + 1);
    provenance.toJson(buffer.data(), length + 1);
    return std::string(buffer.data(), static_cast<size_t>(length));
}

StoreImpl::StoreImpl(const char* path, OpenMode mode, const Configuration& configuration)
    : mode_(mode), configuration_(configuration) {
    if (path == nullptr || *path == '\0') {
        throw InvalidArgumentError("Store::open: the path must not be null or empty");
    }
    path_ = path;
    if (mode != OpenMode::ReadOnly && mode != OpenMode::ReadWrite && mode != OpenMode::CreateNew) {
        const std::string message = "Store::open: unknown open mode " +
                                    std::to_string(static_cast<int32_t>(mode)) + " for '" + path_ + "'";
        throw InvalidArgumentError(message.c_str());
    }
    pool_ = std::make_shared<ConnectionPool>(path_);

    std::error_code ec;
    const bool exists = std::filesystem::exists(std::filesystem::u8path(path_), ec);
    if (ec) throwIo("Store::open: cannot inspect '" + path_ + "'", ec.message().c_str());
    if (mode == OpenMode::CreateNew) {
        if (exists) throwIo("Store::open: '" + path_ + "' already exists; CreateNew needs a new path");
        createNew();
    } else {
        if (!exists) throwIo("Store::open: no store at '" + path_ + "'");
        openExisting();
    }
    open_.store(true, std::memory_order_release);
}

StoreImpl::~StoreImpl() {
    try {
        close();
    } catch (...) {
        // The owner reports close() failures; destruction never throws.
    }
}

void StoreImpl::openWriteConnection(int flags) {
    writeConnection_ = Connection::open(path_, flags | SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX);
}

void StoreImpl::configureWriteConnection() {
    sqlite3* db = writeConnection_.get();
    Statement journal(db, "PRAGMA journal_mode=WAL");
    if (!journal.step() || journal.columnText(0) != "wal") {
        throwIo("Store::open: cannot switch '" + path_ + "' to WAL mode", sqlite3_errmsg(db));
    }
    exec(db, "PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON");
}

void StoreImpl::openExisting() {
    if (mode_ == OpenMode::ReadOnly) {
        ReadLease probe(pool_, pool_->acquire("Store::open"));
        formatVersion_ = readFormatVersion(probe.get());
        return;
    }
    // Read the version before anything touches the file, then take the lock.
    openWriteConnection(0);
    formatVersion_ = readFormatVersion(writeConnection_.get());
    writerLock_ = WriterLock::acquire(path_);
    configureWriteConnection();
}

void StoreImpl::createNew() {
    writerLock_ = WriterLock::acquire(path_);
    std::error_code ec;
    if (std::filesystem::exists(std::filesystem::u8path(path_), ec)) {
        throwIo("Store::open: '" + path_ + "' already exists; CreateNew needs a new path");
    }
    try {
        openWriteConnection(SQLITE_OPEN_CREATE);
        configureWriteConnection();
        const std::string provenance = provenanceToJson(Provenance());
        const std::string uuid = newStoreUuid();
        beginWrite("Store::open");
        try {
            createSchema(writeConnection_.get(), provenance.c_str(), uuid.c_str());
            commitWrite("Store::open");
        } catch (...) {
            rollbackWrite();
            throw;
        }
        formatVersion_ = storeFormatVersion();
    } catch (...) {
        // Leave no half-made store behind: the file is the one created above.
        writeConnection_.close();
        removeStoreFiles(path_);
        writerLock_.release();
        throw;
    }
}

int32_t StoreImpl::readFormatVersion(sqlite3* db) const {
    const int32_t library = storeFormatVersion();
    sqlite3_stmt* raw = nullptr;
    const int rc = sqlite3_prepare_v2(db, "SELECT value FROM meta WHERE key = 'format_version'", -1, &raw, nullptr);
    sqlite3_finalize(raw);
    if (rc == SQLITE_NOTADB || rc == SQLITE_ERROR) {
        // Not a SQLite database, or one without a meta table.
        const std::string message = "Store::open: '" + path_ +
                                    "' is not a slideio-processor store (store format version 0; this library "
                                    "reads version " + std::to_string(library) + ")";
        ErrorContext context;
        context.cause = sqlite3_errmsg(db);
        throw StoreFormatError(message.c_str(), 0, library, context);
    }
    if (rc != SQLITE_OK) throwSqliteError(db, rc, "Store::open", path_.c_str());

    const std::optional<int64_t> stored = metaInt64(db, kMetaFormatVersion);
    const int32_t version =
        stored && *stored > 0 && *stored <= INT32_MAX ? static_cast<int32_t>(*stored) : 0;
    if (version != library) {
        const std::string message = "Store::open: '" + path_ + "' has store format version " +
                                    std::to_string(version) + "; this library reads version " +
                                    std::to_string(library);
        throw StoreFormatError(message.c_str(), version, library);
    }
    return version;
}

void StoreImpl::checkOpen(const char* operation) const {
    if (!isOpen()) ConnectionPool::throwClosed(operation, path_);
}

void StoreImpl::checkWritable(const char* operation) const {
    checkOpen(operation);
    if (isReadOnly()) throwIo(std::string(operation) + ": the store '" + path_ + "' is read-only");
}

ReadLease StoreImpl::acquireRead(const char* operation) const {
    checkOpen(operation);
    return ReadLease(pool_, pool_->acquire(operation));
}

void StoreImpl::beginWrite(const char* operation) {
    sqlite3* db = writeConnection_.get();
    const int rc = sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) throwSqliteError(db, rc, operation, path_.c_str());
}

void StoreImpl::commitWrite(const char* operation) {
    sqlite3* db = writeConnection_.get();
    const int rc = sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) throwSqliteError(db, rc, operation, path_.c_str());
}

void StoreImpl::rollbackWrite() noexcept {
    sqlite3* db = writeConnection_.get();
    if (db != nullptr && sqlite3_get_autocommit(db) == 0) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
    }
}

void StoreImpl::throwTranslated(const char* operation, const std::exception& e) const {
    const std::string message = std::string(operation) + ": unexpected failure on '" + path_ + "'";
    ErrorContext context;
    context.cause = e.what();
    throw InternalError(message.c_str(), context);
}

std::optional<std::string> StoreImpl::metaText(sqlite3* db, const char* key) {
    Statement select(db, "SELECT value FROM meta WHERE key = ?");
    select.bindText(1, key);
    if (!select.step() || select.columnIsNull(0)) return std::nullopt;
    return select.columnText(0);
}

std::optional<int64_t> StoreImpl::metaInt64(sqlite3* db, const char* key) {
    Statement select(db, "SELECT value FROM meta WHERE key = ?");
    select.bindText(1, key);
    if (!select.step() || sqlite3_column_type(select.get(), 0) != SQLITE_INTEGER) return std::nullopt;
    return select.columnInt64(0);
}

void StoreImpl::setMetaText(sqlite3* db, const char* key, const std::string& value) {
    Statement upsert(db, "INSERT OR REPLACE INTO meta(key, value) VALUES (?, ?)");
    upsert.bindText(1, key);
    upsert.bindText(2, value);
    upsert.run();
}

void StoreImpl::setMetaInt64(sqlite3* db, const char* key, int64_t value) {
    Statement upsert(db, "INSERT OR REPLACE INTO meta(key, value) VALUES (?, ?)");
    upsert.bindText(1, key);
    upsert.bindInt64(2, value);
    upsert.run();
}

void StoreImpl::close() {
    if (!open_.exchange(false, std::memory_order_acq_rel)) return;
    pool_->close();
    {
        std::lock_guard<std::mutex> guard(writeMutex_);
        writeConnection_.close();
    }
    writerLock_.release();
}

Provenance parseProvenanceJson(const std::string& json, const std::string& storePath) {
    rapidjson::Document doc;
    doc.Parse(json.c_str(), json.size());
    const auto member = [&doc](const char* name) -> const rapidjson::Value* {
        const auto it = doc.FindMember(name);
        return it != doc.MemberEnd() ? &it->value : nullptr;
    };
    if (!doc.HasParseError() && doc.IsObject()) {
        const rapidjson::Value* slideId = member("slideId");
        const rapidjson::Value* sceneIndex = member("sceneIndex");
        const rapidjson::Value* libraryVersion = member("libraryVersion");
        const rapidjson::Value* createdAt = member("createdAtUnixMs");
        if (slideId != nullptr && slideId->IsString() && sceneIndex != nullptr && sceneIndex->IsInt() &&
            libraryVersion != nullptr && libraryVersion->IsString() && createdAt != nullptr &&
            createdAt->IsInt64()) {
            const std::string id(slideId->GetString(), slideId->GetStringLength());
            return Provenance::restored(id.c_str(), sceneIndex->GetInt(), libraryVersion->GetString(),
                                        createdAt->GetInt64());
        }
    }
    const std::string message =
        "Store::provenance: meta.provenance of '" + storePath + "' is not a provenance record";
    ErrorContext context;
    context.cause = json.c_str();
    throw InternalError(message.c_str(), context);
}

} // namespace slideio::processor::detail
