// Requirements: UR-IMG-13, UR-IMG-10, UR-IMG-11, UR-ALG-6, UR-ALG-8
#include <slideio/processor/algorithm/pixel/convert.hpp>

#include <slideio/processor/core/error.hpp>

#include "detail/bag.hpp"
#include "detail/builtin_factory.hpp"
#include "detail/builtin_operations.hpp"
#include "detail/sample_codec.hpp"

#include <cstring>
#include <memory>
#include <string>

namespace slideio::processor {

namespace {

constexpr const char* kName = "pixel.convert_type";
constexpr const char* const kRoundingSpellings[] = {"Nearest", "TowardZero", "Floor", "Ceil"};
constexpr const char* const kOverflowSpellings[] = {"Saturate", "Error"};

bool isSampleType(DataType type) {
    const auto index = static_cast<int32_t>(type);
    return index >= 0 && index < static_cast<int32_t>(DataType::Unknown);
}

class TypeConvertOperation : public Operation {
public:
    TypeConvertOperation(DataType target, RoundingMode rounding, OverflowMode overflow)
        : target_(target), rounding_(rounding), overflow_(overflow) {
        if (!isSampleType(target)) {
            throw InvalidArgumentError((std::string(kName) + ": parameter 'target' is not a sample type").c_str());
        }
        const auto roundingIndex = static_cast<int32_t>(rounding);
        if (roundingIndex < 0 || roundingIndex > static_cast<int32_t>(RoundingMode::Ceil)) {
            throw InvalidArgumentError((std::string(kName) + ": parameter 'rounding' is not a RoundingMode").c_str());
        }
        if (overflow != OverflowMode::Saturate && overflow != OverflowMode::Error) {
            throw InvalidArgumentError((std::string(kName) + ": parameter 'overflow' is not an OverflowMode").c_str());
        }
        parameters_.setString("target", dataTypeName(target))
            .setString("rounding", kRoundingSpellings[roundingIndex])
            .setString("overflow", kOverflowSpellings[static_cast<int32_t>(overflow)]);
    }

    const char* name() const override { return kName; }
    ChannelArity arity() const override { return ChannelArity::PerChannel; }
    bool requiresUniformTypes() const override { return false; }
    DataTypeSet supportedTypes() const override { return DataTypeSet::all(); }
    Halo halo() const override { return Halo::none(); }
    DataType outputType(DataType) const override { return target_; }
    const OperationParameters& parameters() const override { return parameters_; }
    // None: each sample is converted in place and written straight to the output.
    int64_t workingSetBytesPerLevelPixel() const override { return 0; }
    // None: the overflowing source types are kept in a bit set, and the one
    // type name of a plane fits a string's inline buffer.
    int64_t workingSetBytesPerTile() const override { return 0; }

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
        // Every overflowing sample of the tile is counted before the tile fails.
        int64_t overflowCount = 0;
        // The source types with an overflowing sample, in plane order, each once.
        std::string overflowSources;
        DataTypeSet namedTypes;
        for (int32_t plane = 0; plane < in.planeCount(); ++plane) {
            const DataType source = in.planeType(plane);
            if (!isSampleType(source)) {
                throw UnsupportedDataTypeError((std::string(kName) + ": input plane " + std::to_string(plane) +
                                                " has an unsupported type")
                                                   .c_str(),
                                               source, supportedTypes());
            }
            if (out.planeType(plane) != target_) {
                throw InternalError((std::string(kName) + ": output plane " + std::to_string(plane) +
                                     " is not of the target type")
                                        .c_str());
            }
            const int64_t planeOverflows = convertPlane(in, out, plane, offset);
            if (planeOverflows > 0 && !namedTypes.contains(source)) {
                namedTypes.add(source);
                overflowSources += (overflowSources.empty() ? "" : ", ") + std::string(dataTypeName(source));
            }
            overflowCount += planeOverflows;
        }
        if (overflow_ == OverflowMode::Error && overflowCount > 0) {
            throw InvalidArgumentError((std::string(kName) + ": " + std::to_string(overflowCount) + " samples of " +
                                        overflowSources + " do not fit " + dataTypeName(target_))
                                           .c_str());
        }
    }

private:
    // Converts one plane over the tile rect; returns how many samples overflowed.
    int64_t convertPlane(const BlockView& in, BlockView& out, int32_t plane, Point64 offset) const {
        const DataType source = in.planeType(plane);
        const int64_t inSize = dataTypeSizeBytes(source);
        const int64_t outSize = dataTypeSizeBytes(target_);
        const auto* inBase = static_cast<const unsigned char*>(in.planeData(plane));
        auto* outBase = static_cast<unsigned char*>(out.planeData(plane));
        int64_t overflows = 0;
        for (int64_t y = 0; y < out.height(); ++y) {
            const unsigned char* inRow = inBase + (y + offset.y) * in.planeRowStrideBytes(plane);
            unsigned char* outRow = outBase + y * out.planeRowStrideBytes(plane);
            if (source == target_) {
                // A sample of the target type is copied bit for bit, NaN payloads included.
                std::memcpy(outRow, inRow + offset.x * inSize, static_cast<size_t>(out.width() * outSize));
                continue;
            }
            for (int64_t x = 0; x < out.width(); ++x) {
                bool overflowed = false;
                detail::writeSample(outRow, x, target_, detail::readSample(inRow, x + offset.x, source), rounding_,
                                    overflow_, overflowed);
                if (overflowed) {
                    ++overflows;
                }
            }
        }
        return overflows;
    }

    DataType target_;
    RoundingMode rounding_;
    OverflowMode overflow_;
    OperationParameters parameters_;
};

OperationPtr typeConvertFromParameters(const OperationParameters& parameters) {
    detail::BagReader bag(kName, parameters);
    const char* targetName = bag.getString("target", "Float32");
    DataType target = DataType::Unknown;
    try {
        target = detail::dataTypeFromName(targetName);
    } catch (const InvalidArgumentError&) {
        bag.throwBadValue("target", "is not the name of a data type");
    }
    const RoundingMode rounding = bag.getEnum("rounding", RoundingMode::Nearest, kRoundingSpellings, 4);
    const OverflowMode overflow = bag.getEnum("overflow", OverflowMode::Saturate, kOverflowSpellings, 2);
    bag.finish();
    return makeTypeConvert(target, rounding, overflow);
}

} // namespace

OperationPtr makeTypeConvert(DataType target, RoundingMode rounding, OverflowMode overflow) {
    return std::make_shared<TypeConvertOperation>(target, rounding, overflow);
}

namespace detail {

OperationFactoryPtr makeTypeConvertFactory() {
    return std::make_shared<BuiltinFactory>(
        &typeConvertFromParameters,
        "Analytic: the documented rounding and overflow rules, checked at each type's minimum, maximum and overflow",
        "O(n) in the number of samples");
}

} // namespace detail

} // namespace slideio::processor
