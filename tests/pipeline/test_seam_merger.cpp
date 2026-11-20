// Requirements: UR-SEG-3, UR-QA-2, UR-QA-3
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

#include <pipeline/detail/seam_merger_access.hpp>
#include <pipeline/test_support/synthetic_scene.hpp>
#include <pipeline/test_support/test_operations.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

using namespace slideio::processor;
namespace ts = slideio::processor::testing_support;

namespace {

const LayerId kLayer{7};

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

std::vector<Point64> ringOf(const Polygon& polygon, int32_t ring) {
    std::vector<Point64> points(static_cast<size_t>(polygon.ringPointCount(ring)));
    polygon.ringPoints(ring, points.data(), static_cast<int64_t>(points.size()));
    return points;
}

// A grid of `columns` x `rows` tiles of `tileWidth` x `tileHeight` at level 0.
TileGrid gridOf(int64_t columns, int64_t rows, int32_t tileWidth, int32_t tileHeight) {
    const int64_t width = columns * tileWidth;
    const int64_t height = rows * tileHeight;
    const std::shared_ptr<slideio::Scene> scene =
        ts::makeSyntheticScene({{width, height, 0, 0}}, std::vector<slideio::DataType>{slideio::DataType::DT_Byte},
                               [](int, int64_t, int64_t) { return 0.0; });
    return TileGrid(Rect64{0, 0, width, height}, 0, tileWidth, tileHeight, LevelSelector(scene));
}

std::string scratch() { return std::filesystem::temp_directory_path().u8string(); }

std::unique_ptr<SeamMerger> mergerFor(const TileGrid& grid) {
    return std::make_unique<SeamMerger>(grid, makeMemoryBudget(int64_t{1} << 30), scratch().c_str());
}

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

double measurement(const ObjectRecord& record, const char* name) {
    Measurement m;
    EXPECT_TRUE(record.measurements().find(name, m)) << name;
    return m.value();
}

// The fragments of five objects on a 3x3 grid of 10x10 tiles:
// - a bar across tiles 6|7;
// - a square cut into quarters around the corner (20,20), tiles 4, 5, 7, 8;
// - an annulus around the corner (10,10), tiles 0, 1, 3, 4: one object with a hole;
// - a block in tile 2 lying on the seam with tile 1, where tile 1 holds nothing;
// - two blocks in tiles 2 and 4 touching only at the corner (20,10): two objects.
std::vector<Fragment> sceneFragments() {
    std::vector<Fragment> f;
    f.push_back({6, rectangle(2, 24, 8, 3), {partial(24, 240.0, 2400.0, 10.0, 10.0)}});
    f.push_back({7, rectangle(10, 24, 2, 3), {partial(6, 120.0, 2400.0, 20.0, 20.0)}});

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

// What identifies an emitted object for comparison: id, rings, measurements.
struct Emitted {
    int64_t anchor;
    std::vector<std::vector<Point64>> rings;
    std::vector<bool> holes;
    std::vector<std::pair<std::string, double>> measurements;

    bool operator<(const Emitted& other) const { return anchor < other.anchor; }
};

std::vector<Emitted> summarise(const std::vector<ObjectRecord>& records) {
    std::vector<Emitted> out;
    for (const ObjectRecord& r : records) {
        Emitted e;
        e.anchor = r.id().anchorPacked();
        for (int32_t ring = 0; ring < r.geometry().ringCount(); ++ring) {
            e.rings.push_back(ringOf(r.geometry(), ring));
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

bool samePoints(const std::vector<std::vector<Point64>>& a, const std::vector<std::vector<Point64>>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].size() != b[i].size()) {
            return false;
        }
        for (size_t j = 0; j < a[i].size(); ++j) {
            if (a[i][j].x != b[i][j].x || a[i][j].y != b[i][j].y) {
                return false;
            }
        }
    }
    return true;
}

bool bitwiseEqual(double a, double b) { return std::memcmp(&a, &b, sizeof(double)) == 0; }

} // namespace

TEST(SeamMerger, ClosesWhenItsNeighbourhoodCompletes) {
    const TileGrid grid = gridOf(3, 3, 10, 10);
    auto merger = mergerFor(grid);
    ts::CollectingObjectSink sink;
    merger->addFragment(0, rectangle(5, 2, 5, 4), kLayer, nullptr, 0);
    merger->addFragment(1, rectangle(10, 2, 5, 4), kLayer, nullptr, 0);

    merger->notifyTileComplete(0, sink);
    EXPECT_EQ(sink.emittedCount(), 0);
    merger->notifyTileComplete(1, sink);
    // Tiles 2..8 have not completed: the object's own neighbourhood is enough.
    ASSERT_EQ(sink.emittedCount(), 1);

    const ObjectRecord record = sink.records().front();
    EXPECT_EQ(record.layer(), kLayer);
    EXPECT_EQ(record.id(), ObjectId::fromAnchor(kLayer, Point64{5, 2}));
    ASSERT_EQ(record.geometry().ringCount(), 1);
    EXPECT_TRUE(samePoints({ringOf(record.geometry(), 0)}, {{{5, 2}, {15, 2}, {15, 6}, {5, 6}}}));
    EXPECT_EQ(measurement(record, "area"), 40.0);
    EXPECT_EQ(measurement(record, "perimeter"), 28.0);
    EXPECT_EQ(merger->residentFragmentCount(), 0);
}

TEST(SeamMerger, CompletionOrderDoesNotChangeTheResult) {
    const TileGrid grid = gridOf(3, 3, 10, 10);
    const std::vector<Fragment> fragments = sceneFragments();
    std::mt19937 random(20261116u);
    std::vector<Emitted> reference;
    for (int trial = 0; trial < 100; ++trial) {
        auto merger = mergerFor(grid);
        ts::CollectingObjectSink sink;
        std::vector<size_t> addOrder(fragments.size());
        std::iota(addOrder.begin(), addOrder.end(), size_t{0});
        std::shuffle(addOrder.begin(), addOrder.end(), random);
        std::vector<int64_t> tiles(static_cast<size_t>(grid.tileCount()));
        std::iota(tiles.begin(), tiles.end(), int64_t{0});
        std::shuffle(tiles.begin(), tiles.end(), random);
        // On odd trials some tiles are skipped, as a restricted run skips them,
        // and evaluated only when an open object awaits them. Tiles 2 and 4 hold
        // objects of their own, so they are never skipped, and one of the bar's
        // tiles 6 and 7 is always evaluated: every object is then reached.
        std::vector<char> skipped(static_cast<size_t>(grid.tileCount()), 0);
        if (trial % 2 == 1) {
            for (int64_t tile : {0, 1, 3, 5, 6, 7, 8}) {
                skipped[static_cast<size_t>(tile)] = static_cast<char>(random() % 2);
            }
            skipped[static_cast<size_t>(6 + random() % 2)] = 0;
        }
        // A tile is notified only after its own fragments have arrived, as the executor does.
        const auto evaluate = [&](int64_t tile) {
            for (size_t i : addOrder) {
                if (fragments[i].tile == tile) {
                    add(*merger, fragments[i]);
                }
            }
            merger->notifyTileComplete(tile, sink);
        };
        for (int64_t tile : tiles) {
            if (skipped[static_cast<size_t>(tile)] != 0) {
                detail::SeamMergerAccess::notifyTileSkipped(*merger, tile);
            } else {
                evaluate(tile);
            }
        }
        for (std::vector<int64_t> awaited = detail::SeamMergerAccess::takeAwaitedDeferred(*merger);
             !awaited.empty(); awaited = detail::SeamMergerAccess::takeAwaitedDeferred(*merger)) {
            EXPECT_TRUE(std::is_sorted(awaited.begin(), awaited.end()));
            for (int64_t tile : awaited) {
                EXPECT_NE(skipped[static_cast<size_t>(tile)], 0) << "trial " << trial;
                evaluate(tile);
            }
        }
        detail::SeamMergerAccess::completeDeferred(*merger, sink);
        merger->resolve(sink, nullptr, CancellationToken());

        const std::vector<Emitted> emitted = summarise(sink.records());
        ASSERT_EQ(emitted.size(), 6u) << "trial " << trial;
        if (trial == 0) {
            reference = emitted;
            continue;
        }
        for (size_t i = 0; i < emitted.size(); ++i) {
            EXPECT_EQ(emitted[i].anchor, reference[i].anchor) << "trial " << trial;
            EXPECT_TRUE(samePoints(emitted[i].rings, reference[i].rings)) << "trial " << trial;
            EXPECT_EQ(emitted[i].holes, reference[i].holes) << "trial " << trial;
            ASSERT_EQ(emitted[i].measurements.size(), reference[i].measurements.size()) << "trial " << trial;
            for (size_t m = 0; m < emitted[i].measurements.size(); ++m) {
                EXPECT_EQ(emitted[i].measurements[m].first, reference[i].measurements[m].first);
                EXPECT_TRUE(bitwiseEqual(emitted[i].measurements[m].second, reference[i].measurements[m].second))
                    << emitted[i].measurements[m].first << " trial " << trial;
            }
        }
    }

    // The annulus is one object with one hole; the corner-touching blocks stay two.
    const auto annulus = std::find_if(reference.begin(), reference.end(), [](const Emitted& e) {
        return e.anchor == ObjectId::fromAnchor(kLayer, Point64{4, 4}).anchorPacked();
    });
    ASSERT_NE(annulus, reference.end());
    ASSERT_EQ(annulus->rings.size(), 2u);
    EXPECT_FALSE(annulus->holes[0]);
    EXPECT_TRUE(annulus->holes[1]);
    EXPECT_TRUE(samePoints(annulus->rings, {{{4, 4}, {16, 4}, {16, 16}, {4, 16}}, {{8, 8}, {8, 12}, {12, 12}, {12, 8}}}));
    for (const Point64 anchor : {Point64{20, 7}, Point64{17, 10}, Point64{20, 1}, Point64{2, 24}, Point64{15, 15}}) {
        EXPECT_TRUE(std::any_of(reference.begin(), reference.end(), [&](const Emitted& e) {
            return e.anchor == ObjectId::fromAnchor(kLayer, anchor).anchorPacked();
        })) << anchor.x << "," << anchor.y;
    }
}

TEST(SeamMerger, PartialsCombineInCanonicalOrder) {
    // Sums whose float total depends on the order they are added in. In canonical
    // order (fragment anchor y, then x) they are 1e17, 1, -1e17, 1, which sum to 1:
    // 1e17 + 1 rounds back to 1e17. An order cancelling the large sums first gives 2.
    const TileGrid grid = gridOf(3, 3, 10, 10);
    const std::vector<Fragment> quarters = {
        {4, rectangle(15, 15, 5, 5), {partial(1, 1e17, 0.0, 0.0, 0.0)}},
        {5, rectangle(20, 15, 5, 5), {partial(1, 1.0, 0.0, 0.0, 0.0)}},
        {7, rectangle(15, 20, 5, 5), {partial(1, -1e17, 0.0, 0.0, 0.0)}},
        {8, rectangle(20, 20, 5, 5), {partial(1, 1.0, 0.0, 0.0, 0.0)}},
    };
    std::vector<size_t> order = {0, 1, 2, 3};
    std::vector<int64_t> tiles = {4, 5, 7, 8};
    int trials = 0;
    do {
        for (int rotation = 0; rotation < 4; ++rotation) {
            auto merger = mergerFor(grid);
            ts::CollectingObjectSink sink;
            for (size_t i : order) {
                add(*merger, quarters[i]);
            }
            for (int64_t tile : tiles) {
                merger->notifyTileComplete(tile, sink);
            }
            ASSERT_EQ(sink.emittedCount(), 1);
            const double mean = measurement(sink.records().front(), "intensity.0.mean");
            EXPECT_TRUE(bitwiseEqual(mean, 0.25)) << mean;
            std::rotate(tiles.begin(), tiles.begin() + 1, tiles.end());
            ++trials;
        }
    } while (std::next_permutation(order.begin(), order.end()));
    EXPECT_EQ(trials, 96);
}

TEST(SeamMerger, FilterDropsByAnchor) {
    const TileGrid grid = gridOf(3, 3, 10, 10);
    auto merger = mergerFor(grid);
    std::vector<Point64> asked;
    detail::SeamMergerAccess::setFilter(*merger, [&asked](Point64 anchor) {
        asked.push_back(anchor);
        return !(anchor.x == 5 && anchor.y == 2);
    });
    ts::CollectingObjectSink sink;
    merger->addFragment(0, rectangle(5, 2, 5, 4), kLayer, nullptr, 0);
    merger->addFragment(1, rectangle(10, 2, 5, 4), kLayer, nullptr, 0);
    merger->addFragment(3, rectangle(3, 12, 7, 2), kLayer, nullptr, 0);
    merger->addFragment(4, rectangle(10, 12, 2, 2), kLayer, nullptr, 0);
    for (int64_t tile = 0; tile < grid.tileCount(); ++tile) {
        merger->notifyTileComplete(tile, sink);
    }
    merger->resolve(sink, nullptr, CancellationToken());

    ASSERT_EQ(sink.emittedCount(), 1);
    EXPECT_EQ(sink.records().front().id(), ObjectId::fromAnchor(kLayer, Point64{3, 12}));
    ASSERT_EQ(asked.size(), 2u);
    EXPECT_EQ(merger->mergedObjectCount(), 2);
}

TEST(SeamMerger, CountersReport) {
    const TileGrid grid = gridOf(3, 3, 10, 10);
    auto merger = mergerFor(grid);
    ts::CollectingObjectSink sink;
    EXPECT_EQ(merger->fragmentCount(), 0);
    EXPECT_EQ(merger->largestComponentFragmentCount(), 0);
    const std::vector<Fragment> fragments = sceneFragments();
    for (const Fragment& f : fragments) {
        add(*merger, f);
    }
    EXPECT_EQ(merger->fragmentCount(), 13);
    EXPECT_EQ(merger->residentFragmentCount(), 13);
    EXPECT_EQ(merger->mergedObjectCount(), 0);

    // Tiles 4, 5, 7 and 8 close the quarters; tile 7 also holds the bar's right
    // half, whose left half in tile 6 is still open.
    for (int64_t tile : {4, 5, 7, 8}) {
        merger->notifyTileComplete(tile, sink);
    }
    EXPECT_EQ(sink.emittedCount(), 1);
    EXPECT_EQ(merger->mergedObjectCount(), 1);
    EXPECT_EQ(merger->residentFragmentCount(), 9);
    EXPECT_EQ(merger->largestComponentFragmentCount(), 4);

    for (int64_t tile : {0, 1, 2, 3, 6}) {
        merger->notifyTileComplete(tile, sink);
    }
    merger->resolve(sink, nullptr, CancellationToken());
    EXPECT_EQ(sink.emittedCount(), 6);
    EXPECT_EQ(merger->mergedObjectCount(), 6);
    EXPECT_EQ(merger->fragmentCount(), 13);
    EXPECT_EQ(merger->residentFragmentCount(), 0);
    EXPECT_EQ(merger->largestComponentFragmentCount(), 4);
    EXPECT_EQ(merger->spilledFragmentCount(), 0);
    EXPECT_EQ(merger->spilledBytes(), 0);
}

TEST(SeamMerger, ConcurrentAddFragment) {
    // Two tiles side by side; object i is a bar across their seam at y = 2i.
    const TileGrid grid = gridOf(2, 1, 1000, 2000);
    auto merger = mergerFor(grid);
    ts::CollectingObjectSink sink;
    constexpr int kObjects = 1000;
    constexpr int kThreads = 8;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&merger, t] {
            for (int i = t; i < kObjects; i += kThreads) {
                const IntensityPartial left = partial(5, 5.0 * i, 5.0 * i * i, i, i);
                const IntensityPartial right = partial(5, 5.0 * i, 5.0 * i * i, i, i);
                merger->addFragment(1, rectangle(1000, 2 * i, 5, 1), kLayer, &right, 1);
                merger->addFragment(0, rectangle(995, 2 * i, 5, 1), kLayer, &left, 1);
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(merger->fragmentCount(), 2 * kObjects);
    std::thread first([&] { merger->notifyTileComplete(0, sink); });
    std::thread second([&] { merger->notifyTileComplete(1, sink); });
    first.join();
    second.join();
    merger->resolve(sink, nullptr, CancellationToken());

    ASSERT_EQ(sink.emittedCount(), kObjects);
    std::vector<int64_t> anchors;
    for (const ObjectRecord& record : sink.records()) {
        anchors.push_back(record.id().anchorPacked());
        EXPECT_EQ(measurement(record, "area"), 10.0);
        const int64_t i = record.geometry().anchor().y / 2;
        EXPECT_EQ(measurement(record, "intensity.0.mean"), static_cast<double>(i));
    }
    std::sort(anchors.begin(), anchors.end());
    EXPECT_EQ(std::unique(anchors.begin(), anchors.end()), anchors.end());
    EXPECT_EQ(merger->mergedObjectCount(), kObjects);
    EXPECT_EQ(merger->residentFragmentCount(), 0);
}

TEST(SeamMerger, ResolveWithOpenComponentIsInternalError) {
    const TileGrid grid = gridOf(3, 3, 10, 10);
    auto merger = mergerFor(grid);
    ts::CollectingObjectSink sink;
    merger->addFragment(0, rectangle(5, 2, 5, 4), kLayer, nullptr, 0);
    merger->notifyTileComplete(0, sink);
    // Tile 1, which the fragment expects, never completes.
    try {
        merger->resolve(sink, nullptr, CancellationToken());
        FAIL() << "resolve accepted an open component";
    } catch (const InternalError& e) {
        const std::string message = e.what();
        EXPECT_NE(message.find("layer 7"), std::string::npos) << message;
    }
    EXPECT_EQ(sink.emittedCount(), 0);
}

TEST(SeamMerger, ResolveHonoursCancellation) {
    const TileGrid grid = gridOf(3, 3, 10, 10);
    auto merger = mergerFor(grid);
    ts::CollectingObjectSink sink;
    CancellationSource source;
    source.cancel();
    EXPECT_THROW(merger->resolve(sink, nullptr, source.token()), CancelledError);
}

TEST(SeamMerger, SkippedTileKeepsItsComponentOpen) {
    const TileGrid grid = gridOf(3, 3, 10, 10);
    auto merger = mergerFor(grid);
    ts::CollectingObjectSink sink;
    merger->addFragment(0, rectangle(5, 2, 5, 4), kLayer, nullptr, 0);
    detail::SeamMergerAccess::notifyTileSkipped(*merger, 1);
    merger->notifyTileComplete(0, sink);
    // Tile 1 is deferred, not complete: the object waits for it.
    EXPECT_EQ(sink.emittedCount(), 0);
    EXPECT_EQ(detail::SeamMergerAccess::takeAwaitedDeferred(*merger), std::vector<int64_t>{1});

    merger->addFragment(1, rectangle(10, 2, 5, 4), kLayer, nullptr, 0);
    merger->notifyTileComplete(1, sink);
    ASSERT_EQ(sink.emittedCount(), 1);
    const ObjectRecord record = sink.records().front();
    EXPECT_EQ(record.id(), ObjectId::fromAnchor(kLayer, Point64{5, 2}));
    EXPECT_TRUE(samePoints({ringOf(record.geometry(), 0)}, {{{5, 2}, {15, 2}, {15, 6}, {5, 6}}}));
    EXPECT_EQ(measurement(record, "area"), 40.0);
}

TEST(SeamMerger, SkippedTileCompletedBeforeTheFragmentIsStillAwaited) {
    const TileGrid grid = gridOf(3, 3, 10, 10);
    auto merger = mergerFor(grid);
    ts::CollectingObjectSink sink;
    // Tile 1 is skipped before the fragment expecting it arrives.
    detail::SeamMergerAccess::notifyTileSkipped(*merger, 1);
    merger->addFragment(0, rectangle(5, 2, 5, 4), kLayer, nullptr, 0);
    merger->notifyTileComplete(0, sink);
    EXPECT_EQ(sink.emittedCount(), 0);
    EXPECT_EQ(detail::SeamMergerAccess::takeAwaitedDeferred(*merger), std::vector<int64_t>{1});

    merger->addFragment(1, rectangle(10, 2, 5, 4), kLayer, nullptr, 0);
    merger->notifyTileComplete(1, sink);
    ASSERT_EQ(sink.emittedCount(), 1);
    EXPECT_EQ(measurement(sink.records().front(), "area"), 40.0);
}

TEST(SeamMerger, AwaitedDeferredIsReturnedOnce) {
    const TileGrid grid = gridOf(3, 3, 10, 10);
    auto merger = mergerFor(grid);
    ts::CollectingObjectSink sink;
    detail::SeamMergerAccess::notifyTileSkipped(*merger, 1);
    detail::SeamMergerAccess::notifyTileSkipped(*merger, 4);
    merger->addFragment(0, rectangle(5, 2, 5, 4), kLayer, nullptr, 0);
    merger->notifyTileComplete(0, sink);
    EXPECT_EQ(detail::SeamMergerAccess::takeAwaitedDeferred(*merger), std::vector<int64_t>{1});
    EXPECT_TRUE(detail::SeamMergerAccess::takeAwaitedDeferred(*merger).empty());

    // Tile 1's fragment reaches on into tile 4, which is then awaited in turn.
    merger->addFragment(1, rectangle(10, 2, 5, 8), kLayer, nullptr, 0);
    merger->notifyTileComplete(1, sink);
    EXPECT_EQ(sink.emittedCount(), 0);
    EXPECT_EQ(detail::SeamMergerAccess::takeAwaitedDeferred(*merger), std::vector<int64_t>{4});
    EXPECT_TRUE(detail::SeamMergerAccess::takeAwaitedDeferred(*merger).empty());

    merger->addFragment(4, rectangle(12, 10, 3, 3), kLayer, nullptr, 0);
    merger->notifyTileComplete(4, sink);
    ASSERT_EQ(sink.emittedCount(), 1);
    EXPECT_EQ(measurement(sink.records().front(), "area"), 69.0);
}

// On an 8-connected layer a fragment touching a tile corner awaits the
// diagonal tile, though no edge of the fragment borders it; on a 4-connected
// layer it does not.
TEST(SeamMerger, DeferredDiagonalTileIsAwaitedAcrossACorner) {
    const TileGrid grid = gridOf(3, 3, 10, 10);
    for (const bool eight : {true, false}) {
        auto merger = mergerFor(grid);
        if (eight) {
            detail::SeamMergerAccess::setEightConnected(*merger, kLayer);
        }
        ts::CollectingObjectSink sink;
        // The square ends at the corner (10,10); tile 4 lies diagonally across it.
        detail::SeamMergerAccess::notifyTileSkipped(*merger, 4);
        merger->addFragment(0, rectangle(5, 5, 5, 5), kLayer, nullptr, 0);
        merger->notifyTileComplete(0, sink);
        merger->notifyTileComplete(1, sink);
        merger->notifyTileComplete(3, sink);
        if (!eight) {
            EXPECT_EQ(sink.emittedCount(), 1) << "4-connected: nothing waits on the corner";
            EXPECT_TRUE(detail::SeamMergerAccess::takeAwaitedDeferred(*merger).empty());
            continue;
        }
        EXPECT_EQ(sink.emittedCount(), 0);
        EXPECT_EQ(detail::SeamMergerAccess::takeAwaitedDeferred(*merger), std::vector<int64_t>{4});
        EXPECT_TRUE(detail::SeamMergerAccess::takeAwaitedDeferred(*merger).empty());

        merger->addFragment(4, rectangle(10, 10, 5, 5), kLayer, nullptr, 0);
        merger->notifyTileComplete(4, sink);
        ASSERT_EQ(sink.emittedCount(), 1);
        EXPECT_EQ(measurement(sink.records().front(), "area"), 50.0);
        detail::SeamMergerAccess::completeDeferred(*merger, sink);
        merger->resolve(sink, nullptr, CancellationToken());
        EXPECT_EQ(sink.emittedCount(), 1);
    }
}

TEST(SeamMerger, CompleteDeferredClosesWhatNothingAwaits) {
    const TileGrid grid = gridOf(3, 3, 10, 10);
    {
        // Every tile but 0 and 1 is deferred and nothing awaits them: they
        // complete without evaluation, emit nothing and leave nothing open.
        auto merger = mergerFor(grid);
        ts::CollectingObjectSink sink;
        for (int64_t tile = 2; tile < grid.tileCount(); ++tile) {
            detail::SeamMergerAccess::notifyTileSkipped(*merger, tile);
        }
        merger->addFragment(0, rectangle(5, 2, 5, 4), kLayer, nullptr, 0);
        merger->addFragment(1, rectangle(10, 2, 5, 4), kLayer, nullptr, 0);
        merger->notifyTileComplete(0, sink);
        merger->notifyTileComplete(1, sink);
        ASSERT_EQ(sink.emittedCount(), 1);
        EXPECT_TRUE(detail::SeamMergerAccess::takeAwaitedDeferred(*merger).empty());
        detail::SeamMergerAccess::completeDeferred(*merger, sink);
        EXPECT_EQ(sink.emittedCount(), 1);
        merger->resolve(sink, nullptr, CancellationToken());
        EXPECT_EQ(sink.emittedCount(), 1);
        EXPECT_EQ(merger->residentFragmentCount(), 0);
    }
    {
        // A deferred tile never taken as awaited is completed all the same, and
        // what it held open closes and is emitted.
        auto merger = mergerFor(grid);
        ts::CollectingObjectSink sink;
        merger->addFragment(0, rectangle(5, 2, 5, 4), kLayer, nullptr, 0);
        detail::SeamMergerAccess::notifyTileSkipped(*merger, 1);
        merger->notifyTileComplete(0, sink);
        EXPECT_EQ(sink.emittedCount(), 0);
        detail::SeamMergerAccess::completeDeferred(*merger, sink);
        ASSERT_EQ(sink.emittedCount(), 1);
        EXPECT_EQ(measurement(sink.records().front(), "area"), 20.0);
        merger->resolve(sink, nullptr, CancellationToken());
        EXPECT_EQ(sink.emittedCount(), 1);
    }
    {
        // A demanded tile is left for its evaluation: until it completes, its
        // object stays open.
        auto merger = mergerFor(grid);
        ts::CollectingObjectSink sink;
        merger->addFragment(0, rectangle(5, 2, 5, 4), kLayer, nullptr, 0);
        detail::SeamMergerAccess::notifyTileSkipped(*merger, 1);
        merger->notifyTileComplete(0, sink);
        EXPECT_EQ(detail::SeamMergerAccess::takeAwaitedDeferred(*merger), std::vector<int64_t>{1});
        detail::SeamMergerAccess::completeDeferred(*merger, sink);
        EXPECT_EQ(sink.emittedCount(), 0);
        EXPECT_THROW(merger->resolve(sink, nullptr, CancellationToken()), InternalError);
    }
}

TEST(SeamMerger, MergeFailureNamesTheLayerNotTheGeometry) {
    // Two identical fragments in tile 1 both join the one in tile 0: they overlap.
    const TileGrid grid = gridOf(3, 3, 10, 10);
    auto merger = mergerFor(grid);
    ts::CollectingObjectSink sink;
    merger->addFragment(0, rectangle(5, 2, 5, 4), kLayer, nullptr, 0);
    merger->addFragment(1, rectangle(10, 2, 5, 4), kLayer, nullptr, 0);
    merger->addFragment(1, rectangle(10, 2, 5, 4), kLayer, nullptr, 0);
    merger->notifyTileComplete(0, sink);
    try {
        merger->notifyTileComplete(1, sink);
        FAIL() << "overlapping fragments were merged";
    } catch (const InternalError& e) {
        const std::string message = e.what();
        EXPECT_NE(message.find("layer 7"), std::string::npos) << message;
        EXPECT_NE(message.find("3 fragments"), std::string::npos) << message;
    }
}

// Two regions of one layer sharing the seam x = 10, emitted with different keys
// (adjacent labels, say): two objects, each its own fragment.
TEST(SeamMerger, DifferentKeysTouchingAcrossSeamStaySeparate) {
    const TileGrid grid = gridOf(3, 3, 10, 10);
    auto merger = mergerFor(grid);
    ts::CollectingObjectSink sink;
    merger->addFragment(0, rectangle(5, 2, 5, 4), kLayer, nullptr, 0, 1);
    merger->addFragment(1, rectangle(10, 2, 5, 4), kLayer, nullptr, 0, 2);
    merger->notifyTileComplete(0, sink);
    merger->notifyTileComplete(1, sink);
    merger->resolve(sink, nullptr, CancellationToken());

    ASSERT_EQ(sink.emittedCount(), 2);
    EXPECT_EQ(merger->mergedObjectCount(), 2);
    EXPECT_EQ(merger->largestComponentFragmentCount(), 1);
    for (const ObjectRecord& record : sink.records()) {
        ASSERT_EQ(record.geometry().ringCount(), 1);
        EXPECT_EQ(measurement(record, "area"), 20.0);
    }
}

TEST(SeamMerger, SameKeyJoins) {
    const TileGrid grid = gridOf(3, 3, 10, 10);
    for (const int64_t key : {int64_t{0}, int64_t{5}, int64_t{-7}, INT64_MAX}) {
        auto merger = mergerFor(grid);
        ts::CollectingObjectSink sink;
        merger->addFragment(0, rectangle(5, 2, 5, 4), kLayer, nullptr, 0, key);
        // A third fragment on the same seam with another key stays out of it.
        merger->addFragment(0, rectangle(5, 6, 5, 1), kLayer, nullptr, 0, key - 1);
        merger->addFragment(1, rectangle(10, 2, 5, 5), kLayer, nullptr, 0, key);
        merger->notifyTileComplete(0, sink);
        merger->notifyTileComplete(1, sink);
        merger->resolve(sink, nullptr, CancellationToken());

        ASSERT_EQ(sink.emittedCount(), 2) << key;
        std::vector<Emitted> emitted = summarise(sink.records());
        // Anchors (5,2) and (5,6): the joined object first.
        EXPECT_TRUE(samePoints(emitted[0].rings, {{{5, 2}, {15, 2}, {15, 7}, {10, 7}, {10, 6}, {5, 6}}}))
            << key;
        EXPECT_TRUE(samePoints(emitted[1].rings, {{{5, 6}, {10, 6}, {10, 7}, {5, 7}}})) << key;
    }
}

// One fragment in tile 0 along the seam x = 20 over rows 2..18 and four in tile
// 1 over rows 2..4, 6..8, 10..12 and 14..18: each of tile 1's contacts finds the
// long one, and the long one finds all four, whichever side arrives first.
TEST(SeamMerger, IndexedContactsJoinInAnyOrder) {
    const TileGrid grid = gridOf(2, 1, 20, 20);
    const Fragment left{0, rectangle(15, 2, 5, 16), {}};
    const std::vector<Fragment> right = {{1, rectangle(20, 2, 3, 2), {}},
                                         {1, rectangle(20, 6, 3, 2), {}},
                                         {1, rectangle(20, 10, 3, 2), {}},
                                         {1, rectangle(20, 14, 3, 4), {}}};
    for (const bool leftFirst : {true, false}) {
        auto merger = mergerFor(grid);
        ts::CollectingObjectSink sink;
        if (leftFirst) {
            add(*merger, left);
        }
        for (const Fragment& f : right) {
            add(*merger, f);
        }
        if (!leftFirst) {
            add(*merger, left);
        }
        merger->notifyTileComplete(0, sink);
        merger->notifyTileComplete(1, sink);
        merger->resolve(sink, nullptr, CancellationToken());

        ASSERT_EQ(sink.emittedCount(), 1) << leftFirst;
        EXPECT_EQ(merger->largestComponentFragmentCount(), 5) << leftFirst;
        EXPECT_EQ(measurement(sink.records().front(), "area"), 80.0 + 3 * 6.0 + 12.0) << leftFirst;
    }
}

// Contacts over rows 2..5 and 5..8 meet at one point only: a 4-connected layer
// keeps them two objects.
TEST(SeamMerger, IndexedContactsJoinOnlyOverlaps) {
    const TileGrid grid = gridOf(2, 1, 20, 20);
    auto merger = mergerFor(grid);
    ts::CollectingObjectSink sink;
    add(*merger, {0, rectangle(15, 2, 5, 3), {}});
    add(*merger, {1, rectangle(20, 5, 3, 3), {}});
    merger->notifyTileComplete(0, sink);
    merger->notifyTileComplete(1, sink);
    merger->resolve(sink, nullptr, CancellationToken());

    ASSERT_EQ(sink.emittedCount(), 2);
    EXPECT_EQ(merger->largestComponentFragmentCount(), 1);
}

// Tile 0's only contact, rows 2..6, overlaps tile 1's, rows 5..9, by one unit:
// with tile 0's first, it begins exactly at the lower edge of the search
// window (5 - 4 + 1). They join in either order.
TEST(SeamMerger, IndexedContactsJoinAtTheWindowEdge) {
    const TileGrid grid = gridOf(2, 1, 20, 20);
    const Fragment left{0, rectangle(15, 2, 5, 4), {}};
    const Fragment right{1, rectangle(20, 5, 3, 4), {}};
    for (const bool leftFirst : {true, false}) {
        auto merger = mergerFor(grid);
        ts::CollectingObjectSink sink;
        add(*merger, leftFirst ? left : right);
        add(*merger, leftFirst ? right : left);
        merger->notifyTileComplete(0, sink);
        merger->notifyTileComplete(1, sink);
        merger->resolve(sink, nullptr, CancellationToken());

        ASSERT_EQ(sink.emittedCount(), 1) << leftFirst;
        EXPECT_EQ(measurement(sink.records().front(), "area"), 20.0 + 12.0) << leftFirst;
    }
}

// Contacts of two layers and two join keys overlap on both sides of the seam
// x = 20: each fragment joins only the partner of its own layer and key.
TEST(SeamMerger, IndexedContactsJoinAcrossLayersAndKeys) {
    const TileGrid grid = gridOf(2, 1, 20, 20);
    const LayerId other{8};
    auto merger = mergerFor(grid);
    ts::CollectingObjectSink sink;
    merger->addFragment(0, rectangle(15, 2, 5, 6), kLayer, nullptr, 0, 0); // Rows 2..8.
    merger->addFragment(0, rectangle(15, 4, 5, 6), other, nullptr, 0, 0);  // Rows 4..10.
    merger->addFragment(0, rectangle(15, 10, 5, 4), kLayer, nullptr, 0, 1); // Rows 10..14.
    merger->addFragment(1, rectangle(20, 4, 3, 2), kLayer, nullptr, 0, 0); // Rows 4..6.
    merger->addFragment(1, rectangle(20, 6, 3, 2), other, nullptr, 0, 0);  // Rows 6..8.
    merger->addFragment(1, rectangle(20, 7, 3, 6), kLayer, nullptr, 0, 1); // Rows 7..13.
    merger->notifyTileComplete(0, sink);
    merger->notifyTileComplete(1, sink);
    merger->resolve(sink, nullptr, CancellationToken());

    ASSERT_EQ(sink.emittedCount(), 3);
    EXPECT_EQ(merger->largestComponentFragmentCount(), 2);
    std::vector<std::tuple<int64_t, int64_t, double>> objects; // Layer, anchor y, area.
    for (const ObjectRecord& record : sink.records()) {
        objects.emplace_back(record.layer().value(), record.geometry().anchor().y, measurement(record, "area"));
    }
    std::sort(objects.begin(), objects.end());
    const std::vector<std::tuple<int64_t, int64_t, double>> expected = {
        {kLayer.value(), 2, 36.0}, {kLayer.value(), 7, 38.0}, {other.value(), 4, 36.0}};
    EXPECT_EQ(objects, expected);
}

TEST(SeamMerger, ConstructionValidatesItsArguments) {
    const TileGrid grid = gridOf(3, 3, 10, 10);
    EXPECT_THROW(SeamMerger(grid, nullptr, scratch().c_str()), InvalidArgumentError);
    EXPECT_THROW(SeamMerger(grid, makeMemoryBudget(1024), nullptr), InvalidArgumentError);
    EXPECT_THROW(SeamMerger(grid, makeMemoryBudget(1024), "no/such/directory/for/the/seam/merger"),
                 InvalidArgumentError);
    // Not UTF-8: the path cannot even be formed on some platforms; still the documented error.
    EXPECT_THROW(SeamMerger(grid, makeMemoryBudget(1024), "\xff\xfe-not-utf8"), InvalidArgumentError);
}

TEST(SeamMerger, AddFragmentValidatesItsArguments) {
    const TileGrid grid = gridOf(3, 3, 10, 10);
    auto merger = mergerFor(grid);
    const Polygon fragment = rectangle(5, 2, 5, 4);
    EXPECT_THROW(merger->addFragment(9, fragment, kLayer, nullptr, 0), InvalidArgumentError);
    EXPECT_THROW(merger->addFragment(-1, fragment, kLayer, nullptr, 0), InvalidArgumentError);
    EXPECT_THROW(merger->addFragment(0, fragment, kLayer, nullptr, 1), InvalidArgumentError);
    EXPECT_THROW(merger->addFragment(0, fragment, kLayer, nullptr, -1), InvalidArgumentError);
    EXPECT_THROW(merger->addFragment(0, Polygon(), kLayer, nullptr, 0), InvalidArgumentError);
    EXPECT_THROW(merger->addFragment(0, fragment, LayerId(), nullptr, 0), InvalidArgumentError);
    const IntensityPartial twice[2] = {partial(1, 1.0, 1.0, 1.0, 1.0, 2), partial(1, 1.0, 1.0, 1.0, 1.0, 2)};
    EXPECT_THROW(merger->addFragment(0, fragment, kLayer, twice, 2), InvalidArgumentError);
    ts::CollectingObjectSink sink;
    EXPECT_THROW(merger->notifyTileComplete(9, sink), InvalidArgumentError);
    EXPECT_EQ(merger->fragmentCount(), 0);
}
