// Requirements: UR-PIX-7, UR-QA-2, UR-ALG-6
#include <slideio/processor/algorithm/pixel/filter.hpp>

#include <slideio/processor/core/error.hpp>

#include "detail/bag.hpp"
#include "detail/builtin_factory.hpp"
#include "detail/builtin_operations.hpp"
#include "detail/sample_codec.hpp"
#include "detail/working_set.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

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
    // The module computes in its own memory, all through operator new, one plane
    // at a time. With n the halo'd rect's width, m its height and h <= m the
    // tile's height: the column distances of the tile's rows, uint16, 2 h n <=
    // 2 m n; and two rows of n, the running column distances (uint16) and the
    // envelope's columns (int64), 10 n = 10 m n / m. The rect's height is at
    // least 2 * maximum + 1, so that is at most 10 / (2 * maximum + 1) bytes per
    // pixel, taken rounded up.
    int64_t workingSetBytesPerLevelPixel() const override {
        const int64_t side = 2 * static_cast<int64_t>(maximum_) + 1;
        return 2 + (10 + side - 1) / side;
    }
    // The three vectors' allocator blocks.
    int64_t workingSetBytesPerTile() const override { return 3 * detail::kAllocatorBlockOverhead; }

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
    // The exact squared Euclidean distance transform of Felzenszwalb and
    // Huttenlocher (Theory of Computing 8:415-428, 2012) in two passes, every
    // step in integers:
    //  1. per column, g = the distance to the nearest background pixel of the
    //     column, for each of the tile's rows, capped at H + 1 (a column whose
    //     background is further than H gives no distance of at most H, so the cap
    //     changes no output);
    //  2. per tile row, the lower envelope of the parabolas (x - p)^2 + g(p)^2
    //     over the columns p with g(p) <= H, its breakpoints compared exactly by
    //     cross-multiplication in int64, never divided or rounded.
    // The output is the square root of the least integer squared distance, in
    // double, then as float; or H when that is at least H^2. That is
    // min(true distance, H) at every pixel, whatever the tiling.
    void transformPlane(const BlockView& in, BlockView& out, int32_t plane, int64_t originX, int64_t originY,
                        int64_t width, int64_t height) const {
        const DataType type = in.planeType(plane);
        const int64_t inWidth = in.width();
        const int64_t inHeight = in.height();
        const auto* inBase = static_cast<const unsigned char*>(in.planeData(plane));
        const int64_t inStride = in.planeRowStrideBytes(plane);
        const int cap = maximum_ + 1;
        const size_t columns = static_cast<size_t>(inWidth);
        // Moves the running column distances one row on, to @p row.
        auto step = [&](std::vector<uint16_t>& running, const unsigned char* row) {
            for (int64_t x = 0; x < inWidth; ++x) {
                uint16_t& r = running[static_cast<size_t>(x)];
                const bool background = !foreground(detail::sampleAsDouble(detail::readSample(row, x, type)));
                r = background ? uint16_t{0} : static_cast<uint16_t>(std::min<int>(r + 1, cap));
            }
        };

        // Pass 1 from below: the distance down to background, for the tile's rows.
        std::vector<uint16_t> column(static_cast<size_t>(height) * columns);
        std::vector<uint16_t> running(columns, static_cast<uint16_t>(cap));
        for (int64_t y = inHeight - 1; y >= originY; --y) {
            step(running, inBase + y * inStride);
            if (y < originY + height) {
                std::copy(running.begin(), running.end(),
                          column.begin() + static_cast<std::ptrdiff_t>((y - originY) * inWidth));
            }
        }

        // Pass 1 from above, and pass 2 on each of the tile's rows as it is reached.
        std::fill(running.begin(), running.end(), static_cast<uint16_t>(cap));
        std::vector<int64_t> envelope(columns);
        auto* outBase = static_cast<unsigned char*>(out.planeData(plane));
        const int64_t outStride = out.planeRowStrideBytes(plane);
        for (int64_t y = 0; y < originY + height; ++y) {
            step(running, inBase + y * inStride);
            if (y < originY) {
                continue;
            }
            uint16_t* g = column.data() + (y - originY) * inWidth;
            for (size_t x = 0; x < columns; ++x) {
                g[x] = std::min(g[x], running[x]);
            }
            transformRow(g, inWidth, envelope.data(), originX, width, outBase + (y - originY) * outStride);
        }
    }

    // Pass 2 on one row: @p g the capped column distances of the context's
    // @p inWidth columns; writes the tile's @p width outputs from column @p originX.
    void transformRow(const uint16_t* g, int64_t inWidth, int64_t* v, int64_t originX, int64_t width,
                      unsigned char* outRow) const {
        const int64_t h = maximum_;
        const float capValue = static_cast<float>(maximum_);
        for (int64_t x = 0; x < width; ++x) {
            std::memcpy(outRow + x * static_cast<int64_t>(sizeof(float)), &capValue, sizeof(float));
        }
        auto f = [g](int64_t p) {
            const int64_t d = g[p];
            return d * d;
        };
        // The columns that can give a distance of at most H, in runs whose
        // neighbours are at most 2 H + 1 apart. No pixel is within H of columns
        // further apart than that, and a pixel within H of a later run is further
        // than H from every column of an earlier one, so each run is an envelope
        // of its own, answering the pixels within H of it.
        int64_t p = 0;
        while (p < inWidth) {
            if (g[p] > h) {
                ++p;
                continue;
            }
            // The envelope of one run. Every column dropped between two
            // consecutive members a < b lies within H of a or of b (the envelope
            // there is at most its g <= H, and is a's or b's or further from it),
            // and the run has no gap above 2 H + 1, so b - a <= 4 H + 1; the next
            // column is within 2 H + 1 of the last member. Every product below is
            // then far inside int64 (about 3e12 at most, at H = 4096).
            const int64_t first = p;
            int64_t last = p;
            int64_t k = 0;
            v[0] = p;
            for (int64_t q = p + 1; q < inWidth && q - last <= 2 * h + 1; ++q) {
                if (g[q] > h) {
                    continue;
                }
                last = q;
                // Drop the top b = v[k] while its breakpoint with its predecessor a
                // is not left of its breakpoint with q:
                //   s(a, b) - b = (f(b) - f(a) - alpha^2) / (2 alpha), alpha = b - a,
                //   s(b, q) - b = (f(q) - f(b) + gamma^2) / (2 gamma), gamma = q - b.
                while (k > 0) {
                    const int64_t a = v[k - 1];
                    const int64_t b = v[k];
                    const int64_t alpha = b - a;
                    const int64_t gamma = q - b;
                    if (alpha > 4 * h + 1) {
                        throwInternal("an envelope gap exceeds its bound");
                    }
                    if ((f(b) - f(a) - alpha * alpha) * gamma < (f(q) - f(b) + gamma * gamma) * alpha) {
                        break;
                    }
                    --k;
                }
                v[++k] = q;
            }
            // The tile's pixels within H of the run, walking the envelope: on from
            // a = v[j] to b = v[j + 1] while s(a, b) <= x, that is while
            // f(b) - f(a) <= (b - a) (2 x - a - b).
            const int64_t from = std::max(first - h, originX);
            const int64_t to = std::min(last + h, originX + width - 1);
            int64_t j = 0;
            for (int64_t x = from; x <= to; ++x) {
                while (j < k) {
                    const int64_t a = v[j];
                    const int64_t b = v[j + 1];
                    if (f(b) - f(a) > (b - a) * (2 * x - a - b)) {
                        break;
                    }
                    ++j;
                }
                const int64_t dx = x - v[j];
                if (dx > h || dx < -h) {
                    continue;
                }
                const int64_t squared = dx * dx + f(v[j]);
                if (squared < h * h) {
                    const float value = static_cast<float>(std::sqrt(static_cast<double>(squared)));
                    std::memcpy(outRow + (x - originX) * static_cast<int64_t>(sizeof(float)), &value,
                                sizeof(float));
                }
            }
            p = last + 1;
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
