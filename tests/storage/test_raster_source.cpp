// Requirements: UR-RES-7, UR-STG-10
// RasterRegistry::sourceFor (ruling S50): an unpopulated raster reads zero, a populated
// one reads its file at any scale and region (the coarsest page no coarser than the view,
// sampled nearest; zero outside the extent), and a source stays bound to the file the row
// named when it was made.
#include "storage/detail/tiff_raster_reader.hpp"
#include "storage/raster_test_util.hpp"
#include "storage/store_fixture.hpp"

#include <slideio/processor/core/block_view.hpp>
#include <slideio/processor/core/data_type.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/log.hpp>
#include <slideio/processor/storage/raster_store.hpp>
#include <slideio/processor/storage/stage_state_writer.hpp>
#include <slideio/processor/storage/store.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace slideio::processor;

namespace {

// The written raster of the gradient tests: level 1, scale 2, 2048 x 2048 level pixels.
constexpr int64_t kSide = 2048;

int32_t gradient(int64_t x, int64_t y) { return static_cast<int32_t>(x + y * kSide); }

// Sample (x, y) of plane `plane` of a filled view, as T.
template <class T>
T at(const OwnedView& owned, int32_t plane, int64_t x, int64_t y) {
    T value{};
    const auto* row = static_cast<const uint8_t*>(owned.view.planeData(plane)) + y * owned.view.planeRowStrideBytes(plane);
    std::memcpy(&value, row + x * static_cast<int64_t>(sizeof(T)), sizeof(T));
    return value;
}

// Discards every log message while it lives, so an expected Warning keeps the output clean.
class QuietLog {
public:
    QuietLog() { setLogSink(std::make_shared<Sink>()); }
    QuietLog(const QuietLog&) = delete;
    QuietLog& operator=(const QuietLog&) = delete;
    ~QuietLog() { setLogSink(nullptr); }

private:
    struct Sink : LogSink {
        void write(LogLevel, const char*) override {}
    };
};

class RasterSourceTest : public StoreFixture {
protected:
    void SetUp() override {
        StoreFixture::SetUp();
        a = store().stageStateWriter().declare("segment", "{}");
    }

    std::filesystem::path directory() { return std::filesystem::u8path(store().rasters().directory()); }

    // The only .tif in directory().
    std::filesystem::path onlyTif() {
        std::vector<std::filesystem::path> tifs;
        for (const auto& entry : std::filesystem::directory_iterator(directory())) {
            if (entry.path().extension() == ".tif") tifs.push_back(entry.path());
        }
        EXPECT_EQ(tifs.size(), 1u);
        return tifs.empty() ? std::filesystem::path() : tifs[0];
    }

    // Declares an Int32 raster and writes the gradient at level 1, scale 2, as four
    // 1024 x 1024 tiles. Its pages are 2048, 1024 and 512 pixels square.
    RasterId writeGradient() {
        RasterRegistry& R = store().rasters();
        const RasterId raster = R.declare(RasterDeclaration("gradient", DataType::Int32, 1));
        RasterWriterPtr w = R.writerForStage(a, raster);
        for (int64_t ty = 0; ty < 2; ++ty) {
            for (int64_t tx = 0; tx < 2; ++tx) {
                const int64_t x0 = tx * 1024, y0 = ty * 1024;
                w->writeTile(makeView(1, Rect64{x0 * 2, y0 * 2, 2048, 2048}, 1024, 1024, DataType::Int32, 1,
                                      [&](int64_t x, int64_t y, int32_t) { return double(gradient(x0 + x, y0 + y)); })
                                 ->view);
            }
        }
        w->flush();
        return raster;
    }

    // A view of `width` x `height` over `slideRect`, prefilled with 7, then filled by `source`.
    static std::unique_ptr<OwnedView> fill(TileRasterSource& source, Rect64 slideRect, int64_t width, int64_t height) {
        auto owned = makeView(0, slideRect, width, height, source.planeType(), source.planeCount(), 7.0);
        source.fillTile(owned->view);
        return owned;
    }

    StageId a;
};

}  // namespace

TEST_F(RasterSourceTest, UnpopulatedReadsZero) {
    RasterRegistry& R = store().rasters();
    const RasterId p = R.declare(RasterDeclaration("probability", DataType::Float32, 2));
    TileRasterSourcePtr s = R.sourceFor(p);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->planeType(), DataType::Float32);
    EXPECT_EQ(s->planeCount(), 2);
    const auto view = fill(*s, Rect64{0, 0, 64, 64}, 64, 64);
    for (int32_t plane = 0; plane < 2; ++plane)
        for (int64_t y = 0; y < 64; ++y)
            for (int64_t x = 0; x < 64; ++x) ASSERT_EQ(at<float>(*view, plane, x, y), 0.0f) << x << ", " << y;
    EXPECT_THROW(R.sourceFor(RasterId(9)), InvalidArgumentError);

    // A row that names a file no longer there is not populated either (ruling S48).
    const RasterId orphaned = writeGradient();
    ASSERT_TRUE(std::filesystem::remove(onlyTif()));
    const auto missing = fill(*R.sourceFor(orphaned), Rect64{0, 0, 128, 128}, 64, 64);
    EXPECT_EQ(at<int32_t>(*missing, 0, 63, 63), 0);
}

TEST_F(RasterSourceTest, ReadsBackAtTheWrittenScale) {
    TileRasterSourcePtr s = store().rasters().sourceFor(writeGradient());
    EXPECT_EQ(s->planeType(), DataType::Int32);
    EXPECT_EQ(s->planeCount(), 1);
    // Level-pixel origins: the corner, one straddling the 512 grid, and one ending at the edge.
    const std::vector<std::pair<int64_t, int64_t>> origins = {{0, 0}, {500, 700}, {1748, 1848}};
    for (const auto& origin : origins) {
        const auto view = fill(*s, Rect64{origin.first * 2, origin.second * 2, 600, 400}, 300, 200);
        for (int64_t y = 0; y < 200; ++y)
            for (int64_t x = 0; x < 300; ++x)
                ASSERT_EQ(at<int32_t>(*view, 0, x, y), gradient(origin.first + x, origin.second + y))
                    << "origin " << origin.first << ", " << origin.second << " at " << x << ", " << y;
    }
}

// Review Focus 4: a view coarser than the raster reads the coarsest page no coarser than
// the view; a finer one up-samples page 0.
TEST_F(RasterSourceTest, CoarserReadsUseThePyramidFinerReadsUpsample) {
    TileRasterSourcePtr s = store().rasters().sourceFor(writeGradient());
    const RasterPage page1 = readPage(onlyTif(), 1, 0);
    const RasterPage page2 = readPage(onlyTif(), 2, 0);
    ASSERT_EQ(page1.width, 1024u);
    ASSERT_EQ(page2.width, 512u);

    // Scale 8 is page 2's: view pixel (i, j) is page pixel (100 + i, 200 + j).
    {
        const auto view = fill(*s, Rect64{800, 1600, 2400, 1600}, 300, 200);
        for (int64_t y = 0; y < 200; ++y)
            for (int64_t x = 0; x < 300; ++x)
                ASSERT_EQ(at<int32_t>(*view, 0, x, y),
                          static_cast<int32_t>(page2.at(static_cast<uint32_t>(100 + x), static_cast<uint32_t>(200 + y))))
                    << x << ", " << y;
    }
    // Scale 1, finer than the written 2: each written pixel repeated 2 x 2.
    {
        const auto view = fill(*s, Rect64{1000, 600, 300, 200}, 300, 200);
        for (int64_t y = 0; y < 200; ++y)
            for (int64_t x = 0; x < 300; ++x)
                ASSERT_EQ(at<int32_t>(*view, 0, x, y), gradient(500 + x / 2, 300 + y / 2)) << x << ", " << y;
    }
    // Scale 6 reads page 1 (scale 4 <= 6 < 8), sampled nearest at the pixel centres.
    {
        const auto view = fill(*s, Rect64{1200, 600, 1800, 1200}, 300, 200);
        for (int64_t y = 0; y < 200; ++y) {
            for (int64_t x = 0; x < 300; ++x) {
                const auto px = static_cast<uint32_t>(std::floor((1200 + (x + 0.5) * 6) / 4));
                const auto py = static_cast<uint32_t>(std::floor((600 + (y + 0.5) * 6) / 4));
                ASSERT_EQ(at<int32_t>(*view, 0, x, y), static_cast<int32_t>(page1.at(px, py))) << x << ", " << y;
            }
        }
    }
}

// Review Focus 4: inside the extent the read is nearest, outside it is zero.
TEST_F(RasterSourceTest, OutsideTheExtentIsZero) {
    TileRasterSourcePtr s = store().rasters().sourceFor(writeGradient());
    // Half beyond the right edge, at the written scale: level pixels 1948..2147.
    {
        const auto view = fill(*s, Rect64{3896, 200, 400, 400}, 200, 200);
        for (int64_t y = 0; y < 200; ++y)
            for (int64_t x = 0; x < 200; ++x)
                ASSERT_EQ(at<int32_t>(*view, 0, x, y), x < 100 ? gradient(1948 + x, 100 + y) : 0) << x << ", " << y;
    }
    // Half beyond the top-left corner, at the written scale.
    {
        const auto view = fill(*s, Rect64{-200, -200, 400, 400}, 200, 200);
        for (int64_t y = 0; y < 200; ++y)
            for (int64_t x = 0; x < 200; ++x)
                ASSERT_EQ(at<int32_t>(*view, 0, x, y), x >= 100 && y >= 100 ? gradient(x - 100, y - 100) : 0)
                    << x << ", " << y;
    }
    // Scale 8 (page 2, 512 pixels square), half beyond the bottom edge.
    {
        const RasterPage page2 = readPage(onlyTif(), 2, 0);
        const auto view = fill(*s, Rect64{0, 3296, 800, 1600}, 100, 200);
        for (int64_t y = 0; y < 200; ++y)
            for (int64_t x = 0; x < 100; ++x)
                ASSERT_EQ(at<int32_t>(*view, 0, x, y),
                          y < 100 ? static_cast<int32_t>(page2.at(static_cast<uint32_t>(x), static_cast<uint32_t>(412 + y)))
                                  : 0)
                    << x << ", " << y;
    }
    // Wholly outside.
    {
        const auto view = fill(*s, Rect64{5000, 5000, 64, 64}, 64, 64);
        for (int64_t y = 0; y < 64; ++y)
            for (int64_t x = 0; x < 64; ++x) ASSERT_EQ(at<int32_t>(*view, 0, x, y), 0) << x << ", " << y;
    }
}

TEST_F(RasterSourceTest, ViewIsValidated) {
    RasterRegistry& R = store().rasters();
    const RasterId unpopulated = R.declare(RasterDeclaration("probability", DataType::Float32, 2));
    for (const TileRasterSourcePtr& s : {R.sourceFor(writeGradient()), R.sourceFor(unpopulated)}) {
        const DataType type = s->planeType();
        const int32_t planes = s->planeCount();
        const DataType other = type == DataType::Int32 ? DataType::Float32 : DataType::Int32;
        BlockView null;
        EXPECT_THROW(s->fillTile(null), InvalidArgumentError);
        EXPECT_THROW(s->fillTile(makeView(0, Rect64{0, 0, 64, 64}, 64, 64, type, planes + 1, 0.0)->view),
                     InvalidArgumentError);
        EXPECT_THROW(s->fillTile(makeView(0, Rect64{0, 0, 64, 64}, 64, 64, other, planes, 0.0)->view),
                     InvalidArgumentError);
        // No derivable scale: an empty level-0 rectangle.
        EXPECT_THROW(s->fillTile(makeView(0, Rect64{0, 0, 0, 64}, 64, 64, type, planes, 0.0)->view),
                     InvalidArgumentError);
        EXPECT_THROW(s->fillTile(makeView(0, Rect64{0, 0, 64, 0}, 64, 64, type, planes, 0.0)->view),
                     InvalidArgumentError);
        EXPECT_NO_THROW(s->fillTile(makeView(0, Rect64{0, 0, 64, 64}, 64, 64, type, planes, 0.0)->view));
    }
}

TEST_F(RasterSourceTest, ConcurrentFillsFromManyThreads) {
    TileRasterSourcePtr s = store().rasters().sourceFor(writeGradient());
    struct Request {
        Rect64 slideRect;
        int64_t width;
        int64_t height;
    };
    constexpr int kThreads = 8;
    constexpr int kViews = 200;
    std::mt19937 random(20261115);
    const int64_t scales[] = {1, 2, 3, 6, 8, 16};
    std::uniform_int_distribution<int> pickScale(0, 5);
    std::uniform_int_distribution<int64_t> pickSize(1, 100);
    std::uniform_int_distribution<int64_t> pickOrigin(-1000, 5000);
    std::vector<Request> requests;
    for (int i = 0; i < kThreads * kViews; ++i) {
        const int64_t scale = scales[pickScale(random)];
        const int64_t width = pickSize(random), height = pickSize(random);
        requests.push_back(Request{Rect64{pickOrigin(random), pickOrigin(random), width * scale, height * scale},
                                   width, height});
    }
    // The single-threaded reference.
    std::vector<std::vector<int32_t>> reference;
    for (const Request& r : requests) {
        const auto view = fill(*s, r.slideRect, r.width, r.height);
        std::vector<int32_t> samples;
        for (int64_t y = 0; y < r.height; ++y)
            for (int64_t x = 0; x < r.width; ++x) samples.push_back(at<int32_t>(*view, 0, x, y));
        reference.push_back(std::move(samples));
    }

    std::vector<int> mismatches(kThreads, 0);
    std::vector<std::string> failures(kThreads);
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            try {
                for (int i = t * kViews; i < (t + 1) * kViews; ++i) {
                    const Request& r = requests[static_cast<size_t>(i)];
                    const auto view = fill(*s, r.slideRect, r.width, r.height);
                    size_t k = 0;
                    for (int64_t y = 0; y < r.height; ++y)
                        for (int64_t x = 0; x < r.width; ++x)
                            mismatches[t] += at<int32_t>(*view, 0, x, y) != reference[static_cast<size_t>(i)][k++];
                }
            } catch (const std::exception& e) {
                failures[t] = e.what();
            }
        });
    }
    for (std::thread& thread : threads) thread.join();
    for (int t = 0; t < kThreads; ++t) {
        EXPECT_EQ(mismatches[t], 0) << "thread " << t;
        EXPECT_TRUE(failures[t].empty()) << "thread " << t << ": " << failures[t];
    }
}

TEST_F(RasterSourceTest, BoundToTheFileAtCallTime) {
    RasterRegistry& R = store().rasters();
    const RasterId p = R.declare(RasterDeclaration("probability", DataType::Float32, 1));
    const auto write = [&](double value) {
        RasterWriterPtr w = R.writerForStage(a, p);
        w->writeTile(makeView(0, Rect64{0, 0, 600, 600}, 600, 600, DataType::Float32, 1, value)->view);
        w->flush();
    };
    write(1.0);
    const std::filesystem::path v1 = onlyTif();
    TileRasterSourcePtr s1 = R.sourceFor(p);
    {
        QuietLog quiet;  // on Windows the re-run cannot delete v1, which s1 holds, and warns
        write(2.0);
    }
#ifdef _WIN32
    EXPECT_TRUE(std::filesystem::exists(v1));
#else
    EXPECT_FALSE(std::filesystem::exists(v1));
#endif
    EXPECT_EQ(at<float>(*fill(*s1, Rect64{0, 0, 600, 600}, 600, 600), 0, 599, 599), 1.0f);
    EXPECT_EQ(at<float>(*fill(*s1, Rect64{0, 0, 64, 64}, 64, 64), 0, 0, 0), 1.0f);
    TileRasterSourcePtr s2 = R.sourceFor(p);
    EXPECT_EQ(at<float>(*fill(*s2, Rect64{0, 0, 600, 600}, 600, 600), 0, 599, 599), 2.0f);
    // On Windows v1 outlives s1 as an orphan, until the sweep (Task 6).
    s1.reset();
}

namespace slideio::processor::detail {

// The friend TiffRasterReader names: drives its handle pool directly
// (tiff_raster_reader.cpp is compiled into this test).
struct TiffRasterReaderTestSeam {
    using HandlePtr = std::unique_ptr<TiffRasterReader::Handle>;
    static HandlePtr acquire(TiffRasterReader& reader) { return reader.acquire(); }
    static void discard(TiffRasterReader& reader, HandlePtr handle) { reader.discard(std::move(handle)); }
    static void setOpenHook(TiffRasterReader& reader, std::function<void()> hook) {
        reader.openHook_ = std::move(hook);
    }
    static int32_t openCount(TiffRasterReader& reader) {
        std::lock_guard<std::mutex> guard(reader.mutex_);
        return reader.open_;
    }
    static void wakeWaiters(TiffRasterReader& reader) {
        std::lock_guard<std::mutex> guard(reader.mutex_);
        reader.returned_.notify_all();
    }
};

} // namespace slideio::processor::detail

namespace {

constexpr std::chrono::seconds kDeadline(10);

// Polls `condition` until it holds or kDeadline passes; returns whether it held.
bool eventually(const std::function<bool()>& condition) {
    const auto deadline = std::chrono::steady_clock::now() + kDeadline;
    while (!condition()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

}  // namespace

// A caller waiting for a pooled handle must wake when the last open handle goes: here the
// last one is an open that fails after the only file handle was discarded. The waiter then
// tries an open of its own and throws, rather than waiting forever (ruling S50).
TEST_F(RasterSourceTest, PoolWaiterWakesWhenTheLastOpenFails) {
    using Seam = detail::TiffRasterReaderTestSeam;
    writeGradient();
    detail::TiffRasterReader reader(onlyTif(), detail::RasterFormat{DataType::Int32, 1, RasterCompression::Deflate},
                                    2.0, 2.0, kSide, kSide);
    Seam::HandlePtr held = Seam::acquire(reader);  // the constructor's handle; no other is open

    // Every later open blocks until released, then fails.
    std::mutex mutex;
    std::condition_variable changed;
    int arrivals = 0;
    int released = 0;
    bool releaseAll = false;
    Seam::setOpenHook(reader, [&] {
        std::unique_lock<std::mutex> lock(mutex);
        const int arrival = arrivals++;
        changed.notify_all();
        changed.wait(lock, [&] { return releaseAll || arrival < released; });
        throw IoError("injected open failure");
    });
    const auto arrived = [&](int n) {
        return eventually([&] {
            std::lock_guard<std::mutex> guard(mutex);
            return arrivals >= n;
        });
    };
    const auto release = [&](int n, bool all) {
        std::lock_guard<std::mutex> guard(mutex);
        released = n;
        releaseAll = all;
        changed.notify_all();
    };
    // "" when a handle was acquired (and discarded again), else the failure's text.
    const auto acquireOrFail = [&]() -> std::string {
        try {
            Seam::discard(reader, Seam::acquire(reader));
            return std::string();
        } catch (const IoError& e) {
            return e.what();
        }
    };

    std::future<std::string> waiter = std::async(std::launch::async, acquireOrFail);
    ASSERT_TRUE(arrived(1));
    std::future<std::string> last = std::async(std::launch::async, acquireOrFail);
    ASSERT_TRUE(arrived(2));
    Seam::discard(reader, std::move(held));
    // The first open fails while the second is still open: that caller waits.
    release(1, false);
    ASSERT_TRUE(eventually([&] { return Seam::openCount(reader) == 1; }));
    // The second fails too, and no handle is open: the waiter must wake.
    release(2, true);

    const auto deadline = std::chrono::steady_clock::now() + kDeadline;
    const bool waiterDone = waiter.wait_until(deadline) == std::future_status::ready;
    const bool lastDone = last.wait_until(deadline) == std::future_status::ready;
    EXPECT_TRUE(waiterDone) << "the waiter missed the last open failing";
    EXPECT_TRUE(lastDone);
    if (!waiterDone || !lastDone) Seam::wakeWaiters(reader);  // let a stuck waiter finish
    EXPECT_NE(waiter.get().find("injected open failure"), std::string::npos);
    EXPECT_NE(last.get().find("injected open failure"), std::string::npos);
    EXPECT_EQ(Seam::openCount(reader), 0);
}

#ifndef _WIN32
// A file unlinked after sourceFor stays readable through the handle the source holds;
// further handles cannot open, so concurrent fills wait for that one, and every fill
// returns the file's pixels (ruling S50).
TEST_F(RasterSourceTest, UnlinkedFileServesConcurrentFillsThroughThePool) {
    const RasterId g = writeGradient();
    TileRasterSourcePtr s = store().rasters().sourceFor(g);
    ASSERT_TRUE(std::filesystem::remove(onlyTif()));

    constexpr int kThreads = 8;
    constexpr int kViews = 16;
    std::vector<std::future<std::string>> results;
    for (int t = 0; t < kThreads; ++t) {
        results.push_back(std::async(std::launch::async, [&, t]() -> std::string {
            try {
                for (int i = 0; i < kViews; ++i) {
                    // 64 x 64 level-1 pixels (scale 2), read from page 0.
                    const int64_t x0 = (t * 211 + i * 97) % (kSide - 64);
                    const int64_t y0 = (t * 113 + i * 59) % (kSide - 64);
                    const auto view = fill(*s, Rect64{x0 * 2, y0 * 2, 128, 128}, 64, 64);
                    for (int64_t y = 0; y < 64; ++y)
                        for (int64_t x = 0; x < 64; ++x)
                            if (at<int32_t>(*view, 0, x, y) != gradient(x0 + x, y0 + y))
                                return "thread " + std::to_string(t) + ": wrong sample";
                }
                return std::string();
            } catch (const std::exception& e) {
                return "thread " + std::to_string(t) + ": " + e.what();
            }
        }));
    }
    const auto deadline = std::chrono::steady_clock::now() + kDeadline * 3;
    for (std::future<std::string>& result : results) {
        if (result.wait_until(deadline) != std::future_status::ready) {
            std::fprintf(stderr, "UnlinkedFileServesConcurrentFillsThroughThePool: a fill hung\n");
            std::abort();  // a hung fill cannot be joined
        }
        EXPECT_EQ(result.get(), "");
    }
}
#endif
