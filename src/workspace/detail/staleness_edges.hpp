// Requirements: UR-STG-8, UR-STG-11
#pragma once

// The edges of the staleness graph (SDS 04 4.5), derived from held stage definitions, and
// the forward walk over them. Used by StageRegistry and StalenessGraph; not installed.

#include <slideio/processor/storage/layer_registry.hpp>
#include <slideio/processor/storage/raster_store.hpp>
#include <slideio/processor/workspace/stage_definition.hpp>
#include <slideio/processor/workspace/staleness_graph.hpp>

#include <workspace/detail/stage_reads.hpp>

#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace slideio::processor::detail {

struct StaleEdge {
    int32_t from;
    int32_t to;
    StalenessReason reason;
    // Points into the DetailPool the edges were built with.
    const char* detail;
};

// Pairs (dependent, dependency) the registry's graph carries by name or pin rather than
// by reads: explicit dependsOn edges and the edges pinned around unreadable stages.
using DependencyPairs = std::set<std::pair<int32_t, int32_t>>;

// The detail strings of every edge ever built, each once. The owner keeps it for its own
// life and never removes from it, so a string handed out stays valid across rebuilds;
// it grows only by details not seen before.
using DetailPool = std::set<std::string>;

class StalenessEdges {
public:
    StalenessEdges() = default;
    // The graph is acyclic: the effective edges (every reason but LayerOrdering) come
    // from the registry's acyclic dependency graph, and a LayerOrdering edge U to V is
    // kept only when V does not already reach U. The LayerOrdering edges are decided in
    // ascending (U, V) order, against the effective edges and those kept so far. Details
    // are interned into `pool`. Throws InvalidArgumentError when a layer or raster a
    // definition names is unknown.
    StalenessEdges(const std::vector<StageRef>& stages, const DependencyPairs& namedDependencies,
                   const LayerRegistry& layers, const RasterRegistry& rasters, DetailPool& pool);

    bool knows(int32_t stage) const;
    // The forward answer from a stage: the edges in breadth-first order, each source once.
    std::vector<const StaleEdge*> forward(int32_t stage) const;
    // The edges that end at a stage, ordered by source, then reason.
    std::vector<const StaleEdge*> incoming(int32_t stage) const;

private:
    std::set<int32_t> stages_;
    // Sorted by (from, to, reason).
    std::vector<StaleEdge> edges_;
};

} // namespace slideio::processor::detail
