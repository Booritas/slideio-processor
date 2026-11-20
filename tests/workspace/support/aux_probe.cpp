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

namespace slideio::processor::testing_support {

namespace {

struct Seen {
    int64_t tiles = 0;
};

std::mutex gMutex;
std::map<std::string, Seen> gSeen;

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
        const BlockView* aux = context.auxiliary(binding_.c_str());
        if (aux == nullptr) return;
        std::lock_guard<std::mutex> lock(gMutex);
        Seen& seen = gSeen[binding_];
        ++seen.tiles;
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
            "Records the auxiliary raster it is given; writes zeros", "O(n) in the number of samples"));
    }
}

void resetAuxProbe() {
    std::lock_guard<std::mutex> lock(gMutex);
    gSeen.clear();
}

int64_t auxProbeTiles(const std::string& binding) {
    std::lock_guard<std::mutex> lock(gMutex);
    const auto it = gSeen.find(binding);
    return it == gSeen.end() ? 0 : it->second.tiles;
}


} // namespace slideio::processor::testing_support
