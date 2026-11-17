// Requirements: UR-IMG-11, UR-IMG-12, UR-ALG-4
#pragma once

#include <slideio/processor/core/data_type.hpp>

namespace slideio::processor::detail {

/// The one rule for whether an operation computes a type: the
/// operation declares it and the backend computes it natively. The support
/// matrix writes exactly this, and Pipeline::validate() enforces exactly this.
bool operationAccepts(const DataTypeSet& declared, DataType type) noexcept;

} // namespace slideio::processor::detail
