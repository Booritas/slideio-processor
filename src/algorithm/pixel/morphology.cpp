// Requirements: UR-PIX-4, UR-QA-2, UR-ALG-6
#include <slideio/processor/algorithm/pixel/filter.hpp>

#include <slideio/processor/core/error.hpp>

#include "detail/bag.hpp"
#include "detail/builtin_factory.hpp"
#include "detail/builtin_operations.hpp"
#include "detail/cv_plane.hpp"
#include "detail/structuring_element_access.hpp"
#include "detail/working_set.hpp"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace slideio::processor {

namespace {

enum class MorphKind { Erode, Dilate, Open, Close };

struct MorphSpec {
    const char* name;
    MorphKind kind;
    int32_t haloFactor; // 1 for one pass of iterations, 2 for open and close
};

constexpr MorphSpec kErode{"pixel.morph_erode", MorphKind::Erode, 1};
constexpr MorphSpec kDilate{"pixel.morph_dilate", MorphKind::Dilate, 1};
constexpr MorphSpec kOpen{"pixel.morph_open", MorphKind::Open, 2};
constexpr MorphSpec kClose{"pixel.morph_close", MorphKind::Close, 2};

constexpr const char* kIterationsRule = "must be an integer from 1 to 64";
constexpr const char* kSizeRule = "must be an odd integer from 1 to 255";
constexpr int32_t kMaximumIterations = 64;
constexpr int32_t kMaximumSize = 255;

// The spellings of the bag's "shape" key; index i is StructuringShape i, and
// the last is an element built from a mask.
constexpr const char* kShapeSpellings[] = {"Rectangle", "Ellipse", "Cross", "Custom"};
constexpr int32_t kShapeCount = 4;
constexpr int32_t kCustomIndex = 3;

constexpr const char* kReference = "Analytic: erosion and dilation of synthetic binary shapes by known elements";
constexpr const char* kComplexity = "O(n * elementWidth * elementHeight * iterations) in the number of samples";

bool validIterations(int64_t iterations) {
    return iterations >= 1 && iterations <= kMaximumIterations;
}

bool validSize(int64_t size) {
    return size >= 1 && size <= kMaximumSize && size % 2 == 1;
}

// The number of set pixels of an element.
int64_t setCountOf(const StructuringElement& element) {
    const std::vector<uint8_t>& mask = detail::StructuringElementAccess::mask(element);
    return static_cast<int64_t>(std::count_if(mask.begin(), mask.end(), [](uint8_t v) { return v != 0; }));
}

// The set pixels of an element as offsets from its centre, row by row from
// the top and left to right within a row: the fixed order the float passes
// visit a window in.
struct Offset {
    int64_t dx;
    int64_t dy;
};

std::vector<Offset> offsetsOf(const StructuringElement& element) {
    const std::vector<uint8_t>& mask = detail::StructuringElementAccess::mask(element);
    const int64_t width = element.width();
    const int64_t height = element.height();
    std::vector<Offset> offsets;
    offsets.reserve(static_cast<size_t>(setCountOf(element)));
    for (int64_t j = 0; j < height; ++j) {
        for (int64_t i = 0; i < width; ++i) {
            if (mask[static_cast<size_t>(j * width + i)] != 0) {
                offsets.push_back({i - width / 2, j - height / 2});
            }
        }
    }
    return offsets;
}

// One erosion (Minimum) or dilation of a width x height float plane: each
// output sample is the minimum or maximum of the samples the offsets reach,
// visited in their fixed order. A NaN among them is the result, the first one
// met; between equal values (-0 and +0) the first met is kept. Offsets that
// leave the plane are skipped; a sample none of whose offsets stays inside is
// set to 0. Those samples lie within one pass's reach of the plane's edge,
// where the halo guarantees no tile sample depends on them.
template <typename T, bool Minimum>
void floatPass(const std::vector<T>& in, std::vector<T>& out, int64_t width, int64_t height,
               const std::vector<Offset>& offsets, int64_t rx, int64_t ry) {
    for (int64_t y = 0; y < height; ++y) {
        for (int64_t x = 0; x < width; ++x) {
            const bool inside = x >= rx && x < width - rx && y >= ry && y < height - ry;
            bool found = false;
            T best = T(0);
            for (const Offset& o : offsets) {
                const int64_t px = x + o.dx;
                const int64_t py = y + o.dy;
                if (!inside && (px < 0 || py < 0 || px >= width || py >= height)) {
                    continue;
                }
                const T v = in[static_cast<size_t>(py * width + px)];
                if (std::isnan(v)) {
                    best = v;
                    break;
                }
                bool better = false;
                if constexpr (Minimum) {
                    better = v < best;
                } else {
                    better = v > best;
                }
                if (!found || better) {
                    best = v;
                    found = true;
                }
            }
            out[static_cast<size_t>(y * width + x)] = best;
        }
    }
}

DataTypeSet morphologyTypes() {
    return DataTypeSet()
        .add(DataType::Byte)
        .add(DataType::UInt16)
        .add(DataType::Int16)
        .add(DataType::Float32)
        .add(DataType::Float64);
}

class MorphologyOperation : public Operation {
public:
    MorphologyOperation(const MorphSpec& spec, const StructuringElement& element, int32_t iterations)
        : spec_(spec), element_(element), iterations_(iterations) {
        using Access = detail::StructuringElementAccess;
        const bool custom = Access::isCustom(element_);
        const int32_t shapeIndex = custom ? kCustomIndex : static_cast<int32_t>(Access::shape(element_));
        parameters_.setString("shape", kShapeSpellings[shapeIndex]);
        parameters_.setInt("elementWidth", element_.width());
        parameters_.setInt("elementHeight", element_.height());
        if (custom) {
            const std::vector<uint8_t>& mask = Access::mask(element_);
            const std::vector<double> values(mask.begin(), mask.end());
            parameters_.setDoubleArray("elementMask", values.data(), static_cast<int32_t>(values.size()));
        }
        parameters_.setInt("iterations", iterations);
        const int32_t reach = spec_.haloFactor * iterations_;
        halo_ = Halo{reach * (element_.width() / 2), reach * (element_.height() / 2),
                     reach * (element_.width() / 2), reach * (element_.height() / 2)};
        setCount_ = setCountOf(element_);
    }

    const char* name() const override { return spec_.name; }
    ChannelArity arity() const override { return ChannelArity::PerChannel; }
    bool requiresUniformTypes() const override { return false; }
    DataTypeSet supportedTypes() const override { return morphologyTypes(); }
    Halo halo() const override { return halo_; }
    DataType outputType(DataType input) const override { return input; }
    const OperationParameters& parameters() const override { return parameters_; }
    // Float path: two copies of the halo'd plane, 16 bytes per pixel for Float64.
    // Integer path, OpenCV 4.14 (imgproc/src/morph.dispatch.cpp, morphOp,
    // morphologyEx; filter.simd.hpp, FilterEngine__start, FilterEngine__apply),
    // at most 2-byte samples: the ring buffer of (kernel height + 3) rows of the
    // halo'd width (at most 4 rows' worth per row of the halo'd rect, since the
    // kernel fits the halo), the source and constant-border rows, 12 bytes per
    // pixel; and through cv::fastMalloc the destination, the copy of the source
    // the tiled path takes for an in-place pass, and the enlarged rectangle of
    // an iterated rectangle (no larger than the halo'd rect), 5 more.
    int64_t workingSetBytesPerLevelPixel() const override { return 17; }
    // The larger of the float path's offsets and the integer path's per-kernel
    // buffers (see integerBytesPerTile()).
    int64_t workingSetBytesPerTile() const override {
        const int64_t offsets = 16 * setCount_ + 3 * detail::kAllocatorBlockOverhead;
        return std::max(offsets, integerBytesPerTile());
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
        // The tile's position within the input plane.
        const int64_t originX = tile.x - haloRect.x;
        const int64_t originY = tile.y - haloRect.y;
        if (in.width() != haloRect.width || in.height() != haloRect.height || originX < halo_.left ||
            originY < halo_.top || originX + tile.width + halo_.right > in.width() ||
            originY + tile.height + halo_.bottom > in.height()) {
            throwInternal("the input does not cover the tile grown by the halo");
        }
        for (int32_t plane = 0; plane < in.planeCount(); ++plane) {
            const DataType type = in.planeType(plane);
            if (!morphologyTypes().contains(type)) {
                throw UnsupportedDataTypeError(
                    (std::string(spec_.name) + ": input plane " + std::to_string(plane) + " has an unsupported type")
                        .c_str(),
                    type, morphologyTypes());
            }
            if (out.planeType(plane) != type) {
                throwInternal("output plane " + std::to_string(plane) + " has the wrong type");
            }
            if (tile.width <= 0 || tile.height <= 0) {
                continue;
            }
            switch (type) {
            case DataType::Float32:
                applyFloatPlane<float>(in, out, plane, originX, originY);
                break;
            case DataType::Float64:
                applyFloatPlane<double>(in, out, plane, originX, originY);
                break;
            default:
                applyPlane(in, out, plane, originX, originY);
                break;
            }
        }
    }

private:
    [[noreturn]] void throwInternal(const std::string& problem) const {
        throw InternalError((std::string(spec_.name) + ": " + problem).c_str());
    }

    // The integer path's buffers that do not scale with the tile, for samples of
    // at most 2 bytes, in OpenCV 4.14. One FilterEngine lives at a time (each
    // pass of morphologyEx builds its own in turn); beyond the halo'd width it
    // holds kernel-sized parts of its ring buffer, source row, constant-border
    // row, border table and border value, and kernel height + 3 row pointers
    // (filter.dispatch.cpp, FilterEngine::init; filter.simd.hpp,
    // FilterEngine__start). morphOp turns an iterated rectangle into one pass of
    // a rectangle 1 + iterations * (side - 1) on a side (morph.dispatch.cpp). An
    // element that is not a rectangle has a MorphFilter: coordinates,
    // coefficients and per-call row pointers, 17 bytes per set pixel
    // (morph.simd.hpp, MorphFilter). Then the engine's objects, the images'
    // bookkeeping and the allocator's overhead on the engine's vectors. When
    // OpenCV runs more than one thread, FilterEngine__apply's tiled path gives
    // each thread a padded 128 x 128 tile, row sums for a rectangle and row
    // pointers (filter.simd.hpp, TiledFilterInvoker), counted once per thread.
    int64_t integerBytesPerTile() const {
        constexpr int64_t kSample = 2;
        constexpr int64_t kEngineObjects = 2048;
        const int64_t width = element_.width();
        const int64_t height = element_.height();
        const bool rectangle = setCount_ == width * height;
        const bool enlarged = rectangle && iterations_ > 1;
        const int64_t kw = enlarged ? 1 + int64_t(iterations_) * (width - 1) : width;
        const int64_t kh = enlarged ? 1 + int64_t(iterations_) * (height - 1) : height;
        const int64_t ring = kSample * (rectangle ? 63 : kw + 62) * (kh + 3) + 64;
        const int64_t rows = kSample * (kw - 1) + kSample * (kw + 63) + 4 * kSample * kw + kSample * kw + 8 * (kh + 3);
        const int64_t filter = rectangle ? 0 : 17 * setCount_;
        const int64_t engine = ring + rows + filter + kEngineObjects + 8 * detail::kAllocatorBlockOverhead +
                               2 * detail::kCvMatBookkeeping;
        const int64_t threads = cv::getNumThreads();
        const int64_t perThread = kSample * (127 + kw) * (127 + kh) + (rectangle ? 256 * (127 + kh) : 0) +
                                  8 * (127 + kh) + 2 * detail::kCvMatBookkeeping;
        return engine + (threads > 1 ? threads * perThread : 0);
    }

    // The module's own morphology of a float plane, pass by pass over the
    // whole input plane, so that a NaN's effect is fixed by the window alone.
    // OpenCV's vectorised minimum and maximum treat a NaN differently from its
    // scalar code, which would make the result depend on the sample's column
    // within the input and so on the tiling.
    template <typename T>
    void applyFloatPlane(const BlockView& in, BlockView& out, int32_t plane, int64_t originX,
                         int64_t originY) const {
        const int64_t width = in.width();
        const int64_t height = in.height();
        std::vector<T> current(static_cast<size_t>(width * height));
        const auto* inBase = static_cast<const unsigned char*>(in.planeData(plane));
        const int64_t inStride = in.planeRowStrideBytes(plane);
        for (int64_t y = 0; y < height; ++y) {
            std::memcpy(current.data() + y * width, inBase + y * inStride, static_cast<size_t>(width) * sizeof(T));
        }
        std::vector<T> next(current.size());
        const std::vector<Offset> offsets = offsetsOf(element_);
        const int64_t rx = element_.width() / 2;
        const int64_t ry = element_.height() / 2;
        const auto passes = [&](bool minimum) {
            for (int32_t i = 0; i < iterations_; ++i) {
                if (minimum) {
                    floatPass<T, true>(current, next, width, height, offsets, rx, ry);
                } else {
                    floatPass<T, false>(current, next, width, height, offsets, rx, ry);
                }
                current.swap(next);
            }
        };
        switch (spec_.kind) {
        case MorphKind::Erode:
            passes(true);
            break;
        case MorphKind::Dilate:
            passes(false);
            break;
        case MorphKind::Open:
            passes(true);
            passes(false);
            break;
        case MorphKind::Close:
            passes(false);
            passes(true);
            break;
        }
        auto* outBase = static_cast<unsigned char*>(out.planeData(plane));
        const int64_t outStride = out.planeRowStrideBytes(plane);
        for (int64_t y = 0; y < out.height(); ++y) {
            std::memcpy(outBase + y * outStride, current.data() + (originY + y) * width + originX,
                        static_cast<size_t>(out.width()) * sizeof(T));
        }
    }

    // OpenCV's morphology of an integer plane (Byte, UInt16, Int16, which
    // have no NaN) over the whole input plane, viewed in place, into a
    // scratch image of which the tile is copied out. The halo covers every
    // pass's reach, so the border OpenCV assumes beyond the input never
    // reaches the copied pixels.
    void applyPlane(const BlockView& in, BlockView& out, int32_t plane, int64_t originX, int64_t originY) const {
        const std::vector<uint8_t>& mask = detail::StructuringElementAccess::mask(element_);
        cv::Mat result;
        try {
            const cv::Mat kernel(element_.height(), element_.width(), CV_8U, const_cast<uint8_t*>(mask.data()));
            const cv::Mat source = detail::planeAsMat(in, plane);
            const cv::Point anchor(-1, -1);
            const cv::Scalar border = cv::morphologyDefaultBorderValue();
            switch (spec_.kind) {
            case MorphKind::Erode:
                cv::erode(source, result, kernel, anchor, iterations_, cv::BORDER_CONSTANT, border);
                break;
            case MorphKind::Dilate:
                cv::dilate(source, result, kernel, anchor, iterations_, cv::BORDER_CONSTANT, border);
                break;
            case MorphKind::Open:
                cv::morphologyEx(source, result, cv::MORPH_OPEN, kernel, anchor, iterations_, cv::BORDER_CONSTANT,
                                 border);
                break;
            case MorphKind::Close:
                cv::morphologyEx(source, result, cv::MORPH_CLOSE, kernel, anchor, iterations_,
                                 cv::BORDER_CONSTANT, border);
                break;
            }
        } catch (const cv::Exception&) {
            detail::translateCv(spec_.name);
        }
        const size_t sampleBytes = static_cast<size_t>(dataTypeSizeBytes(in.planeType(plane)));
        const int64_t width = out.width();
        auto* outBase = static_cast<unsigned char*>(out.planeData(plane));
        const int64_t outStride = out.planeRowStrideBytes(plane);
        for (int64_t y = 0; y < out.height(); ++y) {
            const unsigned char* row = result.ptr<unsigned char>(static_cast<int>(originY + y));
            std::memcpy(outBase + y * outStride, row + originX * static_cast<int64_t>(sampleBytes),
                        static_cast<size_t>(width) * sampleBytes);
        }
    }

    MorphSpec spec_;
    StructuringElement element_;
    int32_t iterations_;
    Halo halo_{};
    int64_t setCount_ = 0;
    OperationParameters parameters_;
};

OperationPtr makeMorphology(const MorphSpec& spec, const StructuringElement& element, int32_t iterations) {
    if (!detail::StructuringElementAccess::valid(element)) {
        throw InvalidArgumentError(
            (std::string(spec.name) + ": parameter 'element' is a moved-from StructuringElement").c_str());
    }
    if (!validIterations(iterations)) {
        throw InvalidArgumentError((std::string(spec.name) + ": parameter 'iterations' " + kIterationsRule).c_str());
    }
    return std::make_shared<MorphologyOperation>(spec, element, iterations);
}

OperationPtr morphologyFromParameters(const MorphSpec& spec, const OperationParameters& parameters) {
    detail::BagReader bag(spec.name, parameters);
    const int32_t shapeIndex = bag.getEnum<int32_t>("shape", 0, kShapeSpellings, kShapeCount);
    const int64_t width = bag.getInt("elementWidth", 3);
    if (!validSize(width)) {
        bag.throwBadValue("elementWidth", kSizeRule);
    }
    const int64_t height = bag.getInt("elementHeight", 3);
    if (!validSize(height)) {
        bag.throwBadValue("elementHeight", kSizeRule);
    }
    std::vector<double> maskValues;
    const bool hasMask = bag.getDoubleArray("elementMask", maskValues);
    const int64_t iterations = bag.getInt("iterations", 1);
    if (!validIterations(iterations)) {
        bag.throwBadValue("iterations", kIterationsRule);
    }
    if (shapeIndex != kCustomIndex) {
        if (hasMask) {
            bag.throwBadValue("elementMask", "is allowed only with the shape \"Custom\"");
        }
        bag.finish();
        return makeMorphology(spec,
                              StructuringElement(static_cast<StructuringShape>(shapeIndex),
                                                 static_cast<int32_t>(width), static_cast<int32_t>(height)),
                              static_cast<int32_t>(iterations));
    }
    if (!hasMask) {
        bag.throwBadValue("elementMask", "is required with the shape \"Custom\"");
    }
    if (static_cast<int64_t>(maskValues.size()) != width * height) {
        bag.throwBadValue("elementMask", "must hold elementWidth * elementHeight values");
    }
    std::vector<uint8_t> mask(maskValues.size());
    bool anySet = false;
    for (size_t i = 0; i < maskValues.size(); ++i) {
        if (maskValues[i] != 0.0 && maskValues[i] != 1.0) {
            bag.throwBadValue("elementMask", "must hold only 0 and 1");
        }
        mask[i] = maskValues[i] == 1.0 ? 1 : 0;
        anySet = anySet || mask[i] != 0;
    }
    if (!anySet) {
        bag.throwBadValue("elementMask", "must have at least one 1");
    }
    bag.finish();
    return makeMorphology(
        spec, StructuringElement::custom(mask.data(), static_cast<int32_t>(width), static_cast<int32_t>(height)),
        static_cast<int32_t>(iterations));
}

OperationPtr erodeFromParameters(const OperationParameters& parameters) {
    return morphologyFromParameters(kErode, parameters);
}
OperationPtr dilateFromParameters(const OperationParameters& parameters) {
    return morphologyFromParameters(kDilate, parameters);
}
OperationPtr openFromParameters(const OperationParameters& parameters) {
    return morphologyFromParameters(kOpen, parameters);
}
OperationPtr closeFromParameters(const OperationParameters& parameters) {
    return morphologyFromParameters(kClose, parameters);
}

} // namespace

OperationPtr makeErode(const StructuringElement& element, int32_t iterations) {
    return makeMorphology(kErode, element, iterations);
}

OperationPtr makeDilate(const StructuringElement& element, int32_t iterations) {
    return makeMorphology(kDilate, element, iterations);
}

OperationPtr makeOpen(const StructuringElement& element, int32_t iterations) {
    return makeMorphology(kOpen, element, iterations);
}

OperationPtr makeClose(const StructuringElement& element, int32_t iterations) {
    return makeMorphology(kClose, element, iterations);
}

namespace detail {

OperationFactoryPtr makeErodeFactory() {
    return std::make_shared<BuiltinFactory>(&erodeFromParameters, kReference, kComplexity);
}

OperationFactoryPtr makeDilateFactory() {
    return std::make_shared<BuiltinFactory>(&dilateFromParameters, kReference, kComplexity);
}

OperationFactoryPtr makeOpenFactory() {
    return std::make_shared<BuiltinFactory>(&openFromParameters, kReference, kComplexity);
}

OperationFactoryPtr makeCloseFactory() {
    return std::make_shared<BuiltinFactory>(&closeFromParameters, kReference, kComplexity);
}

} // namespace detail

} // namespace slideio::processor
