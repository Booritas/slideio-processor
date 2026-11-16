// Requirements: UR-EXT-1, UR-EXT-3, UR-EXT-4
#include <pipeline/detail/operation_context_state.hpp>

#include <utility>

namespace slideio::processor::detail {

std::unique_ptr<OperationContext> OperationContextAccess::make(OperationContextState state) {
    std::unique_ptr<OperationContext> context(new OperationContext());
    context->impl_->state = std::move(state);
    return context;
}

std::unique_ptr<OperationContext> makeOperationContext(OperationContextState state) {
    return OperationContextAccess::make(std::move(state));
}

} // namespace slideio::processor::detail
