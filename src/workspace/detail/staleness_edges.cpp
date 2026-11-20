// Requirements: UR-STG-8, UR-STG-11
#include <workspace/detail/staleness_edges.hpp>

#include <algorithm>
#include <deque>

namespace slideio::processor::detail {

namespace {

std::string quoted(const char* name) { return std::string("\"") + name + "\""; }

bool writesLayer(const StageDefinition& def, LayerId layer) {
    for (int32_t i = 0; i < def.writtenLayerCount(); ++i) {
        if (def.writtenLayerAt(i) == layer) return true;
    }
    return false;
}

} // namespace

StalenessEdges::StalenessEdges(const std::vector<StageRef>& stages, const DependencyPairs& namedDependencies,
                               const LayerRegistry& layers, const RasterRegistry& rasters) {
    for (const StageRef& s : stages) stages_.insert(s.id);
    const auto semantic = [&](LayerId layer) { return layers.kind(layer) == LayerKind::Semantic; };
    const auto writesSemantic = [&](const StageDefinition& def) {
        for (int32_t i = 0; i < def.writtenLayerCount(); ++i) {
            if (semantic(def.writtenLayerAt(i))) return true;
        }
        return false;
    };

    for (const StageRef& source : stages) {
        for (const StageRef& target : stages) {
            if (source.id == target.id) continue;
            const StageDefinition& u = *source.definition;
            const StageDefinition& v = *target.definition;
            const std::string by = " written by stage " + quoted(u.name());
            std::string layerRead, derivedFrom, rasterRead, ordering, dependency;

            // A primitive layer read by a stage that writes a semantic layer is a
            // derivation, reported as DerivedFrom instead of LayerRead.
            const bool derives = writesSemantic(v);
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
            if (u.writesAnyRaster()) {
                for (int32_t i = 0; i < v.readRasterCount(); ++i) {
                    if (v.readRasterAt(i) == u.writtenRaster()) {
                        rasterRead = "reads raster " + quoted(rasters.name(u.writtenRaster())) + by;
                        break;
                    }
                }
            }
            // V writes a layer strictly above one U writes in the layer hierarchy.
            for (int32_t a = 0; a < v.writtenLayerCount() && ordering.empty(); ++a) {
                const LayerId upper = v.writtenLayerAt(a);
                if (!semantic(upper)) continue;
                for (int32_t b = 0; b < u.writtenLayerCount(); ++b) {
                    const LayerId lower = u.writtenLayerAt(b);
                    if (semantic(lower) && layers.isAbove(upper, lower)) {
                        ordering = "writes layer " + quoted(layers.name(upper)) + " below layer " +
                                   quoted(layers.name(lower)) + by;
                        break;
                    }
                }
            }
            if (namedDependencies.count({target.id, source.id}) != 0) {
                dependency = "depends on stage " + quoted(u.name());
            }

            const auto add = [&](StalenessReason reason, std::string& text) {
                if (!text.empty()) edges_.push_back({source.id, target.id, reason, std::move(text)});
            };
            add(StalenessReason::LayerRead, layerRead);
            add(StalenessReason::RasterRead, rasterRead);
            add(StalenessReason::LayerOrdering, ordering);
            add(StalenessReason::DerivedFrom, derivedFrom);
            add(StalenessReason::StageDependency, dependency);
        }
    }
    // Sources and targets arrive in id order and reasons in enum order; sorting keeps the
    // documented order exact whatever order the caller listed the stages in.
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

} // namespace slideio::processor::detail
