// Requirements: UR-PIX-5, UR-QA-2, UR-ALG-6, UR-ALG-8
#include <slideio/processor/algorithm/pixel/threshold.hpp>

#include <slideio/processor/core/error.hpp>

#include "detail/bag.hpp"
#include "detail/builtin_factory.hpp"
#include "detail/builtin_operations.hpp"
#include "detail/sample_codec.hpp"

#include <cmath>
#include <cstring>
#include <limits>
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

DataTypeSet supportedAdaptiveTypes() {
    return DataTypeSet()
        .add(DataType::Byte)
        .add(DataType::Int8)
        .add(DataType::UInt16)
        .add(DataType::Int16)
        .add(DataType::UInt32)
        .add(DataType::Int32)
        .add(DataType::Float16)
        .add(DataType::Float32)
        .add(DataType::Float64);
}

template <typename T>
double loadAs(const unsigned char* row, int64_t x) {
    T value;
    std::memcpy(&value, row + x * static_cast<int64_t>(sizeof(T)), sizeof(T));
    return static_cast<double>(value);
}

double loadHalf(const unsigned char* row, int64_t x) {
    uint16_t bits;
    std::memcpy(&bits, row + x * 2, sizeof bits);
    return detail::decodeBinary16(bits);
}

using Loader = double (*)(const unsigned char*, int64_t);

// Every supported type's samples are exact in double, so v > T compares exactly.
Loader loaderFor(DataType type) {
    switch (type) {
    case DataType::Byte: return &loadAs<uint8_t>;
    case DataType::Int8: return &loadAs<int8_t>;
    case DataType::UInt16: return &loadAs<uint16_t>;
    case DataType::Int16: return &loadAs<int16_t>;
    case DataType::UInt32: return &loadAs<uint32_t>;
    case DataType::Int32: return &loadAs<int32_t>;
    case DataType::Float16: return &loadHalf;
    case DataType::Float32: return &loadAs<float>;
    case DataType::Float64: return &loadAs<double>;
    default: return nullptr;
    }
}

class AdaptiveThresholdOperation : public Operation {
public:
    AdaptiveThresholdOperation(AdaptiveMethod method, int32_t blockSize, double offset, ThresholdDirection direction)
        : method_(method), blockSize_(blockSize), offset_(offset), direction_(direction) {
        if (method != AdaptiveMethod::Mean && method != AdaptiveMethod::Gaussian) {
            throwParameter("method", "is not an AdaptiveMethod");
        }
        if (blockSize < kMinimumBlock || blockSize > kMaximumBlock || blockSize % 2 == 0) {
            throwParameter("blockSize", "must be an odd integer from 3 to 1025");
        }
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

        // Mean weighs every pixel 1 and divides the window sum by blockSize^2;
        // Gaussian's weights are normalised to sum 1 here, once.
        weights_.assign(static_cast<size_t>(blockSize), 1.0);
        if (method == AdaptiveMethod::Gaussian) {
            const int32_t r = blockSize / 2;
            const double sigma = 0.3 * ((blockSize - 1) * 0.5 - 1.0) + 0.8;
            double sum = 0.0;
            for (int32_t d = -r; d <= r; ++d) {
                const double w = std::exp(-static_cast<double>(d) * d / (2.0 * sigma * sigma));
                weights_[static_cast<size_t>(d + r)] = w;
                sum += w;
            }
            for (double& w : weights_) {
                w /= sum;
            }
        }
    }

    const char* name() const override { return kName; }
    ChannelArity arity() const override { return ChannelArity::PerChannel; }
    bool requiresUniformTypes() const override { return false; }
    DataTypeSet supportedTypes() const override { return supportedAdaptiveTypes(); }
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
        for (int32_t plane = 0; plane < in.planeCount(); ++plane) {
            if (out.planeType(plane) != DataType::Byte) {
                throwInternal("output plane " + std::to_string(plane) + " is not Byte");
            }
            const Loader load = loaderFor(in.planeType(plane));
            if (load == nullptr) {
                throw UnsupportedDataTypeError(
                    (std::string(kName) + ": input plane " + std::to_string(plane) + " has an unsupported type")
                        .c_str(),
                    in.planeType(plane), supportedTypes());
            }
            applyPlane(in, out, plane, origin, load);
        }
    }

private:
    // Each input row's weighted window sums, H(j, x) = sum over dx of
    // w[dx] * v(j, x + dx), left to right, are kept for the blockSize rows the
    // current output row needs, in a ring. A row's sums depend on its own
    // samples alone, so sharing them between output pixels does not change any
    // pixel's result: T(x, y) is the sum over dy of w[dy] * H(y + dy, x), top to
    // bottom, exactly the per-pixel order.
    void applyPlane(const BlockView& in, BlockView& out, int32_t plane, Point64 origin, Loader load) const {
        const int64_t b = blockSize_;
        const int64_t r = b / 2;
        const int64_t width = out.width();
        const int64_t height = out.height();
        const int64_t inWidth = width + b - 1;
        const auto* inBase = static_cast<const unsigned char*>(in.planeData(plane)) +
                             origin.y * in.planeRowStrideBytes(plane);
        const int64_t inStride = in.planeRowStrideBytes(plane);
        auto* outBase = static_cast<unsigned char*>(out.planeData(plane));
        const int64_t outStride = out.planeRowStrideBytes(plane);
        const double* w = weights_.data();
        const double divisor = method_ == AdaptiveMethod::Mean ? static_cast<double>(b) * static_cast<double>(b) : 1.0;

        std::vector<double> samples(static_cast<size_t>(inWidth));
        std::vector<double> ring(static_cast<size_t>(b * width));
        const auto computeRow = [&](int64_t j) {
            const unsigned char* row = inBase + j * inStride;
            for (int64_t x = 0; x < inWidth; ++x) {
                samples[static_cast<size_t>(x)] = load(row, origin.x + x);
            }
            double* sums = ring.data() + (j % b) * width;
            for (int64_t x = 0; x < width; ++x) {
                const double* s = samples.data() + x;
                double sum = 0.0;
                for (int64_t dx = 0; dx < b; ++dx) {
                    sum += w[dx] * s[dx];
                }
                sums[x] = sum;
            }
        };

        for (int64_t j = 0; j < b - 1; ++j) {
            computeRow(j);
        }
        for (int64_t y = 0; y < height; ++y) {
            computeRow(y + b - 1);
            const unsigned char* centreRow = inBase + (y + r) * inStride;
            unsigned char* outRow = outBase + y * outStride;
            for (int64_t x = 0; x < width; ++x) {
                double sum = 0.0;
                for (int64_t dy = 0; dy < b; ++dy) {
                    sum += w[dy] * ring[static_cast<size_t>(((y + dy) % b) * width + x)];
                }
                // A NaN in the window makes t NaN, and both comparisons false.
                const double t = sum / divisor - offset_;
                const double v = load(centreRow, origin.x + x + r);
                const bool foreground = direction_ == ThresholdDirection::Above ? v > t : v < t;
                outRow[x] = foreground ? 255 : 0;
            }
        }
    }

    AdaptiveMethod method_;
    int32_t blockSize_;
    double offset_;
    ThresholdDirection direction_;
    std::vector<double> weights_;
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
