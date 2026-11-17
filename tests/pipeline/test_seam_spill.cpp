// Requirements: UR-EXE-2, UR-SEG-3
#include <slideio/processor/core/control.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/core/ids.hpp>
#include <slideio/processor/core/measurement.hpp>
#include <slideio/processor/core/memory_budget.hpp>
#include <slideio/processor/core/object_record.hpp>
#include <slideio/processor/pipeline/operation.hpp>
#include <slideio/processor/pipeline/seam_merger.hpp>
#include <slideio/processor/pipeline/tile_grid.hpp>

#include <pipeline/test_support/synthetic_scene.hpp>
#include <pipeline/test_support/test_operations.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace slideio::processor;
namespace ts = slideio::processor::testing_support;
namespace fs = std::filesystem;

namespace {

const LayerId kLayer{3};

Polygon polygonOf(const std::vector<std::vector<Point64>>& rings) {
    Polygon polygon;
    for (size_t i = 0; i < rings.size(); ++i) {
        polygon.addRing(rings[i].data(), static_cast<int64_t>(rings[i].size()), i != 0);
    }
    return polygon;
}

Polygon rectangle(int64_t x, int64_t y, int64_t w, int64_t h) {
    return polygonOf({{{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}}});
}

TileGrid gridOf(int64_t columns, int64_t rows, int32_t tileWidth, int32_t tileHeight) {
    const int64_t width = columns * tileWidth;
    const int64_t height = rows * tileHeight;
    const std::shared_ptr<slideio::Scene> scene =
        ts::makeSyntheticScene({{width, height, 0, 0}}, std::vector<slideio::DataType>{slideio::DataType::DT_Byte},
                               [](int, int64_t, int64_t) { return 0.0; });
    return TileGrid(Rect64{0, 0, width, height}, 0, tileWidth, tileHeight, LevelSelector(scene));
}

// A fresh, empty directory under the temporary directory, removed with the object.
class ScratchDirectory {
public:
    ScratchDirectory() {
        std::random_device device;
        path_ = fs::temp_directory_path() / ("seam-spill-test-" + std::to_string(device()) + "-" +
                                             std::to_string(device()));
        fs::create_directories(path_);
    }
    ~ScratchDirectory() {
        std::error_code error;
        fs::remove_all(path_, error);
    }
    ScratchDirectory(const ScratchDirectory&) = delete;
    ScratchDirectory& operator=(const ScratchDirectory&) = delete;

    const fs::path& path() const { return path_; }
    std::string utf8() const { return path_.u8string(); }
    std::vector<std::string> entries() const {
        std::vector<std::string> names;
        std::error_code error;
        for (const auto& entry : fs::directory_iterator(path_, error)) {
            names.push_back(entry.path().filename().u8string());
        }
        return names;
    }

private:
    fs::path path_;
};

IntensityPartial partial(int64_t count, double sum, double sumOfSquares, double minimum, double maximum,
                         int32_t channel = 0) {
    return IntensityPartial{count, sum, sumOfSquares, minimum, maximum, channel};
}

struct Fragment {
    int64_t tile;
    Polygon polygon;
    std::vector<IntensityPartial> partials;
};

void add(SeamMerger& merger, const Fragment& f) {
    merger.addFragment(f.tile, f.polygon, kLayer, f.partials.empty() ? nullptr : f.partials.data(),
                       static_cast<int32_t>(f.partials.size()));
}

// The bytes the merger reserves for a resident fragment: 16 per ring point, one
// IntensityPartial per partial, and 64 more.
int64_t residentBytes(const Fragment& f) {
    int64_t points = 0;
    for (int32_t ring = 0; ring < f.polygon.ringCount(); ++ring) {
        points += f.polygon.ringPointCount(ring);
    }
    return points * 16 + static_cast<int64_t>(f.partials.size() * sizeof(IntensityPartial)) + 64;
}

// The fragments of six objects on a 3x3 grid of 10x10 tiles, with partials on two
// channels so the spilled partials have to come back intact.
std::vector<Fragment> sceneFragments() {
    std::vector<Fragment> f;
    f.push_back({6, rectangle(2, 24, 8, 3), {partial(24, 240.5, 2400.25, 10.0, 11.0), partial(24, 1.0, 1.0, 0.0, 1.0, 1)}});
    f.push_back({7, rectangle(10, 24, 2, 3), {partial(6, 120.0, 2400.0, 20.0, 20.0), partial(6, 3.0, 9.0, 0.5, 2.0, 1)}});

    f.push_back({4, rectangle(15, 15, 5, 5), {partial(25, 25.0, 25.0, 1.0, 1.0)}});
    f.push_back({5, rectangle(20, 15, 5, 5), {partial(25, 50.0, 100.0, 2.0, 2.0)}});
    f.push_back({7, rectangle(15, 20, 5, 5), {partial(25, 75.0, 225.0, 3.0, 3.0)}});
    f.push_back({8, rectangle(20, 20, 5, 5), {partial(25, 100.0, 400.0, 4.0, 4.0)}});

    f.push_back({0, polygonOf({{{4, 4}, {10, 4}, {10, 8}, {8, 8}, {8, 10}, {4, 10}}}), {}});
    f.push_back({1, polygonOf({{{10, 4}, {16, 4}, {16, 10}, {12, 10}, {12, 8}, {10, 8}}}), {}});
    f.push_back({3, polygonOf({{{4, 10}, {8, 10}, {8, 12}, {10, 12}, {10, 16}, {4, 16}}}), {}});
    f.push_back({4, polygonOf({{{12, 10}, {16, 10}, {16, 16}, {10, 16}, {10, 12}, {12, 12}}}), {}});

    f.push_back({2, rectangle(20, 1, 3, 2), {partial(6, 6.0, 6.0, 1.0, 1.0)}});

    f.push_back({2, rectangle(20, 7, 2, 3), {}});
    f.push_back({4, rectangle(17, 10, 3, 3), {}});
    return f;
}

// An object as compared across runs: id, rings with hole flags, measurements bit for bit.
struct Emitted {
    int64_t anchor = 0;
    std::vector<std::vector<Point64>> rings;
    std::vector<bool> holes;
    std::vector<std::pair<std::string, double>> measurements;

    bool operator<(const Emitted& other) const { return anchor < other.anchor; }
    bool operator==(const Emitted& other) const {
        if (anchor != other.anchor || holes != other.holes || rings.size() != other.rings.size() ||
            measurements.size() != other.measurements.size()) {
            return false;
        }
        for (size_t i = 0; i < rings.size(); ++i) {
            if (rings[i].size() != other.rings[i].size()) {
                return false;
            }
            for (size_t j = 0; j < rings[i].size(); ++j) {
                if (!(rings[i][j] == other.rings[i][j])) {
                    return false;
                }
            }
        }
        for (size_t i = 0; i < measurements.size(); ++i) {
            if (measurements[i].first != other.measurements[i].first ||
                std::memcmp(&measurements[i].second, &other.measurements[i].second, sizeof(double)) != 0) {
                return false;
            }
        }
        return true;
    }
};

std::vector<Emitted> summarise(const std::vector<ObjectRecord>& records) {
    std::vector<Emitted> out;
    for (const ObjectRecord& r : records) {
        Emitted e;
        e.anchor = r.id().anchorPacked();
        for (int32_t ring = 0; ring < r.geometry().ringCount(); ++ring) {
            std::vector<Point64> points(static_cast<size_t>(r.geometry().ringPointCount(ring)));
            r.geometry().ringPoints(ring, points.data(), static_cast<int64_t>(points.size()));
            e.rings.push_back(std::move(points));
            e.holes.push_back(r.geometry().ringIsHole(ring));
        }
        const MeasurementSet& ms = r.measurements();
        for (int32_t i = 0; i < ms.count(); ++i) {
            const Measurement m = ms.at(i);
            e.measurements.emplace_back(m.name(), m.value());
        }
        std::sort(e.measurements.begin(), e.measurements.end());
        out.push_back(std::move(e));
    }
    std::sort(out.begin(), out.end());
    return out;
}

// Adds each tile's fragments, then completes it, tile by tile in index order.
std::vector<Emitted> runSequential(SeamMerger& merger, const TileGrid& grid, const std::vector<Fragment>& fragments) {
    ts::CollectingObjectSink sink;
    for (int64_t tile = 0; tile < grid.tileCount(); ++tile) {
        for (const Fragment& f : fragments) {
            if (f.tile == tile) {
                add(merger, f);
            }
        }
        merger.notifyTileComplete(tile, sink);
    }
    merger.resolve(sink, nullptr, CancellationToken());
    return summarise(sink.records());
}

class ThrowingSink : public ObjectSink {
public:
    void emit(const ObjectRecord&) override { throw IoError("sink failed"); }
    void flush() override {}
    int64_t emittedCount() const override { return 0; }
};

} // namespace

TEST(SeamSpill, SmallBudgetSpillsAndStillMergesIdentically) {
    const TileGrid grid = gridOf(3, 3, 10, 10);
    const std::vector<Fragment> fragments = sceneFragments();

    ScratchDirectory unboundedScratch;
    SeamMerger unbounded(grid, makeMemoryBudget(int64_t{1} << 30), unboundedScratch.utf8().c_str());
    const std::vector<Emitted> reference = runSequential(unbounded, grid, fragments);
    ASSERT_EQ(reference.size(), 6u);
    EXPECT_EQ(unbounded.spilledFragmentCount(), 0);
    EXPECT_EQ(unbounded.spilledBytes(), 0);

    // Room for one small fragment at a time; the hexagons with partials do not fit
    // at all, so both the spill of older components and the spill of a fragment
    // the budget can never hold are exercised.
    ScratchDirectory scratch;
    SeamMerger bounded(grid, makeMemoryBudget(200), scratch.utf8().c_str());
    const std::vector<Emitted> emitted = runSequential(bounded, grid, fragments);
    EXPECT_GT(bounded.spilledFragmentCount(), 0);
    EXPECT_GT(bounded.spilledBytes(), 0);
    EXPECT_EQ(bounded.fragmentCount(), static_cast<int64_t>(fragments.size()));
    EXPECT_EQ(bounded.residentFragmentCount(), 0);
    ASSERT_EQ(emitted.size(), reference.size());
    for (size_t i = 0; i < emitted.size(); ++i) {
        EXPECT_TRUE(emitted[i] == reference[i]) << "object " << i;
    }
}

TEST(SeamSpill, ConcurrentWorkersWithSmallBudgetMergeIdentically) {
    const TileGrid grid = gridOf(3, 3, 10, 10);
    const std::vector<Fragment> fragments = sceneFragments();
    ScratchDirectory referenceScratch;
    SeamMerger unbounded(grid, makeMemoryBudget(int64_t{1} << 30), referenceScratch.utf8().c_str());
    const std::vector<Emitted> reference = runSequential(unbounded, grid, fragments);

    for (int trial = 0; trial < 30; ++trial) {
        ScratchDirectory scratch;
        const MemoryBudgetPtr budget = makeMemoryBudget(300);
        ts::CollectingObjectSink sink;
        {
            SeamMerger merger(grid, budget, scratch.utf8().c_str());
            std::vector<std::thread> workers;
            for (int64_t tile = 0; tile < grid.tileCount(); ++tile) {
                workers.emplace_back([&, tile] {
                    for (const Fragment& f : fragments) {
                        if (f.tile == tile) {
                            add(merger, f);
                        }
                    }
                    merger.notifyTileComplete(tile, sink);
                });
            }
            for (std::thread& worker : workers) {
                worker.join();
            }
            merger.resolve(sink, nullptr, CancellationToken());
            EXPECT_EQ(merger.residentFragmentCount(), 0);
            // Tile 4 alone holds more than the budget, so something is always written out.
            EXPECT_GT(merger.spilledFragmentCount(), 0) << "trial " << trial;
        }
        EXPECT_EQ(budget->reservedBytes(), 0) << "trial " << trial;
        EXPECT_TRUE(scratch.entries().empty()) << "trial " << trial;
        const std::vector<Emitted> emitted = summarise(sink.records());
        ASSERT_EQ(emitted.size(), reference.size()) << "trial " << trial;
        for (size_t i = 0; i < emitted.size(); ++i) {
            EXPECT_TRUE(emitted[i] == reference[i]) << "object " << i << " trial " << trial;
        }
    }
}

TEST(SeamSpill, OldestComponentsSpillFirst) {
    const TileGrid grid = gridOf(3, 3, 10, 10);
    // Two open objects of different sizes, each waiting for the tile across its seam.
    const Fragment square{0, rectangle(5, 2, 5, 4), {}};                                          // 4 points
    const Fragment hexagon{3, polygonOf({{{4, 12}, {10, 12}, {10, 16}, {8, 16}, {8, 18}, {4, 18}}}), {}}; // 6 points
    const Fragment late{6, rectangle(2, 24, 8, 3), {}};
    // The bytes each takes in the scratch file: ring count, per ring a hole flag,
    // a point count and the points, then a partial count.
    const int64_t squareFile = 4 + 1 + 8 + 4 * 16 + 4;
    const int64_t hexagonFile = 4 + 1 + 8 + 6 * 16 + 4;

    for (const bool squareFirst : {true, false}) {
        ScratchDirectory scratch;
        const MemoryBudgetPtr budget = makeMemoryBudget(residentBytes(square) + residentBytes(hexagon));
        SeamMerger merger(grid, budget, scratch.utf8().c_str());
        add(merger, squareFirst ? square : hexagon);
        add(merger, squareFirst ? hexagon : square);
        EXPECT_EQ(merger.spilledFragmentCount(), 0);
        EXPECT_EQ(budget->availableBytes(), 0);

        // The budget is full: the third fragment pushes out the oldest object only.
        add(merger, late);
        EXPECT_EQ(merger.spilledFragmentCount(), 1);
        EXPECT_EQ(merger.spilledBytes(), squareFirst ? squareFile : hexagonFile);
        EXPECT_EQ(merger.residentFragmentCount(), 2);
        ASSERT_EQ(scratch.entries().size(), 1u);
        const std::string name = scratch.entries().front();
        ASSERT_EQ(name.size(), std::string("seam-.bin").size() + 32) << name;
        EXPECT_EQ(name.substr(0, 5), "seam-");
        EXPECT_EQ(name.substr(37), ".bin");
        EXPECT_TRUE(std::all_of(name.begin() + 5, name.begin() + 37,
                                [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
            << name;
        EXPECT_EQ(static_cast<int64_t>(fs::file_size(scratch.path() / name)), merger.spilledBytes());
    }
}

TEST(SeamSpill, FilesRemovedOnDestructionAndOnFailure) {
    const TileGrid grid = gridOf(3, 3, 10, 10);
    const std::vector<Fragment> fragments = sceneFragments();
    {
        ScratchDirectory scratch;
        {
            SeamMerger merger(grid, makeMemoryBudget(200), scratch.utf8().c_str());
            for (const Fragment& f : fragments) {
                add(merger, f);
            }
            ASSERT_GT(merger.spilledFragmentCount(), 0);
            EXPECT_EQ(scratch.entries().size(), 1u);
            ts::CollectingObjectSink sink;
            for (int64_t tile = 0; tile < grid.tileCount(); ++tile) {
                merger.notifyTileComplete(tile, sink);
            }
            merger.resolve(sink, nullptr, CancellationToken());
            EXPECT_EQ(sink.emittedCount(), 6);
        }
        EXPECT_TRUE(scratch.entries().empty());
    }
    {
        ScratchDirectory scratch;
        const MemoryBudgetPtr budget = makeMemoryBudget(200);
        {
            SeamMerger merger(grid, budget, scratch.utf8().c_str());
            for (const Fragment& f : fragments) {
                add(merger, f);
            }
            ASSERT_GT(merger.spilledFragmentCount(), 0);
            ThrowingSink sink;
            bool threw = false;
            for (int64_t tile = 0; tile < grid.tileCount() && !threw; ++tile) {
                try {
                    merger.notifyTileComplete(tile, sink);
                } catch (const IoError&) {
                    threw = true;
                }
            }
            EXPECT_TRUE(threw);
            EXPECT_EQ(scratch.entries().size(), 1u);
        }
        EXPECT_TRUE(scratch.entries().empty());
        EXPECT_EQ(budget->reservedBytes(), 0);
    }
}

TEST(SeamSpill, UnwritableScratchIsIoError) {
    const TileGrid grid = gridOf(3, 3, 10, 10);
    ScratchDirectory parent;
    const fs::path where = parent.path() / "scratch";
    fs::create_directories(where);
    const MemoryBudgetPtr budget = makeMemoryBudget(150);
    SeamMerger merger(grid, budget, where.u8string().c_str());
    // The scratch directory is replaced by a file after construction, so the
    // spill file cannot be created in it.
    fs::remove(where);
    { std::ofstream(where) << "not a directory"; }

    add(merger, Fragment{0, rectangle(5, 2, 5, 4), {}});
    try {
        add(merger, Fragment{3, rectangle(5, 22, 5, 4), {}});
        FAIL() << "expected IoError";
    } catch (const IoError& e) {
        const std::string message = e.what();
        EXPECT_NE(message.find(where.u8string()), std::string::npos) << message;
        EXPECT_NE(message.find("seam-"), std::string::npos) << message;
    }
    EXPECT_EQ(merger.spilledFragmentCount(), 0);
}

TEST(SeamSpill, BudgetReleasedAtClose) {
    const TileGrid grid = gridOf(3, 3, 10, 10);
    const std::vector<Fragment> fragments = sceneFragments();
    ScratchDirectory scratch;
    const MemoryBudgetPtr budget = makeMemoryBudget(int64_t{1} << 30);
    ASSERT_TRUE(budget->tryReserve(1000)); // Someone else's reservation, untouched by the merger.
    const int64_t start = budget->reservedBytes();

    SeamMerger merger(grid, budget, scratch.utf8().c_str());
    int64_t expected = 0;
    for (const Fragment& f : fragments) {
        add(merger, f);
        expected += residentBytes(f);
    }
    EXPECT_EQ(budget->reservedBytes(), start + expected);
    EXPECT_EQ(merger.spilledFragmentCount(), 0);

    ts::CollectingObjectSink sink;
    // Tiles 6 and 7 close the bar alone; its two fragments' bytes come back.
    merger.notifyTileComplete(6, sink);
    merger.notifyTileComplete(7, sink);
    ASSERT_EQ(sink.emittedCount(), 1);
    expected -= residentBytes(fragments[0]) + residentBytes(fragments[1]);
    EXPECT_EQ(budget->reservedBytes(), start + expected);

    for (int64_t tile = 0; tile < grid.tileCount(); ++tile) {
        merger.notifyTileComplete(tile, sink);
    }
    merger.resolve(sink, nullptr, CancellationToken());
    EXPECT_EQ(sink.emittedCount(), 6);
    EXPECT_EQ(budget->reservedBytes(), start);
}

TEST(SeamSpill, DestructionReleasesWhatIsStillHeld) {
    const TileGrid grid = gridOf(3, 3, 10, 10);
    ScratchDirectory scratch;
    const MemoryBudgetPtr budget = makeMemoryBudget(int64_t{1} << 30);
    {
        SeamMerger merger(grid, budget, scratch.utf8().c_str());
        for (const Fragment& f : sceneFragments()) {
            add(merger, f);
        }
        EXPECT_GT(budget->reservedBytes(), 0);
    }
    EXPECT_EQ(budget->reservedBytes(), 0);
}

TEST(SeamSpill, WaiterWhoseFragmentIsSpilledReturns) {
    // The budget is held by someone else (a worker's tile buffer, say) that gives
    // it back only once A's addFragment() has returned. A waits for memory; B's
    // fragment then writes A's pending fragment out, which frees nothing but means
    // A no longer needs memory: A must return rather than wait for ever.
    const TileGrid grid = gridOf(3, 3, 10, 10);
    ScratchDirectory scratch;
    const MemoryBudgetPtr budget = makeMemoryBudget(1000);
    ASSERT_TRUE(budget->tryReserve(1000));
    SeamMerger merger(grid, budget, scratch.utf8().c_str());

    std::atomic<bool> aReturned{false};
    std::thread a([&] {
        add(merger, Fragment{0, rectangle(5, 2, 5, 4), {}});
        aReturned = true;
    });
    // B starts once A's fragment is recorded (A then waits for the budget).
    while (merger.fragmentCount() < 1) {
        std::this_thread::yield();
    }
    std::thread b([&] { add(merger, Fragment{3, rectangle(5, 22, 5, 4), {}}); });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!aReturned && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const bool returnedInTime = aReturned;
    budget->release(1000); // Unblocks every wait either way, so the test cannot hang.
    a.join();
    b.join();
    ASSERT_TRUE(returnedInTime) << "a waiter whose fragment was written out stayed blocked";
    EXPECT_GE(merger.spilledFragmentCount(), 1);

    ts::CollectingObjectSink sink;
    for (int64_t tile = 0; tile < grid.tileCount(); ++tile) {
        merger.notifyTileComplete(tile, sink);
    }
    merger.resolve(sink, nullptr, CancellationToken());
    EXPECT_EQ(sink.emittedCount(), 2);
    EXPECT_EQ(budget->reservedBytes(), 0);
}
