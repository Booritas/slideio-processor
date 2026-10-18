// Requirements: none (umbrella header)
#include <slideio/processor/config/config.hpp>

#include <gtest/gtest.h>

using namespace slideio::processor;

TEST(Umbrella, ExposesTheWholeModule) {
    const Configuration c = Configuration::defaults();
    EXPECT_GE(c.workerCount(), 1);
    EXPECT_GT(StandardVocabulary::instance().labelCount(), 0);
    EXPECT_GT(BackendCapabilities::instance().supportedTypes().count(), 0);
    auto* fn = static_cast<void (*)(const Configuration&)>(&configureLogging);
    EXPECT_NE(fn, nullptr);
}
