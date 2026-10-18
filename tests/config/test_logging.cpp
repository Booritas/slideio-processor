// Requirements: UR-EXE-5
#include <slideio/processor/config/logging.hpp>
#include <slideio/processor/core/error.hpp>

#include <gtest/gtest.h>

#include "common/temp_path.hpp"

#include <memory>
#include <mutex>
#include <regex>
#include <string>
#include <utility>
#include <vector>

namespace slideio::processor {
namespace {

class CaptureSink : public LogSink {
public:
    void write(LogLevel level, const char* message) override {
        std::lock_guard<std::mutex> lock(mutex);
        got.emplace_back(level, message);
    }
    std::mutex mutex;
    std::vector<std::pair<LogLevel, std::string>> got;
};

// Restores core's defaults after every test; the sink is process-global.
class ConfigureLogging : public testing::Test {
protected:
    void TearDown() override {
        setLogSink(nullptr);
        setLogLevel(LogLevel::Warning);
    }
};

TEST_F(ConfigureLogging, CallerSinkAndLevel) {
    auto sink = std::make_shared<CaptureSink>();
    configureLogging(sink, LogLevel::Debug);
    EXPECT_EQ(logLevel(), LogLevel::Debug);
    logMessage(LogLevel::Debug, "d %d", 1);
    logMessage(LogLevel::Trace, "t");
    ASSERT_EQ(sink->got.size(), 1u);
    EXPECT_EQ(sink->got[0].second, "d 1");
    EXPECT_NO_THROW(configureLogging(nullptr, LogLevel::Info));
    logMessage(LogLevel::Error, "dropped");
    EXPECT_EQ(sink->got.size(), 1u);
}

TEST_F(ConfigureLogging, InvalidLevelChangesNothing) {
    auto sink = std::make_shared<CaptureSink>();
    configureLogging(sink, LogLevel::Info);
    EXPECT_THROW(configureLogging(nullptr, static_cast<LogLevel>(99)), InvalidArgumentError);
    EXPECT_THROW(configureLogging(nullptr, static_cast<LogLevel>(-1)), InvalidArgumentError);
    EXPECT_EQ(logLevel(), LogLevel::Info);
    logMessage(LogLevel::Info, "kept");
    EXPECT_EQ(sink->got.size(), 1u);
}

TEST_F(ConfigureLogging, FileSinkAppendsTimestampedLines) {
    auto path = uniqueTempPath(u8"\u00fc_\u65e5\u672c", ".log");
    TempFileGuard guard(path);
    const std::string utf8Path = path.u8string();
    const Configuration c =
        ConfigurationBuilder().logFilePath(utf8Path.c_str()).logLevel(LogLevel::Info).build();
    configureLogging(c);
    logMessage(LogLevel::Warning, "hello %d", 42);
    configureLogging(c);
    logMessage(LogLevel::Info, "again");
    configureLogging(nullptr, LogLevel::Warning); // release the file
    const std::regex line(
        R"(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z (WARNING hello 42|INFO again))");
    const std::string content = readFile(path);
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < content.size()) {
        const size_t end = content.find('\n', start);
        ASSERT_NE(end, std::string::npos) << "line not newline-terminated";
        lines.push_back(content.substr(start, end - start));
        start = end + 1;
    }
    ASSERT_EQ(lines.size(), 2u);
    for (const std::string& l : lines) {
        EXPECT_TRUE(std::regex_match(l, line)) << l;
    }
    EXPECT_NE(lines[0].find("WARNING hello 42"), std::string::npos);
    EXPECT_NE(lines[1].find("INFO again"), std::string::npos);
}

TEST_F(ConfigureLogging, UnopenableFileThrowsAndKeepsThePreviousSink) {
    auto sink = std::make_shared<CaptureSink>();
    configureLogging(sink, LogLevel::Debug);
    auto dir = uniqueTempPath("missing_dir", ""); // never created
    const std::string bad = (dir / "x.log").u8string();
    try {
        configureLogging(ConfigurationBuilder().logFilePath(bad.c_str()).build());
        FAIL() << "expected IoError";
    } catch (const IoError& e) {
        EXPECT_NE(std::string(e.what()).find(bad), std::string::npos);
    }
    EXPECT_EQ(logLevel(), LogLevel::Debug);
    logMessage(LogLevel::Debug, "still here");
    EXPECT_EQ(sink->got.size(), 1u);
}

TEST_F(ConfigureLogging, EmptyPathWritesToStderr) {
    testing::internal::CaptureStderr();
    configureLogging(ConfigurationBuilder().logLevel(LogLevel::Info).build());
    logMessage(LogLevel::Error, "to stderr");
    configureLogging(nullptr, LogLevel::Warning);
    EXPECT_NE(testing::internal::GetCapturedStderr().find("ERROR to stderr"), std::string::npos);
}

} // namespace
} // namespace slideio::processor
