// Requirements: UR-EXE-2, UR-ALG-8
#include <slideio/processor/algorithm/pixel/filter.hpp>
#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/memory_budget.hpp>
#include <slideio/processor/pipeline/operation_parameters.hpp>
#include <slideio/processor/pipeline/pipeline.hpp>

#include <algorithm/support/working_set_check.hpp>
#include <pipeline/detail/pipeline_impl.hpp>
#include <pipeline/detail/run_plan.hpp>
#include <pipeline/test_support/synthetic_scene.hpp>

#include <gtest/gtest.h>

#include <opencv2/core.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace slideio::processor;
namespace ts = slideio::processor::testing_support;

namespace {

using ts::createOperation;
using ts::expectApplyBounded;
using ts::UnmeteredShare;

// The meter counts the calling thread only, so OpenCV runs on that thread
// alone while a check measures; its earlier thread count is restored after.
class OneOpenCvThread {
public:
    OneOpenCvThread() : saved_(cv::getNumThreads()) { cv::setNumThreads(1); }
    ~OneOpenCvThread() { cv::setNumThreads(saved_); }
    OneOpenCvThread(const OneOpenCvThread&) = delete;
    OneOpenCvThread& operator=(const OneOpenCvThread&) = delete;

private:
    int saved_;
};

// The types a filter computing in double accepts.
const DataType kFloatFilterTypes[] = {DataType::Byte,   DataType::Int8,    DataType::UInt16,
                                      DataType::Int16,  DataType::UInt32,  DataType::Int32,
                                      DataType::Float16, DataType::Float32, DataType::Float64};

const DataType kMorphologyTypes[] = {DataType::Byte, DataType::UInt16, DataType::Int16, DataType::Float32,
                                     DataType::Float64};

const std::vector<int64_t> kSmallTile{64};

int64_t haloWidth(const Operation& op, int64_t width) {
    return width + op.halo().left + op.halo().right;
}

int64_t haloHeight(const Operation& op, int64_t height) {
    return height + op.halo().top + op.halo().bottom;
}

// What cv::medianBlur takes through cv::fastMalloc for one Byte plane, with one
// OpenCV thread (OpenCV 4.14): the destination, the size of the halo'd plane
// (imgproc/src/median_blur.dispatch.cpp, cv::medianBlur), and, for a kernel
// above 5, the plane copied with kernelSize / 2 columns of border on either
// side (median_blur.simd.hpp, medianBlur, copyMakeBorder).
int64_t medianShare(const Operation& op, DataType type, int64_t width, int64_t height) {
    if (type != DataType::Byte) {
        return 0;
    }
    const int64_t k = 2 * int64_t(op.halo().left) + 1;
    const int64_t w = haloWidth(op, width);
    const int64_t h = haloHeight(op, height);
    return w * h + (k > 5 ? (w + k - 1) * h : 0);
}

// What cv::erode, cv::dilate and cv::morphologyEx take through cv::fastMalloc
// for one integer plane, with one OpenCV thread (OpenCV 4.14,
// imgproc/src/morph.dispatch.cpp): the destination, the size of the halo'd
// plane (morphologyEx, morphOp); and, for a rectangle iterated more than once,
// the rectangle of 1 + iterations * (side - 1) per side that morphOp builds
// with getStructuringElement in its place.
UnmeteredShare morphologyShare(bool rectangle, int64_t elementWidth, int64_t elementHeight, int64_t iterations) {
    return [=](const Operation& op, DataType type, int64_t width, int64_t height) -> int64_t {
        if (type == DataType::Float32 || type == DataType::Float64) {
            return 0;
        }
        const int64_t area = haloWidth(op, width) * haloHeight(op, height);
        const int64_t enlarged = rectangle && iterations > 1
                                     ? (1 + iterations * (elementWidth - 1)) * (1 + iterations * (elementHeight - 1))
                                     : 0;
        return area * dataTypeSizeBytes(type) + enlarged;
    };
}

// What cv::Canny takes through cv::fastMalloc for one plane, with one OpenCV
// thread (OpenCV 4.14, imgproc/src/canny.cpp): the destination edge map, the
// size of the halo'd plane (cv::Canny); the suppression map of (h + 2) rows of
// the width plus 2 rounded up past one SIMD width, at most 128 more
// (parallelCanny's constructor); and the two 16-bit gradient planes of the
// stripe, here every row (parallelCanny::operator(), Sobel).
int64_t cannyShare(const Operation& op, DataType, int64_t width, int64_t height) {
    const int64_t w = haloWidth(op, width);
    const int64_t h = haloHeight(op, height);
    return w * h + (h + 2) * (w + 128) + 2 * 2 * w * h;
}

OperationParameters morphologyParameters(const char* shape, int64_t width, int64_t height, int64_t iterations) {
    OperationParameters p;
    p.setString("shape", shape).setInt("elementWidth", width).setInt("elementHeight", height).setInt("iterations",
                                                                                                    iterations);
    return p;
}

} // namespace

TEST(WorkingSetFilter, DeclarationsBoundTheMeasuredPeak) {
    const OneOpenCvThread oneThread;

    // pixel.gaussian_blur: the default, the smallest kernels and the largest.
    for (DataType type : kFloatFilterTypes) {
        expectApplyBounded("gaussian_blur default",
                           [] { return createOperation("pixel.gaussian_blur", OperationParameters()); }, {type});
        expectApplyBounded("gaussian_blur 1", [] { return makeGaussianBlur(1.0, 1); }, {type});
        expectApplyBounded("gaussian_blur 3", [] { return makeGaussianBlur(1.0, 3); }, {type});
        expectApplyBounded("gaussian_blur 1025", [] { return makeGaussianBlur(100.0, 1025); }, {type});
    }

    // pixel.sobel, pixel.scharr, pixel.laplacian: the defaults on every type,
    // every kernel size on the narrowest and widest.
    for (DataType type : kFloatFilterTypes) {
        expectApplyBounded("sobel default", [] { return createOperation("pixel.sobel", OperationParameters()); },
                           {type});
        expectApplyBounded("scharr default", [] { return createOperation("pixel.scharr", OperationParameters()); },
                           {type});
        expectApplyBounded("laplacian default",
                           [] { return createOperation("pixel.laplacian", OperationParameters()); }, {type});
    }
    for (DataType type : {DataType::Byte, DataType::Float64}) {
        for (int32_t size : {1, 3, 5, 7}) {
            const std::string at = " " + std::to_string(size);
            expectApplyBounded("sobel 1,0" + at, [size] { return makeSobel(1, 0, size); }, {type});
            expectApplyBounded("sobel 2,2" + at, [size] { return makeSobel(2, 2, size); }, {type});
            expectApplyBounded("laplacian" + at, [size] { return makeLaplacian(size); }, {type});
        }
        expectApplyBounded("scharr 0,1", [] { return makeScharr(0, 1); }, {type});
    }

    // pixel.median_blur: the default on every type; OpenCV's Byte paths (the
    // sorting network, the histogram paths); the largest kernel on the
    // module's own path.
    for (DataType type : ts::kAllDataTypes) {
        expectApplyBounded("median_blur default",
                           [] { return createOperation("pixel.median_blur", OperationParameters()); }, {type},
                           medianShare);
    }
    for (int32_t size : {5, 7, 15, 17, 255}) {
        expectApplyBounded("median_blur " + std::to_string(size), [size] { return makeMedianBlur(size); },
                           {DataType::Byte}, medianShare);
    }
    expectApplyBounded("median_blur 255", [] { return makeMedianBlur(255); }, {DataType::UInt16}, medianShare,
                       kSmallTile);
    expectApplyBounded("median_blur 31", [] { return makeMedianBlur(31); }, {DataType::Float64}, medianShare);

    // pixel.bilateral_filter: the default on every type, the largest diameter.
    for (DataType type : kFloatFilterTypes) {
        expectApplyBounded("bilateral_filter default",
                           [] { return createOperation("pixel.bilateral_filter", OperationParameters()); }, {type});
    }
    for (DataType type : {DataType::Byte, DataType::Float64}) {
        expectApplyBounded("bilateral_filter 255", [] { return makeBilateralFilter(255, 25.0, 50.0); }, {type},
                           UnmeteredShare(), kSmallTile);
    }

    // pixel.canny: the default and every aperture, thresholds that keep many
    // edges and none.
    expectApplyBounded("canny default", [] { return createOperation("pixel.canny", OperationParameters()); },
                       {DataType::Byte}, cannyShare);
    for (int32_t aperture : {3, 5, 7}) {
        const std::string at = " " + std::to_string(aperture);
        expectApplyBounded("canny low" + at, [aperture] { return makeCanny(1.0, 2.0, aperture); },
                           {DataType::Byte}, cannyShare);
        expectApplyBounded("canny high" + at, [aperture] { return makeCanny(1e9, 1e9, aperture); },
                           {DataType::Byte}, cannyShare);
    }

    // The four morphology operations: the default; the largest rectangle and
    // cross; a rectangle iterated 64 times; a large rectangle on the float
    // path, whose cost grows with the element's area.
    for (const char* name : {"pixel.morph_erode", "pixel.morph_dilate", "pixel.morph_open", "pixel.morph_close"}) {
        const std::string op(name);
        for (DataType type : kMorphologyTypes) {
            const bool isFloat = type == DataType::Float32 || type == DataType::Float64;
            expectApplyBounded(op + " default", [name] { return createOperation(name, OperationParameters()); },
                               {type}, morphologyShare(true, 3, 3, 1));
            expectApplyBounded(op + " cross 255x255",
                               [name] { return createOperation(name, morphologyParameters("Cross", 255, 255, 1)); },
                               {type}, morphologyShare(false, 255, 255, 1), kSmallTile);
            expectApplyBounded(op + " rectangle 3x3 x64",
                               [name] { return createOperation(name, morphologyParameters("Rectangle", 3, 3, 64)); },
                               {type}, morphologyShare(true, 3, 3, 64), kSmallTile);
            if (!isFloat) {
                expectApplyBounded(
                    op + " rectangle 255x255",
                    [name] { return createOperation(name, morphologyParameters("Rectangle", 255, 255, 1)); },
                    {type}, morphologyShare(true, 255, 255, 1), kSmallTile);
                expectApplyBounded(
                    op + " ellipse 31x63",
                    [name] { return createOperation(name, morphologyParameters("Ellipse", 31, 63, 2)); }, {type},
                    morphologyShare(false, 31, 63, 2), kSmallTile);
            }
        }
        for (DataType type : {DataType::Float32, DataType::Float64}) {
            expectApplyBounded(op + " rectangle 63x63",
                               [name] { return createOperation(name, morphologyParameters("Rectangle", 63, 63, 1)); },
                               {type}, morphologyShare(true, 63, 63, 1), kSmallTile);
        }
    }
}

// A 4096-pixel tile under the largest bilateral window: the tile's buffers fit a
// 512 MB ceiling, but with the filter's working set they do not, and the
// run is refused before it starts, naming the filter.
TEST(WorkingSetFilter, BilateralLargeTileRefusedUnderTheCeiling) {
    auto source = std::make_shared<ts::SyntheticSource>(
        std::vector<ts::SyntheticLevel>{{5000, 5000, 0, 0}},
        std::vector<slideio::DataType>{slideio::DataType::DT_Byte, slideio::DataType::DT_Byte,
                                       slideio::DataType::DT_Byte},
        [](int, int64_t, int64_t) { return 0.0; });
    const auto scene = ts::makeSyntheticScene(source);
    const Configuration config =
        ConfigurationBuilder().tileSize(4096, 4096).tileSizeFromSlide(false).workerCount(1).build();
    PipelineSpec spec;
    OperationParameters bilateral;
    bilateral.setInt("diameter", 255);
    spec.operation("pixel.bilateral_filter", bilateral);
    constexpr int64_t kCeiling = int64_t(512) * 1024 * 1024;

    Pipeline measured(scene, config, spec);
    measured.memoryBudget(makeMemoryBudget(int64_t(64) << 30));
    measured.validate();
    const int64_t tileBytes = detail::PipelineAccess::plan(measured).tileBytes;
    const OperationPtr op = createOperation("pixel.bilateral_filter", bilateral);
    const int64_t side = 4096 + 254;
    const int64_t workingSet = side * side * op->workingSetBytesPerLevelPixel() + op->workingSetBytesPerTile();
    ASSERT_GT(tileBytes, kCeiling);
    ASSERT_LE(tileBytes - workingSet, kCeiling);

    Pipeline refused(scene, config, spec);
    refused.memoryBudget(makeMemoryBudget(kCeiling));
    std::string message;
    try {
        refused.validate();
    } catch (const InvalidArgumentError& e) {
        message = e.what();
    }
    EXPECT_NE(message.find("pixel.bilateral_filter"), std::string::npos) << message;
    EXPECT_NE(message.find("above the memory ceiling"), std::string::npos) << message;
    EXPECT_EQ(source->readCount(), 0);
}
