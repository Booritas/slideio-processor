// Requirements: UR-PIX-3, UR-QA-2, UR-IMG-11, UR-ALG-6
#include <slideio/processor/algorithm/pixel/filter.hpp>

#include <slideio/processor/core/error.hpp>

#include "detail/bag.hpp"
#include "detail/builtin_factory.hpp"
#include "detail/builtin_operations.hpp"
#include "detail/separable.hpp"
#include "detail/working_set.hpp"

#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace slideio::processor {

namespace {

constexpr const char* kName = "pixel.gaussian_blur";
constexpr int32_t kMaximumKernel = 1025;
// The smallest sigma accepted. Below it 2 * sigma^2 may underflow to 0 and
// every weight become 0 / 0; at it every off-centre weight is already 0.
constexpr double kMinimumSigma = 1e-6;

[[noreturn]] void throwParameter(const char* key, const char* problem) {
    throw InvalidArgumentError((std::string(kName) + ": parameter '" + key + "' " + problem).c_str());
}

[[noreturn]] void throwInternal(const std::string& problem) {
    throw InternalError((std::string(kName) + ": " + problem).c_str());
}

// The kernel size the parameters give: kernelSize itself, or for 0 the size
// 2 * ceil(3 * sigma) + 1 derived from sigma. Throws for a value out of range.
int32_t checkedKernelSize(double sigma, int32_t kernelSize) {
    if (!std::isfinite(sigma) || !(sigma >= kMinimumSigma)) {
        throwParameter("sigma", "must be finite and at least 1e-6");
    }
    if (kernelSize == 0) {
        // Compared before the conversion, so a large sigma cannot overflow it.
        const double reach = std::ceil(3.0 * sigma);
        if (reach > (kMaximumKernel - 1) / 2) {
            throwParameter("sigma", "with kernelSize 0 must derive a kernel no larger than 1025");
        }
        return 2 * static_cast<int32_t>(reach) + 1;
    }
    if (kernelSize < 1 || kernelSize > kMaximumKernel || kernelSize % 2 == 0) {
        throwParameter("kernelSize", "must be an odd integer from 1 to 1025, or 0");
    }
    return kernelSize;
}

// exp(-d^2 / 2 sigma^2) for d from -size / 2 to size / 2, normalised to sum 1.
std::vector<double> gaussianKernel(double sigma, int32_t size) {
    const int32_t r = size / 2;
    std::vector<double> weights(static_cast<size_t>(size));
    double sum = 0.0;
    for (int32_t d = -r; d <= r; ++d) {
        const double w = std::exp(-static_cast<double>(d) * d / (2.0 * sigma * sigma));
        weights[static_cast<size_t>(d + r)] = w;
        sum += w;
    }
    for (double& w : weights) {
        w /= sum;
    }
    return weights;
}

class GaussianBlurOperation : public Operation {
public:
    GaussianBlurOperation(double sigma, int32_t kernelSize)
        : size_(checkedKernelSize(sigma, kernelSize)),
          filter_(gaussianKernel(sigma, size_), gaussianKernel(sigma, size_)) {
        parameters_.setDouble("sigma", sigma).setInt("kernelSize", kernelSize);
    }

    const char* name() const override { return kName; }
    ChannelArity arity() const override { return ChannelArity::PerChannel; }
    bool requiresUniformTypes() const override { return false; }
    DataTypeSet supportedTypes() const override { return detail::floatFilterTypes(); }
    Halo halo() const override { return Halo::uniform(size_ / 2); }
    DataType outputType(DataType input) const override { return detail::floatFilterOutputType(input); }
    const OperationParameters& parameters() const override { return parameters_; }
    // The tile's values (one double per tile pixel, at most 8 bytes per pixel
    // of the halo'd rect) and the separable filter's samples and ring.
    int64_t workingSetBytesPerLevelPixel() const override { return 8 + filter_.workingSetBytesPerPixel(); }
    // The allocator's overhead on those three buffers.
    int64_t workingSetBytesPerTile() const override {
        return (1 + detail::SeparableFilter::kWorkingSetBlocks) * detail::kAllocatorBlockOverhead;
    }

    void apply(OperationContext& context) override {
        const BlockView& in = context.input();
        BlockView& out = context.output();
        const Rect64 tile = context.tileRectAtLevel();
        if (out.planeCount() != in.planeCount()) {
            throwInternal("the output has " + std::to_string(out.planeCount()) + " planes for " +
                          std::to_string(in.planeCount()) + " input planes");
        }
        if (out.width() != tile.width || out.height() != tile.height) {
            throwInternal("the output does not match the tile");
        }
        std::vector<double> values(static_cast<size_t>(tile.width * tile.height));
        for (int32_t plane = 0; plane < in.planeCount(); ++plane) {
            const DataType type = in.planeType(plane);
            if (!supportedTypes().contains(type)) {
                throw UnsupportedDataTypeError(
                    (std::string(kName) + ": input plane " + std::to_string(plane) + " has an unsupported type")
                        .c_str(),
                    type, supportedTypes());
            }
            if (out.planeType(plane) != outputType(type)) {
                throwInternal("output plane " + std::to_string(plane) + " has the wrong type");
            }
            filter_.apply(in, plane, context.haloRectAtLevel(), tile, values.data(), tile.width);
            detail::writeFloatResult(out, plane, values.data(), tile.width);
        }
    }

private:
    int32_t size_;
    detail::SeparableFilter filter_;
    OperationParameters parameters_;
};

OperationPtr gaussianBlurFromParameters(const OperationParameters& parameters) {
    detail::BagReader bag(kName, parameters);
    const double sigma = bag.getDouble("sigma", 1.0);
    const int64_t kernelSize = bag.getInt("kernelSize", 0);
    if (kernelSize < 0 || kernelSize > kMaximumKernel || (kernelSize != 0 && kernelSize % 2 == 0)) {
        bag.throwBadValue("kernelSize", "must be an odd integer from 1 to 1025, or 0");
    }
    bag.finish();
    return makeGaussianBlur(sigma, static_cast<int32_t>(kernelSize));
}

} // namespace

OperationPtr makeGaussianBlur(double sigma, int32_t kernelSize) {
    return std::make_shared<GaussianBlurOperation>(sigma, kernelSize);
}

namespace detail {

OperationFactoryPtr makeGaussianBlurFactory() {
    return std::make_shared<BuiltinFactory>(
        &gaussianBlurFromParameters,
        "Analytic: a normalised Gaussian kernel's response to constant, linear and impulse images",
        "O(n * kernelSize) in the number of samples");
}

} // namespace detail

} // namespace slideio::processor
