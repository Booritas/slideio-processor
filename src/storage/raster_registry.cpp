#include <slideio/processor/storage/raster_store.hpp>

#include "storage/detail/errors.hpp"
#include "storage/detail/event_log.hpp"
#include "storage/detail/file_io.hpp"
#include "storage/detail/raster_files.hpp"
#include "storage/detail/raster_writer_core.hpp"
#include "storage/detail/schema.hpp"
#include "storage/detail/sqlite.hpp"
#include "storage/detail/store_impl.hpp"
#include "storage/detail/tiff_raster_reader.hpp"

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/log.hpp>

#include <deque>
#include <exception>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace slideio::processor {

namespace {

// The TIFF SamplesPerPixel limit (ruling S48).
constexpr int32_t kMaxPlaneCount = 65535;

[[noreturn]] void throwUnknown(const char* operation, RasterId raster) {
    detail::throwInvalid(operation, "there is no raster " + std::to_string(raster.value()));
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
    // A raster row. Rows are never removed, and the declaration columns (id to
    // description) never change; the population columns do, and are refreshed with the
    // revision.
    struct Row {
        RasterId id;
        std::string name;
        DataType type = DataType::Unknown;
        int32_t planeCount = 0;
        RasterCompression compression = RasterCompression::Deflate;
        std::string description;
        StageId writer;
        int32_t level = 0;
        double scaleX = 0.0;  // level-0 pixels per page-0 pixel, fixed at flush
        double scaleY = 0.0;
        int64_t width = 0;  // page 0's size
        int64_t height = 0;
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
                                     "bytes, populated, compression, description, scale_x, scale_y, width, height "
                                     "FROM rasters ORDER BY id");
            while (select.step()) {
                const int32_t id = static_cast<int32_t>(select.columnInt64(0));
                auto known = byId.find(id);
                if (known == byId.end()) {
                    Row row;
                    row.id = RasterId(id);
                    row.name = select.columnText(1);
                    row.type = static_cast<DataType>(select.columnInt64(2));
                    row.planeCount = static_cast<int32_t>(select.columnInt64(3));
                    row.compression = static_cast<RasterCompression>(select.columnInt64(9));
                    row.description = select.columnText(10);
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
                row.scaleX = select.columnDouble(11);
                row.scaleY = select.columnDouble(12);
                row.width = select.columnInt64(13);
                row.height = select.columnInt64(14);
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

    // Deletes `fileName` from the directory, and returns whether this call removed it:
    // false when it was already gone (another deleter got there first). A failure (on
    // Windows, a file a source still holds open) is logged at Warning and returns false:
    // the file is left for removeUnreferencedFiles (ruling S51).
    bool deleteOrWarn(const char* operation, const std::string& fileName) noexcept {
        try {
            return detail::removeFile(directoryPath / std::filesystem::u8path(fileName));
        } catch (const std::exception& e) {
            logMessage(LogLevel::Warning, "%s: %s; it is left for removeUnreferencedFiles", operation, e.what());
        } catch (...) {
            logMessage(LogLevel::Warning, "%s: cannot delete '%s'; it is left for removeUnreferencedFiles", operation,
                       fileName.c_str());
        }
        return false;
    }

    detail::StoreImpl& store;
    const std::filesystem::path directoryPath;
    const std::string directory;  // directoryPath as UTF-8, for directory()
    std::mutex mutex;
    std::optional<int64_t> revision;  // meta.rasters_revision the cache was loaded at
    std::deque<Row> rows;
    std::map<int32_t, Row*> byId;
    std::map<std::string, Row*> byName;
    // The live writers (ruling S47), shared with them; locked after `mutex`, never before.
    const std::shared_ptr<detail::LiveRasterWriters> live = std::make_shared<detail::LiveRasterWriters>();
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

RasterWriterPtr RasterRegistry::writerForStage(StageId stage, RasterId raster) {
    constexpr const char* kOperation = "RasterRegistry::writerForStage";
    impl_->store.checkWritable(kOperation);
    detail::RasterWriterSetup setup;
    {
        // Both locks, so the ownership check and joining the live set are one step: a
        // writer of another stage either is still live here, or has committed before this
        // refresh. This is the early refusal; the publish transaction re-checks (S47).
        std::lock_guard<std::mutex> guard(impl_->mutex);
        std::lock_guard<std::mutex> liveGuard(impl_->live->mutex);
        impl_->refresh(kOperation);
        const auto found = impl_->byId.find(raster.value());
        if (found == impl_->byId.end()) throwUnknown(kOperation, raster);
        const Impl::Row& row = *found->second;
        // Another stage's raster stays its own until it is discarded (ruling S47). The row's
        // flag decides, as in the publish transaction, not isPopulated: a store copied
        // without its files is refused here rather than after a whole pyramid is built.
        const StageId writer = row.populated && row.writer != stage ? row.writer : StageId();
        std::string writerText;
        const bool known = impl_->store.withReadConnection(kOperation, [&](sqlite3* db) {
            if (!detail::stageExists(db, stage)) return false;
            if (writer.isValid()) {
                detail::Statement select(db, "SELECT name FROM stages WHERE id = ?");
                select.bindInt64(1, writer.value());
                const std::string id = std::to_string(writer.value());
                writerText = select.step() ? "'" + select.columnText(0) + "' (id " + id + ")" : id;
            }
            return true;
        });
        if (!known) detail::throwUnknownStage(kOperation, stage);
        if (writer.isValid()) {
            detail::throwInvalid(kOperation, "raster '" + row.name + "' was written by stage " + writerText +
                                                 "; only that stage may write it until it is discarded");
        }
        if (impl_->live->byRaster.count(raster.value()) != 0) {
            detail::throwInvalid(kOperation, "raster '" + row.name +
                                                 "' already has a live writer; a raster has one writer at a time");
        }

        std::error_code error;
        std::filesystem::create_directories(impl_->directoryPath, error);
        if (error) {
            detail::throwIo(std::string(kOperation) + ": cannot create the raster directory '" + impl_->directory +
                                "'",
                            error.message());
        }
        setup.gate = impl_->store.gate();
        setup.live = impl_->live;
        setup.stage = stage;
        setup.raster = raster;
        setup.name = row.name;
        setup.description = row.description;
        setup.format = detail::RasterFormat{row.type, row.planeCount, row.compression};
        setup.directory = impl_->directoryPath;
        const std::string stem = detail::generateRasterFileStem();
        setup.files = detail::LiveRasterFiles{stem + ".spool", stem + ".tif"};
        impl_->live->byRaster.emplace(raster.value(), setup.files);
    }
    // Leaves the live set again if it throws.
    return RasterWriter::Impl::open(std::move(setup));
}

TileRasterSourcePtr RasterRegistry::sourceFor(RasterId raster) const {
    constexpr const char* kOperation = "RasterRegistry::sourceFor";
    for (;;) {
        // The row as it stands now (ruling S50): the source is bound to its file, and a
        // later re-run does not move it.
        const Impl::Row row = impl_->withRow(kOperation, raster, [](const Impl::Row& found) { return found; });
        const detail::RasterFormat format{row.type, row.planeCount, row.compression};
        if (!impl_->isPopulated(row)) return detail::makeZeroRasterSource(format);
        try {
            auto reader = std::make_shared<detail::TiffRasterReader>(
                impl_->directoryPath / std::filesystem::u8path(row.fileName), format, row.scaleX, row.scaleY,
                row.width, row.height);
            return detail::makeFileRasterSource(std::move(reader), format);
        } catch (const IoError&) {
            // A re-run or discard may have committed and deleted the file since the row
            // was read: try again with the row as it is now, unless it still names this file.
            const std::string now =
                impl_->withRow(kOperation, raster, [](const Impl::Row& found) { return found.fileName; });
            if (now == row.fileName) throw;
        }
    }
}

void RasterRegistry::discardRasterOfStage(StageId stage) {
    constexpr const char* kOperation = "RasterRegistry::discardRasterOfStage";
    // The rows first, in one transaction (ruling S51); the files they named go only after
    // the commit, so a row never names a missing file.
    struct Dropped {
        int32_t raster;
        std::string fileName;
    };
    const std::vector<Dropped> dropped = impl_->store.withDurableWriteTransaction(kOperation, [&](sqlite3* db) {
        if (!detail::stageExists(db, stage)) detail::throwUnknownStage(kOperation, stage);
        std::vector<Dropped> rows;
        {
            detail::Statement select(db, "SELECT id, file_name FROM rasters WHERE writer_stage_id = ? ORDER BY id");
            select.bindInt64(1, stage.value());
            while (select.step()) {
                rows.push_back(Dropped{static_cast<int32_t>(select.columnInt64(0)),
                                       select.columnIsNull(1) ? std::string() : select.columnText(1)});
            }
        }
        if (rows.empty()) return rows;
        detail::Statement clear(db,
                                "UPDATE rasters SET writer_stage_id = NULL, level = 0, scale_x = 0, scale_y = 0, "
                                "width = 0, height = 0, file_name = NULL, bytes = 0, populated = 0 "
                                "WHERE writer_stage_id = ?");
        clear.bindInt64(1, stage.value());
        clear.run();
        detail::bumpRastersRevision(db);
        for (const Dropped& row : rows) {
            detail::EventRow event;
            event.kind = EventKind::RasterDropped;
            event.rasterId = row.raster;
            event.stageId = stage.value();
            detail::appendEvent(db, event);
        }
        return rows;
    });
    if (dropped.empty()) return;
    impl_->store.notifyCommitted();
    for (const Dropped& row : dropped) {
        if (!row.fileName.empty()) impl_->deleteOrWarn(kOperation, row.fileName);
    }
}

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

int64_t RasterRegistry::removeUnreferencedFiles() {
    constexpr const char* kOperation = "RasterRegistry::removeUnreferencedFiles";
    impl_->store.checkWritable(kOperation);
    std::error_code error;
    const bool present = std::filesystem::exists(impl_->directoryPath, error);
    if (error) {
        detail::throwIo(std::string(kOperation) + ": cannot inspect the raster directory '" + impl_->directory + "'",
                        error.message());
    }
    if (!present) return 0;

    // The order makes a file the sweep sees either kept or truly unreferenced (ruling
    // S51). A writer joins the live set before it creates its spool, and its TIFF exists
    // only while it is live, so a file listed here belongs to a writer that is live in the
    // snapshot below, or one that has left it: by abandoning, or by committing a row that
    // the refresh after the snapshot sees.
    std::vector<std::string> candidates;
    std::filesystem::directory_iterator entry(impl_->directoryPath, error);
    for (; !error && entry != std::filesystem::directory_iterator(); entry.increment(error)) {
        std::error_code typeError;
        if (!entry->is_regular_file(typeError)) continue;
        std::string name = entry->path().filename().u8string();
        if (detail::isGeneratedRasterFileName(name)) candidates.push_back(std::move(name));
    }
    if (error) {
        detail::throwIo(std::string(kOperation) + ": cannot list the raster directory '" + impl_->directory + "'",
                        error.message());
    }

    std::set<std::string> kept;
    {
        std::lock_guard<std::mutex> guard(impl_->mutex);
        {
            std::lock_guard<std::mutex> liveGuard(impl_->live->mutex);
            for (const auto& owned : impl_->live->byRaster) {
                kept.insert(owned.second.spoolName);
                kept.insert(owned.second.tifName);
            }
        }
        impl_->refresh(kOperation);
        for (const Impl::Row& row : impl_->rows) {
            if (!row.fileName.empty()) kept.insert(row.fileName);
        }
    }

    int64_t reclaimed = 0;
    for (const std::string& name : candidates) {
        if (kept.count(name) != 0) continue;
        std::error_code sizeError;
        const std::uintmax_t size =
            std::filesystem::file_size(impl_->directoryPath / std::filesystem::u8path(name), sizeError);
        if (sizeError) continue;  // gone since the listing
        if (impl_->deleteOrWarn(kOperation, name)) reclaimed += static_cast<int64_t>(size);
    }
    return reclaimed;
}

} // namespace slideio::processor
