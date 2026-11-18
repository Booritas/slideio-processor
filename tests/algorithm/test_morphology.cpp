// Requirements: UR-PIX-4, UR-QA-2, UR-ALG-6
#include <slideio/processor/algorithm/pixel/filter.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/pipeline/operation_catalog.hpp>
#include <slideio/processor/pipeline/pipeline_spec.hpp>

#include <algorithm/support/harness.hpp>
#include <pipeline/detail/parameters_json.hpp>
#include <pipeline/test_support/synthetic_scene.hpp>

#include <opencv2/imgproc.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using namespace slideio::processor;
namespace ts = slideio::processor::testing_support;

namespace {

using Maker = OperationPtr (*)(const StructuringElement&, int32_t);

struct Morph {
    const char* name;
    Maker make;
    int32_t haloFactor;
};

const std::vector<Morph>& morphs() {
    static const std::vector<Morph> all = {
        {"pixel.morph_erode", &makeErode, 1},
        {"pixel.morph_dilate", &makeDilate, 1},
        {"pixel.morph_open", &makeOpen, 2},
        {"pixel.morph_close", &makeClose, 2},
    };
    return all;
}

// The message of the InvalidArgumentError @p f throws; "" when none.
template <typename F>
std::string rejectionOf(F&& f) {
    try {
        f();
    } catch (const InvalidArgumentError& e) {
        return e.what();
    }
    return "";
}

std::string nameRejection(const char* name, const OperationParameters& p) {
    return rejectionOf([&] { OperationCatalog::instance().create(name, p); });
}

// Dilates a single 255 impulse at the centre of a (2w+1) x (2h+1) Byte image
// by @p element once. Dilation reads the sample at offset (i - w/2, j - h/2)
// for the element's set pixel (i, j), so the impulse spreads into the element
// mirrored about its centre.
ts::Image dilatedImpulse(const StructuringElement& element) {
    const int64_t width = 2 * element.width() + 1;
    const int64_t height = 2 * element.height() + 1;
    const int64_t cx = width / 2;
    const int64_t cy = height / 2;
    const OperationPtr op = makeDilate(element, 1);
    return ts::applyDirect(*op, {DataType::Byte}, {0}, width, height,
                           [&](int32_t, int64_t x, int64_t y) { return x == cx && y == cy ? 255.0 : 0.0; });
}

// Checks that the dilated impulse is exactly @p mask (w x h, row-major, non-zero set) mirrored.
void expectImpulseIsMirroredMask(const ts::Image& out, const std::vector<uint8_t>& mask, int32_t w, int32_t h) {
    const int64_t cx = out.width / 2;
    const int64_t cy = out.height / 2;
    for (int64_t y = 0; y < out.height; ++y) {
        for (int64_t x = 0; x < out.width; ++x) {
            // Output (x, y) reads the impulse through the element pixel (i, j)
            // with x + i - w/2 == cx, so i = cx - x + w/2.
            const int64_t i = cx - x + w / 2;
            const int64_t j = cy - y + h / 2;
            const bool set = i >= 0 && j >= 0 && i < w && j < h && mask[static_cast<size_t>(j * w + i)] != 0;
            ASSERT_EQ(out.at(0, x, y), set ? 255.0 : 0.0) << x << "," << y;
        }
    }
}

std::vector<uint8_t> cvMask(int shape, int32_t w, int32_t h) {
    const cv::Mat m = cv::getStructuringElement(shape, cv::Size(w, h));
    std::vector<uint8_t> mask(static_cast<size_t>(w) * static_cast<size_t>(h));
    for (int32_t y = 0; y < h; ++y) {
        for (int32_t x = 0; x < w; ++x) {
            mask[static_cast<size_t>(y * w + x)] = m.at<uint8_t>(y, x);
        }
    }
    return mask;
}

// A deterministic pseudo-random value in [0, 1) per pixel.
double hash01(int64_t x, int64_t y) {
    uint64_t h = static_cast<uint64_t>(x) * 0x9E3779B97F4A7C15ull ^ static_cast<uint64_t>(y) * 0xC2B2AE3D27D4EB4Full;
    h ^= h >> 29;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 32;
    return static_cast<double>(h >> 11) / 9007199254740992.0;
}

// A w x h Byte image; set(x, y) gives 255, else 0.
ts::Image applyBinary(Operation& op, int64_t w, int64_t h, const std::function<bool(int64_t, int64_t)>& set) {
    return ts::applyDirect(op, {DataType::Byte}, {0}, w, h,
                           [&](int32_t, int64_t x, int64_t y) { return set(x, y) ? 255.0 : 0.0; });
}

void expectBinary(const ts::Image& out, const std::function<bool(int64_t, int64_t)>& set) {
    for (int64_t y = 0; y < out.height; ++y) {
        for (int64_t x = 0; x < out.width; ++x) {
            ASSERT_EQ(out.at(0, x, y), set(x, y) ? 255.0 : 0.0) << x << "," << y;
        }
    }
}

bool inRect(int64_t x, int64_t y, int64_t x0, int64_t y0, int64_t x1, int64_t y1) {
    return x >= x0 && x <= x1 && y >= y0 && y <= y1;
}

} // namespace

TEST(StructuringElement, ShapesAndLimits) {
    // Each shape is the mask OpenCV draws, including a non-square box.
    const std::vector<std::pair<StructuringShape, int>> shapes = {
        {StructuringShape::Rectangle, cv::MORPH_RECT},
        {StructuringShape::Ellipse, cv::MORPH_ELLIPSE},
        {StructuringShape::Cross, cv::MORPH_CROSS},
    };
    for (const auto& [shape, cvShape] : shapes) {
        for (const auto& [w, h] : std::vector<std::pair<int32_t, int32_t>>{{5, 5}, {7, 5}, {3, 9}, {1, 1}}) {
            SCOPED_TRACE(std::to_string(static_cast<int>(shape)) + " " + std::to_string(w) + "x" +
                         std::to_string(h));
            const StructuringElement element(shape, w, h);
            EXPECT_EQ(element.width(), w);
            EXPECT_EQ(element.height(), h);
            expectImpulseIsMirroredMask(dilatedImpulse(element), cvMask(cvShape, w, h), w, h);
        }
    }
    // The 5 x 5 ellipse in particular is not the full box.
    const std::vector<uint8_t> ellipse = cvMask(cv::MORPH_ELLIPSE, 5, 5);
    EXPECT_EQ(ellipse[0], 0);
    EXPECT_NE(ellipse[2], 0);

    // An asymmetric custom mask spreads an impulse into its mirror image.
    const std::vector<uint8_t> custom = {1, 1, 0, //
                                         0, 1, 0, //
                                         0, 0, 0, //
                                         0, 0, 9, //
                                         0, 0, 0};
    const StructuringElement fromMask = StructuringElement::custom(custom.data(), 3, 5);
    EXPECT_EQ(fromMask.width(), 3);
    EXPECT_EQ(fromMask.height(), 5);
    expectImpulseIsMirroredMask(dilatedImpulse(fromMask), custom, 3, 5);

    // Sizes: odd, from 1 to 255.
    for (int32_t bad : {0, 2, 4, 256, 257, -1, -3}) {
        const std::string w = rejectionOf([&] { StructuringElement(StructuringShape::Rectangle, bad, 3); });
        EXPECT_NE(w.find("'width'"), std::string::npos) << bad << ": " << w;
        const std::string h = rejectionOf([&] { StructuringElement(StructuringShape::Ellipse, 3, bad); });
        EXPECT_NE(h.find("'height'"), std::string::npos) << bad << ": " << h;
        const uint8_t one[1] = {1};
        const std::string c = rejectionOf([&] { StructuringElement::custom(one, bad, 1); });
        EXPECT_NE(c.find("'width'"), std::string::npos) << bad << ": " << c;
        if (bad == 257 || bad == 256) {
            EXPECT_EQ(w.find(std::to_string(bad)), std::string::npos) << w;
        }
    }
    EXPECT_TRUE(rejectionOf([] { StructuringElement(StructuringShape::Cross, 255, 255); }).empty());
    EXPECT_TRUE(rejectionOf([] { StructuringElement(StructuringShape::Cross, 1, 255); }).empty());
    const std::string badShape =
        rejectionOf([] { StructuringElement(static_cast<StructuringShape>(7), 3, 3); });
    EXPECT_NE(badShape.find("'shape'"), std::string::npos) << badShape;

    // A custom mask needs a set pixel and a non-null pointer.
    const std::vector<uint8_t> zeros(9, 0);
    const std::string allZero = rejectionOf([&] { StructuringElement::custom(zeros.data(), 3, 3); });
    EXPECT_NE(allZero.find("'mask'"), std::string::npos) << allZero;
    const std::string null = rejectionOf([] { StructuringElement::custom(nullptr, 3, 3); });
    EXPECT_NE(null.find("'mask'"), std::string::npos) << null;

    // Copy and move.
    StructuringElement a(StructuringShape::Ellipse, 7, 5);
    StructuringElement b(a);
    EXPECT_EQ(b.width(), 7);
    EXPECT_EQ(b.height(), 5);
    StructuringElement c(StructuringShape::Rectangle, 3, 3);
    c = a;
    EXPECT_EQ(c.width(), 7);
    EXPECT_EQ(c.height(), 5);
    const StructuringElement& self = c;
    c = self;
    EXPECT_EQ(c.width(), 7);
    StructuringElement d(std::move(b));
    EXPECT_EQ(d.width(), 7);
    EXPECT_EQ(d.height(), 5);
    EXPECT_EQ(b.width(), 0); // NOLINT(bugprone-use-after-move): the documented moved-from state
    EXPECT_EQ(b.height(), 0);
    b = fromMask; // a moved-from element may be assigned to
    EXPECT_EQ(b.width(), 3);
    StructuringElement e(StructuringShape::Cross, 1, 1);
    e = std::move(d);
    EXPECT_EQ(e.width(), 7);
    EXPECT_EQ(e.height(), 5);
    // The copy and the moved element behave as the original.
    const ts::Image fromCopy = dilatedImpulse(c);
    const ts::Image fromMoved = dilatedImpulse(e);
    const ts::Image fromOriginal = dilatedImpulse(a);
    EXPECT_TRUE(fromCopy.planes == fromOriginal.planes);
    EXPECT_TRUE(fromMoved.planes == fromOriginal.planes);
    const ts::Image fromAssigned = dilatedImpulse(b);
    expectImpulseIsMirroredMask(fromAssigned, custom, 3, 5);

    // A factory refuses a moved-from element.
    StructuringElement f(StructuringShape::Rectangle, 3, 3);
    StructuringElement g(std::move(f));
    const std::string movedFrom = rejectionOf([&] { makeErode(f, 1); }); // NOLINT(bugprone-use-after-move)
    EXPECT_NE(movedFrom.find("pixel.morph_erode"), std::string::npos) << movedFrom;
}

TEST(Morphology, FactoriesEqualNamePaths) {
    const StructuringElement ellipse(StructuringShape::Ellipse, 7, 5);
    const std::vector<uint8_t> customMask = {0, 1, 0, //
                                             1, 1, 7, //
                                             0, 1, 0};
    const StructuringElement custom = StructuringElement::custom(customMask.data(), 3, 3);
    const double customDoubles[] = {0, 1, 0, 1, 1, 1, 0, 1, 0};
    DataTypeSet expectedTypes;
    for (DataType t : {DataType::Byte, DataType::UInt16, DataType::Int16, DataType::Float32, DataType::Float64}) {
        expectedTypes.add(t);
    }

    for (const Morph& m : morphs()) {
        SCOPED_TRACE(m.name);
        OperationParameters p;
        p.setString("shape", "Ellipse").setInt("elementWidth", 7).setInt("elementHeight", 5).setInt("iterations", 2);
        const OperationPtr byName = OperationCatalog::instance().create(m.name, p);
        const OperationPtr byFactory = m.make(ellipse, 2);
        EXPECT_TRUE(detail::parametersEqual(byFactory->parameters(), byName->parameters()));
        EXPECT_EQ(byFactory->parameters().count(), 4);
        const char* shape = byFactory->parameters().getString("shape", "");
        EXPECT_STREQ(shape, "Ellipse");

        // Custom: the mask is stored as 0/1 doubles, row by row.
        OperationParameters pc;
        pc.setString("shape", "Custom")
            .setInt("elementWidth", 3)
            .setInt("elementHeight", 3)
            .setDoubleArray("elementMask", customDoubles, 9)
            .setInt("iterations", 1);
        const OperationPtr customByName = OperationCatalog::instance().create(m.name, pc);
        const OperationPtr customByFactory = m.make(custom, 1);
        EXPECT_TRUE(detail::parametersEqual(customByFactory->parameters(), customByName->parameters()));
        EXPECT_EQ(customByFactory->parameters().count(), 5);
        double stored[9] = {};
        EXPECT_EQ(customByFactory->parameters().getDoubleArray("elementMask", stored, 9), 9);
        for (int i = 0; i < 9; ++i) {
            EXPECT_EQ(stored[i], customDoubles[i]) << i;
        }

        // Defaults: a 3 x 3 rectangle, once.
        const OperationPtr byDefault = OperationCatalog::instance().create(m.name, OperationParameters());
        EXPECT_TRUE(detail::parametersEqual(
            byDefault->parameters(), m.make(StructuringElement(StructuringShape::Rectangle, 3, 3), 1)->parameters()));
        EXPECT_EQ(byDefault->parameters().count(), 4);

        EXPECT_STREQ(byFactory->name(), m.name);
        EXPECT_EQ(byFactory->arity(), ChannelArity::PerChannel);
        EXPECT_FALSE(byFactory->requiresUniformTypes());
        EXPECT_FALSE(byFactory->producesObjects());
        EXPECT_TRUE(byFactory->supportedTypes() == expectedTypes);
        for (DataType t : {DataType::Byte, DataType::UInt16, DataType::Int16, DataType::Float32, DataType::Float64}) {
            EXPECT_EQ(byFactory->outputType(t), t);
        }
        EXPECT_STREQ(OperationCatalog::instance().correctnessReference(m.name),
                     "Analytic: erosion and dilation of synthetic binary shapes by known elements");
        EXPECT_STREQ(OperationCatalog::instance().complexityNote(m.name),
                     "O(n * elementWidth * elementHeight * iterations) in the number of samples");

        // Iterations: 1 to 64.
        for (int32_t bad : {0, 65, -1}) {
            const std::string message = rejectionOf([&] { m.make(ellipse, bad); });
            EXPECT_NE(message.find("'iterations'"), std::string::npos) << bad << ": " << message;
            EXPECT_NE(message.find(m.name), std::string::npos) << message;
            const std::string byNameMessage =
                nameRejection(m.name, OperationParameters().setInt("iterations", bad));
            EXPECT_NE(byNameMessage.find("'iterations'"), std::string::npos) << byNameMessage;
        }
        EXPECT_TRUE(rejectionOf([&] { m.make(ellipse, 64); }).empty());

        // By name: each key's rules, the message naming the key.
        const auto expectKey = [&](const OperationParameters& bad, const char* key) {
            const std::string message = nameRejection(m.name, bad);
            EXPECT_NE(message.find(std::string("'") + key + "'"), std::string::npos) << key << ": " << message;
        };
        expectKey(OperationParameters().setString("shape", "Disc"), "shape");
        expectKey(OperationParameters().setInt("elementWidth", 4), "elementWidth");
        expectKey(OperationParameters().setInt("elementWidth", 257), "elementWidth");
        expectKey(OperationParameters().setInt("elementHeight", 0), "elementHeight");
        expectKey(OperationParameters().setDoubleArray("elementMask", customDoubles, 9), "elementMask");
        expectKey(OperationParameters().setString("shape", "Ellipse").setDoubleArray("elementMask", customDoubles, 9),
                  "elementMask");
        expectKey(OperationParameters().setString("shape", "Custom"), "elementMask");
        expectKey(OperationParameters().setString("shape", "Custom").setDoubleArray("elementMask", customDoubles, 8),
                  "elementMask");
        const double two[] = {0, 2, 0, 1, 1, 1, 0, 1, 0};
        expectKey(OperationParameters().setString("shape", "Custom").setDoubleArray("elementMask", two, 9),
                  "elementMask");
        const double zeros[9] = {};
        expectKey(OperationParameters().setString("shape", "Custom").setDoubleArray("elementMask", zeros, 9),
                  "elementMask");
        expectKey(OperationParameters().setString("shape", "Custom").setInt("elementMask", 1), "elementMask");
        expectKey(OperationParameters().setString("iterations", "2"), "iterations");
        const std::string unknown = nameRejection(m.name, OperationParameters().setInt("iteration", 2));
        EXPECT_NE(unknown.find("iteration"), std::string::npos) << unknown;
    }
}

TEST(Morphology, HaloDoublesForOpenClose) {
    const StructuringElement element(StructuringShape::Rectangle, 5, 3);
    for (const Morph& m : morphs()) {
        SCOPED_TRACE(m.name);
        const Halo halo = m.make(element, 2)->halo();
        EXPECT_EQ(halo.left, 4 * m.haloFactor);
        EXPECT_EQ(halo.right, 4 * m.haloFactor);
        EXPECT_EQ(halo.top, 2 * m.haloFactor);
        EXPECT_EQ(halo.bottom, 2 * m.haloFactor);
    }
    const Halo widest = makeClose(StructuringElement(StructuringShape::Cross, 255, 1), 64)->halo();
    EXPECT_EQ(widest.left, 2 * 64 * 127);
    EXPECT_EQ(widest.top, 0);
}

TEST(Morphology, AnalyticShapes) {
    constexpr int64_t kSize = 64;
    const StructuringElement box3(StructuringShape::Rectangle, 3, 3);
    const StructuringElement box5(StructuringShape::Rectangle, 5, 5);
    const auto square = [](int64_t x, int64_t y) { return inRect(x, y, 10, 10, 30, 30); }; // 21 x 21

    const OperationPtr erode = makeErode(box3, 1);
    expectBinary(applyBinary(*erode, kSize, kSize, square),
                 [](int64_t x, int64_t y) { return inRect(x, y, 11, 11, 29, 29); }); // 19 x 19
    const OperationPtr dilate = makeDilate(box3, 1);
    expectBinary(applyBinary(*dilate, kSize, kSize, square),
                 [](int64_t x, int64_t y) { return inRect(x, y, 9, 9, 31, 31); }); // 23 x 23
    // Two iterations of 3 x 3 erode as one 5 x 5 does.
    expectBinary(applyBinary(*makeErode(box3, 2), kSize, kSize, square),
                 [](int64_t x, int64_t y) { return inRect(x, y, 12, 12, 28, 28); });

    // Opening removes a 3 x 3 speck and keeps the square.
    const OperationPtr open = makeOpen(box5, 1);
    expectBinary(applyBinary(*open, kSize, kSize,
                             [&](int64_t x, int64_t y) { return square(x, y) || inRect(x, y, 45, 45, 47, 47); }),
                 square);

    // Closing fills a 2-pixel gap between two blocks, and nothing else.
    const OperationPtr close = makeClose(box5, 1);
    expectBinary(applyBinary(*close, kSize, kSize,
                             [](int64_t x, int64_t y) {
                                 return inRect(x, y, 10, 10, 20, 30) || inRect(x, y, 23, 10, 33, 30);
                             }),
                 [](int64_t x, int64_t y) { return inRect(x, y, 10, 10, 33, 30); });

    // The other types give the same shapes.
    for (DataType type : {DataType::UInt16, DataType::Int16, DataType::Float32, DataType::Float64}) {
        SCOPED_TRACE(dataTypeName(type));
        const ts::Image out = ts::applyDirect(*erode, {type}, {0}, kSize, kSize, [&](int32_t, int64_t x, int64_t y) {
            return square(x, y) ? 255.0 : 0.0;
        });
        ASSERT_EQ(out.types, std::vector<DataType>{type});
        expectBinary(out, [](int64_t x, int64_t y) { return inRect(x, y, 11, 11, 29, 29); });
    }
}

TEST(Morphology, EdgeErodesInward) {
    constexpr int64_t kWidth = 300;
    constexpr int64_t kHeight = 260;
    const auto onRing = [](int64_t x, int64_t y) {
        return x == 0 || y == 0 || x == kWidth - 1 || y == kHeight - 1;
    };
    const OperationPtr erode = makeErode(StructuringElement(StructuringShape::Rectangle, 3, 3), 1);
    expectBinary(applyBinary(*erode, kWidth, kHeight, [](int64_t, int64_t) { return true; }),
                 [&](int64_t x, int64_t y) { return !onRing(x, y); });

    auto scene = ts::makeSyntheticScene({{kWidth, kHeight, 0, 0}}, {slideio::DataType::DT_Byte},
                                        [](int, int64_t, int64_t) { return 255.0; });
    PipelineSpec spec;
    spec.then(erode);
    const ts::Image reference = ts::runEager(scene, spec, 4096, 1);
    ASSERT_EQ(reference.width, kWidth);
    ASSERT_EQ(reference.height, kHeight);
    expectBinary(reference, [&](int64_t x, int64_t y) { return !onRing(x, y); });
    for (int32_t tile : {64, 100, 257}) {
        const ts::Image tiled = ts::runEager(scene, spec, tile, 4);
        EXPECT_TRUE(tiled.planes == reference.planes) << "tile " << tile;
    }
}

TEST(Morphology, TiledEqualsSingleTile) {
    const StructuringElement element(StructuringShape::Ellipse, 7, 5);
    const auto pattern = [](int, int64_t x, int64_t y) { return std::floor(hash01(x, y) * 60000.0) - 1000.0; };
    for (slideio::DataType type : {slideio::DataType::DT_Float32, slideio::DataType::DT_UInt16}) {
        auto scene = ts::makeSyntheticScene({{300, 260, 0, 0}}, {type}, pattern);
        for (const Morph& m : morphs()) {
            SCOPED_TRACE(std::string(m.name) + " " + std::to_string(static_cast<int>(type)));
            PipelineSpec spec;
            spec.then(m.make(element, 2));
            const ts::Image reference = ts::runEager(scene, spec, 4096, 1);
            ASSERT_EQ(reference.width, 300);
            ASSERT_EQ(reference.height, 260);
            ASSERT_EQ(reference.types.size(), 1u);
            for (int32_t tile : {64, 100, 257}) {
                const ts::Image tiled = ts::runEager(scene, spec, tile, 4);
                EXPECT_TRUE(tiled.planes == reference.planes) << "tile " << tile;
            }
        }
    }
}

TEST(Morphology, NaNTiledEqualsSingleTile) {
    // Scattered NaNs among fractional values, signed zeros included: the
    // result, compared bit for bit, must not depend on where a tile starts.
    const StructuringElement element(StructuringShape::Ellipse, 7, 5);
    const auto pattern = [](int, int64_t x, int64_t y) {
        const double u = hash01(x, y);
        if (u < 0.03) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        if (u < 0.05) {
            return -0.0;
        }
        return (u - 0.5) * 1000.0;
    };
    auto scene = ts::makeSyntheticScene({{300, 260, 0, 0}}, {slideio::DataType::DT_Float32}, pattern);
    for (const Morph& m : morphs()) {
        for (int32_t iterations : {1, 2}) {
            SCOPED_TRACE(std::string(m.name) + " x" + std::to_string(iterations));
            PipelineSpec spec;
            spec.then(m.make(element, iterations));
            const ts::Image reference = ts::runEager(scene, spec, 4096, 1);
            ASSERT_EQ(reference.types.size(), 1u);
            int64_t nans = 0;
            for (int64_t y = 0; y < reference.height; ++y) {
                for (int64_t x = 0; x < reference.width; ++x) {
                    nans += std::isnan(reference.at(0, x, y)) ? 1 : 0;
                }
            }
            EXPECT_GT(nans, 0); // the NaNs reach the output
            for (int32_t tile : {64, 100, 257}) {
                const ts::Image tiled = ts::runEager(scene, spec, tile, 4);
                EXPECT_TRUE(tiled.planes == reference.planes) << "tile " << tile;
            }
        }
    }

    // The same content shifted by s columns gives the same samples, bit for
    // bit, wherever no window reaches the image's edge: a NaN's effect does
    // not depend on the pixel's column within the input.
    constexpr int64_t kWidth = 97;
    constexpr int64_t kHeight = 23;
    for (DataType type : {DataType::Float32, DataType::Float64}) {
        for (const StructuringElement& e :
             {element, StructuringElement(StructuringShape::Rectangle, 3, 3),
              StructuringElement(StructuringShape::Rectangle, 9, 1)}) {
            for (const Morph& m : morphs()) {
                SCOPED_TRACE(std::string(m.name) + " " + dataTypeName(type) + " " + std::to_string(e.width()) + "x" +
                             std::to_string(e.height()));
                const OperationPtr op = m.make(e, 1);
                const int64_t reachX = op->halo().left;
                const int64_t reachY = op->halo().top;
                const ts::Image base = ts::applyDirect(
                    *op, {type}, {0}, kWidth, kHeight, [&](int32_t p, int64_t x, int64_t y) { return pattern(p, x, y); });
                for (int64_t s = 1; s <= 9; ++s) {
                    const ts::Image shifted =
                        ts::applyDirect(*op, {type}, {0}, kWidth, kHeight,
                                        [&](int32_t p, int64_t x, int64_t y) { return pattern(p, x + s, y); });
                    const size_t size = static_cast<size_t>(dataTypeSizeBytes(type));
                    for (int64_t y = reachY; y < kHeight - reachY; ++y) {
                        for (int64_t x = reachX; x + s + reachX < kWidth; ++x) {
                            const size_t a = static_cast<size_t>(y * kWidth + x) * size;
                            const size_t b = static_cast<size_t>(y * kWidth + x + s) * size;
                            ASSERT_EQ(std::memcmp(shifted.planes[0].data() + a, base.planes[0].data() + b, size), 0)
                                << "shift " << s << " at " << x << "," << y;
                        }
                    }
                }
            }
        }
    }
}

TEST(Morphology, NaNInWindowGivesNaN) {
    // A single NaN turns every sample whose window covers it into NaN, in
    // erosion and dilation alike; samples whose window misses it stay numbers.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const StructuringElement box3(StructuringShape::Rectangle, 3, 3);
    for (Maker make : {&makeErode, &makeDilate}) {
        const OperationPtr op = make(box3, 1);
        const ts::Image out = ts::applyDirect(*op, {DataType::Float32}, {0}, 9, 7, [&](int32_t, int64_t x, int64_t y) {
            return x == 4 && y == 3 ? static_cast<double>(nan) : 5.0 + static_cast<double>(x);
        });
        for (int64_t y = 0; y < 7; ++y) {
            for (int64_t x = 0; x < 9; ++x) {
                const bool covered = x >= 3 && x <= 5 && y >= 2 && y <= 4;
                EXPECT_EQ(std::isnan(out.at(0, x, y)), covered) << op->name() << " " << x << "," << y;
            }
        }
    }
}

TEST(Morphology, AgreesWithOpenCVWithoutNaN) {
    // On NaN-free input every type equals OpenCV's own operation applied to
    // the image surrounded by zeros, the border OpenCV assumes lying beyond.
    constexpr int64_t kWidth = 71;
    constexpr int64_t kHeight = 53;
    const StructuringElement element(StructuringShape::Ellipse, 5, 7);
    const std::vector<std::pair<DataType, int>> types = {{DataType::Byte, CV_8U},
                                                         {DataType::UInt16, CV_16U},
                                                         {DataType::Int16, CV_16S},
                                                         {DataType::Float32, CV_32F},
                                                         {DataType::Float64, CV_64F}};
    const auto pattern = [](int32_t, int64_t x, int64_t y) { return std::floor(hash01(x, y) * 250.0) + 1.0; };
    for (const auto& [type, depth] : types) {
        cv::Mat image(static_cast<int>(kHeight), static_cast<int>(kWidth), CV_64F);
        for (int64_t y = 0; y < kHeight; ++y) {
            for (int64_t x = 0; x < kWidth; ++x) {
                image.at<double>(static_cast<int>(y), static_cast<int>(x)) = pattern(0, x, y);
            }
        }
        image.convertTo(image, depth);
        const cv::Mat kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(5, 7));
        for (const Morph& m : morphs()) {
            SCOPED_TRACE(std::string(m.name) + " " + dataTypeName(type));
            const OperationPtr op = m.make(element, 2);
            const Halo halo = op->halo();
            cv::Mat padded;
            cv::copyMakeBorder(image, padded, halo.top, halo.bottom, halo.left, halo.right, cv::BORDER_CONSTANT,
                               cv::Scalar(0));
            cv::Mat expected;
            const int cvOp = std::string(m.name) == "pixel.morph_erode"    ? cv::MORPH_ERODE
                             : std::string(m.name) == "pixel.morph_dilate" ? cv::MORPH_DILATE
                             : std::string(m.name) == "pixel.morph_open"   ? cv::MORPH_OPEN
                                                                           : cv::MORPH_CLOSE;
            cv::morphologyEx(padded, expected, cvOp, kernel, cv::Point(-1, -1), 2);
            cv::Mat cropped;
            expected(cv::Rect(halo.left, halo.top, static_cast<int>(kWidth), static_cast<int>(kHeight)))
                .convertTo(cropped, CV_64F);
            const ts::Image out = ts::applyDirect(*op, {type}, {0}, kWidth, kHeight, pattern);
            ASSERT_EQ(out.types, std::vector<DataType>{type});
            for (int64_t y = 0; y < kHeight; ++y) {
                for (int64_t x = 0; x < kWidth; ++x) {
                    ASSERT_EQ(out.at(0, x, y), cropped.at<double>(static_cast<int>(y), static_cast<int>(x)))
                        << x << "," << y;
                }
            }
        }
    }
}
