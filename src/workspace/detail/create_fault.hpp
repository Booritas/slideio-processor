// Requirements: UR-STG-9
#pragma once

// Test seams, not API: exported from the engine but declared only here, under src/,
// so no public header carries them. They let a test reach the failures of
// Workspace::create that no argument check can: a failure once its store exists, and
// a racing create on the same path between its occupancy check and its first write,
// to show that a failed create removes what it made and only that.

#if defined(SLIDEIO_PROCESSOR_TEST_SEAMS)

#include <slideio/processor/pipeline/export.hpp>

namespace slideio::processor::detail {

// Makes the next Workspace::create in this process throw IoError once its store is
// created and its provenance set, before the scratch directory is made.
// One-shot: the flag is cleared by the create that throws.
SLIDEIO_PROCESSOR_ENGINE_EXPORTS void failNextWorkspaceCreateForTesting() noexcept;

// Makes the next Workspace::create in this process call hook(context) after its
// occupancy and slide checks, just before it makes anything. One-shot: the hook is
// cleared before it runs, so a create the hook itself makes runs unhooked.
SLIDEIO_PROCESSOR_ENGINE_EXPORTS void hookNextWorkspaceCreateForTesting(void (*hook)(void*), void* context) noexcept;

} // namespace slideio::processor::detail

#endif  // SLIDEIO_PROCESSOR_TEST_SEAMS
