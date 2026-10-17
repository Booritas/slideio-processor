// Requirements: UR-API-6, UR-RES-11
#include <slideio/processor/core/version.hpp>

#include <gtest/gtest.h>

using namespace slideio::processor;

TEST(Version, MatchesProjectVersion) {
    EXPECT_STREQ(libraryVersion(), "0.1.0");
    EXPECT_EQ(libraryVersionMajor(), 0);
    EXPECT_EQ(libraryVersionMinor(), 1);
    EXPECT_EQ(libraryVersionPatch(), 0);
}
