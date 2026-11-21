// Requirements: UR-OBS-1
#pragma once

// Test seam, not API: exported from the engine but declared only here, under src/, so no
// public header carries it. It reaches the race of a read-only refresh whose store closes
// between the change check and the restore, which no argument or storage state can reach
// deterministically.

#include <slideio/processor/pipeline/export.hpp>

namespace slideio::processor::detail {

// Makes the next StageRegistry restore in this process read the stored stage count as 0,
// as StageStateReader::count() does once the store is closed. One-shot: consumed by the
// restore that uses it.
SLIDEIO_PROCESSOR_ENGINE_EXPORTS void readNextStoredStageCountAsZeroForTesting() noexcept;

} // namespace slideio::processor::detail
