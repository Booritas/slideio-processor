// Requirements: UR-SEG-8, UR-SEG-3, UR-SEG-5, UR-SEG-7, UR-QA-2, UR-ALG-6
#include <slideio/processor/algorithm/measure/measure.hpp>
#include <slideio/processor/algorithm/pixel/filter.hpp>
#include <slideio/processor/algorithm/segment/segment.hpp>
#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/block_view.hpp>
#include <slideio/processor/core/control.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/core/measurement.hpp>
#include <slideio/processor/core/object_record.hpp>
#include <slideio/processor/pipeline/operation_catalog.hpp>
#include <slideio/processor/pipeline/pipeline.hpp>
#include <slideio/processor/pipeline/pipeline_spec.hpp>

#include <algorithm/support/harness.hpp>
#include <pipeline/test_support/synthetic_scene.hpp>
#include <pipeline/test_support/test_operations.hpp>

#include <slideio/slideio/scene.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using namespace slideio::processor;
namespace ts = slideio::processor::testing_support;

namespace {

constexpr const char* kName = "segment.watershed";
// The two reach failures' openings: a pixel reached from a marker beyond the
// spacing, or one no marker reached.
constexpr const char* kTooFarPrefix = "segment.watershed: the foreground at level pixel (";
constexpr const char* kUnreachedPrefix =
    "segment.watershed: no marker within maximumMarkerSpacingPx reaches the foreground at level pixel (";
constexpr double kPi = 3.14159265358979323846;

double measurement(const ObjectRecord& record, const char* name) {
    Measurement m;
    return record.measurements().find(name, m) ? m.value() : std::numeric_limits<double>::quiet_NaN();
}

bool has(const ObjectRecord& record, const char* name) {
    Measurement m;
    return record.measurements().find(name, m);
}

std::vector<Point64> ringOf(const Polygon& polygon, int32_t ring) {
    std::vector<Point64> points(static_cast<size_t>(polygon.ringPointCount(ring)));
    polygon.ringPoints(ring, points.data(), static_cast<int64_t>(points.size()));
    return points;
}

bool sameGeometry(const Polygon& a, const Polygon& b) {
    if (a.ringCount() != b.ringCount()) {
        return false;
    }
    for (int32_t r = 0; r < a.ringCount(); ++r) {
        if (a.ringIsHole(r) != b.ringIsHole(r)) {
            return false;
        }
        const std::vector<Point64> pa = ringOf(a, r);
        const std::vector<Point64> pb = ringOf(b, r);
        if (pa.size() != pb.size() || !std::equal(pa.begin(), pa.end(), pb.begin())) {
            return false;
        }
    }
    return true;
}

// Expects @p objects to equal @p reference object by object: ids, layers,
// rings point for point and every measurement exactly.
void expectSameObjects(const std::vector<ObjectRecord>& reference, const std::vector<ObjectRecord>& objects,
                       const std::string& label) {
    ASSERT_EQ(objects.size(), reference.size()) << label;
    for (size_t i = 0; i < reference.size(); ++i) {
        const ObjectRecord& want = reference[i];
        const ObjectRecord& got = objects[i];
        ASSERT_TRUE(got.id() == want.id()) << label << ", object " << i;
        EXPECT_EQ(got.layer(), want.layer()) << label << ", object " << i;
        EXPECT_TRUE(sameGeometry(got.geometry(), want.geometry())) << label << ", object " << i;
        ASSERT_EQ(got.measurements().count(), want.measurements().count()) << label << ", object " << i;
        for (int32_t k = 0; k < want.measurements().count(); ++k) {
            const Measurement m = want.measurements().at(k);
            ASSERT_TRUE(has(got, m.name())) << label << ", object " << i << ": " << m.name();
            EXPECT_EQ(measurement(got, m.name()), m.value()) << label << ", object " << i << ": " << m.name();
        }
    }
}

std::string jsonOf(const OperationParameters& parameters) {
    std::string text(static_cast<size_t>(parameters.toJson(nullptr, 0)) + 1, '\0');
    parameters.toJson(&text[0], static_cast<int32_t>(text.size()));
    text.resize(text.size() - 1);
    return text;
}

PipelineSpec specOf(const WatershedParameters& parameters) {
    PipelineSpec spec;
    spec.then(makeWatershed(parameters));
    return spec;
}

// A disc of pixels: those whose centre lies within @p r of the centre of
// pixel (cx, cy).
struct Disc {
    int64_t cx;
    int64_t cy;
    int64_t r;
    bool contains(int64_t x, int64_t y) const { return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r; }
};

// The Euclidean distance transform of a union of discs over a width x height
// image, by brute force: each pixel of a disc gets the distance from its centre
// to the nearest pixel centre outside every disc (the image's outside
// included), 0 elsewhere. Every background pixel near a disc lies within
// @p reach of it.
std::vector<double> distanceTransform(int64_t width, int64_t height, const std::vector<Disc>& discs,
                                      int64_t reach) {
    const auto inside = [&](int64_t x, int64_t y) {
        return std::any_of(discs.begin(), discs.end(), [x, y](const Disc& d) { return d.contains(x, y); });
    };
    std::vector<uint8_t> mask(static_cast<size_t>(width * height));
    for (int64_t y = 0; y < height; ++y) {
        for (int64_t x = 0; x < width; ++x) {
            mask[static_cast<size_t>(y * width + x)] = inside(x, y) ? 1 : 0;
        }
    }
    std::vector<double> distance(static_cast<size_t>(width * height), 0.0);
    for (const Disc& d : discs) {
        for (int64_t y = d.cy - d.r; y <= d.cy + d.r; ++y) {
            for (int64_t x = d.cx - d.r; x <= d.cx + d.r; ++x) {
                if (x < 0 || y < 0 || x >= width || y >= height || !d.contains(x, y) ||
                    distance[static_cast<size_t>(y * width + x)] > 0.0) {
                    continue;
                }
                int64_t best = std::numeric_limits<int64_t>::max();
                for (int64_t v = y - reach; v <= y + reach; ++v) {
                    for (int64_t u = x - reach; u <= x + reach; ++u) {
                        const bool background =
                            u < 0 || v < 0 || u >= width || v >= height ||
                            mask[static_cast<size_t>(v * width + u)] == 0;
                        if (background) {
                            best = std::min(best, (u - x) * (u - x) + (v - y) * (v - y));
                        }
                    }
                }
                distance[static_cast<size_t>(y * width + x)] = std::sqrt(static_cast<double>(best));
            }
        }
    }
    return distance;
}

std::shared_ptr<slideio::Scene> sceneOf(int64_t width, int64_t height, std::vector<slideio::DataType> types,
                                        ts::PixelFunction pixel) {
    return ts::makeSyntheticScene({{width, height, 0, 0}}, std::move(types), std::move(pixel));
}

// An Int64 raster at level 0: value(x, y) at each pixel.
class AnchorSource : public TileRasterSource {
public:
    explicit AnchorSource(std::function<int64_t(int64_t, int64_t)> value, DataType type = DataType::Int64)
        : value_(std::move(value)), type_(type) {}

    void fillTile(BlockView& out) override {
        const Rect64 origin = out.slideRect();
        const int64_t size = dataTypeSizeBytes(type_);
        for (int64_t y = 0; y < out.height(); ++y) {
            auto* row = static_cast<uint8_t*>(out.planeData(0)) + y * out.planeRowStrideBytes(0);
            for (int64_t x = 0; x < out.width(); ++x) {
                const int64_t v = value_(origin.x + x, origin.y + y);
                if (type_ == DataType::Int64) {
                    std::memcpy(row + x * size, &v, sizeof v);
                } else {
                    const auto narrow = static_cast<uint16_t>(v);
                    std::memcpy(row + x * size, &narrow, sizeof narrow);
                }
            }
        }
    }
    DataType planeType() const override { return type_; }
    int32_t planeCount() const override { return 1; }

private:
    std::function<int64_t(int64_t, int64_t)> value_;
    DataType type_;
};

// A run as ts::runEagerObjects makes it, with @p anchors bound as "anchors".
std::vector<ObjectRecord> runWithAnchors(const std::shared_ptr<slideio::Scene>& scene, const PipelineSpec& spec,
                                         int32_t tileSize, TileRasterSource& anchors) {
    const Configuration config =
        ConfigurationBuilder().tileSize(tileSize, tileSize).tileSizeFromSlide(false).workerCount(4).build();
    Pipeline pipeline(scene, config, spec);
    pipeline.bindLayer("objects", ts::objectsLayer());
    pipeline.auxiliaryRaster("anchors", &anchors);
    ts::CollectingObjectSink sink;
    pipeline.runInto(sink, nullptr, CancellationToken());
    std::vector<ObjectRecord> records = sink.records();
    std::sort(records.begin(), records.end(),
              [](const ObjectRecord& a, const ObjectRecord& b) { return a.id() < b.id(); });
    return records;
}

enum class Reach { Either, TooFar, Unreached };

struct ReachFailure {
    Reach kind = Reach::Either;
    int64_t x = -1;
    int64_t y = -1;
    std::string message;
};

// Expects @p run to fail with a reach error, as a run reports it: the
// operation's own message is the failure's cause. Returns its kind and the
// level pixel it names; @p want, unless Either, is the kind expected.
ReachFailure expectReachError(const std::function<void()>& run, const std::string& label,
                              Reach want = Reach::Either) {
    ReachFailure failure;
    try {
        run();
        ADD_FAILURE() << label << ": nothing thrown";
    } catch (const ProcessorError& e) {
        failure.message = e.cause() != nullptr && e.cause()[0] != '\0' ? e.cause() : e.what();
        const std::string& message = failure.message;
        size_t at = std::string::npos;
        if (message.rfind(kTooFarPrefix, 0) == 0) {
            failure.kind = Reach::TooFar;
            at = std::strlen(kTooFarPrefix);
            EXPECT_NE(message.find(") lies further than maximumMarkerSpacingPx from its marker"), std::string::npos)
                << label << ": " << message;
        } else if (message.rfind(kUnreachedPrefix, 0) == 0) {
            failure.kind = Reach::Unreached;
            at = std::strlen(kUnreachedPrefix);
            EXPECT_NE(message.find("minimumMarkerDistancePx"), std::string::npos) << label << ": " << message;
        }
        if (at == std::string::npos) {
            ADD_FAILURE() << label << ": not a reach error: " << message;
            return failure;
        }
        if (want != Reach::Either) {
            EXPECT_EQ(failure.kind, want) << label << ": " << message;
        }
        // "<x>, <y>)" follows the prefix.
        char* end = nullptr;
        failure.x = std::strtoll(message.c_str() + at, &end, 10);
        EXPECT_EQ(std::string(end, 2), ", ") << label << ": " << message;
        failure.y = std::strtoll(end + 2, &end, 10);
        EXPECT_EQ(*end, ')') << label << ": " << message;
        // The advice names parameters, never their values.
        const std::string advice = message.substr(message.find(')'));
        // Only the 1 of "minimumMarkerDistancePx + 1" is a digit.
        EXPECT_EQ(std::count_if(advice.begin(), advice.end(), [](char c) { return c >= '0' && c <= '9'; }),
                  failure.kind == Reach::Unreached ? 1 : 0)
            << label << ": " << message;
    }
    return failure;
}

int64_t packedKey(int64_t x, int64_t y) { return (y << 31) | x; }

} // namespace

TEST(Watershed, TouchingDiscsSeparated) {
    // Two discs of radius 10 whose centres lie 16 apart; plane 0 is the
    // distance transform of their union, so each centre is a maximum.
    constexpr int64_t kWidth = 64;
    constexpr int64_t kHeight = 48;
    const std::vector<Disc> discs = {{24, 24, 10}, {40, 24, 10}};
    const std::vector<double> dt = distanceTransform(kWidth, kHeight, discs, 12);
    const OperationPtr op = makeWatershed(WatershedParameters());
    EXPECT_EQ(op->arity(), ChannelArity::Joint);
    EXPECT_FALSE(op->objectsAreEightConnected());
    EXPECT_TRUE(op->producesObjects());
    const ts::DirectObjects result = ts::applyDirectObjects(
        *op, {DataType::Float32, DataType::Byte}, {0, 1}, kWidth, kHeight, [&](int32_t plane, int64_t x, int64_t y) {
            const double d = dt[static_cast<size_t>(y * kWidth + x)];
            return plane == 0 ? d : (d > 0.0 ? 100.0 : 0.0);
        });
    EXPECT_TRUE(result.fragments.empty());
    ASSERT_EQ(result.objects.size(), 2u);

    // Each basin is its disc less half the lens the discs share: the disc cut
    // by the chord halfway between the centres.
    const double r = 10.0;
    const double a = 8.0;
    const double halfLens = r * r * std::acos(a / r) - a * std::sqrt(r * r - a * a);
    const double area = kPi * r * r - halfLens;
    // The cut-off segment's centroid lies (2/3)(r^2 - a^2)^(3/2) / halfLens from
    // the centre, so the rest's lies that much times halfLens / area the other
    // way. Pixel (x, y) spans x..x+1, so a centre pixel's centre is +0.5.
    const double shift = (2.0 / 3.0) * std::pow(r * r - a * a, 1.5) / area;
    const double expectedX[2] = {24.5 - shift, 40.5 + shift};
    std::vector<ObjectRecord> objects = result.objects;
    std::sort(objects.begin(), objects.end(), [](const ObjectRecord& p, const ObjectRecord& q) {
        return measurement(p, "centroid.x") < measurement(q, "centroid.x");
    });
    for (size_t i = 0; i < 2; ++i) {
        const ObjectRecord& object = objects[i];
        EXPECT_EQ(object.geometry().ringCount(), 1) << i;
        EXPECT_NEAR(measurement(object, "area"), area, 0.05 * area) << i;
        EXPECT_NEAR(measurement(object, "centroid.x"), expectedX[i], 1.0) << i;
        EXPECT_NEAR(measurement(object, "centroid.y"), 24.5, 1.0) << i;
        EXPECT_EQ(measurement(object, "intensity.1.mean"), 100.0) << i;
    }
    // Together the basins are the union.
    int64_t unionArea = 0;
    for (int64_t y = 0; y < kHeight; ++y) {
        for (int64_t x = 0; x < kWidth; ++x) {
            unionArea += dt[static_cast<size_t>(y * kWidth + x)] > 0.0 ? 1 : 0;
        }
    }
    EXPECT_EQ(measurement(objects[0], "area") + measurement(objects[1], "area"), static_cast<double>(unionArea));

    // The raster: each basin's 1-based index within the tile, 0 elsewhere.
    const ts::Image labels = ts::applyDirect(*op, {DataType::Float32}, {0}, kWidth, kHeight,
                                             [&](int32_t, int64_t x, int64_t y) {
                                                 return dt[static_cast<size_t>(y * kWidth + x)];
                                             });
    ASSERT_EQ(labels.types, std::vector<DataType>{DataType::Int32});
    EXPECT_EQ(labels.at(0, 24, 24), 1.0);
    EXPECT_EQ(labels.at(0, 40, 24), 2.0);
    EXPECT_EQ(labels.at(0, 0, 0), 0.0);
}

TEST(Watershed, TilingInvariantAtThreeSizes) {
    // A grid of 6 x 5 cells, each with a pair of touching discs: radii 8 to
    // 11, centres 1.3 to 1.7 radii apart, side by side, one above the other or
    // diagonal, placed off the cell's centre so seams cut them anywhere.
    constexpr int64_t kCellWidth = 66;
    constexpr int64_t kCellHeight = 58;
    constexpr int64_t kWidth = 6 * kCellWidth;
    constexpr int64_t kHeight = 5 * kCellHeight;
    std::vector<Disc> discs;
    for (int64_t row = 0; row < 5; ++row) {
        for (int64_t column = 0; column < 6; ++column) {
            const int64_t i = row * 6 + column;
            const int64_t r = 8 + i % 4;
            const double separation = static_cast<double>(r) * (1.3 + 0.1 * static_cast<double>(i % 5));
            const int64_t cx = column * kCellWidth + kCellWidth / 2 + (i % 7) - 3;
            const int64_t cy = row * kCellHeight + kCellHeight / 2 + (i % 5) - 2;
            int64_t dx = 0;
            int64_t dy = 0;
            switch (i % 3) {
            case 0: dx = std::llround(separation / 2.0); break;
            case 1: dy = std::llround(separation / 2.0); break;
            default:
                dx = std::llround(separation / 2.0 / std::sqrt(2.0));
                dy = dx;
                break;
            }
            discs.push_back({cx - dx, cy - dy, r});
            discs.push_back({cx + dx, cy + dy, r});
        }
    }
    auto dt = std::make_shared<std::vector<double>>(distanceTransform(kWidth, kHeight, discs, 14));
    const auto scene = sceneOf(kWidth, kHeight, {slideio::DataType::DT_Float32, slideio::DataType::DT_Byte},
                               [dt](int channel, int64_t x, int64_t y) {
                                   if (channel == 1) {
                                       return static_cast<double>((x * 7 + y * 13) % 251);
                                   }
                                   return (*dt)[static_cast<size_t>(y * kWidth + x)];
                               });
    const PipelineSpec spec = specOf(WatershedParameters());
    const std::vector<ObjectRecord> reference = ts::runEagerObjects(scene, spec, 512, 4);
    // Every disc is its own basin.
    ASSERT_EQ(reference.size(), discs.size());
    for (const ObjectRecord& r : reference) {
        ASSERT_TRUE(has(r, "intensity.1.mean"));
        ASSERT_TRUE(has(r, "centroid.x"));
    }
    for (const int32_t tile : {64, 100, 257}) {
        const auto cut = std::count_if(reference.begin(), reference.end(), [tile](const ObjectRecord& r) {
            const Rect64 b = r.boundingBox();
            return b.x / tile != (b.x + b.width - 1) / tile || b.y / tile != (b.y + b.height - 1) / tile;
        });
        ASSERT_GE(cut, 5) << tile << "-pixel seams should cut many objects";
        expectSameObjects(reference, ts::runEagerObjects(scene, spec, tile, 4),
                          std::to_string(tile) + "-pixel tiles");
    }

    // A sheet of touching discs: radius 8 on a jittered 14-pixel lattice,
    // most touching their neighbours, so foreground spans every tile, with a
    // spacing of 12 so the halo is small beside the tiles. Flooded as Float32
    // and as the distance floored to Byte, whose plateaus and ties the order
    // must settle the same way in every tile. Every disc lies inside the
    // level: one cut by its edge could lose its maximum to a neighbour and be
    // flooded from beyond the spacing.
    std::vector<Disc> sheet;
    for (int64_t j = 0; 12 + 14 * j < kHeight - 12; ++j) {
        for (int64_t i = 0; 12 + 14 * i < kWidth - 12; ++i) {
            sheet.push_back({12 + 14 * i + (i * 7 + j * 3) % 5 - 2, 12 + 14 * j + (i * 3 + j * 5) % 5 - 2, 8});
        }
    }
    auto sheetDt = std::make_shared<std::vector<double>>(distanceTransform(kWidth, kHeight, sheet, 12));
    const PipelineSpec sheetSpec = specOf(WatershedParameters().maximumMarkerSpacingPx(12));
    for (const bool floored : {false, true}) {
        const std::string label = floored ? "sheet, Byte" : "sheet, Float32";
        const auto sheetScene =
            sceneOf(kWidth, kHeight, {floored ? slideio::DataType::DT_Byte : slideio::DataType::DT_Float32},
                    [sheetDt, floored](int, int64_t x, int64_t y) {
                        const double d = (*sheetDt)[static_cast<size_t>(y * kWidth + x)];
                        return floored ? std::floor(d) : d;
                    });
        const std::vector<ObjectRecord> sheetReference = ts::runEagerObjects(sheetScene, sheetSpec, 512, 4);
        // Every disc is its own basin.
        ASSERT_EQ(sheetReference.size(), sheet.size()) << label;
        for (const int32_t tile : {37, 64, 100, 257}) {
            expectSameObjects(sheetReference, ts::runEagerObjects(sheetScene, sheetSpec, tile, 4),
                              label + ", " + std::to_string(tile) + "-pixel tiles");
        }
    }
}

TEST(Watershed, SpacingExceededThrowsAtEveryTileSize) {
    // One disc of radius 60, its only maximum at the centre: its rim lies
    // further than the spacing of 32 from the one marker.
    constexpr int64_t kSide = 200;
    const auto pixel = [](int, int64_t x, int64_t y) {
        const double d = std::sqrt(static_cast<double>((x - 100) * (x - 100) + (y - 100) * (y - 100)));
        return d <= 60.0 ? 61.0 - d : 0.0;
    };
    const auto scene = sceneOf(kSide, kSide, {slideio::DataType::DT_Float32}, pixel);
    const PipelineSpec spec = specOf(WatershedParameters().maximumMarkerSpacingPx(32));
    for (const int32_t tile : {64, 100, 257}) {
        expectReachError([&] { ts::runEagerObjects(scene, spec, tile, 4); }, std::to_string(tile) + "-pixel tiles");
    }
    const OperationPtr op = makeWatershed(WatershedParameters().maximumMarkerSpacingPx(32));
    // In one tile the one marker reaches the whole disc: the message names the
    // first rim pixel, in row order, beyond the spacing from the centre.
    const ReachFailure one = expectReachError(
        [&] { ts::applyDirectObjects(*op, {DataType::Float32}, {0}, kSide, kSide, pixel); }, "one tile",
        Reach::TooFar);
    EXPECT_GT(std::max(std::abs(one.x - 100), std::abs(one.y - 100)), 32) << one.message;
    EXPECT_GT(pixel(0, one.x, one.y), 0.0) << one.message;
    EXPECT_EQ(one.message.find("minimumMarkerValue"), std::string::npos) << one.message;
    // With a spacing that covers the disc, it is one object.
    const OperationPtr wide = makeWatershed(WatershedParameters().maximumMarkerSpacingPx(64));
    const ts::DirectObjects result = ts::applyDirectObjects(*wide, {DataType::Float32}, {0}, kSide, kSide, pixel);
    EXPECT_EQ(result.objects.size(), 1u);
    EXPECT_TRUE(result.fragments.empty());
}

TEST(Watershed, StripLongerThanSpacingThrows) {
    // A strip two pixels high falling from its one maximum at x = 0: in one
    // tile its far end is reached from a marker beyond the spacing; in small
    // tiles, those far from the maximum see no marker at all, and their pixels
    // are not reached.
    const auto pixel = [](int, int64_t x, int64_t y) {
        return y >= 4 && y < 6 && x < 150 ? 200.0 - static_cast<double>(x) : 0.0;
    };
    const WatershedParameters parameters = WatershedParameters().minimumMarkerDistancePx(2).maximumMarkerSpacingPx(8);
    const OperationPtr op = makeWatershed(parameters);
    // In one tile the marker at (0, 4) reaches the whole strip; (9, 4) is the
    // first pixel in row order beyond the spacing of 8.
    const ReachFailure one = expectReachError(
        [&] { ts::applyDirectObjects(*op, {DataType::Float32}, {0}, 150, 10, pixel); }, "one tile", Reach::TooFar);
    EXPECT_EQ(one.x, 9) << one.message;
    EXPECT_EQ(one.y, 4) << one.message;
    const auto scene = sceneOf(150, 10, {slideio::DataType::DT_Float32}, pixel);
    // With no floor set, the advice does not mention one.
    const ReachFailure tiled =
        expectReachError([&] { ts::runEagerObjects(scene, specOf(parameters), 16, 4); }, "16-pixel tiles");
    EXPECT_EQ(tiled.message.find("minimumMarkerValue"), std::string::npos) << tiled.message;
    EXPECT_GT(pixel(0, tiled.x, tiled.y), 0.0) << tiled.message;
}

TEST(Watershed, AuxiliaryMarkers) {
    // Two touching discs; the anchor raster marks a 3 x 3 square at each
    // centre, one with a value beyond 32 bits.
    constexpr int64_t kWidth = 120;
    constexpr int64_t kHeight = 80;
    const std::vector<Disc> discs = {{50, 40, 12}, {70, 40, 12}};
    auto dt = std::make_shared<std::vector<double>>(distanceTransform(kWidth, kHeight, discs, 14));
    const auto scene = sceneOf(kWidth, kHeight, {slideio::DataType::DT_Float32},
                               [dt](int, int64_t x, int64_t y) { return (*dt)[static_cast<size_t>(y * kWidth + x)]; });
    AnchorSource anchors([](int64_t x, int64_t y) -> int64_t {
        if (std::abs(y - 40) <= 1 && std::abs(x - 50) <= 1) {
            return 7;
        }
        if (std::abs(y - 40) <= 1 && std::abs(x - 70) <= 1) {
            return int64_t{5000000000};
        }
        return -1;
    });
    const PipelineSpec spec =
        specOf(WatershedParameters().markers(WatershedMarkers::AuxiliaryRaster).auxiliaryName("anchors"));
    const std::vector<ObjectRecord> reference = runWithAnchors(scene, spec, 128, anchors);
    ASSERT_EQ(reference.size(), 2u);
    double total = 0.0;
    for (const ObjectRecord& r : reference) {
        EXPECT_EQ(r.geometry().ringCount(), 1);
        total += measurement(r, "area");
    }
    int64_t unionArea = 0;
    for (const double d : *dt) {
        unionArea += d > 0.0 ? 1 : 0;
    }
    EXPECT_EQ(total, static_cast<double>(unionArea));
    // The seam x = 60 runs between the basins, and x = 50 through the first.
    for (const int32_t tile : {60, 25}) {
        expectSameObjects(reference, runWithAnchors(scene, spec, tile, anchors), std::to_string(tile) + "-pixel tiles");
    }

    // A bound raster that is not Int64 is refused, naming the parameter.
    AnchorSource narrow([](int64_t, int64_t) -> int64_t { return 1; }, DataType::UInt16);
    try {
        runWithAnchors(scene, spec, 128, narrow);
        ADD_FAILURE() << "a UInt16 anchor raster ran";
    } catch (const ProcessorError& e) {
        const std::string message = e.what();
        EXPECT_NE(message.find(kName), std::string::npos) << message;
        EXPECT_NE(message.find("auxiliaryName"), std::string::npos) << message;
    }
}

TEST(Watershed, MissingAuxiliaryThrows) {
    const OperationPtr op =
        makeWatershed(WatershedParameters().markers(WatershedMarkers::AuxiliaryRaster).auxiliaryName("anchors"));
    try {
        ts::applyDirectObjects(*op, {DataType::Byte}, {0}, 16, 16,
                               [](int32_t, int64_t x, int64_t y) { return x > 3 && y > 3 ? 1.0 : 0.0; });
        ADD_FAILURE() << "nothing thrown";
    } catch (const InvalidArgumentError& e) {
        const std::string message = e.what();
        EXPECT_NE(message.find(kName), std::string::npos) << message;
        EXPECT_NE(message.find("auxiliaryName"), std::string::npos) << message;
        EXPECT_EQ(message.find("anchors"), std::string::npos) << message;
    }
}

TEST(Watershed, PlateauMarkerIsTopmostLeftmost) {
    // A region touching the tile's left edge, of value 1, holding a plateau
    // of value 3 wider than the marker distance: rows 10 (x 4..15) and 11
    // (x 2..15). Its topmost-leftmost pixel, (4, 10), is the one marker, so
    // the region is one basin keyed by that pixel.
    const auto pixel = [](int32_t, int64_t x, int64_t y) {
        if ((y == 10 && x >= 4 && x < 16) || (y == 11 && x >= 2 && x < 16)) {
            return 3.0;
        }
        return y >= 5 && y < 25 && x < 20 ? 1.0 : 0.0;
    };
    const OperationPtr op = makeWatershed(WatershedParameters());
    const ts::DirectObjects result = ts::applyDirectObjects(*op, {DataType::Byte}, {0}, 40, 30, pixel);
    EXPECT_TRUE(result.objects.empty());
    ASSERT_EQ(result.fragments.size(), 1u);
    EXPECT_EQ(result.fragmentKeys[0], packedKey(4, 10));

    // Two plateaus further apart than the distance, the value falling away
    // from them: two basins, each keyed by its own plateau's topmost-leftmost
    // pixel, both touching the left edge.
    const auto twoPeaks = [](int32_t, int64_t x, int64_t y) {
        if (y < 2 || y >= 25 || x >= 12) {
            return 0.0;
        }
        // Chebyshev distance to each plateau: x 3..4 on rows 5..6, and x 6..8
        // on row 20; the value falls by 1 per pixel from 10 on them.
        const int64_t d1 = std::max({int64_t{0}, 3 - x, x - 4, 5 - y, y - 6});
        const int64_t d2 = std::max({int64_t{0}, 6 - x, x - 8, 20 - y, y - 20});
        return 10.0 - static_cast<double>(std::min(d1, d2));
    };
    const ts::DirectObjects two = ts::applyDirectObjects(*op, {DataType::Byte}, {0}, 40, 30, twoPeaks);
    ASSERT_EQ(two.fragments.size(), 2u);
    std::vector<int64_t> keys = two.fragmentKeys;
    std::sort(keys.begin(), keys.end());
    EXPECT_EQ(keys, (std::vector<int64_t>{packedKey(3, 5), packedKey(6, 20)}));
}

TEST(Watershed, HaloFollowsSpacing) {
    for (const int32_t spacing : {1, 5, 32, 100}) {
        const OperationPtr op = makeWatershed(
            WatershedParameters().minimumMarkerDistancePx(1).maximumMarkerSpacingPx(spacing));
        const Halo halo = op->halo();
        EXPECT_TRUE(halo.left == 2 * spacing + 1 && halo.top == 2 * spacing + 1 && halo.right == 2 * spacing + 1 &&
                    halo.bottom == 2 * spacing + 1)
            << spacing;
    }
    const Halo defaults = makeWatershed(WatershedParameters())->halo();
    EXPECT_EQ(defaults.left, 65);
}

TEST(Watershed, RejectsDistanceAboveSpacing) {
    const auto expectInvalid = [](const std::function<void()>& make, const char* key, const std::string& what) {
        try {
            make();
            ADD_FAILURE() << what << ": nothing thrown";
        } catch (const InvalidArgumentError& e) {
            const std::string message = e.what();
            EXPECT_NE(message.find(kName), std::string::npos) << what << ": " << message;
            EXPECT_NE(message.find(key), std::string::npos) << what << ": " << message;
        }
    };
    expectInvalid([] { makeWatershed(WatershedParameters().minimumMarkerDistancePx(33)); },
                  "minimumMarkerDistancePx", "distance above the spacing");
    expectInvalid(
        [] { makeWatershed(WatershedParameters().minimumMarkerDistancePx(10).maximumMarkerSpacingPx(9)); },
        "minimumMarkerDistancePx", "spacing below the distance");
    EXPECT_NO_THROW(makeWatershed(WatershedParameters().minimumMarkerDistancePx(9).maximumMarkerSpacingPx(9)));
    expectInvalid([] { makeWatershed(WatershedParameters().minimumMarkerDistancePx(0)); }, "minimumMarkerDistancePx",
                  "zero distance");
    expectInvalid([] { makeWatershed(WatershedParameters().maximumMarkerSpacingPx(0)); }, "maximumMarkerSpacingPx",
                  "zero spacing");
    expectInvalid([] { makeWatershed(WatershedParameters().maximumMarkerSpacingPx(1073741824)); },
                  "maximumMarkerSpacingPx", "spacing whose halo does not fit");
    expectInvalid([] { makeWatershed(WatershedParameters().minimumAreaPx(-1)); }, "minimumAreaPx", "negative area");
    expectInvalid([] { makeWatershed(WatershedParameters().markers(static_cast<WatershedMarkers>(5))); }, "markers",
                  "markers not an enumerator");
    expectInvalid([] { makeWatershed(WatershedParameters().markers(WatershedMarkers::AuxiliaryRaster)); },
                  "auxiliaryName", "auxiliary markers without a name");
    EXPECT_THROW(WatershedParameters().auxiliaryName(nullptr), InvalidArgumentError);
    EXPECT_THROW(WatershedParameters().targetLayer(nullptr), InvalidArgumentError);

    const auto byName = [](const std::function<void(OperationParameters&)>& fill) {
        return [fill] {
            OperationParameters bag;
            fill(bag);
            OperationCatalog::instance().create(kName, bag);
        };
    };
    expectInvalid(byName([](OperationParameters& b) { b.setInt("minimumMarkerDistancePx", 40); }),
                  "minimumMarkerDistancePx", "distance above the spacing by name");
    expectInvalid(byName([](OperationParameters& b) { b.setInt("maximumMarkerSpacingPx", int64_t{1} << 40); }),
                  "maximumMarkerSpacingPx", "spacing beyond 32 bits by name");
    expectInvalid(byName([](OperationParameters& b) { b.setString("markers", "Seeds"); }), "markers",
                  "unknown markers");
    expectInvalid(byName([](OperationParameters& b) { b.setString("measurements", "Area,Bogus"); }),
                  "measurements", "unknown measurement");
    expectInvalid(byName([](OperationParameters& b) { b.setInt("maximumAreaPx", 4); }), "maximumAreaPx",
                  "unknown key");
}

TEST(Watershed, TypedAndNamedBagsAreEqual) {
    const OperationPtr typed = makeWatershed(WatershedParameters()
                                                 .markers(WatershedMarkers::AuxiliaryRaster)
                                                 .auxiliaryName("anchors")
                                                 .minimumMarkerDistancePx(3)
                                                 .maximumMarkerSpacingPx(20)
                                                 .minimumAreaPx(12)
                                                 .measurements(MeasurementSelection().add(MeasurementKind::Area))
                                                 .targetLayer("nuclei"));
    OperationParameters bag;
    bag.setString("markers", "AuxiliaryRaster")
        .setString("auxiliaryName", "anchors")
        .setInt("minimumMarkerDistancePx", 3)
        .setInt("maximumMarkerSpacingPx", 20)
        .setInt("minimumAreaPx", 12)
        .setString("measurements", "Area")
        .setString("targetLayer", "nuclei");
    EXPECT_EQ(jsonOf(typed->parameters()), jsonOf(OperationCatalog::instance().create(kName, bag)->parameters()));

    // The defaults: complete except for the target layer, which is left out.
    const OperationPtr defaults = makeWatershed(WatershedParameters());
    EXPECT_EQ(jsonOf(defaults->parameters()),
              jsonOf(OperationCatalog::instance().create(kName, OperationParameters())->parameters()));
    EXPECT_EQ(defaults->parameters().count(), 6);
    EXPECT_STREQ(defaults->parameters().getString("markers", ""), "LocalMaxima");
    EXPECT_STREQ(defaults->parameters().getString("auxiliaryName", "absent"), "");
    int64_t value = 0;
    EXPECT_TRUE(defaults->parameters().getInt("minimumMarkerDistancePx", value) && value == 5);
    EXPECT_TRUE(defaults->parameters().getInt("maximumMarkerSpacingPx", value) && value == 32);
    EXPECT_TRUE(defaults->parameters().getInt("minimumAreaPx", value) && value == 0);
    EXPECT_STREQ(defaults->parameters().getString("targetLayer", "absent"), "absent");
}

TEST(Watershed, MarkerFloorAbsentByDefault) {
    const OperationPtr typed = makeWatershed(WatershedParameters());
    double value = 0.0;
    EXPECT_FALSE(typed->parameters().getDouble("minimumMarkerValue", value));
    EXPECT_EQ(typed->parameters().count(), 6);
    const OperationPtr named = OperationCatalog::instance().create(kName, OperationParameters());
    EXPECT_FALSE(named->parameters().getDouble("minimumMarkerValue", value));
    EXPECT_EQ(jsonOf(typed->parameters()), jsonOf(named->parameters()));
}

TEST(Watershed, MarkerFloorRoundTrips) {
    const OperationPtr typed = makeWatershed(WatershedParameters().minimumMarkerValue(2.5));
    OperationParameters bag;
    bag.setDouble("minimumMarkerValue", 2.5);
    const OperationPtr named = OperationCatalog::instance().create(kName, bag);
    EXPECT_EQ(jsonOf(typed->parameters()), jsonOf(named->parameters()));
    EXPECT_EQ(typed->parameters().count(), 7);
    double value = 0.0;
    ASSERT_TRUE(named->parameters().getDouble("minimumMarkerValue", value));
    EXPECT_EQ(value, 2.5);

    OperationParameters integerBag;
    integerBag.setInt("minimumMarkerValue", 3);
    const OperationPtr fromInteger = OperationCatalog::instance().create(kName, integerBag);
    value = 0.0;
    ASSERT_TRUE(fromInteger->parameters().getDouble("minimumMarkerValue", value));
    EXPECT_EQ(value, 3.0);
    EXPECT_EQ(jsonOf(fromInteger->parameters()),
              jsonOf(makeWatershed(WatershedParameters().minimumMarkerValue(3.0))->parameters()));
}

TEST(Watershed, UnreachedAbsentByDefault) {
    const auto expectAbsent = [](const OperationPtr& op) {
        EXPECT_STREQ(op->parameters().getString("unreachedForeground", "absent"), "absent");
        EXPECT_EQ(op->parameters().count(), 6);
    };
    expectAbsent(makeWatershed(WatershedParameters()));
    expectAbsent(OperationCatalog::instance().create(kName, OperationParameters()));
    expectAbsent(makeWatershed(WatershedParameters().unreachedForeground(WatershedUnreached::Fail)));
}

TEST(Watershed, UnreachedDropRoundTrips) {
    const OperationPtr typed = makeWatershed(WatershedParameters().unreachedForeground(WatershedUnreached::Drop));
    OperationParameters bag;
    bag.setString("unreachedForeground", "Drop");
    const OperationPtr named = OperationCatalog::instance().create(kName, bag);
    EXPECT_EQ(jsonOf(typed->parameters()), jsonOf(named->parameters()));
    EXPECT_EQ(typed->parameters().count(), 7);
    EXPECT_STREQ(named->parameters().getString("unreachedForeground", "absent"), "Drop");

    OperationParameters failBag;
    failBag.setString("unreachedForeground", "Fail");
    EXPECT_EQ(jsonOf(OperationCatalog::instance().create(kName, failBag)->parameters()),
              jsonOf(makeWatershed(WatershedParameters())->parameters()));
}

TEST(Watershed, UnreachedRejected) {
    const auto expectNamed = [](const OperationParameters& bag, const std::string& bad) {
        try {
            OperationCatalog::instance().create(kName, bag);
            ADD_FAILURE() << bad << ": nothing thrown";
        } catch (const InvalidArgumentError& e) {
            const std::string message = e.what();
            EXPECT_NE(message.find("unreachedForeground"), std::string::npos) << message;
            EXPECT_EQ(message.find(bad), std::string::npos) << message;
        }
    };
    for (const char* bad : {"drop", "Skip"}) {
        OperationParameters bag;
        bag.setString("unreachedForeground", bad);
        expectNamed(bag, bad);
    }
    OperationParameters integerBag;
    integerBag.setInt("unreachedForeground", 1);
    expectNamed(integerBag, "1");
}

TEST(Watershed, MarkerFloorRejected) {
    const auto expectNamed = [](const std::function<void()>& make, const std::string& what) {
        try {
            make();
            ADD_FAILURE() << what << ": nothing thrown";
        } catch (const InvalidArgumentError& e) {
            const std::string message = e.what();
            EXPECT_NE(message.find("minimumMarkerValue"), std::string::npos) << what << ": " << message;
        }
    };
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    for (const double bad : {nan, inf, -inf}) {
        expectNamed([bad] { WatershedParameters().minimumMarkerValue(bad); }, "typed non-finite");
        expectNamed(
            [bad] {
                OperationParameters bag;
                bag.setDouble("minimumMarkerValue", bad);
                OperationCatalog::instance().create(kName, bag);
            },
            "named non-finite");
    }
    // A wrong type by name.
    expectNamed(
        [] {
            OperationParameters bag;
            bag.setString("minimumMarkerValue", "2");
            OperationCatalog::instance().create(kName, bag);
        },
        "string");
    // With anchor markers the floor has nothing to apply to; the message
    // names the key and never the value.
    try {
        makeWatershed(WatershedParameters()
                          .markers(WatershedMarkers::AuxiliaryRaster)
                          .auxiliaryName("anchors")
                          .minimumMarkerValue(7.3125));
        ADD_FAILURE() << "nothing thrown";
    } catch (const InvalidArgumentError& e) {
        const std::string message = e.what();
        EXPECT_NE(message.find(kName), std::string::npos) << message;
        EXPECT_NE(message.find("minimumMarkerValue"), std::string::npos) << message;
        EXPECT_EQ(std::count_if(message.begin(), message.end(), [](char c) { return c >= '0' && c <= '9'; }), 0)
            << message;
    }
    OperationParameters bag;
    bag.setString("markers", "AuxiliaryRaster")
        .setString("auxiliaryName", "anchors")
        .setDouble("minimumMarkerValue", 1.5);
    EXPECT_THROW(OperationCatalog::instance().create(kName, bag), InvalidArgumentError);
}

TEST(Watershed, MarkerFloorDropsShallowSeed) {
    // The discs of TouchingDiscsSeparated plus a third of radius 3 far away,
    // whose deepest value is about 3.2.
    constexpr int64_t kWidth = 64;
    constexpr int64_t kHeight = 48;
    const std::vector<Disc> discs = {{24, 24, 10}, {40, 24, 10}, {58, 8, 3}};
    const std::vector<double> dt = distanceTransform(kWidth, kHeight, discs, 12);
    const auto pixel = [&](int32_t, int64_t x, int64_t y) { return dt[static_cast<size_t>(y * kWidth + x)]; };
    const OperationPtr open = makeWatershed(WatershedParameters().maximumMarkerSpacingPx(12));
    const ts::DirectObjects without = ts::applyDirectObjects(*open, {DataType::Float32}, {0}, kWidth, kHeight, pixel);
    EXPECT_EQ(without.objects.size(), 3u);
    // A floor below every centre changes nothing.
    const OperationPtr low = makeWatershed(WatershedParameters().maximumMarkerSpacingPx(12).minimumMarkerValue(3.0));
    EXPECT_EQ(ts::applyDirectObjects(*low, {DataType::Float32}, {0}, kWidth, kHeight, pixel).objects.size(), 3u);
    // At 4 the small disc has no marker; its pixels stay foreground and lie
    // beyond every seed.
    const OperationPtr floored =
        makeWatershed(WatershedParameters().maximumMarkerSpacingPx(12).minimumMarkerValue(4.0));
    // The message names a pixel of the small disc, and the floor among the causes.
    const ReachFailure failure = expectReachError(
        [&] { ts::applyDirectObjects(*floored, {DataType::Float32}, {0}, kWidth, kHeight, pixel); }, "floored",
        Reach::Unreached);
    EXPECT_LE((failure.x - 58) * (failure.x - 58) + (failure.y - 8) * (failure.y - 8), 9) << failure.message;
    EXPECT_NE(failure.message.find("its peak is below minimumMarkerValue"), std::string::npos) << failure.message;
}

TEST(Watershed, MarkerFloorKeepsTiling) {
    // The grid of disc pairs of TilingInvariantAtThreeSizes: every centre is
    // at least 8 deep, so a floor of 4 drops no marker.
    constexpr int64_t kCellWidth = 66;
    constexpr int64_t kCellHeight = 58;
    constexpr int64_t kWidth = 6 * kCellWidth;
    constexpr int64_t kHeight = 5 * kCellHeight;
    std::vector<Disc> discs;
    for (int64_t row = 0; row < 5; ++row) {
        for (int64_t column = 0; column < 6; ++column) {
            const int64_t i = row * 6 + column;
            const int64_t r = 8 + i % 4;
            const double separation = static_cast<double>(r) * (1.3 + 0.1 * static_cast<double>(i % 5));
            const int64_t cx = column * kCellWidth + kCellWidth / 2 + (i % 7) - 3;
            const int64_t cy = row * kCellHeight + kCellHeight / 2 + (i % 5) - 2;
            int64_t dx = 0;
            int64_t dy = 0;
            switch (i % 3) {
            case 0: dx = std::llround(separation / 2.0); break;
            case 1: dy = std::llround(separation / 2.0); break;
            default:
                dx = std::llround(separation / 2.0 / std::sqrt(2.0));
                dy = dx;
                break;
            }
            discs.push_back({cx - dx, cy - dy, r});
            discs.push_back({cx + dx, cy + dy, r});
        }
    }
    auto dt = std::make_shared<std::vector<double>>(distanceTransform(kWidth, kHeight, discs, 14));
    const auto scene = sceneOf(kWidth, kHeight, {slideio::DataType::DT_Float32},
                               [dt](int, int64_t x, int64_t y) { return (*dt)[static_cast<size_t>(y * kWidth + x)]; });
    const std::vector<ObjectRecord> reference = ts::runEagerObjects(scene, specOf(WatershedParameters()), 512, 4);
    ASSERT_EQ(reference.size(), discs.size());
    const PipelineSpec floored = specOf(WatershedParameters().minimumMarkerValue(4.0));
    for (const int32_t tile : {64, 100, 257}) {
        expectSameObjects(reference, ts::runEagerObjects(scene, floored, tile, 4),
                          std::to_string(tile) + "-pixel tiles");
    }
}

TEST(Watershed, MeasuresTheScenesSamplesAfterEarlierOperations) {
    // The touching discs, with (7 x + 13 y) mod 31 in plane 1; the chain blurs both
    // planes before flooding, so plane 1 reaches the operation smoothed. The
    // blur's halo makes the operation's input rect smaller than the rect read
    // from the scene.
    constexpr int64_t kWidth = 64;
    constexpr int64_t kHeight = 48;
    const std::vector<double> dt = distanceTransform(kWidth, kHeight, {{24, 24, 10}, {40, 24, 10}}, 12);
    const auto pixel = [&dt](int channel, int64_t x, int64_t y) {
        return channel == 0 ? dt[static_cast<size_t>(y * kWidth + x)] : static_cast<double>((7 * x + 13 * y) % 31);
    };
    const auto scene = sceneOf(kWidth, kHeight, {slideio::DataType::DT_Float32, slideio::DataType::DT_Float32}, pixel);
    PipelineSpec spec;
    spec.then(makeGaussianBlur(1.0, 0)).then(makeWatershed(WatershedParameters()));

    // Whether the centre of pixel (x, y) lies inside @p polygon, by crossings
    // of the ray to its right over every ring; vertices lie on pixel corners.
    const auto covers = [](const Polygon& polygon, int64_t x, int64_t y) {
        const double px = static_cast<double>(x) + 0.5;
        const double py = static_cast<double>(y) + 0.5;
        bool inside = false;
        for (int32_t r = 0; r < polygon.ringCount(); ++r) {
            const std::vector<Point64> ring = ringOf(polygon, r);
            for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
                const double ay = static_cast<double>(ring[i].y);
                const double by = static_cast<double>(ring[j].y);
                if ((ay > py) != (by > py)) {
                    const double cx = static_cast<double>(ring[i].x) +
                                      (py - ay) * static_cast<double>(ring[j].x - ring[i].x) / (by - ay);
                    inside = px < cx ? !inside : inside;
                }
            }
        }
        return inside;
    };
    for (const int32_t tile : {64, 32}) {
        const std::vector<ObjectRecord> objects = ts::runEagerObjects(scene, spec, tile, 4);
        ASSERT_EQ(objects.size(), 2u) << tile;
        for (const ObjectRecord& object : objects) {
            // The statistics of plane 1 as read, over the object's pixels.
            int64_t count = 0;
            double sum = 0.0;
            for (int64_t y = 0; y < kHeight; ++y) {
                for (int64_t x = 0; x < kWidth; ++x) {
                    if (covers(object.geometry(), x, y)) {
                        ++count;
                        sum += pixel(1, x, y);
                    }
                }
            }
            // The samples are integers, so every sum is exact.
            EXPECT_EQ(measurement(object, "area"), static_cast<double>(count)) << tile;
            EXPECT_EQ(measurement(object, "intensity.1.mean"), sum / static_cast<double>(count)) << tile;
        }
    }
}

TEST(Watershed, AreaFilterAndMeasuredPlanes) {
    // A small basin below the area bound is dropped; at most three measured
    // planes.
    const auto pixel = [](int32_t, int64_t x, int64_t y) {
        if (x >= 4 && x < 8 && y >= 4 && y < 8) {
            return 2.0;
        }
        return x >= 20 && x < 22 && y >= 20 && y < 22 ? 2.0 : 0.0;
    };
    const OperationPtr op = makeWatershed(WatershedParameters().minimumAreaPx(5));
    const ts::DirectObjects result = ts::applyDirectObjects(*op, {DataType::Byte}, {0}, 30, 30, pixel);
    ASSERT_EQ(result.objects.size(), 1u);
    EXPECT_EQ(measurement(result.objects[0], "area"), 16.0);
    EXPECT_EQ(op->outputPlaneCount(4), 1);
    EXPECT_THROW(op->outputPlaneCount(5), InvalidArgumentError);
    EXPECT_EQ(OperationCatalog::instance().outputPlaneCount(kName, 4), 1);
    EXPECT_THROW(OperationCatalog::instance().outputPlaneCount(kName, 5), InvalidArgumentError);
}
