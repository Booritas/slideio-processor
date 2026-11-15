#include <slideio/processor/storage/raster_store.hpp>

#include "storage/detail/errors.hpp"
#include "storage/detail/event_log.hpp"
#include "storage/detail/raster_files.hpp"
#include "storage/detail/schema.hpp"
#include "storage/detail/sqlite.hpp"
#include "storage/detail/store_impl.hpp"

#include <slideio/processor/core/error.hpp>

#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>

namespace slideio::processor {

namespace {

// The TIFF SamplesPerPixel limit (ruling S48).
constexpr int32_t kMaxPlaneCount = 65535;

[[noreturn]] void throwUnknown(const char* operation, RasterId raster) {
    detail::throwInvalid(operation, "there is no raster " + std::to_string(raster.value()));
}

[[noreturn]] void throwNotImplemented(const char* operation) {
    const std::string message = std::string(operation) + ": not implemented";
    throw InternalError(message.c_str());
}

// Refuses a declaration declare() cannot store (ruling S48). The name's uniqueness is
// checked against the table, inside the transaction.
void validate(const char* operation, const RasterDeclaration& declaration) {
    const char* name = declaration.name();
    if (name == nullptr || *name == '\0') detail::throwInvalid(operation, "the raster name is null or empty");
    const std::string named = std::string(" of raster '") + name + "'";
    const int32_t type = static_cast<int32_t>(declaration.planeType());
    if (type < 0 || type >= static_cast<int32_t>(DataType::Unknown)) {
        detail::throwInvalid(operation, "the plane type " + std::to_string(type) + named + " is not a valid DataType");
    }
    const int32_t planes = declaration.planeCount();
    if (planes < 1 || planes > kMaxPlaneCount) {
        detail::throwInvalid(operation, "the plane count " + std::to_string(planes) + named + " is outside 1.." +
                                            std::to_string(kMaxPlaneCount));
    }
    const int32_t compression = static_cast<int32_t>(declaration.compressionValue());
    if (compression < 0 || compression > static_cast<int32_t>(RasterCompression::Deflate)) {
        detail::throwInvalid(operation, "the compression " + std::to_string(compression) + named +
                                            " is not a RasterCompression");
    }
}

} // namespace

class RasterRegistry::Impl {
public:
    // A raster row. Rows are never removed, and id, name, type and plane count never
    // change; the population columns do, and are refreshed with the revision.
    struct Row {
        RasterId id;
        std::string name;
        DataType type = DataType::Unknown;
        int32_t planeCount = 0;
        StageId writer;
        int32_t level = 0;
        std::string fileName;
        int64_t bytes = 0;
        bool populated = false;  // the row's flag; isPopulated() also needs the file
    };

    explicit Impl(detail::StoreImpl& s)
        : store(s), directoryPath(detail::rasterDirectory(s.path())), directory(directoryPath.u8string()) {}

    // Reloads the cache when meta.rasters_revision differs from the revision it was
    // loaded at: updates the population columns of known rows and appends new ones. The
    // revision is read first, so a write committed between the two reads can only make
    // the cache newer than its revision, and the next call reloads again. Caller holds
    // `mutex`.
    void refresh(const char* operation) {
        store.withReadConnection(operation, [&](sqlite3* db) {
            const int64_t stored = detail::StoreImpl::metaInt64(db, detail::kMetaRastersRevision).value_or(0);
            if (revision && *revision == stored) return;
            detail::Statement select(db,
                                     "SELECT id, name, data_type, plane_count, writer_stage_id, level, file_name, "
                                     "bytes, populated FROM rasters ORDER BY id");
            while (select.step()) {
                const int32_t id = static_cast<int32_t>(select.columnInt64(0));
                auto known = byId.find(id);
                if (known == byId.end()) {
                    Row row;
                    row.id = RasterId(id);
                    row.name = select.columnText(1);
                    row.type = static_cast<DataType>(select.columnInt64(2));
                    row.planeCount = static_cast<int32_t>(select.columnInt64(3));
                    rows.push_back(std::move(row));  // deque: earlier rows (and their names) never move
                    Row& added = rows.back();
                    known = byId.emplace(id, &added).first;
                    byName[added.name] = &added;
                }
                Row& row = *known->second;
                row.writer = StageId(static_cast<int32_t>(select.columnInt64(4)));  // NULL reads 0: invalid
                row.level = static_cast<int32_t>(select.columnInt64(5));
                row.fileName = select.columnIsNull(6) ? std::string() : select.columnText(6);
                row.bytes = select.columnInt64(7);
                row.populated = select.columnInt64(8) != 0;
            }
            revision = stored;
        });
    }

    // The row's flag AND its file's presence (ruling S48): a store copied without its
    // -rasters directory reads every raster as unpopulated.
    bool isPopulated(const Row& row) const noexcept {
        if (!row.populated || row.fileName.empty()) return false;
        try {
            std::error_code ec;
            return std::filesystem::exists(directoryPath / std::filesystem::u8path(row.fileName), ec);
        } catch (...) {
            return false;
        }
    }

    // Locks, refreshes, then hands the row to `read`; throws InvalidArgumentError for an
    // unknown raster.
    template <class Read>
    auto withRow(const char* operation, RasterId raster, Read&& read) {
        std::lock_guard<std::mutex> guard(mutex);
        refresh(operation);
        const auto found = byId.find(raster.value());
        if (found == byId.end()) throwUnknown(operation, raster);
        return read(*found->second);
    }

    // As withRow, for the members that describe the published file.
    template <class Read>
    auto withPopulatedRow(const char* operation, RasterId raster, Read&& read) {
        return withRow(operation, raster, [&](const Row& row) {
            if (!isPopulated(row)) detail::throwInvalid(operation, "raster '" + row.name + "' is not populated");
            return read(row);
        });
    }

    // Locks, refreshes, then hands the row to `read`, or returns `fallback` for an
    // unknown raster, a closed store or a SQLite failure: the noexcept queries.
    template <class T, class Read>
    T readOr(const char* operation, RasterId raster, T fallback, Read&& read) noexcept {
        try {
            std::lock_guard<std::mutex> guard(mutex);
            refresh(operation);
            const auto found = byId.find(raster.value());
            return found == byId.end() ? fallback : read(*found->second);
        } catch (...) {
            return fallback;
        }
    }

    int64_t bytesOf(const Row& row) const noexcept { return isPopulated(row) ? row.bytes : 0; }

    detail::StoreImpl& store;
    const std::filesystem::path directoryPath;
    const std::string directory;  // directoryPath as UTF-8, for directory()
    std::mutex mutex;
    std::optional<int64_t> revision;  // meta.rasters_revision the cache was loaded at
    std::deque<Row> rows;
    std::map<int32_t, Row*> byId;
    std::map<std::string, Row*> byName;
};

RasterRegistry::RasterRegistry(detail::StoreImpl& store) : impl_(std::make_unique<Impl>(store)) {}

RasterRegistry::~RasterRegistry() = default;

RasterId RasterRegistry::declare(const RasterDeclaration& declaration) {
    constexpr const char* kOperation = "RasterRegistry::declare";
    const RasterId id = impl_->store.withWriteTransaction(kOperation, [&](sqlite3* db) {
        validate(kOperation, declaration);
        const std::string name = declaration.name();
        {
            detail::Statement existing(db, "SELECT 1 FROM rasters WHERE name = ?");
            existing.bindText(1, name);
            if (existing.step()) detail::throwInvalid(kOperation, "a raster named '" + name + "' already exists");
        }
        detail::Statement insert(
            db, "INSERT INTO rasters(name, data_type, plane_count, compression, description) VALUES (?, ?, ?, ?, ?)");
        insert.bindText(1, name);
        insert.bindInt64(2, static_cast<int64_t>(declaration.planeType()));
        insert.bindInt64(3, declaration.planeCount());
        insert.bindInt64(4, static_cast<int64_t>(declaration.compressionValue()));
        insert.bindText(5, declaration.descriptionText());
        insert.run();
        const RasterId raster(static_cast<int32_t>(sqlite3_last_insert_rowid(db)));
        detail::bumpRastersRevision(db);
        detail::EventRow event;
        event.kind = EventKind::RasterDeclared;
        event.rasterId = raster.value();
        detail::appendEvent(db, event);
        return raster;
    });
    impl_->store.notifyCommitted();
    return id;
}

bool RasterRegistry::find(const char* name, RasterId& out) const noexcept {
    if (name == nullptr || *name == '\0') return false;
    try {
        std::lock_guard<std::mutex> guard(impl_->mutex);
        impl_->refresh("RasterRegistry::find");
        const auto found = impl_->byName.find(name);
        if (found == impl_->byName.end()) return false;
        out = found->second->id;
        return true;
    } catch (...) {
        return false;
    }
}

const char* RasterRegistry::name(RasterId raster) const {
    return impl_->withRow("RasterRegistry::name", raster, [](const Impl::Row& row) { return row.name.c_str(); });
}

DataType RasterRegistry::planeType(RasterId raster) const {
    return impl_->withRow("RasterRegistry::planeType", raster, [](const Impl::Row& row) { return row.type; });
}

int32_t RasterRegistry::planeCount(RasterId raster) const {
    return impl_->withRow("RasterRegistry::planeCount", raster, [](const Impl::Row& row) { return row.planeCount; });
}

bool RasterRegistry::exists(RasterId raster) const noexcept {
    return impl_->readOr("RasterRegistry::exists", raster, false, [](const Impl::Row&) { return true; });
}

bool RasterRegistry::isPopulated(RasterId raster) const noexcept {
    return impl_->readOr("RasterRegistry::isPopulated", raster, false,
                         [this](const Impl::Row& row) { return impl_->isPopulated(row); });
}

StageId RasterRegistry::writerStage(RasterId raster) const {
    return impl_->withPopulatedRow("RasterRegistry::writerStage", raster,
                                   [](const Impl::Row& row) { return row.writer; });
}

int32_t RasterRegistry::writtenAtLevel(RasterId raster) const {
    return impl_->withPopulatedRow("RasterRegistry::writtenAtLevel", raster,
                                   [](const Impl::Row& row) { return row.level; });
}

int64_t RasterRegistry::bytesOnDisk(RasterId raster) const {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    impl_->refresh("RasterRegistry::bytesOnDisk");
    const auto found = impl_->byId.find(raster.value());
    return found == impl_->byId.end() ? 0 : impl_->bytesOf(*found->second);
}

RasterWriterPtr RasterRegistry::writerForStage(StageId, RasterId) {
    throwNotImplemented("RasterRegistry::writerForStage");
}

TileRasterSourcePtr RasterRegistry::sourceFor(RasterId) const { throwNotImplemented("RasterRegistry::sourceFor"); }

void RasterRegistry::discardRasterOfStage(StageId) { throwNotImplemented("RasterRegistry::discardRasterOfStage"); }

int32_t RasterRegistry::count() const noexcept {
    try {
        std::lock_guard<std::mutex> guard(impl_->mutex);
        impl_->refresh("RasterRegistry::count");
        return static_cast<int32_t>(impl_->rows.size());
    } catch (...) {
        return 0;
    }
}

RasterId RasterRegistry::at(int32_t index) const {
    constexpr const char* kOperation = "RasterRegistry::at";
    std::lock_guard<std::mutex> guard(impl_->mutex);
    impl_->refresh(kOperation);
    if (index < 0 || static_cast<size_t>(index) >= impl_->rows.size()) {
        detail::throwInvalid(kOperation, "index " + std::to_string(index) + " is out of range");
    }
    return impl_->rows[static_cast<size_t>(index)].id;
}

int64_t RasterRegistry::totalBytesOnDisk() const {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    impl_->refresh("RasterRegistry::totalBytesOnDisk");
    int64_t total = 0;
    for (const Impl::Row& row : impl_->rows) total += impl_->bytesOf(row);
    return total;
}

const char* RasterRegistry::directory() const noexcept { return impl_->directory.c_str(); }

int64_t RasterRegistry::removeUnreferencedFiles() { throwNotImplemented("RasterRegistry::removeUnreferencedFiles"); }

} // namespace slideio::processor
