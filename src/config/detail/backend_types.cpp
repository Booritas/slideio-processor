#include "config/detail/backend_types.hpp"

namespace slideio::processor::detail {

DataTypeSet backendTypesForOpenCvMajor(int32_t major) noexcept {
    DataTypeSet types = DataTypeSet::all();
    if (major < 5) {
        types.remove(DataType::UInt32);
        types.remove(DataType::Int64);
        types.remove(DataType::UInt64);
    }
    return types;
}

} // namespace slideio::processor::detail
