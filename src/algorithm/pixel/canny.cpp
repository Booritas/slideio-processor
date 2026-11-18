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

// cv::Canny keeps its edge pixels in std::deque<uchar*> (8-byte entries) and
// holds three of them: the stripe's stack, its border peaks and the shared
// stack, of which only the first fills with one stripe. Their bytes through
// operator new depend on the standard library's deque layout: per entry at the
// largest size N, and fixed, whatever N.
#if defined(_MSVC_STL_VERSION)
// MSVC STL (<deque>, _Block_size, _Growmap, _Emplace_back_internal): blocks of
// 2 entries (16 bytes, 8 per entry), never freed before the deque is; a map of
// block pointers, a power of 2 of at least 8, doubled once the blocks fill it,
// so at most 8 (N + 2) bytes, and old and new maps live together while it
// doubles, 12 (N + 2) at most. An empty deque allocates nothing.
constexpr int64_t kEdgeStackBytesPerEntry = 20;
constexpr int64_t kEdgeStackFixedBytes = 256;
#elif defined(_LIBCPP_VERSION)
// libc++ (<deque>, __deque_block_size, __add_back_capacity, pop_back): blocks
// of 512 entries (4096 bytes, 8 per entry); a __split_buffer map of block
// pointers, doubled when full, old and new together at most 3 pointers per
// block (0.05 per entry). Fixed: a partly filled block at either end and the
// two spare blocks pop_back keeps, 4 x 4096, and the smallest maps. An empty
// deque allocates nothing.
constexpr int64_t kEdgeStackBytesPerEntry = 9;
constexpr int64_t kEdgeStackFixedBytes = 17408;
#elif defined(__GLIBCXX__)
// libstdc++ (bits/stl_deque.h, __deque_buf_size, _M_initialize_map,
// _M_reallocate_map): nodes of 64 entries (512 bytes, 8 per entry); a map of
// node pointers, grown to map size + max(map size, added) + 2 with the nodes
// re-centred in it; a deque that grows at one end fills the map up to about
// 1.5 pointers per node, and the old and new maps live together while it
// grows, about 4.5 pointers per node (0.56 per entry). Fixed: a
// partly filled node at either end, and each of the three deques' first map of
// 8 pointers and first node even while empty, 2 x 512 + 3 x 576.
constexpr int64_t kEdgeStackBytesPerEntry = 9;
constexpr int64_t kEdgeStackFixedBytes = 4096;
#else
// Another standard library: the largest of the three above.
constexpr int64_t kEdgeStackBytesPerEntry = 20;
constexpr int64_t kEdgeStackFixedBytes = 17408;
#endif

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
    // cv::Canny, OpenCV 4.14 (imgproc/src/canny.cpp), on a halo'd rect at least
    // 69 pixels on a side, with OpenCV on the calling thread: one stripe of
    // every row (parallel_for_ runs the body once on the whole range), so
    // parallelCanny::operator() reads no row beyond the plane and keeps no
    // border peaks, and the shared stack cv::Canny then drains stays empty. The
    // edge map (1 byte per pixel); the suppression map, (rows + 2) x (width +
    // 128) at most (3); the 16-bit dx and dy of every row (4); through operator
    // new, the stack of edge pixels, each pushed once, at most one entry per
    // pixel (kEdgeStackBytesPerEntry); and the magnitude rows, 12 bytes per
    // column, with Sobel's FilterEngine, at most 41 bytes per column, fewer
    // than one byte per pixel over 69 rows (1).
    int64_t workingSetBytesPerLevelPixel() const override { return 9 + kEdgeStackBytesPerEntry; }
    // The magnitude rows' and Sobel's kernel-sized parts, kernels, objects and
    // bookkeeping (8 KiB), the maps' bookkeeping (1 KiB), and the edge stacks'
    // fixed bytes. Sobel takes FilterEngine__apply's direct path: its tiled
    // path (filter.simd.hpp, TiledFilterInvoker) needs more than one OpenCV
    // thread.
    int64_t workingSetBytesPerTile() const override { return 8192 + 1024 + kEdgeStackFixedBytes; }

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
