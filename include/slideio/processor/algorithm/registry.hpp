// Requirements: UR-EXT-5, UR-IMG-12
#pragma once

/**
 * @file registry.hpp
 * @brief Registration of the built-in operations with OperationCatalog.
 *
 * The algorithm module's entire coupling to the rest of the library: operations
 * self-register by name, and everything above resolves them through the
 * catalog without linking this module.
 */

#include <slideio/processor/pipeline/export.hpp>

namespace slideio::processor {

/**
 * @brief Registers every built-in operation with OperationCatalog.
 *
 * Idempotent: a name the catalog already holds is left as it is, whoever
 * registered it. Called automatically when the engine library is loaded;
 * exposed so a static build can force it.
 * @throws InvalidArgumentError When the catalog refuses a factory for a reason
 * other than its name being registered already.
 * @throws std::bad_alloc When memory is exhausted.
 * @par Thread-safety
 * ThreadSafe.
 * @par Requirements
 * UR-EXT-5, UR-IMG-12
 */
SLIDEIO_PROCESSOR_ENGINE_EXPORTS void registerBuiltinOperations();

/**
 * @brief Registers the pixel operations (`pixel.*`) only, for a build that
 * ships a subset.
 *
 * Idempotent, as registerBuiltinOperations().
 * @throws InvalidArgumentError As registerBuiltinOperations().
 * @throws std::bad_alloc When memory is exhausted.
 * @par Thread-safety
 * ThreadSafe.
 * @par Requirements
 * UR-EXT-5, UR-IMG-12
 */
SLIDEIO_PROCESSOR_ENGINE_EXPORTS void registerPixelOperations();

/**
 * @brief Registers the segmentation operations (`segment.*`) only, for a build
 * that ships a subset.
 *
 * Idempotent, as registerBuiltinOperations(). The library has no segmentation
 * operation yet, so this registers nothing.
 * @throws InvalidArgumentError As registerBuiltinOperations().
 * @throws std::bad_alloc When memory is exhausted.
 * @par Thread-safety
 * ThreadSafe.
 * @par Requirements
 * UR-EXT-5, UR-IMG-12
 */
SLIDEIO_PROCESSOR_ENGINE_EXPORTS void registerSegmentationOperations();

/**
 * @brief Registers the measurement operations (`measure.*`) only, for a build
 * that ships a subset.
 *
 * Idempotent, as registerBuiltinOperations(). The library has no measurement
 * operation yet, so this registers nothing.
 * @throws InvalidArgumentError As registerBuiltinOperations().
 * @throws std::bad_alloc When memory is exhausted.
 * @par Thread-safety
 * ThreadSafe.
 * @par Requirements
 * UR-EXT-5, UR-IMG-12
 */
SLIDEIO_PROCESSOR_ENGINE_EXPORTS void registerMeasurementOperations();

} // namespace slideio::processor
