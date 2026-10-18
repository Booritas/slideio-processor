#include <slideio/processor/config/backend_capabilities.hpp>

#include "config/backend_version.hpp"
#include "config/detail/backend_types.hpp"

namespace slideio::processor {

class BackendCapabilities::Impl {
public:
    Impl() : supported(detail::backendTypesForOpenCvMajor(SLIDEIO_PROCESSOR_BACKEND_MAJOR)) {}

    DataTypeSet supported;
};

BackendCapabilities::BackendCapabilities() noexcept : impl_(std::make_unique<Impl>()) {}

BackendCapabilities::~BackendCapabilities() = default;

const BackendCapabilities& BackendCapabilities::instance() noexcept {
    static const BackendCapabilities capabilities;
    return capabilities;
}

const char* BackendCapabilities::backendName() const noexcept {
    return SLIDEIO_PROCESSOR_BACKEND_NAME;
}

const char* BackendCapabilities::backendVersion() const noexcept {
    return SLIDEIO_PROCESSOR_BACKEND_VERSION;
}

int32_t BackendCapabilities::backendMajorVersion() const noexcept {
    return SLIDEIO_PROCESSOR_BACKEND_MAJOR;
}

DataTypeSet BackendCapabilities::supportedTypes() const noexcept {
    return impl_->supported;
}

bool BackendCapabilities::supports(DataType type) const noexcept {
    return impl_->supported.contains(type);
}

DataTypeSet BackendCapabilities::storageOnlyTypes() const noexcept {
    DataTypeSet storageOnly = DataTypeSet::all();
    for (int32_t i = 0; i < impl_->supported.count(); ++i) {
        storageOnly.remove(impl_->supported.at(i));
    }
    return storageOnly;
}

} // namespace slideio::processor
