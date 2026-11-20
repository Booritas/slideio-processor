// Requirements: UR-STG-5, UR-STG-6
#pragma once

// Test seams, not API: exported from the engine but declared only here, under src/, so
// no public header carries them. They reach failures of StageRunner's failure recording
// and of its re-run discards, which no argument or storage state can reach
// deterministically.

#include <slideio/processor/pipeline/export.hpp>

namespace slideio::processor::detail {

// Makes every step of the next StageRunner failure recording in this process throw
// IoError, so the recording is seen to fail while the run's own error propagates.
// One-shot: consumed by the recording that uses it.
SLIDEIO_PROCESSOR_ENGINE_EXPORTS void failNextStageFailureRecordingForTesting() noexcept;

// Makes the first discard of the next StageRunner re-run in this process throw IoError,
// after the dependents were marked stale and the stage set Running. One-shot: consumed by
// the re-run that uses it.
SLIDEIO_PROCESSOR_ENGINE_EXPORTS void failNextRerunDiscardForTesting() noexcept;

} // namespace slideio::processor::detail
