// Requirements: UR-STG-8, UR-STG-11
#include <workspace/detail/staleness_edges.hpp>

#include <algorithm>
#include <deque>
#include <limits>
#include <map>

namespace slideio::processor::detail {

namespace {

// An edge before its detail is interned.
struct Candidate {
    int32_t from;
    int32_t to;
    StalenessReason reason;
    std::string detail;
};

using Adjacency = std::map<int32_t, std::set<int32_t>>;

// True when `to` is reachable from `from` along `adjacency` (a stage reaches itself).
bool reaches(const Adjacency& adjacency, int32_t from, int32_t to) {
    std::vector<int32_t> pending{from};
    std::set<int32_t> seen{from};
    while (!pending.empty()) {
        const int32_t current = pending.back();
        pending.pop_back();
        if (current == to) return true;
        const auto found = adjacency.find(current);
        if (found == adjacency.end()) continue;
        for (const int32_t next : found->second) {
            if (seen.insert(next).second) pending.push_back(next);
        }
    }
    return false;
}

} // namespace

StalenessEdges::StalenessEdges(const std::vector<StageRef>& stages, const DependencyPairs& namedDependencies,
                               const LayerRegistry& layers, const RasterRegistry& rasters, DetailPool& pool) {
    for (const StageRef& s : stages) stages_.insert(s.id);
    // LayerRegistry takes its mutex on every call, so each layer's kind and each pair's
    // ancestry is looked up once per build.
    std::map<int32_t, bool> semanticOf;
    const auto semantic = [&](LayerId layer) {
        auto found = semanticOf.find(layer.value());
        if (found == semanticOf.end()) {
            found = semanticOf.emplace(layer.value(), layers.kind(layer) == LayerKind::Semantic).first;
        }
        return found->second;
    };
    std::map<std::pair<int32_t, int32_t>, bool> aboveOf;
    const auto above = [&](LayerId upper, LayerId lower) {
        const std::pair<int32_t, int32_t> key(upper.value(), lower.value());
        auto found = aboveOf.find(key);
        if (found == aboveOf.end()) found = aboveOf.emplace(key, layers.isAbove(upper, lower)).first;
        return found->second;
    };
    const auto writesSemantic = [&](const StageDefinition& def) {
        for (int32_t i = 0; i < def.writtenLayerCount(); ++i) {
            if (semantic(def.writtenLayerAt(i))) return true;
        }
        return false;
    };
    std::map<int32_t, size_t> positionOf;
    for (size_t p = 0; p < stages.size(); ++p) positionOf[stages[p].id] = p;
    const WriterIndex writers(stages);

    std::vector<Candidate> effective;
    std::vector<Candidate> orderings;
    for (size_t t = 0; t < stages.size(); ++t) {
        const StageRef& target = stages[t];
        const StageDefinition& v = *target.definition;

        // The stages that can have an edge to V, so only real pairs are examined: the
        // writers of what V reads, the writers of a semantic layer below one V writes, and
        // the stages V depends on by name or pin.
        std::vector<size_t> sources = writers.readSources(t);
        for (int32_t a = 0; a < v.writtenLayerCount(); ++a) {
            const LayerId upper = v.writtenLayerAt(a);
            if (!semantic(upper)) continue;
            for (const auto& written : writers.writtenLayers()) {
                const LayerId lower(written.first);
                if (semantic(lower) && above(upper, lower)) {
                    sources.insert(sources.end(), written.second.begin(), written.second.end());
                }
            }
        }
        for (auto it = namedDependencies.lower_bound({target.id, std::numeric_limits<int32_t>::min()});
             it != namedDependencies.end() && it->first == target.id; ++it) {
            const auto found = positionOf.find(it->second);
            if (found != positionOf.end()) sources.push_back(found->second);
        }
        std::sort(sources.begin(), sources.end());
        sources.erase(std::unique(sources.begin(), sources.end()), sources.end());

        const bool derives = writesSemantic(v);
        for (const size_t s : sources) {
            if (s == t) continue;
            const StageRef& source = stages[s];
            const StageDefinition& u = *source.definition;
            const std::string by = " written by stage " + quoted(u.name());
            std::string layerRead, derivedFrom, rasterRead, ordering, dependency;

            // A primitive layer read by a stage that writes a semantic layer is a
            // derivation, reported as DerivedFrom instead of LayerRead.
            for (int32_t i = 0; i < v.readLayerCount(); ++i) {
                const LayerId layer = v.readLayerAt(i);
                if (!writesLayer(u, layer)) continue;
                const bool derivation = derives && !semantic(layer);
                std::string& slot = derivation ? derivedFrom : layerRead;
                if (slot.empty()) {
                    slot = std::string(derivation ? "reads primitive layer " : "reads layer ") +
                           quoted(layers.name(layer)) + by;
                }
            }
            if (readsRasterOf(v, u)) rasterRead = "reads raster " + quoted(rasters.name(u.writtenRaster())) + by;
            // V writes a layer strictly above one U writes in the layer hierarchy.
            for (int32_t a = 0; a < v.writtenLayerCount() && ordering.empty(); ++a) {
                const LayerId upper = v.writtenLayerAt(a);
                if (!semantic(upper)) continue;
                for (int32_t b = 0; b < u.writtenLayerCount(); ++b) {
                    const LayerId lower = u.writtenLayerAt(b);
                    if (semantic(lower) && above(upper, lower)) {
                        ordering = "writes layer " + quoted(layers.name(upper)) + " above layer " +
                                   quoted(layers.name(lower)) + by;
                        break;
                    }
                }
            }
            if (namedDependencies.count({target.id, source.id}) != 0) {
                dependency = "depends on stage " + quoted(u.name());
            }

            const auto add = [&](StalenessReason reason, std::string& text) {
                if (text.empty()) return;
                std::vector<Candidate>& list = reason == StalenessReason::LayerOrdering ? orderings : effective;
                list.push_back({source.id, target.id, reason, std::move(text)});
            };
            // DerivedFrom replaces LayerRead for the whole pair, not layer by layer.
            if (!derivedFrom.empty()) layerRead.clear();
            add(StalenessReason::LayerRead, layerRead);
            add(StalenessReason::RasterRead, rasterRead);
            add(StalenessReason::LayerOrdering, ordering);
            add(StalenessReason::DerivedFrom, derivedFrom);
            add(StalenessReason::StageDependency, dependency);
        }
    }

    // The graph stays acyclic, so re-running every stale stage settles. The effective
    // edges come from the registry's acyclic dependency graph; a LayerOrdering edge U to V
    // that would close a cycle (V already reaches U) is suppressed. The case is a stage
    // reading a layer above the one it writes (`cells` reading `nuclei`): that stage
    // re-asserts the parent links, so its effective edge from the `nuclei` writer wins.
    // The LayerOrdering edges are decided in ascending (U, V) order, so the answer does
    // not depend on the order the stages were listed in.
    Adjacency adjacency;
    for (const Candidate& c : effective) adjacency[c.from].insert(c.to);
    std::sort(orderings.begin(), orderings.end(), [](const Candidate& a, const Candidate& b) {
        return a.from != b.from ? a.from < b.from : a.to < b.to;
    });
    for (Candidate& c : orderings) {
        if (reaches(adjacency, c.to, c.from)) continue;
        adjacency[c.from].insert(c.to);
        effective.push_back(std::move(c));
    }

    edges_.reserve(effective.size());
    for (Candidate& c : effective) {
        const std::string& interned = *pool.insert(std::move(c.detail)).first;
        edges_.push_back({c.from, c.to, c.reason, interned.c_str()});
    }
    // The kept LayerOrdering edges arrive after the others; sorting keeps the
    // documented order exact, whatever order the caller listed the stages in.
    std::stable_sort(edges_.begin(), edges_.end(), [](const StaleEdge& a, const StaleEdge& b) {
        if (a.from != b.from) return a.from < b.from;
        if (a.to != b.to) return a.to < b.to;
        return static_cast<int32_t>(a.reason) < static_cast<int32_t>(b.reason);
    });
}

bool StalenessEdges::knows(int32_t stage) const { return stages_.count(stage) != 0; }

std::vector<const StaleEdge*> StalenessEdges::forward(int32_t stage) const {
    std::vector<const StaleEdge*> result;
    std::deque<int32_t> pending{stage};
    std::set<int32_t> expanded{stage};
    while (!pending.empty()) {
        const int32_t current = pending.front();
        pending.pop_front();
        const auto first = std::lower_bound(edges_.begin(), edges_.end(), current,
                                            [](const StaleEdge& e, int32_t from) { return e.from < from; });
        for (auto it = first; it != edges_.end() && it->from == current; ++it) {
            result.push_back(&*it);
            if (expanded.insert(it->to).second) pending.push_back(it->to);
        }
    }
    return result;
}

std::vector<const StaleEdge*> StalenessEdges::incoming(int32_t stage) const {
    std::vector<const StaleEdge*> result;
    for (const StaleEdge& edge : edges_) {
        if (edge.to == stage) result.push_back(&edge);
    }
    return result;
}

} // namespace slideio::processor::detail
