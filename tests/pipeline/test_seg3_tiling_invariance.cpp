// Requirements: UR-SEG-3, UR-QA-2, UR-QA-3
#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/control.hpp>
#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/core/measurement.hpp>
#include <slideio/processor/core/memory_budget.hpp>
#include <slideio/processor/core/object_record.hpp>
#include <slideio/processor/pipeline/pipeline.hpp>

#include <pipeline/detail/pipeline_impl.hpp>
#include <pipeline/test_support/blob_scene.hpp>
#include <pipeline/test_support/synthetic_scene.hpp>
#include <pipeline/test_support/test_operations.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace slideio::processor;
namespace ts = slideio::processor::testing_support;
namespace fs = std::filesystem;

// The reference is a single-tile run (tile size = level extent).
// "Identical" compares, per object sorted by ObjectId: the id, the normalised polygon point for point,
// area, perimeter, centroid, bounding box, and intensity.<c>.{mean,stddev,min,max}.

namespace {

constexpr int64_t kWidth = 1000;
constexpr int64_t kHeight = 800;
const LayerId kLayer{3};

// Byte intensities sum exactly in double, so every measurement of a merged object
// is bitwise the single-tile one. Float32 intensities sum in a grouping that
// depends on where the seams cut the object, so their sums may differ in the last
// bits: intensity measurements are then compared to a relative difference of
// 1e-12, geometry still exactly. Runs that differ only in their worker count
// combine the same fragments in the same order and compare exactly for every type.
enum class IntensityMatch { Exact, Relative };
constexpr double kRelativeTolerance = 1e-12;

struct RunSetup {
    int32_t tileWidth;
    int32_t tileHeight;
    int32_t workers;
    int32_t level = 0;
};

Configuration config(const RunSetup& run, const std::string& scratch) {
    return ConfigurationBuilder()
        .tileSize(run.tileWidth, run.tileHeight)
        .tileSizeFromSlide(false)
        .workerCount(run.workers)
        .scratchDirectory(scratch.c_str())
        .build();
}

PipelineSpec components(int32_t level, bool eightConnected = false) {
    PipelineSpec spec;
    OperationParameters seams;
    seams.setBool("seams", true);
    if (eightConnected) {
        seams.setBool("eightConnected", true);
    }
    spec.operation("test.components", seams);
    spec.analysisLevel(level);
    return spec;
}

std::vector<ObjectRecord> objectsOf(const std::shared_ptr<slideio::Scene>& scene, const RunSetup& run,
                                    bool eightConnected = false) {
    Pipeline p(scene, config(run, std::string()), components(run.level, eightConnected));
    p.bindLayer("objects", kLayer);
    ts::CollectingObjectSink sink;
    p.runInto(sink, nullptr, CancellationToken());
    return sink.records();
}

std::vector<Point64> ringOf(const Polygon& polygon, int32_t ring) {
    std::vector<Point64> points(static_cast<size_t>(polygon.ringPointCount(ring)));
    polygon.ringPoints(ring, points.data(), static_cast<int64_t>(points.size()));
    return points;
}

std::vector<const ObjectRecord*> sortedById(const std::vector<ObjectRecord>& records) {
    std::vector<const ObjectRecord*> sorted;
    for (const ObjectRecord& r : records) {
        sorted.push_back(&r);
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const ObjectRecord* a, const ObjectRecord* b) { return a->id() < b->id(); });
    return sorted;
}

bool isIntensity(const std::string& name) { return name.rfind("intensity.", 0) == 0; }

double relativeDifference(double a, double b) {
    const double scale = std::max(std::abs(a), std::abs(b));
    return scale == 0.0 ? 0.0 : std::abs(a - b) / scale;
}

// Whether `actual` holds the objects of `expected`: the same ids, and per id the
// same layer, rings (hole flags and points, in order), centroid, bounding box and
// measurements (names, units and values, in order). Intensity values are compared
// as `match` says, everything else exactly. Failure messages name object indices
// and measurement names, never coordinates. `worstRelative`, when given, receives
// the largest relative difference among the intensity values.
::testing::AssertionResult identicalObjects(const std::vector<ObjectRecord>& expected,
                                            const std::vector<ObjectRecord>& actual, IntensityMatch match,
                                            double* worstRelative = nullptr) {
    if (expected.size() != actual.size()) {
        return ::testing::AssertionFailure()
               << actual.size() << " objects where the reference has " << expected.size();
    }
    const std::vector<const ObjectRecord*> want = sortedById(expected);
    const std::vector<const ObjectRecord*> got = sortedById(actual);
    double worst = 0.0;
    for (size_t i = 0; i < want.size(); ++i) {
        const ObjectRecord& e = *want[i];
        const ObjectRecord& a = *got[i];
        const std::string object = "object " + std::to_string(i) + " of " + std::to_string(want.size());
        if (e.id() != a.id()) {
            return ::testing::AssertionFailure() << object << ": the id differs";
        }
        if (e.layer() != a.layer()) {
            return ::testing::AssertionFailure() << object << ": the layer differs";
        }
        const Polygon& ep = e.geometry();
        const Polygon& ap = a.geometry();
        if (ep.ringCount() != ap.ringCount()) {
            return ::testing::AssertionFailure()
                   << object << ": " << ap.ringCount() << " rings where the reference has " << ep.ringCount();
        }
        for (int32_t r = 0; r < ep.ringCount(); ++r) {
            if (ep.ringIsHole(r) != ap.ringIsHole(r)) {
                return ::testing::AssertionFailure() << object << ", ring " << r << ": the hole flag differs";
            }
            const std::vector<Point64> er = ringOf(ep, r);
            const std::vector<Point64> ar = ringOf(ap, r);
            if (er.size() != ar.size()) {
                return ::testing::AssertionFailure() << object << ", ring " << r << ": " << ar.size()
                                                     << " points where the reference has " << er.size();
            }
            for (size_t k = 0; k < er.size(); ++k) {
                if (!(er[k] == ar[k])) {
                    return ::testing::AssertionFailure() << object << ", ring " << r << ": point " << k << " differs";
                }
            }
        }
        if (!(ep.centroid() == ap.centroid())) {
            return ::testing::AssertionFailure() << object << ": the centroid differs";
        }
        if (!(e.boundingBox() == a.boundingBox())) {
            return ::testing::AssertionFailure() << object << ": the bounding box differs";
        }
        const MeasurementSet& em = e.measurements();
        const MeasurementSet& am = a.measurements();
        if (em.count() != am.count()) {
            return ::testing::AssertionFailure()
                   << object << ": " << am.count() << " measurements where the reference has " << em.count();
        }
        for (int32_t m = 0; m < em.count(); ++m) {
            const Measurement em1 = em.at(m);
            const Measurement am1 = am.at(m);
            const std::string name = em1.name();
            if (name != am1.name()) {
                return ::testing::AssertionFailure() << object << ": measurement " << m << " is " << am1.name()
                                                     << " where the reference has " << name;
            }
            if (em1.unit() != am1.unit()) {
                return ::testing::AssertionFailure() << object << ": the unit of " << name << " differs";
            }
            if (isIntensity(name)) {
                // A NaN or an infinity would make the relative difference NaN, which
                // no comparison rejects, so the values must be finite first.
                if (match == IntensityMatch::Relative &&
                    (!std::isfinite(em1.value()) || !std::isfinite(am1.value()))) {
                    return ::testing::AssertionFailure() << object << ": " << name << " is not finite";
                }
                const double difference = relativeDifference(em1.value(), am1.value());
                worst = std::max(worst, difference);
                if (match == IntensityMatch::Relative ? !(difference <= kRelativeTolerance)
                                                      : em1.value() != am1.value()) {
                    return ::testing::AssertionFailure()
                           << object << ": " << name << " differs, relative difference " << difference;
                }
            } else if (em1.value() != am1.value()) {
                return ::testing::AssertionFailure() << object << ": " << name << " differs";
            }
        }
    }
    if (worstRelative != nullptr) {
        *worstRelative = worst;
    }
    return ::testing::AssertionSuccess();
}

// Whether the record carries area, perimeter and the four intensity statistics of `channel`.
bool hasStandardMeasurements(const ObjectRecord& record, int32_t channel) {
    const std::string prefix = "intensity." + std::to_string(channel) + ".";
    for (const std::string& name : {std::string("area"), std::string("perimeter"), prefix + "mean",
                                    prefix + "stddev", prefix + "min", prefix + "max"}) {
        bool found = false;
        for (int32_t m = 0; m < record.measurements().count(); ++m) {
            found = found || name == record.measurements().at(m).name();
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

// How many objects of `records` are cut by a seam of `tile`-pixel tiles of a
// level `scale` times coarser than level 0: a seam lies strictly inside the
// object's box mapped to that level.
int64_t cutBySeams(const std::vector<ObjectRecord>& records, int64_t tile, double scale = 1.0) {
    int64_t cut = 0;
    for (const ObjectRecord& r : records) {
        const Rect64 box = r.boundingBox();
        const auto crosses = [tile, scale](int64_t from0, int64_t extent0) {
            const auto from = static_cast<int64_t>(std::floor(static_cast<double>(from0) / scale));
            const auto to = static_cast<int64_t>(std::ceil(static_cast<double>(from0 + extent0) / scale));
            return (from / tile + 1) * tile < to;
        };
        cut += crosses(box.x, box.width) || crosses(box.y, box.height) ? 1 : 0;
    }
    return cut;
}

// What every reference run must show for the suite to mean something: the
// standard measurements on every object, the region spanning the level with its
// four holes, the annuli and the disc with two holes.
void expectMeaningfulReference(const std::vector<ObjectRecord>& reference, const Rect64& level0Extent,
                               int32_t intensityChannel) {
    ASSERT_GT(reference.size(), 20u);
    for (const ObjectRecord& r : reference) {
        ASSERT_TRUE(hasStandardMeasurements(r, intensityChannel));
    }
    const auto spanning = std::count_if(reference.begin(), reference.end(), [&](const ObjectRecord& r) {
        return r.boundingBox() == level0Extent && r.geometry().ringCount() == 5;
    });
    EXPECT_EQ(spanning, 1) << "one region spanning the level, with four holes";
    const auto withHoles = [&](int32_t rings) {
        return std::count_if(reference.begin(), reference.end(),
                             [rings](const ObjectRecord& r) { return r.geometry().ringCount() == rings; });
    };
    EXPECT_EQ(withHoles(2), 3) << "three annuli";
    EXPECT_EQ(withHoles(3), 1) << "one disc with two holes";
}

std::shared_ptr<slideio::Scene> dyadicScene(ts::BlobChannels channels) {
    ts::registerTestOperations();
    return ts::makeBlobScene({{kWidth, kHeight, 0, 0}}, 0, channels);
}

const Rect64 kLevelExtent{0, 0, kWidth, kHeight};

// A fresh, empty directory under the temporary directory, removed with the object.
class TestDirectory {
public:
    TestDirectory() {
        std::random_device device;
        path_ = fs::temp_directory_path() /
                ("seg3-test-" + std::to_string(device()) + "-" + std::to_string(device()));
        fs::create_directories(path_);
    }
    ~TestDirectory() {
        std::error_code error;
        fs::remove_all(path_, error);
    }
    TestDirectory(const TestDirectory&) = delete;
    TestDirectory& operator=(const TestDirectory&) = delete;

    const fs::path& path() const { return path_; }
    std::string utf8() const { return path_.u8string(); }
    bool empty() const {
        std::error_code error;
        return fs::is_empty(path_, error) && !error;
    }

private:
    fs::path path_;
};

// A raster sink that notes, at every tile, whether a seam spill file exists
// anywhere under the scratch directory: the merger creates it at its first spill
// and removes it only when the run ends.
class SpillWatchingRasterSink : public RasterSink {
public:
    explicit SpillWatchingRasterSink(const TestDirectory& scratch) : scratch_(scratch) {}
    void writeTile(const BlockView&) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++written_;
        if (sawSpillFile_) {
            return;
        }
        std::error_code error;
        for (fs::recursive_directory_iterator it(scratch_.path(), error), end; !error && it != end;
             it.increment(error)) {
            const std::string name = it->path().filename().u8string();
            if (name.rfind("seam-", 0) == 0 && it->path().extension() == ".bin") {
                sawSpillFile_ = true;
            }
        }
    }
    void flush() override {}
    int64_t tilesWritten() const override { return written_; }
    bool sawSpillFile() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return sawSpillFile_;
    }

private:
    const TestDirectory& scratch_;
    mutable std::mutex mutex_;
    bool sawSpillFile_ = false;
    std::atomic<int64_t> written_{0};
};

} // namespace

TEST(Seg3, IdenticalAcrossThreeTileSizesByte) {
    const auto scene = dyadicScene(ts::BlobChannels::Mask);
    const std::vector<ObjectRecord> reference = objectsOf(scene, {kWidth, kHeight, 4});
    expectMeaningfulReference(reference, kLevelExtent, 0);
    for (const int32_t tile : {64, 100, 257}) {
        ASSERT_GE(cutBySeams(reference, tile), 5) << tile << "-pixel seams should cut many objects";
        EXPECT_TRUE(identicalObjects(reference, objectsOf(scene, {tile, tile, 4}), IntensityMatch::Exact))
            << tile << "-pixel tiles";
    }
}

// The two-plane form: the Byte mask selects the pixels, the Float32 plane is
// measured, so float partials cross the seams.
TEST(Seg3, IdenticalAcrossTileSizesFloat) {
    const auto scene = dyadicScene(ts::BlobChannels::MaskAndFloatValues);
    const std::vector<ObjectRecord> reference = objectsOf(scene, {kWidth, kHeight, 4});
    expectMeaningfulReference(reference, kLevelExtent, 1);
    for (const int32_t tile : {64, 100, 257}) {
        double worst = 0.0;
        EXPECT_TRUE(
            identicalObjects(reference, objectsOf(scene, {tile, tile, 4}), IntensityMatch::Relative, &worst))
            << tile << "-pixel tiles";
        std::ostringstream worstText;
        worstText << std::scientific << worst;
        RecordProperty("worstRelativeDifference" + std::to_string(tile), worstText.str());
    }
}

// Level 1 of an 8001 x 6001 slide is 2000 x 1500: a scale of 4.0005, so seams
// land on level-0 coordinates that are not multiples of the tile size.
TEST(Seg3, IdenticalOnANonDyadicLevel) {
    ts::registerTestOperations();
    const auto scene =
        ts::makeBlobScene({{8001, 6001, 0, 0}, {2000, 1500, 0, 0}}, 1, ts::BlobChannels::Mask);
    const std::vector<ObjectRecord> reference = objectsOf(scene, {2000, 1500, 4, 1});
    expectMeaningfulReference(reference, Rect64{0, 0, 8001, 6001}, 0);
    for (const int32_t tile : {64, 100, 257}) {
        ASSERT_GE(cutBySeams(reference, tile, 8001.0 / 2000.0), 5) << tile << "-pixel seams should cut many objects";
        EXPECT_TRUE(identicalObjects(reference, objectsOf(scene, {tile, tile, 4, 1}), IntensityMatch::Exact))
            << tile << "-pixel tiles";
    }
}

TEST(Seg3, IdenticalAcrossWorkerCounts) {
    for (const ts::BlobChannels channels : {ts::BlobChannels::Mask, ts::BlobChannels::MaskAndFloatValues}) {
        const auto scene = dyadicScene(channels);
        const char* form = channels == ts::BlobChannels::Mask ? "Byte" : "Float32";
        const std::vector<ObjectRecord> one = objectsOf(scene, {100, 100, 1});
        ASSERT_GE(cutBySeams(one, 100), 5);
        for (const int32_t workers : {2, 4, 8}) {
            EXPECT_TRUE(identicalObjects(one, objectsOf(scene, {100, 100, workers}), IntensityMatch::Exact))
                << form << ", " << workers << " workers";
        }
    }
}

// A memory ceiling of exactly the tiles the workers hold leaves no room for seam
// fragments, so the region spanning the level, open from the first tile to the
// last, goes to the scratch file with every other fragment; the objects are those
// of the run that holds everything in memory.
TEST(Seg3, IdenticalWhenSpilling) {
    for (const ts::BlobChannels channels : {ts::BlobChannels::Mask, ts::BlobChannels::MaskAndFloatValues}) {
        const auto scene = dyadicScene(channels);
        const char* form = channels == ts::BlobChannels::Mask ? "Byte" : "Float32";
        const std::vector<ObjectRecord> roomy = objectsOf(scene, {64, 64, 4});
        for (const int32_t workers : {1, 4}) {
            TestDirectory scratch;
            const RunSetup run{64, 64, workers};
            Pipeline p(scene, config(run, scratch.utf8()), components(0));
            p.bindLayer("objects", kLayer);
            SpillWatchingRasterSink raster(scratch);
            p.rasterSink(&raster);
            p.validate();
            const int64_t tileBytes = detail::PipelineAccess::plan(p).tileBytes;
            p.memoryBudget(makeMemoryBudget(workers * tileBytes));
            ts::CollectingObjectSink sink;
            p.runInto(sink, nullptr, CancellationToken());

            EXPECT_TRUE(raster.sawSpillFile()) << form << ", " << workers << " workers: nothing was spilled";
            EXPECT_TRUE(scratch.empty()) << form << ", " << workers << " workers: the scratch directory survived";
            EXPECT_TRUE(identicalObjects(roomy, sink.records(), IntensityMatch::Exact))
                << form << ", " << workers << " workers";
        }
    }
}

// Two squares meeting only at a tile corner have no seam contact of positive
// length, so they stay two objects at every tile size, as in the single-tile run.
TEST(Seg3, CornerTouchingObjectsStaySeparate) {
    const auto scene = dyadicScene(ts::BlobChannels::Mask);
    const std::vector<ObjectRecord> reference = objectsOf(scene, {kWidth, kHeight, 4});
    for (const int32_t tile : {static_cast<int32_t>(kWidth), 64, 100, 257}) {
        const std::vector<ObjectRecord> objects =
            tile == kWidth ? reference : objectsOf(scene, {tile, tile, 4});
        for (const ts::CornerTouchingPair& pair : ts::cornerTouchingPairs()) {
            const int64_t s = ts::kCornerSquareSide;
            const Rect64 before{pair.x - s, pair.y - s, s, s};
            const Rect64 after{pair.x, pair.y, s, s};
            const Rect64 both{pair.x - s, pair.y - s, 2 * s, 2 * s};
            const auto count = [&](const Rect64& box) {
                return std::count_if(objects.begin(), objects.end(), [&](const ObjectRecord& r) {
                    return r.boundingBox() == box && r.geometry().ringCount() == 1 &&
                           r.geometry().area() == static_cast<double>(box.width * box.height);
                });
            };
            const auto joined = std::count_if(objects.begin(), objects.end(),
                                              [&](const ObjectRecord& r) { return r.boundingBox() == both; });
            EXPECT_EQ(count(before), 1) << "pair at the " << pair.tileSide << "-pixel corner, " << tile << "-pixel tiles";
            EXPECT_EQ(count(after), 1) << "pair at the " << pair.tileSide << "-pixel corner, " << tile << "-pixel tiles";
            EXPECT_EQ(joined, 0) << "pair at the " << pair.tileSide << "-pixel corner, " << tile << "-pixel tiles";
        }
        if (tile != kWidth) {
            EXPECT_TRUE(identicalObjects(reference, objects, IntensityMatch::Exact)) << tile << "-pixel tiles";
        }
    }
}

// 8-connected, each corner-touching pair is one object whose outline touches
// itself at the corner, joined across the seams at every tile size, and every
// other object is the single-tile one.
TEST(Seg3, EightConnectedBlobsInvariantAcrossTileSizes) {
    const auto scene = dyadicScene(ts::BlobChannels::Mask);
    const std::vector<ObjectRecord> reference = objectsOf(scene, {kWidth, kHeight, 4}, true);
    const int64_t s = ts::kCornerSquareSide;
    for (const ts::CornerTouchingPair& pair : ts::cornerTouchingPairs()) {
        const Rect64 both{pair.x - s, pair.y - s, 2 * s, 2 * s};
        const auto joined = std::count_if(reference.begin(), reference.end(), [&](const ObjectRecord& r) {
            return r.boundingBox() == both && r.geometry().ringCount() == 1 &&
                   r.geometry().area() == static_cast<double>(2 * s * s);
        });
        EXPECT_EQ(joined, 1) << "pair at the " << pair.tileSide << "-pixel corner";
    }
    for (const int32_t tile : {64, 100, 257}) {
        ASSERT_GE(cutBySeams(reference, tile), 5) << tile << "-pixel seams should cut many objects";
        EXPECT_TRUE(identicalObjects(reference, objectsOf(scene, {tile, tile, 4}, true), IntensityMatch::Exact))
            << tile << "-pixel tiles";
    }
}
