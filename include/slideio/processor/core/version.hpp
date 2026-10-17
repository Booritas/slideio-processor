// Requirements: UR-API-6, UR-RES-11
#pragma once

#include <slideio/processor/core/export.hpp>

#include <cstdint>

namespace slideio::processor {

/**
 * @brief Returns the library version as "major.minor.patch".
 * @return A static, null-terminated UTF-8 string. Never null.
 * @throws Nothing.
 * @par Thread-safety
 * Safe to call concurrently from any thread.
 * @par Requirements
 * UR-API-6, UR-RES-11
 */
SLIDEIO_PROCESSOR_DATA_EXPORTS const char* libraryVersion() noexcept;

/**
 * @brief Returns the major version number.
 * @return The major version.
 * @throws Nothing.
 * @par Thread-safety
 * Safe to call concurrently from any thread.
 * @par Requirements
 * UR-API-6, UR-RES-11
 */
SLIDEIO_PROCESSOR_DATA_EXPORTS int32_t libraryVersionMajor() noexcept;

/**
 * @brief Returns the minor version number.
 * @return The minor version.
 * @throws Nothing.
 * @par Thread-safety
 * Safe to call concurrently from any thread.
 * @par Requirements
 * UR-API-6, UR-RES-11
 */
SLIDEIO_PROCESSOR_DATA_EXPORTS int32_t libraryVersionMinor() noexcept;

/**
 * @brief Returns the patch version number.
 * @return The patch version.
 * @throws Nothing.
 * @par Thread-safety
 * Safe to call concurrently from any thread.
 * @par Requirements
 * UR-API-6, UR-RES-11
 */
SLIDEIO_PROCESSOR_DATA_EXPORTS int32_t libraryVersionPatch() noexcept;

} // namespace slideio::processor
