// Requirements: UR-STG-8, UR-STG-11
#pragma once

// The state behind StalenessGraph: a view over the registry's edges, its mutex and its
// read-only refresh. Not installed.

#include <slideio/processor/storage/stage_state_reader.hpp>
#include <slideio/processor/workspace/staleness_graph.hpp>

#include <workspace/detail/staleness_edges.hpp>

#include <functional>
#include <mutex>
#include <set>
#include <utility>
#include <vector>

namespace slideio::processor {

class StalenessGraph::Impl {
public:
    // The registry owns all four and outlives the view; refresh runs under the mutex.
    Impl(std::mutex& mutexIn, std::function<void()> refreshIn, std::function<const detail::StalenessEdges&()> edgesIn,
         std::function<const StageStateReader&()> readerIn)
        : mutex(mutexIn), refresh(std::move(refreshIn)), edges(std::move(edgesIn)), reader(std::move(readerIn)) {}

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

    // The causes of a stage's staleness, from the durable flags, statuses and completion
    // times (decision 3): one incoming edge per source that changed since the stage, or a
    // single nullptr when the stage is its own cause. Empty when it is not stale. Caller
    // holds the mutex. Throws InvalidArgumentError for an unknown stage.
    std::vector<const detail::StaleEdge*> causes(StageId stage) const {
        refresh();
        if (!edges().knows(stage.value())) throw InvalidArgumentError("the stage is not known to the registry");
        const StageStateReader& states = reader();
        std::vector<const detail::StaleEdge*> result;
        if (!states.isStale(stage)) return result;
        const int64_t completed = states.completedAtUnixMs(stage);
        std::set<int32_t> reported;  // one cause per source stage: its first reason (the edges are ordered)
        for (const detail::StaleEdge* edge : edges().incoming(stage.value())) {
            const StageId source(edge->from);
            if (reported.count(edge->from) != 0) continue;
            if (states.isStale(source) || states.status(source) != StageStatus::Complete ||
                states.completedAtUnixMs(source) > completed) {
                reported.insert(edge->from);
                result.push_back(edge);
            }
        }
        if (result.empty()) result.push_back(nullptr);
        return result;
    }

    // One cause; nullptr is the stage itself. Caller holds the mutex.
    const detail::StaleEdge* cause(StageId stage, int32_t index) const {
        const std::vector<const detail::StaleEdge*> list = causes(stage);
        if (index < 0 || static_cast<size_t>(index) >= list.size()) {
            throw InvalidArgumentError("the cause index is out of range");
        }
        return list[static_cast<size_t>(index)];
    }

    std::mutex& mutex;
    std::function<void()> refresh;
    std::function<const detail::StalenessEdges&()> edges;
    std::function<const StageStateReader&()> reader;
};

} // namespace slideio::processor
