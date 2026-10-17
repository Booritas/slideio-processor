#include <slideio/processor/core/version.hpp>

namespace slideio::processor {

const char* libraryVersion() noexcept { return SLIDEIO_PROCESSOR_VERSION_STRING; }

int32_t libraryVersionMajor() noexcept { return SLIDEIO_PROCESSOR_VERSION_MAJOR; }

int32_t libraryVersionMinor() noexcept { return SLIDEIO_PROCESSOR_VERSION_MINOR; }

int32_t libraryVersionPatch() noexcept { return SLIDEIO_PROCESSOR_VERSION_PATCH; }

} // namespace slideio::processor
