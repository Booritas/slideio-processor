// Requirements: UR-PIX-7, UR-QA-2, UR-ALG-6
#include <slideio/processor/algorithm/pixel/filter.hpp>

#include <slideio/processor/core/error.hpp>

#include "detail/bag.hpp"
#include "detail/builtin_factory.hpp"
#include "detail/builtin_operations.hpp"
#include "detail/cv_plane.hpp"
#include "detail/sample_codec.hpp"
#include "detail/working_set.hpp"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>

namespace slideio::processor {

namespace {

constexpr const char* kName = "pixel.distance_transform";
constexpr const char* kMaximumRule = "must be from 1 to 4096";
constexpr int32_t kDefaultMaximum = 32;
constexpr int32_t kMinimumMaximum = 1;
constexpr int32_t kMaximumMaximum = 4096;

[[noreturn]] void throwInternal(const std::string& problem) {
    throw InternalError((std::string(kName) + ": " + problem).c_str());
}

bool validMaximum(int64_t maximum) {
    return maximum >= kMinimumMaximum && maximum <= kMaximumMaximum;
}

// A pixel is foreground when its sample is neither 0 nor NaN, as for
// segment.watershed.
bool foreground(double value) {
    return value != 0.0 && !std::isnan(value);
}

class DistanceTransformOperation : public Operation {
public:
    explicit DistanceTransformOperation(int32_t maximum) : maximum_(maximum) {
        parameters_.setInt("maximumDistancePx", maximum);
    }

    const char* name() const override { return kName; }
    ChannelArity arity() const override { return ChannelArity::PerChannel; }
    bool requiresUniformTypes() const override { return false; }
    DataTypeSet supportedTypes() const override { return DataTypeSet::all(); }
    Halo halo() const override { return Halo::uniform(maximum_); }
    DataType outputType(DataType) const override { return DataType::Float32; }
    const OperationParameters& parameters() const override { return parameters_; }
    // Per pixel of the halo'd rect: the Byte mask (1) and OpenCV's Float32
    // destination (4), both through cv::fastMalloc; and the buffers of OpenCV's
    // exact path, read from OpenCV 4.14.0, imgproc/src/distransform.cpp
    // (cv::distanceTransform with DIST_MASK_PRECISE calls trueDistTrans, which
    // is single threaded here), with m the rect's height and n its width:
    //  - trueDistTrans's AutoBuffer, live throughout:
    //    max(m * 2 * 4 + (3 m + 1) * 4, n * 2 * 4) = max(20 m + 4, 8 n) bytes,
    //    plus up to 3 bytes of int alignment;
    //  - while it is live, DTColumnInvoker's AutoBuffer<int>(m), 4 m bytes, and
    //    afterwards, with the column one freed, DTRowInvoker's
    //    AutoBuffer<uchar>((n + 2) * 2 * 4 + (n + 2) * 4) = 12 (n + 2) bytes.
    // The peak is max(20 m + 4, 8 n) + max(4 m, 12 n + 24) + 3, at most
    // 32 L + 31 for L = max(m, n). Both sides are at least s = 2 * maximum + 1,
    // so L / (m * n) <= 1 / s and 31 / (m * n) <= 31 / s, and that is at most
    // 63 / s bytes per pixel, taken rounded up.
    int64_t workingSetBytesPerLevelPixel() const override {
        const int64_t side = 2 * static_cast<int64_t>(maximum_) + 1;
        return 5 + (63 + side - 1) / side;
    }
    // The Mats' bookkeeping (mask, destination), and cv::fastMalloc's alignment
    // of its five blocks (mask, destination and the three AutoBuffers): up to 63
    // bytes of rounding and the original pointer's 8 on each, 128 bounds that.
    int64_t workingSetBytesPerTile() const override {
        return 2 * detail::kCvMatBookkeeping + 5 * 128;
    }

    void apply(OperationContext& context) override {
        detail::useSequentialOpenCv(kName);
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
        const int64_t originX = tile.x - haloRect.x;
        const int64_t originY = tile.y - haloRect.y;
        if (originX < 0 || originY < 0 || in.width() != haloRect.width || in.height() != haloRect.height ||
            originX + tile.width > in.width() || originY + tile.height > in.height()) {
            throwInternal("the input does not cover the tile");
        }
        for (int32_t plane = 0; plane < in.planeCount(); ++plane) {
            const DataType type = in.planeType(plane);
            if (!supportedTypes().contains(type)) {
                throw UnsupportedDataTypeError(
                    (std::string(kName) + ": input plane " + std::to_string(plane) + " has an unsupported type")
                        .c_str(),
                    type, supportedTypes());
            }
            if (out.planeType(plane) != DataType::Float32) {
                throwInternal("output plane " + std::to_string(plane) + " has the wrong type");
            }
            if (tile.width <= 0 || tile.height <= 0) {
                continue;
            }
            transformPlane(in, out, plane, originX, originY, tile.width, tile.height);
        }
    }

private:
    void transformPlane(const BlockView& in, BlockView& out, int32_t plane, int64_t originX, int64_t originY,
                        int64_t width, int64_t height) const {
        const DataType type = in.planeType(plane);
        const int64_t inWidth = in.width();
        const int64_t inHeight = in.height();
        const auto* inBase = static_cast<const unsigned char*>(in.planeData(plane));
        const int64_t inStride = in.planeRowStrideBytes(plane);
        const float cap = static_cast<float>(maximum_);

        // The mask of the whole halo'd input: 255 where foreground.
        bool anyBackground = false;
        cv::Mat distance;
        try {
            cv::Mat mask(static_cast<int>(inHeight), static_cast<int>(inWidth), CV_8U);
            for (int64_t y = 0; y < inHeight; ++y) {
                const unsigned char* row = inBase + y * inStride;
                unsigned char* maskRow = mask.ptr<unsigned char>(static_cast<int>(y));
                for (int64_t x = 0; x < inWidth; ++x) {
                    const bool isForeground = foreground(detail::sampleAsDouble(detail::readSample(row, x, type)));
                    maskRow[x] = isForeground ? 255 : 0;
                    anyBackground = anyBackground || !isForeground;
                }
            }
            if (anyBackground) {
                cv::distanceTransform(mask, distance, cv::DIST_L2, cv::DIST_MASK_PRECISE, CV_32F);
            }
        } catch (const cv::Exception&) {
            detail::translateCv(kName);
        }

        auto* outBase = static_cast<unsigned char*>(out.planeData(plane));
        const int64_t outStride = out.planeRowStrideBytes(plane);
        for (int64_t y = 0; y < height; ++y) {
            auto* outRow = outBase + y * outStride;
            // With no background in the context every pixel is further than the
            // maximum, so the cap is the answer and no OpenCV call is needed.
            const float* distanceRow =
                anyBackground ? distance.ptr<float>(static_cast<int>(originY + y)) + originX : nullptr;
            for (int64_t x = 0; x < width; ++x) {
                const float value = distanceRow != nullptr ? std::min(distanceRow[x], cap) : cap;
                std::memcpy(outRow + x * static_cast<int64_t>(sizeof(float)), &value, sizeof(float));
            }
        }
    }

    int32_t maximum_;
    OperationParameters parameters_;
};

OperationPtr distanceTransformFromParameters(const OperationParameters& parameters) {
    detail::BagReader bag(kName, parameters);
    const int64_t maximum = bag.getInt("maximumDistancePx", kDefaultMaximum);
    if (!validMaximum(maximum)) {
        bag.throwBadValue("maximumDistancePx", kMaximumRule);
    }
    bag.finish();
    return makeDistanceTransform(static_cast<int32_t>(maximum));
}

} // namespace

OperationPtr makeDistanceTransform(int32_t maximumDistancePx) {
    if (!validMaximum(maximumDistancePx)) {
        throw InvalidArgumentError(
            (std::string(kName) + ": parameter 'maximumDistancePx' " + kMaximumRule).c_str());
    }
    return std::make_shared<DistanceTransformOperation>(maximumDistancePx);
}

namespace detail {

OperationFactoryPtr makeDistanceTransformFactory() {
    return std::make_shared<BuiltinFactory>(
        &distanceTransformFromParameters,
        "Published definition: exact Euclidean distance (Felzenszwalb and Huttenlocher); checked against brute "
        "force",
        "O(n) in the number of samples of the halo'd tile");
}

} // namespace detail

} // namespace slideio::processor
