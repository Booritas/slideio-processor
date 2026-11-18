// Requirements: UR-EXT-5, UR-ALG-4, UR-ALG-6, UR-ALG-8
#pragma once

#include <slideio/processor/pipeline/operation.hpp>
#include <slideio/processor/pipeline/operation_catalog.hpp>
#include <slideio/processor/pipeline/operation_parameters.hpp>

#include <cstdint>

namespace slideio::processor::detail {

/// The factory of a built-in operation: create() calls the maker, and every
/// declaration is read from a prototype the maker builds from an empty bag, so
/// it describes the operation created from default parameters. The two texts
/// must be string literals (or otherwise outlive the factory).
class BuiltinFactory : public OperationFactory {
public:
    using Maker = OperationPtr (*)(const OperationParameters&);
    BuiltinFactory(OperationPtr (*maker)(const OperationParameters&), const char* correctnessReference,
                   const char* complexityNote);

    OperationPtr create(const OperationParameters& parameters) const override;
    const char* name() const override;
    ChannelArity arity() const override;
    bool requiresUniformTypes() const override;
    DataTypeSet supportedTypes() const override;
    Halo halo() const override;
    const char* correctnessReference() const override;
    const char* complexityNote() const override;
    TileRequirement tileRequirement() const override;
    int64_t workingSetBytesPerLevelPixel() const override;
    bool producesObjects() const override;
    int32_t outputPlaneCount(int32_t inputPlaneCount) const override;
    int64_t workingSetBytesPerTile() const override;
    int64_t accumulatedStateBytes(int32_t planeCount) const override;

private:
    Maker maker_;
    OperationPtr prototype_;
    const char* correctnessReference_;
    const char* complexityNote_;
};

} // namespace slideio::processor::detail
