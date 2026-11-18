// Requirements: UR-PIX-3, UR-PIX-5, UR-QA-2, UR-IMG-11
#include "detail/separable.hpp"

#include <slideio/processor/core/error.hpp>

#include "detail/sample_codec.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

namespace slideio::processor::detail {

namespace {

constexpr const char* kWho = "separable filter";

[[noreturn]] void throwInternal(const std::string& problem) {
    throw InternalError((std::string(kWho) + ": " + problem).c_str());
}

// @p value rounded to the nearest float, ties to even, as IEEE 754 does. A
// magnitude beyond float's range is handled here, since converting it by a
// cast is undefined: from FLT_MAX plus half an ulp (a tie, which goes to the
// even neighbour, infinity) up it is infinity, below that FLT_MAX.
float toFloat(double value) noexcept {
    constexpr double kRoundsToInfinity = 0x1.ffffffp127;
    constexpr float kInfinity = std::numeric_limits<float>::infinity();
    constexpr float kMaximum = std::numeric_limits<float>::max();
    const double magnitude = std::fabs(value);
    if (magnitude >= kRoundsToInfinity) {
        return value > 0.0 ? kInfinity : -kInfinity;
    }
    if (magnitude > static_cast<double>(kMaximum)) {
        return value > 0.0 ? kMaximum : -kMaximum;
    }
    return static_cast<float>(value);
}

} // namespace

SeparableFilter::SeparableFilter(std::vector<double> horizontal, std::vector<double> vertical)
    : horizontal_(std::move(horizontal)), vertical_(std::move(vertical)) {
    if (horizontal_.size() % 2 == 0 || vertical_.size() % 2 == 0) {
        throwInternal("a kernel's length must be odd");
    }
}

int32_t SeparableFilter::horizontalRadius() const noexcept {
    return static_cast<int32_t>(horizontal_.size() / 2);
}

int32_t SeparableFilter::verticalRadius() const noexcept {
    return static_cast<int32_t>(vertical_.size() / 2);
}

// Each input row's weighted sums, H(j, x) = sum over dx of h[dx] * v(j, x + dx),
// left to right, are kept for the rows the current output row needs, in a
// ring. Output (x, y) is the sum over dy of w[dy] * H(y + dy, x), top to bottom:
// exactly the per-pixel order, since a row's sums depend on its samples alone.
void SeparableFilter::apply(const BlockView& input, int32_t plane, const Rect64& haloRectAtLevel,
                            const Rect64& tileRectAtLevel, double* out, int64_t outStride) const {
    const DataType type = input.planeType(plane);
    if (type == DataType::Int64 || type == DataType::UInt64) {
        throwInternal("a 64-bit integer plane's samples are not exact in double");
    }
    const int64_t bh = static_cast<int64_t>(horizontal_.size());
    const int64_t bv = static_cast<int64_t>(vertical_.size());
    const int64_t width = tileRectAtLevel.width;
    const int64_t height = tileRectAtLevel.height;
    // The first window of the tile starts at origin within the input.
    const int64_t originX = tileRectAtLevel.x - bh / 2 - haloRectAtLevel.x;
    const int64_t originY = tileRectAtLevel.y - bv / 2 - haloRectAtLevel.y;
    if (originX < 0 || originY < 0 || input.width() != haloRectAtLevel.width ||
        input.height() != haloRectAtLevel.height || originX + width + bh - 1 > input.width() ||
        originY + height + bv - 1 > input.height() || outStride < width) {
        throwInternal("the input does not cover the tile grown by the kernel");
    }
    if (width <= 0 || height <= 0) {
        return;
    }

    const auto* inBase = static_cast<const unsigned char*>(input.planeData(plane)) +
                         originY * input.planeRowStrideBytes(plane);
    const int64_t inStride = input.planeRowStrideBytes(plane);
    const int64_t inWidth = width + bh - 1;
    const double* h = horizontal_.data();
    const double* w = vertical_.data();

    std::vector<double> samples(static_cast<size_t>(inWidth));
    std::vector<double> ring(static_cast<size_t>(bv * width));
    const auto computeRow = [&](int64_t j) {
        const unsigned char* row = inBase + j * inStride;
        for (int64_t x = 0; x < inWidth; ++x) {
            samples[static_cast<size_t>(x)] = sampleAsDouble(readSample(row, originX + x, type));
        }
        double* sums = ring.data() + (j % bv) * width;
        for (int64_t x = 0; x < width; ++x) {
            const double* s = samples.data() + x;
            double sum = 0.0;
            for (int64_t dx = 0; dx < bh; ++dx) {
                sum += h[dx] * s[dx];
            }
            sums[x] = sum;
        }
    };

    for (int64_t j = 0; j < bv - 1; ++j) {
        computeRow(j);
    }
    for (int64_t y = 0; y < height; ++y) {
        computeRow(y + bv - 1);
        double* outRow = out + y * outStride;
        for (int64_t x = 0; x < width; ++x) {
            double sum = 0.0;
            for (int64_t dy = 0; dy < bv; ++dy) {
                sum += w[dy] * ring[static_cast<size_t>(((y + dy) % bv) * width + x)];
            }
            outRow[x] = sum;
        }
    }
}

void writeFloatResult(BlockView& output, int32_t plane, const double* values, int64_t stride) {
    const DataType type = output.planeType(plane);
    if (type != DataType::Float32 && type != DataType::Float64) {
        throwInternal("output plane " + std::to_string(plane) + " is neither Float32 nor Float64");
    }
    const int64_t width = output.width();
    if (stride < width) {
        throwInternal("the result rows overlap");
    }
    auto* base = static_cast<unsigned char*>(output.planeData(plane));
    const int64_t outStride = output.planeRowStrideBytes(plane);
    for (int64_t y = 0; y < output.height(); ++y) {
        const double* row = values + y * stride;
        unsigned char* outRow = base + y * outStride;
        if (type == DataType::Float64) {
            std::memcpy(outRow, row, static_cast<size_t>(width) * sizeof(double));
        } else {
            for (int64_t x = 0; x < width; ++x) {
                const float value = toFloat(row[x]);
                std::memcpy(outRow + x * static_cast<int64_t>(sizeof(float)), &value, sizeof value);
            }
        }
    }
}

DataTypeSet floatFilterTypes() {
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

DataType floatFilterOutputType(DataType input) noexcept {
    return input == DataType::Float64 ? DataType::Float64 : DataType::Float32;
}

} // namespace slideio::processor::detail
