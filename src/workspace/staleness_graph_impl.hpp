// Requirements: UR-STG-8, UR-STG-11
#pragma once

// The state behind StalenessGraph: a view over the registry's edges, its mutex and its
// read-only refresh. Not installed.

#include <slideio/processor/workspace/staleness_graph.hpp>

#include <workspace/detail/staleness_edges.hpp>

#include <functional>
#include <mutex>
#include <utility>

namespace slideio::processor {

class StalenessGraph::Impl {
public:
    // The registry owns all three and outlives the view; refresh runs under the mutex.
    Impl(std::mutex& mutexIn, std::function<void()> refreshIn, std::function<const detail::StalenessEdges&()> edgesIn)
        : mutex(mutexIn), refresh(std::move(refreshIn)), edges(std::move(edgesIn)) {}

    // The forward answer of a stage. Caller holds the mutex. Throws InvalidArgumentError
    // for an unknown stage.
    std::vector<const detail::StaleEdge*> answer(StageId stage) const {
        refresh();
        if (!edges().knows(stage.value())) throw InvalidArgumentError("the stage is not known to the registry");
        return edges().forward(stage.value());
    }

    // One entry of the answer. Caller holds the mutex.
    const detail::StaleEdge& entry(StageId stage, int32_t index) const {
        const std::vector<const detail::StaleEdge*> list = answer(stage);
        if (index < 0 || static_cast<size_t>(index) >= list.size()) {
            throw InvalidArgumentError("the affected-stage index is out of range");
        }
        return *list[static_cast<size_t>(index)];
    }

    std::mutex& mutex;
    std::function<void()> refresh;
    std::function<const detail::StalenessEdges&()> edges;
};

} // namespace slideio::processor
