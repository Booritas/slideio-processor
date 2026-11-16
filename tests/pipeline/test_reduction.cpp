// Requirements: UR-PIX-5, UR-QA-3, UR-IMG-2
#include <slideio/processor/core/block_view.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/pipeline/reduction.hpp>

#include <pipeline/detail/half.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

using namespace slideio::processor;

namespace {

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

BlockView viewOf(std::vector<BlockPlane> planes, int64_t width, int64_t height) {
    return BlockView(width, height, Rect64{0, 0, width, height}, 0, planes.data(),
                     static_cast<int32_t>(planes.size()));
}

BlockPlane planeOf(void* data, DataType type, int64_t width) {
    return BlockPlane{data, type, width * dataTypeSizeBytes(type), 0};
}

std::vector<int64_t> binsOf(const Histogram64& h, int32_t n) {
    std::vector<int64_t> out;
    for (int32_t i = 0; i < n; ++i) {
        out.push_back(h.bin(i));
    }
    return out;
}

} // namespace

TEST(Histogram64, BinsAndEdges) {
    Histogram64 h(4, 0, 4);
    std::vector<float> data = {0.f, 0.99f, 1.f, 3.5f, 4.f, 5.f};
    h.accumulate(viewOf({planeOf(data.data(), DataType::Float32, 6)}, 6, 1), 0);
    EXPECT_EQ(binsOf(h, 4), (std::vector<int64_t>{2, 1, 0, 2}));
    EXPECT_EQ(h.totalCount(), 5);
}

TEST(Histogram64, IgnoresNaNAndBelowRange) {
    Histogram64 h(2, 0, 1);
    std::vector<float> data = {kNaN, -0.5f, 0.25f, std::numeric_limits<float>::infinity()};
    h.accumulate(viewOf({planeOf(data.data(), DataType::Float32, 4)}, 4, 1), 0);
    EXPECT_EQ(binsOf(h, 2), (std::vector<int64_t>{1, 0}));
    EXPECT_EQ(h.totalCount(), 1);
}

TEST(Histogram64, ReadsEveryPlaneByItsType) {
    Histogram64 h(2, 0, 256);
    std::vector<uint8_t> bytes = {10, 200};
    std::vector<float> floats = {5.f, 250.f};
    h.accumulate(viewOf({planeOf(bytes.data(), DataType::Byte, 2), planeOf(floats.data(), DataType::Float32, 2)}, 2, 1), 0);
    EXPECT_EQ(binsOf(h, 2), (std::vector<int64_t>{2, 2}));
}

TEST(Histogram64, ReadsEveryIntegerAndFloatType) {
    Histogram64 h(2, -128, 128);
    const int8_t i8[] = {-5, 5};
    const int16_t i16[] = {-5, 5};
    const uint16_t u16[] = {5, 6};
    const int32_t i32[] = {-5, 5};
    const uint32_t u32[] = {5, 6};
    const int64_t i64[] = {-5, 5};
    const uint64_t u64[] = {5, 6};
    const double f64[] = {-5.0, 5.0};
    const uint16_t half[] = {0xC500, 0x4500}; // -5, 5
    h.accumulate(viewOf({planeOf(const_cast<int8_t*>(i8), DataType::Int8, 2),
                         planeOf(const_cast<int16_t*>(i16), DataType::Int16, 2),
                         planeOf(const_cast<uint16_t*>(u16), DataType::UInt16, 2),
                         planeOf(const_cast<int32_t*>(i32), DataType::Int32, 2),
                         planeOf(const_cast<uint32_t*>(u32), DataType::UInt32, 2),
                         planeOf(const_cast<int64_t*>(i64), DataType::Int64, 2),
                         planeOf(const_cast<uint64_t*>(u64), DataType::UInt64, 2),
                         planeOf(const_cast<double*>(f64), DataType::Float64, 2),
                         planeOf(const_cast<uint16_t*>(half), DataType::Float16, 2)},
                        2, 1),
                 0);
    // Negative samples: i8, i16, i32, i64, f64, half = 6. Others (u16, u32, u64: 5 and 6) and positives land high.
    EXPECT_EQ(h.bin(0), 6);
    EXPECT_EQ(h.totalCount(), 18);
}

TEST(Histogram64, HonoursRowStride) {
    Histogram64 h(2, 0, 2);
    // 2x2 Byte plane with 4 bytes per row: padding bytes (99) must not be read.
    std::vector<uint8_t> data = {0, 1, 99, 99, 1, 0, 99, 99};
    BlockPlane p{data.data(), DataType::Byte, 4, 0};
    h.accumulate(viewOf({p}, 2, 2), 0);
    EXPECT_EQ(binsOf(h, 2), (std::vector<int64_t>{2, 2}));
}

TEST(Histogram64, NullViewThrows) {
    Histogram64 h(2, 0, 1);
    EXPECT_THROW(h.accumulate(BlockView(), 0), InvalidArgumentError);
}

TEST(Histogram64, ConcurrentAccumulateEqualsSequential) {
    constexpr int kThreads = 8;
    constexpr int kTilesPerThread = 64;
    constexpr int64_t kSide = 16;
    // Distinct tiles of a mixed view: a Byte plane and a Float32 plane.
    std::vector<std::vector<uint8_t>> bytes;
    std::vector<std::vector<float>> floats;
    for (int t = 0; t < kThreads * kTilesPerThread; ++t) {
        std::vector<uint8_t> b(kSide * kSide);
        std::vector<float> f(kSide * kSide);
        for (int i = 0; i < kSide * kSide; ++i) {
            b[i] = static_cast<uint8_t>((i * 7 + t * 13) % 256);
            f[i] = static_cast<float>(((i * 11 + t * 3) % 300) - 20) + 0.5f; // some out of range
        }
        bytes.push_back(std::move(b));
        floats.push_back(std::move(f));
    }
    auto viewAt = [&](int t) {
        return viewOf({planeOf(bytes[t].data(), DataType::Byte, kSide), planeOf(floats[t].data(), DataType::Float32, kSide)},
                      kSide, kSide);
    };

    Histogram64 sequential(16, 0, 256);
    for (int t = 0; t < kThreads * kTilesPerThread; ++t) {
        sequential.accumulate(viewAt(t), t);
    }

    Histogram64 parallel(16, 0, 256);
    std::vector<std::thread> threads;
    for (int w = 0; w < kThreads; ++w) {
        threads.emplace_back([&, w] {
            for (int k = 0; k < kTilesPerThread; ++k) {
                const int t = w * kTilesPerThread + k;
                parallel.accumulate(viewAt(t), t);
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }
    EXPECT_EQ(binsOf(parallel, 16), binsOf(sequential, 16));
    EXPECT_EQ(parallel.totalCount(), sequential.totalCount());
    EXPECT_GT(sequential.totalCount(), 0);
}

TEST(Histogram64, CombineAddsAndChecksShape) {
    std::vector<float> a = {0.f, 1.f};
    std::vector<float> b = {1.f, 1.f, 3.f};
    Histogram64 x(4, 0, 4);
    Histogram64 y(4, 0, 4);
    x.accumulate(viewOf({planeOf(a.data(), DataType::Float32, 2)}, 2, 1), 0);
    y.accumulate(viewOf({planeOf(b.data(), DataType::Float32, 3)}, 3, 1), 1);
    x.combine(y);
    EXPECT_EQ(binsOf(x, 4), (std::vector<int64_t>{1, 3, 0, 1}));
    EXPECT_EQ(binsOf(y, 4), (std::vector<int64_t>{0, 2, 0, 1})); // the argument is untouched

    EXPECT_THROW(x.combine(Histogram64(5, 0, 4)), InvalidArgumentError);
    EXPECT_THROW(x.combine(Histogram64(4, 1, 4)), InvalidArgumentError);
    EXPECT_THROW(x.combine(Histogram64(4, 0, 5)), InvalidArgumentError);

    struct Other : Reduction {
        void accumulate(const BlockView&, int64_t) override {}
        void combine(const Reduction&) override {}
        bool isOrderIndependent() const override { return true; }
    } other;
    EXPECT_THROW(x.combine(other), InvalidArgumentError);
}

TEST(Histogram64, CopyCopiesValuesAndMoveTransfers) {
    std::vector<float> a = {0.f, 1.f, 3.f};
    Histogram64 x(4, 0, 4);
    x.accumulate(viewOf({planeOf(a.data(), DataType::Float32, 3)}, 3, 1), 0);

    Histogram64 copy(x);
    EXPECT_EQ(binsOf(copy, 4), binsOf(x, 4));
    copy.accumulate(viewOf({planeOf(a.data(), DataType::Float32, 3)}, 3, 1), 0);
    EXPECT_EQ(x.totalCount(), 3); // independent of the copy
    EXPECT_EQ(copy.totalCount(), 6);

    Histogram64 assigned(2, 0, 1);
    assigned = x;
    EXPECT_EQ(binsOf(assigned, 4), binsOf(x, 4));

    Histogram64 moved(std::move(copy));
    EXPECT_EQ(moved.totalCount(), 6);
    EXPECT_THROW(copy.totalCount(), InvalidArgumentError); // NOLINT: moved-from is specified
    copy = moved; // a moved-from histogram can be assigned to
    EXPECT_EQ(copy.totalCount(), 6);
}

TEST(Histogram64, IsOrderIndependent) {
    EXPECT_TRUE(Histogram64(2, 0, 1).isOrderIndependent());
}

TEST(Histogram64, Validation) {
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW(Histogram64(0, 0, 1), InvalidArgumentError);
    EXPECT_THROW(Histogram64(-1, 0, 1), InvalidArgumentError);
    EXPECT_THROW(Histogram64(2, 1, 1), InvalidArgumentError);
    EXPECT_THROW(Histogram64(2, 2, 1), InvalidArgumentError);
    EXPECT_THROW(Histogram64(2, nan, 1), InvalidArgumentError);
    EXPECT_THROW(Histogram64(2, 0, nan), InvalidArgumentError);
    EXPECT_THROW(Histogram64(2, -inf, 1), InvalidArgumentError);
    EXPECT_THROW(Histogram64(2, 0, inf), InvalidArgumentError);
    Histogram64 h(3, 0, 1);
    EXPECT_THROW(h.bin(-1), InvalidArgumentError);
    EXPECT_THROW(h.bin(3), InvalidArgumentError);
    EXPECT_EQ(h.bin(2), 0);
}

TEST(Histogram64, SpanOverflowIsRejected) {
    EXPECT_THROW(Histogram64(2, -1e308, 1e308), InvalidArgumentError);
    EXPECT_NO_THROW(Histogram64(2, -1e307, 1e307));
}

TEST(Histogram64, RoundUpIsClampedToTheLastBin) {
    // Found by search: over [0.2, 0.9) with 10 bins, the value just below 0.9 gives
    // (v - 0.2) / (0.9 - 0.2) == 1.0 after rounding, so the unclamped index is 10, not 9.
    const double lo = 0.2;
    const double hi = 0.9;
    const int32_t n = 10;
    const double v = std::nextafter(hi, lo);
    ASSERT_LT(v, hi);
    ASSERT_EQ(std::floor((v - lo) / (hi - lo) * n), static_cast<double>(n)); // the premise
    Histogram64 h(n, lo, hi);
    h.accumulate(viewOf({planeOf(const_cast<double*>(&v), DataType::Float64, 1)}, 1, 1), 0);
    EXPECT_EQ(h.bin(n - 1), 1);
    EXPECT_EQ(h.totalCount(), 1);
}

TEST(HalfToDouble, CoversEveryClass) {
    using detail::halfToDouble;
    EXPECT_EQ(halfToDouble(0x0000), 0.0);
    EXPECT_TRUE(std::signbit(halfToDouble(0x8000)));
    EXPECT_EQ(halfToDouble(0x3C00), 1.0);
    EXPECT_EQ(halfToDouble(0xC000), -2.0);
    EXPECT_EQ(halfToDouble(0x7BFF), 65504.0);               // largest finite
    EXPECT_EQ(halfToDouble(0x0400), std::ldexp(1.0, -14));  // smallest normal
    EXPECT_EQ(halfToDouble(0x0001), std::ldexp(1.0, -24));  // smallest subnormal
    EXPECT_EQ(halfToDouble(0x03FF), 1023.0 * std::ldexp(1.0, -24)); // largest subnormal
    EXPECT_EQ(halfToDouble(0x7C00), std::numeric_limits<double>::infinity());
    EXPECT_EQ(halfToDouble(0xFC00), -std::numeric_limits<double>::infinity());
    EXPECT_TRUE(std::isnan(halfToDouble(0x7E00)));
    EXPECT_TRUE(std::isnan(halfToDouble(0x7C01)));
    EXPECT_EQ(halfToDouble(0x3555), 0.333251953125);
}
