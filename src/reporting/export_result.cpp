// Requirements: UR-RES-9, UR-QA-3
#include <slideio/processor/reporting/export_result.hpp>

namespace slideio::processor {

ExportResult::ExportResult(int64_t objectsWritten, int64_t bytesWritten, bool wasCancelled) noexcept
    : objectsWritten_(objectsWritten), bytesWritten_(bytesWritten), wasCancelled_(wasCancelled) {}

int64_t ExportResult::objectsWritten() const noexcept { return objectsWritten_; }
int64_t ExportResult::bytesWritten() const noexcept { return bytesWritten_; }
bool ExportResult::wasCancelled() const noexcept { return wasCancelled_; }

} // namespace slideio::processor
