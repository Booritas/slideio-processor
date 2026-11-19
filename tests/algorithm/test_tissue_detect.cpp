// Requirements: UR-SEG-1, UR-SEG-3, UR-SEG-4, UR-SEG-6, UR-SEG-7, UR-QA-2, UR-ALG-6
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

#include <slideio/slideio/scene.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using namespace slideio::processor;
namespace ts = slideio::processor::testing_support;

namespace {

constexpr const char* kName = "segment.tissue_detect";
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

PipelineSpec specOf(const TissueDetectParameters& parameters, int32_t analysisLevel = 0) {
    PipelineSpec spec;
    spec.analysisLevel(analysisLevel).then(makeTissueDetect(parameters));
    return spec;
}

double areaOf(const std::vector<ObjectRecord>& objects) {
    double area = 0.0;
    for (const ObjectRecord& r : objects) {
        area += measurement(r, "area");
    }
    return area;
}

// A deterministic hash of a pixel position, for noise.
uint32_t hashOf(int64_t x, int64_t y) {
    uint32_t h = static_cast<uint32_t>(x) * 73856093u ^ static_cast<uint32_t>(y) * 19349663u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return h;
}

struct Ellipse {
    double cx;
    double cy;
    double a;
    double b;
    // Whether the pixel's centre lies inside.
    bool contains(int64_t x, int64_t y) const {
        const double dx = (static_cast<double>(x) + 0.5 - cx) / a;
        const double dy = (static_cast<double>(y) + 0.5 - cy) / b;
        return dx * dx + dy * dy <= 1.0;
    }
    double area() const { return kPi * a * b; }
};

struct Box {
    int64_t x;
    int64_t y;
    int64_t width;
    int64_t height;
    bool contains(int64_t px, int64_t py) const { return px >= x && px < x + width && py >= y && py < y + height; }
};

// The RGB of a synthetic slide: pink tissue, its brightness varying, with a
// white one-pixel hole here and there that the closing fills; elsewhere
// near-white glass whose grey level varies and whose channels differ by at most
// 4 (saturation below 0.02), with dark grey dust.
double slideSample(bool tissue, int channel, int64_t x, int64_t y) {
    const uint32_t h = hashOf(x, y);
    if (tissue && h % 37 != 0) {
        static const double pink[3] = {225.0, 150.0, 195.0};
        return pink[channel] + static_cast<double>(static_cast<int32_t>(h % 21) - 10);
    }
    if (h % 101 == 0) {
        return 60.0;
    }
    const double grey = 215.0 + static_cast<double>((h >> 8) % 41);
    return grey + static_cast<double>(static_cast<int32_t>((h >> (16 + 3 * channel)) % 5) - 2);
}

// The synthetic slide's tissue: three ellipses, cut by the seams of 64-, 100-
// and 257-pixel tiles. With the extras, two rectangles 2 pixels apart, the gap
// on a seam of 100-pixel tiles, and two combs, one of columns and one of rows,
// each a band of bars 3 pixels wide 4 apart (period 7) across many seams: at
// some seam, every phase of bar and gap. A closing of radius 2 or more joins
// the rectangles and fills each comb, a 4-pixel gap closed only by tissue 4
// pixels away: what a tile's halo must hold.
const Ellipse kEllipses[] = {{300.0, 250.0, 180.0, 110.0}, {720.0, 520.0, 150.0, 200.0}, {760.0, 170.0, 90.0, 60.0}};
const Box kBridged[] = {{100, 600, 200, 100}, {302, 600, 148, 100}};
const Box kColumnComb{0, 740, 1000, 40};
const Box kRowComb{940, 0, 40, 700};

std::shared_ptr<slideio::Scene> slideScene(bool withExtras, slideio::DataType type, double scale) {
    return ts::makeSyntheticScene(
        {{1000, 800, 0, 0}}, {type, type, type}, [withExtras, scale](int channel, int64_t x, int64_t y) {
            bool tissue = false;
            for (const Ellipse& e : kEllipses) {
                tissue = tissue || e.contains(x, y);
            }
            if (withExtras) {
                for (const Box& b : kBridged) {
                    tissue = tissue || b.contains(x, y);
                }
                tissue = tissue || (kColumnComb.contains(x, y) && x % 7 < 3);
                tissue = tissue || (kRowComb.contains(x, y) && y % 7 < 3);
            }
            return scale * slideSample(tissue, channel, x, y);
        });
}

// Pure pink inside @p tissue and white outside.
std::function<double(int32_t, int64_t, int64_t)> pinkOn(std::function<bool(int64_t, int64_t)> tissue) {
    return [tissue](int32_t channel, int64_t x, int64_t y) {
        static const double pink[3] = {225.0, 150.0, 195.0};
        return tissue(x, y) ? pink[channel] : 245.0;
    };
}

} // namespace

TEST(TissueDetect, TissueAreaOnSyntheticSlide) {
    const auto scene = slideScene(false, slideio::DataType::DT_Byte, 1.0);
    const std::vector<ObjectRecord> objects =
        ts::runEagerObjects(scene, specOf(TissueDetectParameters()), 256, 4);
    ASSERT_EQ(objects.size(), 3u);
    double expected = 0.0;
    for (const Ellipse& e : kEllipses) {
        expected += e.area();
    }
    const double area = areaOf(objects);
    EXPECT_LE(std::fabs(area - expected), 0.01 * expected) << "detected " << area << ", analytic " << expected;
    for (const ObjectRecord& r : objects) {
        // The holes are closed, the glass and dust are not tissue.
        EXPECT_EQ(r.geometry().ringCount(), 1);
        EXPECT_TRUE(has(r, "perimeter"));
        EXPECT_FALSE(has(r, "centroid.x"));
        EXPECT_FALSE(has(r, "area.um2"));
        EXPECT_EQ(r.layer(), ts::objectsLayer());
    }
    // Without the closing, the one-pixel holes stay.
    const std::vector<ObjectRecord> open =
        ts::runEagerObjects(scene, specOf(TissueDetectParameters().closingRadiusPx(0)), 256, 4);
    // (Holes next to the outline may also cut off a pixel or two.)
    EXPECT_EQ(std::count_if(open.begin(), open.end(),
                            [](const ObjectRecord& r) { return r.geometry().ringCount() > 10; }),
              3);
}

TEST(TissueDetect, SaturationRule) {
    // Threshold 0.5: (200, 100, 100) has saturation exactly 0.5 and is not
    // tissue, (200, 99, 100) is; grey and black are not, in any channel order.
    struct Pixel {
        double rgb[3];
        bool tissue;
    };
    const Pixel pixels[] = {
        {{200, 100, 100}, false}, {{200, 99, 100}, true}, {{100, 99, 200}, true}, {{0, 0, 0}, false},
        {{128, 128, 128}, false}, {{0, 0, 1}, true},      {{255, 255, 0}, true},
    };
    const OperationPtr op = makeTissueDetect(TissueDetectParameters().saturationThreshold(0.5).closingRadiusPx(0));
    // Each pixel alone in a 3 x 3 cell of black, side by side.
    const int64_t count = static_cast<int64_t>(std::size(pixels));
    const ts::Image out = ts::applyDirect(
        *op, {DataType::Byte, DataType::Byte, DataType::Byte}, {0, 1, 2}, 3 * count, 3,
        [&](int32_t channel, int64_t x, int64_t y) {
            return x % 3 == 1 && y == 1 ? pixels[x / 3].rgb[channel] : 0.0;
        });
    ASSERT_EQ(out.types.size(), 1u);
    EXPECT_EQ(out.types[0], DataType::Byte);
    for (int64_t i = 0; i < count; ++i) {
        for (int64_t y = 0; y < 3; ++y) {
            for (int64_t x = 3 * i; x < 3 * i + 3; ++x) {
                const bool centre = x == 3 * i + 1 && y == 1;
                EXPECT_EQ(out.at(0, x, y), centre && pixels[i].tissue ? 255.0 : 0.0) << "pixel " << i;
            }
        }
    }
}

TEST(TissueDetect, ClosingBridgesGaps) {
    // Two 13 x 20 rectangles 2 pixels apart, the left one with a one-pixel hole.
    const Box left{5, 5, 13, 20};
    const Box right{20, 5, 13, 20};
    const auto tissue = [&](int64_t x, int64_t y) {
        return (left.contains(x, y) && !(x == 10 && y == 15)) || right.contains(x, y);
    };
    const auto pixel = pinkOn(tissue);
    const std::vector<DataType> rgb{DataType::Byte, DataType::Byte, DataType::Byte};

    const OperationPtr none = makeTissueDetect(TissueDetectParameters().closingRadiusPx(0));
    const ts::DirectObjects apart = ts::applyDirectObjects(*none, rgb, {0, 1, 2}, 40, 30, pixel);
    ASSERT_EQ(apart.objects.size(), 2u);
    EXPECT_TRUE(apart.fragments.empty());
    EXPECT_EQ(areaOf(apart.objects), 519.0);

    const OperationPtr closing = makeTissueDetect(TissueDetectParameters().closingRadiusPx(2));
    const ts::DirectObjects joined = ts::applyDirectObjects(*closing, rgb, {0, 1, 2}, 40, 30, pixel);
    ASSERT_EQ(joined.objects.size(), 1u);
    EXPECT_EQ(joined.objects[0].geometry().ringCount(), 1);
    // Both rectangles whole, and the gap filled but for its ends.
    const double area = measurement(joined.objects[0], "area");
    EXPECT_GT(area, 520.0 + 2.0 * 16.0);
    EXPECT_LE(area, 520.0 + 2.0 * 20.0);
    const ts::Image out = ts::applyDirect(*closing, rgb, {0, 1, 2}, 40, 30, pixel);
    for (int64_t y = 8; y < 22; ++y) {
        EXPECT_EQ(out.at(0, 18, y), 255.0) << y;
        EXPECT_EQ(out.at(0, 19, y), 255.0) << y;
    }
    EXPECT_EQ(out.at(0, 10, 15), 255.0);
    EXPECT_EQ(out.at(0, 2, 2), 0.0);
}

TEST(TissueDetect, SmallRegionsFiltered) {
    // Rendered at level 1 of a two-level scene: 6 x 6 and two 12 x 12 squares,
    // one cut by the seams of 64-pixel tiles: 144 and 576 square level-0
    // pixels. At level 1 they cover 36 and 144 pixels, all below 200.
    const Box squares[] = {{20, 20, 6, 6}, {100, 100, 12, 12}, {58, 58, 12, 12}};
    const auto scene = ts::makeSyntheticScene(
        {{400, 400, 0, 0}, {200, 200, 0, 0}},
        {slideio::DataType::DT_Byte, slideio::DataType::DT_Byte, slideio::DataType::DT_Byte},
        [&](int channel, int64_t x, int64_t y) {
            const auto inside = [&](int64_t px, int64_t py) {
                return std::any_of(std::begin(squares), std::end(squares),
                                   [&](const Box& b) { return b.contains(px, py); });
            };
            return pinkOn(inside)(channel, x, y);
        });
    for (const int32_t tile : {64, 200}) {
        const std::vector<ObjectRecord> all = ts::runEagerObjects(scene, specOf(TissueDetectParameters(), 1), tile, 4);
        ASSERT_EQ(all.size(), 3u) << tile;
        EXPECT_EQ(areaOf(all), 144.0 + 2.0 * 576.0) << tile;
        const std::vector<ObjectRecord> kept =
            ts::runEagerObjects(scene, specOf(TissueDetectParameters().minimumRegionAreaPx(200), 1), tile, 4);
        ASSERT_EQ(kept.size(), 2u) << tile;
        for (const ObjectRecord& r : kept) {
            EXPECT_EQ(measurement(r, "area"), 576.0) << tile;
        }
        // The bound is inclusive.
        EXPECT_EQ(ts::runEagerObjects(scene, specOf(TissueDetectParameters().minimumRegionAreaPx(576), 1), tile, 4)
                      .size(),
                  2u)
            << tile;
        EXPECT_TRUE(
            ts::runEagerObjects(scene, specOf(TissueDetectParameters().minimumRegionAreaPx(577), 1), tile, 4).empty())
            << tile;
    }
}

TEST(TissueDetect, TilingInvariantAtThreeSizes) {
    const auto scene = slideScene(true, slideio::DataType::DT_Byte, 1.0);
    for (const int32_t radius : {2, 0, 5}) {
        const PipelineSpec spec = specOf(TissueDetectParameters().closingRadiusPx(radius));
        const std::vector<ObjectRecord> reference = ts::runEagerObjects(scene, spec, 1000, 4);
        // With a closing, six large objects: the ellipses, the joined
        // rectangles and the filled combs; without, the rectangles and the 243
        // bars apart. Either way, holes next to an outline may cut off a pixel
        // or two.
        if (radius == 0) {
            ASSERT_GE(reference.size(), 248u);
        } else {
            EXPECT_EQ(std::count_if(reference.begin(), reference.end(),
                                    [](const ObjectRecord& r) { return measurement(r, "area") > 10000.0; }),
                      6)
                << radius;
        }
        for (const int32_t tile : {64, 100, 257}) {
            expectSameObjects(reference, ts::runEagerObjects(scene, spec, tile, 4),
                              "radius " + std::to_string(radius) + ", " + std::to_string(tile) + "-pixel tiles");
        }
    }
}

TEST(TissueDetect, LevelTwoAreasInLevelZeroPixels) {
    // Two ellipses rendered at level 2 of a three-level scene; at level 0 they
    // would have semi-axes four times as long.
    const Ellipse ellipses[] = {{100.0, 80.0, 40.0, 24.0}, {190.0, 130.0, 30.0, 30.0}};
    const auto inside = [&](int64_t x, int64_t y) {
        return std::any_of(std::begin(ellipses), std::end(ellipses),
                           [&](const Ellipse& e) { return e.contains(x, y); });
    };
    const auto scene = ts::makeSyntheticScene(
        {{1024, 768, 0, 0}, {512, 384, 0, 0}, {256, 192, 0, 0}},
        {slideio::DataType::DT_Byte, slideio::DataType::DT_Byte, slideio::DataType::DT_Byte},
        [&](int channel, int64_t x, int64_t y) { return pinkOn(inside)(channel, x, y); });
    for (const int32_t radius : {0, 2}) {
        const std::vector<ObjectRecord> objects =
            ts::runEagerObjects(scene, specOf(TissueDetectParameters().closingRadiusPx(radius), 2), 64, 4);
        ASSERT_EQ(objects.size(), 2u) << radius;
        for (const Ellipse& e : ellipses) {
            const auto found = std::find_if(objects.begin(), objects.end(), [&](const ObjectRecord& r) {
                const Rect64 b = r.boundingBox();
                return std::abs(static_cast<double>(b.x + b.width / 2) - 4.0 * e.cx) <= 8.0;
            });
            ASSERT_NE(found, objects.end()) << radius;
            const Rect64 box = found->boundingBox();
            EXPECT_EQ(box.x % 4, 0);
            EXPECT_EQ(box.y % 4, 0);
            EXPECT_EQ(box.width % 4, 0);
            EXPECT_EQ(box.height % 4, 0);
            const double area = measurement(*found, "area");
            if (radius == 0) {
                // Exactly 16 square level-0 pixels per level-2 pixel.
                int64_t pixels = 0;
                for (int64_t y = 0; y < 192; ++y) {
                    for (int64_t x = 0; x < 256; ++x) {
                        pixels += e.contains(x, y) ? 1 : 0;
                    }
                }
                EXPECT_EQ(area, 16.0 * static_cast<double>(pixels));
            }
            // The staircase: a level-2 pixel counts whole or not at all, so the
            // count differs from the ellipse's area by at most the pixels its
            // outline crosses: four monotone arcs, each crossing at most
            // a + b + 1 pixels. Each such pixel is 16 level-0 pixels.
            const double tolerance = 16.0 * 4.0 * (e.a + e.b + 1.0);
            const double analytic = kPi * (4.0 * e.a) * (4.0 * e.b);
            EXPECT_LE(std::fabs(area - analytic), tolerance)
                << "radius " << radius << ": area " << area << ", analytic " << analytic;
        }
    }
}

TEST(TissueDetect, UInt16Accepted) {
    // The slide in UInt16, every sample times 257: the same saturations, so
    // the same objects.
    const PipelineSpec spec = specOf(TissueDetectParameters());
    const std::vector<ObjectRecord> bytes =
        ts::runEagerObjects(slideScene(true, slideio::DataType::DT_Byte, 1.0), spec, 100, 4);
    ASSERT_GE(bytes.size(), 6u);
    expectSameObjects(bytes, ts::runEagerObjects(slideScene(true, slideio::DataType::DT_UInt16, 257.0), spec, 100, 4),
                      "UInt16");
}

TEST(TissueDetect, NonRgbRejected) {
    const Configuration config = ConfigurationBuilder().tileSize(16, 16).tileSizeFromSlide(false).workerCount(1).build();
    const PipelineSpec spec = specOf(TissueDetectParameters());
    const auto validate = [&](std::vector<slideio::DataType> types) {
        Pipeline pipeline(ts::makeSyntheticScene({{16, 16, 0, 0}}, std::move(types),
                                                 [](int, int64_t, int64_t) { return 1.0; }),
                          config, spec);
        pipeline.bindLayer("objects", ts::objectsLayer());
        pipeline.validate();
    };
    EXPECT_NO_THROW(validate({slideio::DataType::DT_Byte, slideio::DataType::DT_Byte, slideio::DataType::DT_Byte}));
    for (const size_t channels : {size_t{1}, size_t{2}, size_t{4}}) {
        try {
            validate(std::vector<slideio::DataType>(channels, slideio::DataType::DT_Byte));
            ADD_FAILURE() << channels << " channels validated";
        } catch (const ChannelArityError& e) {
            EXPECT_STREQ(e.what(), "segment.tissue_detect: needs exactly three channels, red, green and blue")
                << channels;
        }
    }
    EXPECT_THROW(validate({slideio::DataType::DT_Int16, slideio::DataType::DT_Int16, slideio::DataType::DT_Int16}),
                 UnsupportedDataTypeError);
    EXPECT_THROW(validate({slideio::DataType::DT_Byte, slideio::DataType::DT_UInt16, slideio::DataType::DT_Byte}),
                 ProcessorError);

    const OperationPtr op = makeTissueDetect(TissueDetectParameters());
    EXPECT_EQ(op->outputPlaneCount(3), 1);
    EXPECT_EQ(op->outputType(DataType::UInt16), DataType::Byte);
    EXPECT_THROW(op->outputPlaneCount(1), ChannelArityError);
    EXPECT_THROW(op->outputPlaneCount(4), ChannelArityError);
    EXPECT_EQ(OperationCatalog::instance().outputPlaneCount(kName, 3), 1);
    EXPECT_THROW(OperationCatalog::instance().outputPlaneCount(kName, 2), ChannelArityError);
    // Applied directly to two planes, the tile is refused too.
    EXPECT_THROW(ts::applyDirect(*op, {DataType::Byte, DataType::Byte}, {0, 1}, 8, 8,
                                 [](int32_t, int64_t, int64_t) { return 1.0; }),
                 ChannelArityError);
}

TEST(TissueDetect, Declarations) {
    const OperationPtr op = makeTissueDetect(TissueDetectParameters());
    EXPECT_EQ(op->arity(), ChannelArity::Joint);
    EXPECT_TRUE(op->requiresUniformTypes());
    EXPECT_TRUE(op->supportedTypes() == DataTypeSet().add(DataType::Byte).add(DataType::UInt16));
    EXPECT_TRUE(op->producesObjects());
    EXPECT_FALSE(op->objectsAreEightConnected());
    EXPECT_EQ(op->halo().left, 5);
    EXPECT_EQ(op->halo().bottom, 5);
    EXPECT_EQ(makeTissueDetect(TissueDetectParameters().closingRadiusPx(0))->halo().right, 1);
    EXPECT_EQ(makeTissueDetect(TissueDetectParameters().closingRadiusPx(127))->halo().top, 255);
    EXPECT_GT(std::strlen(OperationCatalog::instance().correctnessReference(kName)), 0u);
}

TEST(TissueDetect, CalibratedAreasInMicrometres) {
    const auto tissue = [](int64_t x, int64_t y) { return x >= 2 && x < 6 && y >= 2 && y < 7; };
    const OperationPtr op = makeTissueDetect(TissueDetectParameters());
    const ts::DirectObjects result = ts::applyDirectObjects(*op, {DataType::Byte, DataType::Byte, DataType::Byte},
                                                            {0, 1, 2}, 10, 10, pinkOn(tissue), 0.5);
    ASSERT_EQ(result.objects.size(), 1u);
    const ObjectRecord& r = result.objects[0];
    EXPECT_EQ(measurement(r, "area"), 20.0);
    EXPECT_EQ(measurement(r, "perimeter"), 18.0);
    EXPECT_EQ(measurement(r, "area.um2"), 5.0);
    EXPECT_EQ(measurement(r, "perimeter.um"), 9.0);
    EXPECT_EQ(r.measurements().count(), 4);
}

TEST(TissueDetect, TypedAndNamedBagsAreEqual) {
    const OperationPtr typed = makeTissueDetect(TissueDetectParameters()
                                                    .saturationThreshold(0.15)
                                                    .minimumRegionAreaPx(5000)
                                                    .closingRadiusPx(4)
                                                    .targetLayer("tissue"));
    OperationParameters bag;
    bag.setDouble("saturationThreshold", 0.15)
        .setInt("minimumRegionAreaPx", 5000)
        .setInt("closingRadiusPx", 4)
        .setString("targetLayer", "tissue");
    const OperationPtr named = OperationCatalog::instance().create(kName, bag);
    EXPECT_EQ(jsonOf(typed->parameters()), jsonOf(named->parameters()));
    EXPECT_EQ(named->halo().left, 9);

    // The defaults: complete except for the target layer, which is left out.
    const OperationPtr defaults = makeTissueDetect(TissueDetectParameters());
    EXPECT_EQ(jsonOf(defaults->parameters()),
              jsonOf(OperationCatalog::instance().create(kName, OperationParameters())->parameters()));
    EXPECT_EQ(defaults->parameters().count(), 3);
    double threshold = -1.0;
    int64_t area = -1;
    int64_t radius = -1;
    ASSERT_TRUE(defaults->parameters().getDouble("saturationThreshold", threshold));
    ASSERT_TRUE(defaults->parameters().getInt("minimumRegionAreaPx", area));
    ASSERT_TRUE(defaults->parameters().getInt("closingRadiusPx", radius));
    EXPECT_EQ(threshold, 0.07);
    EXPECT_EQ(area, 0);
    EXPECT_EQ(radius, 2);
    EXPECT_STREQ(defaults->parameters().getString("targetLayer", "absent"), "absent");
    const OperationPtr cleared = makeTissueDetect(TissueDetectParameters().targetLayer("tissue").targetLayer(""));
    EXPECT_EQ(jsonOf(cleared->parameters()), jsonOf(defaults->parameters()));
}

TEST(TissueDetect, RejectsBadParameters) {
    const auto expectInvalid = [](const std::function<void()>& make, const char* key, const std::string& what) {
        try {
            make();
            ADD_FAILURE() << what << ": nothing thrown";
        } catch (const InvalidArgumentError& e) {
            const std::string message = e.what();
            EXPECT_NE(message.find(kName), std::string::npos) << what << ": " << message;
            EXPECT_NE(message.find(key), std::string::npos) << what << ": " << message;
            EXPECT_EQ(message.find("128"), std::string::npos) << what << ": " << message;
        }
    };
    expectInvalid([] { makeTissueDetect(TissueDetectParameters().saturationThreshold(-0.01)); },
                  "saturationThreshold", "negative threshold");
    expectInvalid([] { makeTissueDetect(TissueDetectParameters().saturationThreshold(1.01)); },
                  "saturationThreshold", "threshold above 1");
    expectInvalid(
        [] {
            makeTissueDetect(
                TissueDetectParameters().saturationThreshold(std::numeric_limits<double>::quiet_NaN()));
        },
        "saturationThreshold", "NaN threshold");
    expectInvalid([] { makeTissueDetect(TissueDetectParameters().minimumRegionAreaPx(-1)); }, "minimumRegionAreaPx",
                  "negative area");
    expectInvalid([] { makeTissueDetect(TissueDetectParameters().closingRadiusPx(-1)); }, "closingRadiusPx",
                  "negative radius");
    expectInvalid([] { makeTissueDetect(TissueDetectParameters().closingRadiusPx(128)); }, "closingRadiusPx",
                  "radius above 127");
    EXPECT_NO_THROW(makeTissueDetect(TissueDetectParameters().saturationThreshold(0.0)));
    EXPECT_NO_THROW(makeTissueDetect(TissueDetectParameters().saturationThreshold(1.0)));
    EXPECT_THROW(TissueDetectParameters().targetLayer(nullptr), InvalidArgumentError);

    const auto byName = [](const std::function<void(OperationParameters&)>& fill) {
        return [fill] {
            OperationParameters bag;
            fill(bag);
            OperationCatalog::instance().create(kName, bag);
        };
    };
    expectInvalid(byName([](OperationParameters& b) { b.setDouble("saturationThreshold", 2.0); }),
                  "saturationThreshold", "threshold above 1 by name");
    expectInvalid(byName([](OperationParameters& b) { b.setInt("closingRadiusPx", 128); }), "closingRadiusPx",
                  "radius above 127 by name");
    expectInvalid(byName([](OperationParameters& b) { b.setInt("closingRadiusPx", int64_t{1} << 40); }),
                  "closingRadiusPx", "radius beyond int32 by name");
    expectInvalid(byName([](OperationParameters& b) { b.setInt("minimumRegionAreaPx", -3); }),
                  "minimumRegionAreaPx", "negative area by name");
    expectInvalid(byName([](OperationParameters& b) { b.setString("saturationThreshold", "0.1"); }),
                  "saturationThreshold", "threshold of the wrong type");
    expectInvalid(byName([](OperationParameters& b) { b.setInt("minimumAreaPx", 4); }), "minimumAreaPx",
                  "unknown key");
}
