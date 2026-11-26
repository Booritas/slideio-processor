// Requirements: UR-SEG-1, UR-SEG-3, UR-SEG-4, UR-SEG-6, UR-SEG-7, UR-QA-2, UR-ALG-6
#include <slideio/processor/algorithm/segment/segment.hpp>
#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/channel_selection.hpp>
#include <slideio/processor/core/control.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/measurement.hpp>
#include <slideio/processor/core/object_record.hpp>
#include <slideio/processor/core/raster_mask.hpp>
#include <slideio/processor/pipeline/operation_catalog.hpp>
#include <slideio/processor/pipeline/pipeline.hpp>
#include <slideio/processor/pipeline/pipeline_spec.hpp>

#include <algorithm/support/harness.hpp>
#include <pipeline/test_support/synthetic_scene.hpp>
#include <segment/detail/triangle_threshold.hpp>

#include <slideio/slideio/scene.hpp>

#include <gtest/gtest.h>
#include <opencv2/imgproc.hpp>

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

TEST(TissueDetect, BrightfieldFloat32) {
    // The slide in Float32 at the same values: the same objects as Byte.
    const PipelineSpec spec = specOf(TissueDetectParameters());
    const std::vector<ObjectRecord> bytes =
        ts::runEagerObjects(slideScene(true, slideio::DataType::DT_Byte, 1.0), spec, 100, 4);
    ASSERT_GE(bytes.size(), 6u);
    expectSameObjects(bytes, ts::runEagerObjects(slideScene(true, slideio::DataType::DT_Float32, 1.0), spec, 100, 4),
                      "Float32");

    // Pixel by pixel, without a closing: saturated pink is tissue; a NaN in any
    // plane makes the pixel background, the other two pink whatever their
    // order; so does a maximum of 0 over negative samples, whose spread over
    // the maximum would divide by zero.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    struct Pixel {
        double samples[3];
        bool tissue;
    };
    const Pixel pixels[] = {
        {{225.0, 150.0, 195.0}, true}, {{nan, 150.0, 195.0}, false}, {{225.0, nan, 195.0}, false},
        {{225.0, 150.0, nan}, false},  {{nan, nan, nan}, false},     {{0.0, -1.0, -2.0}, false},
    };
    const OperationPtr op = makeTissueDetect(TissueDetectParameters().closingRadiusPx(0));
    const int64_t count = static_cast<int64_t>(std::size(pixels));
    const ts::Image out = ts::applyDirect(*op, {DataType::Float32, DataType::Float32, DataType::Float32}, {0, 1, 2},
                                          3 * count, 3, [&](int32_t channel, int64_t x, int64_t y) {
                                              return x % 3 == 1 && y == 1 ? pixels[x / 3].samples[channel] : 0.0;
                                          });
    for (int64_t i = 0; i < count; ++i) {
        EXPECT_EQ(out.at(0, 3 * i + 1, 1), pixels[i].tissue ? 255.0 : 0.0) << "pixel " << i;
    }
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
            EXPECT_STREQ(e.what(),
                         "segment.tissue_detect: needs exactly three channels, red, green and blue; for fluorescence, "
                         "set mode to Fluorescence")
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
    EXPECT_TRUE(op->supportedTypes() ==
                DataTypeSet().add(DataType::Byte).add(DataType::UInt16).add(DataType::Float32));
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

TEST(TissueDetect, BrightfieldBagUnchanged) {
    const char* expected = R"({"closingRadiusPx":2,"minimumRegionAreaPx":0,"saturationThreshold":0.07})";
    EXPECT_EQ(jsonOf(makeTissueDetect(TissueDetectParameters())->parameters()), expected);
    EXPECT_EQ(jsonOf(makeTissueDetect(TissueDetectParameters().mode(TissueMode::Brightfield))->parameters()),
              expected);
}

TEST(TissueDetect, FluorescenceTypedAndNamedBagsAreEqual) {
    const OperationPtr typed =
        makeTissueDetect(TissueDetectParameters().mode(TissueMode::Fluorescence).closingRadiusPx(3));
    OperationParameters bag;
    bag.setString("mode", "Fluorescence").setInt("closingRadiusPx", 3);
    const OperationPtr named = OperationCatalog::instance().create(kName, bag);
    EXPECT_EQ(jsonOf(typed->parameters()), jsonOf(named->parameters()));
    const OperationParameters& recorded = typed->parameters();
    EXPECT_EQ(recorded.count(), 4);
    EXPECT_STREQ(recorded.getString("mode", "absent"), "Fluorescence");
    EXPECT_STREQ(recorded.getString("thresholdMethod", "absent"), "Triangle");
    int64_t value = -1;
    EXPECT_TRUE(recorded.getInt("minimumRegionAreaPx", value));
    EXPECT_TRUE(recorded.getInt("closingRadiusPx", value));
    EXPECT_EQ(value, 3);
    double unused = 0.0;
    EXPECT_FALSE(recorded.getDouble("saturationThreshold", unused));
    EXPECT_FALSE(recorded.getDouble("intensityThreshold", unused));

    const OperationPtr fixed = makeTissueDetect(TissueDetectParameters()
                                                    .mode(TissueMode::Fluorescence)
                                                    .thresholdMethod(TissueThresholdMethod::Fixed)
                                                    .intensityThreshold(5.5));
    OperationParameters fixedBag;
    fixedBag.setString("mode", "Fluorescence").setString("thresholdMethod", "Fixed").setDouble("intensityThreshold", 5.5);
    EXPECT_EQ(jsonOf(fixed->parameters()), jsonOf(OperationCatalog::instance().create(kName, fixedBag)->parameters()));
    EXPECT_EQ(fixed->parameters().count(), 5);
    double threshold = -1.0;
    ASSERT_TRUE(fixed->parameters().getDouble("intensityThreshold", threshold));
    EXPECT_EQ(threshold, 5.5);
    EXPECT_STREQ(fixed->parameters().getString("thresholdMethod", "absent"), "Fixed");
}

TEST(TissueDetect, RejectsBadFluorescenceParameters) {
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
    const auto byName = [](const std::function<void(OperationParameters&)>& fill) {
        return [fill] {
            OperationParameters bag;
            fill(bag);
            OperationCatalog::instance().create(kName, bag);
        };
    };
    const TissueMode fl = TissueMode::Fluorescence;
    expectInvalid([=] { makeTissueDetect(TissueDetectParameters().mode(fl).saturationThreshold(0.1)); },
                  "saturationThreshold", "saturation in fluorescence");
    expectInvalid(byName([](OperationParameters& b) {
                      b.setString("mode", "Fluorescence").setDouble("saturationThreshold", 0.07);
                  }),
                  "saturationThreshold", "saturation in fluorescence by name");
    expectInvalid(
        [] { makeTissueDetect(TissueDetectParameters().thresholdMethod(TissueThresholdMethod::Fixed)); },
        "thresholdMethod", "method in brightfield");
    expectInvalid(byName([](OperationParameters& b) { b.setString("thresholdMethod", "Triangle"); }),
                  "thresholdMethod", "method in brightfield by name");
    expectInvalid([] { makeTissueDetect(TissueDetectParameters().intensityThreshold(3.0)); }, "intensityThreshold",
                  "intensity in brightfield");
    expectInvalid(byName([](OperationParameters& b) { b.setDouble("intensityThreshold", 3.0); }),
                  "intensityThreshold", "intensity in brightfield by name");
    expectInvalid(
        [=] {
            makeTissueDetect(TissueDetectParameters().mode(fl).thresholdMethod(TissueThresholdMethod::Fixed));
        },
        "intensityThreshold", "Fixed without intensity");
    expectInvalid(byName([](OperationParameters& b) {
                      b.setString("mode", "Fluorescence").setString("thresholdMethod", "Fixed");
                  }),
                  "intensityThreshold", "Fixed without intensity by name");
    expectInvalid([=] { makeTissueDetect(TissueDetectParameters().mode(fl).intensityThreshold(3.0)); },
                  "intensityThreshold", "Triangle with intensity");
    expectInvalid(byName([](OperationParameters& b) {
                      b.setString("mode", "Fluorescence").setDouble("intensityThreshold", 3.0);
                  }),
                  "intensityThreshold", "Triangle with intensity by name");
    expectInvalid(
        [=] {
            makeTissueDetect(TissueDetectParameters()
                                 .mode(fl)
                                 .thresholdMethod(TissueThresholdMethod::Fixed)
                                 .intensityThreshold(std::numeric_limits<double>::quiet_NaN()));
        },
        "intensityThreshold", "NaN intensity");
    expectInvalid(byName([](OperationParameters& b) { b.setString("mode", "Dark"); }), "mode", "unknown mode");
    expectInvalid(byName([](OperationParameters& b) {
                      b.setString("mode", "Fluorescence").setString("thresholdMethod", "Otsu");
                  }),
                  "thresholdMethod", "unknown method");
    EXPECT_NO_THROW(makeTissueDetect(TissueDetectParameters().mode(fl).closingRadiusPx(0)));
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

// A fluorescence slide: dark noise, nothing at all left of column 40 (outside
// the scan), an ellipse bright in channel 0 only and one bright in channel 2
// only; every sample times @p gain.
const Ellipse kFluorescenceA{180.0, 200.0, 120.0, 90.0};
const Ellipse kFluorescenceB{440.0, 200.0, 100.0, 120.0};

constexpr int64_t kFluorescenceWidth = 600;
constexpr int64_t kFluorescenceHeight = 400;

// A sample of the fluorescence slide at gain 1.
static double fluorescenceValue(int channel, int64_t x, int64_t y) {
    if (x < 40) {
        return 0.0;
    }
    if ((channel == 0 && kFluorescenceA.contains(x, y)) || (channel == 2 && kFluorescenceB.contains(x, y))) {
        return 30.0 + static_cast<double>(hashOf(x, y) % 40);
    }
    return static_cast<double>(hashOf(x, y + 1000 * channel) % 5);
}

static std::shared_ptr<slideio::Scene> fluorescenceScene(slideio::DataType type, double gain) {
    return ts::makeSyntheticScene({{kFluorescenceWidth, kFluorescenceHeight, 0, 0}}, {type, type, type},
                                  [gain](int channel, int64_t x, int64_t y) {
                                      return gain * fluorescenceValue(channel, x, y);
                                  });
}

static PipelineSpec fluorescenceSpec(double cutoff, int32_t radius = 2) {
    return specOf(TissueDetectParameters()
                      .mode(TissueMode::Fluorescence)
                      .thresholdMethod(TissueThresholdMethod::Fixed)
                      .intensityThreshold(cutoff)
                      .closingRadiusPx(radius));
}

static double fluorescenceArea() { return kFluorescenceA.area() + kFluorescenceB.area(); }

TEST(TissueDetect, FluorescenceFixedFindsBothBlobs) {
    const std::vector<ObjectRecord> objects = ts::runEagerObjects(
        fluorescenceScene(slideio::DataType::DT_Byte, 1.0), fluorescenceSpec(4.5), 1000, 4);
    ASSERT_EQ(objects.size(), 2u);
    EXPECT_LE(std::fabs(areaOf(objects) - fluorescenceArea()), 0.01 * fluorescenceArea()) << areaOf(objects);
}

TEST(TissueDetect, FluorescenceFloat32Fixed) {
    const PipelineSpec spec = fluorescenceSpec(4.5);
    const std::vector<ObjectRecord> bytes =
        ts::runEagerObjects(fluorescenceScene(slideio::DataType::DT_Byte, 1.0), spec, 100, 4);
    ASSERT_EQ(bytes.size(), 2u);
    expectSameObjects(bytes, ts::runEagerObjects(fluorescenceScene(slideio::DataType::DT_Float32, 1.0), spec, 100, 4),
                      "Float32");
}

TEST(TissueDetect, FluorescenceUInt16Fixed) {
    const std::vector<ObjectRecord> objects = ts::runEagerObjects(
        fluorescenceScene(slideio::DataType::DT_UInt16, 256.0), fluorescenceSpec(4.5 * 256.0), 100, 4);
    ASSERT_EQ(objects.size(), 2u);
    EXPECT_LE(std::fabs(areaOf(objects) - fluorescenceArea()), 0.01 * fluorescenceArea()) << areaOf(objects);
}

TEST(TissueDetect, FluorescenceTilingInvariant) {
    const auto scene = fluorescenceScene(slideio::DataType::DT_Byte, 1.0);
    for (const int32_t radius : {0, 2, 5}) {
        const PipelineSpec spec = fluorescenceSpec(4.5, radius);
        const std::vector<ObjectRecord> reference = ts::runEagerObjects(scene, spec, 1000, 4);
        ASSERT_FALSE(reference.empty()) << radius;
        for (const int32_t tile : {64, 100, 257}) {
            expectSameObjects(reference, ts::runEagerObjects(scene, spec, tile, 4),
                              "radius " + std::to_string(radius) + ", " + std::to_string(tile) + "-pixel tiles");
        }
        expectSameObjects(reference, ts::runEagerObjects(scene, spec, 100, 1),
                          "radius " + std::to_string(radius) + ", one worker");
    }
}

TEST(TissueDetect, SingleChannelSelection) {
    PipelineSpec spec = fluorescenceSpec(4.5);
    spec.channels(ChannelSelection::single(2));
    const std::vector<ObjectRecord> objects =
        ts::runEagerObjects(fluorescenceScene(slideio::DataType::DT_Byte, 1.0), spec, 100, 4);
    ASSERT_EQ(objects.size(), 1u);
    EXPECT_LE(std::fabs(areaOf(objects) - kFluorescenceB.area()), 0.01 * kFluorescenceB.area());
}

TEST(TissueDetect, FluorescenceZeroPlanesRefused) {
    const OperationPtr op = makeTissueDetect(TissueDetectParameters().mode(TissueMode::Fluorescence));
    EXPECT_THROW(op->outputPlaneCount(0), ChannelArityError);
    EXPECT_EQ(op->outputPlaneCount(1), 1);
    EXPECT_EQ(op->outputPlaneCount(5), 1);
    EXPECT_TRUE(op->supportedTypes() ==
                DataTypeSet().add(DataType::Byte).add(DataType::UInt16).add(DataType::Float32));
    EXPECT_TRUE(makeTissueDetect(TissueDetectParameters())->supportedTypes() == op->supportedTypes());
}

TEST(TissueDetect, FluorescenceFixedIsStrictlyAbove) {
    // The background's maximum is 4: a cut-off of 4 leaves it out.
    const std::vector<ObjectRecord> objects = ts::runEagerObjects(
        fluorescenceScene(slideio::DataType::DT_Byte, 1.0), fluorescenceSpec(4.0), 100, 4);
    ASSERT_EQ(objects.size(), 2u);
    EXPECT_LE(std::fabs(areaOf(objects) - fluorescenceArea()), 0.01 * fluorescenceArea()) << areaOf(objects);
}

TEST(TissueDetect, FluorescenceNaNSamplesIgnored) {
    // A NaN sample is left out of the maximum, in any plane; a pixel whose
    // samples are all NaN is background.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    struct Pixel {
        double samples[2];
        bool tissue;
    };
    const Pixel pixels[] = {
        {{nan, 50.0}, true}, {{50.0, nan}, true}, {{nan, nan}, false}, {{nan, 1.0}, false}, {{1.0, nan}, false},
    };
    const OperationPtr op = makeTissueDetect(TissueDetectParameters()
                                                 .mode(TissueMode::Fluorescence)
                                                 .thresholdMethod(TissueThresholdMethod::Fixed)
                                                 .intensityThreshold(4.5)
                                                 .closingRadiusPx(0));
    const int64_t count = static_cast<int64_t>(std::size(pixels));
    const ts::Image out =
        ts::applyDirect(*op, {DataType::Float32, DataType::Float32}, {0, 1}, 3 * count, 3,
                        [&](int32_t channel, int64_t x, int64_t y) {
                            return x % 3 == 1 && y == 1 ? pixels[x / 3].samples[channel] : 0.0;
                        });
    for (int64_t i = 0; i < count; ++i) {
        for (int64_t y = 0; y < 3; ++y) {
            for (int64_t x = 3 * i; x < 3 * i + 3; ++x) {
                const bool centre = x == 3 * i + 1 && y == 1;
                EXPECT_EQ(out.at(0, x, y), centre && pixels[i].tissue ? 255.0 : 0.0) << "pixel " << i;
            }
        }
    }
}

TEST(TissueDetect, FluorescenceBeyondLevelIsBackground) {
    // Every sample is at least 0 and the cut-off is -1, so the whole level is
    // tissue: one object of exactly the level's area, whatever the tiling.
    constexpr int64_t width = 150;
    constexpr int64_t height = 130;
    const std::vector<slideio::DataType> types{slideio::DataType::DT_Float32, slideio::DataType::DT_Float32};
    const auto noise = [](int channel, int64_t x, int64_t y) {
        return static_cast<double>(hashOf(x, y + 1000 * channel) % 5);
    };
    const PipelineSpec spec = fluorescenceSpec(-1.0);
    const auto whole = ts::makeSyntheticScene({{width, height, 0, 0}}, types, noise);
    const std::vector<ObjectRecord> reference = ts::runEagerObjects(whole, spec, 1000, 4);
    ASSERT_EQ(reference.size(), 1u);
    EXPECT_EQ(measurement(reference[0], "area"), static_cast<double>(width * height));
    EXPECT_EQ(reference[0].geometry().ringCount(), 1);
    for (const int32_t tile : {64, 100}) {
        expectSameObjects(reference, ts::runEagerObjects(whole, spec, tile, 4), std::to_string(tile) + "-pixel tiles");
    }

    // A 3 x 3 patch below the cut-off on the level's left edge: beyond the
    // level is background, so the closing does not fill it; its column on the
    // edge, at least, stays out.
    const auto notched =
        ts::makeSyntheticScene({{width, height, 0, 0}}, types, [noise](int channel, int64_t x, int64_t y) {
            return x < 3 && y >= 60 && y < 63 ? -5.0 : noise(channel, x, y);
        });
    const std::vector<ObjectRecord> notch = ts::runEagerObjects(notched, spec, 1000, 4);
    ASSERT_EQ(notch.size(), 1u);
    const double area = measurement(notch[0], "area");
    EXPECT_LE(area, static_cast<double>(width * height - 3));
    EXPECT_GE(area, static_cast<double>(width * height - 9));
    for (const int32_t tile : {64, 100}) {
        expectSameObjects(notch, ts::runEagerObjects(notched, spec, tile, 4),
                          "notched, " + std::to_string(tile) + "-pixel tiles");
    }
}

static PipelineSpec triangleSpec(int32_t radius = 2) {
    return specOf(TissueDetectParameters()
                      .mode(TissueMode::Fluorescence)
                      .thresholdMethod(TissueThresholdMethod::Triangle)
                      .closingRadiusPx(radius));
}

// The fluorescence slide's maximum over its channels at gain 1.
static cv::Mat fluorescenceMaximum() {
    cv::Mat image(static_cast<int>(kFluorescenceHeight), static_cast<int>(kFluorescenceWidth), CV_8UC1);
    for (int64_t y = 0; y < kFluorescenceHeight; ++y) {
        for (int64_t x = 0; x < kFluorescenceWidth; ++x) {
            double maximum = 0.0;
            for (int channel = 0; channel < 3; ++channel) {
                maximum = std::max(maximum, fluorescenceValue(channel, x, y));
            }
            image.at<uint8_t>(static_cast<int>(y), static_cast<int>(x)) = static_cast<uint8_t>(maximum);
        }
    }
    return image;
}

// The 256-bin histogram of an 8-bit image, over the pixels @p admitted accepts.
static std::vector<int64_t> histogramOf(const cv::Mat& image,
                                        const std::function<bool(int64_t, int64_t)>& admitted = nullptr) {
    std::vector<int64_t> bins(256, 0);
    for (int y = 0; y < image.rows; ++y) {
        for (int x = 0; x < image.cols; ++x) {
            if (!admitted || admitted(x, y)) {
                ++bins[image.at<uint8_t>(y, x)];
            }
        }
    }
    return bins;
}

TEST(TissueDetect, TriangleMatchesOpenCv) {
    constexpr int rows = 150;
    constexpr int cols = 200;
    const auto make = [](const std::function<int(int, int)>& value) {
        cv::Mat image(rows, cols, CV_8UC1);
        for (int y = 0; y < rows; ++y) {
            for (int x = 0; x < cols; ++x) {
                image.at<uint8_t>(y, x) = static_cast<uint8_t>(value(x, y));
            }
        }
        return image;
    };
    cv::Mat uniform(rows, cols, CV_8UC1);
    cv::RNG rng(12345);
    rng.fill(uniform, cv::RNG::UNIFORM, 0, 256);
    // A peak at 12 to 18 and a long tail up to 251: the peak is nearer the
    // left bound, so the histogram is flipped.
    const auto darkPeak = [](int x, int y) {
        const uint32_t h = hashOf(x, y);
        return h % 100 < 70 ? 12 + static_cast<int>(h % 7) : 12 + static_cast<int>((h >> 8) % 240);
    };
    const struct {
        const char* name;
        cv::Mat image;
    } cases[] = {
        {"random uniform", uniform},
        {"dark peak, bright tail", make(darkPeak)},
        {"bright peak, dark tail", make([&](int x, int y) { return 255 - darkPeak(x, y); })},
        {"two values", make([](int x, int y) { return hashOf(x, y) % 10 < 7 ? 50 : 200; })},
        {"one value", make([](int, int) { return 77; })},
        {"zero only", make([](int, int) { return 0; })},
        {"255 only", make([](int, int) { return 255; })},
        {"fluorescence maximum", fluorescenceMaximum()},
    };
    for (const auto& c : cases) {
        cv::Mat dst;
        const double expected = cv::threshold(c.image, dst, 0, 255, cv::THRESH_BINARY | cv::THRESH_TRIANGLE);
        EXPECT_EQ(detail::triangleThresholdBin(histogramOf(c.image)), static_cast<int64_t>(expected)) << c.name;
    }
    // OpenCV's answer for a single value v is v - 1: every pixel is above it.
    EXPECT_EQ(detail::triangleThresholdBin(histogramOf(make([](int, int) { return 77; }))), 76);

    // Value v from 0 to 9 on v + 1 pixels in each run of 55: a histogram
    // rising straight to its peak, no bin above the line, so the cut-off is
    // the left bound 0 less one, -1, and every pixel is above it.
    cv::Mat ramp(100, 55, CV_8UC1);
    for (int y = 0; y < ramp.rows; ++y) {
        int v = 0;
        for (int x = 0; x < ramp.cols; ++x) {
            if ((v + 1) * (v + 2) / 2 <= x) {
                ++v;
            }
            ramp.at<uint8_t>(y, x) = static_cast<uint8_t>(v);
        }
    }
    cv::Mat dst;
    const double expected = cv::threshold(ramp, dst, 0, 255, cv::THRESH_BINARY | cv::THRESH_TRIANGLE);
    EXPECT_EQ(expected, -1.0);
    EXPECT_EQ(detail::triangleThresholdBin(histogramOf(ramp)), -1);
}

TEST(TissueDetect, TriangleAllZeroIsMinusOne) {
    EXPECT_EQ(detail::triangleThresholdBin(std::vector<int64_t>(256, 0)), -1);
    EXPECT_EQ(detail::triangleThresholdBin(std::vector<int64_t>(65536, 0)), -1);
    EXPECT_EQ(detail::triangleThresholdBin(std::vector<int64_t>()), -1);
}

TEST(TissueDetect, TriangleWithSmallZeroBand) {
    // The zeros left of column 40, a band of 1 pixel in 15, do not pull the
    // cut-off below the background's maximum of 4, nor does the tail lift it
    // into the blobs. Zeros that outweigh the glass do: see
    // TriangleDominantZeroAreaLowersCutoff.
    const int64_t bin = detail::triangleThresholdBin(histogramOf(fluorescenceMaximum()));
    EXPECT_GE(bin, 4);
    EXPECT_LE(bin, 29);
    const std::vector<ObjectRecord> objects =
        ts::runEagerObjects(fluorescenceScene(slideio::DataType::DT_Byte, 1.0), triangleSpec(2), 100, 4);
    ASSERT_EQ(objects.size(), 2u);
    EXPECT_LE(std::fabs(areaOf(objects) - fluorescenceArea()), 0.01 * fluorescenceArea()) << areaOf(objects);
}

TEST(TissueDetect, TriangleUInt16) {
    const std::vector<ObjectRecord> objects =
        ts::runEagerObjects(fluorescenceScene(slideio::DataType::DT_UInt16, 256.0), triangleSpec(2), 100, 4);
    ASSERT_EQ(objects.size(), 2u);
    EXPECT_LE(std::fabs(areaOf(objects) - fluorescenceArea()), 0.01 * fluorescenceArea()) << areaOf(objects);
}

// A one-channel fluorescence slide whose glass, 1 to 8, peaks at 3, below its
// maximum, with one ellipse of 30 to 69.
const Ellipse kQuantisedBlob{200.0, 150.0, 100.0, 70.0};
constexpr int64_t kQuantisedWidth = 400;
constexpr int64_t kQuantisedHeight = 300;

static int quantisedValue(int64_t x, int64_t y) {
    const uint32_t h = hashOf(x, y);
    if (kQuantisedBlob.contains(x, y)) {
        return 30 + static_cast<int>(h % 40);
    }
    static const uint32_t upTo[] = {8, 22, 52, 70, 82, 90, 96, 100};
    const uint32_t r = (h >> 8) % 100;
    int value = 1;
    while (r >= upTo[static_cast<size_t>(value - 1)]) {
        ++value;
    }
    return value;
}

TEST(TissueDetect, TriangleUInt16StepQuantised) {
    // The slide in Byte, and in UInt16 at gain 257, whose values are all
    // multiples of 257: the UInt16 histogram, reduced by the step, is the
    // Byte one, so its cut-off is the Byte cut-off times 257 and the objects
    // are the same. Unreduced, its empty bins next to the glass's peak would
    // put the cut-off at the peak and make most of the glass tissue.
    std::vector<int64_t> bins(256, 0);
    for (int64_t y = 0; y < kQuantisedHeight; ++y) {
        for (int64_t x = 0; x < kQuantisedWidth; ++x) {
            ++bins[static_cast<size_t>(quantisedValue(x, y))];
        }
    }
    const int64_t cutoff = detail::triangleThresholdBin(bins);
    ASSERT_GE(cutoff, 8);
    ASSERT_LT(cutoff, 30);
    const auto scene = [](slideio::DataType type, double gain) {
        return ts::makeSyntheticScene({{kQuantisedWidth, kQuantisedHeight, 0, 0}}, {type},
                                      [gain](int, int64_t x, int64_t y) { return gain * quantisedValue(x, y); });
    };
    const std::vector<ObjectRecord> bytes =
        ts::runEagerObjects(scene(slideio::DataType::DT_Byte, 1.0), triangleSpec(2), 100, 4);
    ASSERT_EQ(bytes.size(), 1u);
    EXPECT_LE(std::fabs(areaOf(bytes) - kQuantisedBlob.area()), 0.01 * kQuantisedBlob.area()) << areaOf(bytes);
    expectSameObjects(bytes,
                      ts::runEagerObjects(scene(slideio::DataType::DT_Byte, 1.0),
                                          fluorescenceSpec(static_cast<double>(cutoff)), 100, 4),
                      "Byte, Fixed at the triangle cut-off");

    const auto words = scene(slideio::DataType::DT_UInt16, 257.0);
    const std::vector<ObjectRecord> uint16 = ts::runEagerObjects(words, triangleSpec(2), 100, 4);
    expectSameObjects(bytes, uint16, "UInt16 at gain 257");
    // Fixed at the Byte cut-off times 257 gives the same objects: the UInt16
    // cut-off lies between it and the next multiple of 257, the same cut.
    expectSameObjects(uint16,
                      ts::runEagerObjects(words, fluorescenceSpec(static_cast<double>(cutoff) * 257.0), 100, 4),
                      "UInt16, Fixed at the Byte cut-off times 257");
}

// The fluorescence slide shifted right by 600 pixels, so the zeros left of it
// cover 640 of its 1200 columns: more than half the slide, outweighing the
// glass.
constexpr int64_t kZeroShift = 600;

static double dominantZeroValue(int channel, int64_t x, int64_t y) {
    return x < kZeroShift ? 0.0 : fluorescenceValue(channel, x - kZeroShift, y);
}

TEST(TissueDetect, TriangleDominantZeroAreaLowersCutoff) {
    // Pins today's behaviour, so changing it is deliberate: zeros at more than
    // half the counted pixels make bin 0 the peak, the cut-off falls below the
    // glass's maximum of 4, and the glass becomes tissue.
    constexpr int64_t width = kFluorescenceWidth + kZeroShift;
    std::vector<int64_t> bins(256, 0);
    int64_t zeros = 0;
    for (int64_t y = 0; y < kFluorescenceHeight; ++y) {
        for (int64_t x = 0; x < width; ++x) {
            double maximum = 0.0;
            for (int channel = 0; channel < 3; ++channel) {
                maximum = std::max(maximum, dominantZeroValue(channel, x, y));
            }
            ++bins[static_cast<size_t>(maximum)];
            zeros += maximum == 0.0 ? 1 : 0;
        }
    }
    ASSERT_GE(2 * zeros, width * kFluorescenceHeight);
    const int64_t cutoff = detail::triangleThresholdBin(bins);
    EXPECT_LT(cutoff, 4);

    const auto scene = ts::makeSyntheticScene(
        {{width, kFluorescenceHeight, 0, 0}},
        {slideio::DataType::DT_Byte, slideio::DataType::DT_Byte, slideio::DataType::DT_Byte}, &dominantZeroValue);
    const std::vector<ObjectRecord> objects = ts::runEagerObjects(scene, triangleSpec(2), 100, 4);
    // The scanned area, columns 640 on, is one object, glass and blobs alike
    // (with, at most, a stray pixel or two beside it).
    const double scanned = static_cast<double>((width - kZeroShift - 40) * kFluorescenceHeight);
    ASSERT_FALSE(objects.empty());
    double largest = 0.0;
    for (const ObjectRecord& r : objects) {
        largest = std::max(largest, measurement(r, "area"));
    }
    EXPECT_GE(largest, 0.95 * scanned) << largest;
    EXPECT_LE(areaOf(objects), scanned) << areaOf(objects);

    // The workarounds find only the blobs: Triangle restricted to the scanned
    // area, and Fixed.
    const Point64 points[] = {{kZeroShift + 40, 0}, {width, 0}, {width, kFluorescenceHeight},
                              {kZeroShift + 40, kFluorescenceHeight}};
    Polygon polygon;
    polygon.addRing(points, 4, false);
    PipelineSpec restricted = triangleSpec(2);
    restricted.restrictToPolygon(polygon);
    const std::vector<ObjectRecord> blobs = ts::runEagerObjects(scene, restricted, 100, 4);
    ASSERT_EQ(blobs.size(), 2u);
    EXPECT_LE(std::fabs(areaOf(blobs) - fluorescenceArea()), 0.01 * fluorescenceArea()) << areaOf(blobs);
    const std::vector<ObjectRecord> fixed = ts::runEagerObjects(scene, fluorescenceSpec(4.5), 100, 4);
    ASSERT_EQ(fixed.size(), 2u);
    EXPECT_LE(std::fabs(areaOf(fixed) - fluorescenceArea()), 0.01 * fluorescenceArea()) << areaOf(fixed);
}

TEST(TissueDetect, TriangleTilingAndWorkersInvariant) {
    const auto scene = fluorescenceScene(slideio::DataType::DT_Byte, 1.0);
    const PipelineSpec spec = triangleSpec(2);
    const std::vector<ObjectRecord> reference = ts::runEagerObjects(scene, spec, 1000, 4);
    ASSERT_EQ(reference.size(), 2u);
    for (const int32_t tile : {64, 100, 257}) {
        expectSameObjects(reference, ts::runEagerObjects(scene, spec, tile, 4), std::to_string(tile) + "-pixel tiles");
    }
    expectSameObjects(reference, ts::runEagerObjects(scene, spec, 100, 1), "one worker");
}

TEST(TissueDetect, TriangleRestrictedCountsOnlyAdmittedPixels) {
    // Ellipse A shrunk by 3 pixels: the polygon admits blob samples only,
    // 30 to 69, so its cut-off lies among them, where the slide's, set by the
    // background, lies below 30. Every polygon of ellipse A and the
    // background around it shares the slide's cut-off (its peak at 4, the
    // empty bins 5 to 29 decide it), so the background is left out here.
    std::vector<Point64> ring;
    const double a = kFluorescenceA.a - 3.0;
    const double b = kFluorescenceA.b - 3.0;
    constexpr int steps = 90;
    for (int i = 0; i < steps; ++i) {
        const double theta = 2.0 * kPi * i / steps;
        ring.push_back(Point64{std::llround(kFluorescenceA.cx + a * std::cos(theta)),
                               std::llround(kFluorescenceA.cy + b * std::sin(theta))});
    }
    Polygon polygon;
    polygon.addRing(ring.data(), static_cast<int64_t>(ring.size()), false);
    RasterMask mask(Rect64{0, 0, kFluorescenceWidth, kFluorescenceHeight}, 0, 1.0);
    rasteriseInto(mask, polygon);

    const cv::Mat maximum = fluorescenceMaximum();
    const int64_t inside = detail::triangleThresholdBin(
        histogramOf(maximum, [&mask](int64_t x, int64_t y) { return mask.test(Point64{x, y}); }));
    const int64_t whole = detail::triangleThresholdBin(histogramOf(maximum));
    // The restriction must change the cut-off, and the objects, or the test
    // proves nothing.
    ASSERT_GE(inside, 30) << whole;
    ASSERT_LT(inside, 69);
    ASSERT_LT(whole, 30);

    // Without a closing, the blob's samples above the cut-off form many
    // small objects, most anchored inside the polygon.
    const auto scene = fluorescenceScene(slideio::DataType::DT_Byte, 1.0);
    PipelineSpec fixed = fluorescenceSpec(static_cast<double>(inside), 0);
    fixed.restrictToPolygon(polygon);
    PipelineSpec fixedWhole = fluorescenceSpec(static_cast<double>(whole), 0);
    fixedWhole.restrictToPolygon(polygon);
    PipelineSpec triangle = triangleSpec(0);
    triangle.restrictToPolygon(polygon);
    const std::vector<ObjectRecord> expected = ts::runEagerObjects(scene, fixed, 100, 4);
    ASSERT_GT(expected.size(), 10u) << "cut-off " << inside;
    const std::vector<ObjectRecord> wrong = ts::runEagerObjects(scene, fixedWhole, 100, 4);
    ASSERT_TRUE(wrong.size() != expected.size() || areaOf(wrong) != areaOf(expected));
    for (const int32_t workers : {1, 4}) {
        expectSameObjects(expected, ts::runEagerObjects(scene, triangle, 100, workers),
                          "restricted, " + std::to_string(workers) + " workers");
    }
}

TEST(TissueDetect, EmptyHistogramFindsNothing) {
    // A polygon inside the band of zeros: only bin 0 is counted, the cut-off
    // is above it, and nothing is tissue.
    const Point64 points[] = {{5, 5}, {35, 5}, {35, 395}, {5, 395}};
    Polygon polygon;
    polygon.addRing(points, 4, false);
    PipelineSpec spec = triangleSpec(2);
    spec.restrictToPolygon(polygon);
    std::vector<ObjectRecord> objects;
    EXPECT_NO_THROW(objects = ts::runEagerObjects(fluorescenceScene(slideio::DataType::DT_Byte, 1.0), spec, 100, 4));
    EXPECT_TRUE(objects.empty());

    // Nothing counted at all: no cut-off, no tissue, and no failure.
    const OperationPtr op = makeTissueDetect(TissueDetectParameters().mode(TissueMode::Fluorescence));
    EXPECT_TRUE(op->needsGlobalContext());
    op->finalizeAccumulation();
    const ts::DirectObjects direct = ts::applyDirectObjects(*op, {DataType::Byte, DataType::Byte}, {0, 1}, 20, 20,
                                                            [](int32_t, int64_t, int64_t) { return 200.0; });
    EXPECT_TRUE(direct.objects.empty());
    EXPECT_TRUE(direct.fragments.empty());
}

TEST(TissueDetect, TriangleFloat32Refused) {
    // Refused when the run is planned, before any tile is read or counted.
    const Configuration config =
        ConfigurationBuilder().tileSize(100, 100).tileSizeFromSlide(false).workerCount(1).build();
    Pipeline pipeline(fluorescenceScene(slideio::DataType::DT_Float32, 1.0), config, triangleSpec(2));
    pipeline.bindLayer("objects", ts::objectsLayer());
    try {
        pipeline.validate();
        FAIL() << "validate() accepted Float32 input with Triangle";
    } catch (const UnsupportedDataTypeError& e) {
        const std::string message = e.what();
        EXPECT_NE(message.find("Fixed"), std::string::npos) << message;
        ASSERT_NE(e.operationName(), nullptr);
        EXPECT_STREQ(e.operationName(), kName);
        EXPECT_EQ(e.offendingType(), DataType::Float32);
    }
    // A run is refused the same way, not by a tile.
    EXPECT_THROW(
        ts::runEagerObjects(fluorescenceScene(slideio::DataType::DT_Float32, 1.0), triangleSpec(2), 100, 4),
        UnsupportedDataTypeError);
    // Fixed accepts Float32, as brightfield does, and Triangle Byte and UInt16.
    const OperationPtr triangle = makeTissueDetect(TissueDetectParameters().mode(TissueMode::Fluorescence));
    EXPECT_THROW(triangle->outputType(DataType::Float32), UnsupportedDataTypeError);
    EXPECT_EQ(triangle->outputType(DataType::Byte), DataType::Byte);
    EXPECT_EQ(triangle->outputType(DataType::UInt16), DataType::Byte);
    EXPECT_EQ(makeTissueDetect(TissueDetectParameters()
                                   .mode(TissueMode::Fluorescence)
                                   .thresholdMethod(TissueThresholdMethod::Fixed)
                                   .intensityThreshold(4.5))
                  ->outputType(DataType::Float32),
              DataType::Byte);
    EXPECT_EQ(makeTissueDetect(TissueDetectParameters())->outputType(DataType::Float32), DataType::Byte);
}

TEST(TissueDetect, TriangleUInt16TwoLevels) {
    // Zeros, more than twice the signal, and the two ellipses at exactly
    // 1000: the step reduction makes the histogram {0, 1}, whose triangle
    // cut-off, 2, scaled back to 2000 would leave every value out; the
    // unreduced histogram's cut-off lies between 0 and 1000. The objects are
    // those of the Byte slide at 0 and 200, and of Fixed at 500.
    const auto twoLevels = [](slideio::DataType type, double level) {
        return ts::makeSyntheticScene({{kFluorescenceWidth, kFluorescenceHeight, 0, 0}}, {type},
                                      [level](int, int64_t x, int64_t y) {
                                          return kFluorescenceA.contains(x, y) || kFluorescenceB.contains(x, y)
                                                     ? level
                                                     : 0.0;
                                      });
    };
    ASSERT_GT(static_cast<double>(kFluorescenceWidth * kFluorescenceHeight) - fluorescenceArea(),
              2.0 * fluorescenceArea());
    const std::vector<ObjectRecord> bytes =
        ts::runEagerObjects(twoLevels(slideio::DataType::DT_Byte, 200.0), triangleSpec(2), 100, 4);
    ASSERT_EQ(bytes.size(), 2u);
    EXPECT_LE(std::fabs(areaOf(bytes) - fluorescenceArea()), 0.01 * fluorescenceArea()) << areaOf(bytes);
    const auto words = twoLevels(slideio::DataType::DT_UInt16, 1000.0);
    const std::vector<ObjectRecord> uint16 = ts::runEagerObjects(words, triangleSpec(2), 100, 4);
    expectSameObjects(bytes, uint16, "UInt16 at 1000");
    expectSameObjects(ts::runEagerObjects(words, fluorescenceSpec(500.0), 100, 4), uint16, "UInt16, Fixed at 500");
}

TEST(TissueDetect, TriangleSingleValueRestricted) {
    // Zeros everywhere but a rectangle at one value, and a restriction that
    // admits exactly the rectangle: one counted value v, so the cut-off is
    // v - 1. The rectangle is tissue and the zeros outside the restriction,
    // never counted, are not.
    constexpr int64_t left = 100;
    constexpr int64_t top = 100;
    constexpr int64_t right = 300;
    constexpr int64_t bottom = 250;
    const Point64 points[] = {{left, top}, {right, top}, {right, bottom}, {left, bottom}};
    Polygon polygon;
    polygon.addRing(points, 4, false);
    PipelineSpec spec = triangleSpec(2);
    spec.restrictToPolygon(polygon);
    const struct {
        slideio::DataType type;
        double value;
    } cases[] = {{slideio::DataType::DT_Byte, 1.0}, {slideio::DataType::DT_UInt16, 1000.0}};
    for (const auto& c : cases) {
        const double value = c.value;
        const auto scene = ts::makeSyntheticScene(
            {{kFluorescenceWidth, kFluorescenceHeight, 0, 0}}, {c.type}, [value](int, int64_t x, int64_t y) {
                return x >= left && x < right && y >= top && y < bottom ? value : 0.0;
            });
        const std::vector<ObjectRecord> objects = ts::runEagerObjects(scene, spec, 100, 4);
        ASSERT_EQ(objects.size(), 1u) << value;
        EXPECT_EQ(measurement(objects[0], "area"), static_cast<double>((right - left) * (bottom - top))) << value;
        EXPECT_EQ(objects[0].geometry().ringCount(), 1) << value;
    }
}

TEST(TissueDetect, GlobalContextOnlyForTriangle) {
    EXPECT_FALSE(makeTissueDetect(TissueDetectParameters())->needsGlobalContext());
    EXPECT_FALSE(makeTissueDetect(TissueDetectParameters()
                                      .mode(TissueMode::Fluorescence)
                                      .thresholdMethod(TissueThresholdMethod::Fixed)
                                      .intensityThreshold(4.5))
                     ->needsGlobalContext());
    EXPECT_TRUE(makeTissueDetect(TissueDetectParameters().mode(TissueMode::Fluorescence))->needsGlobalContext());
}
