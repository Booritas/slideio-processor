// Requirements: UR-STG-8, UR-STG-11
#pragma once

// The state behind StalenessGraph: a view over the registry's edges, its mutex and its
// read-only refresh, and the answers it caches. Not installed.

#include <slideio/processor/storage/stage_state_reader.hpp>
#include <slideio/processor/workspace/staleness_graph.hpp>

#include <workspace/detail/staleness_edges.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <utility>
#include <vector>

namespace slideio::processor {

class StalenessGraph::Impl {
public:
    using Answer = std::vector<const detail::StaleEdge*>;

    // What the causes of a stage were computed against: the edges, the stale flags the
    // registry wrote, and the newest entry of the store's event journal, which every
    // status change (and so every completion time) appends in its own transaction.
    struct CausesKey {
        uint64_t edges = 0;
        uint64_t stale = 0;
        int64_t journal = 0;
        bool operator==(const CausesKey& other) const {
            return edges == other.edges && stale == other.stale && journal == other.journal;
        }
    };

    // The registry owns everything the functions reach and outlives the view; they run
    // under the mutex. `edgesGeneration` changes whenever the edges are replaced;
    // `causesKey` fills the key of the durable state the causes read, or returns false
    // when they must not be cached.
    Impl(std::mutex& mutexIn, std::function<void()> refreshIn, std::function<const detail::StalenessEdges&()> edgesIn,
         std::function<const StageStateReader&()> readerIn, std::function<uint64_t()> edgesGenerationIn,
         std::function<bool(CausesKey&)> causesKeyIn)
        : mutex(mutexIn), refresh(std::move(refreshIn)), edges(std::move(edgesIn)), reader(std::move(readerIn)),
          edgesGeneration(std::move(edgesGenerationIn)), causesKey(std::move(causesKeyIn)) {}

    // The forward answer of a stage, computed once per stage and set of edges: it depends
    // on the edges alone, and the pointers stay valid until they are replaced. Caller holds
    // the mutex. Throws InvalidArgumentError for an unknown stage.
    const Answer& answer(StageId stage) const {
        refresh();
        if (!edges().knows(stage.value())) throw InvalidArgumentError("the stage is not known to the registry");
        const uint64_t generation = edgesGeneration();
        if (generation != forwardGeneration) {
            forwardCache.clear();
            forwardGeneration = generation;
        }
        const auto found = forwardCache.find(stage.value());
        if (found != forwardCache.end()) return found->second;
        return forwardCache.emplace(stage.value(), edges().forward(stage.value())).first->second;
    }

    // One entry of the answer. Caller holds the mutex.
    const detail::StaleEdge& entry(StageId stage, int32_t index) const {
        const Answer& list = answer(stage);
        if (index < 0 || static_cast<size_t>(index) >= list.size()) {
            throw InvalidArgumentError("the affected-stage index is out of range");
        }
        return *list[static_cast<size_t>(index)];
    }

    // The causes of a stage's staleness, from the durable flags, statuses and completion
    // times (decision 3): every incoming edge whose source changed since the stage, or a
    // single nullptr when the stage is its own cause. Empty when it is not stale. Cached
    // per stage under causesKey(), which follows every change to the flags, statuses and
    // completion times the registry or a run makes; a read-only registry, whose writer is
    // another process that writes stale flags without a trace this one sees, computes
    // them every call. Caller holds the mutex. Throws InvalidArgumentError for an unknown
    // stage.
    const Answer& causes(StageId stage) const {
        refresh();
        if (!edges().knows(stage.value())) throw InvalidArgumentError("the stage is not known to the registry");
        CausesKey key;
        if (!causesKey(key)) {
            uncached = computeCauses(stage);
            return uncached;
        }
        if (!(key == causesGeneration)) {
            causesCache.clear();
            causesGeneration = key;
        }
        const auto found = causesCache.find(stage.value());
        if (found != causesCache.end()) return found->second;
        return causesCache.emplace(stage.value(), computeCauses(stage)).first->second;
    }

    // One cause; nullptr is the stage itself. Caller holds the mutex.
    const detail::StaleEdge* cause(StageId stage, int32_t index) const {
        const Answer& list = causes(stage);
        if (index < 0 || static_cast<size_t>(index) >= list.size()) {
            throw InvalidArgumentError("the cause index is out of range");
        }
        return list[static_cast<size_t>(index)];
    }

    std::mutex& mutex;
    std::function<void()> refresh;
    std::function<const detail::StalenessEdges&()> edges;
    std::function<const StageStateReader&()> reader;
    std::function<uint64_t()> edgesGeneration;
    std::function<bool(CausesKey&)> causesKey;

private:
    Answer computeCauses(StageId stage) const {
        const StageStateReader& states = reader();
        Answer result;
        if (!states.isStale(stage)) return result;
        const int64_t completed = states.completedAtUnixMs(stage);
        // Each source is read once, though several reasons can join it to the stage.
        bool read = false;
        int32_t lastSource = 0;
        bool lastChanged = false;
        for (const detail::StaleEdge* edge : edges().incoming(stage.value())) {
            if (!read || edge->from != lastSource) {
                read = true;
                lastSource = edge->from;
                lastChanged = changedSince(states, StageId(edge->from), completed);
            }
            if (lastChanged) result.push_back(edge);
        }
        if (result.empty()) result.push_back(nullptr);
        return result;
    }

    // Whether a source changed since the stage last completed (`completed`, 0 when it
    // never did). A stale source changed: marking it is what reaches the stage. A source
    // that never completed and is not stale never produced output, so nothing of it
    // changed: Pending, or a first run that is Running or Failed (a LayerOrdering target
    // runs without it). One that completed and is now Running or Failed has a re-run
    // under way or failed, which replaced the output the stage was built against; the
    // store keeps no time for it, so it counts whenever it happened. A Complete source
    // changed when it completed after the stage did, which a stage that never completed
    // has no time to compare with: only a stale or re-running source is its cause.
    static bool changedSince(const StageStateReader& states, StageId source, int64_t completed) {
        if (states.isStale(source)) return true;
        const int64_t sourceCompleted = states.completedAtUnixMs(source);
        if (sourceCompleted == 0) return false;
        if (states.status(source) != StageStatus::Complete) return true;
        return completed != 0 && sourceCompleted > completed;
    }

    // Guarded by the registry mutex, like everything the view reads.
    mutable uint64_t forwardGeneration = ~uint64_t(0);
    mutable std::map<int32_t, Answer> forwardCache;
    mutable CausesKey causesGeneration{~uint64_t(0), 0, 0};
    mutable std::map<int32_t, Answer> causesCache;
    mutable Answer uncached;
};

} // namespace slideio::processor
