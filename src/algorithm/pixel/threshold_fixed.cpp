// Requirements: UR-PIX-5, UR-IMG-11, UR-ALG-6, UR-ALG-8
#include <slideio/processor/algorithm/pixel/threshold.hpp>

#include <slideio/processor/core/error.hpp>

#include "detail/bag.hpp"
#include "detail/builtin_factory.hpp"
#include "detail/sample_codec.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <string>

namespace slideio::processor {

namespace {

constexpr const char* kName = "pixel.threshold_fixed";
constexpr const char* const kDirectionSpellings[] = {"Above", "Below"};
constexpr double kTwo63 = 9223372036854775808.0;
constexpr double kTwo64 = 18446744073709551616.0;

// What a threshold means for one integer type: every sample on one side, or a
// strict comparison against an integer bound of the sample's own range.
enum class Decision { Background, Foreground, Compare };

template <typename B>
struct Bound {
    Decision decision;
    B value;
};

// Above: v > t exactly when v > floor(t). Below: v < t exactly when v < ceil(t).
// A bound outside the type's range puts every sample on one side.
template <typename T>
Bound<int64_t> signedBound(double t, ThresholdDirection direction) {
    const auto lo = static_cast<int64_t>(std::numeric_limits<T>::lowest());
    const auto hi = static_cast<int64_t>(std::numeric_limits<T>::max());
    if (direction == ThresholdDirection::Above) {
        const double f = std::floor(t);
        if (f < static_cast<double>(lo)) {
            return {Decision::Foreground, 0};
        }
        if (f >= kTwo63) {
            return {Decision::Background, 0};
        }
        const auto b = static_cast<int64_t>(f);
        return b >= hi ? Bound<int64_t>{Decision::Background, 0} : Bound<int64_t>{Decision::Compare, b};
    }
    const double c = std::ceil(t);
    if (c >= kTwo63) {
        return {Decision::Foreground, 0};
    }
    if (c <= static_cast<double>(lo)) {
        return {Decision::Background, 0};
    }
    const auto b = static_cast<int64_t>(c);
    return b > hi ? Bound<int64_t>{Decision::Foreground, 0} : Bound<int64_t>{Decision::Compare, b};
}

Bound<uint64_t> unsignedBound(double t, ThresholdDirection direction) {
    if (direction == ThresholdDirection::Above) {
        const double f = std::floor(t);
        if (f < 0.0) {
            return {Decision::Foreground, 0};
        }
        if (f >= kTwo64) {
            return {Decision::Background, 0};
        }
        const auto b = static_cast<uint64_t>(f);
        return b == std::numeric_limits<uint64_t>::max() ? Bound<uint64_t>{Decision::Background, 0}
                                                         : Bound<uint64_t>{Decision::Compare, b};
    }
    const double c = std::ceil(t);
    if (c <= 0.0) {
        return {Decision::Background, 0};
    }
    if (c >= kTwo64) {
        return {Decision::Foreground, 0};
    }
    return {Decision::Compare, static_cast<uint64_t>(c)};
}

template <typename T>
T load(const unsigned char* row, int64_t x) {
    T value;
    std::memcpy(&value, row + x * static_cast<int64_t>(sizeof(T)), sizeof(T));
    return value;
}

// Writes 255 where foreground(sample) holds, else 0, over the tile rect.
template <typename Read, typename Foreground>
void fill(const BlockView& in, BlockView& out, int32_t plane, Point64 offset, Read read, Foreground foreground) {
    const auto* inBase = static_cast<const unsigned char*>(in.planeData(plane));
    auto* outBase = static_cast<unsigned char*>(out.planeData(plane));
    for (int64_t y = 0; y < out.height(); ++y) {
        const unsigned char* inRow = inBase + (y + offset.y) * in.planeRowStrideBytes(plane);
        unsigned char* outRow = outBase + y * out.planeRowStrideBytes(plane);
        for (int64_t x = 0; x < out.width(); ++x) {
            outRow[x] = foreground(read(inRow, x + offset.x)) ? 255 : 0;
        }
    }
}

void fillConstant(BlockView& out, int32_t plane, unsigned char value) {
    auto* outBase = static_cast<unsigned char*>(out.planeData(plane));
    for (int64_t y = 0; y < out.height(); ++y) {
        std::memset(outBase + y * out.planeRowStrideBytes(plane), value, static_cast<size_t>(out.width()));
    }
}

template <typename T, typename B>
void applyInteger(const BlockView& in, BlockView& out, int32_t plane, Point64 offset, Bound<B> bound,
                  ThresholdDirection direction) {
    if (bound.decision != Decision::Compare) {
        fillConstant(out, plane, bound.decision == Decision::Foreground ? 255 : 0);
        return;
    }
    const auto read = [](const unsigned char* row, int64_t x) { return static_cast<B>(load<T>(row, x)); };
    const B b = bound.value;
    if (direction == ThresholdDirection::Above) {
        fill(in, out, plane, offset, read, [b](B v) { return v > b; });
    } else {
        fill(in, out, plane, offset, read, [b](B v) { return v < b; });
    }
}

template <typename T>
void applySigned(const BlockView& in, BlockView& out, int32_t plane, Point64 offset, double t,
                 ThresholdDirection direction) {
    applyInteger<T>(in, out, plane, offset, signedBound<T>(t, direction), direction);
}

// NaN compares false either way, so it is background.
template <typename Read>
void applyFloat(const BlockView& in, BlockView& out, int32_t plane, Point64 offset, double t,
                ThresholdDirection direction, Read read) {
    if (direction == ThresholdDirection::Above) {
        fill(in, out, plane, offset, read, [t](double v) { return v > t; });
    } else {
        fill(in, out, plane, offset, read, [t](double v) { return v < t; });
    }
}

class FixedThresholdOperation : public Operation {
public:
    FixedThresholdOperation(double value, ThresholdDirection direction) : value_(value), direction_(direction) {
        if (!std::isfinite(value)) {
            throw InvalidArgumentError((std::string(kName) + ": parameter 'value' must be finite").c_str());
        }
        if (direction != ThresholdDirection::Above && direction != ThresholdDirection::Below) {
            throw InvalidArgumentError(
                (std::string(kName) + ": parameter 'direction' is not a ThresholdDirection").c_str());
        }
        parameters_.setDouble("value", value).setString("direction",
                                                        kDirectionSpellings[static_cast<int32_t>(direction)]);
    }

    const char* name() const override { return kName; }
    ChannelArity arity() const override { return ChannelArity::PerChannel; }
    bool requiresUniformTypes() const override { return false; }
    DataTypeSet supportedTypes() const override { return DataTypeSet::all(); }
    Halo halo() const override { return Halo::none(); }
    DataType outputType(DataType) const override { return DataType::Byte; }
    const OperationParameters& parameters() const override { return parameters_; }

    void apply(OperationContext& context) override {
        const BlockView& in = context.input();
        BlockView& out = context.output();
        if (out.planeCount() != in.planeCount()) {
            throw InternalError((std::string(kName) + ": the output has " + std::to_string(out.planeCount()) +
                                 " planes for " + std::to_string(in.planeCount()) + " input planes")
                                    .c_str());
        }
        const Point64 offset{context.tileRectAtLevel().x - context.haloRectAtLevel().x,
                             context.tileRectAtLevel().y - context.haloRectAtLevel().y};
        for (int32_t plane = 0; plane < in.planeCount(); ++plane) {
            if (out.planeType(plane) != DataType::Byte) {
                throw InternalError((std::string(kName) + ": output plane " + std::to_string(plane) +
                                     " is not Byte")
                                        .c_str());
            }
            applyPlane(in, out, plane, offset);
        }
    }

private:
    void applyPlane(const BlockView& in, BlockView& out, int32_t plane, Point64 offset) const {
        const double t = value_;
        const ThresholdDirection d = direction_;
        switch (in.planeType(plane)) {
        case DataType::Byte: applySigned<uint8_t>(in, out, plane, offset, t, d); return;
        case DataType::Int8: applySigned<int8_t>(in, out, plane, offset, t, d); return;
        case DataType::UInt16: applySigned<uint16_t>(in, out, plane, offset, t, d); return;
        case DataType::Int16: applySigned<int16_t>(in, out, plane, offset, t, d); return;
        case DataType::UInt32: applySigned<uint32_t>(in, out, plane, offset, t, d); return;
        case DataType::Int32: applySigned<int32_t>(in, out, plane, offset, t, d); return;
        case DataType::Int64: applySigned<int64_t>(in, out, plane, offset, t, d); return;
        case DataType::UInt64: applyInteger<uint64_t>(in, out, plane, offset, unsignedBound(t, d), d); return;
        case DataType::Float16:
            applyFloat(in, out, plane, offset, t, d, [](const unsigned char* row, int64_t x) {
                return detail::decodeBinary16(load<uint16_t>(row, x));
            });
            return;
        case DataType::Float32:
            applyFloat(in, out, plane, offset, t, d, [](const unsigned char* row, int64_t x) {
                return static_cast<double>(load<float>(row, x));
            });
            return;
        case DataType::Float64:
            applyFloat(in, out, plane, offset, t, d, [](const unsigned char* row, int64_t x) {
                return load<double>(row, x);
            });
            return;
        default: break;
        }
        throw UnsupportedDataTypeError((std::string(kName) + ": input plane " + std::to_string(plane) +
                                        " has an unsupported type")
                                           .c_str(),
                                       in.planeType(plane), supportedTypes());
    }

    double value_;
    ThresholdDirection direction_;
    OperationParameters parameters_;
};

OperationPtr fixedThresholdFromParameters(const OperationParameters& parameters) {
    detail::BagReader bag(kName, parameters);
    const double value = bag.getDouble("value", 128.0);
    const ThresholdDirection direction =
        bag.getEnum("direction", ThresholdDirection::Above, kDirectionSpellings, 2);
    bag.finish();
    return makeFixedThreshold(value, direction);
}

} // namespace

OperationPtr makeFixedThreshold(double value, ThresholdDirection direction) {
    return std::make_shared<FixedThresholdOperation>(value, direction);
}

namespace detail {

OperationFactoryPtr makeFixedThresholdFactory() {
    return std::make_shared<BuiltinFactory>(
        &fixedThresholdFromParameters,
        "Analytic: a sample is foreground exactly when it is strictly above (or below) the value; NaN is background",
        "O(n) in the number of samples");
}

} // namespace detail

} // namespace slideio::processor
