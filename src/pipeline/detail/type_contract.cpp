// Requirements: UR-IMG-11, UR-IMG-12, UR-ALG-4
#include <pipeline/detail/type_contract.hpp>

#include <slideio/processor/pipeline/backend.hpp>

namespace slideio::processor::detail {

bool operationAccepts(const DataTypeSet& declared, DataType type) noexcept {
    return declared.contains(type) && Backend::instance().supports(type);
}

} // namespace slideio::processor::detail
