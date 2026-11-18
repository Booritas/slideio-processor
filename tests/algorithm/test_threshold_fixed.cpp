// Requirements: UR-PIX-5, UR-IMG-11, UR-ALG-6, UR-QA-2
#include <slideio/processor/algorithm/pixel/threshold.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/pipeline/operation_catalog.hpp>
#include <slideio/processor/pipeline/pipeline_spec.hpp>

#include <algorithm/support/harness.hpp>
#include <pipeline/detail/parameters_json.hpp>
#include <pipeline/test_support/synthetic_scene.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace slideio::processor;
namespace ts = slideio::processor::testing_support;

namespace {

// The message of the InvalidArgumentError creating pixel.threshold_fixed from @p p throws; "" when none.
std::string rejection(const OperationParameters& p) {
    try {
        OperationCatalog::instance().create("pixel.threshold_fixed", p);
    } catch (const InvalidArgumentError& e) {
        return e.what();
    }
    return "";
}

template <typename T>
std::vector<uint8_t> rawOf(const std::vector<T>& samples) {
    std::vector<uint8_t> bytes(samples.size() * sizeof(T));
    std::memcpy(bytes.data(), samples.data(), bytes.size());
    return bytes;
}

std::vector<double> row(const ts::Image& image) {
    std::vector<double> out;
    for (int64_t x = 0; x < image.width; ++x) {
        out.push_back(image.at(0, x, 0));
    }
    return out;
}

// Samples 99, 100, 101 (and NaN for a float type) against 100 in both directions.
void expectStrict(DataType type) {
    SCOPED_TRACE(dataTypeName(type));
    const bool isFloat = dataTypeIsFloat(type);
    const int64_t width = isFloat ? 4 : 3;
    for (ThresholdDirection direction : {ThresholdDirection::Above, ThresholdDirection::Below}) {
        const OperationPtr op = makeFixedThreshold(100.0, direction);
        ts::Image out;
        if (type == DataType::Float16) {
            // 99, 100, 101 and a quiet NaN as binary16.
            out = ts::applyDirectRaw(*op, {type}, {0}, width, 1,
                                     {rawOf(std::vector<uint16_t>{0x5630, 0x5640, 0x5650, 0x7E00})});
        } else if (type == DataType::Int64) {
            out = ts::applyDirectRaw(*op, {type}, {0}, width, 1, {rawOf(std::vector<int64_t>{99, 100, 101})});
        } else if (type == DataType::UInt64) {
            out = ts::applyDirectRaw(*op, {type}, {0}, width, 1, {rawOf(std::vector<uint64_t>{99, 100, 101})});
        } else {
            out = ts::applyDirect(*op, {type}, {0}, width, 1, [](int32_t, int64_t x, int64_t) {
                return x == 3 ? std::numeric_limits<double>::quiet_NaN() : 99.0 + static_cast<double>(x);
            });
        }
        ASSERT_EQ(out.types, std::vector<DataType>{DataType::Byte});
        std::vector<double> expected = direction == ThresholdDirection::Above ? std::vector<double>{0, 0, 255}
                                                                              : std::vector<double>{255, 0, 0};
        if (isFloat) {
            expected.push_back(0);
        }
        EXPECT_EQ(row(out), expected) << (direction == ThresholdDirection::Above ? "Above" : "Below");
    }
}

} // namespace

TEST(ThresholdFixed, FactoryEqualsNamePath) {
    OperationParameters p;
    p.setDouble("value", 100.0).setString("direction", "Below");
    EXPECT_TRUE(detail::parametersEqual(makeFixedThreshold(100.0, ThresholdDirection::Below)->parameters(),
                                        OperationCatalog::instance().create("pixel.threshold_fixed", p)->parameters()));
}

TEST(ThresholdFixed, DefaultsFillTheBag) {
    const OperationPtr op = OperationCatalog::instance().create("pixel.threshold_fixed", OperationParameters());
    const OperationParameters& bag = op->parameters();
    EXPECT_EQ(bag.count(), 2);
    double value = 0.0;
    ASSERT_TRUE(bag.getDouble("value", value));
    EXPECT_EQ(value, 128.0);
    EXPECT_STREQ(bag.getString("direction", nullptr), "Above");

    // A partial bag is completed the same way.
    const OperationPtr partial =
        OperationCatalog::instance().create("pixel.threshold_fixed", OperationParameters().setString("direction", "Below"));
    EXPECT_TRUE(detail::parametersEqual(partial->parameters(),
                                        makeFixedThreshold(128.0, ThresholdDirection::Below)->parameters()));
}

TEST(ThresholdFixed, DeclaresItsContract) {
    const OperationPtr op = makeFixedThreshold(1.0, ThresholdDirection::Above);
    EXPECT_STREQ(op->name(), "pixel.threshold_fixed");
    EXPECT_EQ(op->arity(), ChannelArity::PerChannel);
    EXPECT_FALSE(op->requiresUniformTypes());
    EXPECT_TRUE(op->supportedTypes() == DataTypeSet::all());
    EXPECT_TRUE(op->halo().isEmpty());
    EXPECT_FALSE(op->producesObjects());
    for (int32_t i = 0; i < 11; ++i) {
        EXPECT_EQ(op->outputType(static_cast<DataType>(i)), DataType::Byte);
    }
    EXPECT_STREQ(OperationCatalog::instance().correctnessReference("pixel.threshold_fixed"),
                 "Analytic: a sample is foreground exactly when it is strictly above (or below) the value; NaN is "
                 "background");
    EXPECT_STREQ(OperationCatalog::instance().complexityNote("pixel.threshold_fixed"), "O(n) in the number of samples");
}

TEST(ThresholdFixed, RejectsUnknownKeyAndBadValues) {
    const std::string unknown = rejection(OperationParameters().setDouble("valeu", 100.0));
    EXPECT_NE(unknown.find("valeu"), std::string::npos) << unknown;
    EXPECT_NE(unknown.find("pixel.threshold_fixed"), std::string::npos) << unknown;
    EXPECT_EQ(unknown.find("100"), std::string::npos) << unknown;

    const std::string direction = rejection(OperationParameters().setString("direction", "Up"));
    EXPECT_FALSE(direction.empty());
    EXPECT_NE(direction.find("direction"), std::string::npos) << direction;
    EXPECT_EQ(direction.find("Up"), std::string::npos) << direction;

    // A present key of the wrong type is refused, naming the key and not the value.
    const std::string wrongType = rejection(OperationParameters().setString("value", "ninety"));
    EXPECT_NE(wrongType.find("value"), std::string::npos) << wrongType;
    EXPECT_EQ(wrongType.find("ninety"), std::string::npos) << wrongType;
    EXPECT_FALSE(rejection(OperationParameters().setBool("value", true)).empty());
    EXPECT_FALSE(rejection(OperationParameters().setDouble("direction", 1.0)).empty());

    EXPECT_THROW(makeFixedThreshold(std::numeric_limits<double>::quiet_NaN(), ThresholdDirection::Above),
                 InvalidArgumentError);
    EXPECT_THROW(makeFixedThreshold(std::numeric_limits<double>::infinity(), ThresholdDirection::Above),
                 InvalidArgumentError);
    EXPECT_THROW(makeFixedThreshold(1.0, static_cast<ThresholdDirection>(7)), InvalidArgumentError);
}

TEST(ThresholdFixed, IntegerValueReadAsDouble) {
    // An integer a double holds exactly is the same value as that double.
    for (int64_t value : {int64_t{128}, int64_t{-5}, int64_t{1} << 53, -(int64_t{1} << 53)}) {
        const OperationPtr fromInt =
            OperationCatalog::instance().create("pixel.threshold_fixed", OperationParameters().setInt("value", value));
        const OperationPtr fromDouble = OperationCatalog::instance().create(
            "pixel.threshold_fixed", OperationParameters().setDouble("value", static_cast<double>(value)));
        EXPECT_TRUE(detail::parametersEqual(fromInt->parameters(), fromDouble->parameters())) << value;
        double stored = 0.0;
        EXPECT_TRUE(fromInt->parameters().getDouble("value", stored)) << value;
    }
    // One beyond 2^53 has no exact double: refused, naming the key and not the value.
    for (int64_t value : {(int64_t{1} << 53) + 1, -(int64_t{1} << 53) - 1}) {
        const std::string message = rejection(OperationParameters().setInt("value", value));
        EXPECT_NE(message.find("pixel.threshold_fixed"), std::string::npos) << message;
        EXPECT_NE(message.find("'value'"), std::string::npos) << message;
        EXPECT_EQ(message.find(std::to_string(value)), std::string::npos) << message;
    }
}

TEST(ThresholdFixed, HandWrittenIntegerJsonCreates) {
    // A recorded configuration written by hand with "value": 128 reads back as an
    // integer and still creates the operation.
    PipelineSpec written;
    written.operation("pixel.threshold_fixed",
                      OperationParameters().setDouble("value", 128.5).setString("direction", "Above"));
    const int32_t n = written.toJson(nullptr, 0);
    std::vector<char> buffer(static_cast<size_t>(n) + 1);
    written.toJson(buffer.data(), n + 1);
    std::string json(buffer.data(), static_cast<size_t>(n));
    const size_t at = json.find("128.5");
    ASSERT_NE(at, std::string::npos) << json;
    json.replace(at, 5, "128");
    const PipelineSpec spec = PipelineSpec::fromJson(json.c_str());
    int64_t asInt = 0;
    ASSERT_TRUE(spec.operationParameters(0).getInt("value", asInt)) << json;
    EXPECT_EQ(asInt, 128);
    const OperationPtr op = OperationCatalog::instance().create(spec.operationName(0), spec.operationParameters(0));
    EXPECT_TRUE(detail::parametersEqual(op->parameters(),
                                        makeFixedThreshold(128.0, ThresholdDirection::Above)->parameters()));

    auto scene = ts::makeSyntheticScene({{20, 10, 0, 0}}, {slideio::DataType::DT_Byte},
                                        [](int, int64_t x, int64_t) { return static_cast<double>(x * 13); });
    const ts::Image out = ts::runEager(scene, spec, 64, 1);
    ASSERT_EQ(out.width, 20);
    for (int64_t x = 0; x < 20; ++x) {
        EXPECT_EQ(out.at(0, x, 0), x * 13 > 128 ? 255.0 : 0.0) << x;
    }
}

TEST(ThresholdFixed, StrictComparisonEveryType) {
    for (int32_t i = 0; i < 11; ++i) {
        expectStrict(static_cast<DataType>(i));
    }
}

TEST(ThresholdFixed, FractionalAndOutOfRangeThresholdsOnIntegers) {
    const auto samples = [](int32_t, int64_t x, int64_t) { return static_cast<double>(x == 0 ? 0 : x == 1 ? 100 : x == 2 ? 101 : 255); };
    auto run = [&](double value, ThresholdDirection direction, DataType type) {
        const OperationPtr op = makeFixedThreshold(value, direction);
        return row(ts::applyDirect(*op, {type}, {0}, 4, 1, samples));
    };
    // 100.5 lies between two integers: 100 is below it, 101 above it.
    EXPECT_EQ(run(100.5, ThresholdDirection::Above, DataType::Byte), (std::vector<double>{0, 0, 255, 255}));
    EXPECT_EQ(run(100.5, ThresholdDirection::Below, DataType::Byte), (std::vector<double>{255, 255, 0, 0}));
    EXPECT_EQ(run(100.5, ThresholdDirection::Above, DataType::Int16), (std::vector<double>{0, 0, 255, 255}));
    // Thresholds outside the type's range: every sample lies on one side.
    EXPECT_EQ(run(-5.0, ThresholdDirection::Above, DataType::Byte), (std::vector<double>{255, 255, 255, 255}));
    EXPECT_EQ(run(-5.0, ThresholdDirection::Below, DataType::Byte), (std::vector<double>{0, 0, 0, 0}));
    EXPECT_EQ(run(300.0, ThresholdDirection::Above, DataType::Byte), (std::vector<double>{0, 0, 0, 0}));
    EXPECT_EQ(run(300.0, ThresholdDirection::Below, DataType::Byte), (std::vector<double>{255, 255, 255, 255}));
    EXPECT_EQ(run(1e300, ThresholdDirection::Below, DataType::Int32), (std::vector<double>{255, 255, 255, 255}));
    EXPECT_EQ(run(-1e300, ThresholdDirection::Above, DataType::UInt32), (std::vector<double>{255, 255, 255, 255}));
    // At the range's edge: 255 is not strictly above 255, 0 not strictly below 0.
    EXPECT_EQ(run(255.0, ThresholdDirection::Above, DataType::Byte), (std::vector<double>{0, 0, 0, 0}));
    EXPECT_EQ(run(0.0, ThresholdDirection::Below, DataType::Byte), (std::vector<double>{0, 0, 0, 0}));
}

TEST(ThresholdFixed, Int64Near2Pow53IsExact) {
    constexpr int64_t kTwo53 = int64_t{1} << 53;
    const OperationPtr above = makeFixedThreshold(9007199254740992.0, ThresholdDirection::Above);
    const ts::Image signedOut =
        ts::applyDirectRaw(*above, {DataType::Int64}, {0}, 2, 1, {rawOf(std::vector<int64_t>{kTwo53, kTwo53 + 1})});
    EXPECT_EQ(row(signedOut), (std::vector<double>{0, 255}));

    const OperationPtr below = makeFixedThreshold(9007199254740994.0, ThresholdDirection::Below);
    const ts::Image belowOut =
        ts::applyDirectRaw(*below, {DataType::Int64}, {0}, 2, 1, {rawOf(std::vector<int64_t>{kTwo53 + 1, kTwo53 + 2})});
    EXPECT_EQ(row(belowOut), (std::vector<double>{255, 0}));

    const OperationPtr top = makeFixedThreshold(1.8446744073709552e19, ThresholdDirection::Above);
    const ts::Image unsignedOut = ts::applyDirectRaw(
        *top, {DataType::UInt64}, {0}, 1, 1, {rawOf(std::vector<uint64_t>{18446744073709551615ull})});
    EXPECT_EQ(row(unsignedOut), (std::vector<double>{0}));

    const OperationPtr topBelow = makeFixedThreshold(1.8446744073709552e19, ThresholdDirection::Below);
    const ts::Image unsignedBelow = ts::applyDirectRaw(
        *topBelow, {DataType::UInt64}, {0}, 1, 1, {rawOf(std::vector<uint64_t>{18446744073709551615ull})});
    EXPECT_EQ(row(unsignedBelow), (std::vector<double>{255}));
}

TEST(ThresholdFixed, UInt64NegativeAndFractional) {
    const std::vector<uint8_t> samples = rawOf(std::vector<uint64_t>{0, 4, 5, 18446744073709551615ull});
    const auto run = [&](double value, ThresholdDirection direction) {
        const OperationPtr op = makeFixedThreshold(value, direction);
        return row(ts::applyDirectRaw(*op, {DataType::UInt64}, {0}, 4, 1, {samples}));
    };
    // Below zero: every sample is above, none below.
    EXPECT_EQ(run(-0.5, ThresholdDirection::Above), (std::vector<double>{255, 255, 255, 255}));
    EXPECT_EQ(run(-0.5, ThresholdDirection::Below), (std::vector<double>{0, 0, 0, 0}));
    EXPECT_EQ(run(-1e19, ThresholdDirection::Above), (std::vector<double>{255, 255, 255, 255}));
    EXPECT_EQ(run(-1e19, ThresholdDirection::Below), (std::vector<double>{0, 0, 0, 0}));
    // Between two integers.
    EXPECT_EQ(run(4.5, ThresholdDirection::Above), (std::vector<double>{0, 0, 255, 255}));
    EXPECT_EQ(run(4.5, ThresholdDirection::Below), (std::vector<double>{255, 255, 0, 0}));
    EXPECT_EQ(run(0.25, ThresholdDirection::Below), (std::vector<double>{255, 0, 0, 0}));
}

TEST(ThresholdFixed, Int64NearItsLimits) {
    constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
    constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
    constexpr int64_t kBelowTwo63 = kMax - 1023; // 2^63 - 1024, the largest double below 2^63
    const std::vector<uint8_t> samples =
        rawOf(std::vector<int64_t>{kMin, kMin + 1, kBelowTwo63, kBelowTwo63 + 1, kMax});
    const auto run = [&](double value, ThresholdDirection direction) {
        const OperationPtr op = makeFixedThreshold(value, direction);
        return row(ts::applyDirectRaw(*op, {DataType::Int64}, {0}, 5, 1, {samples}));
    };
    constexpr double kTwo63 = 9223372036854775808.0;
    // 2^63 lies above every Int64.
    EXPECT_EQ(run(kTwo63, ThresholdDirection::Above), (std::vector<double>{0, 0, 0, 0, 0}));
    EXPECT_EQ(run(kTwo63, ThresholdDirection::Below), (std::vector<double>{255, 255, 255, 255, 255}));
    // 2^63 - 1024 is an Int64 value: compared exactly.
    EXPECT_EQ(run(static_cast<double>(kBelowTwo63), ThresholdDirection::Above), (std::vector<double>{0, 0, 0, 255, 255}));
    EXPECT_EQ(run(static_cast<double>(kBelowTwo63), ThresholdDirection::Below),
              (std::vector<double>{255, 255, 0, 0, 0}));
    // -2^63 is the minimum itself: nothing is below it, everything else above.
    EXPECT_EQ(run(-kTwo63, ThresholdDirection::Above), (std::vector<double>{0, 255, 255, 255, 255}));
    EXPECT_EQ(run(-kTwo63, ThresholdDirection::Below), (std::vector<double>{0, 0, 0, 0, 0}));
    // Below the type's range.
    EXPECT_EQ(run(-1e19, ThresholdDirection::Above), (std::vector<double>{255, 255, 255, 255, 255}));
    EXPECT_EQ(run(-1e19, ThresholdDirection::Below), (std::vector<double>{0, 0, 0, 0, 0}));
}

TEST(ThresholdFixed, TiledEqualsSingleTile) {
    auto scene = ts::makeSyntheticScene({{300, 260, 0, 0}}, {slideio::DataType::DT_Byte}, [](int, int64_t x, int64_t y) {
        return static_cast<double>((x * 7 + y * 13 + (x * y) % 11) % 256);
    });
    PipelineSpec spec;
    spec.then(makeFixedThreshold(128.0, ThresholdDirection::Above));
    const ts::Image reference = ts::runEager(scene, spec, 4096, 1);
    ASSERT_EQ(reference.width, 300);
    ASSERT_EQ(reference.height, 260);
    ASSERT_EQ(reference.types, std::vector<DataType>{DataType::Byte});
    for (int64_t y = 0; y < 260; y += 37) {
        for (int64_t x = 0; x < 300; x += 23) {
            const double sample = static_cast<double>((x * 7 + y * 13 + (x * y) % 11) % 256);
            EXPECT_EQ(reference.at(0, x, y), sample > 128.0 ? 255.0 : 0.0) << x << "," << y;
        }
    }
    for (int32_t tile : {64, 100, 257}) {
        const ts::Image tiled = ts::runEager(scene, spec, tile, 4);
        EXPECT_TRUE(tiled.planes == reference.planes) << "tile " << tile;
    }
}
