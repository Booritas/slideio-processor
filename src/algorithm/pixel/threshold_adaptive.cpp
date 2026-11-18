// Requirements: UR-PIX-5, UR-QA-2, UR-ALG-6, UR-ALG-8
#include <slideio/processor/algorithm/pixel/threshold.hpp>

#include <slideio/processor/core/error.hpp>

#include "detail/bag.hpp"
#include "detail/builtin_factory.hpp"
#include "detail/builtin_operations.hpp"
#include "detail/sample_codec.hpp"
#include "detail/separable.hpp"

#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace slideio::processor {

namespace {

constexpr const char* kName = "pixel.threshold_adaptive";
constexpr const char* const kMethodSpellings[] = {"Mean", "Gaussian"};
constexpr const char* const kDirectionSpellings[] = {"Above", "Below"};
constexpr int32_t kMinimumBlock = 3;
constexpr int32_t kMaximumBlock = 1025;

[[noreturn]] void throwParameter(const char* key, const char* problem) {
    throw InvalidArgumentError((std::string(kName) + ": parameter '" + key + "' " + problem).c_str());
}

[[noreturn]] void throwInternal(const std::string& problem) {
    throw InternalError((std::string(kName) + ": " + problem).c_str());
}

double asDouble(const detail::Sample& sample) noexcept {
    return sample.kind == detail::Sample::Kind::Float ? sample.f : static_cast<double>(sample.s);
}

// Mean weighs every pixel 1 (the window sum is divided by blockSize^2 when the
// threshold is formed); Gaussian's weights are normalised to sum 1 here, once.
std::vector<double> windowWeights(AdaptiveMethod method, int32_t blockSize) {
    std::vector<double> weights(static_cast<size_t>(blockSize), 1.0);
    if (method == AdaptiveMethod::Gaussian) {
        const int32_t r = blockSize / 2;
        const double sigma = 0.3 * ((blockSize - 1) * 0.5 - 1.0) + 0.8;
        double sum = 0.0;
        for (int32_t d = -r; d <= r; ++d) {
            const double w = std::exp(-static_cast<double>(d) * d / (2.0 * sigma * sigma));
            weights[static_cast<size_t>(d + r)] = w;
            sum += w;
        }
        for (double& w : weights) {
            w /= sum;
        }
    }
    return weights;
}

class AdaptiveThresholdOperation : public Operation {
public:
    AdaptiveThresholdOperation(AdaptiveMethod method, int32_t blockSize, double offset, ThresholdDirection direction)
        : method_(checkedMethod(method)), blockSize_(checkedBlockSize(blockSize)), offset_(offset),
          direction_(direction), filter_(windowWeights(method_, blockSize_), windowWeights(method_, blockSize_)) {
        if (!std::isfinite(offset)) {
            throwParameter("offset", "must be finite");
        }
        if (direction != ThresholdDirection::Above && direction != ThresholdDirection::Below) {
            throwParameter("direction", "is not a ThresholdDirection");
        }
        parameters_.setString("method", kMethodSpellings[static_cast<int32_t>(method)])
            .setInt("blockSize", blockSize)
            .setDouble("offset", offset)
            .setString("direction", kDirectionSpellings[static_cast<int32_t>(direction)]);
    }

    const char* name() const override { return kName; }
    ChannelArity arity() const override { return ChannelArity::PerChannel; }
    bool requiresUniformTypes() const override { return false; }
    DataTypeSet supportedTypes() const override { return detail::floatFilterTypes(); }
    Halo halo() const override { return Halo::uniform(blockSize_ / 2); }
    DataType outputType(DataType) const override { return DataType::Byte; }
    const OperationParameters& parameters() const override { return parameters_; }

    void apply(OperationContext& context) override {
        const BlockView& in = context.input();
        BlockView& out = context.output();
        if (out.planeCount() != in.planeCount()) {
            throwInternal("the output has " + std::to_string(out.planeCount()) + " planes for " +
                          std::to_string(in.planeCount()) + " input planes");
        }
        // The input covers the halo rect, which must hold the tile grown by
        // the halo on every side; origin is where the first window starts.
        const Rect64 tile = context.tileRectAtLevel();
        const Rect64 haloRect = context.haloRectAtLevel();
        const int64_t r = blockSize_ / 2;
        const Point64 origin{tile.x - r - haloRect.x, tile.y - r - haloRect.y};
        if (origin.x < 0 || origin.y < 0 || in.width() != haloRect.width || in.height() != haloRect.height ||
            origin.x + tile.width + 2 * r > in.width() || origin.y + tile.height + 2 * r > in.height() ||
            out.width() != tile.width || out.height() != tile.height) {
            throwInternal("the input does not cover the tile grown by the halo");
        }
        std::vector<double> means(static_cast<size_t>(tile.width * tile.height));
        for (int32_t plane = 0; plane < in.planeCount(); ++plane) {
            if (out.planeType(plane) != DataType::Byte) {
                throwInternal("output plane " + std::to_string(plane) + " is not Byte");
            }
            const DataType type = in.planeType(plane);
            if (!supportedTypes().contains(type)) {
                throw UnsupportedDataTypeError(
                    (std::string(kName) + ": input plane " + std::to_string(plane) + " has an unsupported type")
                        .c_str(),
                    type, supportedTypes());
            }
            filter_.apply(in, plane, haloRect, tile, means.data(), tile.width);
            applyPlane(in, out, plane, origin, means.data());
        }
    }

private:
    static AdaptiveMethod checkedMethod(AdaptiveMethod method) {
        if (method != AdaptiveMethod::Mean && method != AdaptiveMethod::Gaussian) {
            throwParameter("method", "is not an AdaptiveMethod");
        }
        return method;
    }

    static int32_t checkedBlockSize(int32_t blockSize) {
        if (blockSize < kMinimumBlock || blockSize > kMaximumBlock || blockSize % 2 == 0) {
            throwParameter("blockSize", "must be an odd integer from 3 to 1025");
        }
        return blockSize;
    }

    // T(x, y) is the window's weighted sum, formed by the separable filter in
    // its fixed order, divided by blockSize^2 for Mean, minus the offset. Every
    // supported type's samples are exact in double, so v > T compares exactly.
    void applyPlane(const BlockView& in, BlockView& out, int32_t plane, Point64 origin, const double* sums) const {
        const int64_t b = blockSize_;
        const int64_t r = b / 2;
        const int64_t width = out.width();
        const int64_t height = out.height();
        const DataType type = in.planeType(plane);
        const auto* inBase = static_cast<const unsigned char*>(in.planeData(plane));
        const int64_t inStride = in.planeRowStrideBytes(plane);
        auto* outBase = static_cast<unsigned char*>(out.planeData(plane));
        const int64_t outStride = out.planeRowStrideBytes(plane);
        const double divisor = method_ == AdaptiveMethod::Mean ? static_cast<double>(b) * static_cast<double>(b) : 1.0;
        for (int64_t y = 0; y < height; ++y) {
            const unsigned char* centreRow = inBase + (origin.y + y + r) * inStride;
            unsigned char* outRow = outBase + y * outStride;
            const double* sumRow = sums + y * width;
            for (int64_t x = 0; x < width; ++x) {
                // A NaN in the window makes t NaN, and both comparisons false.
                const double t = sumRow[x] / divisor - offset_;
                const double v = asDouble(detail::readSample(centreRow, origin.x + x + r, type));
                const bool foreground = direction_ == ThresholdDirection::Above ? v > t : v < t;
                outRow[x] = foreground ? 255 : 0;
            }
        }
    }

    AdaptiveMethod method_;
    int32_t blockSize_;
    double offset_;
    ThresholdDirection direction_;
    detail::SeparableFilter filter_;
    OperationParameters parameters_;
};

OperationPtr adaptiveThresholdFromParameters(const OperationParameters& parameters) {
    detail::BagReader bag(kName, parameters);
    const AdaptiveMethod method = bag.getEnum("method", AdaptiveMethod::Mean, kMethodSpellings, 2);
    const int64_t blockSize = bag.getInt("blockSize", 11);
    if (blockSize < kMinimumBlock || blockSize > kMaximumBlock || blockSize % 2 == 0) {
        bag.throwBadValue("blockSize", "must be an odd integer from 3 to 1025");
    }
    const double offset = bag.getDouble("offset", 2.0);
    if (!std::isfinite(offset)) {
        bag.throwBadValue("offset", "must be finite");
    }
    const ThresholdDirection direction = bag.getEnum("direction", ThresholdDirection::Above, kDirectionSpellings, 2);
    bag.finish();
    return makeAdaptiveThreshold(method, static_cast<int32_t>(blockSize), offset, direction);
}

} // namespace

OperationPtr makeAdaptiveThreshold(AdaptiveMethod method, int32_t blockSize, double offset,
                                   ThresholdDirection direction) {
    return std::make_shared<AdaptiveThresholdOperation>(method, blockSize, offset, direction);
}

namespace detail {

OperationFactoryPtr makeAdaptiveThresholdFactory() {
    return std::make_shared<BuiltinFactory>(
        &adaptiveThresholdFromParameters,
        "Analytic: the threshold at each pixel is its window mean (or Gaussian-weighted mean) minus the offset, "
        "compared with a direct per-pixel computation",
        "O(n * blockSize) in the number of samples");
}

} // namespace detail

} // namespace slideio::processor
