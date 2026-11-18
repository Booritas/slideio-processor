// Requirements: UR-PIX-3, UR-QA-2, UR-ALG-6
#include <slideio/processor/algorithm/pixel/filter.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/pipeline/operation_catalog.hpp>
#include <slideio/processor/pipeline/pipeline_spec.hpp>

#include <algorithm/support/harness.hpp>
#include <pipeline/detail/parameters_json.hpp>
#include <pipeline/test_support/synthetic_scene.hpp>

#include <opencv2/imgproc.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

using namespace slideio::processor;
namespace ts = slideio::processor::testing_support;

namespace {

constexpr const char* kSobel = "pixel.sobel";
constexpr const char* kScharr = "pixel.scharr";
constexpr const char* kLaplacian = "pixel.laplacian";

// The message of the InvalidArgumentError @p create throws; "" when none.
std::string rejection(const std::function<void()>& create) {
    try {
        create();
    } catch (const InvalidArgumentError& e) {
        return e.what();
    }
    return "";
}

std::string byNameRejection(const char* name, const OperationParameters& p) {
    return rejection([&] { OperationCatalog::instance().create(name, p); });
}

void expectSameContract(const OperationPtr& byFactory, const OperationPtr& byName, const char* name,
                        int32_t halo) {
    EXPECT_TRUE(detail::parametersEqual(byFactory->parameters(), byName->parameters())) << name;
    EXPECT_STREQ(byFactory->name(), name);
    EXPECT_STREQ(byName->name(), name);
    EXPECT_EQ(byFactory->arity(), ChannelArity::PerChannel);
    EXPECT_FALSE(byFactory->requiresUniformTypes());
    EXPECT_FALSE(byFactory->producesObjects());
    for (const OperationPtr& op : {byFactory, byName}) {
        const Halo h = op->halo();
        EXPECT_EQ(h.left, halo) << name;
        EXPECT_EQ(h.top, halo) << name;
        EXPECT_EQ(h.right, halo) << name;
        EXPECT_EQ(h.bottom, halo) << name;
    }
    for (int32_t i = 0; i < 11; ++i) {
        const auto type = static_cast<DataType>(i);
        EXPECT_EQ(byFactory->supportedTypes().contains(type),
                  type != DataType::Int64 && type != DataType::UInt64)
            << name << " " << dataTypeName(type);
    }
    EXPECT_EQ(byFactory->outputType(DataType::Byte), DataType::Float32);
    EXPECT_EQ(byFactory->outputType(DataType::Float64), DataType::Float64);
    EXPECT_STREQ(OperationCatalog::instance().complexityNote(name), "O(n * kernelSize) in the number of samples");
}

// Applies @p op to a width x height Float64 image of @p f and returns the output.
ts::Image applyTo(const OperationPtr& op, int64_t width, int64_t height,
                  const std::function<double(int64_t, int64_t)>& f) {
    return ts::applyDirect(*op, {DataType::Float64}, {0}, width, height,
                           [&](int32_t, int64_t x, int64_t y) { return f(x, y); });
}

// Asserts every output pixel at least @p margin from the border equals @p expected.
void expectInterior(const ts::Image& out, int64_t margin, double expected, const std::string& what) {
    ASSERT_EQ(out.types, std::vector<DataType>{DataType::Float64}) << what;
    for (int64_t y = margin; y < out.height - margin; ++y) {
        for (int64_t x = margin; x < out.width - margin; ++x) {
            ASSERT_EQ(out.at(0, x, y), expected) << what << " at " << x << "," << y;
        }
    }
}

// OpenCV's result for the same impulse image with a zero border, and the
// outer product of getDerivKernels(dx, dy, ksize), placed as a correlation
// places it: output (c + i, c + j) is kx[r - i] * ky[r - j].
cv::Mat impulse(int32_t size) {
    cv::Mat m = cv::Mat::zeros(size, size, CV_64F);
    m.at<double>(size / 2, size / 2) = 1.0;
    return m;
}

cv::Mat outerProduct(int32_t dx, int32_t dy, int32_t ksize, int32_t size) {
    cv::Mat kx;
    cv::Mat ky;
    cv::getDerivKernels(kx, ky, dx, dy, ksize, false, CV_64F);
    const int32_t rx = static_cast<int32_t>(kx.total()) / 2;
    const int32_t ry = static_cast<int32_t>(ky.total()) / 2;
    const int32_t c = size / 2;
    cv::Mat m = cv::Mat::zeros(size, size, CV_64F);
    for (int32_t j = -ry; j <= ry; ++j) {
        for (int32_t i = -rx; i <= rx; ++i) {
            m.at<double>(c + j, c + i) = kx.ptr<double>()[rx - i] * ky.ptr<double>()[ry - j];
        }
    }
    return m;
}

void expectImpulseResponse(const OperationPtr& op, const cv::Mat& expected, const std::string& what) {
    const int32_t size = expected.rows;
    const ts::Image out = applyTo(op, size, size, [&](int64_t x, int64_t y) {
        return x == size / 2 && y == size / 2 ? 1.0 : 0.0;
    });
    for (int32_t y = 0; y < size; ++y) {
        for (int32_t x = 0; x < size; ++x) {
            ASSERT_EQ(out.at(0, x, y), expected.at<double>(y, x)) << what << " at " << x << "," << y;
        }
    }
}

} // namespace

TEST(Derivatives, FactoriesEqualNamePaths) {
    auto& catalog = OperationCatalog::instance();
    // Defaults: Sobel (1, 0, 3), Scharr (1, 0), Laplacian 1.
    const OperationPtr sobelDefault = catalog.create(kSobel, OperationParameters());
    EXPECT_EQ(sobelDefault->parameters().count(), 3);
    expectSameContract(makeSobel(1, 0, 3), sobelDefault, kSobel, 1);
    const OperationPtr scharrDefault = catalog.create(kScharr, OperationParameters());
    EXPECT_EQ(scharrDefault->parameters().count(), 2);
    expectSameContract(makeScharr(1, 0), scharrDefault, kScharr, 1);
    const OperationPtr laplacianDefault = catalog.create(kLaplacian, OperationParameters());
    EXPECT_EQ(laplacianDefault->parameters().count(), 1);
    expectSameContract(makeLaplacian(1), laplacianDefault, kLaplacian, 1);

    // Explicit values; halo max(1, kernelSize / 2).
    expectSameContract(makeSobel(0, 2, 7),
                       catalog.create(kSobel, OperationParameters().setInt("dx", 0).setInt("dy", 2).setInt(
                                                  "kernelSize", 7)),
                       kSobel, 3);
    expectSameContract(makeSobel(1, 1, 1),
                       catalog.create(kSobel, OperationParameters().setInt("dx", 1).setInt("dy", 1).setInt(
                                                  "kernelSize", 1)),
                       kSobel, 1);
    expectSameContract(makeScharr(0, 1), catalog.create(kScharr, OperationParameters().setInt("dx", 0).setInt("dy", 1)),
                       kScharr, 1);
    expectSameContract(makeLaplacian(5), catalog.create(kLaplacian, OperationParameters().setInt("kernelSize", 5)),
                       kLaplacian, 2);

    EXPECT_STREQ(catalog.correctnessReference(kSobel),
                 "Analytic: responses to polynomial images; coefficients of OpenCV getDerivKernels");
    EXPECT_STREQ(catalog.correctnessReference(kLaplacian),
                 "Analytic: responses to polynomial images; coefficients of OpenCV getDerivKernels");
    EXPECT_STREQ(catalog.correctnessReference(kScharr),
                 "Analytic: response to a linear ramp; Scharr coefficients [3 10 3] x [-1 0 1]");
}

TEST(Derivatives, RejectsBadOrders) {
    struct Case {
        std::function<void()> create;
        const char* name;
        const char* key;
        const char* value; // must not appear in the message; "" to skip
    };
    const std::vector<Case> cases = {
        {[] { makeSobel(0, 0, 3); }, kSobel, "'dx'", ""},
        {[] { makeSobel(3, 0, 3); }, kSobel, "'dx'", "3"},
        {[] { makeSobel(0, -1, 3); }, kSobel, "'dy'", "-1"},
        {[] { makeSobel(1, 0, 4); }, kSobel, "'kernelSize'", "4"},
        {[] { makeSobel(1, 0, 9); }, kSobel, "'kernelSize'", "9"},
        {[] { makeScharr(1, 1); }, kScharr, "'dx'", ""},
        {[] { makeScharr(0, 0); }, kScharr, "'dx'", ""},
        {[] { makeScharr(2, 0); }, kScharr, "'dx'", ""},
        {[] { makeLaplacian(9); }, kLaplacian, "'kernelSize'", "9"},
        {[] { makeLaplacian(0); }, kLaplacian, "'kernelSize'", ""},
    };
    for (const Case& c : cases) {
        const std::string message = rejection(c.create);
        EXPECT_NE(message.find(c.name), std::string::npos) << message;
        EXPECT_NE(message.find(c.key), std::string::npos) << message;
        if (*c.value != '\0') {
            EXPECT_EQ(message.find(c.value), std::string::npos) << message;
        }
    }

    // By name, the same values, an unknown key, a wrong type and a value beyond int32.
    EXPECT_NE(byNameRejection(kSobel, OperationParameters().setInt("dx", 0).setInt("dy", 0)).find("'dx'"),
              std::string::npos);
    EXPECT_NE(byNameRejection(kSobel, OperationParameters().setInt("kernelSize", 4)).find("'kernelSize'"),
              std::string::npos);
    EXPECT_NE(byNameRejection(kSobel, OperationParameters().setInt("dx", int64_t{1} << 40)).find("'dx'"),
              std::string::npos);
    EXPECT_NE(byNameRejection(kScharr, OperationParameters().setInt("dy", 1)).find("'dx'"), std::string::npos);
    EXPECT_NE(byNameRejection(kLaplacian, OperationParameters().setInt("kernelSize", 9)).find("'kernelSize'"),
              std::string::npos);
    EXPECT_NE(byNameRejection(kLaplacian, OperationParameters().setInt("ksize", 3)).find("ksize"),
              std::string::npos);
    EXPECT_FALSE(byNameRejection(kSobel, OperationParameters().setDouble("dx", 1.0)).empty());
    EXPECT_FALSE(byNameRejection(kScharr, OperationParameters().setString("dx", "1")).empty());

    // Every valid combination is accepted.
    for (int32_t k : {1, 3, 5, 7}) {
        for (int32_t dx = 0; dx <= 2; ++dx) {
            for (int32_t dy = 0; dy <= 2; ++dy) {
                if (dx + dy > 0) {
                    EXPECT_TRUE(rejection([&] { makeSobel(dx, dy, k); }).empty()) << dx << dy << k;
                }
            }
        }
        EXPECT_TRUE(rejection([&] { makeLaplacian(k); }).empty()) << k;
    }
}

TEST(Derivatives, AnalyticResponses) {
    constexpr int64_t kSize = 24;
    const auto x1 = [](int64_t x, int64_t) { return static_cast<double>(x); };
    const auto y1 = [](int64_t, int64_t y) { return static_cast<double>(y); };
    const auto x2 = [](int64_t x, int64_t) { return static_cast<double>(x * x); };
    const auto r2 = [](int64_t x, int64_t y) { return static_cast<double>(x * x + y * y); };
    expectInterior(applyTo(makeSobel(1, 0, 3), kSize, kSize, x1), 1, 8.0, "Sobel dx=1 on x");
    expectInterior(applyTo(makeSobel(0, 1, 3), kSize, kSize, y1), 1, 8.0, "Sobel dy=1 on y");
    expectInterior(applyTo(makeSobel(2, 0, 3), kSize, kSize, x2), 1, 8.0, "Sobel dx=2 on x^2");
    expectInterior(applyTo(makeScharr(1, 0), kSize, kSize, x1), 1, 32.0, "Scharr dx=1 on x");
    expectInterior(applyTo(makeLaplacian(1), kSize, kSize, r2), 1, 4.0, "Laplacian 1 on x^2 + y^2");
    expectInterior(applyTo(makeLaplacian(3), kSize, kSize, x2), 1, 8.0, "Laplacian 3 on x^2");
    // Sobel of size 1 does not smooth across the derivative's axis.
    expectInterior(applyTo(makeSobel(1, 0, 1), kSize, kSize, x1), 1, 2.0, "Sobel dx=1 size 1 on x");
}

TEST(Derivatives, CoefficientsMatchOpenCV) {
    constexpr int32_t kImage = 17;
    for (int32_t k : {1, 3, 5, 7}) {
        for (int32_t dx = 0; dx <= 2; ++dx) {
            for (int32_t dy = 0; dy <= 2; ++dy) {
                if (dx + dy == 0) {
                    continue;
                }
                const std::string what = "Sobel " + std::to_string(dx) + "," + std::to_string(dy) + "," +
                                         std::to_string(k);
                const cv::Mat expected = outerProduct(dx, dy, k, kImage);
                cv::Mat byOpenCV;
                cv::Sobel(impulse(kImage), byOpenCV, CV_64F, dx, dy, k, 1.0, 0.0, cv::BORDER_CONSTANT);
                ASSERT_EQ(cv::norm(expected, byOpenCV, cv::NORM_INF), 0.0) << what;
                expectImpulseResponse(makeSobel(dx, dy, k), expected, what);
            }
        }
        const cv::Mat laplacian = outerProduct(2, 0, k, kImage) + outerProduct(0, 2, k, kImage);
        cv::Mat byOpenCV;
        cv::Laplacian(impulse(kImage), byOpenCV, CV_64F, k, 1.0, 0.0, cv::BORDER_CONSTANT);
        ASSERT_EQ(cv::norm(laplacian, byOpenCV, cv::NORM_INF), 0.0) << "Laplacian " << k;
        expectImpulseResponse(makeLaplacian(k), laplacian, "Laplacian " + std::to_string(k));
    }
    for (const auto& [dx, dy] : {std::pair<int32_t, int32_t>{1, 0}, {0, 1}}) {
        const std::string what = "Scharr " + std::to_string(dx) + "," + std::to_string(dy);
        const cv::Mat expected = outerProduct(dx, dy, cv::FILTER_SCHARR, kImage);
        cv::Mat byOpenCV;
        cv::Scharr(impulse(kImage), byOpenCV, CV_64F, dx, dy, 1.0, 0.0, cv::BORDER_CONSTANT);
        ASSERT_EQ(cv::norm(expected, byOpenCV, cv::NORM_INF), 0.0) << what;
        expectImpulseResponse(makeScharr(dx, dy), expected, what);
    }
}

TEST(Derivatives, TiledEqualsSingleTile) {
    const auto pattern = [](int, int64_t x, int64_t y) {
        return static_cast<double>((x * 7 + y * 13 + (x * y) % 11) % 256) * 3.25 - 100.0;
    };
    auto scene = ts::makeSyntheticScene({{300, 260, 0, 0}}, {slideio::DataType::DT_Float32}, pattern);
    for (const OperationPtr& op : {makeSobel(1, 1, 5), makeScharr(0, 1), makeLaplacian(7)}) {
        SCOPED_TRACE(op->name());
        PipelineSpec spec;
        spec.then(op);
        const ts::Image reference = ts::runEager(scene, spec, 4096, 1);
        ASSERT_EQ(reference.width, 300);
        ASSERT_EQ(reference.height, 260);
        ASSERT_EQ(reference.types, std::vector<DataType>{DataType::Float32});
        for (int32_t tile : {64, 100, 257}) {
            const ts::Image tiled = ts::runEager(scene, spec, tile, 4);
            EXPECT_TRUE(tiled.planes == reference.planes) << "tile " << tile;
        }
    }
}
