// Requirements: UR-EXE-4
#include <slideio/processor/core/control.hpp>
#include <slideio/processor/core/error.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <type_traits>
#include <utility>

using namespace slideio::processor;

TEST(Cancellation, SourceCancelsAllTokens) {
    CancellationSource s; CancellationToken t = s.token(), u = t;
    EXPECT_FALSE(t.isCancelled()); s.cancel();
    EXPECT_TRUE(u.isCancelled()); EXPECT_TRUE(s.isCancelled());
    EXPECT_THROW(t.throwIfCancelled(), CancelledError);
    EXPECT_NO_THROW(CancellationToken().throwIfCancelled());
}
TEST(Cancellation, DefaultTokenNeverCancelled) {
    CancellationToken t;
    EXPECT_FALSE(t.isCancelled());
    EXPECT_NO_THROW(t.throwIfCancelled());
}
TEST(Cancellation, CopyAndMoveShareState) {
    CancellationSource s;
    CancellationToken a = s.token();
    CancellationToken b(a);
    CancellationToken c;
    c = a;
    CancellationToken m(std::move(b));
    CancellationToken n;
    n = std::move(c);
    s.cancel();
    EXPECT_TRUE(a.isCancelled());
    EXPECT_TRUE(m.isCancelled());
    EXPECT_TRUE(n.isCancelled());
    EXPECT_FALSE(b.isCancelled()); // moved-from token holds no state
    EXPECT_FALSE(c.isCancelled());
}
TEST(Cancellation, TokenSurvivesSource) {
    CancellationToken t;
    {
        CancellationSource s;
        t = s.token();
        s.cancel();
    }
    EXPECT_TRUE(t.isCancelled());
}
TEST(Cancellation, ThrowsNamedMessage) {
    CancellationSource s; s.cancel();
    try {
        s.token().throwIfCancelled();
        FAIL() << "expected CancelledError";
    } catch (const CancelledError& e) {
        EXPECT_STREQ(e.what(), "operation cancelled");
    }
}
TEST(Cancellation, NoexceptAndSourceNotCopyable) {
    static_assert(std::is_nothrow_default_constructible<CancellationToken>::value, "");
    static_assert(std::is_nothrow_copy_constructible<CancellationToken>::value, "");
    static_assert(std::is_nothrow_move_constructible<CancellationToken>::value, "");
    static_assert(std::is_nothrow_copy_assignable<CancellationToken>::value, "");
    static_assert(std::is_nothrow_move_assignable<CancellationToken>::value, "");
    static_assert(!std::is_copy_constructible<CancellationSource>::value, "");
    static_assert(!std::is_move_constructible<CancellationSource>::value, "");
    SUCCEED();
}
TEST(Cancellation, CancelFromAnotherThread) {
    CancellationSource s; CancellationToken t = s.token();
    std::thread th([&] { s.cancel(); });
    th.join();
    EXPECT_TRUE(t.isCancelled());
}
namespace {
class CountingObserver : public ProgressObserver {
public:
    void onProgress(const char*, double, int64_t completed, int64_t) override { last = completed; }
    int64_t last = -1;
};
}
TEST(Progress, ObserverIsImplementable) {
    CountingObserver o; ProgressObserver& p = o;
    p.onProgress("stage", 0.5, 5, 10);
    EXPECT_EQ(o.last, 5);
}
