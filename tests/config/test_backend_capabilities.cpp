// Requirements: UR-IMG-10, UR-IMG-11, UR-IMG-12, UR-BLD-4
#include <slideio/processor/config/backend_capabilities.hpp>
#include <slideio/processor/core/data_type.hpp>

#include "config/detail/backend_types.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

using slideio::processor::BackendCapabilities;
using slideio::processor::DataType;
using slideio::processor::DataTypeSet;
namespace detail = slideio::processor::detail;

namespace {

constexpr std::array<DataType, 11> kAllTypes = {
    DataType::Byte,   DataType::Int8,   DataType::UInt16, DataType::Int16,
    DataType::UInt32, DataType::Int32,  DataType::UInt64, DataType::Int64,
    DataType::Float16, DataType::Float32, DataType::Float64};

} // namespace

// The expected version is the conanfile.py pin, read independently of the
// build's find_package result (tests/config/CMakeLists.txt).
TEST(BackendCapabilities, ReportsThePinnedOpenCv) {
    const BackendCapabilities& b = BackendCapabilities::instance();
    EXPECT_EQ(&b, &BackendCapabilities::instance());
    EXPECT_STREQ(b.backendName(), "OpenCV");
    EXPECT_STREQ(b.backendVersion(), SLIDEIO_PROCESSOR_TEST_PINNED_OPENCV_VERSION);
    EXPECT_EQ(b.backendMajorVersion(), SLIDEIO_PROCESSOR_TEST_PINNED_OPENCV_MAJOR);
}

TEST(BackendCapabilities, SupportedAndStorageOnlyPartitionAllTypes) {
    const BackendCapabilities& b = BackendCapabilities::instance();
    for (DataType t : kAllTypes) {
        EXPECT_NE(b.supportedTypes().contains(t), b.storageOnlyTypes().contains(t));
        EXPECT_EQ(b.supports(t), b.supportedTypes().contains(t));
    }
    EXPECT_FALSE(b.supports(DataType::Unknown));
    EXPECT_EQ(b.supportedTypes().count() + b.storageOnlyTypes().count(), 11);
}

TEST(BackendCapabilities, OpenCv4LacksExactlyTheThreeWideIntegers) { // UR-BLD-4
    const DataTypeSet four = detail::backendTypesForOpenCvMajor(4);
    EXPECT_EQ(four.count(), 8);
    EXPECT_FALSE(four.contains(DataType::UInt32));
    EXPECT_FALSE(four.contains(DataType::Int64));
    EXPECT_FALSE(four.contains(DataType::UInt64));
    EXPECT_TRUE(four.contains(DataType::Float16));
    EXPECT_EQ(detail::backendTypesForOpenCvMajor(5).count(), 11);
    EXPECT_EQ(detail::backendTypesForOpenCvMajor(6).count(), 11);
}

TEST(BackendCapabilities, InstanceMatchesTheRuleForThePinnedMajor) { // UR-BLD-4
    const BackendCapabilities& b = BackendCapabilities::instance();
    const bool wideIntegers = SLIDEIO_PROCESSOR_TEST_PINNED_OPENCV_MAJOR >= 5;
    EXPECT_EQ(b.supports(DataType::UInt32), wideIntegers);
    EXPECT_EQ(b.supports(DataType::Int64), wideIntegers);
    EXPECT_EQ(b.supports(DataType::UInt64), wideIntegers);
    EXPECT_EQ(b.supportedTypes().count(), wideIntegers ? 11 : 8);
}
