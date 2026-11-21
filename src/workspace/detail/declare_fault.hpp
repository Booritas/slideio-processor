// Requirements: UR-STG-2, UR-STG-8, UR-OBS-1
#pragma once

// Test seam, not API: exported from the engine but declared only here, under src/, so no
// public header carries it. It reaches the middle of a StageRegistry::declare, between its
// first write and its dependency rows, which no argument or storage state can reach
// deterministically: to show that a reader refreshing there sees no half-written
// declaration, and that a declare failing there changes nothing.

#include <slideio/processor/pipeline/export.hpp>

namespace slideio::processor::detail {

// Makes the next StageRegistry::declare in this process that writes a configuration (a new
// stage, or a redeclaration that changes it) call hook(context) once that configuration is
// written and before any dependency row is. An equivalent redeclaration does not call it.
// A hook that throws makes the declare throw that exception, and the declare changes
// nothing. One-shot: the hook is cleared before it runs.
SLIDEIO_PROCESSOR_ENGINE_EXPORTS void hookNextDeclareMidwayForTesting(void (*hook)(void*), void* context) noexcept;

} // namespace slideio::processor::detail
