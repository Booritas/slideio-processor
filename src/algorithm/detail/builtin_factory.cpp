// Requirements: UR-EXT-5, UR-ALG-4, UR-ALG-6, UR-ALG-8
#include "detail/builtin_factory.hpp"

#include <slideio/processor/core/error.hpp>

namespace slideio::processor::detail {

BuiltinFactory::BuiltinFactory(OperationPtr (*maker)(const OperationParameters&), const char* correctnessReference,
                               const char* complexityNote)
    : maker_(maker),
      prototype_(maker != nullptr ? maker(OperationParameters()) : nullptr),
      correctnessReference_(correctnessReference),
      complexityNote_(complexityNote) {
    if (prototype_ == nullptr) {
        throw InternalError("BuiltinFactory: the maker is null or returned null for default parameters");
    }
}

OperationPtr BuiltinFactory::create(const OperationParameters& parameters) const { return maker_(parameters); }
const char* BuiltinFactory::name() const { return prototype_->name(); }
ChannelArity BuiltinFactory::arity() const { return prototype_->arity(); }
bool BuiltinFactory::requiresUniformTypes() const { return prototype_->requiresUniformTypes(); }
DataTypeSet BuiltinFactory::supportedTypes() const { return prototype_->supportedTypes(); }
Halo BuiltinFactory::halo() const { return prototype_->halo(); }
const char* BuiltinFactory::correctnessReference() const { return correctnessReference_; }
const char* BuiltinFactory::complexityNote() const { return complexityNote_; }
TileRequirement BuiltinFactory::tileRequirement() const { return prototype_->tileRequirement(); }
int64_t BuiltinFactory::workingSetBytesPerLevelPixel() const { return prototype_->workingSetBytesPerLevelPixel(); }
bool BuiltinFactory::producesObjects() const { return prototype_->producesObjects(); }
int32_t BuiltinFactory::outputPlaneCount(int32_t inputPlaneCount) const {
    return prototype_->outputPlaneCount(inputPlaneCount);
}
int64_t BuiltinFactory::workingSetBytesPerTile() const { return prototype_->workingSetBytesPerTile(); }
int64_t BuiltinFactory::accumulatedStateBytes(int32_t planeCount) const {
    return prototype_->accumulatedStateBytes(planeCount);
}

} // namespace slideio::processor::detail
