// Requirements: UR-RES-11
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/provenance.hpp>
#include <slideio/processor/core/version.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <string>
#include <utility>

using namespace slideio::processor;

TEST(Provenance, JsonInFixedKeyOrderWithEscaping) {
    Provenance p; p.setSlideId("slide \"A\""); p.setSceneIndex(2);
    std::string expect = "{\"slideId\":\"slide \\\"A\\\"\",\"sceneIndex\":2,\"libraryVersion\":\"0.1.0\","
                         "\"createdAtUnixMs\":" + std::to_string(p.createdAtUnixMs()) + "}";
    int32_t n = p.toJson(nullptr, 0); EXPECT_EQ(n, (int32_t)expect.size());
    std::string buf(n + 1, '\0'); p.toJson(buf.data(), n + 1); EXPECT_EQ(buf.c_str(), expect);
    EXPECT_THROW(p.setSceneIndex(-1), InvalidArgumentError);
    EXPECT_THROW(p.setSlideId(nullptr), InvalidArgumentError);
    Provenance c(p); EXPECT_STREQ(c.slideId(), "slide \"A\"");
}

TEST(Provenance, Defaults) {
    const auto before = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    Provenance p;
    const auto after = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    EXPECT_STREQ(p.slideId(), "");
    EXPECT_EQ(p.sceneIndex(), 0);
    EXPECT_STREQ(p.libraryVersion(), libraryVersion());
    EXPECT_GE(p.createdAtUnixMs(), before);
    EXPECT_LE(p.createdAtUnixMs(), after);
}

TEST(Provenance, FailedSettersLeaveStateUnchanged) {
    Provenance p; p.setSlideId("a"); p.setSceneIndex(4);
    EXPECT_THROW(p.setSlideId(nullptr), InvalidArgumentError);
    EXPECT_THROW(p.setSceneIndex(-1), InvalidArgumentError);
    EXPECT_STREQ(p.slideId(), "a");
    EXPECT_EQ(p.sceneIndex(), 4);
}

TEST(Provenance, BufferContractTruncatesAndTerminates) {
    Provenance p; p.setSlideId("abc");
    const int32_t n = p.toJson(nullptr, 0);
    ASSERT_GT(n, 10);
    char small[8];
    std::memset(small, 'Z', sizeof(small));
    EXPECT_EQ(p.toJson(small, 8), n);
    EXPECT_EQ(std::strlen(small), 7u);
    EXPECT_EQ(std::string(small), std::string("{\"slide"));
    char untouched[2] = {'Z', 'Z'};
    EXPECT_EQ(p.toJson(untouched, 0), n);
    EXPECT_EQ(untouched[0], 'Z');
}

TEST(Provenance, CopyAndMoveSet) {
    Provenance a; a.setSlideId("s"); a.setSceneIndex(3);
    const int64_t created = a.createdAtUnixMs();
    Provenance b(a);
    EXPECT_EQ(b.createdAtUnixMs(), created);
    EXPECT_EQ(b.sceneIndex(), 3);
    b.setSlideId("other");
    EXPECT_STREQ(a.slideId(), "s");

    Provenance c; c = a;
    EXPECT_STREQ(c.slideId(), "s");
    EXPECT_EQ(c.createdAtUnixMs(), created);

    Provenance d(std::move(a));
    EXPECT_STREQ(d.slideId(), "s");
    EXPECT_EQ(d.sceneIndex(), 3);
    // Moved-from is valid: getters work and setters re-initialise it.
    EXPECT_STREQ(a.slideId(), "");
    EXPECT_EQ(a.sceneIndex(), 0);
    EXPECT_STREQ(a.libraryVersion(), libraryVersion());
    EXPECT_GE(a.toJson(nullptr, 0), 0);
    a.setSlideId("again");
    EXPECT_STREQ(a.slideId(), "again");

    Provenance e; e = std::move(d);
    EXPECT_STREQ(e.slideId(), "s");
    EXPECT_EQ(e.createdAtUnixMs(), created);
    Provenance& self = e; e = self;
    EXPECT_STREQ(e.slideId(), "s");
}
