// Requirements: UR-PIX-3, UR-QA-2, UR-IMG-11, UR-ALG-6
#include <slideio/processor/algorithm/pixel/filter.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/pipeline/operation_catalog.hpp>
#include <slideio/processor/pipeline/pipeline_spec.hpp>

#include <algorithm/support/harness.hpp>
#include <pipeline/detail/parameters_json.hpp>
#include <pipeline/test_support/synthetic_scene.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <vector>

using namespace slideio::processor;
namespace ts = slideio::processor::testing_support;

namespace {

constexpr const char* kName = "pixel.median_blur";

// The message of the InvalidArgumentError creating the operation from @p p throws; "" when none.
std::string rejection(const OperationParameters& p) {
    try {
        OperationCatalog::instance().create(kName, p);
    } catch (const InvalidArgumentError& e) {
        return e.what();
    }
    return "";
}

// The message of the InvalidArgumentError the factory throws; "" when none.
std::string factoryRejection(int32_t kernelSize) {
    try {
        makeMedianBlur(kernelSize);
    } catch (const InvalidArgumentError& e) {
        return e.what();
    }
    return "";
}

template <typename T>
std::vector<uint8_t> rawOf(const std::vector<T>& samples) {
    std::vector<uint8_t> raw(samples.size() * sizeof(T));
    std::memcpy(raw.data(), samples.data(), raw.size());
    return raw;
}

template <typename T>
std::vector<T> samplesOf(const std::vector<uint8_t>& raw) {
    std::vector<T> samples(raw.size() / sizeof(T));
    std::memcpy(samples.data(), raw.data(), raw.size());
    return samples;
}

// The definition, directly: the middle value of the sorted k x k window of
// @p v (width x height) centred on (x, y), with zero beyond the image.
template <typename T>
T referenceMedian(const std::vector<T>& v, int64_t width, int64_t height, int64_t x, int64_t y, int32_t k) {
    const int64_t r = k / 2;
    std::vector<T> window;
    for (int64_t dy = -r; dy <= r; ++dy) {
        for (int64_t dx = -r; dx <= r; ++dx) {
            const int64_t px = x + dx;
            const int64_t py = y + dy;
            window.push_back(px < 0 || py < 0 || px >= width || py >= height
                                 ? T(0)
                                 : v[static_cast<size_t>(py * width + px)]);
        }
    }
    std::sort(window.begin(), window.end());
    return window[window.size() / 2];
}

// Applies the median of size @p k to @p values and checks every output sample
// against referenceMedian.
template <typename T>
void checkDefinition(DataType type, const std::vector<T>& values, int64_t width, int64_t height, int32_t k) {
    SCOPED_TRACE(std::string(dataTypeName(type)) + " size " + std::to_string(k));
    const OperationPtr op = makeMedianBlur(k);
    const ts::Image out = ts::applyDirectRaw(*op, {type}, {0}, width, height, {rawOf(values)});
    ASSERT_EQ(out.types, std::vector<DataType>{type});
    const std::vector<T> result = samplesOf<T>(out.planes[0]);
    ASSERT_EQ(result.size(), values.size());
    for (int64_t y = 0; y < height; ++y) {
        for (int64_t x = 0; x < width; ++x) {
            ASSERT_EQ(result[static_cast<size_t>(y * width + x)], referenceMedian(values, width, height, x, y, k))
                << x << "," << y;
        }
    }
}

} // namespace

TEST(MedianBlur, FactoryEqualsNamePath) {
    OperationParameters p;
    p.setInt("kernelSize", 5);
    const OperationPtr byName = OperationCatalog::instance().create(kName, p);
    const OperationPtr byFactory = makeMedianBlur(5);
    EXPECT_TRUE(detail::parametersEqual(byFactory->parameters(), byName->parameters()));
    EXPECT_EQ(byFactory->parameters().count(), 1);

    // The default fills the bag: kernelSize 3.
    const OperationPtr byDefault = OperationCatalog::instance().create(kName, OperationParameters());
    EXPECT_EQ(byDefault->parameters().count(), 1);
    EXPECT_TRUE(detail::parametersEqual(byDefault->parameters(), makeMedianBlur(3)->parameters()));
    EXPECT_EQ(byDefault->halo().left, 1);

    EXPECT_STREQ(byFactory->name(), kName);
    EXPECT_EQ(byFactory->arity(), ChannelArity::PerChannel);
    EXPECT_FALSE(byFactory->requiresUniformTypes());
    EXPECT_FALSE(byFactory->producesObjects());
    for (const OperationPtr& op : {byFactory, byName}) {
        const Halo halo = op->halo();
        EXPECT_EQ(halo.left, 2);
        EXPECT_EQ(halo.top, 2);
        EXPECT_EQ(halo.right, 2);
        EXPECT_EQ(halo.bottom, 2);
    }
    EXPECT_EQ(makeMedianBlur(255)->halo().left, 127);
    EXPECT_STREQ(OperationCatalog::instance().correctnessReference(kName),
                 "Published definition: the middle value of the sorted window; checked against a direct "
                 "sort-based reference");
    EXPECT_STREQ(OperationCatalog::instance().complexityNote(kName),
                 "O(n * kernelSize^2) in the number of samples (O(n * kernelSize) for Byte)");
}

TEST(MedianBlur, RejectsBadKernel) {
    for (int32_t kernelSize : {2, 1, 257}) {
        const std::string message = factoryRejection(kernelSize);
        EXPECT_NE(message.find("'kernelSize'"), std::string::npos) << kernelSize << ": " << message;
        EXPECT_NE(message.find(kName), std::string::npos) << message;
        const std::string byName = rejection(OperationParameters().setInt("kernelSize", kernelSize));
        EXPECT_NE(byName.find("'kernelSize'"), std::string::npos) << byName;
        if (kernelSize != 2) { // "2" is a digit of the stated bound
            EXPECT_EQ(message.find(std::to_string(kernelSize)), std::string::npos) << message;
            EXPECT_EQ(byName.find(std::to_string(kernelSize)), std::string::npos) << byName;
        }
    }
    EXPECT_TRUE(factoryRejection(3).empty());
    EXPECT_TRUE(factoryRejection(255).empty());
    EXPECT_FALSE(factoryRejection(-3).empty());

    // By name: unknown key and wrong type.
    const std::string unknown = rejection(OperationParameters().setInt("kernelsize", 3));
    EXPECT_NE(unknown.find("kernelsize"), std::string::npos) << unknown;
    EXPECT_FALSE(rejection(OperationParameters().setString("kernelSize", "3")).empty());
}

TEST(MedianBlur, MatchesDefinition) {
    constexpr int64_t kWidth = 61;
    constexpr int64_t kHeight = 47;
    constexpr size_t kCount = static_cast<size_t>(kWidth * kHeight);
    std::mt19937_64 rng(20261118u);

    std::vector<uint8_t> bytes(kCount);
    std::uniform_int_distribution<int> byteDist(0, 255);
    for (uint8_t& v : bytes) {
        v = static_cast<uint8_t>(byteDist(rng));
    }
    for (int32_t k : {3, 5, 7}) {
        checkDefinition(DataType::Byte, bytes, kWidth, kHeight, k);
    }

    std::vector<uint16_t> words(kCount);
    std::uniform_int_distribution<int> wordDist(0, 65535);
    for (uint16_t& v : words) {
        v = static_cast<uint16_t>(wordDist(rng));
    }
    std::vector<float> floats(kCount);
    std::uniform_real_distribution<float> floatDist(-1000.0f, 1000.0f);
    for (float& v : floats) {
        v = floatDist(rng);
    }
    // The full Int64 range, beyond what a double holds exactly.
    std::vector<int64_t> longs(kCount);
    std::uniform_int_distribution<int64_t> longDist(std::numeric_limits<int64_t>::min(),
                                                    std::numeric_limits<int64_t>::max());
    for (int64_t& v : longs) {
        v = longDist(rng);
    }
    for (int32_t k : {3, 7}) {
        checkDefinition(DataType::UInt16, words, kWidth, kHeight, k);
        checkDefinition(DataType::Float32, floats, kWidth, kHeight, k);
        checkDefinition(DataType::Int64, longs, kWidth, kHeight, k);
    }
}

TEST(MedianBlur, ByteAndUInt16Agree) {
    const auto pattern = [](int32_t, int64_t x, int64_t y) {
        return static_cast<double>((x * 37 + y * 101 + (x * y) % 29) % 256);
    };
    const OperationPtr op = makeMedianBlur(9);
    const ts::Image byOpenCV = ts::applyDirect(*op, {DataType::Byte}, {0}, 53, 41, pattern);
    const ts::Image byOwn = ts::applyDirect(*op, {DataType::UInt16}, {0}, 53, 41, pattern);
    ASSERT_EQ(byOpenCV.types, std::vector<DataType>{DataType::Byte});
    ASSERT_EQ(byOwn.types, std::vector<DataType>{DataType::UInt16});
    for (int64_t y = 0; y < 41; ++y) {
        for (int64_t x = 0; x < 53; ++x) {
            ASSERT_EQ(byOpenCV.at(0, x, y), byOwn.at(0, x, y)) << x << "," << y;
        }
    }
}

TEST(MedianBlur, OutputTypeIsInputType) {
    const OperationPtr op = makeMedianBlur(3);
    for (int32_t i = 0; i < 11; ++i) {
        const auto type = static_cast<DataType>(i);
        SCOPED_TRACE(dataTypeName(type));
        EXPECT_TRUE(op->supportedTypes().contains(type));
        EXPECT_EQ(op->outputType(type), type);
        // A constant image keeps its exact samples away from the zero border.
        const size_t size = static_cast<size_t>(dataTypeSizeBytes(type));
        std::vector<uint8_t> raw(static_cast<size_t>(6 * 5) * size);
        for (size_t s = 0; s < raw.size(); s += size) {
            for (size_t b = 0; b < size; ++b) {
                raw[s + b] = static_cast<uint8_t>(0x31 + b); // a finite, non-zero sample in every type
            }
        }
        const ts::Image out = ts::applyDirectRaw(*op, {type}, {0}, 6, 5, {raw});
        ASSERT_EQ(out.types, std::vector<DataType>{type});
        for (int64_t y = 1; y < 4; ++y) {
            for (int64_t x = 1; x < 5; ++x) {
                const size_t at = static_cast<size_t>(y * 6 + x) * size;
                EXPECT_EQ(std::memcmp(out.planes[0].data() + at, raw.data() + at, size), 0) << x << "," << y;
            }
        }
    }
}

TEST(MedianBlur, NaNSortsAboveEveryNumber) {
    // Of the nine samples of the centre window, five are NaN: the middle of
    // the sorted window is then NaN. With four NaN it is the largest number.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const OperationPtr op = makeMedianBlur(3);
    const std::vector<float> fiveNaN = {1, nan, 2, nan, nan, nan, 3, nan, 4};
    const ts::Image a = ts::applyDirectRaw(*op, {DataType::Float32}, {0}, 3, 3, {rawOf(fiveNaN)});
    EXPECT_TRUE(std::isnan(a.at(0, 1, 1)));
    const std::vector<float> fourNaN = {1, nan, 2, nan, 5, nan, 3, nan, 4};
    const ts::Image b = ts::applyDirectRaw(*op, {DataType::Float32}, {0}, 3, 3, {rawOf(fourNaN)});
    EXPECT_EQ(b.at(0, 1, 1), 5.0);
}

TEST(MedianBlur, TiledEqualsSingleTile) {
    const auto pattern = [](int, int64_t x, int64_t y) {
        return static_cast<double>((x * 7 + y * 13 + (x * y) % 11) % 256) * 0.75;
    };
    for (slideio::DataType type : {slideio::DataType::DT_Byte, slideio::DataType::DT_Float32}) {
        SCOPED_TRACE(static_cast<int>(type));
        auto scene = ts::makeSyntheticScene({{300, 260, 0, 0}}, {type}, pattern);
        PipelineSpec spec;
        spec.then(makeMedianBlur(7));
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
