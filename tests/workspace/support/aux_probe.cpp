// Requirements: UR-STG-10
#include "workspace/support/aux_probe.hpp"

#include "pipeline/test_support/test_operations.hpp"

#include <slideio/processor/core/data_type.hpp>
#include <slideio/processor/pipeline/operation.hpp>
#include <slideio/processor/pipeline/operation_catalog.hpp>
#include <slideio/processor/pipeline/operation_parameters.hpp>

#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace slideio::processor::testing_support {

namespace {

std::mutex gMutex;
std::map<std::string, int64_t> gTiles;  // tiles on which each binding was seen

// Operations have no hook to declare an auxiliary input, so the probe reads the binding by
// the name its "binding" parameter gives.
class AuxProbeOperation : public Operation {
public:
    explicit AuxProbeOperation(const OperationParameters& parameters) : parameters_(parameters) {
        binding_ = parameters_.getString("binding", "");
    }
    const char* name() const override { return "test.ws.auxProbe"; }
    ChannelArity arity() const override { return ChannelArity::PerChannel; }
    bool requiresUniformTypes() const override { return false; }
    DataTypeSet supportedTypes() const override { return DataTypeSet().add(DataType::Byte); }
    Halo halo() const override { return Halo::uniform(0); }
    DataType outputType(DataType) const override { return DataType::Byte; }
    const OperationParameters& parameters() const override { return parameters_; }
    void apply(OperationContext& context) override {
        if (context.auxiliary(binding_.c_str()) == nullptr) return;
        std::lock_guard<std::mutex> lock(gMutex);
        ++gTiles[binding_];
    }

private:
    OperationParameters parameters_;
    std::string binding_;
};


// "test.ws.auxAdd": each plane of the output is the input plane plus the same plane of the
// auxiliary raster bound under "binding" (Float32), or the input alone when none is bound.
// Joint over Byte planes, Float32 out, halo 0: a stage that reads the raster it writes
// sees its own prior output in the sum.
class AuxAddOperation : public Operation {
public:
    explicit AuxAddOperation(const OperationParameters& parameters) : parameters_(parameters) {
        binding_ = parameters_.getString("binding", "");
    }
    const char* name() const override { return "test.ws.auxAdd"; }
    ChannelArity arity() const override { return ChannelArity::Joint; }
    bool requiresUniformTypes() const override { return false; }
    DataTypeSet supportedTypes() const override { return DataTypeSet().add(DataType::Byte); }
    Halo halo() const override { return Halo::uniform(0); }
    DataType outputType(DataType) const override { return DataType::Float32; }
    const OperationParameters& parameters() const override { return parameters_; }
    void apply(OperationContext& context) override {
        const BlockView& in = context.input();
        BlockView& out = context.output();
        const BlockView* aux = context.auxiliary(binding_.c_str());
        for (int32_t p = 0; p < out.planeCount(); ++p) {
            const bool added = aux != nullptr && p < aux->planeCount() && aux->planeType(p) == DataType::Float32 &&
                               aux->width() == out.width() && aux->height() == out.height();
            for (int64_t y = 0; y < out.height(); ++y) {
                const uint8_t* a = static_cast<const uint8_t*>(in.planeData(p)) + y * in.planeRowStrideBytes(p);
                float* o = reinterpret_cast<float*>(static_cast<uint8_t*>(out.planeData(p)) +
                                                    y * out.planeRowStrideBytes(p));
                const float* b = added ? reinterpret_cast<const float*>(static_cast<const uint8_t*>(aux->planeData(p)) +
                                                                       y * aux->planeRowStrideBytes(p))
                                       : nullptr;
                for (int64_t x = 0; x < out.width(); ++x) o[x] = static_cast<float>(a[x]) + (b ? b[x] : 0.0f);
            }
        }
    }

private:
    OperationParameters parameters_;
    std::string binding_;
};

} // namespace

void registerAuxProbe() {
    registerTestOperations();
    OperationCatalog& catalog = OperationCatalog::instance();
    if (!catalog.isRegistered("test.ws.auxProbe")) {
        catalog.registerFactory(std::make_shared<PrototypeFactory>(
            [](const OperationParameters& p) -> OperationPtr { return std::make_shared<AuxProbeOperation>(p); },
            "Records the auxiliary raster it is given; leaves the output as allocated", "O(1) per tile"));
    }
    if (!catalog.isRegistered("test.ws.auxAdd")) {
        catalog.registerFactory(std::make_shared<PrototypeFactory>(
            [](const OperationParameters& p) -> OperationPtr { return std::make_shared<AuxAddOperation>(p); },
            "Adds the auxiliary raster it is given to its input", "O(pixels) per tile"));
    }
}

void resetAuxProbe() {
    std::lock_guard<std::mutex> lock(gMutex);
    gTiles.clear();
}

int64_t auxProbeTiles(const std::string& binding) {
    std::lock_guard<std::mutex> lock(gMutex);
    const auto it = gTiles.find(binding);
    return it == gTiles.end() ? 0 : it->second;
}

} // namespace slideio::processor::testing_support
