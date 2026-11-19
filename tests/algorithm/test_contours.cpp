// Requirements: UR-SEG-2, UR-SEG-3, UR-SEG-4, UR-QA-2, UR-ALG-6
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

constexpr const char* kName = "segment.contours";

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

const ObjectRecord* findByBox(const std::vector<ObjectRecord>& objects, const Rect64& box) {
    for (const ObjectRecord& r : objects) {
        if (r.boundingBox() == box) {
            return &r;
        }
    }
    return nullptr;
}

bool hasVertex(const Polygon& polygon, int32_t ring, Point64 point) {
    const std::vector<Point64> points = ringOf(polygon, ring);
    return std::find(points.begin(), points.end(), point) != points.end();
}

std::string jsonOf(const OperationParameters& parameters) {
    std::string text(static_cast<size_t>(parameters.toJson(nullptr, 0)) + 1, '\0');
    parameters.toJson(&text[0], static_cast<int32_t>(text.size()));
    text.resize(text.size() - 1);
    return text;
}

PipelineSpec specOf(const ContourParameters& parameters) {
    PipelineSpec spec;
    spec.then(makeContourExtraction(parameters));
    return spec;
}

std::shared_ptr<slideio::Scene> sceneOf(int64_t width, int64_t height, slideio::DataType type,
                                        ts::PixelFunction pixel) {
    return ts::makeSyntheticScene({{width, height, 0, 0}}, std::vector<slideio::DataType>{type}, std::move(pixel));
}

int64_t bitsOf(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return static_cast<int64_t>(bits);
}

// The label raster of TilingInvariantAtThreeSizes: cells of 45 x 38 pixels
// taking values 1 to 4, so neighbouring cells often differ and touch across
// seams; zero stripes for concave outlines; a disc of value 5 in every third
// cell, a hole in the region around it; and a single-pixel island of value 6.
double labelRaster(int64_t x, int64_t y) {
    if ((x + 2 * y) % 97 < 5) {
        return 0.0;
    }
    const int64_t dx = x % 45 - 22;
    const int64_t dy = y % 38 - 19;
    if ((x / 45 + y / 38) % 3 == 0 && dx * dx + dy * dy < 7 * 7) {
        return dx == 0 && dy == 0 ? 6.0 : 5.0;
    }
    return static_cast<double>(1 + ((x / 45) * 7 + (y / 38) * 3) % 4);
}

} // namespace

TEST(Contours, LabelRegionsBecomeObjects) {
    // Values 1 and 2 side by side above a strip of 3 touching both; a second,
    // separate region of value 1; and two regions of value 4 touching only at
    // a corner, which are two objects.
    auto pixel = [](int32_t, int64_t x, int64_t y) {
        if (y >= 2 && y < 6 && x >= 2 && x < 6) {
            return 1.0;
        }
        if (y >= 2 && y < 6 && x >= 6 && x < 10) {
            return 2.0;
        }
        if (y >= 6 && y < 8 && x >= 2 && x < 10) {
            return 3.0;
        }
        if (y >= 10 && y < 12 && x >= 2 && x < 4) {
            return 1.0;
        }
        return (x == 12 && y == 10) || (x == 13 && y == 11) ? 4.0 : 0.0;
    };
    const OperationPtr op = makeContourExtraction(ContourParameters());
    EXPECT_EQ(op->arity(), ChannelArity::PerChannel);
    EXPECT_FALSE(op->objectsAreEightConnected());
    const Halo halo = op->halo();
    EXPECT_TRUE(halo.left == 1 && halo.top == 1 && halo.right == 1 && halo.bottom == 1);
    const ts::DirectObjects result = ts::applyDirectObjects(*op, {DataType::Int32}, {0}, 16, 14, pixel);
    EXPECT_TRUE(result.fragments.empty());
    ASSERT_EQ(result.objects.size(), 6u);
    const ObjectRecord* one = findByBox(result.objects, {2, 2, 4, 4});
    const ObjectRecord* two = findByBox(result.objects, {6, 2, 4, 4});
    const ObjectRecord* three = findByBox(result.objects, {2, 6, 8, 2});
    ASSERT_NE(one, nullptr);
    ASSERT_NE(two, nullptr);
    ASSERT_NE(three, nullptr);
    ASSERT_NE(findByBox(result.objects, {2, 10, 2, 2}), nullptr);
    ASSERT_NE(findByBox(result.objects, {12, 10, 1, 1}), nullptr);
    ASSERT_NE(findByBox(result.objects, {13, 11, 1, 1}), nullptr);
    EXPECT_EQ(measurement(*one, "area"), 16.0);
    EXPECT_EQ(measurement(*two, "area"), 16.0);
    EXPECT_EQ(measurement(*three, "area"), 16.0);
    EXPECT_EQ(measurement(*three, "perimeter"), 20.0);
    // Geometry only: area and perimeter, nothing else.
    for (const ObjectRecord& r : result.objects) {
        EXPECT_EQ(r.layer(), ts::objectsLayer());
        EXPECT_EQ(r.geometry().ringCount(), 1);
        EXPECT_EQ(r.measurements().count(), 2);
    }
    // The rings share the edges between the regions.
    for (const Point64 p : {Point64{6, 2}, Point64{6, 6}}) {
        EXPECT_TRUE(hasVertex(one->geometry(), 0, p));
        EXPECT_TRUE(hasVertex(two->geometry(), 0, p));
    }
    for (const Point64 p : {Point64{2, 6}, Point64{10, 6}}) {
        EXPECT_TRUE(hasVertex(three->geometry(), 0, p));
    }
    EXPECT_TRUE(hasVertex(one->geometry(), 0, Point64{2, 6}));
    EXPECT_TRUE(hasVertex(two->geometry(), 0, Point64{10, 6}));

    // The raster: each object's 1-based index in the order its first pixel is
    // met row by row, 0 elsewhere.
    const ts::Image labels = ts::applyDirect(*op, {DataType::Int32}, {0}, 16, 14, pixel);
    ASSERT_EQ(labels.types, std::vector<DataType>{DataType::Int32});
    EXPECT_EQ(labels.at(0, 2, 2), 1.0);
    EXPECT_EQ(labels.at(0, 9, 5), 2.0);
    EXPECT_EQ(labels.at(0, 5, 7), 3.0);
    EXPECT_EQ(labels.at(0, 3, 11), 4.0);
    EXPECT_EQ(labels.at(0, 12, 10), 5.0);
    EXPECT_EQ(labels.at(0, 13, 11), 6.0);
    EXPECT_EQ(labels.at(0, 0, 0), 0.0);

    // Over a run, regions of different values touching across the seams of
    // 50- and 25-pixel tiles stay apart, as in one tile: the seam x = 50 runs
    // between the regions of values 1 and 2, and every region is cut.
    auto seamPixel = [](int, int64_t x, int64_t y) {
        if (y >= 30 && y < 70 && x >= 30 && x < 50) {
            return 1.0;
        }
        if (y >= 30 && y < 70 && x >= 50 && x < 70) {
            return 2.0;
        }
        if (y >= 70 && y < 80 && x >= 30 && x < 70) {
            return 3.0;
        }
        if (y >= 40 && y < 60 && x >= 70 && x < 90) {
            return 1.0;
        }
        return 0.0;
    };
    const auto scene = sceneOf(100, 100, slideio::DataType::DT_Int16, seamPixel);
    const PipelineSpec spec = specOf(ContourParameters());
    const std::vector<ObjectRecord> reference = ts::runEagerObjects(scene, spec, 100, 2);
    ASSERT_EQ(reference.size(), 4u);
    const Rect64 boxes[] = {{30, 30, 20, 40}, {50, 30, 20, 40}, {30, 70, 40, 10}, {70, 40, 20, 20}};
    for (const Rect64& box : boxes) {
        const ObjectRecord* object = findByBox(reference, box);
        ASSERT_NE(object, nullptr);
        EXPECT_EQ(object->geometry().ringCount(), 1);
        EXPECT_EQ(measurement(*object, "area"), static_cast<double>(box.width * box.height));
    }
    for (const int32_t tile : {50, 25}) {
        expectSameObjects(reference, ts::runEagerObjects(scene, spec, tile, 4), std::to_string(tile) + "-pixel tiles");
    }
}

TEST(Contours, FragmentsCarryTheirValue) {
    // Regions touching the tile's edge are fragments keyed by their value.
    auto pixel = [](int32_t, int64_t x, int64_t) { return x < 4 ? -5.0 : 300.0; };
    const OperationPtr op = makeContourExtraction(ContourParameters());
    const ts::DirectObjects result = ts::applyDirectObjects(*op, {DataType::Int16}, {0}, 8, 8, pixel);
    EXPECT_TRUE(result.objects.empty());
    ASSERT_EQ(result.fragmentKeys.size(), 2u);
    EXPECT_EQ(result.fragmentKeys[0], -5);
    EXPECT_EQ(result.fragmentKeys[1], 300);
    const ts::DirectObjects big = ts::applyDirectObjects(
        *op, {DataType::UInt32}, {0}, 8, 8, [](int32_t, int64_t, int64_t) { return 4000000000.0; });
    ASSERT_EQ(big.fragmentKeys.size(), 1u);
    EXPECT_EQ(big.fragmentKeys[0], int64_t{4000000000});
}

TEST(Contours, HoleAsSeparateRing) {
    // An 8 x 8 square of value 7 around a 4 x 4 region of value 9, and a 6 x 6
    // square of value 7 around a background hole.
    auto pixel = [](int32_t, int64_t x, int64_t y) {
        if (x >= 4 && x < 8 && y >= 4 && y < 8) {
            return 9.0;
        }
        if (x >= 2 && x < 10 && y >= 2 && y < 10) {
            return 7.0;
        }
        if (x >= 13 && x < 19 && y >= 2 && y < 8) {
            return x >= 15 && x < 17 && y >= 4 && y < 6 ? 0.0 : 7.0;
        }
        return 0.0;
    };
    const OperationPtr op = makeContourExtraction(ContourParameters());
    const ts::DirectObjects result = ts::applyDirectObjects(*op, {DataType::Byte}, {0}, 22, 12, pixel);
    ASSERT_EQ(result.objects.size(), 3u);
    const ObjectRecord* outer = findByBox(result.objects, {2, 2, 8, 8});
    const ObjectRecord* inner = findByBox(result.objects, {4, 4, 4, 4});
    const ObjectRecord* ringed = findByBox(result.objects, {13, 2, 6, 6});
    ASSERT_NE(outer, nullptr);
    ASSERT_NE(inner, nullptr);
    ASSERT_NE(ringed, nullptr);
    ASSERT_EQ(outer->geometry().ringCount(), 2);
    EXPECT_FALSE(outer->geometry().ringIsHole(0));
    EXPECT_TRUE(outer->geometry().ringIsHole(1));
    const std::vector<Point64> hole = ringOf(outer->geometry(), 1);
    ASSERT_EQ(hole.size(), 4u);
    for (const Point64& p : hole) {
        EXPECT_TRUE((p.x == 4 || p.x == 8) && (p.y == 4 || p.y == 8));
    }
    EXPECT_EQ(measurement(*outer, "area"), 48.0);
    EXPECT_EQ(measurement(*outer, "perimeter"), 48.0);
    ASSERT_EQ(inner->geometry().ringCount(), 1);
    EXPECT_EQ(measurement(*inner, "area"), 16.0);
    ASSERT_EQ(ringed->geometry().ringCount(), 2);
    EXPECT_EQ(measurement(*ringed, "area"), 32.0);
}

TEST(Contours, IncludeHolesFalseDropsHoles) {
    auto pixel = [](int32_t, int64_t x, int64_t y) {
        if (x >= 4 && x < 7 && y >= 4 && y < 7) {
            return 3.0;
        }
        return x >= 2 && x < 10 && y >= 2 && y < 10 ? 1.0 : 0.0;
    };
    const OperationPtr op = makeContourExtraction(ContourParameters().includeHoles(false));
    const ts::DirectObjects result = ts::applyDirectObjects(*op, {DataType::UInt16}, {0}, 12, 12, pixel, 0.5);
    ASSERT_EQ(result.objects.size(), 2u);
    const ObjectRecord* outer = findByBox(result.objects, {2, 2, 8, 8});
    ASSERT_NE(outer, nullptr);
    EXPECT_EQ(outer->geometry().ringCount(), 1);
    EXPECT_EQ(measurement(*outer, "area"), 64.0);
    EXPECT_EQ(measurement(*outer, "perimeter"), 32.0);
    EXPECT_EQ(measurement(*outer, "area.um2"), 16.0);
    EXPECT_EQ(measurement(*outer, "perimeter.um"), 16.0);
    EXPECT_EQ(outer->measurements().count(), 4);
    // The region in the hole is still its own object.
    const ObjectRecord* inner = findByBox(result.objects, {4, 4, 3, 3});
    ASSERT_NE(inner, nullptr);
    EXPECT_EQ(measurement(*inner, "area"), 9.0);
}

TEST(Contours, TilingInvariantAtThreeSizes) {
    for (const slideio::DataType type : {slideio::DataType::DT_Byte, slideio::DataType::DT_Int32}) {
        const auto scene = sceneOf(1000, 800, type, [](int, int64_t x, int64_t y) { return labelRaster(x, y); });
        const PipelineSpec spec = specOf(ContourParameters());
        const std::vector<ObjectRecord> reference = ts::runEagerObjects(scene, spec, 1000, 4);
        ASSERT_GT(reference.size(), 100u);
        const auto holed = std::count_if(reference.begin(), reference.end(),
                                         [](const ObjectRecord& r) { return r.geometry().ringCount() > 1; });
        EXPECT_GT(holed, 5);
        for (const int32_t tile : {64, 100, 257}) {
            const auto cut = std::count_if(reference.begin(), reference.end(), [tile](const ObjectRecord& r) {
                const Rect64 b = r.boundingBox();
                return b.x / tile != (b.x + b.width - 1) / tile || b.y / tile != (b.y + b.height - 1) / tile;
            });
            ASSERT_GE(cut, 20) << tile << "-pixel seams should cut many objects";
            expectSameObjects(reference, ts::runEagerObjects(scene, spec, tile, 4),
                              std::to_string(tile) + "-pixel tiles");
        }
    }
}

TEST(Contours, Float32LabelsExact) {
    // 1 and the next Float32 after it are different labels; -0 and NaN are
    // background.
    const float one = 1.0f;
    const float next = std::nextafter(one, 2.0f);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    auto pixel = [=](int32_t, int64_t x, int64_t y) {
        if (y < 2 || y >= 6 || x == 0) {
            return x == 3 ? nan : -0.0;
        }
        if (x == 3) {
            return nan;
        }
        return x < 6 ? static_cast<double>(one) : static_cast<double>(next);
    };
    const OperationPtr op = makeContourExtraction(ContourParameters());
    const ts::DirectObjects result = ts::applyDirectObjects(*op, {DataType::Float32}, {0}, 10, 8, pixel);
    // Columns 1-2 and 4-5 of value 1, split by NaN at column 3, and 6-9 of the
    // next value, touching the right edge.
    ASSERT_EQ(result.objects.size(), 2u);
    EXPECT_NE(findByBox(result.objects, {1, 2, 2, 4}), nullptr);
    EXPECT_NE(findByBox(result.objects, {4, 2, 2, 4}), nullptr);
    ASSERT_EQ(result.fragments.size(), 1u);
    EXPECT_EQ(result.fragments[0].boundingBox(), (Rect64{6, 2, 4, 4}));
    EXPECT_EQ(result.fragmentKeys[0], bitsOf(next));
    ASSERT_EQ(bitsOf(next), bitsOf(one) + 1);

    // Across seams: a run joins only fragments of the very same value.
    auto seamPixel = [=](int, int64_t x, int64_t y) {
        if (y < 20 || y >= 80 || x < 20 || x >= 80) {
            return 0.0;
        }
        return (x / 10 + y / 10) % 2 == 0 ? static_cast<double>(one) : static_cast<double>(next);
    };
    const auto scene = sceneOf(100, 100, slideio::DataType::DT_Float32, seamPixel);
    const PipelineSpec spec = specOf(ContourParameters());
    const std::vector<ObjectRecord> reference = ts::runEagerObjects(scene, spec, 100, 2);
    ASSERT_EQ(reference.size(), 36u);
    for (const ObjectRecord& r : reference) {
        EXPECT_EQ(measurement(r, "area"), 100.0);
    }
    for (const int32_t tile : {25, 50, 64}) {
        expectSameObjects(reference, ts::runEagerObjects(scene, spec, tile, 4), std::to_string(tile) + "-pixel tiles");
    }
}

TEST(Contours, UnsupportedTypesRejected) {
    const OperationPtr op = makeContourExtraction(ContourParameters());
    for (const DataType type : {DataType::Byte, DataType::Int8, DataType::UInt16, DataType::Int16, DataType::UInt32,
                                DataType::Int32, DataType::Float32}) {
        EXPECT_TRUE(op->supportedTypes().contains(type)) << dataTypeName(type);
    }
    for (const DataType type : {DataType::Int64, DataType::UInt64, DataType::Float64, DataType::Float16}) {
        EXPECT_FALSE(op->supportedTypes().contains(type)) << dataTypeName(type);
    }
    EXPECT_EQ(OperationCatalog::instance().supportedTypes(kName), op->supportedTypes());
    const Configuration config = ConfigurationBuilder().tileSize(16, 16).tileSizeFromSlide(false).workerCount(1).build();
    for (const slideio::DataType type : {slideio::DataType::DT_Int64, slideio::DataType::DT_Float64}) {
        const auto scene = sceneOf(16, 16, type, [](int, int64_t x, int64_t) { return x < 4 ? 1.0 : 0.0; });
        Pipeline pipeline(scene, config, specOf(ContourParameters()));
        pipeline.bindLayer("objects", ts::objectsLayer());
        EXPECT_THROW(pipeline.validate(), UnsupportedDataTypeError);
    }
    // Given one anyway, apply() refuses it.
    for (const DataType type : {DataType::Int64, DataType::Float64}) {
        EXPECT_THROW(ts::applyDirectObjects(*op, {type}, {0}, 8, 8, [](int32_t, int64_t, int64_t) { return 1.0; }),
                     UnsupportedDataTypeError)
            << dataTypeName(type);
    }
}

TEST(Contours, TypedAndNamedBagsAreEqual) {
    const OperationPtr typed = makeContourExtraction(ContourParameters().includeHoles(false).targetLayer("cells"));
    OperationParameters bag;
    bag.setBool("includeHoles", false).setString("targetLayer", "cells");
    const OperationPtr named = OperationCatalog::instance().create(kName, bag);
    EXPECT_EQ(jsonOf(typed->parameters()), jsonOf(named->parameters()));

    // The defaults: complete except for the target layer, which is left out.
    const OperationPtr defaults = makeContourExtraction(ContourParameters());
    EXPECT_EQ(jsonOf(defaults->parameters()),
              jsonOf(OperationCatalog::instance().create(kName, OperationParameters())->parameters()));
    EXPECT_EQ(defaults->parameters().count(), 1);
    bool includeHoles = false;
    EXPECT_TRUE(defaults->parameters().getBool("includeHoles", includeHoles));
    EXPECT_TRUE(includeHoles);
    EXPECT_STREQ(defaults->parameters().getString("targetLayer", "absent"), "absent");
    const OperationPtr cleared = makeContourExtraction(ContourParameters().targetLayer("cells").targetLayer(""));
    EXPECT_EQ(jsonOf(cleared->parameters()), jsonOf(defaults->parameters()));
}

TEST(Contours, RejectsBadParameters) {
    EXPECT_THROW(ContourParameters().targetLayer(nullptr), InvalidArgumentError);
    const auto expectInvalid = [](const std::function<void(OperationParameters&)>& fill, const char* key) {
        OperationParameters bag;
        fill(bag);
        try {
            OperationCatalog::instance().create(kName, bag);
            ADD_FAILURE() << key << ": nothing thrown";
        } catch (const InvalidArgumentError& e) {
            const std::string message = e.what();
            EXPECT_NE(message.find(kName), std::string::npos) << message;
            EXPECT_NE(message.find(key), std::string::npos) << message;
        }
    };
    expectInvalid([](OperationParameters& b) { b.setInt("includeHoles", 1); }, "includeHoles");
    expectInvalid([](OperationParameters& b) { b.setBool("targetLayer", true); }, "targetLayer");
    expectInvalid([](OperationParameters& b) { b.setInt("connectivity", 4); }, "connectivity");

    ContourParameters moved;
    ContourParameters taken(std::move(moved));
    EXPECT_THROW(moved.includeHoles(true), InvalidArgumentError); // NOLINT(bugprone-use-after-move)
    EXPECT_THROW(makeContourExtraction(moved), InvalidArgumentError);
}

TEST(Contours, NeedsExactlyOneChannel) {
    // A label raster stored as identical channels would otherwise give every
    // region once per channel, under one id.
    const auto scene = ts::makeSyntheticScene(
        {{16, 16, 0, 0}}, std::vector<slideio::DataType>{slideio::DataType::DT_Byte, slideio::DataType::DT_Byte},
        [](int, int64_t x, int64_t) { return x < 4 ? 1.0 : 0.0; });
    const Configuration config = ConfigurationBuilder().tileSize(16, 16).tileSizeFromSlide(false).workerCount(1).build();
    Pipeline refused(scene, config, specOf(ContourParameters()));
    refused.bindLayer("objects", ts::objectsLayer());
    try {
        refused.validate();
        FAIL() << "validate() accepted segment.contours over two channels";
    } catch (const InvalidArgumentError& e) {
        const std::string message = e.what();
        EXPECT_NE(message.find("\"segment.contours\""), std::string::npos) << message;
        EXPECT_NE(message.find("exactly one channel"), std::string::npos) << message;
    }

    PipelineSpec one = specOf(ContourParameters());
    one.channels(ChannelSelection::single(0));
    Pipeline accepted(scene, config, one);
    accepted.bindLayer("objects", ts::objectsLayer());
    EXPECT_NO_THROW(accepted.validate());
}

TEST(Contours, TargetLayerByName) {
    auto pixel = [](int, int64_t x, int64_t y) { return x >= 4 && x < 8 && y >= 4 && y < 8 ? 2.0 : 0.0; };
    const auto scene = sceneOf(16, 16, slideio::DataType::DT_Byte, pixel);
    const Configuration config = ConfigurationBuilder().tileSize(16, 16).tileSizeFromSlide(false).workerCount(1).build();
    const LayerId other{9};
    Pipeline pipeline(scene, config, specOf(ContourParameters().targetLayer("second")));
    pipeline.bindLayer("objects", ts::objectsLayer()).bindLayer("second", other);
    ts::CollectingObjectSink sink;
    pipeline.runInto(sink, nullptr, CancellationToken());
    const std::vector<ObjectRecord> records = sink.records();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].layer(), other);
}
