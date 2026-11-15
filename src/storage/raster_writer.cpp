#include <slideio/processor/storage/raster_store.hpp>

#include "storage/detail/errors.hpp"
#include "storage/detail/event_log.hpp"
#include "storage/detail/file_io.hpp"
#include "storage/detail/pyramid_tiff_writer.hpp"
#include "storage/detail/raster_writer_core.hpp"
#include "storage/detail/schema.hpp"
#include "storage/detail/sqlite.hpp"
#include "storage/detail/store_impl.hpp"

#include <slideio/processor/core/block_view.hpp>
#include <slideio/processor/core/data_type.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/log.hpp>

#include <algorithm>
#include <cmath>
#include <exception>
#include <filesystem>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace slideio::processor {

namespace detail {

namespace {

constexpr const char* kWriteTile = "RasterWriter::writeTile";
constexpr const char* kFlush = "RasterWriter::flush";

// The largest raster extent in level pixels (ruling S45).
constexpr int64_t kMaxExtent = std::numeric_limits<int32_t>::max();

// Removes the writer's entry from the live set, unless a later writer's has replaced it.
void leaveLive(LiveRasterWriters& live, RasterId raster, const LiveRasterFiles& files) noexcept {
    std::lock_guard<std::mutex> guard(live.mutex);
    const auto found = live.byRaster.find(raster.value());
    if (found != live.byRaster.end() && found->second.spoolName == files.spoolName) live.byRaster.erase(found);
}

// Deletes `path` on a clean-up path: a failure is logged at Warning, never thrown, and the
// file is left for RasterRegistry::removeUnreferencedFiles.
void deleteOrWarn(const std::filesystem::path& path, const char* why) noexcept {
    try {
        removeFile(path);
    } catch (const std::exception& e) {
        logMessage(LogLevel::Warning, "RasterWriter: %s: %s", why, e.what());
    } catch (...) {
        logMessage(LogLevel::Warning, "RasterWriter: %s: cannot delete a file", why);
    }
}

} // namespace

// The state of one RasterWriter, shared with the store gate so that Store::close() can
// abandon it (ruling S47). Its destructor abandons too, as a last resort.
class RasterWriterCore final : public GateClient {
public:
    enum class State { Open, Flushed, Abandoned };

    RasterWriterCore(RasterWriterSetup s, std::unique_ptr<TileSpool> sp) : setup(std::move(s)), spool(std::move(sp)) {}
    ~RasterWriterCore() override { abandon("the writer was released"); }
    RasterWriterCore(const RasterWriterCore&) = delete;
    RasterWriterCore& operator=(const RasterWriterCore&) = delete;

    std::filesystem::path pathOf(const std::string& name) const { return setup.directory / std::filesystem::u8path(name); }

    // "raster '<name>'", for messages.
    std::string rasterText() const { return "raster '" + setup.name + "'"; }

    // Refuses a tile the declaration or the first tile's geometry rules out (ruling S45),
    // and returns its origin in level pixels. The first accepted tile fixes the level and
    // scale. Caller holds `lifecycle` shared.
    void place(const BlockView& tile, int64_t& originX, int64_t& originY) {
        if (tile.isNull()) detail::throwInvalid(kWriteTile, "the tile is null");
        if (tile.planeCount() != setup.format.planes) {
            detail::throwInvalid(kWriteTile, "the tile's planeCount " + std::to_string(tile.planeCount()) + " is not " +
                                                 rasterText() + "'s " + std::to_string(setup.format.planes));
        }
        for (int32_t p = 0; p < tile.planeCount(); ++p) {
            if (tile.planeType(p) != setup.format.type) {
                detail::throwInvalid(kWriteTile, "the tile's planes[" + std::to_string(p) + "].type " +
                                                     dataTypeName(tile.planeType(p)) + " is not " + rasterText() +
                                                     "'s " + dataTypeName(setup.format.type));
            }
        }
        const Rect64 rect = tile.slideRect();
        const int64_t width = tile.width();
        const int64_t height = tile.height();

        std::lock_guard<std::mutex> guard(mutex);
        // SDS 01 §1.17: the scale is read from the view's integers, not from a level index.
        const int32_t tileLevel = hasGeometry ? level : tile.level();
        const double sx = hasGeometry ? scaleX : static_cast<double>(rect.width) / static_cast<double>(width);
        const double sy = hasGeometry ? scaleY : static_cast<double>(rect.height) / static_cast<double>(height);
        if (tile.level() != tileLevel) {
            detail::throwInvalid(kWriteTile, "the tile's level " + std::to_string(tile.level()) + " is not " +
                                                 std::to_string(tileLevel) + ", the level of " + rasterText() +
                                                 "'s first tile");
        }
        // Within one level pixel of the first tile's scale, so clipped edge tiles pass.
        const auto offScale = [](int64_t extent, int64_t pixels, double scale) {
            return std::abs(static_cast<double>(extent) - static_cast<double>(pixels) * scale) > scale * (1.0 + 1e-9);
        };
        if (offScale(rect.width, width, sx)) {
            detail::throwInvalid(kWriteTile, "the tile's slideRect.width " + std::to_string(rect.width) + " over " +
                                                 std::to_string(width) + " pixels is off the scale " +
                                                 std::to_string(sx) + " of " + rasterText() +
                                                 "'s first tile by more than one level pixel");
        }
        if (offScale(rect.height, height, sy)) {
            detail::throwInvalid(kWriteTile, "the tile's slideRect.height " + std::to_string(rect.height) + " over " +
                                                 std::to_string(height) + " pixels is off the scale " +
                                                 std::to_string(sy) + " of " + rasterText() +
                                                 "'s first tile by more than one level pixel");
        }
        // Rounded to the nearest level pixel, as rasteriseInto does; checked as doubles
        // first, so a far slideRect cannot overflow.
        const double x = std::round(static_cast<double>(rect.x) / sx);
        const double y = std::round(static_cast<double>(rect.y) / sy);
        if (x < 0 || y < 0) {
            detail::throwInvalid(kWriteTile, "the tile's origin (" + std::to_string(static_cast<int64_t>(x)) + ", " +
                                                 std::to_string(static_cast<int64_t>(y)) +
                                                 ") in level pixels is negative");
        }
        if (x + static_cast<double>(width) > static_cast<double>(kMaxExtent) ||
            y + static_cast<double>(height) > static_cast<double>(kMaxExtent)) {
            detail::throwInvalid(kWriteTile, "the tile's extent reaches beyond 2^31-1 level pixels");
        }
        originX = static_cast<int64_t>(x);
        originY = static_cast<int64_t>(y);
        if (!hasGeometry) {
            hasGeometry = true;
            level = tileLevel;
            scaleX = sx;
            scaleY = sy;
        }
    }

    // Counts an appended tile into tilesWritten and the event's bounding box.
    void counted(const Rect64& rect) {
        std::lock_guard<std::mutex> guard(mutex);
        if (tiles == 0) {
            minX = rect.x;
            minY = rect.y;
            maxX = rect.right();
            maxY = rect.bottom();
        } else {
            minX = std::min(minX, rect.x);
            minY = std::min(minY, rect.y);
            maxX = std::max(maxX, rect.right());
            maxY = std::max(maxY, rect.bottom());
        }
        ++tiles;
    }

    int64_t tileCount() const {
        std::lock_guard<std::mutex> guard(mutex);
        return tiles;
    }

    // Throws IoError "<operation>: store is closed ('<path>')".
    [[noreturn]] void throwClosed(const char* operation) const {
        const std::string message = std::string(operation) + ": store is closed ('" + setup.gate->path() + "')";
        throw IoError(message.c_str());
    }

    void writeTile(const BlockView& tile) {
        std::shared_lock<std::shared_mutex> life(lifecycle);
        // Refused once the store is closing, as ObjectWriter::emit is: Store::close then
        // abandons the writer, which waits for this call to return.
        setup.gate->with(kWriteTile, [](StoreImpl& store) { store.checkAcceptingObjects(kWriteTile); });
        if (state == State::Abandoned) throwClosed(kWriteTile);
        if (state == State::Flushed) {
            detail::throwInvalid(kWriteTile, rasterText() + " was flushed; a flushed writer accepts no more tiles");
        }
        int64_t originX = 0, originY = 0;
        place(tile, originX, originY);
        std::vector<const void*> planes(static_cast<size_t>(setup.format.planes));
        std::vector<int64_t> strides(planes.size());
        for (int32_t p = 0; p < setup.format.planes; ++p) {
            planes[static_cast<size_t>(p)] = tile.planeData(p);
            strides[static_cast<size_t>(p)] = tile.planeRowStrideBytes(p);
        }
        // Compressed outside every lock but the shared one (ruling S46).
        spool->append(originX, originY, tile.width(), tile.height(), planes.data(), strides.data());
        counted(tile.slideRect());
    }

    // Builds the file, then publishes it in one transaction (ruling S47).
    void flush() {
        std::unique_lock<std::shared_mutex> life(lifecycle);
        setup.gate->with(kFlush, [](StoreImpl& store) { store.checkWritable(kFlush); });
        if (state == State::Abandoned) throwClosed(kFlush);
        if (state == State::Flushed) return;
        state = State::Flushed;  // terminal, whatever follows

        EventRow event;
        event.kind = EventKind::RasterWritten;
        event.rasterId = setup.raster.value();
        event.stageId = setup.stage.value();
        PyramidMetadata metadata;
        metadata.name = setup.name;
        metadata.description = setup.description;
        {
            std::lock_guard<std::mutex> guard(mutex);
            event.objectCount = tiles;
            event.minX = minX;
            event.minY = minY;
            event.maxX = maxX;
            event.maxY = maxY;
            metadata.level = level;
            metadata.scaleX = scaleX;
            metadata.scaleY = scaleY;
        }
        if (event.objectCount == 0) {
            leaveLive(*setup.live, setup.raster, setup.files);
            spool->remove();
            return;
        }

        const std::filesystem::path tif = pathOf(setup.files.tifName);
        PyramidInfo info;
        int64_t bytes = 0;
        try {
            info = writePyramidTiff(tif, *spool, setup.format, metadata);
            // A committed row never names a file that is not yet on disk.
            syncFile(tif);
            std::error_code error;
            const std::uintmax_t size = std::filesystem::file_size(tif, error);
            if (error) throwIo(std::string(kFlush) + ": cannot read the size of '" + tif.u8string() + "'", error.message());
            bytes = static_cast<int64_t>(size);
        } catch (...) {
            deleteOrWarn(tif, "deleting a partial raster file");
            removeSpool();
            leaveLive(*setup.live, setup.raster, setup.files);
            throw;
        }
        removeSpool();

        std::string oldFile;
        try {
            setup.gate->with(kFlush, [&](StoreImpl& store) {
                oldFile = store.withWriteTransaction(kFlush, [&](sqlite3* db) { return publish(db, info, metadata, bytes, event); });
                store.notifyCommitted();
            });
        } catch (...) {
            deleteOrWarn(tif, "deleting an unpublished raster file");
            leaveLive(*setup.live, setup.raster, setup.files);
            throw;
        }
        leaveLive(*setup.live, setup.raster, setup.files);
        // A re-run by the same stage replaced the file; the old one goes only now, so the
        // row never names a missing file. On Windows a source may still hold it open.
        if (!oldFile.empty() && oldFile != setup.files.tifName) {
            deleteOrWarn(pathOf(oldFile), "deleting the replaced raster file");
        }
    }

    // Points the row at the new file and appends RasterWritten (ruling S49). Returns the
    // file the row named before, or "". This is the authoritative ownership check:
    // writerForStage refuses early from the registry's cache, but another stage may have
    // published since, so a raster populated by another stage is refused here, in the
    // transaction that would overwrite it.
    std::string publish(sqlite3* db, const PyramidInfo& info, const PyramidMetadata& metadata, int64_t bytes,
                        const EventRow& event) {
        std::string oldFile;
        {
            Statement select(db, "SELECT file_name, writer_stage_id, populated FROM rasters WHERE id = ?");
            select.bindInt64(1, setup.raster.value());
            if (!select.step()) throw InternalError((std::string(kFlush) + ": the raster row is gone").c_str());
            const int64_t owner = select.columnInt64(1);  // NULL reads 0
            if (select.columnInt64(2) != 0 && owner != setup.stage.value()) {
                Statement stage(db, "SELECT name FROM stages WHERE id = ?");
                stage.bindInt64(1, owner);
                const std::string id = std::to_string(owner);
                const std::string ownerText = stage.step() ? "'" + stage.columnText(0) + "' (id " + id + ")" : id;
                detail::throwInvalid(kFlush, rasterText() + " was published by stage " + ownerText +
                                                 " while this writer ran; only that stage may write it until it is "
                                                 "discarded. Nothing was published");
            }
            if (!select.columnIsNull(0)) oldFile = select.columnText(0);
        }
        Statement update(db,
                         "UPDATE rasters SET writer_stage_id = ?, level = ?, scale_x = ?, scale_y = ?, width = ?, "
                         "height = ?, file_name = ?, bytes = ?, populated = 1 WHERE id = ?");
        update.bindInt64(1, setup.stage.value());
        update.bindInt64(2, metadata.level);
        update.bindDouble(3, metadata.scaleX);
        update.bindDouble(4, metadata.scaleY);
        update.bindInt64(5, info.width);
        update.bindInt64(6, info.height);
        update.bindText(7, setup.files.tifName);
        update.bindInt64(8, bytes);
        update.bindInt64(9, setup.raster.value());
        update.run();
        bumpRastersRevision(db);
        appendEvent(db, event);
        return oldFile;
    }

    // Deletes the spool; a failure is logged, and the sweep reclaims the file.
    void removeSpool() noexcept {
        try {
            spool->remove();
        } catch (const std::exception& e) {
            logMessage(LogLevel::Warning, "RasterWriter: deleting the spool of %s: %s", rasterText().c_str(), e.what());
        } catch (...) {
        }
    }

    // Discards every tile: deletes the spool and leaves the live set. Never publishes,
    // never throws, and is a no-op once flushed or abandoned. A partial TIFF exists only
    // inside flush(), which holds `lifecycle` and deletes its own.
    void abandon(const char* why) noexcept {
        try {
            std::unique_lock<std::shared_mutex> life(lifecycle);
            if (state != State::Open) return;
            state = State::Abandoned;
            leaveLive(*setup.live, setup.raster, setup.files);
            removeSpool();
            const int64_t abandoned = tileCount();
            if (abandoned > 0) {
                logMessage(LogLevel::Warning,
                           "RasterWriter: %lld tile(s) of %s by stage %d in '%s' were abandoned unflushed (%s)",
                           static_cast<long long>(abandoned), rasterText().c_str(), setup.stage.value(),
                           setup.gate->path().c_str(), why);
            }
        } catch (...) {
        }
    }

    // Store::close() abandons a writer that was not flushed: closing never builds a
    // pyramid, and a stage that did not finish leaves its raster unpopulated.
    void flushBeforeClose() override { abandon("the store was closed"); }

    const RasterWriterSetup setup;
    const std::unique_ptr<TileSpool> spool;

    // Held shared by writeTile for its whole call, and exclusively by flush and abandon,
    // so neither ever runs beside an append.
    std::shared_mutex lifecycle;
    State state = State::Open;  // written only under `lifecycle` held exclusively

    mutable std::mutex mutex;  // guards everything below
    bool hasGeometry = false;
    int32_t level = 0;
    double scaleX = 0.0;
    double scaleY = 0.0;
    int64_t tiles = 0;
    int64_t minX = 0, minY = 0, maxX = 0, maxY = 0;  // the union of the tiles' slideRects
};

} // namespace detail

RasterWriterPtr RasterWriter::Impl::open(detail::RasterWriterSetup setup) {
    const std::shared_ptr<detail::LiveRasterWriters> live = setup.live;
    const RasterId raster = setup.raster;
    const detail::LiveRasterFiles files = setup.files;
    const std::filesystem::path spoolPath = setup.directory / std::filesystem::u8path(files.spoolName);
    try {
        auto spool = std::make_unique<detail::TileSpool>(spoolPath, setup.format);
        // From here the core's destructor abandons: it deletes the spool and leaves the set.
        auto core = std::make_shared<detail::RasterWriterCore>(std::move(setup), std::move(spool));
        core->setup.gate->track(core);
        auto impl = std::make_unique<Impl>();
        impl->core = std::move(core);
        return RasterWriterPtr(new RasterWriter(std::move(impl)));
    } catch (...) {
        detail::leaveLive(*live, raster, files);
        detail::deleteOrWarn(spoolPath, "deleting the spool of a writer that failed to open");
        throw;
    }
}

RasterWriter::RasterWriter(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

RasterWriter::~RasterWriter() { impl_->core->abandon("the writer was destroyed unflushed"); }

void RasterWriter::writeTile(const BlockView& tile) { impl_->core->writeTile(tile); }

void RasterWriter::flush() { impl_->core->flush(); }

int64_t RasterWriter::tilesWritten() const { return impl_->core->tileCount(); }

RasterId RasterWriter::raster() const noexcept { return impl_->core->setup.raster; }

StageId RasterWriter::stage() const noexcept { return impl_->core->setup.stage; }

} // namespace slideio::processor
