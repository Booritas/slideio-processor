// Requirements: none (umbrella header)
#include <slideio/processor/storage/storage.hpp>

#include "common/temp_path.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <system_error>

using namespace slideio::processor;

TEST(Umbrella, ExposesTheWholeModule) {
    const std::filesystem::path path = uniqueTempPath("umbrella", ".store");
    {
        StorePtr store = Store::open(path.u8string().c_str(), OpenMode::CreateNew, Configuration::defaults());
        ASSERT_NE(store, nullptr);

        EXPECT_TRUE(store->layers().exists(store->layers().pixelLayer()));
        EXPECT_GT(store->vocabulary().count(), 0);
        EXPECT_EQ(store->objects().totalObjectCount(), 0);
        EXPECT_GE(store->events().eventCount(), 0);
        EXPECT_EQ(store->stageStates().count(), 0);
        const StageId stage = store->stageStateWriter().declare("umbrella", "{}");
        EXPECT_EQ(store->stageStates().count(), 1);
        EXPECT_STREQ(store->stageStates().name(stage), "umbrella");
        store->close();
    }
    std::error_code ec;
    for (const char* suffix : {"", "-wal", "-shm", "-writer.lock"}) {
        std::filesystem::path side = path;
        side += suffix;
        std::filesystem::remove(side, ec);
    }
}
