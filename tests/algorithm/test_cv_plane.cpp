// Requirements: UR-PIX-3, UR-IMG-11
#include "detail/cv_plane.hpp"

#include <slideio/processor/core/error.hpp>

#include <gtest/gtest.h>

#include <opencv2/core.hpp>

#include <new>
#include <string>

using namespace slideio::processor;

namespace {

// Runs translateCv while handling a cv::Exception built from the arguments.
void translate(int code, const char* message, const char* function) {
    try {
        throw cv::Exception(code, message, function, "source.cpp", 42);
    } catch (const cv::Exception&) {
        detail::translateCv("pixel.example");
    }
}

} // namespace

TEST(CvPlane, TranslateCvNamesTheCodeAndFunctionOnly) {
    // OpenCV's own message may print the values it checked; the cause keeps
    // only the error code and the function.
    try {
        translate(cv::Error::StsAssert, "Expected 'v <= 12345', where 'v' is 67890", "cv::Canny");
        FAIL() << "no exception";
    } catch (const InternalError& e) {
        ASSERT_NE(e.operationName(), nullptr);
        EXPECT_STREQ(e.operationName(), "pixel.example");
        ASSERT_NE(e.cause(), nullptr);
        EXPECT_STREQ(e.cause(), "OpenCV error -215 in cv::Canny");
        const std::string what = e.what();
        EXPECT_EQ(what.find("12345"), std::string::npos) << what;
        EXPECT_EQ(what.find("67890"), std::string::npos) << what;
        EXPECT_EQ(what.find("source.cpp"), std::string::npos) << what;
    }
}

TEST(CvPlane, TranslateCvMapsOutOfMemoryToBadAlloc) {
    EXPECT_THROW(translate(cv::Error::StsNoMem, "Failed to allocate 123456 bytes", "cv::OutOfMemoryError"),
                 std::bad_alloc);
}
