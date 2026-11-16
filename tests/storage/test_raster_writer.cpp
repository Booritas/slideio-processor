// Requirements: UR-RES-7, UR-STG-4, UR-OBS-2
// RasterWriter and the publish protocol (rulings S45, S46 and S47): nothing is readable
// until flush() commits, every tile is validated against the declaration and the first
// tile's geometry, and a writer that is destroyed or closed before flush() publishes
// nothing.
#include "storage/detail/store_impl.hpp"
#include "storage/raster_test_util.hpp"
#include "storage/store_fixture.hpp"

#include <slideio/processor/core/block_view.hpp>
#include <slideio/processor/core/data_type.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/log.hpp>
#include <slideio/processor/storage/event_journal.hpp>
#include <slideio/processor/storage/raster_store.hpp>
#include <slideio/processor/storage/stage_state_writer.hpp>
#include <slideio/processor/storage/store.hpp>

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace slideio::processor;

namespace {

std::string messageOf(const std::function<void()>& call) {
    try {
        call();
    } catch (const std::exception& e) {
        return e.what();
    }
    return std::string();
}

bool contains(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

// Collects the Warning messages logged while it lives.
class WarningCapture {
public:
    WarningCapture() : sink_(std::make_shared<Sink>()) { setLogSink(sink_); }
    WarningCapture(const WarningCapture&) = delete;
    WarningCapture& operator=(const WarningCapture&) = delete;
    ~WarningCapture() { setLogSink(nullptr); }

    // How many Warning messages so far contain `text`.
    int count(const char* text) const {
        std::lock_guard<std::mutex> guard(sink_->mutex);
        int n = 0;
        for (const std::string& m : sink_->warnings) n += m.find(text) != std::string::npos ? 1 : 0;
        return n;
    }

private:
    struct Sink : LogSink {
        void write(LogLevel level, const char* message) override {
            std::lock_guard<std::mutex> guard(mutex);
            if (level == LogLevel::Warning) warnings.emplace_back(message);
        }
        mutable std::mutex mutex;
        std::vector<std::string> warnings;
    };
    std::shared_ptr<Sink> sink_;
};

std::vector<Event> eventsOfKind(Store& store, EventKind kind) {
    std::vector<Event> events;
    EventCursorPtr cursor = store.events().subscribe(EventJournal::kFromBeginning);
    while (cursor->next(0)) {
        if (cursor->current().kind() == kind) events.push_back(cursor->current());
    }
    return events;
}

class RasterWriterTest : public StoreFixture {
protected:
    void SetUp() override {
        StoreFixture::SetUp();
        p = store().rasters().declare(RasterDeclaration("probability", DataType::Float32, 1));
        a = store().stageStateWriter().declare("segment", "{}");
        b = store().stageStateWriter().declare("classify", "{}");
    }

    std::filesystem::path directory() { return std::filesystem::u8path(store().rasters().directory()); }

    // The names in directory() ending in `extension`; none when it does not exist.
    std::vector<std::string> filesEndingIn(const char* extension) {
        std::vector<std::string> names;
        std::error_code ec;
        if (!std::filesystem::exists(directory(), ec)) return names;
        for (const auto& entry : std::filesystem::directory_iterator(directory())) {
            if (entry.path().extension() == extension) names.push_back(entry.path().filename().u8string());
        }
        return names;
    }

    // Every file in directory(), whatever its name.
    size_t fileCount() {
        std::error_code ec;
        if (!std::filesystem::exists(directory(), ec)) return 0;
        size_t count = 0;
        for (auto it = std::filesystem::directory_iterator(directory()); it != std::filesystem::directory_iterator(); ++it)
            ++count;
        return count;
    }

    // Runs `sql` on a connection of its own, as a foreign writer would; false on failure.
    bool rawExec(const std::string& sql) {
        sqlite3* db = nullptr;
        bool ok = false;
        if (sqlite3_open_v2(pathUtf8().c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK) {
            ok = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK;
        }
        sqlite3_close(db);
        return ok;
    }

    // The raster row's file_name, read on a connection of its own; "" for NULL.
    std::string rowFileName(RasterId raster) {
        sqlite3* db = nullptr;
        std::string name;
        if (sqlite3_open_v2(pathUtf8().c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
            sqlite3_stmt* select = nullptr;
            const std::string sql = "SELECT file_name FROM rasters WHERE id = " + std::to_string(raster.value());
            if (sqlite3_prepare_v2(db, sql.c_str(), -1, &select, nullptr) == SQLITE_OK &&
                sqlite3_step(select) == SQLITE_ROW && sqlite3_column_text(select, 0) != nullptr) {
                name = reinterpret_cast<const char*>(sqlite3_column_text(select, 0));
            }
            sqlite3_finalize(select);
        }
        sqlite3_close(db);
        return name;
    }

    // A REAL column of the raster row (scale_x, scale_y), read on a connection of its own.
    double rowReal(RasterId raster, const char* column) {
        sqlite3* db = nullptr;
        double value = -1.0;
        if (sqlite3_open_v2(pathUtf8().c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
            sqlite3_stmt* select = nullptr;
            const std::string sql =
                std::string("SELECT ") + column + " FROM rasters WHERE id = " + std::to_string(raster.value());
            if (sqlite3_prepare_v2(db, sql.c_str(), -1, &select, nullptr) == SQLITE_OK &&
                sqlite3_step(select) == SQLITE_ROW) {
                value = sqlite3_column_double(select, 0);
            }
            sqlite3_finalize(select);
        }
        sqlite3_close(db);
        return value;
    }

    RasterId p;
    StageId a;
    StageId b;
};

}  // namespace

// Level 2, scale 4: a 256 x 256 view over 1024 x 1024 level-0 pixels.
TEST_F(RasterWriterTest, NothingIsReadableUntilFlush) {
    RasterRegistry& R = store().rasters();
    RasterWriterPtr w = R.writerForStage(a, p);
    EXPECT_EQ(w->raster(), p);
    EXPECT_EQ(w->stage(), a);
    const auto tile = makeView(2, Rect64{0, 0, 1024, 1024}, 256, 256, DataType::Float32, 1, 1.5);
    w->writeTile(tile->view);
    EXPECT_EQ(w->tilesWritten(), 1);
    EXPECT_FALSE(R.isPopulated(p));
    EXPECT_TRUE(eventsOfKind(store(), EventKind::RasterWritten).empty());
    EXPECT_EQ(filesEndingIn(".spool").size(), 1u);
    EXPECT_TRUE(filesEndingIn(".tif").empty());

    w->flush();
    EXPECT_TRUE(R.isPopulated(p));
    EXPECT_EQ(R.writerStage(p), a);
    EXPECT_EQ(R.writtenAtLevel(p), 2);
    EXPECT_GT(R.bytesOnDisk(p), 0);
    EXPECT_EQ(R.totalBytesOnDisk(), R.bytesOnDisk(p));
    const std::vector<Event> written = eventsOfKind(store(), EventKind::RasterWritten);
    ASSERT_EQ(written.size(), 1u);
    EXPECT_EQ(written[0].raster(), p);
    EXPECT_EQ(written[0].stage(), a);
    EXPECT_EQ(written[0].objectCount(), 1);
    const Rect64 box = written[0].boundingBox();
    EXPECT_EQ(box.x, 0);
    EXPECT_EQ(box.y, 0);
    EXPECT_EQ(box.width, 1024);
    EXPECT_EQ(box.height, 1024);
    EXPECT_TRUE(filesEndingIn(".spool").empty());
    const std::vector<std::string> tifs = filesEndingIn(".tif");
    ASSERT_EQ(tifs.size(), 1u);
    EXPECT_EQ(rowFileName(p), tifs[0]);
    EXPECT_EQ(R.bytesOnDisk(p), static_cast<int64_t>(std::filesystem::file_size(directory() / tifs[0])));
    const RasterPage page = readPage(directory() / tifs[0], 0, 0);
    EXPECT_EQ(page.width, 256u);
    EXPECT_EQ(page.height, 256u);
    EXPECT_EQ(page.at(0, 0), 1.5);
    EXPECT_EQ(page.at(255, 255), 1.5);

    // flush() is terminal.
    EXPECT_NO_THROW(w->flush());
    EXPECT_EQ(eventsOfKind(store(), EventKind::RasterWritten).size(), 1u);
    const std::string late = messageOf([&] { w->writeTile(tile->view); });
    EXPECT_TRUE(contains(late, "flush")) << late;
    EXPECT_THROW(w->writeTile(tile->view), InvalidArgumentError);
    EXPECT_EQ(w->tilesWritten(), 1);
}

TEST_F(RasterWriterTest, TilesAreValidated) {
    RasterWriterPtr w = store().rasters().writerForStage(a, p);
    const auto expectRefused = [&](const BlockView& view, const char* field) {
        const std::string message = messageOf([&] { w->writeTile(view); });
        EXPECT_TRUE(contains(message, field)) << "expected '" << field << "' in: " << message;
        EXPECT_THROW(w->writeTile(view), InvalidArgumentError) << field;
    };
    expectRefused(BlockView(), "null");
    expectRefused(makeView(2, Rect64{0, 0, 1024, 1024}, 256, 256, DataType::Float32, 2, 0.0)->view, "planeCount");
    expectRefused(makeView(2, Rect64{0, 0, 1024, 1024}, 256, 256, DataType::Byte, 1, 0.0)->view, "planes[0].type");
    EXPECT_EQ(w->tilesWritten(), 0);

    // The first accepted tile fixes level 2 and scale 4.
    w->writeTile(makeView(2, Rect64{0, 0, 1024, 1024}, 256, 256, DataType::Float32, 1, 1.0)->view);
    expectRefused(makeView(3, Rect64{1024, 0, 2048, 2048}, 256, 256, DataType::Float32, 1, 0.0)->view, "level");
    // Off by 2 level pixels (8 level-0 pixels) across, and down.
    expectRefused(makeView(2, Rect64{1024, 0, 1032, 1024}, 256, 256, DataType::Float32, 1, 0.0)->view,
                  "slideRect.width");
    expectRefused(makeView(2, Rect64{1024, 0, 1024, 1016}, 256, 256, DataType::Float32, 1, 0.0)->view,
                  "slideRect.height");
    expectRefused(makeView(2, Rect64{-1024, 0, 1024, 1024}, 256, 256, DataType::Float32, 1, 0.0)->view, "origin");
    // An origin of 2^31 - 100 level pixels puts the right edge beyond 2^31 - 1.
    const int64_t far = (int64_t(std::numeric_limits<int32_t>::max()) - 99) * 4;
    expectRefused(makeView(2, Rect64{far, 0, 1024, 1024}, 256, 256, DataType::Float32, 1, 0.0)->view, "extent");
    EXPECT_EQ(w->tilesWritten(), 1);

    // A clipped edge tile: 100 level pixels over 399 level-0 pixels at scale 4.
    w->writeTile(makeView(2, Rect64{1024, 0, 399, 1024}, 100, 256, DataType::Float32, 1, 2.0)->view);
    EXPECT_EQ(w->tilesWritten(), 2);
    w->flush();
    const std::vector<std::string> tifs = filesEndingIn(".tif");
    ASSERT_EQ(tifs.size(), 1u);
    const RasterPage page = readPage(directory() / tifs[0], 0, 0);
    EXPECT_EQ(page.width, 356u);
    EXPECT_EQ(page.height, 256u);
    EXPECT_EQ(page.at(255, 10), 1.0);
    EXPECT_EQ(page.at(256, 10), 2.0);
    EXPECT_EQ(page.at(355, 255), 2.0);
}

// The first tile fixes the level only; the scale is fixed at flush by the widest tile
// (ruling S45 as amended in fix round 1). A clipped edge tile arriving first (100 pixels
// over 399, scale 3.99) must not misplace the full tile after it.
TEST_F(RasterWriterTest, ClippedFirstTileDoesNotFixTheScale) {
    RasterRegistry& R = store().rasters();
    RasterWriterPtr w = R.writerForStage(a, p);
    w->writeTile(makeView(2, Rect64{1024, 0, 399, 1024}, 100, 256, DataType::Float32, 1, 2.0)->view);
    w->writeTile(makeView(2, Rect64{0, 0, 1024, 1024}, 256, 256, DataType::Float32, 1, 1.0)->view);
    w->flush();

    EXPECT_EQ(R.writtenAtLevel(p), 2);
    EXPECT_EQ(rowReal(p, "scale_x"), 4.0);
    EXPECT_EQ(rowReal(p, "scale_y"), 4.0);
    const std::vector<std::string> tifs = filesEndingIn(".tif");
    ASSERT_EQ(tifs.size(), 1u);
    const RasterPage page = readPage(directory() / tifs[0], 0, 0);
    ASSERT_EQ(page.width, 356u);
    ASSERT_EQ(page.height, 256u);
    int64_t wrong = 0;
    for (uint32_t y = 0; y < page.height; ++y)
        for (uint32_t x = 0; x < page.width; ++x)
            if (page.at(x, y) != (x < 256 ? 1.0 : 2.0)) ++wrong;
    EXPECT_EQ(wrong, 0);
}

// A non-integral scale: 5000-pixel tiles over 20001 level-0 pixels (4.0002), and a clipped
// last tile (1234 pixels over 4936, which alone reads as scale 4) written first, far from
// the origin. Every tile lands exactly at 5000 * i.
TEST_F(RasterWriterTest, NonIntegralScalePlacesEveryTileExactly) {
    RasterRegistry& R = store().rasters();
    RasterWriterPtr w = R.writerForStage(a, p);
    constexpr int64_t kWidth = 5000, kSpan = 20001, kRows = 8;
    w->writeTile(makeView(0, Rect64{3 * kSpan, 0, 4936, 32}, 1234, kRows, DataType::Float32, 1, 4.0)->view);
    for (int64_t i = 0; i < 3; ++i) {
        w->writeTile(makeView(0, Rect64{i * kSpan, 0, kSpan, 32}, kWidth, kRows, DataType::Float32, 1,
                              static_cast<double>(i + 1))
                         ->view);
    }
    w->flush();

    EXPECT_EQ(rowReal(p, "scale_x"), 20001.0 / 5000.0);
    EXPECT_EQ(rowReal(p, "scale_y"), 4.0);
    const std::vector<std::string> tifs = filesEndingIn(".tif");
    ASSERT_EQ(tifs.size(), 1u);
    const RasterPage page = readPage(directory() / tifs[0], 0, 0);
    ASSERT_EQ(page.width, 16234u);
    ASSERT_EQ(page.height, 8u);
    int64_t wrong = 0;
    for (uint32_t y = 0; y < page.height; ++y)
        for (uint32_t x = 0; x < page.width; ++x)
            if (page.at(x, y) != static_cast<double>(x / kWidth + 1)) ++wrong;
    EXPECT_EQ(wrong, 0);
}

// A tile within one level pixel of the widest tile seen when it was written, but not of
// the widest tile overall, makes flush refuse; nothing is published.
TEST_F(RasterWriterTest, FlushRefusesATileOffTheFinalScale) {
    RasterRegistry& R = store().rasters();
    RasterWriterPtr w = R.writerForStage(a, p);
    w->writeTile(makeView(0, Rect64{0, 0, 395, 8}, 100, 2, DataType::Float32, 1, 1.0)->view);      // 3.95
    w->writeTile(makeView(0, Rect64{400, 0, 4000, 8}, 1000, 2, DataType::Float32, 1, 2.0)->view);  // 4.0
    EXPECT_EQ(w->tilesWritten(), 2);
    const std::string refused = messageOf([&] { w->flush(); });
    EXPECT_TRUE(contains(refused, "slideRect.width")) << refused;
    EXPECT_FALSE(R.isPopulated(p));
    EXPECT_TRUE(eventsOfKind(store(), EventKind::RasterWritten).empty());
    EXPECT_EQ(fileCount(), 0u);
    EXPECT_NO_THROW(w->flush());  // terminal
    EXPECT_NO_THROW(R.writerForStage(a, p));  // no longer live
}

// Review Focus 2: eight threads, 1000 x 1000 tiles that straddle the 512 grid.
TEST_F(RasterWriterTest, ConcurrentTilesStraddlingTheGrid) {
    RasterRegistry& R = store().rasters();
    const RasterId labels = R.declare(RasterDeclaration("labels", DataType::Int32, 1));
    RasterWriterPtr w = R.writerForStage(a, labels);
    constexpr int64_t kTile = 1000;
    constexpr int kGrid = 4;
    std::atomic<int> next{0};
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&] {
            for (int i = next++; i < kGrid * kGrid; i = next++) {
                const int64_t x0 = (i % kGrid) * kTile, y0 = (i / kGrid) * kTile;
                const auto tile = makeView(0, Rect64{x0, y0, kTile, kTile}, kTile, kTile, DataType::Int32, 1,
                                           [&](int64_t x, int64_t y, int32_t) {
                                               return static_cast<double>((y0 + y) * 4000 + (x0 + x));
                                           });
                try {
                    w->writeTile(tile->view);
                } catch (...) {
                    ++failures;
                }
            }
        });
    }
    for (std::thread& thread : threads) thread.join();
    EXPECT_EQ(failures.load(), 0);
    EXPECT_EQ(w->tilesWritten(), kGrid * kGrid);
    w->flush();

    const std::vector<std::string> tifs = filesEndingIn(".tif");
    ASSERT_EQ(tifs.size(), 1u);
    const RasterPage page = readPage(directory() / tifs[0], 0, 0);
    ASSERT_EQ(page.width, 4000u);
    ASSERT_EQ(page.height, 4000u);
    int64_t wrong = 0;
    for (uint32_t y = 0; y < page.height; ++y)
        for (uint32_t x = 0; x < page.width; ++x)
            if (page.at(x, y) != static_cast<double>(int64_t(y) * 4000 + x)) ++wrong;
    EXPECT_EQ(wrong, 0);
    const std::vector<Event> written = eventsOfKind(store(), EventKind::RasterWritten);
    ASSERT_EQ(written.size(), 1u);
    EXPECT_EQ(written[0].objectCount(), kGrid * kGrid);
    EXPECT_EQ(written[0].boundingBox().width, 4000);
    EXPECT_EQ(written[0].boundingBox().height, 4000);
}

TEST_F(RasterWriterTest, OneWriterOneStage) {
    RasterRegistry& R = store().rasters();
    RasterWriterPtr first = R.writerForStage(a, p);
    const std::string live = messageOf([&] { R.writerForStage(a, p); });
    EXPECT_TRUE(contains(live, "live writer")) << live;
    EXPECT_THROW(R.writerForStage(b, p), InvalidArgumentError);

    first->writeTile(makeView(0, Rect64{0, 0, 64, 64}, 64, 64, DataType::Float32, 1, 1.0)->view);
    first->flush();
    // A flushed writer is no longer live, though it still exists.
    const std::string other = messageOf([&] { R.writerForStage(b, p); });
    EXPECT_TRUE(contains(other, "probability")) << other;
    EXPECT_TRUE(contains(other, "segment")) << other;
    EXPECT_THROW(R.writerForStage(b, p), InvalidArgumentError);
    RasterWriterPtr again;
    EXPECT_NO_THROW(again = R.writerForStage(a, p));

    // A destroyed writer is no longer live either.
    const RasterId q = R.declare(RasterDeclaration("classes", DataType::Byte, 1));
    RasterWriterPtr doomed = R.writerForStage(b, q);
    EXPECT_THROW(R.writerForStage(b, q), InvalidArgumentError);
    doomed.reset();
    EXPECT_NO_THROW(R.writerForStage(b, q));
}

TEST_F(RasterWriterTest, SameStageRerunReplacesThenDeletesTheOldFile) {
    RasterRegistry& R = store().rasters();
    {
        RasterWriterPtr w = R.writerForStage(a, p);
        w->writeTile(makeView(0, Rect64{0, 0, 600, 600}, 600, 600, DataType::Float32, 1, 1.0)->view);
        w->flush();
    }
    const std::string v1 = rowFileName(p);
    ASSERT_FALSE(v1.empty());
    ASSERT_TRUE(std::filesystem::exists(directory() / std::filesystem::u8path(v1)));

    RasterWriterPtr w = R.writerForStage(a, p);
    w->writeTile(makeView(0, Rect64{0, 0, 600, 600}, 600, 600, DataType::Float32, 1, 2.0)->view);
    // Until the re-run publishes, the old file stays the raster's.
    EXPECT_EQ(rowFileName(p), v1);
    EXPECT_TRUE(R.isPopulated(p));
    w->flush();

    const std::string v2 = rowFileName(p);
    EXPECT_NE(v2, v1);
    EXPECT_FALSE(std::filesystem::exists(directory() / std::filesystem::u8path(v1)));
    const std::vector<std::string> tifs = filesEndingIn(".tif");
    ASSERT_EQ(tifs.size(), 1u);
    EXPECT_EQ(tifs[0], v2);
    const RasterPage page = readPage(directory() / tifs[0], 0, 0);
    EXPECT_EQ(page.at(0, 0), 2.0);
    EXPECT_EQ(page.at(599, 599), 2.0);
    EXPECT_EQ(R.bytesOnDisk(p), static_cast<int64_t>(std::filesystem::file_size(directory() / tifs[0])));
    EXPECT_EQ(eventsOfKind(store(), EventKind::RasterWritten).size(), 2u);
}

TEST_F(RasterWriterTest, AbandonedWritersPublishNothing) {
    RasterRegistry& R = store().rasters();
    const auto tile = makeView(0, Rect64{0, 0, 64, 64}, 64, 64, DataType::Float32, 1, 1.0);
    WarningCapture warnings;

    // Destroyed before flush.
    {
        RasterWriterPtr w = R.writerForStage(a, p);
        w->writeTile(tile->view);
        EXPECT_EQ(fileCount(), 1u);  // its spool
    }
    EXPECT_FALSE(R.isPopulated(p));
    EXPECT_TRUE(eventsOfKind(store(), EventKind::RasterWritten).empty());
    EXPECT_EQ(fileCount(), 0u);
    EXPECT_EQ(warnings.count("abandoned"), 1);

    // Flushed with no tile; and destroyed with no tile, which is not worth a warning.
    {
        RasterWriterPtr w = R.writerForStage(a, p);
        w->flush();
        EXPECT_EQ(fileCount(), 0u);
        EXPECT_EQ(w->tilesWritten(), 0);
    }
    R.writerForStage(a, p).reset();
    EXPECT_FALSE(R.isPopulated(p));
    EXPECT_TRUE(eventsOfKind(store(), EventKind::RasterWritten).empty());
    EXPECT_EQ(fileCount(), 0u);
    EXPECT_EQ(warnings.count("abandoned"), 1);

    // Unflushed at Store::close: abandoned there, and refused afterwards.
    RasterWriterPtr w = R.writerForStage(a, p);
    w->writeTile(tile->view);
    store().close();
    EXPECT_EQ(fileCount(), 0u);
    EXPECT_EQ(warnings.count("abandoned"), 2);
    const std::string late = messageOf([&] { w->writeTile(tile->view); });
    EXPECT_TRUE(contains(late, "store is closed")) << late;
    EXPECT_THROW(w->writeTile(tile->view), IoError);
    EXPECT_THROW(w->flush(), IoError);
    EXPECT_EQ(fileCount(), 0u);

    StorePtr reopened = reopen(OpenMode::ReadWrite);
    EXPECT_FALSE(reopened->rasters().isPopulated(p));
    EXPECT_TRUE(eventsOfKind(*reopened, EventKind::RasterWritten).empty());
    w.reset();  // destroying a writer after its store closed is harmless
    EXPECT_EQ(fileCount(), 0u);
}

namespace {

// Flushes `writer` from inside Store::close(), as a stage thread racing the close would.
class LateFlusher final : public detail::GateClient {
public:
    void flushBeforeClose() override {
        try {
            writer->flush();
        } catch (const IoError& e) {
            refusal = e.what();
        }
    }
    RasterWriterPtr writer;
    std::string refusal;  // empty when the flush ran
};

}  // namespace

// Closing never builds a pyramid: a flush that starts once close() has begun is refused,
// and close() then abandons the writer.
TEST_F(RasterWriterTest, FlushDuringCloseIsRefused) {
    RasterRegistry& R = store().rasters();
    const auto late = std::make_shared<LateFlusher>();
    detail::trackForCloseTesting(store(), late);  // before the writer, so close() calls it first
    late->writer = R.writerForStage(a, p);
    late->writer->writeTile(makeView(0, Rect64{0, 0, 64, 64}, 64, 64, DataType::Float32, 1, 1.0)->view);
    WarningCapture warnings;
    store().close();
    EXPECT_TRUE(contains(late->refusal, "store is closed")) << "refusal: '" << late->refusal << "'";
    EXPECT_EQ(warnings.count("abandoned"), 1);
    EXPECT_EQ(fileCount(), 0u);
    late->writer.reset();

    StorePtr reopened = reopen(OpenMode::ReadWrite);
    EXPECT_FALSE(reopened->rasters().isPopulated(p));
    EXPECT_TRUE(eventsOfKind(*reopened, EventKind::RasterWritten).empty());
}

// A flush that fails deletes its partial files, leaves the row as it was, and rethrows.
// The failure is forced by a directory squatting on the name of the writer's .tif.
TEST_F(RasterWriterTest, FailedFlushLeavesTheRasterAsItWas) {
    RasterRegistry& R = store().rasters();
    const auto tile = makeView(0, Rect64{0, 0, 64, 64}, 64, 64, DataType::Float32, 1, 1.0);
    {
        RasterWriterPtr w = R.writerForStage(a, p);
        w->writeTile(tile->view);
        w->flush();
    }
    const std::string v1 = rowFileName(p);
    ASSERT_FALSE(v1.empty());

    RasterWriterPtr w = R.writerForStage(a, p);
    w->writeTile(tile->view);
    const std::vector<std::string> spools = filesEndingIn(".spool");
    ASSERT_EQ(spools.size(), 1u);
    const std::string stem = spools[0].substr(0, spools[0].size() - std::string(".spool").size());
    std::filesystem::create_directory(directory() / std::filesystem::u8path(stem + ".tif"));
    EXPECT_THROW(w->flush(), IoError);

    EXPECT_EQ(rowFileName(p), v1);
    EXPECT_TRUE(R.isPopulated(p));
    EXPECT_EQ(eventsOfKind(store(), EventKind::RasterWritten).size(), 1u);
    EXPECT_EQ(fileCount(), 1u);  // v1 alone: the spool and the squatter are gone
    EXPECT_TRUE(std::filesystem::exists(directory() / std::filesystem::u8path(v1)));
    // The failed writer is finished, and no longer live.
    EXPECT_NO_THROW(w->flush());
    EXPECT_THROW(w->writeTile(tile->view), InvalidArgumentError);
    EXPECT_NO_THROW(R.writerForStage(a, p));
}

// Publication re-checks ownership in its own transaction (ruling S47, fix round 1). The
// race it closes: writerForStage(a, p) reads a cache that predates b's publication of p,
// and b has left the live set by the time a joins it. The test makes the cache stale on
// purpose: b publishes, then a foreign connection winds meta.rasters_revision back, so
// the registry never re-reads the row.
TEST_F(RasterWriterTest, PublishRefusesARasterAnotherStageOwns) {
    RasterRegistry& R = store().rasters();
    const auto tile = makeView(0, Rect64{0, 0, 64, 64}, 64, 64, DataType::Float32, 1, 1.0);
    RasterWriterPtr byB = R.writerForStage(b, p);  // the cache is loaded here: p unpopulated
    byB->writeTile(tile->view);
    byB->flush();
    const std::string ownedByB = rowFileName(p);
    ASSERT_FALSE(ownedByB.empty());
    ASSERT_TRUE(rawExec("UPDATE meta SET value = value - 1 WHERE key = 'rasters_revision';"));

    RasterWriterPtr byA;
    ASSERT_NO_THROW(byA = R.writerForStage(a, p));  // the stale cache lets a in
    byA->writeTile(makeView(0, Rect64{0, 0, 64, 64}, 64, 64, DataType::Float32, 1, 2.0)->view);
    const std::string refused = messageOf([&] { byA->flush(); });
    EXPECT_TRUE(contains(refused, "probability")) << refused;
    EXPECT_TRUE(contains(refused, "classify")) << refused;

    EXPECT_EQ(rowFileName(p), ownedByB);
    EXPECT_TRUE(std::filesystem::exists(directory() / std::filesystem::u8path(ownedByB)));
    const std::vector<std::string> tifs = filesEndingIn(".tif");
    ASSERT_EQ(tifs.size(), 1u);
    EXPECT_EQ(tifs[0], ownedByB);
    EXPECT_TRUE(filesEndingIn(".spool").empty());
    EXPECT_EQ(eventsOfKind(store(), EventKind::RasterWritten).size(), 1u);
    EXPECT_EQ(readPage(directory() / tifs[0], 0, 0).at(0, 0), 1.0);
}

// A publish transaction that fails after the TIFF is built leaves the raster as it was.
// A trigger installed by a foreign connection makes every UPDATE of rasters abort.
TEST_F(RasterWriterTest, FailedPublishTransactionLeavesTheRasterAsItWas) {
    RasterRegistry& R = store().rasters();
    const auto tile = makeView(0, Rect64{0, 0, 64, 64}, 64, 64, DataType::Float32, 1, 1.0);
    {
        RasterWriterPtr w = R.writerForStage(a, p);
        w->writeTile(tile->view);
        w->flush();
    }
    const std::string v1 = rowFileName(p);
    ASSERT_FALSE(v1.empty());

    RasterWriterPtr w = R.writerForStage(a, p);
    w->writeTile(makeView(0, Rect64{0, 0, 64, 64}, 64, 64, DataType::Float32, 1, 2.0)->view);
    ASSERT_TRUE(rawExec("CREATE TRIGGER refuse BEFORE UPDATE ON rasters BEGIN SELECT RAISE(ABORT, 'refused'); END;"));
    EXPECT_THROW(w->flush(), IoError);

    EXPECT_EQ(rowFileName(p), v1);
    EXPECT_TRUE(std::filesystem::exists(directory() / std::filesystem::u8path(v1)));
    const std::vector<std::string> tifs = filesEndingIn(".tif");
    ASSERT_EQ(tifs.size(), 1u);
    EXPECT_EQ(tifs[0], v1);
    EXPECT_TRUE(filesEndingIn(".spool").empty());
    EXPECT_EQ(eventsOfKind(store(), EventKind::RasterWritten).size(), 1u);
    EXPECT_TRUE(R.isPopulated(p));
    EXPECT_NO_THROW(R.writerForStage(a, p));  // the failed writer is no longer live
    ASSERT_TRUE(rawExec("DROP TRIGGER refuse;"));
}

TEST_F(RasterWriterTest, UnknownIdsAndReadOnly) {
    RasterRegistry& R = store().rasters();
    const std::string stage = messageOf([&] { R.writerForStage(StageId(99), p); });
    EXPECT_TRUE(contains(stage, "stage 99")) << stage;
    EXPECT_THROW(R.writerForStage(StageId(99), p), InvalidArgumentError);
    const std::string raster = messageOf([&] { R.writerForStage(a, RasterId(99)); });
    EXPECT_TRUE(contains(raster, "raster 99")) << raster;
    EXPECT_THROW(R.writerForStage(a, RasterId(99)), InvalidArgumentError);
    EXPECT_EQ(fileCount(), 0u);

    StorePtr readOnly = reopen(OpenMode::ReadOnly);
    const std::string refused = messageOf([&] { readOnly->rasters().writerForStage(a, p); });
    EXPECT_TRUE(contains(refused, "read-only")) << refused;
    EXPECT_THROW(readOnly->rasters().writerForStage(a, p), IoError);
    readOnly->close();
    EXPECT_THROW(readOnly->rasters().writerForStage(a, p), IoError);
    EXPECT_EQ(fileCount(), 0u);
}
