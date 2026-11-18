// Requirements: UR-PIX-3, UR-QA-2, UR-IMG-11, UR-ALG-6, UR-ALG-8
#include <slideio/processor/algorithm/pixel/filter.hpp>

#include <slideio/processor/core/error.hpp>

#include "detail/bag.hpp"
#include "detail/builtin_factory.hpp"
#include "detail/builtin_operations.hpp"
#include "detail/cv_plane.hpp"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>

namespace slideio::processor {

namespace {

constexpr const char* kName = "pixel.canny";
constexpr const char* kApertureRule = "must be 3, 5 or 7";
constexpr const char* kThresholdRule = "must be finite and at least 0";
constexpr const char* kOrderRule = "must not exceed 'highThreshold'";
// How far beyond the gradient's own neighbourhood hysteresis may follow a weak
// chain to its strong edge and still agree with single-tile Canny.
constexpr int32_t kHysteresisReach = 32;
// The largest threshold passed to cv::Canny. OpenCV converts each threshold to
// int (after dividing it by 16 at aperture 7), which is undefined beyond int's
// range, and compares it with the L1 magnitude |dx| + |dy| of two 16-bit
// signed gradients, at most 65536. 1e7, and 1e7 / 16, exceed every such
// magnitude and fit an int, so a larger threshold behaves exactly as this one.
constexpr double kThresholdCeiling = 1e7;

[[noreturn]] void throwInternal(const std::string& problem) {
    throw InternalError((std::string(kName) + ": " + problem).c_str());
}

[[noreturn]] void throwParameter(const char* key, const char* problem) {
    throw InvalidArgumentError((std::string(kName) + ": parameter '" + key + "' " + problem).c_str());
}

bool validAperture(int64_t apertureSize) {
    return apertureSize == 3 || apertureSize == 5 || apertureSize == 7;
}

bool validThreshold(double value) {
    return std::isfinite(value) && value >= 0.0;
}

// The first parameter that breaks a rule, as (key, rule); key is null when all hold.
struct Violation {
    const char* key;
    const char* rule;
};

Violation firstViolation(double lowThreshold, double highThreshold, int64_t apertureSize) {
    if (!validThreshold(lowThreshold)) {
        return {"lowThreshold", kThresholdRule};
    }
    if (!validThreshold(highThreshold)) {
        return {"highThreshold", kThresholdRule};
    }
    if (lowThreshold > highThreshold) {
        return {"lowThreshold", kOrderRule};
    }
    if (!validAperture(apertureSize)) {
        return {"apertureSize", kApertureRule};
    }
    return {nullptr, nullptr};
}

DataTypeSet cannyTypes() {
    return DataTypeSet().add(DataType::Byte);
}

class CannyOperation : public Operation {
public:
    CannyOperation(double lowThreshold, double highThreshold, int32_t apertureSize)
        : low_(lowThreshold), high_(highThreshold), aperture_(apertureSize),
          halo_(apertureSize / 2 + 1 + kHysteresisReach) {
        parameters_.setDouble("lowThreshold", lowThreshold)
            .setDouble("highThreshold", highThreshold)
            .setInt("apertureSize", apertureSize);
    }

    const char* name() const override { return kName; }
    ChannelArity arity() const override { return ChannelArity::PerChannel; }
    bool requiresUniformTypes() const override { return false; }
    DataTypeSet supportedTypes() const override { return cannyTypes(); }
    Halo halo() const override { return Halo::uniform(halo_); }
    DataType outputType(DataType) const override { return DataType::Byte; }
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
        // The tile's position within the input plane.
        const int64_t originX = tile.x - haloRect.x;
        const int64_t originY = tile.y - haloRect.y;
        if (in.width() != haloRect.width || in.height() != haloRect.height || originX < halo_ || originY < halo_ ||
            originX + tile.width + halo_ > in.width() || originY + tile.height + halo_ > in.height()) {
            throwInternal("the input does not cover the tile grown by the halo");
        }
        for (int32_t plane = 0; plane < in.planeCount(); ++plane) {
            const DataType type = in.planeType(plane);
            if (type != DataType::Byte) {
                throw UnsupportedDataTypeError(
                    (std::string(kName) + ": input plane " + std::to_string(plane) + " has an unsupported type")
                        .c_str(),
                    type, cannyTypes());
            }
            if (out.planeType(plane) != DataType::Byte) {
                throwInternal("output plane " + std::to_string(plane) + " has the wrong type");
            }
            if (tile.width <= 0 || tile.height <= 0) {
                continue;
            }
            applyPlane(in, out, plane, originX, originY);
        }
    }

private:
    // cv::Canny over the whole input plane, viewed in place, into a scratch
    // image, of which the tile is copied out. The gradient and suppression of
    // every tile pixel lie wholly inside the input, so OpenCV's own border rule
    // never reaches them; hysteresis sees the halo and no further.
    void applyPlane(const BlockView& in, BlockView& out, int32_t plane, int64_t originX, int64_t originY) const {
        cv::Mat edges;
        try {
            const cv::Mat source = detail::planeAsMat(in, plane);
            cv::Canny(source, edges, std::min(low_, kThresholdCeiling), std::min(high_, kThresholdCeiling),
                      aperture_, false);
        } catch (const cv::Exception&) {
            detail::translateCv(kName);
        }
        const int64_t width = out.width();
        auto* outBase = static_cast<unsigned char*>(out.planeData(plane));
        const int64_t outStride = out.planeRowStrideBytes(plane);
        for (int64_t y = 0; y < out.height(); ++y) {
            const unsigned char* row = edges.ptr<unsigned char>(static_cast<int>(originY + y));
            std::memcpy(outBase + y * outStride, row + originX, static_cast<size_t>(width));
        }
    }

    double low_;
    double high_;
    int32_t aperture_;
    int32_t halo_;
    OperationParameters parameters_;
};

OperationPtr cannyFromParameters(const OperationParameters& parameters) {
    detail::BagReader bag(kName, parameters);
    const double lowThreshold = bag.getDouble("lowThreshold", 50.0);
    const double highThreshold = bag.getDouble("highThreshold", 150.0);
    const int64_t apertureSize = bag.getInt("apertureSize", 3);
    const Violation violation = firstViolation(lowThreshold, highThreshold, apertureSize);
    if (violation.key != nullptr) {
        bag.throwBadValue(violation.key, violation.rule);
    }
    bag.finish();
    return makeCanny(lowThreshold, highThreshold, static_cast<int32_t>(apertureSize));
}

} // namespace

OperationPtr makeCanny(double lowThreshold, double highThreshold, int32_t apertureSize) {
    const Violation violation = firstViolation(lowThreshold, highThreshold, apertureSize);
    if (violation.key != nullptr) {
        throwParameter(violation.key, violation.rule);
    }
    return std::make_shared<CannyOperation>(lowThreshold, highThreshold, apertureSize);
}

namespace detail {

OperationFactoryPtr makeCannyFactory() {
    return std::make_shared<BuiltinFactory>(
        &cannyFromParameters,
        "Canny J. A computational approach to edge detection. IEEE TPAMI 1986;8:679-698; equals OpenCV Canny on "
        "one tile",
        "O(n) in the number of pixels; equals single-tile Canny wherever every weak edge chain reaches a strong edge "
        "within 32 pixels, and may drop or keep weak edges beyond that reach");
}

} // namespace detail

} // namespace slideio::processor
