// Requirements: UR-PIX-3, UR-QA-2, UR-IMG-11, UR-ALG-6
#include <slideio/processor/algorithm/pixel/filter.hpp>

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

constexpr const char* kName = "pixel.bilateral_filter";
constexpr int32_t kMinimumDiameter = 3;
constexpr int32_t kMaximumDiameter = 255;

[[noreturn]] void throwParameter(const char* key, const char* problem) {
    throw InvalidArgumentError((std::string(kName) + ": parameter '" + key + "' " + problem).c_str());
}

[[noreturn]] void throwInternal(const std::string& problem) {
    throw InternalError((std::string(kName) + ": " + problem).c_str());
}

bool isValidDiameter(int64_t diameter) noexcept {
    return diameter >= kMinimumDiameter && diameter <= kMaximumDiameter && diameter % 2 != 0;
}

int32_t checkedDiameter(int32_t diameter) {
    if (!isValidDiameter(diameter)) {
        throwParameter("diameter", "must be an odd integer from 3 to 255");
    }
    return diameter;
}

double checkedSigma(const char* key, double sigma) {
    if (!std::isfinite(sigma) || !(sigma > 0.0)) {
        throwParameter(key, "must be finite and greater than 0");
    }
    return sigma;
}

// One point of the disc and its spatial weight exp(-(dx^2 + dy^2) / (2 sigmaSpace^2)).
struct DiscPoint {
    int64_t dx;
    int64_t dy;
    double spatial;
};

// The disc dx^2 + dy^2 <= r^2 of radius diameter / 2, row by row (dy
// ascending), left to right (dx ascending): the order every output sample's
// sums are formed in.
std::vector<DiscPoint> discPoints(int32_t diameter, double sigmaSpace) {
    const int64_t r = diameter / 2;
    const double spaceDenominator = 2.0 * sigmaSpace * sigmaSpace;
    std::vector<DiscPoint> points;
    for (int64_t dy = -r; dy <= r; ++dy) {
        for (int64_t dx = -r; dx <= r; ++dx) {
            if (dx * dx + dy * dy > r * r) {
                continue;
            }
            const double spatial = std::exp(-static_cast<double>(dx * dx + dy * dy) / spaceDenominator);
            points.push_back({dx, dy, spatial});
        }
    }
    return points;
}

class BilateralFilterOperation : public Operation {
public:
    BilateralFilterOperation(int32_t diameter, double sigmaColour, double sigmaSpace)
        : radius_(checkedDiameter(diameter) / 2),
          colourDenominator_(2.0 * checkedSigma("sigmaColour", sigmaColour) * sigmaColour),
          points_(discPoints(diameter, checkedSigma("sigmaSpace", sigmaSpace))) {
        parameters_.setInt("diameter", diameter)
            .setDouble("sigmaColour", sigmaColour)
            .setDouble("sigmaSpace", sigmaSpace);
    }

    const char* name() const override { return kName; }
    ChannelArity arity() const override { return ChannelArity::PerChannel; }
    bool requiresUniformTypes() const override { return false; }
    DataTypeSet supportedTypes() const override { return detail::floatFilterTypes(); }
    Halo halo() const override { return Halo::uniform(radius_); }
    DataType outputType(DataType input) const override { return detail::floatFilterOutputType(input); }
    const OperationParameters& parameters() const override { return parameters_; }

    void apply(OperationContext& context) override {
        const BlockView& in = context.input();
        BlockView& out = context.output();
        const Rect64 tile = context.tileRectAtLevel();
        const Rect64 haloRect = context.haloRectAtLevel();
        if (out.planeCount() != in.planeCount()) {
            throwInternal("the output has " + std::to_string(out.planeCount()) + " planes for " +
                          std::to_string(in.planeCount()) + " input planes");
        }
        if (out.width() != tile.width || out.height() != tile.height) {
            throwInternal("the output does not match the tile");
        }
        const int64_t r = radius_;
        // The tile grown by the radius starts at origin within the input.
        const int64_t originX = tile.x - r - haloRect.x;
        const int64_t originY = tile.y - r - haloRect.y;
        const int64_t inWidth = tile.width + 2 * r;
        const int64_t inHeight = tile.height + 2 * r;
        if (originX < 0 || originY < 0 || in.width() != haloRect.width || in.height() != haloRect.height ||
            originX + inWidth > in.width() || originY + inHeight > in.height()) {
            throwInternal("the input does not cover the tile grown by the radius");
        }
        std::vector<double> samples(static_cast<size_t>(inWidth * inHeight));
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
            const auto* base = static_cast<const unsigned char*>(in.planeData(plane));
            const int64_t stride = in.planeRowStrideBytes(plane);
            for (int64_t j = 0; j < inHeight; ++j) {
                const unsigned char* row = base + (originY + j) * stride;
                double* target = samples.data() + j * inWidth;
                for (int64_t i = 0; i < inWidth; ++i) {
                    target[i] = detail::sampleAsDouble(detail::readSample(row, originX + i, type));
                }
            }
            filterPlane(samples.data(), inWidth, tile.width, tile.height, values.data());
            detail::writeFloatResult(out, plane, values.data(), tile.width);
        }
    }

private:
    // Filters a (width + 2r) x (height + 2r) block of samples, rows @p inWidth
    // apart, into width x height values. Each output's sums run over the disc
    // in its fixed order, so a value depends on its own window alone.
    void filterPlane(const double* samples, int64_t inWidth, int64_t width, int64_t height, double* values) const {
        const int64_t r = radius_;
        const double colourDenominator = colourDenominator_;
        for (int64_t y = 0; y < height; ++y) {
            for (int64_t x = 0; x < width; ++x) {
                const double* centre = samples + (y + r) * inWidth + (x + r);
                const double c = *centre;
                // A centre that is not finite (NaN or an infinity) is written
                // unchanged; a neighbour that is not finite is skipped, so it
                // never spreads into its finite neighbours.
                if (!std::isfinite(c)) {
                    values[y * width + x] = c;
                    continue;
                }
                // Shaped as the direct reference computation in the tests, so
                // that, with no multiply-add contraction, both agree bit for bit.
                double sum = 0.0;
                double norm = 0.0;
                for (const DiscPoint& p : points_) {
                    const double s = centre[p.dy * inWidth + p.dx];
                    if (!std::isfinite(s)) {
                        continue;
                    }
                    const double d = s - c;
                    const double weight = p.spatial * std::exp(-(d * d) / colourDenominator);
                    sum += weight * s;
                    norm += weight;
                }
                values[y * width + x] = sum / norm;
            }
        }
    }

    int32_t radius_;
    double colourDenominator_;
    std::vector<DiscPoint> points_;
    OperationParameters parameters_;
};

OperationPtr bilateralFilterFromParameters(const OperationParameters& parameters) {
    detail::BagReader bag(kName, parameters);
    const int64_t diameter = bag.getInt("diameter", 5);
    const double sigmaColour = bag.getDouble("sigmaColour", 25.0);
    const double sigmaSpace = bag.getDouble("sigmaSpace", 5.0);
    if (!isValidDiameter(diameter)) {
        bag.throwBadValue("diameter", "must be an odd integer from 3 to 255");
    }
    bag.finish();
    return makeBilateralFilter(static_cast<int32_t>(diameter), sigmaColour, sigmaSpace);
}

} // namespace

OperationPtr makeBilateralFilter(int32_t diameter, double sigmaColour, double sigmaSpace) {
    return std::make_shared<BilateralFilterOperation>(diameter, sigmaColour, sigmaSpace);
}

namespace detail {

OperationFactoryPtr makeBilateralFilterFactory() {
    return std::make_shared<BuiltinFactory>(&bilateralFilterFromParameters,
                                            "Tomasi C, Manduchi R. Bilateral filtering for gray and color images. "
                                            "ICCV 1998; checked against a direct per-pixel computation",
                                            "O(n * diameter^2) in the number of samples");
}

} // namespace detail

} // namespace slideio::processor
