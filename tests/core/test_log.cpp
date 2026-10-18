// Requirements: UR-EXE-5
#include <slideio/processor/core/log.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace slideio::processor;

namespace {

struct CaptureSink : LogSink {
    std::vector<std::pair<LogLevel, std::string>> got;
    void write(LogLevel l, const char* m) override { got.emplace_back(l, m); }
};

// Restores the process-global defaults (null sink, Warning) however the test ends.
class LogTest : public ::testing::Test {
protected:
    void SetUp() override { restore(); }
    void TearDown() override { restore(); }
    static void restore() {
        setLogSink(nullptr);
        setLogLevel(LogLevel::Warning);
    }
};

} // namespace

TEST_F(LogTest, ExchangeReturnsThePreviousSink) {
    auto first = std::make_shared<CaptureSink>();
    auto second = std::make_shared<CaptureSink>();
    EXPECT_EQ(exchangeLogSink(first), nullptr);
    EXPECT_EQ(exchangeLogSink(second), first);
    setLogLevel(LogLevel::Info);
    logMessage(LogLevel::Info, "to second");
    EXPECT_TRUE(first->got.empty());
    ASSERT_EQ(second->got.size(), 1u);
    EXPECT_EQ(exchangeLogSink(nullptr), second);
}

TEST_F(LogTest, LevelFilterAndFormatting) {
    auto s = std::make_shared<CaptureSink>();
    setLogSink(s);
    setLogLevel(LogLevel::Info);
    logMessage(LogLevel::Debug, "hidden %d", 1);
    logMessage(LogLevel::Warning, "tile %d of %s", 3, "nuclei");
    logMessage(LogLevel::Off, "never");
    ASSERT_EQ(s->got.size(), 1u);
    EXPECT_EQ(s->got[0].first, LogLevel::Warning);
    EXPECT_EQ(s->got[0].second, "tile 3 of nuclei");
}

TEST_F(LogTest, ThrowingSinkIsSwallowed) {
    struct Bad : LogSink {
        void write(LogLevel, const char*) override { throw std::runtime_error("x"); }
    };
    setLogSink(std::make_shared<Bad>());
    EXPECT_NO_THROW(logMessage(LogLevel::Error, "e"));
}

TEST_F(LogTest, DefaultsAndAccessors) {
    EXPECT_EQ(logLevel(), LogLevel::Warning);
    setLogLevel(LogLevel::Trace);
    EXPECT_EQ(logLevel(), LogLevel::Trace);
    EXPECT_NO_THROW(logMessage(LogLevel::Error, "no sink installed %d", 1));
}

TEST_F(LogTest, LevelOffDiscardsEverything) {
    auto s = std::make_shared<CaptureSink>();
    setLogSink(s);
    setLogLevel(LogLevel::Off);
    logMessage(LogLevel::Error, "e");
    logMessage(LogLevel::Off, "o");
    EXPECT_TRUE(s->got.empty());
}

TEST_F(LogTest, NullFormatIsNoOp) {
    auto s = std::make_shared<CaptureSink>();
    setLogSink(s);
    // A null format is deliberate here; silence GCC/Clang format checking for it.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat"
#pragma GCC diagnostic ignored "-Wformat-security"
#endif
    EXPECT_NO_THROW(logMessage(LogLevel::Error, nullptr));
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
    EXPECT_TRUE(s->got.empty());
}

TEST_F(LogTest, LongMessageIsNotTruncated) {
    auto s = std::make_shared<CaptureSink>();
    setLogSink(s);
    const std::string big(5000, 'x');
    logMessage(LogLevel::Error, "%s!", big.c_str());
    ASSERT_EQ(s->got.size(), 1u);
    EXPECT_EQ(s->got[0].second, big + "!");
}

TEST_F(LogTest, SinkMayLogWithoutDeadlock) {
    struct Reentrant : LogSink {
        int depth = 0;
        void write(LogLevel, const char*) override {
            if (depth++ == 0) {
                logMessage(LogLevel::Error, "inner");
            }
        }
    };
    auto s = std::make_shared<Reentrant>();
    setLogSink(s);
    logMessage(LogLevel::Error, "outer");
    EXPECT_EQ(s->depth, 2);
}
