// Requirements: UR-PIX-2, UR-IMG-7, UR-ALG-3, UR-ALG-6, UR-IMG-11
#include <slideio/processor/algorithm/pixel/colour.hpp>

#include <slideio/processor/core/error.hpp>

#include "detail/bag.hpp"
#include "detail/builtin_factory.hpp"
#include "detail/builtin_operations.hpp"
#include "detail/rgb_input.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace slideio::processor {

namespace {

constexpr const char* kName = "pixel.stain_deconvolution";
constexpr double kMinDeterminant = 1e-6;

[[noreturn]] void throwInvalid(const char* problem) {
    throw InvalidArgumentError((std::string(kName) + ": " + problem).c_str());
}

std::string nameKey(int32_t index) {
    return "stainName" + std::to_string(index);
}

void normalise(double (&v)[3]) {
    const double length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    v[0] /= length;
    v[1] /= length;
    v[2] /= length;
}

// Writes the inverse of @p m into @p inverse; returns the determinant of @p m.
double invert(const double (&m)[3][3], double (&inverse)[3][3]) {
    const double c00 = m[1][1] * m[2][2] - m[1][2] * m[2][1];
    const double c01 = m[1][2] * m[2][0] - m[1][0] * m[2][2];
    const double c02 = m[1][0] * m[2][1] - m[1][1] * m[2][0];
    const double det = m[0][0] * c00 + m[0][1] * c01 + m[0][2] * c02;
    if (!(std::fabs(det) >= kMinDeterminant)) {
        return det;
    }
    inverse[0][0] = c00 / det;
    inverse[1][0] = c01 / det;
    inverse[2][0] = c02 / det;
    inverse[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det;
    inverse[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det;
    inverse[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det;
    inverse[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det;
    inverse[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det;
    inverse[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det;
    return det;
}

void writeFloat(unsigned char* row, int64_t x, double value) {
    const auto sample = static_cast<float>(value);
    std::memcpy(row + x * static_cast<int64_t>(sizeof(float)), &sample, sizeof(float));
}

class StainDeconvolutionOperation : public Operation {
public:
    explicit StainDeconvolutionOperation(const StainVectors& stains) : count_(stains.count()) {
        if (count_ < 2) {
            throwInvalid("needs two or three stains");
        }
        double m[3][3] = {};
        std::vector<double> given;
        for (int32_t i = 0; i < count_; ++i) {
            stains.getVector(i, m[i][0], m[i][1], m[i][2]);
            given.insert(given.end(), {m[i][0], m[i][1], m[i][2]});
            normalise(m[i]);
        }
        if (count_ == 2) {
            m[2][0] = m[0][1] * m[1][2] - m[0][2] * m[1][1];
            m[2][1] = m[0][2] * m[1][0] - m[0][0] * m[1][2];
            m[2][2] = m[0][0] * m[1][1] - m[0][1] * m[1][0];
            const double length = std::sqrt(m[2][0] * m[2][0] + m[2][1] * m[2][1] + m[2][2] * m[2][2]);
            if (!(length >= kMinDeterminant)) {
                throwInvalid("the stain vectors are linearly dependent");
            }
            normalise(m[2]);
        }
        if (!(std::fabs(invert(m, inverse_)) >= kMinDeterminant)) {
            throwInvalid("the stain vectors are linearly dependent");
        }
        parameters_.setDoubleArray("vectors", given.data(), static_cast<int32_t>(given.size()));
        for (int32_t i = 0; i < count_; ++i) {
            parameters_.setString(nameKey(i).c_str(), stains.stainName(i));
        }
    }

    const char* name() const override { return kName; }
    ChannelArity arity() const override { return ChannelArity::Joint; }
    bool requiresUniformTypes() const override { return true; }
    DataTypeSet supportedTypes() const override { return DataTypeSet().add(DataType::Byte); }
    Halo halo() const override { return Halo::none(); }
    DataType outputType(DataType) const override { return DataType::Float32; }
    const OperationParameters& parameters() const override { return parameters_; }
    int32_t outputPlaneCount(int32_t) const override { return count_; }

    void apply(OperationContext& context) override {
        const BlockView& in = context.input();
        BlockView& out = context.output();
        detail::requireRgbInput(in, kName);
        if (out.planeCount() != count_) {
            throw InternalError((std::string(kName) + ": the output has " + std::to_string(out.planeCount()) +
                                 " planes, not " + std::to_string(count_))
                                    .c_str());
        }
        for (int32_t plane = 0; plane < count_; ++plane) {
            if (out.planeType(plane) != DataType::Float32) {
                throw InternalError(
                    (std::string(kName) + ": output plane " + std::to_string(plane) + " is not Float32").c_str());
            }
        }
        // The optical density of each 8-bit level, max(I, 1) so that 0 stays finite.
        double density[256];
        for (int32_t level = 0; level < 256; ++level) {
            density[level] = -std::log10(static_cast<double>(level == 0 ? 1 : level) / 255.0);
        }
        const Point64 offset{context.tileRectAtLevel().x - context.haloRectAtLevel().x,
                             context.tileRectAtLevel().y - context.haloRectAtLevel().y};
        for (int64_t y = 0; y < out.height(); ++y) {
            const unsigned char* inRows[3] = {};
            for (int32_t plane = 0; plane < 3; ++plane) {
                inRows[plane] = static_cast<const unsigned char*>(in.planeData(plane)) +
                                (y + offset.y) * in.planeRowStrideBytes(plane) + offset.x;
            }
            unsigned char* outRows[3] = {};
            for (int32_t plane = 0; plane < count_; ++plane) {
                outRows[plane] = static_cast<unsigned char*>(out.planeData(plane)) + y * out.planeRowStrideBytes(plane);
            }
            for (int64_t x = 0; x < out.width(); ++x) {
                const double od[3] = {density[inRows[0][x]], density[inRows[1][x]], density[inRows[2][x]]};
                // The row vector od times the inverse: one amount per stain.
                for (int32_t stain = 0; stain < count_; ++stain) {
                    const double amount =
                        od[0] * inverse_[0][stain] + od[1] * inverse_[1][stain] + od[2] * inverse_[2][stain];
                    writeFloat(outRows[stain], x, amount);
                }
            }
        }
    }

private:
    int32_t count_;
    double inverse_[3][3] = {};
    OperationParameters parameters_;
};

OperationPtr stainDeconvolutionFromParameters(const OperationParameters& parameters) {
    detail::BagReader bag(kName, parameters);
    std::vector<double> vectors;
    StainVectors stains;
    if (!bag.getDoubleArray("vectors", vectors)) {
        stains = StainVectors::hematoxylinEosin();
    } else {
        if (vectors.size() != 6 && vectors.size() != 9) {
            bag.throwBadValue("vectors", "must hold 6 or 9 components, 3 per stain");
        }
        const auto count = static_cast<int32_t>(vectors.size() / 3);
        for (int32_t i = 0; i < count; ++i) {
            try {
                stains.setVector(i, vectors[3 * i], vectors[3 * i + 1], vectors[3 * i + 2]);
            } catch (const InvalidArgumentError&) {
                bag.throwBadValue("vectors", "holds a vector with a negative component or all components zero");
            }
        }
    }
    for (int32_t i = 0; i < stains.count(); ++i) {
        const char* name = bag.getString(nameKey(i).c_str(), nullptr);
        if (name != nullptr) {
            stains.setStainName(i, name);
        }
    }
    bag.finish();
    return makeStainDeconvolution(stains);
}

} // namespace

OperationPtr makeStainDeconvolution(const StainVectors& stains) {
    return std::make_shared<StainDeconvolutionOperation>(stains);
}

namespace detail {

OperationFactoryPtr makeStainDeconvolutionFactory() {
    return std::make_shared<BuiltinFactory>(
        &stainDeconvolutionFromParameters,
        "Ruifrok AC, Johnston DA. Quantification of histochemical staining by color deconvolution. Anal Quant Cytol "
        "Histol 2001;23:291-299; synthetic images composed from known vectors",
        "O(n) in the number of pixels");
}

} // namespace detail

} // namespace slideio::processor
