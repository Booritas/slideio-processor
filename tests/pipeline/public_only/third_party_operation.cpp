// Requirements: UR-EXT-1, UR-EXT-2
#include "third_party_operation.hpp"

#include <slideio/processor/core/error.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>

namespace thirdparty {

namespace {

using namespace slideio::processor;

class InvertOperation : public Operation {
public:
    explicit InvertOperation(const OperationParameters& parameters) : parameters_(parameters) {
        parameters_.getInt("bias", bias_);
    }

    const char* name() const override { return "thirdparty.invert"; }
    ChannelArity arity() const override { return ChannelArity::PerChannel; }
    bool requiresUniformTypes() const override { return false; }
    DataTypeSet supportedTypes() const override { return DataTypeSet().add(DataType::Byte); }
    Halo halo() const override { return Halo::uniform(1); }
    DataType outputType(DataType) const override { return DataType::Byte; }
    const OperationParameters& parameters() const override { return parameters_; }

    void apply(OperationContext& context) override {
        const BlockView& in = context.input();
        BlockView& out = context.output();
        const int64_t ox = context.tileRectAtLevel().x - context.haloRectAtLevel().x;
        const int64_t oy = context.tileRectAtLevel().y - context.haloRectAtLevel().y;
        for (int32_t plane = 0; plane < in.planeCount(); ++plane) {
            if (in.planeType(plane) != DataType::Byte) {
                throw UnsupportedDataTypeError("thirdparty.invert supports Byte only", in.planeType(plane),
                                               supportedTypes());
            }
            for (int64_t y = 0; y < out.height(); ++y) {
                const auto* src = static_cast<const uint8_t*>(in.planeData(plane)) +
                                  (y + oy) * in.planeRowStrideBytes(plane) + ox;
                auto* dst = static_cast<uint8_t*>(out.planeData(plane)) + y * out.planeRowStrideBytes(plane);
                for (int64_t x = 0; x < out.width(); ++x) {
                    dst[x] = static_cast<uint8_t>(std::max<int64_t>(0, 255 - src[x] - bias_));
                }
            }
        }
    }

private:
    OperationParameters parameters_;
    int64_t bias_ = 0;
};

class InvertFactory : public OperationFactory {
public:
    OperationPtr create(const OperationParameters& parameters) const override {
        return std::make_shared<InvertOperation>(parameters);
    }
    const char* name() const override { return "thirdparty.invert"; }
    ChannelArity arity() const override { return ChannelArity::PerChannel; }
    bool requiresUniformTypes() const override { return false; }
    DataTypeSet supportedTypes() const override { return DataTypeSet().add(DataType::Byte); }
    Halo halo() const override { return Halo::uniform(1); }
    const char* correctnessReference() const override { return "Analytic: out = max(0, 255 - in - bias)"; }
    const char* complexityNote() const override { return "O(n) in the number of samples"; }
};

} // namespace

slideio::processor::OperationFactoryPtr makeInvertFactory() {
    return std::make_shared<InvertFactory>();
}

slideio::processor::OperationPtr makeInvertOperation(const slideio::processor::OperationParameters& parameters) {
    return std::make_shared<InvertOperation>(parameters);
}

} // namespace thirdparty
