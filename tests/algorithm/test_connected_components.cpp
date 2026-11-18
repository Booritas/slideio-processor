// Requirements: UR-SEG-2, UR-SEG-3, UR-SEG-4, UR-SEG-5, UR-SEG-7
#include <slideio/processor/algorithm/measure/measure.hpp>
#include <slideio/processor/algorithm/segment/segment.hpp>
#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/control.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/measurement.hpp>
#include <slideio/processor/core/object_record.hpp>
#include <slideio/processor/pipeline/operation_catalog.hpp>
#include <slideio/processor/pipeline/pipeline.hpp>
#include <slideio/processor/pipeline/pipeline_spec.hpp>

#include <algorithm/support/harness.hpp>
#include <pipeline/detail/pipeline_impl.hpp>
#include <pipeline/test_support/blob_scene.hpp>
#include <pipeline/test_support/synthetic_scene.hpp>
#include <pipeline/test_support/test_operations.hpp>

#include <slideio/slideio/scene.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using namespace slideio::processor;
namespace ts = slideio::processor::testing_support;

namespace {

constexpr const char* kName = "segment.connected_components";

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
// rings point for point and every measurement, exactly except the intensity
// statistics of @p relativeChannel, which may differ by 1e-12 relative.
void expectSameObjects(const std::vector<ObjectRecord>& reference, const std::vector<ObjectRecord>& objects,
                       const std::string& label, int32_t relativeChannel = -1) {
    ASSERT_EQ(objects.size(), reference.size()) << label;
    const std::string relativePrefix = "intensity." + std::to_string(relativeChannel) + ".";
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
            const double value = measurement(got, m.name());
            if (relativeChannel >= 0 && std::string(m.name()).rfind(relativePrefix, 0) == 0) {
                EXPECT_LE(std::fabs(value - m.value()), 1e-12 * std::fabs(m.value()))
                    << label << ", object " << i << ": " << m.name();
            } else {
                EXPECT_EQ(value, m.value()) << label << ", object " << i << ": " << m.name();
            }
        }
    }
}

const ObjectRecord* findByBox(const std::vector<ObjectRecord>& objects, const Rect64& box) {
    for (const ObjectRecord& r : objects) {
        if (r.boundingBox() == box) {
            return &r;
        }
    }
    return nullptr;
}

std::string jsonOf(const OperationParameters& parameters) {
    std::string text(static_cast<size_t>(parameters.toJson(nullptr, 0)) + 1, '\0');
    parameters.toJson(&text[0], static_cast<int32_t>(text.size()));
    text.resize(text.size() - 1);
    return text;
}

PipelineSpec specOf(const ConnectedComponentsParameters& parameters) {
    PipelineSpec spec;
    spec.then(makeConnectedComponents(parameters));
    return spec;
}

// A run as ts::runEagerObjects makes it, also reporting the objects the
// finishing step discarded.
std::vector<ObjectRecord> runCounted(const std::shared_ptr<slideio::Scene>& scene, const PipelineSpec& spec,
                                     int32_t tileSize, int64_t& droppedByFilter) {
    const Configuration config =
        ConfigurationBuilder().tileSize(tileSize, tileSize).tileSizeFromSlide(false).workerCount(4).build();
    Pipeline pipeline(scene, config, spec);
    pipeline.bindLayer("objects", ts::objectsLayer());
    ts::CollectingObjectSink sink;
    pipeline.runInto(sink, nullptr, CancellationToken());
    droppedByFilter = detail::PipelineAccess::impl(pipeline).counters.droppedByFilter.load();
    std::vector<ObjectRecord> records = sink.records();
    std::sort(records.begin(), records.end(),
              [](const ObjectRecord& a, const ObjectRecord& b) { return a.id() < b.id(); });
    return records;
}

std::shared_ptr<slideio::Scene> sceneOf(int64_t width, int64_t height, std::vector<slideio::DataType> types,
                                        ts::PixelFunction pixel) {
    return ts::makeSyntheticScene({{width, height, 0, 0}}, std::move(types), std::move(pixel));
}

// The blob layout of makeBlobScene with three channels: the Byte mask, the
// mask again as a Byte intensity, and the Float32 intensity.
std::shared_ptr<slideio::Scene> threeChannelBlobScene() {
    constexpr int64_t kWidth = 1000;
    constexpr int64_t kHeight = 800;
    auto mask = std::make_shared<std::vector<uint8_t>>(ts::blobMask(kWidth, kHeight));
    return sceneOf(kWidth, kHeight,
                   {slideio::DataType::DT_Byte, slideio::DataType::DT_Byte, slideio::DataType::DT_Float32},
                   [mask](int channel, int64_t x, int64_t y) {
                       if (channel == 2) {
                           return ts::blobIntensity(x, y);
                       }
                       return static_cast<double>((*mask)[static_cast<size_t>(y * kWidth + x)]);
                   });
}

} // namespace

TEST(ConnectedComponents, KnownObjectsCountAndShape) {
    // Five rectangles and one 8 x 8 ring around a 4 x 4 hole, none touching
    // another or the tile's edge.
    const Rect64 rectangles[] = {{2, 2, 3, 4}, {8, 2, 5, 1}, {15, 2, 2, 2}, {2, 10, 6, 3}, {10, 10, 1, 1}};
    const Rect64 ring{20, 10, 8, 8};
    const Rect64 hole{22, 12, 4, 4};
    const auto inside = [](const Rect64& r, int64_t x, int64_t y) {
        return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
    };
    auto pixel = [&](int32_t, int64_t x, int64_t y) {
        for (const Rect64& r : rectangles) {
            if (inside(r, x, y)) {
                return 1.0;
            }
        }
        return inside(ring, x, y) && !inside(hole, x, y) ? 1.0 : 0.0;
    };
    for (const Connectivity connectivity : {Connectivity::FourConnected, Connectivity::EightConnected}) {
        const OperationPtr op = makeConnectedComponents(ConnectedComponentsParameters().connectivity(connectivity));
        const ts::DirectObjects result = ts::applyDirectObjects(*op, {DataType::Byte}, {0}, 32, 24, pixel);
        ASSERT_EQ(result.objects.size(), 6u);
        EXPECT_TRUE(result.fragments.empty());
        for (const Rect64& r : rectangles) {
            const ObjectRecord* object = findByBox(result.objects, r);
            ASSERT_NE(object, nullptr);
            EXPECT_EQ(object->layer(), ts::objectsLayer());
            EXPECT_EQ(object->geometry().ringCount(), 1);
            EXPECT_EQ(measurement(*object, "area"), static_cast<double>(r.width * r.height));
        }
        const ObjectRecord* annulus = findByBox(result.objects, ring);
        ASSERT_NE(annulus, nullptr);
        ASSERT_EQ(annulus->geometry().ringCount(), 2);
        EXPECT_FALSE(annulus->geometry().ringIsHole(0));
        EXPECT_TRUE(annulus->geometry().ringIsHole(1));
        const std::vector<Point64> holeRing = ringOf(annulus->geometry(), 1);
        ASSERT_EQ(holeRing.size(), 4u);
        for (const Point64& p : holeRing) {
            EXPECT_TRUE((p.x == 22 || p.x == 26) && (p.y == 12 || p.y == 16));
        }
        EXPECT_EQ(measurement(*annulus, "area"), 48.0);
        EXPECT_EQ(measurement(*annulus, "perimeter"), 48.0);
    }

    // The raster: each object's 1-based index in the order its first pixel is
    // met row by row, 0 elsewhere.
    const OperationPtr op = makeConnectedComponents(ConnectedComponentsParameters());
    const ts::Image labels = ts::applyDirect(*op, {DataType::Byte}, {0}, 32, 24, pixel);
    ASSERT_EQ(labels.types, std::vector<DataType>{DataType::Int32});
    EXPECT_EQ(labels.at(0, 2, 2), 1.0);
    EXPECT_EQ(labels.at(0, 12, 2), 2.0);
    EXPECT_EQ(labels.at(0, 16, 3), 3.0);
    EXPECT_EQ(labels.at(0, 7, 12), 4.0);
    EXPECT_EQ(labels.at(0, 10, 10), 5.0);
    EXPECT_EQ(labels.at(0, 27, 17), 6.0);
    EXPECT_EQ(labels.at(0, 23, 13), 0.0);
    EXPECT_EQ(labels.at(0, 0, 0), 0.0);
}

TEST(ConnectedComponents, DiagonalNeighboursFollowTheConnectivity) {
    // Two pixels touching at a corner, away from the edge.
    auto pixel = [](int32_t, int64_t x, int64_t y) { return (x == 3 && y == 3) || (x == 4 && y == 4) ? 1.0 : 0.0; };
    const OperationPtr four =
        makeConnectedComponents(ConnectedComponentsParameters().connectivity(Connectivity::FourConnected));
    const OperationPtr eight = makeConnectedComponents(ConnectedComponentsParameters());
    EXPECT_FALSE(four->objectsAreEightConnected());
    EXPECT_TRUE(eight->objectsAreEightConnected());
    EXPECT_EQ(ts::applyDirectObjects(*four, {DataType::Byte}, {0}, 8, 8, pixel).objects.size(), 2u);
    const ts::DirectObjects joined = ts::applyDirectObjects(*eight, {DataType::Byte}, {0}, 8, 8, pixel);
    ASSERT_EQ(joined.objects.size(), 1u);
    EXPECT_EQ(measurement(joined.objects[0], "area"), 2.0);
    EXPECT_EQ(joined.objects[0].geometry().ringCount(), 1);
}

TEST(ConnectedComponents, NaNAndZeroAreBackground) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    // Row 2: 1, NaN, 1 splits into two objects; -0.5 at (5, 5) is foreground.
    auto pixel = [nan](int32_t, int64_t x, int64_t y) {
        if (y == 2 && x >= 2 && x <= 4) {
            return x == 3 ? nan : 1.0;
        }
        return x == 5 && y == 5 ? -0.5 : 0.0;
    };
    const OperationPtr op = makeConnectedComponents(ConnectedComponentsParameters());
    for (const DataType type : {DataType::Float32, DataType::Float64}) {
        EXPECT_EQ(ts::applyDirectObjects(*op, {type}, {0}, 8, 8, pixel).objects.size(), 3u) << dataTypeName(type);
    }
}

TEST(ConnectedComponents, MeasurementsAnalytic) {
    // A 10 x 6 rectangle at (5, 4), intensity 7 on plane 1 (source channel 3).
    auto pixel = [](int32_t plane, int64_t x, int64_t y) {
        const bool inside = x >= 5 && x < 15 && y >= 4 && y < 10;
        if (plane == 0) {
            return inside ? 1.0 : 0.0;
        }
        return inside ? 7.0 : 200.0;
    };
    const OperationPtr op = makeConnectedComponents(ConnectedComponentsParameters());
    const ts::DirectObjects result =
        ts::applyDirectObjects(*op, {DataType::Byte, DataType::Byte}, {0, 3}, 24, 16, pixel, 0.5);
    ASSERT_EQ(result.objects.size(), 1u);
    const ObjectRecord& r = result.objects[0];
    EXPECT_EQ(measurement(r, "area"), 60.0);
    EXPECT_EQ(measurement(r, "perimeter"), 32.0);
    EXPECT_EQ(measurement(r, "centroid.x"), 10.0);
    EXPECT_EQ(measurement(r, "centroid.y"), 7.0);
    EXPECT_EQ(measurement(r, "boundingBox.x"), 5.0);
    EXPECT_EQ(measurement(r, "boundingBox.y"), 4.0);
    EXPECT_EQ(measurement(r, "boundingBox.width"), 10.0);
    EXPECT_EQ(measurement(r, "boundingBox.height"), 6.0);
    EXPECT_EQ(measurement(r, "intensity.3.mean"), 7.0);
    EXPECT_EQ(measurement(r, "intensity.3.stddev"), 0.0);
    EXPECT_EQ(measurement(r, "intensity.3.min"), 7.0);
    EXPECT_EQ(measurement(r, "intensity.3.max"), 7.0);
    EXPECT_EQ(measurement(r, "area.um2"), 15.0);
    EXPECT_EQ(measurement(r, "perimeter.um"), 16.0);
    EXPECT_EQ(measurement(r, "centroid.x.um"), 5.0);
    EXPECT_EQ(measurement(r, "centroid.y.um"), 3.5);
    EXPECT_EQ(measurement(r, "boundingBox.x.um"), 2.5);
    EXPECT_EQ(measurement(r, "boundingBox.y.um"), 2.0);
    EXPECT_EQ(measurement(r, "boundingBox.width.um"), 5.0);
    EXPECT_EQ(measurement(r, "boundingBox.height.um"), 3.0);
    EXPECT_FALSE(has(r, "intensity.0.mean"));
    EXPECT_EQ(r.measurements().count(), 20);

    // Only the selected kinds are kept, whichever path made the operation.
    OperationParameters bag;
    bag.setString("measurements", "Perimeter,IntensityMax");
    const OperationPtr named = OperationCatalog::instance().create(kName, bag);
    const ts::DirectObjects selected =
        ts::applyDirectObjects(*named, {DataType::Byte, DataType::Byte}, {0, 3}, 24, 16, pixel, 0.5);
    ASSERT_EQ(selected.objects.size(), 1u);
    EXPECT_EQ(selected.objects[0].measurements().count(), 3);
    EXPECT_EQ(measurement(selected.objects[0], "perimeter"), 32.0);
    EXPECT_EQ(measurement(selected.objects[0], "perimeter.um"), 16.0);
    EXPECT_EQ(measurement(selected.objects[0], "intensity.3.max"), 7.0);
}

TEST(ConnectedComponents, TypedAndNamedBagsAreEqual) {
    const OperationPtr typed = makeConnectedComponents(ConnectedComponentsParameters()
                                                           .connectivity(Connectivity::FourConnected)
                                                           .minimumAreaPx(4)
                                                           .maximumAreaPx(400)
                                                           .extractHoles(false)
                                                           .measurements(MeasurementSelection()
                                                                             .add(MeasurementKind::IntensityMin)
                                                                             .add(MeasurementKind::Area))
                                                           .targetLayer("nuclei"));
    OperationParameters bag;
    bag.setString("connectivity", "FourConnected")
        .setInt("minimumAreaPx", 4)
        .setInt("maximumAreaPx", 400)
        .setBool("extractHoles", false)
        .setString("measurements", "IntensityMin,Area,Area")
        .setString("targetLayer", "nuclei");
    const OperationPtr named = OperationCatalog::instance().create(kName, bag);
    EXPECT_EQ(jsonOf(typed->parameters()), jsonOf(named->parameters()));
    EXPECT_STREQ(typed->parameters().getString("measurements", ""), "Area,IntensityMin");

    // The defaults: complete except for the target layer, which is left out.
    const OperationPtr defaults = makeConnectedComponents(ConnectedComponentsParameters());
    EXPECT_EQ(jsonOf(defaults->parameters()),
              jsonOf(OperationCatalog::instance().create(kName, OperationParameters())->parameters()));
    EXPECT_EQ(defaults->parameters().count(), 5);
    EXPECT_STREQ(defaults->parameters().getString("connectivity", ""), "EightConnected");
    EXPECT_STREQ(defaults->parameters().getString("measurements", ""),
                 "Area,Perimeter,Centroid,BoundingBox,IntensityMean,IntensityStdDev,IntensityMin,IntensityMax");
    EXPECT_STREQ(defaults->parameters().getString("targetLayer", "absent"), "absent");
    const OperationPtr cleared =
        makeConnectedComponents(ConnectedComponentsParameters().targetLayer("nuclei").targetLayer(""));
    EXPECT_EQ(jsonOf(cleared->parameters()), jsonOf(defaults->parameters()));

    // An empty selection keeps no measurement.
    const OperationPtr none = makeConnectedComponents(ConnectedComponentsParameters().measurements(MeasurementSelection()));
    EXPECT_STREQ(none->parameters().getString("measurements", "absent"), "");
    OperationParameters empty;
    empty.setString("measurements", "");
    EXPECT_EQ(jsonOf(OperationCatalog::instance().create(kName, empty)->parameters()), jsonOf(none->parameters()));
}

TEST(ConnectedComponents, RejectsBadParameters) {
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
    expectInvalid([] { makeConnectedComponents(ConnectedComponentsParameters().minimumAreaPx(-1)); },
                  "minimumAreaPx", "negative minimum");
    expectInvalid([] { makeConnectedComponents(ConnectedComponentsParameters().maximumAreaPx(-1)); },
                  "maximumAreaPx", "negative maximum");
    expectInvalid(
        [] { makeConnectedComponents(ConnectedComponentsParameters().minimumAreaPx(10).maximumAreaPx(9)); },
        "maximumAreaPx", "maximum below minimum");
    expectInvalid(
        [] { makeConnectedComponents(ConnectedComponentsParameters().connectivity(static_cast<Connectivity>(7))); },
        "connectivity", "connectivity not an enumerator");
    // A maximum of 0 is unbounded, whatever the minimum.
    EXPECT_NO_THROW(makeConnectedComponents(ConnectedComponentsParameters().minimumAreaPx(10).maximumAreaPx(0)));
    EXPECT_NO_THROW(makeConnectedComponents(ConnectedComponentsParameters().minimumAreaPx(10).maximumAreaPx(10)));
    EXPECT_THROW(ConnectedComponentsParameters().targetLayer(nullptr), InvalidArgumentError);

    const auto byName = [](const std::function<void(OperationParameters&)>& fill) {
        return [fill] {
            OperationParameters bag;
            fill(bag);
            OperationCatalog::instance().create(kName, bag);
        };
    };
    expectInvalid(byName([](OperationParameters& b) { b.setInt("minimumAreaPx", -5); }), "minimumAreaPx",
                  "negative minimum by name");
    expectInvalid(byName([](OperationParameters& b) { b.setInt("maximumAreaPx", -5); }), "maximumAreaPx",
                  "negative maximum by name");
    expectInvalid(byName([](OperationParameters& b) { b.setInt("minimumAreaPx", 8).setInt("maximumAreaPx", 7); }),
                  "maximumAreaPx", "maximum below minimum by name");
    expectInvalid(byName([](OperationParameters& b) { b.setString("connectivity", "SixConnected"); }),
                  "connectivity", "unknown connectivity");
    expectInvalid(byName([](OperationParameters& b) { b.setDouble("minimumAreaPx", 4.0); }), "minimumAreaPx",
                  "area of the wrong type");
    expectInvalid(byName([](OperationParameters& b) { b.setString("measurements", "Area,Bogus"); }),
                  "measurements", "unknown measurement");
    expectInvalid(byName([](OperationParameters& b) { b.setString("measurements", "Area, Perimeter"); }),
                  "measurements", "measurement with a space");
    expectInvalid(byName([](OperationParameters& b) { b.setString("measurements", "Area,,Perimeter"); }),
                  "measurements", "empty measurement name");
    expectInvalid(byName([](OperationParameters& b) { b.setBool("measurements", true); }), "measurements",
                  "measurements of the wrong type");
    expectInvalid(byName([](OperationParameters& b) { b.setInt("minimumArea", 4); }), "minimumArea",
                  "unknown key");
    try {
        OperationParameters bag;
        bag.setString("measurements", "Area,Bogus");
        OperationCatalog::instance().create(kName, bag);
        ADD_FAILURE() << "nothing thrown";
    } catch (const InvalidArgumentError& e) {
        EXPECT_EQ(std::string(e.what()).find("Bogus"), std::string::npos) << e.what();
    }
}

TEST(ConnectedComponents, TilingInvariantAtThreeSizes) {
    struct Case {
        const char* label;
        std::shared_ptr<slideio::Scene> scene;
        int32_t floatChannel;
    };
    const Case cases[] = {
        {"blob scene, Byte mask and Float32 values",
         ts::makeBlobScene({{1000, 800, 0, 0}}, 0, ts::BlobChannels::MaskAndFloatValues), 1},
        {"blob layout, Byte mask, Byte and Float32 values", threeChannelBlobScene(), 2},
    };
    for (const Case& c : cases) {
        for (const Connectivity connectivity : {Connectivity::EightConnected, Connectivity::FourConnected}) {
            const PipelineSpec spec = specOf(ConnectedComponentsParameters().connectivity(connectivity));
            const std::string label = std::string(c.label) +
                                      (connectivity == Connectivity::EightConnected ? ", 8-connected" : ", 4-connected");
            const std::vector<ObjectRecord> reference = ts::runEagerObjects(c.scene, spec, 1000, 4);
            ASSERT_GT(reference.size(), 15u) << label;
            // The region spanning the level has four holes; the others hold intensity.
            EXPECT_EQ(std::count_if(reference.begin(), reference.end(),
                                    [](const ObjectRecord& r) {
                                        return r.boundingBox() == Rect64{0, 0, 1000, 800} &&
                                               r.geometry().ringCount() == 5;
                                    }),
                      1)
                << label;
            const std::string floatMean = "intensity." + std::to_string(c.floatChannel) + ".mean";
            for (const ObjectRecord& r : reference) {
                ASSERT_TRUE(has(r, floatMean.c_str())) << label;
                ASSERT_TRUE(has(r, "centroid.x")) << label;
            }
            for (const int32_t tile : {64, 100, 257}) {
                const auto cut = std::count_if(reference.begin(), reference.end(), [tile](const ObjectRecord& r) {
                    const Rect64 b = r.boundingBox();
                    return b.x / tile != (b.x + b.width - 1) / tile || b.y / tile != (b.y + b.height - 1) / tile;
                });
                ASSERT_GE(cut, 5) << label << ": " << tile << "-pixel seams should cut many objects";
                expectSameObjects(reference, ts::runEagerObjects(c.scene, spec, tile, 4),
                                  label + ", " + std::to_string(tile) + "-pixel tiles", c.floatChannel);
            }
        }
    }
}

TEST(ConnectedComponents, ObjectAcrossFourTilesWithSplitHole) {
    // A 40 x 40 square at (30, 30) around a 10 x 10 hole at (45, 45): tiles of
    // 48, 50 and 52 pixels cut both the square and the hole with both seams.
    auto pixel = [](int channel, int64_t x, int64_t y) {
        const bool square = x >= 30 && x < 70 && y >= 30 && y < 70;
        const bool hole = x >= 45 && x < 55 && y >= 45 && y < 55;
        if (channel == 0) {
            return square && !hole ? 255.0 : 0.0;
        }
        if (channel == 1) {
            return static_cast<double>((x * 7 + y * 3) % 251);
        }
        return 0.1 * static_cast<double>(x) + 0.37 * static_cast<double>(y);
    };
    const auto scene = sceneOf(
        100, 100, {slideio::DataType::DT_Byte, slideio::DataType::DT_Byte, slideio::DataType::DT_Float32}, pixel);
    const PipelineSpec spec = specOf(ConnectedComponentsParameters());
    const std::vector<ObjectRecord> reference = ts::runEagerObjects(scene, spec, 100, 2);
    ASSERT_EQ(reference.size(), 1u);
    const Polygon& g = reference[0].geometry();
    ASSERT_EQ(g.ringCount(), 2);
    EXPECT_TRUE(g.ringIsHole(1));
    EXPECT_EQ(g.boundingBox(), (Rect64{30, 30, 40, 40}));
    EXPECT_EQ(measurement(reference[0], "area"), 1500.0);
    EXPECT_EQ(measurement(reference[0], "perimeter"), 200.0);
    EXPECT_EQ(measurement(reference[0], "centroid.x"), 50.0);
    EXPECT_EQ(measurement(reference[0], "centroid.y"), 50.0);
    for (const int32_t tile : {48, 50, 52}) {
        expectSameObjects(reference, ts::runEagerObjects(scene, spec, tile, 4),
                          std::to_string(tile) + "-pixel tiles", 2);
    }
}

TEST(ConnectedComponents, DiagonalChainAcrossCornerIsOneObject) {
    // A diagonal chain through the corner (50, 50) of the 50- and 25-pixel
    // grids, and an anti-diagonal one through (50, 100).
    auto pixel = [](int, int64_t x, int64_t y) {
        const bool diagonal = x >= 40 && x < 60 && y == x;
        const bool antiDiagonal = x >= 40 && x < 60 && y == 149 - x;
        return diagonal || antiDiagonal ? 1.0 : 0.0;
    };
    const auto scene = sceneOf(100, 150, {slideio::DataType::DT_Byte}, pixel);

    const PipelineSpec eight = specOf(ConnectedComponentsParameters());
    const std::vector<ObjectRecord> reference = ts::runEagerObjects(scene, eight, 150, 2);
    ASSERT_EQ(reference.size(), 2u);
    for (const ObjectRecord& r : reference) {
        EXPECT_EQ(measurement(r, "area"), 20.0);
        EXPECT_EQ(r.geometry().ringCount(), 1);
        EXPECT_EQ(r.boundingBox().width, 20);
        EXPECT_EQ(r.boundingBox().height, 20);
    }
    for (const int32_t tile : {25, 50}) {
        expectSameObjects(reference, ts::runEagerObjects(scene, eight, tile, 4), std::to_string(tile) + "-pixel tiles");
    }

    // 4-connected, every pixel of the chains is its own object.
    const PipelineSpec four = specOf(ConnectedComponentsParameters().connectivity(Connectivity::FourConnected));
    for (const int32_t tile : {25, 50, 150}) {
        EXPECT_EQ(ts::runEagerObjects(scene, four, tile, 4).size(), 40u) << tile;
    }
}

TEST(ConnectedComponents, AreaFilterAppliesToMergedObjects) {
    // A 20 x 20 square cut into four 10 x 10 fragments by the seams of
    // 50-pixel tiles, and a 12 x 12 square inside one tile.
    auto pixel = [](int, int64_t x, int64_t y) {
        const bool cut = x >= 40 && x < 60 && y >= 40 && y < 60;
        const bool whole = x >= 5 && x < 17 && y >= 5 && y < 17;
        return cut || whole ? 1.0 : 0.0;
    };
    const auto scene = sceneOf(100, 100, {slideio::DataType::DT_Byte}, pixel);
    for (const int32_t tile : {50, 100}) {
        int64_t dropped = -1;
        // Every fragment is below the minimum; the merged object is not, the
        // whole square is.
        std::vector<ObjectRecord> kept = runCounted(scene, specOf(ConnectedComponentsParameters().minimumAreaPx(150)),
                                                    tile, dropped);
        ASSERT_EQ(kept.size(), 1u) << tile;
        EXPECT_EQ(measurement(kept[0], "area"), 400.0) << tile;
        EXPECT_EQ(dropped, 1) << tile;
        // Every fragment is within the maximum; the merged object is not.
        kept = runCounted(scene, specOf(ConnectedComponentsParameters().maximumAreaPx(300)), tile, dropped);
        ASSERT_EQ(kept.size(), 1u) << tile;
        EXPECT_EQ(measurement(kept[0], "area"), 144.0) << tile;
        EXPECT_EQ(dropped, 1) << tile << ": the merged object is counted as filtered";
    }
}

TEST(ConnectedComponents, NaNIntensitySamplesAreSkipped) {
    // A 40 x 40 square at (20, 20), cut by the seams of 32- and 50-pixel tiles;
    // its intensity is x + 100 y, NaN where (x + y) % 7 == 0, NaN samples on the
    // seams included.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const auto inSquare = [](int64_t x, int64_t y) { return x >= 20 && x < 60 && y >= 20 && y < 60; };
    auto pixel = [=](int channel, int64_t x, int64_t y) {
        if (channel == 0) {
            return inSquare(x, y) ? 1.0 : 0.0;
        }
        return (x + y) % 7 == 0 ? nan : static_cast<double>(x + 100 * y);
    };
    const auto scene = sceneOf(100, 100, {slideio::DataType::DT_Byte, slideio::DataType::DT_Float32}, pixel);
    int64_t count = 0;
    double sum = 0.0;
    double sumOfSquares = 0.0;
    double minimum = std::numeric_limits<double>::infinity();
    double maximum = -minimum;
    for (int64_t y = 20; y < 60; ++y) {
        for (int64_t x = 20; x < 60; ++x) {
            const double v = pixel(1, x, y);
            if (std::isnan(v)) {
                continue;
            }
            ++count;
            sum += v;
            sumOfSquares += v * v;
            minimum = std::min(minimum, v);
            maximum = std::max(maximum, v);
        }
    }
    ASSERT_LT(count, 1600);
    const double mean = sum / static_cast<double>(count);
    const double stddev = std::sqrt(sumOfSquares / static_cast<double>(count) - mean * mean);

    const PipelineSpec spec = specOf(ConnectedComponentsParameters());
    const std::vector<ObjectRecord> reference = ts::runEagerObjects(scene, spec, 100, 2);
    ASSERT_EQ(reference.size(), 1u);
    EXPECT_EQ(measurement(reference[0], "area"), 1600.0);
    EXPECT_EQ(measurement(reference[0], "intensity.1.mean"), mean);
    EXPECT_EQ(measurement(reference[0], "intensity.1.min"), minimum);
    EXPECT_EQ(measurement(reference[0], "intensity.1.max"), maximum);
    EXPECT_NEAR(measurement(reference[0], "intensity.1.stddev"), stddev, 1e-9 * stddev);
    // The samples are integers, so every sum is exact and the tilings agree bit for bit.
    for (const int32_t tile : {32, 50}) {
        expectSameObjects(reference, ts::runEagerObjects(scene, spec, tile, 4), std::to_string(tile) + "-pixel tiles");
    }
}

TEST(ConnectedComponents, FilteredRunStoresFewerObjects) {
    // One-pixel artefacts every 6 pixels, and four 8 x 8 squares, on a level
    // cut by 64-pixel seams.
    const Rect64 squares[] = {{20, 20, 8, 8}, {60, 60, 8, 8}, {130, 10, 8, 8}, {100, 150, 8, 8}};
    auto inSquare = [squares](int64_t x, int64_t y) {
        for (const Rect64& s : squares) {
            if (x >= s.x - 2 && x < s.x + s.width + 2 && y >= s.y - 2 && y < s.y + s.height + 2) {
                return x >= s.x && x < s.x + s.width && y >= s.y && y < s.y + s.height ? 2 : 1;
            }
        }
        return 0;
    };
    auto pixel = [inSquare](int, int64_t x, int64_t y) {
        const int s = inSquare(x, y);
        if (s != 0) {
            return s == 2 ? 1.0 : 0.0;
        }
        return x % 6 == 3 && y % 6 == 3 ? 1.0 : 0.0;
    };
    const auto scene = sceneOf(200, 200, {slideio::DataType::DT_Byte}, pixel);
    int64_t dropped = -1;
    const std::vector<ObjectRecord> all = runCounted(scene, specOf(ConnectedComponentsParameters()), 64, dropped);
    EXPECT_EQ(dropped, 0);
    const auto artefacts = std::count_if(all.begin(), all.end(),
                                         [](const ObjectRecord& r) { return measurement(r, "area") == 1.0; });
    ASSERT_GT(artefacts, 900);
    EXPECT_EQ(all.size(), static_cast<size_t>(artefacts) + 4u);

    const std::vector<ObjectRecord> filtered =
        runCounted(scene, specOf(ConnectedComponentsParameters().minimumAreaPx(4)), 64, dropped);
    ASSERT_EQ(filtered.size(), 4u);
    for (const ObjectRecord& r : filtered) {
        EXPECT_EQ(measurement(r, "area"), 64.0);
    }
    EXPECT_EQ(dropped, artefacts);
}

TEST(ConnectedComponents, ExtractHolesFalseKeepsTheOuterRing) {
    auto pixel = [](int32_t, int64_t x, int64_t y) {
        const bool square = x >= 2 && x < 10 && y >= 2 && y < 10;
        const bool hole = x >= 4 && x < 7 && y >= 4 && y < 7;
        return square && !hole ? 1.0 : 0.0;
    };
    const OperationPtr op = makeConnectedComponents(ConnectedComponentsParameters().extractHoles(false));
    const ts::DirectObjects result = ts::applyDirectObjects(*op, {DataType::Byte}, {0}, 12, 12, pixel);
    ASSERT_EQ(result.objects.size(), 1u);
    EXPECT_EQ(result.objects[0].geometry().ringCount(), 1);
    EXPECT_EQ(measurement(result.objects[0], "area"), 64.0);
    EXPECT_EQ(measurement(result.objects[0], "perimeter"), 32.0);
}

TEST(ConnectedComponents, TargetLayerByName) {
    auto pixel = [](int, int64_t x, int64_t y) { return x >= 4 && x < 8 && y >= 4 && y < 8 ? 1.0 : 0.0; };
    const auto scene = sceneOf(16, 16, {slideio::DataType::DT_Byte}, pixel);
    const Configuration config = ConfigurationBuilder().tileSize(16, 16).tileSizeFromSlide(false).workerCount(1).build();
    const LayerId other{9};
    {
        Pipeline pipeline(scene, config, specOf(ConnectedComponentsParameters().targetLayer("second")));
        pipeline.bindLayer("objects", ts::objectsLayer()).bindLayer("second", other);
        ts::CollectingObjectSink sink;
        pipeline.runInto(sink, nullptr, CancellationToken());
        const std::vector<ObjectRecord> records = sink.records();
        ASSERT_EQ(records.size(), 1u);
        EXPECT_EQ(records[0].layer(), other);
    }
    {
        // Without a name, two bound layers are ambiguous.
        Pipeline pipeline(scene, config, specOf(ConnectedComponentsParameters()));
        pipeline.bindLayer("objects", ts::objectsLayer()).bindLayer("second", other);
        EXPECT_THROW(pipeline.validate(), ProcessorError);
    }
}

TEST(ConnectedComponents, MoreThanThreeMeasuredPlanesRefused) {
    const Configuration config = ConfigurationBuilder().tileSize(16, 16).tileSizeFromSlide(false).workerCount(1).build();
    const auto sceneWith = [](size_t channels) {
        return sceneOf(16, 16, std::vector<slideio::DataType>(channels, slideio::DataType::DT_Byte),
                       [](int, int64_t x, int64_t y) { return x == 4 && y == 4 ? 1.0 : 0.0; });
    };
    const PipelineSpec spec = specOf(ConnectedComponentsParameters());
    {
        // Plane 0 segmented, three measured.
        Pipeline pipeline(sceneWith(4), config, spec);
        pipeline.bindLayer("objects", ts::objectsLayer());
        EXPECT_NO_THROW(pipeline.validate());
    }
    {
        Pipeline pipeline(sceneWith(5), config, spec);
        pipeline.bindLayer("objects", ts::objectsLayer());
        try {
            pipeline.validate();
            ADD_FAILURE() << "five channels validated";
        } catch (const InvalidArgumentError& e) {
            EXPECT_STREQ(e.what(), "segment.connected_components: measures at most three channels besides the one "
                                   "it segments");
        }
    }
    const OperationPtr op = makeConnectedComponents(ConnectedComponentsParameters());
    EXPECT_EQ(op->outputPlaneCount(4), 1);
    EXPECT_THROW(op->outputPlaneCount(5), InvalidArgumentError);
    EXPECT_EQ(OperationCatalog::instance().outputPlaneCount(kName, 4), 1);
    EXPECT_THROW(OperationCatalog::instance().outputPlaneCount(kName, 5), InvalidArgumentError);
}
