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

} // namespace

void registerAuxProbe() {
    registerTestOperations();
    OperationCatalog& catalog = OperationCatalog::instance();
    if (!catalog.isRegistered("test.ws.auxProbe")) {
        catalog.registerFactory(std::make_shared<PrototypeFactory>(
            [](const OperationParameters& p) -> OperationPtr { return std::make_shared<AuxProbeOperation>(p); },
            "Records the auxiliary raster it is given; leaves the output as allocated", "O(1) per tile"));
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
