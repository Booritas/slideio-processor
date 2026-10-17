// Requirements: UR-API-1
#pragma once

/**
 * @file export.hpp
 * @brief Export macro for the slideio-processor-data shared library.
 *
 * Every exported class, free function and interface carries
 * SLIDEIO_PROCESSOR_DATA_EXPORTS. The library itself is compiled with
 * SLIDEIO_PROCESSOR_DATA_API defined.
 */
#if defined(_WIN32)
#if defined(SLIDEIO_PROCESSOR_DATA_API)
#define SLIDEIO_PROCESSOR_DATA_EXPORTS __declspec(dllexport)
#else
#define SLIDEIO_PROCESSOR_DATA_EXPORTS __declspec(dllimport)
#endif
#else
#define SLIDEIO_PROCESSOR_DATA_EXPORTS __attribute__((visibility("default")))
#endif
