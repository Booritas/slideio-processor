// Requirements: UR-EXE-2
#include <slideio/processor/core/control.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/memory_budget.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace slideio::processor;

TEST(MemoryBudget, ReserveRelease) {
    auto b = makeMemoryBudget(100);
    EXPECT_TRUE(b->tryReserve(60)); EXPECT_FALSE(b->tryReserve(41)); EXPECT_TRUE(b->tryReserve(40));
    EXPECT_EQ(b->availableBytes(), 0); b->release(100); EXPECT_EQ(b->reservedBytes(), 0);
    EXPECT_THROW(b->release(1), InvalidArgumentError);
    EXPECT_THROW(b->tryReserve(-1), InvalidArgumentError);
    EXPECT_THROW(makeMemoryBudget(0), InvalidArgumentError);
    EXPECT_THROW(b->reserveOrWait(101, CancellationToken()), InvalidArgumentError);
}
TEST(MemoryBudget, EdgeCases) {
    EXPECT_THROW(makeMemoryBudget(-5), InvalidArgumentError);
    auto b = makeMemoryBudget(100);
    EXPECT_EQ(b->ceilingBytes(), 100);
    EXPECT_TRUE(b->tryReserve(0));
    EXPECT_TRUE(b->tryReserve(100));
    EXPECT_TRUE(b->tryReserve(0));
    EXPECT_FALSE(b->tryReserve(1));
    EXPECT_THROW(b->release(-1), InvalidArgumentError);
    EXPECT_THROW(b->release(101), InvalidArgumentError);
    EXPECT_EQ(b->reservedBytes(), 100);
    EXPECT_THROW(b->reserveOrWait(-1, CancellationToken()), InvalidArgumentError);
    b->release(100);
    EXPECT_NO_THROW(b->reserveOrWait(100, CancellationToken()));
    EXPECT_EQ(b->availableBytes(), 0);
}
TEST(MemoryBudget, WaiterWakesOnRelease) {
    auto b = makeMemoryBudget(100); ASSERT_TRUE(b->tryReserve(100));
    std::atomic<bool> got{false};
    std::thread w([&]{ b->reserveOrWait(50, CancellationToken()); got = true; });
    std::this_thread::sleep_for(std::chrono::milliseconds(50)); EXPECT_FALSE(got);
    b->release(50); w.join(); EXPECT_TRUE(got); EXPECT_EQ(b->reservedBytes(), 100);
}
TEST(MemoryBudget, WaiterHonoursCancellation) {
    auto b = makeMemoryBudget(10); ASSERT_TRUE(b->tryReserve(10)); CancellationSource s;
    std::thread c([&]{ std::this_thread::sleep_for(std::chrono::milliseconds(30)); s.cancel(); });
    auto t0 = std::chrono::steady_clock::now();
    EXPECT_THROW(b->reserveOrWait(5, s.token()), CancelledError);
    EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(1));
    c.join(); EXPECT_EQ(b->reservedBytes(), 10);
}
TEST(MemoryBudget, AlreadyCancelledTokenThrowsWhenFull) {
    auto b = makeMemoryBudget(10); ASSERT_TRUE(b->tryReserve(10));
    CancellationSource s; s.cancel();
    EXPECT_THROW(b->reserveOrWait(5, s.token()), CancelledError);
    EXPECT_EQ(b->reservedBytes(), 10);
}
TEST(MemoryBudget, ConcurrentReserveNeverExceedsCeiling) {
    auto b = makeMemoryBudget(100);
    std::atomic<bool> exceeded{false};
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&] {
            for (int k = 0; k < 200; ++k) {
                b->reserveOrWait(40, CancellationToken());
                if (b->reservedBytes() > b->ceilingBytes()) exceeded = true;
                b->release(40);
            }
        });
    }
    for (auto& t : threads) t.join();
    EXPECT_FALSE(exceeded);
    EXPECT_EQ(b->reservedBytes(), 0);
}
