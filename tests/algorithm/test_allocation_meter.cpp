// Requirements: UR-EXE-2
#include <slideio/processor/pipeline/operation_catalog.hpp>
#include <slideio/processor/pipeline/operation_parameters.hpp>

#include <algorithm/support/allocation_meter.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace slideio::processor;
namespace ts = slideio::processor::testing_support;

namespace {

constexpr int64_t kMegabyte = 1 << 20;

// Every block the tests allocate has its address stored here. A compiler may
// leave out an allocation whose address never escapes, even when its contents
// are read (clang does so for a new-expression and for libc++'s std::allocator,
// which allocates through __builtin_operator_new); a store to a volatile
// object is observable, so the block must exist.
void* volatile gEscape = nullptr;

void escape(const void* block) { gEscape = const_cast<void*>(block); }

void touch(const std::vector<uint8_t>& v) { escape(v.data()); }

std::vector<uint8_t> megabyte() {
    std::vector<uint8_t> v(static_cast<size_t>(kMegabyte), uint8_t{1});
    touch(v);
    return v;
}

struct alignas(64) Wide {
    uint8_t bytes[256];
};

// An operation whose apply() allocates exactly bytes_ through operator new and
// nothing else.
class AllocatingOperation : public Operation {
public:
    explicit AllocatingOperation(int64_t bytes) : bytes_(bytes) {}
    const char* name() const override { return "test.allocating"; }
    ChannelArity arity() const override { return ChannelArity::PerChannel; }
    bool requiresUniformTypes() const override { return false; }
    DataTypeSet supportedTypes() const override { return DataTypeSet().add(DataType::Byte); }
    Halo halo() const override { return Halo::none(); }
    DataType outputType(DataType) const override { return DataType::Byte; }
    const OperationParameters& parameters() const override { return parameters_; }
    void apply(OperationContext&) override {
        const std::unique_ptr<uint8_t[]> block(new uint8_t[static_cast<size_t>(bytes_)]);
        escape(block.get());
    }

private:
    int64_t bytes_;
    OperationParameters parameters_;
};

} // namespace

TEST(AllocationMeter, CountsOnlyItsThreadAndScope) {
    {
        ts::AllocationMeter meter;
        const std::vector<uint8_t> v = megabyte();
        EXPECT_GE(meter.peakBytes(), kMegabyte);
        EXPECT_GE(meter.liveBytes(), kMegabyte);
    }

    // A block allocated before the scope is not counted, and freeing it in the
    // scope does not take the count below zero.
    {
        std::vector<uint8_t> before = megabyte();
        ts::AllocationMeter meter;
        before.clear();
        before.shrink_to_fit();
        EXPECT_EQ(meter.peakBytes(), 0);
        EXPECT_EQ(meter.liveBytes(), 0);
    }

    // A block another thread allocates while the meter is alive is not counted.
    // The thread is started before the meter and synchronised through atomics,
    // which allocate nothing.
    {
        std::atomic<int> stage{0};
        std::thread other([&stage] {
            while (stage.load() != 1) {
                std::this_thread::yield();
            }
            const std::vector<uint8_t> v = megabyte();
            stage.store(2);
            while (stage.load() != 3) {
                std::this_thread::yield();
            }
        });
        {
            ts::AllocationMeter meter;
            stage.store(1);
            while (stage.load() != 2) {
                std::this_thread::yield();
            }
            EXPECT_EQ(meter.peakBytes(), 0);
            stage.store(3);
        }
        other.join();
    }
}

TEST(AllocationMeter, PeakNotTotal) {
    ts::AllocationMeter meter;
    for (int i = 0; i < 10; ++i) {
        const std::vector<uint8_t> v = megabyte();
    }
    EXPECT_GE(meter.peakBytes(), kMegabyte);
    EXPECT_LT(meter.peakBytes(), kMegabyte + kMegabyte / 16);
    EXPECT_EQ(meter.liveBytes(), 0);
}

TEST(AllocationMeter, SizedAndAlignedDeletes) {
    ts::AllocationMeter meter;

    // Aligned new-expressions, single and array, keep the alignment and are
    // counted; their deletes balance them.
    Wide* one = new Wide;
    escape(one);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(one) % alignof(Wide), 0u);
    EXPECT_GE(meter.liveBytes(), static_cast<int64_t>(sizeof(Wide)));
    Wide* many = new Wide[100];
    escape(many);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(many) % alignof(Wide), 0u);
    EXPECT_GE(meter.liveBytes(), static_cast<int64_t>(101 * sizeof(Wide)));
    delete one;
    delete[] many;
    EXPECT_EQ(meter.liveBytes(), 0);

    // Every operator form, called directly.
    const std::size_t n = 4096;
    const auto align = std::align_val_t{256};
    void* plain = ::operator new(n);
    void* array = ::operator new[](n);
    void* nothrowPlain = ::operator new(n, std::nothrow);
    void* nothrowArray = ::operator new[](n, std::nothrow);
    void* aligned = ::operator new(n, align);
    void* alignedArray = ::operator new[](n, align);
    void* alignedNothrow = ::operator new(n, align, std::nothrow);
    void* alignedArrayNothrow = ::operator new[](n, align, std::nothrow);
    ASSERT_NE(nothrowPlain, nullptr);
    ASSERT_NE(nothrowArray, nullptr);
    ASSERT_NE(alignedNothrow, nullptr);
    ASSERT_NE(alignedArrayNothrow, nullptr);
    for (void* p : {aligned, alignedArray, alignedNothrow, alignedArrayNothrow}) {
        EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % 256u, 0u);
    }
    EXPECT_EQ(meter.liveBytes(), static_cast<int64_t>(8 * n));
    // The Wide blocks were freed before these were made, and are smaller.
    EXPECT_EQ(meter.peakBytes(), static_cast<int64_t>(8 * n));

    ::operator delete(plain, n);
    ::operator delete[](array, n);
    ::operator delete(nothrowPlain, std::nothrow);
    ::operator delete[](nothrowArray, std::nothrow);
    ::operator delete(aligned, n, align);
    ::operator delete[](alignedArray, align);
    ::operator delete(alignedNothrow, align, std::nothrow);
    ::operator delete[](alignedArrayNothrow, n, align);
    EXPECT_EQ(meter.liveBytes(), 0);
}

TEST(AllocationMeter, RefusesNesting) {
    ts::AllocationMeter meter;
    EXPECT_THROW(ts::AllocationMeter nested, std::logic_error);
    // The refused meter left the first one in charge.
    const std::vector<uint8_t> v = megabyte();
    EXPECT_GE(meter.peakBytes(), kMegabyte);
}

TEST(AllocationMeter, MeasuresApplyAlone) {
    // The input, the output and the context are built outside the meter: what
    // is measured is exactly what apply() allocates.
    AllocatingOperation op(12345);
    const int64_t peak =
        ts::measureApplyPeak(op, {DataType::Byte}, {0}, 64, 64, [](int32_t, int64_t, int64_t) { return 7.0; });
    EXPECT_EQ(peak, 12345);
}

TEST(AllocationMeter, SeesABuiltInOperationsAllocations) {
    // The built-in operations are compiled into this executable, so their
    // allocations go through the counting operator new: a Gaussian blur's
    // separable pass allocates its row buffers through std::vector.
    const OperationPtr op =
        OperationCatalog::instance().create("pixel.gaussian_blur", OperationParameters().setDouble("sigma", 2.0));
    const int64_t peak = ts::measureApplyPeak(*op, {DataType::Float32}, {0}, 64, 64,
                                              [](int32_t, int64_t x, int64_t y) { return double(x + y); });
    EXPECT_GT(peak, 0);
}
