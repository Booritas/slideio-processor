// Requirements: UR-STG-5
#pragma once

// A test seam, not API: exported from the engine but declared only here, under src/, so
// no public header carries it. It reaches the failure of StageRunner's failure recording,
// which no argument or storage state can reach deterministically.

#include <slideio/processor/pipeline/export.hpp>

namespace slideio::processor::detail {

// Makes every step of the next StageRunner failure recording in this process throw
// IoError, so the recording is seen to fail while the run's own error propagates.
// One-shot: consumed by the recording that uses it.
SLIDEIO_PROCESSOR_ENGINE_EXPORTS void failNextStageFailureRecordingForTesting() noexcept;

} // namespace slideio::processor::detail
