// Requirements: UR-STG-4, UR-RES-11
#pragma once

// The durable record of a stage definition (SDS 04 4.2), shared by the stage registry
// that stores it and the code that compares declarations. Not installed; exported from
// the engine, declared only here, so the tests and the registry reach them.

#include <pipeline/detail/engine_test_export.hpp>
#include <slideio/processor/storage/layer_registry.hpp>
#include <slideio/processor/storage/raster_store.hpp>
#include <slideio/processor/workspace/stage_definition.hpp>

#include <cstdint>
#include <string>

namespace slideio::processor::detail {

// Reads of a definition's members the public accessors do not give: the explicit
// readsLayer calls, apart from the reads the other declarations imply.
struct StageDefinitionAccess {
    static int32_t explicitReadCount(const StageDefinition& definition) noexcept;
    static LayerId explicitReadAt(const StageDefinition& definition, int32_t index);
};

// The record, stageRecordVersion 1, naming every layer and raster through the registries.
// Throws InvalidArgumentError naming the stage when an id is unknown to them.
SLIDEIO_PROCESSOR_ENGINE_TEST_EXPORTS std::string stageRecordJson(const StageDefinition& definition,
                                                                   const LayerRegistry& layers,
                                                                   const RasterRegistry& rasters);

// The inverse. Throws InvalidArgumentError on text that does not parse, a missing or
// mistyped member, a stageRecordVersion other than 1, or a layer or raster the
// registries do not hold (the message names it).
SLIDEIO_PROCESSOR_ENGINE_TEST_EXPORTS StageDefinition stageFromRecord(const char* json,
                                                                       const LayerRegistry& layers,
                                                                       const RasterRegistry& rasters);

// True when the two declare the same stage: name, pipeline (PipelineSpec::equals) and
// every declaration list and optional, each list compared without regard to order.
SLIDEIO_PROCESSOR_ENGINE_TEST_EXPORTS bool sameDeclaration(const StageDefinition& a, const StageDefinition& b);

} // namespace slideio::processor::detail
