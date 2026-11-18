// Requirements: UR-PIX-2, UR-IMG-7, UR-ALG-6
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
#include <limits>
#include <string>
#include <utility>
#include <vector>

using namespace slideio::processor;
namespace ts = slideio::processor::testing_support;

namespace {

constexpr const char* kName = "pixel.stain_deconvolution";

struct Vec3 {
    double r;
    double g;
    double b;
};

constexpr Vec3 kHematoxylin{0.650, 0.704, 0.286};
constexpr Vec3 kEosin{0.072, 0.990, 0.105};
constexpr Vec3 kDab{0.268, 0.570, 0.776};

void expectVector(const StainVectors& stains, int32_t index, const Vec3& expected) {
    double r = -1.0;
    double g = -1.0;
    double b = -1.0;
    stains.getVector(index, r, g, b);
    EXPECT_EQ(r, expected.r) << index;
    EXPECT_EQ(g, expected.g) << index;
    EXPECT_EQ(b, expected.b) << index;
}

Vec3 unit(const Vec3& v) {
    const double length = std::sqrt(v.r * v.r + v.g * v.g + v.b * v.b);
    return {v.r / length, v.g / length, v.b / length};
}

// The message of the InvalidArgumentError creating the operation from @p p throws; "" when none.
std::string rejection(const OperationParameters& p) {
    try {
        OperationCatalog::instance().create(kName, p);
    } catch (const InvalidArgumentError& e) {
        return e.what();
    }
    return "";
}

double scenePixel(int channel, int64_t x, int64_t y) {
    return static_cast<double>((x * 7 + y * 13 + channel * 71 + (x * y) % 11) % 256);
}

// Runs the deconvolution over @p scene; returns the TileError's operation name, or "" when the run succeeds.
std::string runFailure(const std::shared_ptr<slideio::Scene>& scene, const ChannelSelection& selection,
                       std::string& cause) {
    PipelineSpec spec;
    spec.channels(selection);
    spec.then(makeStainDeconvolution(StainVectors::hematoxylinEosin()));
    try {
        ts::runEager(scene, spec, 128, 2);
    } catch (const TileError& e) {
        cause = e.cause();
        return e.operationName();
    }
    return "";
}

// Composes c1 * first + c2 * second for c1, c2 in {0, 0.1, ..., 1.0}, deconvolves
// it with @p stains and returns the largest error over the pixels whose every
// channel is at least 64.
double recoveryError(const StainVectors& stains, const Vec3& first, const Vec3& second, int64_t& checked) {
    const Vec3 v1 = unit(first);
    const Vec3 v2 = unit(second);
    const auto intensity = [&](int32_t plane, int64_t x, int64_t y) {
        const double c1 = static_cast<double>(x) / 10.0;
        const double c2 = static_cast<double>(y) / 10.0;
        const double a = plane == 0 ? v1.r : plane == 1 ? v1.g : v1.b;
        const double b = plane == 0 ? v2.r : plane == 1 ? v2.g : v2.b;
        return std::round(255.0 * std::pow(10.0, -(c1 * a + c2 * b)));
    };
    const OperationPtr op = makeStainDeconvolution(stains);
    const ts::Image out =
        ts::applyDirect(*op, {DataType::Byte, DataType::Byte, DataType::Byte}, {0, 1, 2}, 11, 11, intensity);
    EXPECT_EQ(out.types, (std::vector<DataType>{DataType::Float32, DataType::Float32}));
    double worst = 0.0;
    checked = 0;
    for (int64_t y = 0; y <= 10; ++y) {
        for (int64_t x = 0; x <= 10; ++x) {
            if (intensity(0, x, y) < 64 || intensity(1, x, y) < 64 || intensity(2, x, y) < 64) {
                continue;
            }
            ++checked;
            const double e1 = std::fabs(out.at(0, x, y) - static_cast<double>(x) / 10.0);
            const double e2 = std::fabs(out.at(1, x, y) - static_cast<double>(y) / 10.0);
            if (!(e1 <= worst)) {
                worst = e1;
            }
            if (!(e2 <= worst)) {
                worst = e2;
            }
        }
    }
    return worst;
}

} // namespace

TEST(StainVectors, Builtins) {
    const StainVectors he = StainVectors::hematoxylinEosin();
    ASSERT_EQ(he.count(), 2);
    expectVector(he, 0, kHematoxylin);
    expectVector(he, 1, kEosin);
    EXPECT_STREQ(he.stainName(0), "Hematoxylin");
    EXPECT_STREQ(he.stainName(1), "Eosin");

    const StainVectors hd = StainVectors::hematoxylinDab();
    ASSERT_EQ(hd.count(), 2);
    expectVector(hd, 0, kHematoxylin);
    expectVector(hd, 1, kDab);
    EXPECT_STREQ(hd.stainName(0), "Hematoxylin");
    EXPECT_STREQ(hd.stainName(1), "DAB");
}

TEST(StainVectors, SetGetAndLimits) {
    StainVectors stains;
    EXPECT_EQ(stains.count(), 0);
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    EXPECT_THROW(stains.getVector(0, r, g, b), InvalidArgumentError);
    EXPECT_THROW(stains.stainName(0), InvalidArgumentError);
    EXPECT_THROW(stains.setStainName(0, "x"), InvalidArgumentError);
    // The index is 0 to count(); count() appends.
    EXPECT_THROW(stains.setVector(1, 0.1, 0.2, 0.3), InvalidArgumentError);
    EXPECT_THROW(stains.setVector(-1, 0.1, 0.2, 0.3), InvalidArgumentError);
    stains.setVector(0, 0.1, 0.2, 0.3);
    EXPECT_EQ(stains.count(), 1);
    expectVector(stains, 0, {0.1, 0.2, 0.3});
    EXPECT_STREQ(stains.stainName(0), "");
    stains.setVector(0, 3.0, 0.0, 4.0); // replaces; stored as given, not normalised
    EXPECT_EQ(stains.count(), 1);
    expectVector(stains, 0, {3.0, 0.0, 4.0});
    stains.setVector(1, 0.0, 1.0, 0.0);
    stains.setVector(2, 0.0, 0.0, 1.0);
    EXPECT_EQ(stains.count(), 3);
    // At most three stains.
    EXPECT_THROW(stains.setVector(3, 0.1, 0.1, 0.1), InvalidArgumentError);
    EXPECT_EQ(stains.count(), 3);

    // Rejected components; the stored vector is unchanged.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    EXPECT_THROW(stains.setVector(0, -0.1, 0.5, 0.5), InvalidArgumentError);
    EXPECT_THROW(stains.setVector(0, nan, 0.5, 0.5), InvalidArgumentError);
    EXPECT_THROW(stains.setVector(0, 0.5, inf, 0.5), InvalidArgumentError);
    EXPECT_THROW(stains.setVector(0, 0.0, 0.0, 0.0), InvalidArgumentError);
    expectVector(stains, 0, {3.0, 0.0, 4.0});
    try {
        stains.setVector(0, -0.125, 0.5, 0.5);
    } catch (const InvalidArgumentError& e) {
        EXPECT_EQ(std::string(e.what()).find("0.125"), std::string::npos) << e.what();
    }

    // Names.
    stains.setStainName(1, "Green");
    EXPECT_STREQ(stains.stainName(1), "Green");
    EXPECT_THROW(stains.setStainName(1, nullptr), InvalidArgumentError);
    EXPECT_THROW(stains.setStainName(3, "x"), InvalidArgumentError);
    EXPECT_THROW(stains.setStainName(-1, "x"), InvalidArgumentError);
    EXPECT_THROW(stains.stainName(3), InvalidArgumentError);
    EXPECT_THROW(stains.getVector(3, r, g, b), InvalidArgumentError);
    EXPECT_THROW(stains.getVector(-1, r, g, b), InvalidArgumentError);

    // Copy, move and assignment.
    StainVectors copy(stains);
    copy.setStainName(1, "Changed");
    EXPECT_STREQ(stains.stainName(1), "Green");
    EXPECT_EQ(copy.count(), 3);
    expectVector(copy, 0, {3.0, 0.0, 4.0});

    StainVectors moved(std::move(copy));
    EXPECT_EQ(moved.count(), 3);
    EXPECT_STREQ(moved.stainName(1), "Changed");
    EXPECT_EQ(copy.count(), 0); // NOLINT(bugprone-use-after-move): moved-from is empty and reusable
    copy.setVector(0, 1.0, 1.0, 1.0);
    EXPECT_EQ(copy.count(), 1);

    StainVectors assigned;
    assigned = stains;
    EXPECT_EQ(assigned.count(), 3);
    EXPECT_STREQ(assigned.stainName(1), "Green");
    assigned = StainVectors::hematoxylinDab();
    EXPECT_EQ(assigned.count(), 2);
    EXPECT_STREQ(assigned.stainName(1), "DAB");
    assigned = std::move(moved);
    EXPECT_EQ(assigned.count(), 3);
    EXPECT_EQ(moved.count(), 0); // NOLINT(bugprone-use-after-move)
    EXPECT_THROW(moved.stainName(0), InvalidArgumentError);
    const StainVectors& self = assigned;
    assigned = self;
    EXPECT_EQ(assigned.count(), 3);
}

TEST(StainDeconvolution, FactoryEqualsNamePath) {
    // The default is H&E.
    const OperationPtr byDefault = OperationCatalog::instance().create(kName, OperationParameters());
    const OperationPtr he = makeStainDeconvolution(StainVectors::hematoxylinEosin());
    EXPECT_TRUE(detail::parametersEqual(byDefault->parameters(), he->parameters()));
    const OperationParameters& bag = he->parameters();
    EXPECT_EQ(bag.count(), 3);
    double values[9] = {};
    ASSERT_EQ(bag.getDoubleArray("vectors", values, 9), 6);
    EXPECT_EQ(values[0], 0.650);
    EXPECT_EQ(values[4], 0.990);
    EXPECT_STREQ(bag.getString("stainName0", nullptr), "Hematoxylin");
    EXPECT_STREQ(bag.getString("stainName1", nullptr), "Eosin");

    // A typed operation and the same bag by name are equal, names included.
    StainVectors three;
    three.setVector(0, 0.650, 0.704, 0.286);
    three.setVector(1, 0.072, 0.990, 0.105);
    three.setVector(2, 0.268, 0.570, 0.776);
    three.setStainName(2, "DAB");
    const OperationPtr typed = makeStainDeconvolution(three);
    EXPECT_STREQ(typed->parameters().getString("stainName0", nullptr), "");
    EXPECT_STREQ(typed->parameters().getString("stainName2", nullptr), "DAB");
    const OperationPtr byName = OperationCatalog::instance().create(kName, typed->parameters());
    EXPECT_TRUE(detail::parametersEqual(typed->parameters(), byName->parameters()));
    EXPECT_EQ(typed->outputPlaneCount(3), 3);
    EXPECT_EQ(he->outputPlaneCount(3), 2);
    const OperationPtr hd = makeStainDeconvolution(StainVectors::hematoxylinDab());
    EXPECT_TRUE(detail::parametersEqual(
        OperationCatalog::instance().create(kName, hd->parameters())->parameters(), hd->parameters()));

    // Names are optional by name: an absent one reads as "".
    const double two[6] = {0.650, 0.704, 0.286, 0.268, 0.570, 0.776};
    const OperationPtr unnamed =
        OperationCatalog::instance().create(kName, OperationParameters().setDoubleArray("vectors", two, 6));
    EXPECT_STREQ(unnamed->parameters().getString("stainName0", nullptr), "");
    EXPECT_STREQ(unnamed->parameters().getString("stainName1", nullptr), "");

    // The declarations.
    EXPECT_STREQ(he->name(), kName);
    EXPECT_EQ(he->arity(), ChannelArity::Joint);
    EXPECT_TRUE(he->requiresUniformTypes());
    EXPECT_TRUE(he->supportedTypes() == DataTypeSet().add(DataType::Byte));
    EXPECT_TRUE(he->halo().isEmpty());
    EXPECT_FALSE(he->producesObjects());
    EXPECT_EQ(he->outputType(DataType::Byte), DataType::Float32);
    EXPECT_STREQ(OperationCatalog::instance().correctnessReference(kName),
                 "Ruifrok AC, Johnston DA. Quantification of histochemical staining by color deconvolution. Anal "
                 "Quant Cytol Histol 2001;23:291-299; synthetic images composed from known vectors");
    EXPECT_STREQ(OperationCatalog::instance().complexityNote(kName), "O(n) in the number of pixels");

    // Bad bags throw, naming the key and not the value.
    const double four[4] = {0.1, 0.2, 0.3, 0.4};
    const std::string badCount = rejection(OperationParameters().setDoubleArray("vectors", four, 4));
    EXPECT_NE(badCount.find("vectors"), std::string::npos) << badCount;
    EXPECT_NE(badCount.find(kName), std::string::npos) << badCount;
    const double twelve[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 1};
    EXPECT_NE(rejection(OperationParameters().setDoubleArray("vectors", twelve, 12)).find("vectors"),
              std::string::npos);
    EXPECT_NE(rejection(OperationParameters().setDoubleArray("vectors", nullptr, 0)).find("vectors"),
              std::string::npos);
    const double negative[6] = {0.650, -0.704, 0.286, 0.268, 0.570, 0.776};
    const std::string badVector = rejection(OperationParameters().setDoubleArray("vectors", negative, 6));
    EXPECT_NE(badVector.find("vectors"), std::string::npos) << badVector;
    EXPECT_EQ(badVector.find("0.704"), std::string::npos) << badVector;
    EXPECT_NE(rejection(OperationParameters().setDouble("vectors", 1.0)).find("vectors"), std::string::npos);
    EXPECT_NE(rejection(OperationParameters().setString("vectors", "he")).find("vectors"), std::string::npos);
    EXPECT_NE(rejection(OperationParameters().setInt("stainName0", 1)).find("stainName0"), std::string::npos);
    const std::string extra = rejection(OperationParameters().setString("stainName2", "x"));
    EXPECT_NE(extra.find("stainName2"), std::string::npos) << extra;
    const std::string unknown = rejection(OperationParameters().setString("vector", "x"));
    EXPECT_NE(unknown.find("vector"), std::string::npos) << unknown;

    // Fewer than two stains are refused.
    EXPECT_THROW(makeStainDeconvolution(StainVectors()), InvalidArgumentError);
    StainVectors one;
    one.setVector(0, 0.650, 0.704, 0.286);
    EXPECT_THROW(makeStainDeconvolution(one), InvalidArgumentError);
}

TEST(StainDeconvolution, RejectsDependentVectors) {
    StainVectors same;
    same.setVector(0, 0.650, 0.704, 0.286);
    same.setVector(1, 0.650, 0.704, 0.286);
    EXPECT_THROW(makeStainDeconvolution(same), InvalidArgumentError);
    // Parallel but of different length: dependent once normalised.
    StainVectors scaled;
    scaled.setVector(0, 0.650, 0.704, 0.286);
    scaled.setVector(1, 1.300, 1.408, 0.572);
    EXPECT_THROW(makeStainDeconvolution(scaled), InvalidArgumentError);
    // Three vectors in one plane.
    StainVectors planar;
    planar.setVector(0, 1.0, 0.0, 0.0);
    planar.setVector(1, 0.0, 1.0, 0.0);
    planar.setVector(2, 1.0, 1.0, 0.0);
    try {
        makeStainDeconvolution(planar);
        FAIL() << "no InvalidArgumentError";
    } catch (const InvalidArgumentError& e) {
        EXPECT_NE(std::string(e.what()).find(kName), std::string::npos) << e.what();
    }
    // By name too.
    const double equal[6] = {0.650, 0.704, 0.286, 0.650, 0.704, 0.286};
    EXPECT_FALSE(rejection(OperationParameters().setDoubleArray("vectors", equal, 6)).empty());
}

TEST(StainDeconvolution, RecoversComposedIntensities) {
    int64_t checked = 0;
    const double heError =
        recoveryError(StainVectors::hematoxylinEosin(), kHematoxylin, kEosin, checked);
    EXPECT_LE(heError, 0.01);
    EXPECT_GT(checked, 20);
    const double hdError = recoveryError(StainVectors::hematoxylinDab(), kHematoxylin, kDab, checked);
    EXPECT_LE(hdError, 0.01);
    EXPECT_GT(checked, 20);

    // White (255 everywhere) is no stain, and 0 reads as 1 rather than infinity.
    const OperationPtr op = makeStainDeconvolution(StainVectors::hematoxylinEosin());
    const ts::Image white = ts::applyDirect(*op, {DataType::Byte, DataType::Byte, DataType::Byte}, {0, 1, 2}, 2, 1,
                                            [](int32_t, int64_t x, int64_t) { return x == 0 ? 255.0 : 0.0; });
    EXPECT_NEAR(white.at(0, 0, 0), 0.0, 1e-6);
    EXPECT_NEAR(white.at(1, 0, 0), 0.0, 1e-6);
    EXPECT_TRUE(std::isfinite(white.at(0, 1, 0)));
    EXPECT_TRUE(std::isfinite(white.at(1, 1, 0)));

    // Three stains: every plane is written.
    StainVectors three = StainVectors::hematoxylinEosin();
    three.setVector(2, kDab.r, kDab.g, kDab.b);
    const OperationPtr threeOp = makeStainDeconvolution(three);
    const Vec3 h = unit(kHematoxylin);
    const Vec3 d = unit(kDab);
    const auto composed = [&](int32_t plane, int64_t, int64_t) {
        const double od = plane == 0 ? 0.3 * h.r + 0.2 * d.r : plane == 1 ? 0.3 * h.g + 0.2 * d.g : 0.3 * h.b + 0.2 * d.b;
        return std::round(255.0 * std::pow(10.0, -od));
    };
    const ts::Image out =
        ts::applyDirect(*threeOp, {DataType::Byte, DataType::Byte, DataType::Byte}, {0, 1, 2}, 1, 1, composed);
    ASSERT_EQ(out.types.size(), 3u);
    EXPECT_NEAR(out.at(0, 0, 0), 0.3, 0.01);
    EXPECT_NEAR(out.at(1, 0, 0), 0.0, 0.01);
    EXPECT_NEAR(out.at(2, 0, 0), 0.2, 0.01);
}

TEST(StainDeconvolution, RequiresRgb) {
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

    // A UInt16 scene is refused when the pipeline validates the chain, since
    // the operation declares Byte only, before any tile runs.
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
