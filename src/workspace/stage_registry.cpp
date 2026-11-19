// Requirements: UR-STG-2, UR-STG-4, UR-STG-8
#include <slideio/processor/workspace/stage_registry.hpp>

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/log.hpp>
#include <slideio/processor/storage/layer_registry.hpp>
#include <slideio/processor/storage/raster_store.hpp>
#include <slideio/processor/storage/stage_state_reader.hpp>
#include <slideio/processor/storage/stage_state_writer.hpp>

#include <workspace/detail/stage_record.hpp>

#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace slideio::processor {

namespace {

struct Entry {
    int32_t id;
    const StageDefinition* definition;
};

struct Edge {
    int32_t dependency;
    bool isExplicit;
};

// Stage id to its effective dependencies, ascending by dependency id, each once.
using Graph = std::map<int32_t, std::vector<Edge>>;

std::string quoted(const char* name) { return std::string("\"") + name + "\""; }

bool writesAnyOf(const StageDefinition& writer, const StageDefinition& reader) {
    for (int32_t r = 0; r < reader.readLayerCount(); ++r) {
        const LayerId layer = reader.readLayerAt(r);
        for (int32_t w = 0; w < writer.writtenLayerCount(); ++w) {
            if (writer.writtenLayerAt(w) == layer) return true;
        }
    }
    if (writer.writesAnyRaster()) {
        for (int32_t r = 0; r < reader.readRasterCount(); ++r) {
            if (reader.readRasterAt(r) == writer.writtenRaster()) return true;
        }
    }
    return false;
}

// Derives the effective dependencies of every entry: writers of what it reads, and
// its explicit dependsOn names. Throws StageError for an unknown or self-named
// explicit dependency.
Graph deriveGraph(const std::vector<Entry>& entries) {
    std::map<std::string, int32_t> byName;
    for (const Entry& e : entries) byName[e.definition->name()] = e.id;

    Graph graph;
    for (const Entry& stage : entries) {
        std::map<int32_t, bool> deps;
        for (const Entry& other : entries) {
            if (other.id != stage.id && writesAnyOf(*other.definition, *stage.definition)) deps[other.id] |= false;
        }
        for (int32_t i = 0; i < stage.definition->explicitDependencyCount(); ++i) {
            const char* target = stage.definition->explicitDependencyAt(i);
            const auto found = byName.find(target);
            if (found == byName.end()) {
                throw StageError(("stage " + quoted(stage.definition->name()) + " depends on stage " +
                                  quoted(target) + ", which is not declared")
                                     .c_str());
            }
            if (found->second == stage.id) {
                throw StageError(("stage " + quoted(stage.definition->name()) + " depends on itself").c_str());
            }
            deps[found->second] = true;
        }
        std::vector<Edge>& edges = graph[stage.id];
        for (const auto& d : deps) edges.push_back({d.first, d.second});
    }
    return graph;
}

// Depth-first search for a cycle; throws StageError naming the stages in it.
void requireAcyclic(const Graph& graph, const std::vector<Entry>& entries) {
    std::map<int32_t, const char*> names;
    for (const Entry& e : entries) names[e.id] = e.definition->name();

    enum class Mark { Unvisited, OnPath, Done };
    std::map<int32_t, Mark> marks;
    std::vector<int32_t> path;
    std::function<void(int32_t)> visit = [&](int32_t id) {
        marks[id] = Mark::OnPath;
        path.push_back(id);
        for (const Edge& edge : graph.at(id)) {
            const Mark mark = marks[edge.dependency];
            if (mark == Mark::OnPath) {
                std::string text;
                const auto start = std::find(path.begin(), path.end(), edge.dependency);
                for (auto it = start; it != path.end(); ++it) text += quoted(names[*it]) + " -> ";
                text += quoted(names[edge.dependency]);
                throw StageError(("the stages form a dependency cycle: " + text).c_str());
            }
            if (mark == Mark::Unvisited) visit(edge.dependency);
        }
        path.pop_back();
        marks[id] = Mark::Done;
    };
    for (const Entry& e : entries) {
        if (marks[e.id] == Mark::Unvisited) visit(e.id);
    }
}


} // namespace

class StageRegistry::Impl {
public:
    explicit Impl(Store& storeIn) noexcept : store(storeIn) {}

    std::vector<Entry> entries() const {
        std::vector<Entry> result;
        for (const auto& d : definitions) result.push_back({d.first, d.second.get()});
        return result;
    }

    // Every id the definition names must be known to the store, and the pixel layer
    // is never written to.
    void validate(const StageDefinition& def) const {
        const LayerRegistry& layers = store.layers();
        const std::string stage = quoted(def.name());
        const auto requireLayer = [&](LayerId layer, const char* role) {
            if (!layers.exists(layer)) {
                throw InvalidArgumentError(("stage " + stage + " " + role + " layer id " +
                                            std::to_string(layer.value()) + ", which is not declared")
                                               .c_str());
            }
        };
        for (int32_t i = 0; i < def.readLayerCount(); ++i) requireLayer(def.readLayerAt(i), "reads");
        for (int32_t i = 0; i < def.writtenLayerCount(); ++i) {
            const LayerId layer = def.writtenLayerAt(i);
            requireLayer(layer, "writes");
            if (layer == layers.pixelLayer()) {
                throw InvalidArgumentError(("stage " + stage + " writes the pixel layer").c_str());
            }
        }
        for (int32_t i = 0; i < def.annotatedLayerCount(); ++i) requireLayer(def.annotatedLayerAt(i), "annotates");
        if (def.hasLayerRestriction()) requireLayer(def.layerRestriction(), "is restricted to");
        const RasterRegistry& rasters = store.rasters();
        const auto requireRaster = [&](RasterId raster, const char* role) {
            if (!rasters.exists(raster)) {
                throw InvalidArgumentError(("stage " + stage + " " + role + " raster id " +
                                            std::to_string(raster.value()) + ", which is not declared")
                                               .c_str());
            }
        };
        if (def.writesAnyRaster()) requireRaster(def.writtenRaster(), "writes");
        for (int32_t i = 0; i < def.readRasterCount(); ++i) requireRaster(def.readRasterAt(i), "reads");
    }

    // Derives the dependencies of every held stage and rewrites every stage's stored
    // rows (clear, then add), so a partial failure of an earlier declare is repaired by
    // the next. The in-memory graph changes only after the whole rewrite succeeds. One
    // function, so redeclaring a stage can reuse it.
    void deriveAndPersist() {
        Graph next = deriveGraph(entries());
        StageStateWriter& writer = store.stageStateWriter();
        for (const auto& stage : next) {
            writer.clearDependencies(StageId(stage.first));
            for (const Edge& edge : stage.second) writer.addDependency(StageId(stage.first), StageId(edge.dependency));
        }
        graph = std::move(next);
    }

    const std::vector<Edge>& edgesOf(StageId stage) const {
        const auto found = graph.find(stage.value());
        if (found == graph.end()) throw InvalidArgumentError("the stage is not known to the registry");
        return found->second;
    }

    // Sets the stale flag on the stage and on every stage reachable from it through the
    // reversed edges of the current graph. Caller holds the mutex.
    void markStaleLocked(StageId stage) {
        edgesOf(stage);
        std::vector<int32_t> pending{stage.value()};
        std::set<int32_t> seen{stage.value()};
        StageStateWriter& writer = store.stageStateWriter();
        while (!pending.empty()) {
            const int32_t current = pending.back();
            pending.pop_back();
            writer.setStale(StageId(current), true);
            for (const auto& other : graph) {
                for (const Edge& edge : other.second) {
                    if (edge.dependency == current && seen.insert(other.first).second) pending.push_back(other.first);
                }
            }
        }
    }

    // Rebuilds the definitions and the graph from the stored rows. A row whose record does
    // not parse is kept by name only: an empty definition, so no reads, no writes and no
    // edges, listed in `unreadable` so it is never runnable. Writes nothing, so it works
    // on a read-only store.
    void restore() {
        const StageStateReader& reader = store.stageStates();
        for (int32_t i = 0; i < reader.count(); ++i) {
            const StageId id = reader.at(i);
            const char* name = reader.name(id);
            std::unique_ptr<StageDefinition> def;
            try {
                def = std::make_unique<StageDefinition>(
                    detail::stageFromRecord(reader.configuration(id), store.layers(), store.rasters()));
                if (std::string(def->name()) != name) {
                    throw InvalidArgumentError("the record names a different stage");
                }
            } catch (const std::bad_alloc&) {
                throw;
            } catch (const std::exception& e) {
                logMessage(LogLevel::Warning,
                           "StageRegistry: the record of stage %s cannot be read (%s); the stage is kept by name only "
                           "and is never runnable",
                           quoted(name).c_str(), e.what());
                def = std::make_unique<StageDefinition>(name);
                unreadable.insert(id.value());
            }
            definitions[id.value()] = std::move(def);
        }
        try {
            graph = deriveGraph(entries());
        } catch (const StageError& e) {
            // Only a damaged store gets here (an explicit dependency on a stage that is
            // gone). Keep what was persisted, with every edge treated as derived.
            logMessage(LogLevel::Warning, "StageRegistry: the stored stages cannot be re-derived (%s); using the "
                                          "persisted dependencies",
                       e.what());
            graph.clear();
            for (const auto& d : definitions) {
                std::vector<Edge>& edges = graph[d.first];
                const StageId id(d.first);
                for (int32_t i = 0; i < reader.dependencyCount(id); ++i) {
                    edges.push_back({reader.dependencyAt(id, i).value(), false});
                }
            }
        }
    }

    Store& store;
    mutable std::mutex mutex;
    // Held by StageId; unique_ptr keeps references from definition() stable.
    std::map<int32_t, std::unique_ptr<StageDefinition>> definitions;
    // Stages restored by name only, whose record could not be read.
    std::set<int32_t> unreadable;
    Graph graph;
};

StageRegistry::StageRegistry(Store& store) : impl_(new Impl(store)) { impl_->restore(); }

StageRegistry::~StageRegistry() = default;

StageId StageRegistry::declare(const StageDefinition& definition) {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    Store& store = impl_->store;
    StageStateReader& reader = store.stageStates();
    StageId existing;
    const bool redeclared = reader.find(definition.name(), existing);
    impl_->validate(definition);
    const std::string record = detail::stageRecordJson(definition, store.layers(), store.rasters());

    // Validate the candidate graph in memory; nothing is written until it passes. A
    // redeclared stage takes the place of its held definition.
    std::vector<Entry> candidate = impl_->entries();
    if (redeclared) {
        for (Entry& e : candidate) {
            if (e.id == existing.value()) e.definition = &definition;
        }
    } else {
        candidate.push_back({std::numeric_limits<int32_t>::max(), &definition});
    }
    requireAcyclic(deriveGraph(candidate), candidate);

    // Everything that can run out of memory is allocated before the store is touched:
    // the copy, and for a new stage a map node parked under a placeholder key until the
    // id is known.
    auto copy = std::make_unique<StageDefinition>(definition);
    StageStateWriter& writer = store.stageStateWriter();

    if (redeclared) {
        const auto held = impl_->definitions.find(existing.value());
        if (held == impl_->definitions.end()) throw InvalidArgumentError("the stage has no definition in the registry");
        if (impl_->unreadable.count(existing.value()) == 0 && detail::sameDeclaration(*held->second, definition)) {
            return existing;
        }
        // Stale first, along the old dependents: if a later write fails the stage is
        // still marked, never silently current with a new record.
        impl_->markStaleLocked(existing);
        writer.setConfiguration(existing, record.c_str());
        held->second.swap(copy);
        impl_->unreadable.erase(existing.value());
        for (int32_t i = 0; i < definition.writtenLayerCount(); ++i) {
            writer.addWrittenLayer(existing, definition.writtenLayerAt(i));
        }
        impl_->deriveAndPersist();
        impl_->markStaleLocked(existing);
        return existing;
    }

    const int32_t placeholder = std::numeric_limits<int32_t>::min();
    impl_->definitions[placeholder] = std::move(copy);
    auto node = impl_->definitions.extract(placeholder);
    const StageId id = writer.declare(definition.name(), record.c_str());
    // The stage now exists in the store, so its definition is kept even if a later
    // write throws; the next declare rewrites every dependency row.
    node.key() = id.value();
    impl_->definitions.insert(std::move(node));
    for (int32_t i = 0; i < definition.writtenLayerCount(); ++i) writer.addWrittenLayer(id, definition.writtenLayerAt(i));
    impl_->deriveAndPersist();
    return id;
}

void StageRegistry::markStale(StageId stage) {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    impl_->markStaleLocked(stage);
}

void StageRegistry::clearStale(StageId stage) {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    impl_->edgesOf(stage);
    impl_->store.stageStateWriter().setStale(stage, false);
}

// Every accessor takes the registry mutex, so a reader never observes a declaration
// in progress through this registry.
#define SLIDEIO_REGISTRY_READ(expr)                                   \
    std::lock_guard<std::mutex> guard(impl_->mutex);                  \
    return impl_->store.stageStates().expr

int32_t StageRegistry::count() const noexcept { SLIDEIO_REGISTRY_READ(count()); }

StageId StageRegistry::at(int32_t index) const { SLIDEIO_REGISTRY_READ(at(index)); }

bool StageRegistry::find(const char* name, StageId& out) const noexcept { SLIDEIO_REGISTRY_READ(find(name, out)); }

const char* StageRegistry::name(StageId stage) const { SLIDEIO_REGISTRY_READ(name(stage)); }

StageStatus StageRegistry::status(StageId stage) const { SLIDEIO_REGISTRY_READ(status(stage)); }

bool StageRegistry::isStale(StageId stage) const { SLIDEIO_REGISTRY_READ(isStale(stage)); }

const char* StageRegistry::configuration(StageId stage) const { SLIDEIO_REGISTRY_READ(configuration(stage)); }

int64_t StageRegistry::completedAtUnixMs(StageId stage) const { SLIDEIO_REGISTRY_READ(completedAtUnixMs(stage)); }

int32_t StageRegistry::dependencyCount(StageId stage) const { SLIDEIO_REGISTRY_READ(dependencyCount(stage)); }

StageId StageRegistry::dependencyAt(StageId stage, int32_t index) const {
    SLIDEIO_REGISTRY_READ(dependencyAt(stage, index));
}

#undef SLIDEIO_REGISTRY_READ

bool StageRegistry::isExplicitDependency(StageId stage, int32_t index) const {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    const std::vector<Edge>& edges = impl_->edgesOf(stage);
    if (index < 0 || static_cast<size_t>(index) >= edges.size()) {
        throw InvalidArgumentError("the dependency index is out of range");
    }
    return edges[static_cast<size_t>(index)].isExplicit;
}

bool StageRegistry::isRunnable(StageId stage) const {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    if (impl_->unreadable.count(stage.value()) != 0) return false;
    const StageStateReader& reader = impl_->store.stageStates();
    const int32_t n = reader.dependencyCount(stage);
    for (int32_t i = 0; i < n; ++i) {
        const StageId dependency = reader.dependencyAt(stage, i);
        if (reader.status(dependency) != StageStatus::Complete || reader.isStale(dependency)) return false;
    }
    return true;
}

const StageDefinition& StageRegistry::definition(StageId stage) const {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    const auto found = impl_->definitions.find(stage.value());
    if (found == impl_->definitions.end()) throw InvalidArgumentError("the stage has no definition in the registry");
    return *found->second;
}

} // namespace slideio::processor
