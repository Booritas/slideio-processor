// Requirements: UR-PIX-3, UR-QA-2, UR-IMG-11, UR-ALG-6
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
#include <utility>
#include <vector>

namespace slideio::processor {

namespace {

constexpr const char* kName = "pixel.median_blur";
constexpr const char* kKernelSizeRule = "must be an odd integer from 3 to 255";
constexpr int32_t kMinimumKernel = 3;
constexpr int32_t kMaximumKernel = 255;

[[noreturn]] void throwInternal(const std::string& problem) {
    throw InternalError((std::string(kName) + ": " + problem).c_str());
}

bool validKernelSize(int64_t kernelSize) {
    return kernelSize >= kMinimumKernel && kernelSize <= kMaximumKernel && kernelSize % 2 == 1;
}

// Where the tile's first window starts within the input plane, and the
// tile's shape.
struct Geometry {
    int64_t originX; // input column of the first window's left edge
    int64_t originY; // input row of the first window's top edge
    int64_t width;   // tile width
    int64_t height;  // tile height
};

// The sort keys of the three sample kinds. A float key orders every number by
// value and puts NaN above every number; NaNs are equivalent to each other.
struct SignedLess {
    bool operator()(const std::pair<int64_t, int64_t>& a, const std::pair<int64_t, int64_t>& b) const {
        return a.first < b.first;
    }
};
struct UnsignedLess {
    bool operator()(const std::pair<uint64_t, int64_t>& a, const std::pair<uint64_t, int64_t>& b) const {
        return a.first < b.first;
    }
};
struct FloatLess {
    bool operator()(const std::pair<double, int64_t>& a, const std::pair<double, int64_t>& b) const {
        return !std::isnan(a.first) && (std::isnan(b.first) || a.first < b.first);
    }
};

int64_t signedKey(const detail::Sample& s) {
    return s.s;
}
uint64_t unsignedKey(const detail::Sample& s) {
    return s.u;
}
double floatKey(const detail::Sample& s) {
    return s.f;
}

// The module's own exact median of one plane: every input sample is decoded
// once into its key, then for each output pixel the window's keys, each with
// the position of its sample, are copied and std::nth_element selects the
// middle one. The selected sample's bytes are copied unchanged, so the result
// is an existing sample bit for bit (a Float16 or a NaN keeps its pattern).
// The window is copied in the same row-major order for every pixel, so the
// choice among equal keys (signed zeros, NaNs) does not depend on the tiling.
template <typename Key, typename Less>
void ownMedian(const BlockView& in, BlockView& out, int32_t plane, const Geometry& g, int32_t k,
               Key (*keyOf)(const detail::Sample&)) {
    const DataType type = in.planeType(plane);
    const size_t sampleBytes = static_cast<size_t>(dataTypeSizeBytes(type));
    const int64_t inWidth = in.width();
    const int64_t inHeight = in.height();
    const auto* inBase = static_cast<const unsigned char*>(in.planeData(plane));
    const int64_t inStride = in.planeRowStrideBytes(plane);

    std::vector<Key> keys(static_cast<size_t>(inWidth * inHeight));
    for (int64_t y = 0; y < inHeight; ++y) {
        const unsigned char* row = inBase + y * inStride;
        for (int64_t x = 0; x < inWidth; ++x) {
            keys[static_cast<size_t>(y * inWidth + x)] = keyOf(detail::readSample(row, x, type));
        }
    }

    auto* outBase = static_cast<unsigned char*>(out.planeData(plane));
    const int64_t outStride = out.planeRowStrideBytes(plane);
    const size_t count = static_cast<size_t>(k) * static_cast<size_t>(k);
    std::vector<std::pair<Key, int64_t>> window(count);
    const auto middle = window.begin() + static_cast<std::ptrdiff_t>(count / 2);
    for (int64_t y = 0; y < g.height; ++y) {
        unsigned char* outRow = outBase + y * outStride;
        for (int64_t x = 0; x < g.width; ++x) {
            size_t i = 0;
            for (int64_t wy = 0; wy < k; ++wy) {
                const int64_t rowStart = (g.originY + y + wy) * inWidth + g.originX + x;
                for (int64_t wx = 0; wx < k; ++wx) {
                    const int64_t position = rowStart + wx;
                    window[i++] = {keys[static_cast<size_t>(position)], position};
                }
            }
            std::nth_element(window.begin(), middle, window.end(), Less());
            const int64_t position = middle->second;
            const unsigned char* source =
                inBase + (position / inWidth) * inStride + (position % inWidth) * static_cast<int64_t>(sampleBytes);
            std::memcpy(outRow + x * static_cast<int64_t>(sampleBytes), source, sampleBytes);
        }
    }
}

// OpenCV's median of a Byte plane: cv::medianBlur over the whole input plane,
// viewed in place, into a scratch image, of which the tile is copied out. The
// tile's windows lie wholly inside the input, so OpenCV's own border rule never
// reaches the copied pixels.
void byteMedian(const BlockView& in, BlockView& out, int32_t plane, const Geometry& g, int32_t k) {
    const int64_t r = k / 2;
    cv::Mat median;
    try {
        const cv::Mat source = detail::planeAsMat(in, plane);
        cv::medianBlur(source, median, k);
    } catch (const cv::Exception&) {
        detail::translateCv(kName);
    }
    auto* outBase = static_cast<unsigned char*>(out.planeData(plane));
    const int64_t outStride = out.planeRowStrideBytes(plane);
    for (int64_t y = 0; y < g.height; ++y) {
        const unsigned char* row = median.ptr<unsigned char>(static_cast<int>(g.originY + r + y));
        std::memcpy(outBase + y * outStride, row + g.originX + r, static_cast<size_t>(g.width));
    }
}

class MedianBlurOperation : public Operation {
public:
    explicit MedianBlurOperation(int32_t kernelSize) : size_(kernelSize) {
        detail::useSequentialOpenCv();
        parameters_.setInt("kernelSize", kernelSize);
    }

    const char* name() const override { return kName; }
    ChannelArity arity() const override { return ChannelArity::PerChannel; }
    bool requiresUniformTypes() const override { return false; }
    DataTypeSet supportedTypes() const override { return DataTypeSet::all(); }
    Halo halo() const override { return Halo::uniform(size_ / 2); }
    DataType outputType(DataType input) const override { return input; }
    const OperationParameters& parameters() const override { return parameters_; }
    // The module's own path: one 8-byte key per pixel of the halo'd rect. The
    // Byte path (OpenCV 4.14, imgproc/src/median_blur.dispatch.cpp, cv::medianBlur;
    // median_blur.simd.hpp, medianBlur): the halo'd-size destination, and for a
    // kernel above 5 the plane with k / 2 border columns either side
    // (copyMakeBorder), at most 3 bytes per pixel, since k - 1 < the halo'd width.
    int64_t workingSetBytesPerLevelPixel() const override { return 8; }
    // The larger of the own path's window, k * k (key, position) pairs, with the
    // allocator's overhead on it and on the keys, and the Byte path's histograms
    // (median_blur.simd.hpp, medianBlur_8u_O1: 2 * (272 * (min(width, 512) + k - 1)
    // + 128) bytes through operator new) with the Mats' bookkeeping.
    int64_t workingSetBytesPerTile() const override {
        const int64_t k = size_;
        const int64_t own = 16 * k * k + 2 * detail::kAllocatorBlockOverhead;
        const int64_t byte = (k > 5 ? 2 * (272 * (512 + k - 1) + 128) : 0) + 2 * detail::kCvMatBookkeeping;
        return std::max(own, byte);
    }

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
        const int64_t r = size_ / 2;
        const Geometry g{tile.x - r - haloRect.x, tile.y - r - haloRect.y, tile.width, tile.height};
        if (g.originX < 0 || g.originY < 0 || in.width() != haloRect.width || in.height() != haloRect.height ||
            g.originX + g.width + 2 * r > in.width() || g.originY + g.height + 2 * r > in.height()) {
            throwInternal("the input does not cover the tile grown by the kernel");
        }
        for (int32_t plane = 0; plane < in.planeCount(); ++plane) {
            const DataType type = in.planeType(plane);
            if (!supportedTypes().contains(type)) {
                throw UnsupportedDataTypeError(
                    (std::string(kName) + ": input plane " + std::to_string(plane) + " has an unsupported type")
                        .c_str(),
                    type, supportedTypes());
            }
            if (out.planeType(plane) != type) {
                throwInternal("output plane " + std::to_string(plane) + " has the wrong type");
            }
            if (g.width <= 0 || g.height <= 0) {
                continue;
            }
            switch (type) {
            case DataType::Byte:
                byteMedian(in, out, plane, g, size_);
                break;
            case DataType::UInt64:
                ownMedian<uint64_t, UnsignedLess>(in, out, plane, g, size_, &unsignedKey);
                break;
            case DataType::Float16:
            case DataType::Float32:
            case DataType::Float64:
                ownMedian<double, FloatLess>(in, out, plane, g, size_, &floatKey);
                break;
            default:
                ownMedian<int64_t, SignedLess>(in, out, plane, g, size_, &signedKey);
                break;
            }
        }
    }

private:
    int32_t size_;
    OperationParameters parameters_;
};

OperationPtr medianBlurFromParameters(const OperationParameters& parameters) {
    detail::BagReader bag(kName, parameters);
    const int64_t kernelSize = bag.getInt("kernelSize", 3);
    if (!validKernelSize(kernelSize)) {
        bag.throwBadValue("kernelSize", kKernelSizeRule);
    }
    bag.finish();
    return makeMedianBlur(static_cast<int32_t>(kernelSize));
}

} // namespace

OperationPtr makeMedianBlur(int32_t kernelSize) {
    if (!validKernelSize(kernelSize)) {
        throw InvalidArgumentError(
            (std::string(kName) + ": parameter 'kernelSize' " + kKernelSizeRule).c_str());
    }
    return std::make_shared<MedianBlurOperation>(kernelSize);
}

namespace detail {

OperationFactoryPtr makeMedianBlurFactory() {
    return std::make_shared<BuiltinFactory>(
        &medianBlurFromParameters,
        "Published definition: the middle value of the sorted window; checked against a direct sort-based reference",
        "O(n * kernelSize^2) in the number of samples (O(n * kernelSize) for Byte)");
}

} // namespace detail

} // namespace slideio::processor
