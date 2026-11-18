// Requirements: UR-PIX-3, UR-QA-2, UR-IMG-11, UR-ALG-6
#include <slideio/processor/algorithm/pixel/filter.hpp>

#include <slideio/processor/core/error.hpp>

#include "detail/bag.hpp"
#include "detail/builtin_factory.hpp"
#include "detail/builtin_operations.hpp"
#include "detail/cv_plane.hpp"
#include "detail/separable.hpp"
#include "detail/working_set.hpp"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace slideio::processor {

namespace {

constexpr const char* kSobel = "pixel.sobel";
constexpr const char* kScharr = "pixel.scharr";
constexpr const char* kLaplacian = "pixel.laplacian";
constexpr const char* kComplexity = "O(n * kernelSize) in the number of samples";
constexpr const char* kKernelSizeRule = "must be 1, 3, 5 or 7";

[[noreturn]] void throwParameter(const char* name, const char* key, const char* problem) {
    throw InvalidArgumentError((std::string(name) + ": parameter '" + key + "' " + problem).c_str());
}

[[noreturn]] void throwInternal(const char* name, const std::string& problem) {
    throw InternalError((std::string(name) + ": " + problem).c_str());
}

bool validKernelSize(int64_t kernelSize) {
    return kernelSize == 1 || kernelSize == 3 || kernelSize == 5 || kernelSize == 7;
}

std::vector<double> toVector(const cv::Mat& kernel) {
    const cv::Mat contiguous = kernel.isContinuous() ? kernel : kernel.clone();
    const auto* data = contiguous.ptr<double>();
    return std::vector<double>(data, data + contiguous.total());
}

// The separable derivative kernels OpenCV defines for these orders and size,
// unnormalised: the first is applied along x, the second along y. A failure
// inside OpenCV is a defect here, since every argument has been checked.
detail::SeparableFilter derivativeFilter(const char* name, int32_t dx, int32_t dy, int32_t ksize) {
    std::vector<double> horizontal;
    std::vector<double> vertical;
    try {
        cv::Mat kx;
        cv::Mat ky;
        cv::getDerivKernels(kx, ky, dx, dy, ksize, false, CV_64F);
        horizontal = toVector(kx);
        vertical = toVector(ky);
    } catch (const cv::Exception&) {
        detail::translateCv(name);
    }
    return detail::SeparableFilter(std::move(horizontal), std::move(vertical));
}

// A derivative filter: the sum of one or more separable passes, added per
// pixel in the order given, each computed in double in the fixed order of
// SeparableFilter, written as Float32, or Float64 for Float64 input.
class DerivativeOperation : public Operation {
public:
    DerivativeOperation(const char* name, OperationParameters parameters, int32_t halo,
                        std::vector<detail::SeparableFilter> terms)
        : name_(name), parameters_(std::move(parameters)), halo_(halo), terms_(std::move(terms)) {}

    const char* name() const override { return name_; }
    ChannelArity arity() const override { return ChannelArity::PerChannel; }
    bool requiresUniformTypes() const override { return false; }
    DataTypeSet supportedTypes() const override { return detail::floatFilterTypes(); }
    Halo halo() const override { return Halo::uniform(halo_); }
    DataType outputType(DataType input) const override { return detail::floatFilterOutputType(input); }
    const OperationParameters& parameters() const override { return parameters_; }
    // The tile's values and, for a sum of passes, one pass's values (each one
    // double per tile pixel, at most 8 bytes per pixel of the halo'd rect), and
    // the samples and ring of one pass at a time; every kernel fits the halo.
    int64_t workingSetBytesPerLevelPixel() const override {
        int64_t pass = 0;
        for (const detail::SeparableFilter& term : terms_) {
            pass = std::max(pass, term.workingSetBytesPerPixel());
        }
        return (terms_.size() > 1 ? 16 : 8) + pass;
    }
    // The allocator's overhead on those buffers.
    int64_t workingSetBytesPerTile() const override {
        return ((terms_.size() > 1 ? 2 : 1) + detail::SeparableFilter::kWorkingSetBlocks) *
               detail::kAllocatorBlockOverhead;
    }

    void apply(OperationContext& context) override {
        const BlockView& in = context.input();
        BlockView& out = context.output();
        const Rect64 tile = context.tileRectAtLevel();
        if (out.planeCount() != in.planeCount()) {
            throwInternal(name_, "the output has " + std::to_string(out.planeCount()) + " planes for " +
                                     std::to_string(in.planeCount()) + " input planes");
        }
        if (out.width() != tile.width || out.height() != tile.height) {
            throwInternal(name_, "the output does not match the tile");
        }
        const size_t count = static_cast<size_t>(tile.width * tile.height);
        std::vector<double> values(count);
        std::vector<double> term(terms_.size() > 1 ? count : 0);
        for (int32_t plane = 0; plane < in.planeCount(); ++plane) {
            const DataType type = in.planeType(plane);
            if (!supportedTypes().contains(type)) {
                throw UnsupportedDataTypeError(
                    (std::string(name_) + ": input plane " + std::to_string(plane) + " has an unsupported type")
                        .c_str(),
                    type, supportedTypes());
            }
            if (out.planeType(plane) != outputType(type)) {
                throwInternal(name_, "output plane " + std::to_string(plane) + " has the wrong type");
            }
            terms_.front().apply(in, plane, context.haloRectAtLevel(), tile, values.data(), tile.width);
            for (size_t t = 1; t < terms_.size(); ++t) {
                terms_[t].apply(in, plane, context.haloRectAtLevel(), tile, term.data(), tile.width);
                for (size_t i = 0; i < count; ++i) {
                    values[i] += term[i];
                }
            }
            detail::writeFloatResult(out, plane, values.data(), tile.width);
        }
    }

private:
    const char* name_;
    OperationParameters parameters_;
    int32_t halo_;
    std::vector<detail::SeparableFilter> terms_;
};

int32_t derivativeHalo(int32_t kernelSize) {
    return std::max(1, kernelSize / 2);
}

// Reads an integer key, refusing a value beyond int32 so it cannot wrap.
int32_t readInt(detail::BagReader& bag, const char* key, int32_t fallback, const char* rule) {
    const int64_t value = bag.getInt(key, fallback);
    if (value < std::numeric_limits<int32_t>::min() || value > std::numeric_limits<int32_t>::max()) {
        bag.throwBadValue(key, rule);
    }
    return static_cast<int32_t>(value);
}

OperationPtr sobelFromParameters(const OperationParameters& parameters) {
    detail::BagReader bag(kSobel, parameters);
    const int32_t dx = readInt(bag, "dx", 1, "must be 0, 1 or 2");
    const int32_t dy = readInt(bag, "dy", 0, "must be 0, 1 or 2");
    const int32_t kernelSize = readInt(bag, "kernelSize", 3, kKernelSizeRule);
    bag.finish();
    return makeSobel(dx, dy, kernelSize);
}

OperationPtr scharrFromParameters(const OperationParameters& parameters) {
    detail::BagReader bag(kScharr, parameters);
    const int32_t dx = readInt(bag, "dx", 1, "must be 0 or 1");
    const int32_t dy = readInt(bag, "dy", 0, "must be 0 or 1");
    bag.finish();
    return makeScharr(dx, dy);
}

OperationPtr laplacianFromParameters(const OperationParameters& parameters) {
    detail::BagReader bag(kLaplacian, parameters);
    const int32_t kernelSize = readInt(bag, "kernelSize", 1, kKernelSizeRule);
    bag.finish();
    return makeLaplacian(kernelSize);
}

} // namespace

OperationPtr makeSobel(int32_t dx, int32_t dy, int32_t kernelSize) {
    if (dx < 0 || dx > 2) {
        throwParameter(kSobel, "dx", "must be 0, 1 or 2");
    }
    if (dy < 0 || dy > 2) {
        throwParameter(kSobel, "dy", "must be 0, 1 or 2");
    }
    if (dx + dy == 0) {
        throwParameter(kSobel, "dx", "and 'dy' must not both be 0");
    }
    if (!validKernelSize(kernelSize)) {
        throwParameter(kSobel, "kernelSize", kKernelSizeRule);
    }
    OperationParameters parameters;
    parameters.setInt("dx", dx).setInt("dy", dy).setInt("kernelSize", kernelSize);
    std::vector<detail::SeparableFilter> terms;
    terms.push_back(derivativeFilter(kSobel, dx, dy, kernelSize));
    return std::make_shared<DerivativeOperation>(kSobel, std::move(parameters), derivativeHalo(kernelSize),
                                                 std::move(terms));
}

OperationPtr makeScharr(int32_t dx, int32_t dy) {
    if (!((dx == 1 && dy == 0) || (dx == 0 && dy == 1))) {
        throwParameter(kScharr, "dx", "and 'dy' must select a first derivative along exactly one axis");
    }
    OperationParameters parameters;
    parameters.setInt("dx", dx).setInt("dy", dy);
    std::vector<detail::SeparableFilter> terms;
    terms.push_back(derivativeFilter(kScharr, dx, dy, cv::FILTER_SCHARR));
    return std::make_shared<DerivativeOperation>(kScharr, std::move(parameters), 1, std::move(terms));
}

OperationPtr makeLaplacian(int32_t kernelSize) {
    if (!validKernelSize(kernelSize)) {
        throwParameter(kLaplacian, "kernelSize", kKernelSizeRule);
    }
    OperationParameters parameters;
    parameters.setInt("kernelSize", kernelSize);
    // d2/dx2 first, then d2/dy2. For size 1 OpenCV's kernels are the second
    // differences [1, -2, 1] along the derivative's axis and [1] across it, so
    // the sum is the 3 x 3 kernel [[0, 1, 0], [1, -4, 1], [0, 1, 0]].
    std::vector<detail::SeparableFilter> terms;
    terms.push_back(derivativeFilter(kLaplacian, 2, 0, kernelSize));
    terms.push_back(derivativeFilter(kLaplacian, 0, 2, kernelSize));
    return std::make_shared<DerivativeOperation>(kLaplacian, std::move(parameters), derivativeHalo(kernelSize),
                                                 std::move(terms));
}

namespace detail {

OperationFactoryPtr makeSobelFactory() {
    return std::make_shared<BuiltinFactory>(
        &sobelFromParameters, "Analytic: responses to polynomial images; coefficients of OpenCV getDerivKernels",
        kComplexity);
}

OperationFactoryPtr makeScharrFactory() {
    return std::make_shared<BuiltinFactory>(
        &scharrFromParameters, "Analytic: response to a linear ramp; Scharr coefficients [3 10 3] x [-1 0 1]",
        kComplexity);
}

OperationFactoryPtr makeLaplacianFactory() {
    return std::make_shared<BuiltinFactory>(
        &laplacianFromParameters,
        "Analytic: responses to polynomial images; coefficients of OpenCV getDerivKernels", kComplexity);
}

} // namespace detail

} // namespace slideio::processor
