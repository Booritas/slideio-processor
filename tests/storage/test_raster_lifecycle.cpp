// Requirements: UR-STG-6, UR-STG-9, UR-OBS-2
// The end of a raster's file (ruling S51): discardRasterOfStage clears the rows in one
// transaction and deletes the files only after the commit; removeUnreferencedFiles
// reclaims what a crash, a failed delete or an abandoned writer left behind, and spares
// rows, live writers and foreign names. A process that dies before flush leaves a
// declared, unpopulated raster (S47).
#include "storage/helper_process.hpp"
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

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

using namespace slideio::processor;

namespace {

// Collects the Warning messages logged while it lives.
class WarningCapture {
public:
    WarningCapture() : sink_(std::make_shared<Sink>()) { setLogSink(sink_); }
    WarningCapture(const WarningCapture&) = delete;
    WarningCapture& operator=(const WarningCapture&) = delete;
    ~WarningCapture() { setLogSink(nullptr); }

    // How many Warning messages so far contain `text`.
    int count(const std::string& text) const {
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

// The names of the files in `directory`; none when it does not exist.
std::set<std::string> fileNames(const std::filesystem::path& directory) {
    std::set<std::string> names;
    std::error_code ec;
    if (!std::filesystem::exists(directory, ec)) return names;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        names.insert(entry.path().filename().u8string());
    }
    return names;
}

// Sample (0, 0) of plane 0 of `source`, read over a 64 x 64 view at level 0.
float firstSample(TileRasterSource& source) {
    auto owned = makeView(0, Rect64{0, 0, 64, 64}, 64, 64, DataType::Float32, 1, 7.0);
    source.fillTile(owned->view);
    float value = 0.0f;
    std::memcpy(&value, owned->view.planeData(0), sizeof(value));
    return value;
}

class RasterLifecycleTest : public StoreFixture {
protected:
    void SetUp() override {
        StoreFixture::SetUp();
        a = store().stageStateWriter().declare("segment", "{}");
        b = store().stageStateWriter().declare("classify", "{}");
    }

    std::filesystem::path directory() { return std::filesystem::u8path(store().rasters().directory()); }

    // Declares a Float32 raster named `name` and has `stage` write it as one 64 x 64 tile
    // at level 0, every sample `value`. Returns the raster and its file's name in `file`.
    RasterId writeRaster(const char* name, StageId stage, float value, std::string& file) {
        RasterRegistry& R = store().rasters();
        const RasterId raster = R.declare(RasterDeclaration(name, DataType::Float32, 1));
        const std::set<std::string> before = fileNames(directory());
        RasterWriterPtr w = R.writerForStage(stage, raster);
        w->writeTile(makeView(0, Rect64{0, 0, 64, 64}, 64, 64, DataType::Float32, 1, value)->view);
        w->flush();
        for (const std::string& created : fileNames(directory())) {
            if (before.count(created) == 0) file = created;
        }
        EXPECT_FALSE(file.empty());
        return raster;
    }

    StageId a;
    StageId b;
};

} // namespace

TEST_F(RasterLifecycleTest, DiscardClearsRowFirstThenDeletesTheFile) {
    RasterRegistry& R = store().rasters();
    std::string pFile, qFile, rFile;
    const RasterId p = writeRaster("p", a, 1.0f, pFile);
    const RasterId q = writeRaster("q", a, 2.0f, qFile);
    const RasterId r = writeRaster("r", b, 3.0f, rFile);
    ASSERT_TRUE(R.isPopulated(p) && R.isPopulated(q) && R.isPopulated(r));

    R.discardRasterOfStage(a);

    EXPECT_FALSE(R.isPopulated(p));
    EXPECT_FALSE(R.isPopulated(q));
    EXPECT_EQ(R.bytesOnDisk(p), 0);
    EXPECT_THROW(R.writerStage(p), InvalidArgumentError);
    // The declarations survive.
    EXPECT_EQ(R.count(), 3);
    EXPECT_STREQ(R.name(p), "p");
    EXPECT_STREQ(R.name(q), "q");
    EXPECT_EQ(R.planeType(q), DataType::Float32);

    const std::vector<Event> dropped = eventsOfKind(store(), EventKind::RasterDropped);
    ASSERT_EQ(dropped.size(), 2u);
    EXPECT_EQ(dropped[0].raster(), p);
    EXPECT_EQ(dropped[1].raster(), q);
    EXPECT_EQ(dropped[0].stage(), a);
    EXPECT_EQ(dropped[1].stage(), a);
    EXPECT_LT(dropped[0].sequence(), dropped[1].sequence());

    const std::set<std::string> files = fileNames(directory());
    EXPECT_EQ(files.count(pFile), 0u);
    EXPECT_EQ(files.count(qFile), 0u);
    EXPECT_EQ(files.count(rFile), 1u);
    EXPECT_TRUE(R.isPopulated(r));
    EXPECT_EQ(R.writerStage(r), b);
    EXPECT_EQ(firstSample(*R.sourceFor(r)), 3.0f);
    EXPECT_EQ(firstSample(*R.sourceFor(p)), 0.0f);

    // A stage with nothing left is a no-op with no event.
    R.discardRasterOfStage(a);
    EXPECT_EQ(eventsOfKind(store(), EventKind::RasterDropped).size(), 2u);

    // The discarded raster is free again: another stage may write it.
    RasterWriterPtr w = R.writerForStage(b, p);
    w->writeTile(makeView(0, Rect64{0, 0, 64, 64}, 64, 64, DataType::Float32, 1, 4.0)->view);
    w->flush();
    EXPECT_EQ(R.writerStage(p), b);

    EXPECT_THROW(R.discardRasterOfStage(StageId(99)), InvalidArgumentError);
    EXPECT_THROW(R.discardRasterOfStage(StageId()), InvalidArgumentError);

    StorePtr readOnly = reopen(OpenMode::ReadOnly);
    EXPECT_THROW(readOnly->rasters().discardRasterOfStage(b), IoError);
    EXPECT_TRUE(readOnly->rasters().isPopulated(r));
}

TEST_F(RasterLifecycleTest, CrashBeforeFlushLeavesAnUnpopulatedRasterAndAnOrphan) {
    store().close();
    const std::filesystem::path listing = uniqueTempPath("raster_crash", ".txt");
    TempFileGuard removeListing(listing);
    ASSERT_EQ(runHelper({pathUtf8(), "raster-crash", listing.u8string()}), 0);

    std::vector<std::string> orphans;
    {
        std::istringstream lines(readFile(listing));
        for (std::string line; std::getline(lines, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) orphans.push_back(line);
        }
    }
    ASSERT_FALSE(orphans.empty());

    StorePtr reopened = reopen(OpenMode::ReadWrite);
    RasterRegistry& R = reopened->rasters();
    RasterId crash;
    ASSERT_TRUE(R.find("crash", crash));
    EXPECT_FALSE(R.isPopulated(crash));
    EXPECT_EQ(R.bytesOnDisk(crash), 0);
    EXPECT_TRUE(eventsOfKind(*reopened, EventKind::RasterWritten).empty());

    int64_t expected = 0;
    for (const std::string& name : orphans) {
        const std::filesystem::path file = directory() / std::filesystem::u8path(name);
        ASSERT_TRUE(std::filesystem::exists(file)) << name;
        expected += static_cast<int64_t>(std::filesystem::file_size(file));
    }
    EXPECT_GT(expected, 0);
    EXPECT_EQ(R.removeUnreferencedFiles(), expected);
    const std::set<std::string> left = fileNames(directory());
    for (const std::string& name : orphans) EXPECT_EQ(left.count(name), 0u) << name;

    // The raster stays declared, and a new writer may populate it.
    RasterWriterPtr w = R.writerForStage(a, crash);
    w->writeTile(makeView(0, Rect64{0, 0, 64, 64}, 64, 64, DataType::Float32, 1, 5.0)->view);
    w->flush();
    EXPECT_EQ(firstSample(*R.sourceFor(crash)), 5.0f);
}

TEST_F(RasterLifecycleTest, SweepSparesRowsLiveWritersAndForeignNames) {
    RasterRegistry& R = store().rasters();
    // No directory yet.
    ASSERT_FALSE(std::filesystem::exists(directory()));
    EXPECT_EQ(R.removeUnreferencedFiles(), 0);

    std::string pFile;
    const RasterId p = writeRaster("p", a, 1.0f, pFile);
    const RasterId q = R.declare(RasterDeclaration("q", DataType::Float32, 1));
    const std::set<std::string> before = fileNames(directory());
    RasterWriterPtr live = R.writerForStage(b, q);
    live->writeTile(makeView(0, Rect64{0, 0, 64, 64}, 64, 64, DataType::Float32, 1, 2.0)->view);
    std::string spool;
    for (const std::string& name : fileNames(directory())) {
        if (before.count(name) == 0) spool = name;
    }
    ASSERT_FALSE(spool.empty());

    const std::string readme = "readme.txt";
    const std::string upper = "ABCDEF0123456789ABCDEF0123456789.tif";
    const std::string stray = "0123456789abcdef0123456789abcdef.tif";
    const std::string straySpool = "fedcba9876543210fedcba9876543210.spool";
    writeFile(directory() / readme, "keep me");
    writeFile(directory() / upper, "keep me too");
    writeFile(directory() / stray, std::string(100, 'x'));
    writeFile(directory() / straySpool, std::string(23, 'y'));
    // A directory with a generated name is not a regular file.
    const std::string folder = "00000000000000000000000000000000.tif";
    std::filesystem::create_directory(directory() / folder);

    EXPECT_EQ(R.removeUnreferencedFiles(), 123);

    const std::set<std::string> left = fileNames(directory());
    EXPECT_EQ(left.count(pFile), 1u);
    EXPECT_EQ(left.count(spool), 1u);
    EXPECT_EQ(left.count(readme), 1u);
    EXPECT_EQ(left.count(upper), 1u);
    EXPECT_EQ(left.count(folder), 1u);
    EXPECT_EQ(left.count(stray), 0u);
    EXPECT_EQ(left.count(straySpool), 0u);
    EXPECT_TRUE(R.isPopulated(p));

    // The live writer was untouched: it still publishes.
    live->flush();
    EXPECT_TRUE(R.isPopulated(q));
    EXPECT_EQ(firstSample(*R.sourceFor(q)), 2.0f);
    EXPECT_EQ(R.removeUnreferencedFiles(), 0);

    StorePtr readOnly = reopen(OpenMode::ReadOnly);
    EXPECT_THROW(readOnly->rasters().removeUnreferencedFiles(), IoError);
    readOnly->close();
    EXPECT_THROW(readOnly->rasters().removeUnreferencedFiles(), IoError);
}

TEST_F(RasterLifecycleTest, DeleteBlockedByAnOpenSourceIsLoggedNotThrown) {
    RasterRegistry& R = store().rasters();
    std::string pFile;
    const RasterId p = writeRaster("p", a, 6.0f, pFile);
    const std::filesystem::path file = directory() / std::filesystem::u8path(pFile);
    const int64_t size = static_cast<int64_t>(std::filesystem::file_size(file));
    TileRasterSourcePtr s = R.sourceFor(p);
    ASSERT_EQ(firstSample(*s), 6.0f);  // the source now holds the file open

    {
        WarningCapture warnings;
        ASSERT_NO_THROW(R.discardRasterOfStage(a));
        EXPECT_FALSE(R.isPopulated(p));
        ASSERT_EQ(eventsOfKind(store(), EventKind::RasterDropped).size(), 1u);
#ifdef _WIN32
        EXPECT_TRUE(std::filesystem::exists(file));
        EXPECT_EQ(warnings.count(pFile), 1);
#else
        EXPECT_FALSE(std::filesystem::exists(file));
        EXPECT_EQ(warnings.count(pFile), 0);
#endif
    }
    // The open source keeps reading the old pixels; a new one reads zero.
    EXPECT_EQ(firstSample(*s), 6.0f);
    EXPECT_EQ(firstSample(*R.sourceFor(p)), 0.0f);

    s.reset();
#ifdef _WIN32
    EXPECT_EQ(R.removeUnreferencedFiles(), size);
#else
    (void)size;
    EXPECT_EQ(R.removeUnreferencedFiles(), 0);
#endif
    EXPECT_FALSE(std::filesystem::exists(file));
}

TEST_F(RasterLifecycleTest, StoreFileCopiedWithoutRastersReadsUnpopulated) {
    std::string pFile;
    const RasterId p = writeRaster("p", a, 8.0f, pFile);
    ASSERT_TRUE(store().rasters().isPopulated(p));
    store().close();

    StoreFilesGuard copy("raster_copy");
    std::filesystem::copy_file(std::filesystem::u8path(pathUtf8()), copy.path());
    StorePtr copied = Store::open(copy.utf8().c_str(), OpenMode::ReadWrite, config());
    RasterRegistry& R = copied->rasters();
    EXPECT_EQ(R.count(), 1);
    EXPECT_STREQ(R.name(p), "p");
    EXPECT_FALSE(R.isPopulated(p));
    EXPECT_EQ(R.bytesOnDisk(p), 0);
    EXPECT_EQ(R.totalBytesOnDisk(), 0);
    EXPECT_EQ(firstSample(*R.sourceFor(p)), 0.0f);
    copied->close();
}
