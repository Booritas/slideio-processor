// Requirements: UR-BLD-3, UR-BLD-4, UR-IMG-11, UR-IMG-12
#include <slideio/processor/config/backend_capabilities.hpp>
#include <slideio/processor/core/data_type.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/pipeline/backend.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

using slideio::processor::Backend;
using slideio::processor::BackendCapabilities;
using slideio::processor::DataType;
using slideio::processor::DataTypeSet;
using slideio::processor::UnsupportedDataTypeError;

TEST(Backend, NameAndVersion) {
    EXPECT_EQ(&Backend::instance(), &Backend::instance());
    EXPECT_STREQ(Backend::instance().name(), "OpenCV");
    EXPECT_STREQ(Backend::instance().version(), BackendCapabilities::instance().backendVersion());
}

// config derives the set without linking OpenCV; the engine maps it from the
// OpenCV it links. The two must never disagree (ruling P4).
TEST(Backend, AgreesWithBackendCapabilities) {
    EXPECT_TRUE(Backend::instance().supportedTypes() == BackendCapabilities::instance().supportedTypes());
    for (int32_t i = 0; i < DataTypeSet::all().count(); ++i) {
        const DataType t = DataTypeSet::all().at(i);
        EXPECT_EQ(Backend::instance().supports(t), BackendCapabilities::instance().supports(t))
            << slideio::processor::dataTypeName(t);
    }
    EXPECT_FALSE(Backend::instance().supports(DataType::Unknown));
}

TEST(Backend, DepthRoundTripForEverySupportedType) {
    const Backend& backend = Backend::instance();
    const DataTypeSet types = backend.supportedTypes();
    ASSERT_GT(types.count(), 0);
    for (int32_t i = 0; i < types.count(); ++i) {
        const DataType t = types.at(i);
        EXPECT_EQ(backend.fromBackendDepth(backend.toBackendDepth(t)), t)
            << slideio::processor::dataTypeName(t);
    }
}

TEST(Backend, UnsupportedTypeNamesTypeAndBackend) {
    if (BackendCapabilities::instance().backendMajorVersion() >= 5) {
        GTEST_SKIP() << "UInt32 is computable from OpenCV 5 on";
    }
    EXPECT_FALSE(Backend::instance().supports(DataType::UInt32));
    try {
        Backend::instance().toBackendDepth(DataType::UInt32);
        FAIL() << "expected UnsupportedDataTypeError";
    } catch (const UnsupportedDataTypeError& e) {
        const std::string what = e.what();
        EXPECT_NE(what.find("UInt32"), std::string::npos) << what;
        EXPECT_NE(what.find("OpenCV"), std::string::npos) << what;
        EXPECT_EQ(e.offendingType(), DataType::UInt32);
        EXPECT_TRUE(e.supportedTypes() == Backend::instance().supportedTypes());
    }
}

TEST(Backend, UnknownTypeIsRejected) {
    EXPECT_THROW(Backend::instance().toBackendDepth(DataType::Unknown), UnsupportedDataTypeError);
}

TEST(Backend, UnknownDepthIsUnknown) {
    EXPECT_EQ(Backend::instance().fromBackendDepth(9999), DataType::Unknown);
    EXPECT_EQ(Backend::instance().fromBackendDepth(-1), DataType::Unknown);
}
