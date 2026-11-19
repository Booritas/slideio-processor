// Requirements: UR-STG-9
#pragma once

// Test seam, not API: exported from the engine but declared only here, under src/,
// so no public header carries it. It lets a test fail Workspace::create after the
// workspace directory and its store exist, a failure no argument check can reach,
// to show that a failed create leaves nothing behind.

#include <slideio/processor/pipeline/export.hpp>

namespace slideio::processor::detail {

// Makes the next Workspace::create in this process throw IoError once its store is
// created and its provenance set, before the scratch directory is made.
// One-shot: the flag is cleared by the create that throws.
SLIDEIO_PROCESSOR_ENGINE_EXPORTS void failNextWorkspaceCreateForTesting() noexcept;

} // namespace slideio::processor::detail
