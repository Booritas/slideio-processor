// Requirements: UR-PIX-5, UR-QA-3, UR-EXE-1, UR-ALG-6, UR-ALG-8
#include <slideio/processor/algorithm/pixel/threshold.hpp>

#include <slideio/processor/core/block_view.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/raster_mask.hpp>
#include <slideio/processor/pipeline/reduction.hpp>

#include "detail/bag.hpp"
#include "detail/builtin_factory.hpp"
#include "detail/builtin_operations.hpp"
#include "detail/sample_codec.hpp"

#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace slideio::processor {

namespace {

constexpr const char* kName = "pixel.threshold_otsu";
constexpr const char* const kDirectionSpellings[] = {"Above", "Below"};
constexpr int64_t kDefaultBinCount = 256;
constexpr int64_t kMaxBinCount = 1048576;

[[noreturn]] void fail(const std::string& problem) {
    throw InvalidArgumentError((std::string(kName) + ": " + problem).c_str());
}

// How one channel's samples map to bins.
struct BinShape {
    // One bin per value of an 8- or 16-bit integer type: the bin is the integer
    // offset v - typeMinimum, never computed through a double.
    bool perValue = false;
    int64_t typeMinimum = 0;
    // The Histogram64 bounds. Per value, maxValue is typeMinimum + binCount, a
    // value the type cannot hold, so Histogram64's floor((v - min) / (max - min)
    // * binCount) divides and multiplies by the same power of two and is exactly
    // the integer offset.
    int32_t binCount = 0;
    double minValue = 0.0;
    double maxValue = 0.0;

    bool operator==(const BinShape& o) const {
        return perValue == o.perValue && typeMinimum == o.typeMinimum && binCount == o.binCount &&
               minValue == o.minValue && maxValue == o.maxValue;
    }
};

// One source channel's histogram and, once finalised, its threshold bin.
struct ChannelState {
    DataType type = DataType::Byte;
    BinShape shape;
    std::unique_ptr<Histogram64> histogram;
    bool counted = false;
    int64_t thresholdBin = 0;
};

class OtsuThresholdOperation : public Operation {
public:
    OtsuThresholdOperation(ThresholdDirection direction, bool hasRange, double minimum, double maximum,
                           int64_t binCount)
        : direction_(direction), hasRange_(hasRange), minimum_(minimum), maximum_(maximum) {
        if (direction != ThresholdDirection::Above && direction != ThresholdDirection::Below) {
            fail("parameter 'direction' is not a ThresholdDirection");
        }
        parameters_.setString("direction", kDirectionSpellings[static_cast<int32_t>(direction)]);
        if (hasRange) {
            if (!std::isfinite(minimum)) {
                fail("parameter 'histogramMinimum' must be finite");
            }
            if (!std::isfinite(maximum)) {
                fail("parameter 'histogramMaximum' must be finite");
            }
            if (!(minimum < maximum)) {
                fail("parameter 'histogramMinimum' must be less than 'histogramMaximum'");
            }
            if (!std::isfinite(maximum - minimum)) {
                fail("parameters 'histogramMinimum' and 'histogramMaximum' span more than a double can hold");
            }
            if (binCount < 2 || binCount > kMaxBinCount) {
                fail("parameter 'binCount' must be from 2 to 1048576");
            }
            binCount_ = static_cast<int32_t>(binCount);
            parameters_.setDouble("histogramMinimum", minimum)
                .setDouble("histogramMaximum", maximum)
                .setInt("binCount", binCount);
        }
    }

    const char* name() const override { return kName; }
    ChannelArity arity() const override { return ChannelArity::PerChannel; }
    bool requiresUniformTypes() const override { return false; }
    DataTypeSet supportedTypes() const override { return DataTypeSet::all(); }
    Halo halo() const override { return Halo::none(); }
    DataType outputType(DataType) const override { return DataType::Byte; }
    const OperationParameters& parameters() const override { return parameters_; }
    bool needsGlobalContext() const override { return true; }

    void accumulate(const OperationContext& context) override {
        const BlockView& in = context.input();
        const Rect64 tile = context.tileRectAtLevel();
        const Rect64 haloRect = context.haloRectAtLevel();
        const RasterMask* mask = context.mask();
        for (int32_t plane = 0; plane < in.planeCount(); ++plane) {
            ChannelState& state = stateFor(in.planeSourceChannel(plane), in.planeType(plane));
            accumulatePlane(in, plane, tile, haloRect, mask, state);
        }
    }

    void combine(const Operation& other) override {
        const auto* o = dynamic_cast<const OtsuThresholdOperation*>(&other);
        if (o == nullptr) {
            fail("cannot combine with an operation of another kind");
        }
        for (const auto& entry : o->channels_) {
            const ChannelState& theirs = entry.second;
            auto found = channels_.find(entry.first);
            if (found == channels_.end()) {
                ChannelState copy;
                copy.type = theirs.type;
                copy.shape = theirs.shape;
                copy.histogram = std::make_unique<Histogram64>(*theirs.histogram);
                channels_.emplace(entry.first, std::move(copy));
                continue;
            }
            ChannelState& ours = found->second;
            if (!(ours.shape == theirs.shape) || ours.type != theirs.type) {
                fail("cannot combine histograms of different shapes for channel " + std::to_string(entry.first));
            }
            ours.histogram->combine(*theirs.histogram);
        }
    }

    void finalizeAccumulation() override {
        for (auto& entry : channels_) {
            finalizeChannel(entry.second);
        }
    }

    void apply(OperationContext& context) override {
        const BlockView& in = context.input();
        BlockView& out = context.output();
        if (out.planeCount() != in.planeCount()) {
            throw InternalError((std::string(kName) + ": the output has " + std::to_string(out.planeCount()) +
                                 " planes for " + std::to_string(in.planeCount()) + " input planes")
                                    .c_str());
        }
        const Rect64 tile = context.tileRectAtLevel();
        const Rect64 haloRect = context.haloRectAtLevel();
        const int64_t offsetX = tile.x - haloRect.x;
        const int64_t offsetY = tile.y - haloRect.y;
        for (int32_t plane = 0; plane < in.planeCount(); ++plane) {
            if (out.planeType(plane) != DataType::Byte) {
                throw InternalError(
                    (std::string(kName) + ": output plane " + std::to_string(plane) + " is not Byte").c_str());
            }
            const DataType type = in.planeType(plane);
            if (dataTypeSizeBytes(type) <= 0) {
                throw UnsupportedDataTypeError(
                    (std::string(kName) + ": input plane " + std::to_string(plane) + " has an unsupported type")
                        .c_str(),
                    type, supportedTypes());
            }
            const auto found = channels_.find(in.planeSourceChannel(plane));
            const ChannelState* state = found == channels_.end() ? nullptr : &found->second;
            const auto* inBase = static_cast<const unsigned char*>(in.planeData(plane));
            auto* outBase = static_cast<unsigned char*>(out.planeData(plane));
            for (int64_t y = 0; y < out.height(); ++y) {
                unsigned char* outRow = outBase + y * out.planeRowStrideBytes(plane);
                if (state == nullptr || !state->counted || state->type != type) {
                    // A channel that counted nothing is background throughout.
                    std::memset(outRow, 0, static_cast<size_t>(out.width()));
                    continue;
                }
                const unsigned char* inRow = inBase + (y + offsetY) * in.planeRowStrideBytes(plane);
                for (int64_t x = 0; x < out.width(); ++x) {
                    outRow[x] = isForeground(*state, detail::readSample(inRow, x + offsetX, type)) ? 255 : 0;
                }
            }
        }
    }

private:
    // The channel's state, created with its bin shape on first sight.
    ChannelState& stateFor(int32_t channel, DataType type) {
        auto found = channels_.find(channel);
        if (found != channels_.end()) {
            if (found->second.type != type) {
                fail("channel " + std::to_string(channel) + " changed type between planes");
            }
            return found->second;
        }
        ChannelState state;
        state.type = type;
        state.shape = shapeFor(type);
        state.histogram =
            std::make_unique<Histogram64>(state.shape.binCount, state.shape.minValue, state.shape.maxValue);
        return channels_.emplace(channel, std::move(state)).first->second;
    }

    BinShape shapeFor(DataType type) const {
        BinShape shape;
        if (hasRange_) {
            shape.binCount = binCount_;
            shape.minValue = minimum_;
            shape.maxValue = maximum_;
            return shape;
        }
        int64_t typeMinimum = 0;
        int32_t bins = 0;
        switch (type) {
        case DataType::Byte: bins = 256; typeMinimum = 0; break;
        case DataType::Int8: bins = 256; typeMinimum = -128; break;
        case DataType::UInt16: bins = 65536; typeMinimum = 0; break;
        case DataType::Int16: bins = 65536; typeMinimum = -32768; break;
        default:
            fail(std::string("a ") + dataTypeName(type) +
                 " input needs the parameters 'histogramMinimum' and 'histogramMaximum'; only Byte, Int8, UInt16 "
                 "and Int16 are binned one bin per value without them");
        }
        shape.perValue = true;
        shape.typeMinimum = typeMinimum;
        shape.binCount = bins;
        shape.minValue = static_cast<double>(typeMinimum);
        shape.maxValue = static_cast<double>(typeMinimum + bins);
        return shape;
    }

    // Packs the samples of the tile rect that the mask admits into one row and
    // lets the channel's Histogram64 bin them.
    static void accumulatePlane(const BlockView& in, int32_t plane, const Rect64& tile, const Rect64& haloRect,
                                const RasterMask* mask, ChannelState& state) {
        const DataType type = in.planeType(plane);
        const int64_t size = dataTypeSizeBytes(type);
        const int64_t offsetX = tile.x - haloRect.x;
        const int64_t offsetY = tile.y - haloRect.y;
        const auto* base = static_cast<const unsigned char*>(in.planeData(plane));
        std::vector<unsigned char> packed;
        packed.reserve(static_cast<size_t>(tile.width * tile.height * size));
        for (int64_t y = 0; y < tile.height; ++y) {
            const unsigned char* row = base + (y + offsetY) * in.planeRowStrideBytes(plane) + offsetX * size;
            if (mask == nullptr) {
                packed.insert(packed.end(), row, row + tile.width * size);
                continue;
            }
            for (int64_t x = 0; x < tile.width; ++x) {
                if (mask->test(Point64{tile.x + x, tile.y + y})) {
                    packed.insert(packed.end(), row + x * size, row + (x + 1) * size);
                }
            }
        }
        const auto count = static_cast<int64_t>(packed.size()) / size;
        if (count == 0) {
            return;
        }
        BlockPlane packedPlane{packed.data(), type, count * size, in.planeSourceChannel(plane)};
        const BlockView view(count, 1, Rect64{0, 0, count, 1}, 0, &packedPlane, 1);
        state.histogram->accumulate(view, 0);
    }

    // Otsu over bin indices. Counts are exact 64-bit integers; the class sums
    // are formed in double in ascending bin order.
    static void finalizeChannel(ChannelState& state) {
        const int32_t n = state.shape.binCount;
        std::vector<int64_t> counts(static_cast<size_t>(n));
        int64_t total = 0;
        double sumAll = 0.0;
        for (int32_t i = 0; i < n; ++i) {
            const int64_t c = state.histogram->bin(i);
            counts[static_cast<size_t>(i)] = c;
            total += c;
            sumAll += static_cast<double>(i) * static_cast<double>(c);
        }
        state.counted = total != 0;
        if (!state.counted) {
            return;
        }
        int64_t best = -1;
        double bestValue = -1.0;
        int64_t c0 = 0;
        double s0 = 0.0;
        for (int32_t k = 0; k <= n - 2; ++k) {
            const int64_t c = counts[static_cast<size_t>(k)];
            c0 += c;
            s0 += static_cast<double>(k) * static_cast<double>(c);
            const int64_t c1 = total - c0;
            if (c0 == 0 || c1 == 0) {
                continue;
            }
            const double w0 = static_cast<double>(c0) / static_cast<double>(total);
            const double w1 = static_cast<double>(c1) / static_cast<double>(total);
            const double d = s0 / static_cast<double>(c0) - (sumAll - s0) / static_cast<double>(c1);
            const double value = w0 * w1 * d * d;
            if (value > bestValue) {
                bestValue = value;
                best = k;
            }
        }
        if (best < 0) {
            // A single non-empty bin: no split has two non-empty classes.
            for (int32_t i = n - 1; i >= 0; --i) {
                if (counts[static_cast<size_t>(i)] != 0) {
                    best = i;
                    break;
                }
            }
        }
        state.thresholdBin = best;
    }

    // The sample's bin, as Histogram64 bins it: -1 below the range and binCount
    // above it; false (background) for NaN.
    bool isForeground(const ChannelState& state, const detail::Sample& sample) const {
        const BinShape& shape = state.shape;
        int64_t bin = 0;
        if (shape.perValue) {
            bin = sample.s - shape.typeMinimum;
        } else {
            double v = 0.0;
            switch (sample.kind) {
            case detail::Sample::Kind::Signed: v = static_cast<double>(sample.s); break;
            case detail::Sample::Kind::Unsigned: v = static_cast<double>(sample.u); break;
            case detail::Sample::Kind::Float: v = sample.f; break;
            }
            if (std::isnan(v)) {
                return false;
            }
            if (v < shape.minValue) {
                bin = -1;
            } else if (v > shape.maxValue) {
                bin = shape.binCount;
            } else {
                const double range = shape.maxValue - shape.minValue;
                bin = static_cast<int64_t>(std::floor((v - shape.minValue) / range * shape.binCount));
                if (bin >= shape.binCount) {
                    bin = shape.binCount - 1;
                }
                if (bin < 0) {
                    bin = 0;
                }
            }
        }
        return direction_ == ThresholdDirection::Above ? bin > state.thresholdBin : bin <= state.thresholdBin;
    }

    ThresholdDirection direction_;
    bool hasRange_;
    double minimum_;
    double maximum_;
    int32_t binCount_ = 0;
    OperationParameters parameters_;
    std::map<int32_t, ChannelState> channels_;
};

bool hasKey(const OperationParameters& parameters, const char* key) {
    for (int32_t i = 0; i < parameters.count(); ++i) {
        if (std::strcmp(parameters.keyAt(i), key) == 0) {
            return true;
        }
    }
    return false;
}

OperationPtr otsuThresholdFromParameters(const OperationParameters& parameters) {
    detail::BagReader bag(kName, parameters);
    const ThresholdDirection direction =
        bag.getEnum("direction", ThresholdDirection::Above, kDirectionSpellings, 2);
    const bool hasMinimum = hasKey(parameters, "histogramMinimum");
    const bool hasMaximum = hasKey(parameters, "histogramMaximum");
    const bool hasBinCount = hasKey(parameters, "binCount");
    const double minimum = bag.getDouble("histogramMinimum", 0.0);
    const double maximum = bag.getDouble("histogramMaximum", 0.0);
    const int64_t binCount = bag.getInt("binCount", kDefaultBinCount);
    bag.finish();
    if (hasMinimum != hasMaximum) {
        bag.throwBadValue(hasMinimum ? "histogramMaximum" : "histogramMinimum",
                          "must be given together with the other end of the histogram range");
    }
    if (hasBinCount && !hasMinimum) {
        bag.throwBadValue("binCount", "needs 'histogramMinimum' and 'histogramMaximum'");
    }
    return std::make_shared<OtsuThresholdOperation>(direction, hasMinimum, minimum, maximum, binCount);
}

} // namespace

OperationPtr makeOtsuThreshold(ThresholdDirection direction) {
    return std::make_shared<OtsuThresholdOperation>(direction, false, 0.0, 0.0, kDefaultBinCount);
}

namespace detail {

OperationFactoryPtr makeOtsuThresholdFactory() {
    return std::make_shared<BuiltinFactory>(
        &otsuThresholdFromParameters,
        "Otsu N. A threshold selection method from gray-level histograms. IEEE Trans SMC 1979;9:62-66; equals the "
        "threshold of a full in-memory histogram of the same slide",
        "O(n) in the number of samples, plus O(bins) once per channel; two passes over the slide");
}

} // namespace detail

} // namespace slideio::processor
