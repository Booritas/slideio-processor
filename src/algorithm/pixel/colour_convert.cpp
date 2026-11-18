// Requirements: UR-PIX-1, UR-IMG-7, UR-ALG-3, UR-ALG-6, UR-IMG-11
#include <slideio/processor/algorithm/pixel/colour.hpp>

#include <slideio/processor/core/error.hpp>

#include "detail/bag.hpp"
#include "detail/builtin_factory.hpp"
#include "detail/builtin_operations.hpp"
#include "detail/colour_math.hpp"
#include "detail/rgb_input.hpp"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

namespace slideio::processor {

namespace {

constexpr const char* kName = "pixel.colour_convert";
constexpr const char* const kSpaceSpellings[] = {"Rgb", "Grey", "Hsv", "Lab"};

bool isColourSpace(ColourSpace space) {
    const auto index = static_cast<int32_t>(space);
    return index >= 0 && index <= static_cast<int32_t>(ColourSpace::Lab);
}

DataTypeSet byteAndFloat32() {
    return DataTypeSet().add(DataType::Byte).add(DataType::Float32);
}

// The sample at column x of a Byte or Float32 row.
double readValue(const unsigned char* row, int64_t x, DataType type) {
    if (type == DataType::Byte) {
        return static_cast<double>(row[x]);
    }
    float value = 0.0f;
    std::memcpy(&value, row + x * static_cast<int64_t>(sizeof(float)), sizeof(float));
    return static_cast<double>(value);
}

void writeFloat(unsigned char* row, int64_t x, double value) {
    const auto sample = static_cast<float>(value);
    std::memcpy(row + x * static_cast<int64_t>(sizeof(float)), &sample, sizeof(float));
}

class ColourConvertOperation : public Operation {
public:
    ColourConvertOperation(ColourSpace from, ColourSpace to) : from_(from), to_(to) {
        if (!isColourSpace(from)) {
            throw InvalidArgumentError((std::string(kName) + ": parameter 'from' is not a ColourSpace").c_str());
        }
        if (!isColourSpace(to)) {
            throw InvalidArgumentError((std::string(kName) + ": parameter 'to' is not a ColourSpace").c_str());
        }
        if (from == to) {
            throw InvalidArgumentError(
                (std::string(kName) + ": parameters 'from' and 'to' name the same colour space").c_str());
        }
        parameters_.setString("from", kSpaceSpellings[static_cast<int32_t>(from)])
            .setString("to", kSpaceSpellings[static_cast<int32_t>(to)]);
    }

    const char* name() const override { return kName; }
    ChannelArity arity() const override { return ChannelArity::Joint; }
    bool requiresUniformTypes() const override { return true; }
    DataTypeSet supportedTypes() const override { return byteAndFloat32(); }
    Halo halo() const override { return Halo::none(); }
    DataType outputType(DataType) const override { return DataType::Float32; }
    const OperationParameters& parameters() const override { return parameters_; }
    int32_t outputPlaneCount(int32_t) const override { return to_ == ColourSpace::Grey ? 1 : 3; }

    void apply(OperationContext& context) override {
        const BlockView& in = context.input();
        BlockView& out = context.output();
        checkInput(in);
        const int32_t outPlanes = outputPlaneCount(in.planeCount());
        if (out.planeCount() != outPlanes) {
            throw InternalError((std::string(kName) + ": the output has " + std::to_string(out.planeCount()) +
                                 " planes, not " + std::to_string(outPlanes))
                                    .c_str());
        }
        for (int32_t plane = 0; plane < outPlanes; ++plane) {
            if (out.planeType(plane) != DataType::Float32) {
                throw InternalError(
                    (std::string(kName) + ": output plane " + std::to_string(plane) + " is not Float32").c_str());
            }
        }
        const Point64 offset{context.tileRectAtLevel().x - context.haloRectAtLevel().x,
                             context.tileRectAtLevel().y - context.haloRectAtLevel().y};
        const int32_t inPlanes = in.planeCount();
        for (int64_t y = 0; y < out.height(); ++y) {
            const unsigned char* inRows[3] = {};
            for (int32_t plane = 0; plane < inPlanes; ++plane) {
                inRows[plane] = static_cast<const unsigned char*>(in.planeData(plane)) +
                                (y + offset.y) * in.planeRowStrideBytes(plane);
            }
            unsigned char* outRows[3] = {};
            for (int32_t plane = 0; plane < outPlanes; ++plane) {
                outRows[plane] =
                    static_cast<unsigned char*>(out.planeData(plane)) + y * out.planeRowStrideBytes(plane);
            }
            for (int64_t x = 0; x < out.width(); ++x) {
                detail::Triple pixel{};
                double* components[3] = {&pixel.c0, &pixel.c1, &pixel.c2};
                for (int32_t plane = 0; plane < inPlanes; ++plane) {
                    *components[plane] = readValue(inRows[plane], x + offset.x, in.planeType(plane));
                }
                const detail::Triple rgb = toRgb(pixel);
                if (to_ == ColourSpace::Grey) {
                    writeFloat(outRows[0], x, detail::rgbToGrey(rgb));
                    continue;
                }
                const detail::Triple result = fromRgb(rgb);
                writeFloat(outRows[0], x, result.c0);
                writeFloat(outRows[1], x, result.c1);
                writeFloat(outRows[2], x, result.c2);
            }
        }
    }

private:
    void checkInput(const BlockView& in) const {
        if (from_ == ColourSpace::Rgb) {
            detail::requireRgbInput(in, kName);
            return;
        }
        const int32_t expected = from_ == ColourSpace::Grey ? 1 : 3;
        const char* space = from_ == ColourSpace::Grey ? "grey" : from_ == ColourSpace::Hsv ? "HSV" : "Lab";
        if (in.planeCount() != expected) {
            throw ChannelArityError((std::string(kName) + ": " + space + " input needs " + std::to_string(expected) +
                                     (expected == 1 ? " plane" : " planes") + "; found " +
                                     std::to_string(in.planeCount()))
                                        .c_str(),
                                    in.planeCount());
        }
        const DataTypeSet accepted = from_ == ColourSpace::Grey ? byteAndFloat32() : DataTypeSet().add(DataType::Float32);
        for (int32_t plane = 0; plane < in.planeCount(); ++plane) {
            const DataType type = in.planeType(plane);
            if (!accepted.contains(type)) {
                throw UnsupportedDataTypeError((std::string(kName) + ": " + space + " input plane " +
                                                std::to_string(plane) + " is " + dataTypeName(type) + ", not " +
                                                (from_ == ColourSpace::Grey ? "Byte or Float32" : "Float32"))
                                                   .c_str(),
                                               type, accepted);
            }
        }
    }

    detail::Triple toRgb(const detail::Triple& pixel) const {
        switch (from_) {
        case ColourSpace::Grey: return {pixel.c0, pixel.c0, pixel.c0};
        case ColourSpace::Hsv: return detail::hsvToRgb(pixel);
        case ColourSpace::Lab: return detail::labToRgb(pixel);
        case ColourSpace::Rgb: break;
        }
        return pixel;
    }

    detail::Triple fromRgb(const detail::Triple& rgb) const {
        switch (to_) {
        case ColourSpace::Hsv: return detail::rgbToHsv(rgb);
        case ColourSpace::Lab: return detail::rgbToLab(rgb);
        case ColourSpace::Grey:
        case ColourSpace::Rgb: break;
        }
        return rgb;
    }

    ColourSpace from_;
    ColourSpace to_;
    OperationParameters parameters_;
};

OperationPtr colourConvertFromParameters(const OperationParameters& parameters) {
    detail::BagReader bag(kName, parameters);
    const ColourSpace from = bag.getEnum("from", ColourSpace::Rgb, kSpaceSpellings, 4);
    const ColourSpace to = bag.getEnum("to", ColourSpace::Grey, kSpaceSpellings, 4);
    bag.finish();
    return makeColourConvert(from, to);
}

} // namespace

OperationPtr makeColourConvert(ColourSpace from, ColourSpace to) {
    return std::make_shared<ColourConvertOperation>(from, to);
}

namespace detail {

OperationFactoryPtr makeColourConvertFactory() {
    return std::make_shared<BuiltinFactory>(
        &colourConvertFromParameters,
        "Analytic: IEC 61966-2-1 sRGB, CIE 1976 L*a*b* with D65 white, hexcone HSV, ITU-R BT.601 luma; round trips "
        "within 1e-3 on the 0-255 scale",
        "O(n) in the number of pixels");
}

} // namespace detail

} // namespace slideio::processor
