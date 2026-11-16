// Requirements: UR-IMG-9, UR-IMG-4, UR-EXT-3
#include <slideio/processor/core/error.hpp>

#include <pipeline/detail/level_mapping.hpp>
#include <pipeline/detail/plane_buffers.hpp>
#include <pipeline/detail/scene_reader.hpp>
#include <pipeline/test_support/synthetic_scene.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace slideio::processor;
using slideio::processor::detail::PlaneBuffers;
using slideio::processor::detail::SceneReader;
namespace ts = slideio::processor::testing_support;

namespace {

double f(int c, int64_t x, int64_t y) {
    return static_cast<double>((c * 50 + x * 7 + y * 3) % 200);
}

template <typename T>
T sampleAt(const BlockView& v, int32_t plane, int64_t x, int64_t y) {
    const auto* row = static_cast<const uint8_t*>(v.planeData(plane)) + y * v.planeRowStrideBytes(plane);
    T s;
    std::memcpy(&s, row + x * static_cast<int64_t>(sizeof(T)), sizeof(T));
    return s;
}

} // namespace

TEST(SceneReader, ReadsUniformChannelsInOneCall) {
    auto source = std::make_shared<ts::SyntheticSource>(
        std::vector<ts::SyntheticLevel>{{20, 16, 0, 0}},
        std::vector<slideio::DataType>(3, slideio::DataType::DT_Byte), f);
    SceneReader reader(ts::makeSyntheticScene(source));
    EXPECT_EQ(reader.channelTypes(), std::vector<DataType>(3, DataType::Byte));

    const Rect64 rect{2, 3, 9, 7};
    PlaneBuffers planes(9, 7, {DataType::Byte, DataType::Byte, DataType::Byte}, {0, 1, 2});
    reader.read(0, rect, {0, 1, 2}, planes);
    EXPECT_EQ(source->readCount(), 1);

    const BlockView v = planes.view(0, rect, 1.0);
    ASSERT_EQ(v.planeCount(), 3);
    for (int32_t c = 0; c < 3; ++c) {
        EXPECT_EQ(v.planeSourceChannel(c), c);
        for (int64_t y = 0; y < 7; ++y) {
            for (int64_t x = 0; x < 9; ++x) {
                ASSERT_EQ(sampleAt<uint8_t>(v, c, x, y), static_cast<uint8_t>(f(c, x + 2, y + 3)));
            }
        }
    }
}

TEST(SceneReader, ReadsMixedTypesOneChannelPerCall) {
    auto source = std::make_shared<ts::SyntheticSource>(
        std::vector<ts::SyntheticLevel>{{12, 10, 0, 0}},
        std::vector<slideio::DataType>{slideio::DataType::DT_Byte, slideio::DataType::DT_Float32}, f);
    SceneReader reader(ts::makeSyntheticScene(source));
    EXPECT_EQ(reader.channelTypes(), (std::vector<DataType>{DataType::Byte, DataType::Float32}));

    const Rect64 rect{1, 1, 8, 6};
    PlaneBuffers planes(8, 6, {DataType::Byte, DataType::Float32}, {0, 1});
    reader.read(0, rect, {0, 1}, planes);
    EXPECT_EQ(source->readCount(), 2);

    const BlockView v = planes.view(0, rect, 1.0);
    EXPECT_EQ(v.planeType(0), DataType::Byte);
    EXPECT_EQ(v.planeType(1), DataType::Float32);
    for (int64_t y = 0; y < 6; ++y) {
        for (int64_t x = 0; x < 8; ++x) {
            ASSERT_EQ(sampleAt<uint8_t>(v, 0, x, y), static_cast<uint8_t>(f(0, x + 1, y + 1)));
            ASSERT_EQ(sampleAt<float>(v, 1, x, y), static_cast<float>(f(1, x + 1, y + 1)));
        }
    }
}

TEST(SceneReader, ZeroFillsOutsideTheLevel) {
    // Pixel value 9 everywhere inside, so any non-zero sample outside would show.
    SceneReader reader(ts::makeSyntheticScene({{5, 5, 0, 0}}, {slideio::DataType::DT_Byte},
                                              [](int, int64_t, int64_t) { return 9.0; }));
    const Rect64 rect{-3, -2, 10, 10};
    PlaneBuffers planes(10, 10, {DataType::Byte}, {0});
    reader.read(0, rect, {0}, planes);
    const BlockView v = planes.view(0, rect, 1.0);
    for (int64_t y = 0; y < 10; ++y) {
        for (int64_t x = 0; x < 10; ++x) {
            const bool inside = x >= 3 && x < 8 && y >= 2 && y < 7;
            ASSERT_EQ(sampleAt<uint8_t>(v, 0, x, y), inside ? 9 : 0) << x << "," << y;
        }
    }
}

TEST(SceneReader, ARectWhollyOutsideTheLevelNeverCallsTheSource) {
    auto source = std::make_shared<ts::SyntheticSource>(
        std::vector<ts::SyntheticLevel>{{5, 5, 0, 0}}, std::vector<slideio::DataType>{slideio::DataType::DT_Byte},
        [](int, int64_t, int64_t) { return 9.0; });
    SceneReader reader(ts::makeSyntheticScene(source));
    PlaneBuffers planes(4, 4, {DataType::Byte}, {0});
    reader.read(0, Rect64{10, 10, 4, 4}, {0}, planes);
    EXPECT_EQ(source->readCount(), 0);
    const BlockView v = planes.view(0, Rect64{10, 10, 4, 4}, 1.0);
    EXPECT_EQ(sampleAt<uint8_t>(v, 0, 2, 2), 0);
}

TEST(SceneReader, ReadsTheNamedLevelOnly) {
    auto source = std::make_shared<ts::SyntheticSource>(
        std::vector<ts::SyntheticLevel>{{64, 64, 0, 0}, {32, 32, 0, 0}, {16, 16, 0, 0}},
        std::vector<slideio::DataType>{slideio::DataType::DT_Byte}, f);
    SceneReader reader(ts::makeSyntheticScene(source));
    PlaneBuffers planes(8, 8, {DataType::Byte}, {0});
    reader.read(2, Rect64{4, 4, 8, 8}, {0}, planes);
    const std::vector<int> levels = source->readLevels();
    ASSERT_FALSE(levels.empty());
    for (int level : levels) {
        EXPECT_EQ(level, 2);
    }
    const BlockView v = planes.view(2, Rect64{4, 4, 8, 8}, 4.0);
    EXPECT_EQ(sampleAt<uint8_t>(v, 0, 1, 2), static_cast<uint8_t>(f(0, 5, 6)));
}

TEST(SceneReader, SlideioFailureIsIoError) {
    auto source = std::make_shared<ts::SyntheticSource>(
        std::vector<ts::SyntheticLevel>{{8, 8, 0, 0}}, std::vector<slideio::DataType>{slideio::DataType::DT_Byte}, f);
    SceneReader reader(ts::makeSyntheticScene(source));
    source->failReadsWith("disk on fire");
    PlaneBuffers planes(4, 4, {DataType::Byte}, {0});
    try {
        reader.read(0, Rect64{0, 0, 4, 4}, {0}, planes);
        FAIL() << "expected IoError";
    } catch (const IoError& e) {
        EXPECT_NE(std::string(e.cause()).find("disk on fire"), std::string::npos);
    }
}

TEST(SceneReader, RejectsBadArguments) {
    EXPECT_THROW(SceneReader{std::shared_ptr<slideio::Scene>{}}, InvalidArgumentError);
    SceneReader reader(ts::makeSyntheticScene({{8, 8, 0, 0}}, {slideio::DataType::DT_Byte}, f));
    PlaneBuffers planes(4, 4, {DataType::Byte}, {0});
    EXPECT_THROW(reader.read(1, Rect64{0, 0, 4, 4}, {0}, planes), InvalidArgumentError);
    EXPECT_THROW(reader.read(0, Rect64{0, 0, 4, 4}, {1}, planes), InvalidArgumentError);
    EXPECT_THROW(reader.read(0, Rect64{0, 0, 5, 4}, {0}, planes), InvalidArgumentError);
    EXPECT_THROW(reader.read(0, Rect64{0, 0, 4, 4}, {}, planes), InvalidArgumentError);
}

TEST(PlaneBuffers, ViewCarriesGeometry) {
    PlaneBuffers planes(30, 40, {DataType::Byte, DataType::Float32}, {0, 2});
    EXPECT_EQ(planes.bytes(), 30 * 40 * 1 + 30 * 40 * 4);
    const Rect64 rect{10, 20, 30, 40};
    const BlockView v = planes.view(1, rect, 4.0005);
    EXPECT_EQ(v.width(), 30);
    EXPECT_EQ(v.height(), 40);
    EXPECT_EQ(v.level(), 1);
    const Rect64 expected = detail::rectToLevel0(rect, 4.0005);
    EXPECT_EQ(v.slideRect().x, expected.x);
    EXPECT_EQ(v.slideRect().y, expected.y);
    EXPECT_EQ(v.slideRect().width, expected.width);
    EXPECT_EQ(v.slideRect().height, expected.height);
    ASSERT_EQ(v.planeCount(), 2);
    EXPECT_EQ(v.planeType(0), DataType::Byte);
    EXPECT_EQ(v.planeType(1), DataType::Float32);
    EXPECT_EQ(v.planeRowStrideBytes(0), 30);
    EXPECT_EQ(v.planeRowStrideBytes(1), 120);
    EXPECT_EQ(v.planeSourceChannel(0), 0);
    EXPECT_EQ(v.planeSourceChannel(1), 2);
}

TEST(PlaneBuffers, RejectsBadConstruction) {
    EXPECT_THROW(PlaneBuffers(0, 4, {DataType::Byte}, {0}), InvalidArgumentError);
    EXPECT_THROW(PlaneBuffers(4, 4, {}, {}), InvalidArgumentError);
    EXPECT_THROW(PlaneBuffers(4, 4, {DataType::Byte}, {0, 1}), InvalidArgumentError);
    EXPECT_THROW(PlaneBuffers(4, 4, {DataType::Unknown}, {0}), InvalidArgumentError);
    PlaneBuffers planes(4, 4, {DataType::Byte}, {0});
    EXPECT_THROW(planes.view(0, Rect64{0, 0, 5, 4}, 1.0), InvalidArgumentError);
}
