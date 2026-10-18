// Requirements: UR-EXE-2, UR-EXE-3, UR-EXE-6, UR-OBS-6
#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/log.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using slideio::processor::Configuration;
using slideio::processor::ConfigurationBuilder;
using slideio::processor::InvalidArgumentError;
using slideio::processor::LogLevel;

namespace {

// The process default is shared by every test in the process: put it back.
class ConfigurationProcessDefault : public ::testing::Test {
protected:
    void TearDown() override { Configuration::setProcessDefault(Configuration::defaults()); }
};

} // namespace

TEST(Configuration, DocumentedDefaults) {
    const Configuration c = Configuration::defaults();
    EXPECT_EQ(c.memoryCeilingBytes(), int64_t(2) << 30);
    const unsigned hw = std::thread::hardware_concurrency();
    EXPECT_EQ(c.workerCount(), hw == 0 ? 1 : static_cast<int32_t>(hw));
    EXPECT_EQ(c.tileWidth(), 1024);
    EXPECT_EQ(c.tileHeight(), 1024);
    EXPECT_TRUE(c.tileSizeFromSlide());
    EXPECT_STREQ(c.logFilePath(), "");
    EXPECT_EQ(c.logLevel(), LogLevel::Info);
    EXPECT_EQ(c.eventPollIntervalMs(), 150);
    EXPECT_TRUE(c.eventSignalEnabled());
    EXPECT_EQ(c.objectBatchSize(), 512);
    EXPECT_STREQ(c.scratchDirectory(), "");
    EXPECT_TRUE(Configuration().equals(c));
}

TEST(ConfigurationBuilder, EverySetterRoundTrips) {
    const Configuration c = ConfigurationBuilder()
                                .memoryCeilingBytes(int64_t(5) << 30)
                                .workerCount(1)
                                .tileSize(512, 256)
                                .tileSizeFromSlide(false)
                                .logFilePath(u8"日本/log.txt")
                                .logLevel(LogLevel::Off)
                                .eventPollIntervalMs(1)
                                .eventSignalEnabled(false)
                                .objectBatchSize(1)
                                .scratchDirectory("tmp")
                                .build();
    EXPECT_EQ(c.memoryCeilingBytes(), int64_t(5) << 30);
    EXPECT_EQ(c.workerCount(), 1);
    EXPECT_EQ(c.tileWidth(), 512);
    EXPECT_EQ(c.tileHeight(), 256);
    EXPECT_FALSE(c.tileSizeFromSlide());
    EXPECT_STREQ(c.logFilePath(), reinterpret_cast<const char*>(u8"日本/log.txt"));
    EXPECT_EQ(c.logLevel(), LogLevel::Off);
    EXPECT_EQ(c.eventPollIntervalMs(), 1);
    EXPECT_FALSE(c.eventSignalEnabled());
    EXPECT_EQ(c.objectBatchSize(), 1);
    EXPECT_STREQ(c.scratchDirectory(), "tmp");
    EXPECT_TRUE(ConfigurationBuilder(c).build().equals(c));
}

TEST(ConfigurationBuilder, BuildNamesEveryOffendingSetting) { // Review Focus 2
    ConfigurationBuilder b;
    b.memoryCeilingBytes(0)
        .workerCount(0)
        .tileSize(0, -1)
        .eventPollIntervalMs(0)
        .objectBatchSize(0)
        .logLevel(static_cast<LogLevel>(99))
        .logFilePath(nullptr)
        .scratchDirectory(nullptr);
    try {
        b.build();
        FAIL() << "build() accepted an invalid configuration";
    } catch (const InvalidArgumentError& e) {
        const std::string m = e.what();
        EXPECT_EQ(m.rfind("ConfigurationBuilder: invalid configuration: ", 0), 0u);
        for (const char* s : {"memoryCeilingBytes must be > 0 (got 0)", "workerCount must be >= 1 (got 0)",
                              "tileWidth must be >= 1 (got 0)", "tileHeight must be >= 1 (got -1)",
                              "eventPollIntervalMs must be >= 1 (got 0)", "objectBatchSize must be >= 1 (got 0)",
                              "logLevel", "logFilePath must not be null", "scratchDirectory must not be null"})
            EXPECT_NE(m.find(s), std::string::npos) << s;
        EXPECT_NE(m.find("(got 99)"), std::string::npos);
        EXPECT_LT(m.find("memoryCeilingBytes"), m.find("scratchDirectory")); // declaration order
        EXPECT_LT(m.find("tileHeight"), m.find("eventPollIntervalMs"));
        EXPECT_LT(m.find("logFilePath"), m.find("logLevel"));
    }
}

TEST(ConfigurationBuilder, ValidBoundariesAreAccepted) {
    EXPECT_NO_THROW(ConfigurationBuilder()
                        .memoryCeilingBytes(1)
                        .workerCount(1)
                        .tileSize(1, 1)
                        .eventPollIntervalMs(1)
                        .objectBatchSize(1)
                        .logLevel(LogLevel::Trace)
                        .build());
}

TEST(ConfigurationBuilder, CopyAndMoveKeepTheSettings) {
    ConfigurationBuilder a;
    a.workerCount(3).scratchDirectory("x");
    ConfigurationBuilder copy(a);
    ConfigurationBuilder assigned;
    assigned = a;
    EXPECT_EQ(copy.build().workerCount(), 3);
    EXPECT_STREQ(assigned.build().scratchDirectory(), "x");
    ConfigurationBuilder moved(std::move(a));
    EXPECT_EQ(moved.build().workerCount(), 3);
    EXPECT_TRUE(a.build().equals(Configuration::defaults())); // NOLINT(bugprone-use-after-move)
    ConfigurationBuilder moveAssigned;
    moveAssigned = std::move(moved);
    EXPECT_EQ(moveAssigned.build().workerCount(), 3);
}

TEST(Configuration, BuiltValueIsIndependentOfItsBuilder) {
    ConfigurationBuilder b;
    b.workerCount(3);
    const Configuration c = b.build();
    b.workerCount(7);
    EXPECT_EQ(c.workerCount(), 3);
}

TEST(Configuration, EqualsComparesStringsByContent) {
    std::string first = "log.txt";
    std::string second = "log.txt";
    const Configuration a = ConfigurationBuilder().logFilePath(first.c_str()).scratchDirectory("s").build();
    const Configuration b = ConfigurationBuilder().logFilePath(second.c_str()).scratchDirectory("s").build();
    EXPECT_NE(a.logFilePath(), b.logFilePath());
    EXPECT_TRUE(a.equals(b));
    EXPECT_FALSE(a.equals(ConfigurationBuilder(a).logFilePath("other.txt").build()));
    EXPECT_FALSE(a.equals(ConfigurationBuilder(a).scratchDirectory("t").build()));
    EXPECT_FALSE(a.equals(ConfigurationBuilder(a).workerCount(a.workerCount() + 1).build()));
    EXPECT_FALSE(a.equals(ConfigurationBuilder(a).objectBatchSize(7).build()));
    EXPECT_FALSE(a.equals(ConfigurationBuilder(a).logLevel(LogLevel::Error).build()));
}

TEST(Configuration, CopyAssignmentCopies) {
    const Configuration a = ConfigurationBuilder().workerCount(4).build();
    Configuration b;
    b = a;
    EXPECT_TRUE(b.equals(a));
    EXPECT_EQ(Configuration(a).workerCount(), 4);
}

TEST(Configuration, MovedFromReadsAsDefaults) {
    Configuration a = ConfigurationBuilder().workerCount(2).build();
    Configuration b(std::move(a));
    EXPECT_EQ(b.workerCount(), 2);
    EXPECT_TRUE(a.equals(Configuration::defaults())); // NOLINT(bugprone-use-after-move)
    Configuration c;
    c = std::move(b);
    EXPECT_EQ(c.workerCount(), 2);
    EXPECT_TRUE(b.equals(Configuration::defaults())); // NOLINT(bugprone-use-after-move)
}

TEST_F(ConfigurationProcessDefault, EqualsDefaultsUntilSet) {
    EXPECT_TRUE(Configuration::processDefault().equals(Configuration::defaults()));
}

TEST_F(ConfigurationProcessDefault, ProcessDefaultReferenceSurvivesReplacement) { // Review Focus 1, G3
    const Configuration& before = Configuration::processDefault();
    EXPECT_TRUE(before.equals(Configuration::defaults()));
    Configuration::setProcessDefault(ConfigurationBuilder().workerCount(5).build());
    EXPECT_EQ(Configuration::processDefault().workerCount(), 5);
    EXPECT_TRUE(before.equals(Configuration::defaults())); // still valid, still the old values
}

TEST_F(ConfigurationProcessDefault, ProcessDefaultIsSafeUnderConcurrentReplacement) {
    const Configuration one = ConfigurationBuilder().workerCount(1).build();
    const Configuration two = ConfigurationBuilder().workerCount(2).build();
    Configuration::setProcessDefault(one);

    std::atomic<int> bad{0};
    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&bad] {
            for (int i = 0; i < 10000; ++i) {
                const int32_t n = Configuration::processDefault().workerCount();
                if (n != 1 && n != 2)
                    ++bad;
            }
        });
    }
    for (int i = 0; i < 200; ++i)
        Configuration::setProcessDefault(i % 2 == 0 ? two : one);
    for (std::thread& r : readers)
        r.join();
    EXPECT_EQ(bad.load(), 0);
}
