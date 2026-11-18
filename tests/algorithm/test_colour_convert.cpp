// Requirements: UR-PIX-1, UR-IMG-7, UR-ALG-3, UR-ALG-6
#include <slideio/processor/algorithm/pixel/colour.hpp>
#include <slideio/processor/core/channel_selection.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/pipeline/operation_catalog.hpp>
#include <slideio/processor/pipeline/pipeline_spec.hpp>

#include <algorithm/support/harness.hpp>
#include <pipeline/detail/parameters_json.hpp>
#include <pipeline/test_support/synthetic_scene.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace slideio::processor;
namespace ts = slideio::processor::testing_support;

namespace {

constexpr const char* kName = "pixel.colour_convert";

// The message of the InvalidArgumentError creating the operation from @p p throws; "" when none.
std::string rejection(const OperationParameters& p) {
    try {
        OperationCatalog::instance().create(kName, p);
    } catch (const InvalidArgumentError& e) {
        return e.what();
    }
    return "";
}

// Converts one RGB Byte pixel and returns the output planes' samples.
std::vector<double> convertPixel(ColourSpace to, double r, double g, double b) {
    const OperationPtr op = makeColourConvert(ColourSpace::Rgb, to);
    const double rgb[3] = {r, g, b};
    const ts::Image out = ts::applyDirect(*op, {DataType::Byte, DataType::Byte, DataType::Byte}, {0, 1, 2}, 1, 1,
                                          [&](int32_t plane, int64_t, int64_t) { return rgb[plane]; });
    std::vector<double> values;
    for (size_t plane = 0; plane < out.planes.size(); ++plane) {
        values.push_back(out.at(static_cast<int32_t>(plane), 0, 0));
    }
    return values;
}

// The level of the grid step i in [0, 64): 0 to 255 inclusive.
double gridLevel(int64_t i) {
    return std::round(static_cast<double>(i) * 255.0 / 63.0);
}

// Every RGB triple of the 64-step grid, one per pixel of a 512 x 512 image.
double gridSample(int32_t plane, int64_t x, int64_t y) {
    const int64_t index = y * 512 + x;
    const int64_t step = plane == 0 ? index % 64 : plane == 1 ? (index / 64) % 64 : index / 4096;
    return gridLevel(step);
}

// Converts the grid Rgb -> @p via -> Rgb; returns the largest absolute error.
double roundTripError(ColourSpace via) {
    const std::vector<DataType> byteRgb{DataType::Byte, DataType::Byte, DataType::Byte};
    const OperationPtr forward = makeColourConvert(ColourSpace::Rgb, via);
    const ts::Image middle = ts::applyDirect(*forward, byteRgb, {0, 1, 2}, 512, 512, &gridSample);
    EXPECT_EQ(middle.types, (std::vector<DataType>{DataType::Float32, DataType::Float32, DataType::Float32}));
    const OperationPtr backward = makeColourConvert(via, ColourSpace::Rgb);
    const ts::Image back = ts::applyDirect(*backward, middle.types, {0, 1, 2}, 512, 512,
                                           [&](int32_t plane, int64_t x, int64_t y) { return middle.at(plane, x, y); });
    EXPECT_EQ(back.types, middle.types);
    double worst = 0.0;
    for (int32_t plane = 0; plane < 3; ++plane) {
        for (int64_t y = 0; y < 512; ++y) {
            for (int64_t x = 0; x < 512; ++x) {
                const double error = std::fabs(back.at(plane, x, y) - gridSample(plane, x, y));
                if (!(error <= worst)) {
                    worst = error;
                }
            }
        }
    }
    return worst;
}

double scenePixel(int channel, int64_t x, int64_t y) {
    return static_cast<double>((x * 7 + y * 13 + channel * 71 + (x * y) % 11) % 256);
}

// Runs the conversion over @p scene; returns the TileError's operation name, or "" when the run succeeds.
std::string runFailure(const std::shared_ptr<slideio::Scene>& scene, const ChannelSelection& selection,
                       std::string& cause) {
    PipelineSpec spec;
    spec.channels(selection);
    spec.then(makeColourConvert(ColourSpace::Rgb, ColourSpace::Grey));
    try {
        ts::runEager(scene, spec, 128, 2);
    } catch (const TileError& e) {
        cause = e.cause();
        return e.operationName();
    }
    return "";
}

} // namespace

TEST(ColourConvert, FactoryEqualsNamePath) {
    const char* spellings[] = {"Rgb", "Grey", "Hsv", "Lab"};
    for (int32_t from = 0; from < 4; ++from) {
        for (int32_t to = 0; to < 4; ++to) {
            if (from == to) {
                continue;
            }
            OperationParameters p;
            p.setString("from", spellings[from]).setString("to", spellings[to]);
            const OperationPtr byName = OperationCatalog::instance().create(kName, p);
            const OperationPtr typed = makeColourConvert(static_cast<ColourSpace>(from), static_cast<ColourSpace>(to));
            EXPECT_TRUE(detail::parametersEqual(typed->parameters(), byName->parameters()))
                << spellings[from] << " -> " << spellings[to];
            EXPECT_EQ(typed->outputPlaneCount(3), to == 1 ? 1 : 3);
            EXPECT_EQ(typed->outputPlaneCount(1), to == 1 ? 1 : 3);
        }
    }

    // Defaults fill the bag.
    const OperationPtr byDefault = OperationCatalog::instance().create(kName, OperationParameters());
    const OperationParameters& bag = byDefault->parameters();
    EXPECT_EQ(bag.count(), 2);
    EXPECT_STREQ(bag.getString("from", nullptr), "Rgb");
    EXPECT_STREQ(bag.getString("to", nullptr), "Grey");
    EXPECT_TRUE(detail::parametersEqual(
        OperationCatalog::instance().create(kName, OperationParameters().setString("to", "Lab"))->parameters(),
        makeColourConvert(ColourSpace::Rgb, ColourSpace::Lab)->parameters()));

    // The declarations.
    const OperationPtr op = makeColourConvert(ColourSpace::Rgb, ColourSpace::Hsv);
    EXPECT_STREQ(op->name(), kName);
    EXPECT_EQ(op->arity(), ChannelArity::Joint);
    EXPECT_TRUE(op->requiresUniformTypes());
    EXPECT_TRUE(op->supportedTypes() == DataTypeSet().add(DataType::Byte).add(DataType::Float32));
    EXPECT_TRUE(op->halo().isEmpty());
    EXPECT_FALSE(op->producesObjects());
    EXPECT_EQ(op->outputType(DataType::Byte), DataType::Float32);
    EXPECT_EQ(op->outputType(DataType::Float32), DataType::Float32);
    EXPECT_STREQ(OperationCatalog::instance().correctnessReference(kName),
                 "Analytic: IEC 61966-2-1 sRGB, CIE 1976 L*a*b* with D65 white, hexcone HSV, ITU-R BT.601 luma; round "
                 "trips within 1e-3 on the 0-255 scale");
    EXPECT_STREQ(OperationCatalog::instance().complexityNote(kName), "O(n) in the number of pixels");

    // Unknown spellings, wrong value types and unknown keys throw, naming the key and not the value.
    const std::string bad = rejection(OperationParameters().setString("to", "Xyz"));
    EXPECT_NE(bad.find("to"), std::string::npos) << bad;
    EXPECT_NE(bad.find(kName), std::string::npos) << bad;
    EXPECT_EQ(bad.find("Xyz"), std::string::npos) << bad;
    EXPECT_FALSE(rejection(OperationParameters().setInt("from", 0)).empty());
    const std::string unknown = rejection(OperationParameters().setString("form", "Rgb"));
    EXPECT_NE(unknown.find("form"), std::string::npos) << unknown;
    EXPECT_THROW(makeColourConvert(static_cast<ColourSpace>(7), ColourSpace::Grey), InvalidArgumentError);
    EXPECT_THROW(makeColourConvert(ColourSpace::Rgb, static_cast<ColourSpace>(-1)), InvalidArgumentError);
}

TEST(ColourConvert, SameSpaceRejected) {
    for (int32_t space = 0; space < 4; ++space) {
        EXPECT_THROW(makeColourConvert(static_cast<ColourSpace>(space), static_cast<ColourSpace>(space)),
                     InvalidArgumentError);
    }
    const std::string message = rejection(OperationParameters().setString("from", "Grey").setString("to", "Grey"));
    EXPECT_NE(message.find(kName), std::string::npos) << message;
    EXPECT_EQ(message.find("Grey"), std::string::npos) << message;
}

TEST(ColourConvert, KnownValues) {
    const std::vector<double> hsv = convertPixel(ColourSpace::Hsv, 255, 0, 0);
    ASSERT_EQ(hsv.size(), 3u);
    EXPECT_NEAR(hsv[0], 0.0, 1e-3);
    EXPECT_NEAR(hsv[1], 1.0, 1e-3);
    EXPECT_NEAR(hsv[2], 1.0, 1e-3);

    const std::vector<double> lab = convertPixel(ColourSpace::Lab, 255, 0, 0);
    ASSERT_EQ(lab.size(), 3u);
    EXPECT_NEAR(lab[0], 53.2408, 1e-3);
    EXPECT_NEAR(lab[1], 80.0925, 1e-3);
    EXPECT_NEAR(lab[2], 67.2032, 1e-3);

    const std::vector<double> white = convertPixel(ColourSpace::Lab, 255, 255, 255);
    EXPECT_NEAR(white[0], 100.0, 1e-3);
    EXPECT_NEAR(white[1], 0.0, 1e-3);
    EXPECT_NEAR(white[2], 0.0, 1e-3);

    const std::vector<double> grey = convertPixel(ColourSpace::Grey, 10, 20, 30);
    ASSERT_EQ(grey.size(), 1u);
    EXPECT_NEAR(grey[0], 18.15, 1e-4);

    // Grey: S = 0 gives H = 0.
    const std::vector<double> neutral = convertPixel(ColourSpace::Hsv, 128, 128, 128);
    EXPECT_EQ(neutral[0], 0.0);
    EXPECT_EQ(neutral[1], 0.0);
    EXPECT_NEAR(neutral[2], 128.0 / 255.0, 1e-6);

    // A hue just below 360 stays in [0, 360).
    const std::vector<double> nearRed = convertPixel(ColourSpace::Hsv, 255, 0, 1);
    EXPECT_GE(nearRed[0], 0.0);
    EXPECT_LT(nearRed[0], 360.0);

    // Grey replicates into RGB, from Byte and from Float32.
    const OperationPtr greyToRgb = makeColourConvert(ColourSpace::Grey, ColourSpace::Rgb);
    for (DataType type : {DataType::Byte, DataType::Float32}) {
        const ts::Image rgb = ts::applyDirect(*greyToRgb, {type}, {0}, 1, 1, [](int32_t, int64_t, int64_t) { return 77.0; });
        ASSERT_EQ(rgb.types, (std::vector<DataType>{DataType::Float32, DataType::Float32, DataType::Float32}));
        for (int32_t plane = 0; plane < 3; ++plane) {
            EXPECT_EQ(rgb.at(plane, 0, 0), 77.0);
        }
    }

    // Out-of-gamut Lab gives RGB outside 0-255, unclamped.
    const OperationPtr labToRgb = makeColourConvert(ColourSpace::Lab, ColourSpace::Rgb);
    const double outOfGamut[3] = {50.0, 120.0, -120.0};
    const ts::Image wide = ts::applyDirect(*labToRgb, {DataType::Float32, DataType::Float32, DataType::Float32},
                                           {0, 1, 2}, 1, 1,
                                           [&](int32_t plane, int64_t, int64_t) { return outOfGamut[plane]; });
    const double lowest = std::fmin(wide.at(0, 0, 0), std::fmin(wide.at(1, 0, 0), wide.at(2, 0, 0)));
    const double highest = std::fmax(wide.at(0, 0, 0), std::fmax(wide.at(1, 0, 0), wide.at(2, 0, 0)));
    EXPECT_TRUE(lowest < 0.0 || highest > 255.0) << lowest << " " << highest;
}

TEST(ColourConvert, InputChecks) {
    const std::vector<DataType> threeFloat{DataType::Float32, DataType::Float32, DataType::Float32};
    const auto value = [](int32_t, int64_t, int64_t) { return 10.0; };
    // Hsv and Lab need three Float32 planes.
    const OperationPtr hsvToGrey = makeColourConvert(ColourSpace::Hsv, ColourSpace::Grey);
    EXPECT_THROW(ts::applyDirect(*hsvToGrey, {DataType::Float32}, {0}, 1, 1, value), ChannelArityError);
    EXPECT_THROW(ts::applyDirect(*hsvToGrey, {DataType::Byte, DataType::Byte, DataType::Byte}, {0, 1, 2}, 1, 1, value),
                 UnsupportedDataTypeError);
    EXPECT_NO_THROW(ts::applyDirect(*hsvToGrey, threeFloat, {0, 1, 2}, 1, 1, value));
    // Grey needs one plane.
    const OperationPtr greyToLab = makeColourConvert(ColourSpace::Grey, ColourSpace::Lab);
    EXPECT_THROW(ts::applyDirect(*greyToLab, threeFloat, {0, 1, 2}, 1, 1, value), ChannelArityError);
    // Rgb needs Byte channels 0, 1, 2.
    const OperationPtr rgbToHsv = makeColourConvert(ColourSpace::Rgb, ColourSpace::Hsv);
    EXPECT_THROW(ts::applyDirect(*rgbToHsv, threeFloat, {0, 1, 2}, 1, 1, value), InvalidArgumentError);
    EXPECT_THROW(ts::applyDirect(*rgbToHsv, {DataType::Byte, DataType::Byte}, {0, 1}, 1, 1, value), ChannelArityError);
    try {
        ts::applyDirect(*rgbToHsv, {DataType::Byte, DataType::Byte, DataType::Byte}, {1, 2, 3}, 1, 1, value);
        FAIL() << "no InvalidArgumentError";
    } catch (const InvalidArgumentError& e) {
        const std::string message = e.what();
        EXPECT_NE(message.find(kName), std::string::npos) << message;
        EXPECT_NE(message.find("needs channels 0, 1, 2 as RGB, Byte"), std::string::npos) << message;
        EXPECT_NE(message.find("1 (Byte), 2 (Byte), 3 (Byte)"), std::string::npos) << message;
    }
}

TEST(ColourConvert, RoundTrips) {
    EXPECT_LE(roundTripError(ColourSpace::Hsv), 1e-3);
    EXPECT_LE(roundTripError(ColourSpace::Lab), 1e-3);
}

TEST(ColourConvert, RgbIsInferredFromChannels) {
    const auto byte3 = ts::makeSyntheticScene({{300, 260, 0, 0}},
                                              {slideio::DataType::DT_Byte, slideio::DataType::DT_Byte,
                                               slideio::DataType::DT_Byte},
                                              &scenePixel);
    const auto byte4 = ts::makeSyntheticScene({{300, 260, 0, 0}},
                                              {slideio::DataType::DT_Byte, slideio::DataType::DT_Byte,
                                               slideio::DataType::DT_Byte, slideio::DataType::DT_Byte},
                                              &scenePixel);
    std::string cause;
    EXPECT_EQ(runFailure(byte3, ChannelSelection().add(2).add(1).add(0), cause), kName);
    EXPECT_NE(cause.find("needs channels 0, 1, 2 as RGB, Byte"), std::string::npos) << cause;
    EXPECT_EQ(runFailure(byte4, ChannelSelection::all(), cause), kName);
    EXPECT_NE(cause.find("needs channels 0, 1, 2 as RGB, Byte"), std::string::npos) << cause;
    EXPECT_EQ(runFailure(byte3, ChannelSelection().add(0).add(1).add(2), cause), "");

    // A UInt16 scene is refused too. The operation declares Byte and Float32
    // only, so the pipeline refuses it when it validates the chain, before
    // any tile runs, with an error naming the operation.
    const auto uint16 = ts::makeSyntheticScene({{300, 260, 0, 0}},
                                               {slideio::DataType::DT_UInt16, slideio::DataType::DT_UInt16,
                                                slideio::DataType::DT_UInt16},
                                               &scenePixel);
    try {
        runFailure(uint16, ChannelSelection::all(), cause);
        FAIL() << "the UInt16 scene ran";
    } catch (const UnsupportedDataTypeError& e) {
        EXPECT_NE(std::string(e.what()).find(kName), std::string::npos) << e.what();
    }
}

TEST(ColourConvert, TiledEqualsSingleTile) {
    const auto scene = ts::makeSyntheticScene({{300, 260, 0, 0}},
                                              {slideio::DataType::DT_Byte, slideio::DataType::DT_Byte,
                                               slideio::DataType::DT_Byte},
                                              &scenePixel);
    for (ColourSpace to : {ColourSpace::Grey, ColourSpace::Hsv, ColourSpace::Lab}) {
        PipelineSpec spec;
        spec.then(makeColourConvert(ColourSpace::Rgb, to));
        const ts::Image reference = ts::runEager(scene, spec, 4096, 1);
        ASSERT_EQ(reference.width, 300);
        ASSERT_EQ(reference.height, 260);
        ASSERT_EQ(reference.planes.size(), to == ColourSpace::Grey ? 1u : 3u);
        for (int32_t tile : {64, 257}) {
            const ts::Image tiled = ts::runEager(scene, spec, tile, 4);
            EXPECT_TRUE(tiled.planes == reference.planes) << "tile " << tile;
        }
    }
}
