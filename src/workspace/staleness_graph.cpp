// Requirements: UR-STG-8, UR-STG-11
#include <slideio/processor/workspace/staleness_graph.hpp>

#include <slideio/processor/core/error.hpp>

#include <workspace/staleness_graph_impl.hpp>

#include <mutex>

namespace slideio::processor {

StalenessGraph::StalenessGraph(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

StalenessGraph::~StalenessGraph() = default;

int32_t StalenessGraph::affectedCount(StageId stage) const {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    return static_cast<int32_t>(impl_->answer(stage).size());
}

StageId StalenessGraph::affectedAt(StageId stage, int32_t index) const {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    return StageId(impl_->entry(stage, index).to);
}

StalenessReason StalenessGraph::reasonAt(StageId stage, int32_t index) const {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    return impl_->entry(stage, index).reason;
}

const char* StalenessGraph::reasonDetailAt(StageId stage, int32_t index) const {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    return impl_->entry(stage, index).detail.c_str();
}

} // namespace slideio::processor
