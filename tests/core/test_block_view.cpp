// Requirements: UR-API-3, UR-IMG-9, UR-IMG-10
#include <slideio/processor/core/block_view.hpp>
#include <slideio/processor/core/error.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using namespace slideio::processor;

namespace {
// The message must begin with the field name.
template <typename F>
void expectField(const char* field, F&& make) {
    try { make(); FAIL() << field; }
    catch (const InvalidArgumentError& e) {
        EXPECT_EQ(std::string(e.what()).rfind(field, 0), 0u) << e.what();
    }
}
} // namespace

TEST(BlockView, MixedTypePlanesOverCallerMemory) {
    std::vector<uint8_t> a(4 * 3); std::vector<float> b(4 * 3);
    BlockPlane planes[] = {{a.data(), DataType::Byte, 4, 0}, {b.data(), DataType::Float32, 16, 3}};
    BlockView v(4, 3, Rect64{100, 200, 16, 12}, 2, planes, 2);
    planes[0].data = nullptr;                                     // array not retained
    EXPECT_EQ(v.planeData(0), a.data()); EXPECT_EQ(v.planeType(1), DataType::Float32);
    EXPECT_EQ(v.planeRowStrideBytes(1), 16); EXPECT_EQ(v.planeSourceChannel(1), 3);
    EXPECT_EQ(v.level(), 2); EXPECT_EQ(v.slideRect(), (Rect64{100, 200, 16, 12}));
    BlockView c(v); EXPECT_EQ(c.planeData(1), b.data());           // copies pointers
    EXPECT_THROW(v.planeType(2), InvalidArgumentError);
}

TEST(BlockView, ValidationNamesField) {
    uint8_t buf[64]; BlockPlane ok{buf, DataType::Byte, 4, 0};
    expectField("width",  [&]{ BlockView(0, 1, Rect64{0,0,1,1}, 0, &ok, 1); });
    expectField("level",  [&]{ BlockView(1, 1, Rect64{0,0,1,1}, -1, &ok, 1); });
    expectField("slideRect", [&]{ BlockView(1, 1, Rect64{0,0,0,1}, 0, &ok, 1); });
    expectField("planeCount", [&]{ BlockView(1, 1, Rect64{0,0,1,1}, 0, &ok, 0); });
    BlockPlane bad = ok; bad.rowStrideBytes = 3;
    expectField("planes[0].rowStrideBytes", [&]{ BlockView(4, 1, Rect64{0,0,4,1}, 0, &bad, 1); });
    bad = ok; bad.type = DataType::Unknown;
    expectField("planes[0].type", [&]{ BlockView(1, 1, Rect64{0,0,1,1}, 0, &bad, 1); });
    bad = ok; bad.sourceChannel = -1;
    expectField("planes[0].sourceChannel", [&]{ BlockView(1, 1, Rect64{0,0,1,1}, 0, &bad, 1); });
    EXPECT_TRUE(BlockView().isNull());
}

TEST(BlockView, RemainingFieldsAndPlaneIndexInMessage) {
    uint8_t buf[64]; BlockPlane ok{buf, DataType::Byte, 4, 0};
    expectField("height", [&]{ BlockView(1, 0, Rect64{0,0,1,1}, 0, &ok, 1); });
    expectField("planes", [&]{ BlockView(1, 1, Rect64{0,0,1,1}, 0, nullptr, 1); });
    BlockPlane two[] = {ok, ok};
    two[1].data = nullptr;
    expectField("planes[1].data", [&]{ BlockView(1, 1, Rect64{0,0,1,1}, 0, two, 2); });
    two[1] = ok; two[1].sourceChannel = -2;
    expectField("planes[1].sourceChannel", [&]{ BlockView(1, 1, Rect64{0,0,1,1}, 0, two, 2); });
}

TEST(BlockView, StrideProductOverflowIsUndersizedStride) {
    uint8_t buf[8]; BlockPlane p{buf, DataType::Float64, std::numeric_limits<int64_t>::max(), 0};
    const int64_t huge = std::numeric_limits<int64_t>::max() / 2;
    try { BlockView(huge, 1, Rect64{0,0,1,1}, 0, &p, 1); FAIL(); }
    catch (const InvalidArgumentError& e) {
        EXPECT_NE(std::string(e.what()).find("planes[0].rowStrideBytes"), std::string::npos) << e.what();
    }
}

TEST(BlockView, NullAndMovedFromViewsAreSafe) {
    BlockView n;
    EXPECT_TRUE(n.isNull()); EXPECT_EQ(n.width(), 0); EXPECT_EQ(n.height(), 0);
    EXPECT_EQ(n.planeCount(), 0); EXPECT_EQ(n.level(), 0); EXPECT_TRUE(n.slideRect().isEmpty());
    EXPECT_THROW(n.planeData(0), InvalidArgumentError);
    EXPECT_THROW(n.planeType(0), InvalidArgumentError);
    EXPECT_THROW(n.planeRowStrideBytes(0), InvalidArgumentError);
    EXPECT_THROW(n.planeSourceChannel(0), InvalidArgumentError);

    uint8_t buf[16]; BlockPlane p{buf, DataType::Byte, 4, 1};
    BlockView v(4, 2, Rect64{0, 0, 4, 2}, 0, &p, 1);
    EXPECT_THROW(v.planeData(-1), InvalidArgumentError);
    BlockView m(std::move(v));
    EXPECT_TRUE(v.isNull()); EXPECT_EQ(m.planeCount(), 1); EXPECT_EQ(m.width(), 4);
    BlockView copyOfNull(v); EXPECT_TRUE(copyOfNull.isNull());
    BlockView a; a = m; EXPECT_EQ(a.planeData(0), buf);
    a = n; EXPECT_TRUE(a.isNull());
    BlockView b; b = std::move(m); EXPECT_EQ(b.planeSourceChannel(0), 1); EXPECT_TRUE(m.isNull());
}

TEST(BlockView, ValidationOrderIsPinned) {
    uint8_t buf[64]; BlockPlane ok{buf, DataType::Byte, 4, 0};
    expectField("width", [&]{ BlockView(0, 1, Rect64{0,0,1,1}, -1, &ok, 1); });
    BlockPlane bad = ok; bad.data = nullptr; bad.type = DataType::Unknown;
    expectField("planes[0].data", [&]{ BlockView(1, 1, Rect64{0,0,1,1}, 0, &bad, 1); });
    // Per-plane order: every check on plane 0 runs before any check on plane 1.
    BlockPlane two[] = {ok, bad};
    two[0].sourceChannel = -1;
    expectField("planes[0].sourceChannel", [&]{ BlockView(1, 1, Rect64{0,0,1,1}, 0, two, 2); });
}

TEST(BlockView, LastRowOffsetOverflowThrows) {
    uint8_t buf[8]; BlockPlane p{buf, DataType::Byte, 4, 0};
    expectField("planes[0].rowStrideBytes", [&]{
        BlockView(4, std::numeric_limits<int64_t>::max() / 2, Rect64{0,0,4,1}, 0, &p, 1); });
}
