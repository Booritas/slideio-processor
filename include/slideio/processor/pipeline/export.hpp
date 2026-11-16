// Requirements: UR-API-1
#pragma once

/**
 * @file export.hpp
 * @brief Export macro for the slideio-processor-engine shared library.
 *
 * Every exported class, free function and interface of the engine library
 * (the pipeline module, later also algorithm and workspace) carries
 * SLIDEIO_PROCESSOR_ENGINE_EXPORTS. The library itself is compiled with
 * SLIDEIO_PROCESSOR_ENGINE_API defined.
 */

/**
 * @def SLIDEIO_PROCESSOR_ENGINE_EXPORTS
 * @brief Marks a declaration as part of the slideio-processor-engine ABI.
 *
 * On Windows it expands to `__declspec(dllexport)` while the library is built
 * (SLIDEIO_PROCESSOR_ENGINE_API defined) and to `__declspec(dllimport)` for
 * consumers; elsewhere it gives the symbol default visibility.
 * @par Requirements
 * UR-API-1
 */
#if defined(_WIN32)
#if defined(SLIDEIO_PROCESSOR_ENGINE_API)
#define SLIDEIO_PROCESSOR_ENGINE_EXPORTS __declspec(dllexport)
#else
#define SLIDEIO_PROCESSOR_ENGINE_EXPORTS __declspec(dllimport)
#endif
#else
#define SLIDEIO_PROCESSOR_ENGINE_EXPORTS __attribute__((visibility("default")))
#endif
