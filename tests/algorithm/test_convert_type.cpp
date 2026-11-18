// Requirements: UR-IMG-13, UR-IMG-10, UR-IMG-11, UR-ALG-6
#include <slideio/processor/algorithm/pixel/convert.hpp>
#include <slideio/processor/core/block_view.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/pipeline/operation_catalog.hpp>
#include <slideio/processor/pipeline/pipeline_spec.hpp>

#include <algorithm/support/harness.hpp>
#include <pipeline/detail/operation_context_state.hpp>
#include <pipeline/detail/parameters_json.hpp>
#include <pipeline/test_support/synthetic_scene.hpp>

#include <gtest/gtest.h>

#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace slideio::processor;
namespace ts = slideio::processor::testing_support;

namespace {

constexpr const char* kName = "pixel.convert_type";
constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

template <typename T>
std::vector<uint8_t> rawOf(const std::vector<T>& samples) {
    std::vector<uint8_t> bytes(samples.size() * sizeof(T));
    std::memcpy(bytes.data(), samples.data(), bytes.size());
    return bytes;
}

// The samples of plane 0, row 0 of @p image, read as T.
template <typename T>
std::vector<T> samplesOf(const ts::Image& image) {
    std::vector<T> out(static_cast<size_t>(image.width));
    std::memcpy(out.data(), image.planes.at(0).data(), out.size() * sizeof(T));
    return out;
}

// Converts one row of raw samples.
template <typename T>
ts::Image convertRow(DataType from, const std::vector<T>& samples, DataType to,
                     RoundingMode rounding = RoundingMode::Nearest, OverflowMode overflow = OverflowMode::Saturate) {
    const OperationPtr op = makeTypeConvert(to, rounding, overflow);
    return ts::applyDirectRaw(*op, {from}, {0}, static_cast<int64_t>(samples.size()), 1, {rawOf(samples)});
}

// The message of the InvalidArgumentError converting the row under OverflowMode::Error; "" when none.
template <typename T>
std::string errorOf(DataType from, const std::vector<T>& samples, DataType to,
                    RoundingMode rounding = RoundingMode::Nearest) {
    try {
        convertRow(from, samples, to, rounding, OverflowMode::Error);
    } catch (const InvalidArgumentError& e) {
        return e.what();
    }
    return "";
}

std::string rejection(const OperationParameters& p) {
    try {
        OperationCatalog::instance().create(kName, p);
    } catch (const InvalidArgumentError& e) {
        return e.what();
    }
    return "";
}

template <typename T>
void expectIdentity(DataType type, std::vector<T> samples) {
    SCOPED_TRACE(dataTypeName(type));
    samples.push_back(std::numeric_limits<T>::lowest());
    samples.push_back(std::numeric_limits<T>::max());
    samples.push_back(T(0));
    samples.push_back(T(1));
    for (OverflowMode overflow : {OverflowMode::Saturate, OverflowMode::Error}) {
        const ts::Image out = convertRow(type, samples, type, RoundingMode::Nearest, overflow);
        ASSERT_EQ(out.types, std::vector<DataType>{type});
        EXPECT_TRUE(out.planes.at(0) == rawOf(samples));
    }
}

} // namespace

TEST(ConvertType, FactoryEqualsNamePath) {
    OperationParameters p;
    p.setString("target", "Int16").setString("rounding", "Floor").setString("overflow", "Error");
    EXPECT_TRUE(detail::parametersEqual(makeTypeConvert(DataType::Int16, RoundingMode::Floor, OverflowMode::Error)->parameters(),
                                        OperationCatalog::instance().create(kName, p)->parameters()));

    // Defaults fill the bag.
    const OperationPtr byDefault = OperationCatalog::instance().create(kName, OperationParameters());
    const OperationParameters& bag = byDefault->parameters();
    EXPECT_EQ(bag.count(), 3);
    EXPECT_STREQ(bag.getString("target", nullptr), "Float32");
    EXPECT_STREQ(bag.getString("rounding", nullptr), "Nearest");
    EXPECT_STREQ(bag.getString("overflow", nullptr), "Saturate");
    EXPECT_TRUE(detail::parametersEqual(
        OperationCatalog::instance().create(kName, OperationParameters().setString("rounding", "TowardZero"))->parameters(),
        makeTypeConvert(DataType::Float32, RoundingMode::TowardZero, OverflowMode::Saturate)->parameters()));

    // Every enumerator spelling round-trips.
    for (const char* spelling : {"Nearest", "TowardZero", "Floor", "Ceil"}) {
        EXPECT_STREQ(OperationCatalog::instance()
                         .create(kName, OperationParameters().setString("rounding", spelling))
                         ->parameters()
                         .getString("rounding", nullptr),
                     spelling);
    }
    for (int32_t i = 0; i < 11; ++i) {
        const char* name = dataTypeName(static_cast<DataType>(i));
        const OperationPtr op = OperationCatalog::instance().create(kName, OperationParameters().setString("target", name));
        EXPECT_EQ(op->outputType(DataType::Byte), static_cast<DataType>(i)) << name;
    }

    // An unknown target spelling, enumerator or key throws, naming the key and not the value.
    const std::string target = rejection(OperationParameters().setString("target", "Float31"));
    EXPECT_NE(target.find("target"), std::string::npos) << target;
    EXPECT_NE(target.find(kName), std::string::npos) << target;
    EXPECT_EQ(target.find("Float31"), std::string::npos) << target;
    EXPECT_FALSE(rejection(OperationParameters().setString("target", "Unknown")).empty());
    EXPECT_FALSE(rejection(OperationParameters().setInt("target", 9)).empty());
    EXPECT_FALSE(rejection(OperationParameters().setString("rounding", "Up")).empty());
    EXPECT_FALSE(rejection(OperationParameters().setString("overflow", "Wrap")).empty());
    const std::string unknown = rejection(OperationParameters().setString("taget", "Byte"));
    EXPECT_NE(unknown.find("taget"), std::string::npos) << unknown;

    EXPECT_THROW(makeTypeConvert(DataType::Unknown, RoundingMode::Nearest, OverflowMode::Saturate), InvalidArgumentError);
    EXPECT_THROW(makeTypeConvert(DataType::Byte, static_cast<RoundingMode>(9), OverflowMode::Saturate),
                 InvalidArgumentError);
    EXPECT_THROW(makeTypeConvert(DataType::Byte, RoundingMode::Nearest, static_cast<OverflowMode>(9)),
                 InvalidArgumentError);
}

TEST(ConvertType, DeclaresItsContract) {
    const OperationPtr op = makeTypeConvert(DataType::Int16, RoundingMode::Nearest, OverflowMode::Saturate);
    EXPECT_STREQ(op->name(), kName);
    EXPECT_EQ(op->arity(), ChannelArity::PerChannel);
    EXPECT_FALSE(op->requiresUniformTypes());
    EXPECT_TRUE(op->supportedTypes() == DataTypeSet::all());
    EXPECT_TRUE(op->halo().isEmpty());
    EXPECT_FALSE(op->producesObjects());
    for (int32_t i = 0; i < 11; ++i) {
        EXPECT_EQ(op->outputType(static_cast<DataType>(i)), DataType::Int16);
    }
    EXPECT_STREQ(OperationCatalog::instance().correctnessReference(kName),
                 "Analytic: the documented rounding and overflow rules, checked at each type's minimum, maximum and "
                 "overflow");
    EXPECT_STREQ(OperationCatalog::instance().complexityNote(kName), "O(n) in the number of samples");
}

TEST(ConvertType, IdentityIsBitExactEveryType) {
    constexpr int64_t kTwo53Plus1 = (int64_t{1} << 53) + 1;
    expectIdentity<uint8_t>(DataType::Byte, {7, 200});
    expectIdentity<int8_t>(DataType::Int8, {-7, 100});
    expectIdentity<uint16_t>(DataType::UInt16, {7, 60000});
    expectIdentity<int16_t>(DataType::Int16, {-7, 30000});
    expectIdentity<uint32_t>(DataType::UInt32, {7, 4000000000u});
    expectIdentity<int32_t>(DataType::Int32, {-7, 2000000000});
    expectIdentity<uint64_t>(DataType::UInt64, {static_cast<uint64_t>(kTwo53Plus1), 18446744073709551615ull});
    expectIdentity<int64_t>(DataType::Int64, {kTwo53Plus1, -kTwo53Plus1});
    expectIdentity<float>(DataType::Float32, {-0.0f, std::numeric_limits<float>::denorm_min(),
                                              std::numeric_limits<float>::infinity(),
                                              -std::numeric_limits<float>::infinity(),
                                              std::numeric_limits<float>::quiet_NaN(), 0.1f});
    expectIdentity<double>(DataType::Float64, {-0.0, std::numeric_limits<double>::denorm_min(), kInf, -kInf, kNaN, 0.1});

    // Float16: a subnormal, -0, +inf, -inf, NaN (two payloads), the largest finite value, 1.
    const std::vector<uint16_t> half{0x0001, 0x03FF, 0x8000, 0x7C00, 0xFC00, 0x7E00, 0x7D01, 0x7BFF, 0xFBFF, 0x3C00};
    for (OverflowMode overflow : {OverflowMode::Saturate, OverflowMode::Error}) {
        const ts::Image out = convertRow(DataType::Float16, half, DataType::Float16, RoundingMode::Nearest, overflow);
        EXPECT_TRUE(out.planes.at(0) == rawOf(half));
    }
}

TEST(ConvertType, IntegerBoundaries) {
    const std::vector<int16_t> values{-1, 0, 255, 256};
    EXPECT_EQ(samplesOf<uint8_t>(convertRow(DataType::Int16, values, DataType::Byte)),
              (std::vector<uint8_t>{0, 0, 255, 255}));
    const std::string error = errorOf(DataType::Int16, values, DataType::Byte);
    EXPECT_EQ(error, "pixel.convert_type: 2 samples of Int16 do not fit Byte");
    EXPECT_NE(error.find("2 samples"), std::string::npos) << error;
    EXPECT_EQ(error.find("256"), std::string::npos) << error;

    EXPECT_EQ(samplesOf<int64_t>(convertRow(DataType::UInt64, std::vector<uint64_t>{18446744073709551615ull, 5},
                                            DataType::Int64)),
              (std::vector<int64_t>{std::numeric_limits<int64_t>::max(), 5}));
    EXPECT_FALSE(errorOf(DataType::UInt64, std::vector<uint64_t>{18446744073709551615ull}, DataType::Int64).empty());

    EXPECT_EQ(samplesOf<uint32_t>(convertRow(DataType::Int64,
                                             std::vector<int64_t>{std::numeric_limits<int64_t>::min(),
                                                                  int64_t{4294967296}, int64_t{4294967295}},
                                             DataType::UInt32)),
              (std::vector<uint32_t>{0, 4294967295u, 4294967295u}));

    // Signed into UInt64, and the widest values in range, are exact.
    EXPECT_EQ(samplesOf<uint64_t>(convertRow(DataType::Int64,
                                             std::vector<int64_t>{-1, std::numeric_limits<int64_t>::max()},
                                             DataType::UInt64)),
              (std::vector<uint64_t>{0, 9223372036854775807ull}));
    EXPECT_EQ(samplesOf<int8_t>(convertRow(DataType::Int32, std::vector<int32_t>{-128, 127, -129, 128},
                                           DataType::Int8)),
              (std::vector<int8_t>{-128, 127, -128, 127}));
    EXPECT_EQ(errorOf(DataType::Int32, std::vector<int32_t>{-128, 127}, DataType::Int8), "");
}

TEST(ConvertType, RoundingModes) {
    const std::vector<float> values{2.5f, -2.5f, 1.5f, -0.5f};
    auto run = [&](RoundingMode mode) { return samplesOf<int32_t>(convertRow(DataType::Float32, values, DataType::Int32, mode)); };
    EXPECT_EQ(run(RoundingMode::Nearest), (std::vector<int32_t>{2, -2, 2, 0}));
    EXPECT_EQ(run(RoundingMode::TowardZero), (std::vector<int32_t>{2, -2, 1, 0}));
    EXPECT_EQ(run(RoundingMode::Floor), (std::vector<int32_t>{2, -3, 1, -1}));
    EXPECT_EQ(run(RoundingMode::Ceil), (std::vector<int32_t>{3, -2, 2, 0}));

    // Rounding affects only integer targets.
    const std::vector<double> fine{1.0 + std::ldexp(1.0, -40)};
    EXPECT_EQ(samplesOf<float>(convertRow(DataType::Float64, fine, DataType::Float32, RoundingMode::Ceil)),
              (std::vector<float>{1.0f}));
}

TEST(ConvertType, FloatToIntegerRange) {
    const double two63 = std::ldexp(1.0, 63);
    EXPECT_EQ(samplesOf<int64_t>(convertRow(DataType::Float64, std::vector<double>{1e19, -1e19, two63, -two63},
                                            DataType::Int64)),
              (std::vector<int64_t>{std::numeric_limits<int64_t>::max(), std::numeric_limits<int64_t>::min(),
                                    std::numeric_limits<int64_t>::max(), std::numeric_limits<int64_t>::min()}));
    EXPECT_EQ(errorOf(DataType::Float64, std::vector<double>{1e19, -1e19, two63, -two63}, DataType::Int64),
              "pixel.convert_type: 3 samples of Float64 do not fit Int64");
    EXPECT_EQ(samplesOf<uint64_t>(convertRow(DataType::Float64, std::vector<double>{-1.0, two63, std::ldexp(1.0, 64)},
                                             DataType::UInt64)),
              (std::vector<uint64_t>{0, 9223372036854775808ull, 18446744073709551615ull}));
    // 255.4 rounds into range; 255.5 rounds to 256 and out of it; -0.5 rounds to 0 and in.
    EXPECT_EQ(samplesOf<uint8_t>(convertRow(DataType::Float32, std::vector<float>{255.4f, 255.5f, -0.5f},
                                            DataType::Byte)),
              (std::vector<uint8_t>{255, 255, 0}));
    EXPECT_EQ(errorOf(DataType::Float32, std::vector<float>{255.4f, 255.5f, -0.5f}, DataType::Byte),
              "pixel.convert_type: 1 samples of Float32 do not fit Byte");
    EXPECT_EQ(errorOf(DataType::Float32, std::vector<float>{255.5f}, DataType::Byte, RoundingMode::Floor), "");
    // A Float16 source.
    EXPECT_EQ(samplesOf<int16_t>(convertRow(DataType::Float16, std::vector<uint16_t>{0x7BFF, 0xC100, 0x3800},
                                            DataType::Int16)),
              (std::vector<int16_t>{32767, -2, 0}));
}

TEST(ConvertType, NonFiniteToInteger) {
    const std::vector<float> values{std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                                    -std::numeric_limits<float>::infinity()};
    EXPECT_EQ(samplesOf<uint8_t>(convertRow(DataType::Float32, values, DataType::Byte)),
              (std::vector<uint8_t>{0, 255, 0}));
    for (float value : values) {
        EXPECT_EQ(errorOf(DataType::Float32, std::vector<float>{value}, DataType::Byte),
                  "pixel.convert_type: 1 samples of Float32 do not fit Byte");
    }
    EXPECT_EQ(samplesOf<int64_t>(convertRow(DataType::Float64, std::vector<double>{kNaN, kInf, -kInf}, DataType::Int64)),
              (std::vector<int64_t>{0, std::numeric_limits<int64_t>::max(), std::numeric_limits<int64_t>::min()}));
}

TEST(ConvertType, FloatNarrowing) {
    EXPECT_EQ(samplesOf<float>(convertRow(DataType::Float64, std::vector<double>{1e300, -1e300}, DataType::Float32)),
              (std::vector<float>{FLT_MAX, -FLT_MAX}));
    EXPECT_EQ(errorOf(DataType::Float64, std::vector<double>{1e300}, DataType::Float32),
              "pixel.convert_type: 1 samples of Float64 do not fit Float32");
    // FLT_MAX itself and the largest double that rounds down to it are in range.
    EXPECT_EQ(errorOf(DataType::Float64, std::vector<double>{static_cast<double>(FLT_MAX), 0x1.fffffefffffffp+127},
                      DataType::Float32),
              "");
    EXPECT_EQ(errorOf(DataType::Float64, std::vector<double>{0x1.ffffffp+127}, DataType::Float32),
              "pixel.convert_type: 1 samples of Float64 do not fit Float32");

    EXPECT_EQ(samplesOf<uint16_t>(convertRow(DataType::Float32, std::vector<float>{65520.0f, -65520.0f}, DataType::Float16)),
              (std::vector<uint16_t>{0x7BFF, 0xFBFF}));
    EXPECT_EQ(errorOf(DataType::Float32, std::vector<float>{65520.0f}, DataType::Float16),
              "pixel.convert_type: 1 samples of Float32 do not fit Float16");
    EXPECT_EQ(errorOf(DataType::Float32, std::vector<float>{65504.0f, 65519.0f}, DataType::Float16), "");
    EXPECT_EQ(samplesOf<uint16_t>(convertRow(DataType::Float32, std::vector<float>{65519.0f}, DataType::Float16)),
              (std::vector<uint16_t>{0x7BFF}));

    EXPECT_EQ(samplesOf<float>(convertRow(DataType::Float64, std::vector<double>{1.0 + std::ldexp(1.0, -53)},
                                          DataType::Float32)),
              (std::vector<float>{1.0f}));

    // Binary16 encoding rounds to nearest, ties to even, subnormals included.
    EXPECT_EQ(samplesOf<uint16_t>(convertRow(DataType::Float32,
                                             std::vector<float>{2049.0f, 2051.0f, std::ldexp(1.0f, -25),
                                                                std::ldexp(3.0f, -25), std::ldexp(1.0f, -24),
                                                                std::ldexp(1023.5f, -24), -0.0f, 1.0f, 0.1f},
                                             DataType::Float16)),
              (std::vector<uint16_t>{0x6800, 0x6802, 0x0000, 0x0002, 0x0001, 0x0400, 0x8000, 0x3C00, 0x2E66}));

    // NaN and infinity pass through, and are no overflow.
    const std::vector<double> nonFinite{kNaN, kInf, -kInf};
    const std::vector<float> narrowed = samplesOf<float>(convertRow(DataType::Float64, nonFinite, DataType::Float32));
    EXPECT_TRUE(std::isnan(narrowed[0]));
    EXPECT_EQ(narrowed[1], std::numeric_limits<float>::infinity());
    EXPECT_EQ(narrowed[2], -std::numeric_limits<float>::infinity());
    EXPECT_EQ(errorOf(DataType::Float64, nonFinite, DataType::Float32), "");
    const std::vector<uint16_t> half = samplesOf<uint16_t>(convertRow(DataType::Float64, nonFinite, DataType::Float16));
    EXPECT_EQ(half[0] & 0x7C00, 0x7C00);
    EXPECT_NE(half[0] & 0x03FF, 0);
    EXPECT_EQ(half[1], 0x7C00);
    EXPECT_EQ(half[2], 0xFC00);
    EXPECT_EQ(errorOf(DataType::Float64, nonFinite, DataType::Float16), "");

    // Widening is exact.
    EXPECT_EQ(samplesOf<double>(convertRow(DataType::Float16, std::vector<uint16_t>{0x0001, 0x7BFF, 0x8000},
                                           DataType::Float64)),
              (std::vector<double>{std::ldexp(1.0, -24), 65504.0, -0.0}));
    EXPECT_EQ(samplesOf<double>(convertRow(DataType::Float32, std::vector<float>{0.1f}, DataType::Float64)),
              (std::vector<double>{static_cast<double>(0.1f)}));
}

TEST(ConvertType, IntegerToFloat16) {
    const std::vector<int32_t> values{65504, 65519, 65520, -70000, 2049};
    EXPECT_EQ(samplesOf<uint16_t>(convertRow(DataType::Int32, values, DataType::Float16)),
              (std::vector<uint16_t>{0x7BFF, 0x7BFF, 0x7BFF, 0xFBFF, 0x6800}));
    EXPECT_EQ(errorOf(DataType::Int32, values, DataType::Float16),
              "pixel.convert_type: 2 samples of Int32 do not fit Float16");
    EXPECT_EQ(samplesOf<uint16_t>(convertRow(DataType::UInt64, std::vector<uint64_t>{18446744073709551615ull},
                                             DataType::Float16)),
              (std::vector<uint16_t>{0x7BFF}));
}

TEST(ConvertType, Int64ToFloat64RoundsToNearest) {
    constexpr int64_t kTwo53 = int64_t{1} << 53;
    EXPECT_EQ(samplesOf<double>(convertRow(DataType::Int64, std::vector<int64_t>{kTwo53 + 1, kTwo53 + 3, -(kTwo53 + 1)},
                                           DataType::Float64)),
              (std::vector<double>{9007199254740992.0, 9007199254740996.0, -9007199254740992.0}));
    EXPECT_EQ(errorOf(DataType::Int64, std::vector<int64_t>{kTwo53 + 1}, DataType::Float64), "");
    EXPECT_EQ(samplesOf<double>(convertRow(DataType::UInt64, std::vector<uint64_t>{18446744073709551615ull},
                                           DataType::Float64)),
              (std::vector<double>{18446744073709551616.0}));
    // Ties with the top bit set round to the even significand: 1 ulp + half goes up to 2 ulp, half an ulp
    // goes down to 0; just above a tie rounds up.
    const uint64_t two63 = uint64_t{1} << 63;
    EXPECT_EQ(samplesOf<float>(convertRow(DataType::UInt64,
                                          std::vector<uint64_t>{two63 + (uint64_t{1} << 39), two63 + 3 * (uint64_t{1} << 39),
                                                                two63 + (uint64_t{1} << 39) + 1},
                                          DataType::Float32)),
              (std::vector<float>{std::ldexp(1.0f, 63), std::ldexp(1.0f, 63) + std::ldexp(1.0f, 41),
                                  std::ldexp(1.0f, 63) + std::ldexp(1.0f, 40)}));
    EXPECT_EQ(samplesOf<double>(convertRow(DataType::UInt64,
                                           std::vector<uint64_t>{two63 + (uint64_t{1} << 10), two63 + 3 * (uint64_t{1} << 10),
                                                                 two63 + (uint64_t{1} << 10) + 1},
                                           DataType::Float64)),
              (std::vector<double>{std::ldexp(1.0, 63), std::ldexp(1.0, 63) + std::ldexp(1.0, 12),
                                   std::ldexp(1.0, 63) + std::ldexp(1.0, 11)}));
    const int64_t two62 = int64_t{1} << 62;
    EXPECT_EQ(samplesOf<float>(convertRow(DataType::Int64,
                                          std::vector<int64_t>{-(two62 + (int64_t{1} << 38)),
                                                               -(two62 + 3 * (int64_t{1} << 38)),
                                                               std::numeric_limits<int64_t>::min()},
                                          DataType::Float32)),
              (std::vector<float>{-std::ldexp(1.0f, 62), -(std::ldexp(1.0f, 62) + std::ldexp(1.0f, 40)),
                                  -std::ldexp(1.0f, 63)}));
    EXPECT_EQ(samplesOf<double>(convertRow(DataType::Int64,
                                           std::vector<int64_t>{-(two62 + (int64_t{1} << 9)),
                                                                -(two62 + 3 * (int64_t{1} << 9)),
                                                                std::numeric_limits<int64_t>::min()},
                                           DataType::Float64)),
              (std::vector<double>{-std::ldexp(1.0, 62), -(std::ldexp(1.0, 62) + std::ldexp(1.0, 11)),
                                   -std::ldexp(1.0, 63)}));
    // 2^24 + 1 ties to 2^24 in Float32.
    EXPECT_EQ(samplesOf<float>(convertRow(DataType::Int32, std::vector<int32_t>{16777217, 16777219}, DataType::Float32)),
              (std::vector<float>{16777216.0f, 16777220.0f}));
}

TEST(ConvertType, ThroughAPipeline) {
    const auto pixel = [](int64_t x, int64_t y) { return static_cast<double>((x * 257 + y * 131 + x * y) % 65536); };
    auto scene = ts::makeSyntheticScene({{300, 260, 0, 0}}, {slideio::DataType::DT_UInt16},
                                        [&](int, int64_t x, int64_t y) { return pixel(x, y); });
    PipelineSpec spec;
    spec.then(makeTypeConvert(DataType::Float32, RoundingMode::Nearest, OverflowMode::Error));
    for (int32_t tile : {64, 257}) {
        const ts::Image out = ts::runEager(scene, spec, tile, 4);
        ASSERT_EQ(out.width, 300);
        ASSERT_EQ(out.height, 260);
        ASSERT_EQ(out.types, std::vector<DataType>{DataType::Float32});
        int64_t mismatches = 0;
        for (int64_t y = 0; y < out.height; ++y) {
            for (int64_t x = 0; x < out.width; ++x) {
                if (out.at(0, x, y) != pixel(x, y)) {
                    ++mismatches;
                }
            }
        }
        EXPECT_EQ(mismatches, 0) << "tile " << tile;
    }
}

TEST(ConvertType, ErrorNamesEveryOverflowingSourceType) {
    // One call over planes of several types, as a caller applying the operation
    // to a multi-plane tile makes it: Int16 (2 overflow), Byte (none), Int32 (1),
    // Int16 again (1), all into Int8.
    std::vector<std::vector<uint8_t>> planes{rawOf(std::vector<int16_t>{200, -200, 5}),
                                             rawOf(std::vector<uint8_t>{1, 2, 3}),
                                             rawOf(std::vector<int32_t>{1000, 0, 0}),
                                             rawOf(std::vector<int16_t>{0, 0, 300})};
    const std::vector<DataType> types{DataType::Int16, DataType::Byte, DataType::Int32, DataType::Int16};
    std::vector<BlockPlane> inPlanes;
    std::vector<std::vector<uint8_t>> outBytes(planes.size(), std::vector<uint8_t>(3, 0));
    std::vector<BlockPlane> outPlanes;
    for (size_t p = 0; p < planes.size(); ++p) {
        inPlanes.push_back(BlockPlane{planes[p].data(), types[p], 3 * dataTypeSizeBytes(types[p]),
                                      static_cast<int32_t>(p)});
        outPlanes.push_back(BlockPlane{outBytes[p].data(), DataType::Int8, 3, static_cast<int32_t>(p)});
    }
    const BlockView input(3, 1, Rect64{0, 0, 3, 1}, 0, inPlanes.data(), static_cast<int32_t>(inPlanes.size()));
    BlockView output(3, 1, Rect64{0, 0, 3, 1}, 0, outPlanes.data(), static_cast<int32_t>(outPlanes.size()));
    const auto apply = [&](OverflowMode overflow) {
        const OperationPtr op = makeTypeConvert(DataType::Int8, RoundingMode::Nearest, overflow);
        detail::OperationContextState state;
        state.input = &input;
        state.output = &output;
        state.tileRectAtLevel = state.haloRectAtLevel = state.tileRect = state.haloRect = Rect64{0, 0, 3, 1};
        state.tileIndex = 0;
        op->apply(*detail::OperationContextAccess::make(std::move(state)));
    };
    try {
        apply(OverflowMode::Error);
        FAIL() << "no InvalidArgumentError";
    } catch (const InvalidArgumentError& e) {
        EXPECT_STREQ(e.what(), "pixel.convert_type: 4 samples of Int16, Int32 do not fit Int8");
    }
    EXPECT_NO_THROW(apply(OverflowMode::Saturate));
}
