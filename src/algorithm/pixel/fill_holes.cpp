// Requirements: UR-PIX-8, UR-QA-2, UR-ALG-6
#include <slideio/processor/algorithm/pixel/filter.hpp>

#include <slideio/processor/core/error.hpp>

#include "detail/bag.hpp"
#include "detail/builtin_factory.hpp"
#include "detail/builtin_operations.hpp"
#include "detail/sample_codec.hpp"
#include "detail/working_set.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace slideio::processor {

namespace {

constexpr const char* kName = "pixel.fill_holes";
constexpr const char* kSizeRule = "must be from 1 to 4096";
constexpr const char* kAreaRule = "must be at least 1";
constexpr int32_t kDefaultSize = 32;
constexpr int32_t kMinimumSize = 1;
constexpr int32_t kMaximumSize = 4096;

// The per-pixel state of the labelling mask.
constexpr uint8_t kForeground = 0;
constexpr uint8_t kBackground = 1; // background not yet reached
constexpr uint8_t kVisited = 2;    // reached, in a component that is not (yet) a hole
constexpr uint8_t kHole = 3;       // in a hole

[[noreturn]] void throwInternal(const std::string& problem) {
    throw InternalError((std::string(kName) + ": " + problem).c_str());
}

bool validSize(int64_t size) {
    return size >= kMinimumSize && size <= kMaximumSize;
}

// A pixel is foreground when its sample is neither 0 nor NaN, as for
// segment.watershed.
bool foreground(double value) {
    return value != 0.0 && !std::isnan(value);
}

class FillHolesOperation : public Operation {
public:
    // @p area 0: no area limit (the key stays absent).
    FillHolesOperation(int32_t size, int64_t area) : size_(size), area_(area) {
        parameters_.setInt("maximumHoleSizePx", size);
        if (area > 0) {
            parameters_.setInt("maximumHoleAreaPx", area);
        }
    }

    const char* name() const override { return kName; }
    ChannelArity arity() const override { return ChannelArity::PerChannel; }
    bool requiresUniformTypes() const override { return false; }
    DataTypeSet supportedTypes() const override { return DataTypeSet::all(); }
    Halo halo() const override { return Halo::uniform(size_); }
    DataType outputType(DataType) const override { return DataType::Byte; }
    const OperationParameters& parameters() const override { return parameters_; }
    // The module labels in its own memory, all through operator new: per pixel of
    // the halo'd rect, the Byte state mask (1) and the queue of one component's
    // pixel indices as int32 (4), sized for the whole rect. No other buffer.
    int64_t workingSetBytesPerLevelPixel() const override { return 5; }
    // The two vectors' allocator blocks.
    int64_t workingSetBytesPerTile() const override { return 2 * detail::kAllocatorBlockOverhead; }

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
        const int64_t originX = tile.x - haloRect.x;
        const int64_t originY = tile.y - haloRect.y;
        if (originX < 0 || originY < 0 || in.width() != haloRect.width || in.height() != haloRect.height ||
            originX + tile.width > in.width() || originY + tile.height > in.height()) {
            throwInternal("the input does not cover the tile");
        }
        // Labels index pixels as int32. The memory ceiling does not keep a larger
        // context out (it is configurable, and 5 bytes a pixel of 2^31 pixels fits
        // a large one), so this is the configuration's to fix: a smaller tile.
        if (in.width() * in.height() > std::numeric_limits<int32_t>::max()) {
            throw InvalidArgumentError((std::string(kName) + ": the tile grown by its halo has " +
                                        std::to_string(in.width() * in.height()) +
                                        " pixels, more than the 2147483647 it can label; use a smaller tile size")
                                           .c_str());
        }
        for (int32_t plane = 0; plane < in.planeCount(); ++plane) {
            const DataType type = in.planeType(plane);
            if (!supportedTypes().contains(type)) {
                throw UnsupportedDataTypeError(
                    (std::string(kName) + ": input plane " + std::to_string(plane) + " has an unsupported type")
                        .c_str(),
                    type, supportedTypes());
            }
            if (out.planeType(plane) != DataType::Byte) {
                throwInternal("output plane " + std::to_string(plane) + " has the wrong type");
            }
            if (tile.width <= 0 || tile.height <= 0) {
                continue;
            }
            fillPlane(in, out, plane, originX, originY, tile.width, tile.height);
        }
    }

private:
    void fillPlane(const BlockView& in, BlockView& out, int32_t plane, int64_t originX, int64_t originY,
                   int64_t width, int64_t height) const {
        const DataType type = in.planeType(plane);
        const int32_t inWidth = static_cast<int32_t>(in.width());
        const int32_t inHeight = static_cast<int32_t>(in.height());
        const auto* inBase = static_cast<const unsigned char*>(in.planeData(plane));
        const int64_t inStride = in.planeRowStrideBytes(plane);

        // The state of the whole halo'd context.
        std::vector<uint8_t> state(static_cast<size_t>(inWidth) * static_cast<size_t>(inHeight));
        for (int32_t y = 0; y < inHeight; ++y) {
            const unsigned char* row = inBase + y * inStride;
            uint8_t* stateRow = state.data() + static_cast<size_t>(y) * static_cast<size_t>(inWidth);
            for (int32_t x = 0; x < inWidth; ++x) {
                stateRow[x] = foreground(detail::sampleAsDouble(detail::readSample(row, x, type))) ? kForeground
                                                                                                   : kBackground;
            }
        }

        // The 4-connected components of the background, one at a time by breadth-first search;
        // the queue holds the current component's pixel indices.
        std::vector<int32_t> queue(state.size());
        for (int32_t startY = 0; startY < inHeight; ++startY) {
            for (int32_t startX = 0; startX < inWidth; ++startX) {
                const int32_t start = startY * inWidth + startX;
                if (state[static_cast<size_t>(start)] != kBackground) {
                    continue;
                }
                int32_t minX = startX;
                int32_t maxX = startX;
                int32_t minY = startY;
                int32_t maxY = startY;
                size_t head = 0;
                size_t tail = 0;
                queue[tail++] = start;
                state[static_cast<size_t>(start)] = kVisited;
                while (head < tail) {
                    const int32_t at = queue[head++];
                    const int32_t x = at % inWidth;
                    const int32_t y = at / inWidth;
                    minX = std::min(minX, x);
                    maxX = std::max(maxX, x);
                    minY = std::min(minY, y);
                    maxY = std::max(maxY, y);
                    if (x > 0 && state[static_cast<size_t>(at - 1)] == kBackground) {
                        state[static_cast<size_t>(at - 1)] = kVisited;
                        queue[tail++] = at - 1;
                    }
                    if (x + 1 < inWidth && state[static_cast<size_t>(at + 1)] == kBackground) {
                        state[static_cast<size_t>(at + 1)] = kVisited;
                        queue[tail++] = at + 1;
                    }
                    if (y > 0 && state[static_cast<size_t>(at - inWidth)] == kBackground) {
                        state[static_cast<size_t>(at - inWidth)] = kVisited;
                        queue[tail++] = at - inWidth;
                    }
                    if (y + 1 < inHeight && state[static_cast<size_t>(at + inWidth)] == kBackground) {
                        state[static_cast<size_t>(at + inWidth)] = kVisited;
                        queue[tail++] = at + inWidth;
                    }
                }
                // A hole does not touch the context's border, and fits the size and the area.
                const bool enclosed = minX > 0 && minY > 0 && maxX + 1 < inWidth && maxY + 1 < inHeight;
                const bool fits = static_cast<int64_t>(maxX) - minX + 1 <= size_ &&
                                  static_cast<int64_t>(maxY) - minY + 1 <= size_ &&
                                  (area_ == 0 || static_cast<int64_t>(tail) <= area_);
                if (enclosed && fits) {
                    for (size_t i = 0; i < tail; ++i) {
                        state[static_cast<size_t>(queue[i])] = kHole;
                    }
                }
            }
        }

        auto* outBase = static_cast<unsigned char*>(out.planeData(plane));
        const int64_t outStride = out.planeRowStrideBytes(plane);
        for (int64_t y = 0; y < height; ++y) {
            unsigned char* outRow = outBase + y * outStride;
            const uint8_t* stateRow =
                state.data() + static_cast<size_t>(originY + y) * static_cast<size_t>(inWidth) +
                static_cast<size_t>(originX);
            for (int64_t x = 0; x < width; ++x) {
                const uint8_t s = stateRow[x];
                outRow[x] = (s == kForeground || s == kHole) ? 255 : 0;
            }
        }
    }

    int32_t size_;
    int64_t area_;
    OperationParameters parameters_;
};

OperationPtr fillHolesFromParameters(const OperationParameters& parameters) {
    detail::BagReader bag(kName, parameters);
    const int64_t size = bag.getInt("maximumHoleSizePx", kDefaultSize);
    if (!validSize(size)) {
        bag.throwBadValue("maximumHoleSizePx", kSizeRule);
    }
    int64_t area = 0;
    if (bag.contains("maximumHoleAreaPx")) {
        area = bag.getInt("maximumHoleAreaPx", 0);
        if (area < 1) {
            bag.throwBadValue("maximumHoleAreaPx", kAreaRule);
        }
    }
    bag.finish();
    return makeFillHoles(static_cast<int32_t>(size), area);
}

} // namespace

OperationPtr makeFillHoles(int32_t maximumHoleSizePx, int64_t maximumHoleAreaPx) {
    if (!validSize(maximumHoleSizePx)) {
        throw InvalidArgumentError((std::string(kName) + ": parameter 'maximumHoleSizePx' " + kSizeRule).c_str());
    }
    if (maximumHoleAreaPx < 0) {
        throw InvalidArgumentError((std::string(kName) + ": parameter 'maximumHoleAreaPx' " + kAreaRule).c_str());
    }
    return std::make_shared<FillHolesOperation>(maximumHoleSizePx, maximumHoleAreaPx);
}

namespace detail {

OperationFactoryPtr makeFillHolesFactory() {
    return std::make_shared<BuiltinFactory>(
        &fillHolesFromParameters,
        "Published definition: background components enclosed within the declared size; checked against a "
        "brute-force flood fill",
        "O(n) in the number of samples of the halo'd tile");
}

} // namespace detail

} // namespace slideio::processor
