// Requirements: UR-BLD-2
#pragma once

#include <slideio/processor/core/export.hpp>

// SLIDEIO_PROCESSOR_DATA_TEST_EXPORTS marks a detail function that production calls only
// inside the data library and that only tests call across the library boundary. It exports
// the function while test seams are on (SLIDEIO_PROCESSOR_TEST_SEAMS defined) and leaves it
// internal otherwise, so a release build does not export it.
#if defined(SLIDEIO_PROCESSOR_TEST_SEAMS)
#define SLIDEIO_PROCESSOR_DATA_TEST_EXPORTS SLIDEIO_PROCESSOR_DATA_EXPORTS
#else
#define SLIDEIO_PROCESSOR_DATA_TEST_EXPORTS
#endif
