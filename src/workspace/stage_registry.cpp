// Requirements: UR-STG-2, UR-STG-4, UR-STG-8, UR-STG-10, UR-STG-11, UR-OBS-1
#include <slideio/processor/workspace/stage_registry.hpp>

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/log.hpp>
#include <slideio/processor/core/provenance.hpp>
#include <slideio/processor/storage/layer_registry.hpp>
#include <slideio/processor/storage/raster_store.hpp>
#include <slideio/processor/storage/stage_state_reader.hpp>
#include <slideio/processor/storage/stage_state_writer.hpp>

#include <workspace/detail/declare_fault.hpp>
#include <workspace/detail/refresh_fault.hpp>
#include <workspace/detail/stage_reads.hpp>
#include <workspace/detail/stage_record.hpp>
#include <workspace/detail/staleness_edges.hpp>
#include <workspace/staleness_graph_impl.hpp>

#include <algorithm>
#include <atomic>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace slideio::processor {

namespace {

std::atomic<bool> gReadStoredCountAsZero{false};

std::mutex gDeclareHookMutex;
void (*gDeclareHook)(void*) = nullptr;
void* gDeclareHookContext = nullptr;

// Runs and clears the hook set by hookNextDeclareMidwayForTesting, if any.
void runDeclareHook() {
    void (*hook)(void*) = nullptr;
    void* context = nullptr;
    {
        std::lock_guard<std::mutex> guard(gDeclareHookMutex);
        hook = std::exchange(gDeclareHook, nullptr);
        context = std::exchange(gDeclareHookContext, nullptr);
    }
    if (hook != nullptr) hook(context);
}

using Entry = detail::StageRef;
using detail::quoted;

struct Edge {
    int32_t dependency;
    bool isExplicit;
};

// Stage id to its effective dependencies, ascending by dependency id, each once.
using Graph = std::map<int32_t, std::vector<Edge>>;

// Derives the effective dependencies of every entry: writers of what it reads
// (detail::WriterIndex, shared with the staleness edges), and its explicit dependsOn
// names. A name no entry carries gives no edge until a stage of that name is declared
// (the next derivation resolves it). Throws StageError for a self-named explicit
// dependency.
Graph deriveGraph(const std::vector<Entry>& entries) {
    std::map<std::string, int32_t> byName;
    for (const Entry& e : entries) byName[e.definition->name()] = e.id;
    const detail::WriterIndex writers(entries);

    Graph graph;
    for (size_t p = 0; p < entries.size(); ++p) {
        const Entry& stage = entries[p];
        std::map<int32_t, bool> deps;
        for (const size_t source : writers.readSources(p)) deps[entries[source].id] |= false;
        for (int32_t i = 0; i < stage.definition->explicitDependencyCount(); ++i) {
            const char* target = stage.definition->explicitDependencyAt(i);
            const auto found = byName.find(target);
            if (found == byName.end()) continue;  // unresolved: no edge until declared
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

// Stage id to the positions (explicitDependencyAt) of its dependsOn names that no held
// stage carries yet, in declaration order. Positions, not strings, so the answer holds for
// an equal definition that replaces the one it was derived from.
using Unresolved = std::map<int32_t, std::vector<int32_t>>;

// The unresolved dependsOn names of every entry.
Unresolved deriveUnresolved(const std::vector<Entry>& entries) {
    std::set<std::string> names;
    for (const Entry& e : entries) names.insert(e.definition->name());
    Unresolved result;
    for (const Entry& e : entries) {
        std::vector<int32_t>& missing = result[e.id];
        for (int32_t i = 0; i < e.definition->explicitDependencyCount(); ++i) {
            if (names.count(e.definition->explicitDependencyAt(i)) == 0) missing.push_back(i);
        }
    }
    return result;
}

// Edges (stage, dependency) kept from the stored rows around a stage whose record could
// not be read: its empty definition derives none of them.
using Pins = std::set<std::pair<int32_t, int32_t>>;

// deriveGraph plus the pinned edges, each non-explicit unless already derived.
Graph deriveWithPins(const std::vector<Entry>& entries, const Pins& pins) {
    Graph graph = deriveGraph(entries);
    for (const auto& pin : pins) {
        std::vector<Edge>& edges = graph[pin.first];
        const auto at =
            std::find_if(edges.begin(), edges.end(), [&](const Edge& e) { return e.dependency == pin.second; });
        if (at != edges.end()) continue;
        edges.push_back({pin.second, false});
        std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) { return a.dependency < b.dependency; });
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

// Held by StageId; unique_ptr keeps references from definition() stable.
using Definitions = std::map<int32_t, std::unique_ptr<StageDefinition>>;

// The staleness edges (SDS 04 4.5) of the held definitions. The explicit and pinned
// edges of the effective graph are the ones reads do not derive. Built from the
// definitions, never from LayerRegistry::writerStageAt, which keeps a writer a
// redeclaration dropped.
detail::StalenessEdges buildStalenessEdges(const std::vector<Entry>& entries, const Graph& graph, const Pins& pins,
                                           const LayerRegistry& layers, const RasterRegistry& rasters,
                                           detail::DetailPool& pool) {
    detail::DependencyPairs named(pins.begin(), pins.end());
    for (const auto& stage : graph) {
        for (const Edge& edge : stage.second) {
            if (edge.isExplicit) named.insert({stage.first, edge.dependency});
        }
    }
    return detail::StalenessEdges(entries, named, layers, rasters, pool);
}

std::vector<Entry> entriesOf(const Definitions& definitions) {
    std::vector<Entry> result;
    for (const auto& d : definitions) result.push_back({d.first, d.second.get()});
    return result;
}

} // namespace

void detail::readNextStoredStageCountAsZeroForTesting() noexcept { gReadStoredCountAsZero.store(true); }

void detail::hookNextDeclareMidwayForTesting(void (*hook)(void*), void* context) noexcept {
    std::lock_guard<std::mutex> guard(gDeclareHookMutex);
    gDeclareHook = hook;
    gDeclareHookContext = context;
}

class StageRegistry::Impl {
public:
    explicit Impl(Store& storeIn) noexcept : store(storeIn) {}

    std::vector<Entry> entries() const { return entriesOf(definitions); }

    // Throws IoError when the store is closed: provenance() is a read every open store
    // answers, read-only included, so the caller learns the real cause rather than an
    // undeclared layer.
    void requireOpen(const char* operation) const {
        try {
            (void)store.provenance();
        } catch (const IoError& e) {
            throw IoError((std::string(operation) + ": the workspace store cannot be used: " + e.what()).c_str());
        }
    }

    // True when a writer elsewhere declared or redeclared a stage since the last
    // restore: either adds a row or changes a configuration. Stages are never removed,
    // so a count below the snapshot means the store cannot be read (it is closed), and
    // the snapshot stands.
    bool storeChanged() const {
        const StageStateReader& reader = store.stageStates();
        const auto held = static_cast<int32_t>(snapshot.size());
        const int32_t stored = reader.count();
        if (stored != held) return stored > held;
        for (const auto& s : snapshot) {
            if (s.second != reader.configuration(StageId(s.first))) return true;
        }
        return false;
    }

    // A read-only registry follows a live writer: before answering from its graph or
    // definitions it restores again when the stored stages changed. Caller holds the
    // mutex. A writer registry is the only writer, so its memory is current.
    void refreshIfReadOnly() {
        if (store.isReadOnly() && storeChanged()) restore();
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

    // What a declare derives from its candidate stages and pins, aside, and adopts once its
    // writes commit.
    struct Derived {
        Graph graph;
        detail::StalenessEdges edges;
        Unresolved unresolved;
    };

    // Derives the dependencies, staleness edges and unresolved names of the candidate.
    // Touches neither the store nor the registry's state, except to intern detail strings
    // into `details`, which only grows. Throws StageError for a cycle.
    Derived derive(const std::vector<Entry>& candidate, const Pins& candidatePins) {
        Derived next;
        next.graph = deriveWithPins(candidate, candidatePins);
        requireAcyclic(next.graph, candidate);
        next.unresolved = deriveUnresolved(candidate);
        next.edges =
            buildStalenessEdges(candidate, next.graph, candidatePins, store.layers(), store.rasters(), details);
        return next;
    }

    // Rewrites every stage's stored dependency rows (clear, then add) from a derived
    // graph. Runs inside the caller's batch, so the rows change with the rest of the
    // declare or not at all. Rewriting every stage also repairs the rows of a store
    // written before declares were one transaction, which can hold partial rows.
    void persist(const Graph& next) {
        StageStateWriter& writer = store.stageStateWriter();
        for (const auto& stage : next) {
            writer.clearDependencies(StageId(stage.first));
            for (const Edge& edge : stage.second) writer.addDependency(StageId(stage.first), StageId(edge.dependency));
        }
    }

    // Swaps in what a declare derived, once its writes have committed. Moves alone, so it
    // cannot throw and the registry never holds half of a declare.
    static_assert(std::is_nothrow_move_assignable<Graph>::value && std::is_nothrow_move_assignable<Pins>::value &&
                      std::is_nothrow_move_assignable<Unresolved>::value &&
                      std::is_nothrow_move_assignable<detail::StalenessEdges>::value,
                  "adopt swaps in by moves that cannot throw");
    void adopt(Derived& next, Pins& nextPins) noexcept {
        pins = std::move(nextPins);
        graph = std::move(next.graph);
        stalenessEdges = std::move(next.edges);
        unresolved = std::move(next.unresolved);
        ++edgesGeneration;
    }

    const std::vector<Edge>& edgesOf(StageId stage) const {
        const auto found = graph.find(stage.value());
        if (found == graph.end()) throw InvalidArgumentError("the stage is not known to the registry");
        return found->second;
    }

    // The positions of a held stage's dependsOn names that no held stage carries yet, in
    // declaration order, derived with the graph (deriveUnresolved). Caller holds the mutex.
    const std::vector<int32_t>& unresolvedOf(StageId stage) const {
        const auto found = unresolved.find(stage.value());
        if (found == unresolved.end()) throw InvalidArgumentError("the stage is not known to the registry");
        return found->second;
    }

    // One unresolved name. The string lives in the held definition, which is never freed
    // (a replaced one is retired). Caller holds the mutex.
    const char* unresolvedName(StageId stage, int32_t position) const {
        return definitions.at(stage.value())->explicitDependencyAt(position);
    }

    // The first reason the stage cannot run, in StageRunner's order, and the name it
    // concerns (nullptr for None and Unreadable) into `name` unless it is null: isRunnable
    // discards the name, so it does not read it. Caller holds the mutex.
    RunBlocker blockerLocked(StageId stage, const char** name) const {
        if (name != nullptr) *name = nullptr;
        const std::vector<Edge>& edges = edgesOf(stage);
        if (unreadable.count(stage.value()) != 0) return RunBlocker::Unreadable;
        const std::vector<int32_t>& missing = unresolvedOf(stage);
        if (!missing.empty()) {
            if (name != nullptr) *name = unresolvedName(stage, missing.front());
            return RunBlocker::UnresolvedDependency;
        }
        const StageStateReader& reader = store.stageStates();
        for (const Edge& edge : edges) {
            const StageId dependency(edge.dependency);
            const bool complete = reader.status(dependency) == StageStatus::Complete;
            if (!complete || reader.isStale(dependency)) {
                if (name != nullptr) *name = reader.name(dependency);
                return complete ? RunBlocker::DependencyStale : RunBlocker::DependencyIncomplete;
            }
        }
        return RunBlocker::None;
    }

    // Sets the stale flag on the stage and on every stage its forward answer lists, along
    // all five paths of the staleness graph, each once. Caller holds the mutex.
    void markStaleLocked(StageId stage) {
        edgesOf(stage);
        store.stageStateWriter().setStale(stage, true);
        markAffectedLocked(stage);
    }

    // Sets the stale flag on every stage the forward answer of `stage` lists, each once,
    // and not on the stage itself: the staleness graph is acyclic. Caller holds the mutex.
    void markAffectedLocked(StageId stage) {
        edgesOf(stage);
        StageStateWriter& writer = store.stageStateWriter();
        std::set<int32_t> seen;
        for (const detail::StaleEdge* edge : stalenessEdges.forward(stage.value())) {
            if (seen.insert(edge->to).second) writer.setStale(StageId(edge->to), true);
        }
    }

    // Rebuilds the definitions and the graph from the stored rows. A row whose record does
    // not parse, or whose explicit dependsOn names the stage itself,
    // is kept by name only: an empty definition, so no reads, no writes and no edges of
    // its own, listed in `unreadable` so it is never runnable. Everything is built aside
    // and swapped in at the end, so a failure leaves the registry as it was. Writes
    // nothing, so it works on a read-only store; a read-only registry runs it again
    // whenever a writer changes the stages.
    void restore() {
        const StageStateReader& reader = store.stageStates();
        Definitions nextDefinitions;
        std::set<int32_t> nextUnreadable;
        std::map<int32_t, std::string> nextSnapshot;
        const auto keepByName = [&](StageId id, const char* name) {
            nextDefinitions[id.value()] = std::make_unique<StageDefinition>(name);
            nextUnreadable.insert(id.value());
        };
        const int32_t stored = gReadStoredCountAsZero.exchange(false) ? 0 : reader.count();
        // Stages are never removed, so fewer than the snapshot holds means the store cannot
        // be read (count() answers 0 once it is closed): the snapshot stands.
        if (stored < static_cast<int32_t>(snapshot.size())) return;
        for (int32_t i = 0; i < stored; ++i) {
            const StageId id = reader.at(i);
            const char* name = reader.name(id);
            const std::string record = reader.configuration(id);
            nextSnapshot[id.value()] = record;
            try {
                auto def = std::make_unique<StageDefinition>(
                    detail::stageFromRecord(record.c_str(), store.layers(), store.rasters()));
                if (std::string(def->name()) != name) {
                    throw InvalidArgumentError("the record names a different stage");
                }
                nextDefinitions[id.value()] = std::move(def);
            } catch (const InvalidArgumentError& e) {
                warnUnreadable(name, e.what());
                keepByName(id, name);
            } catch (const InternalError& e) {
                warnUnreadable(name, e.what());
                keepByName(id, name);
            }
        }
        // A parsed record whose explicit dependency names the stage itself cannot be
        // derived: only a damaged store holds one. It is kept by name only like an
        // unreadable record, so its stored edges are pinned below. A name no stored stage
        // carries stays unresolved, as at declare.
        for (auto& d : nextDefinitions) {
            const StageDefinition& def = *d.second;
            for (int32_t i = 0; i < def.explicitDependencyCount(); ++i) {
                if (std::string(def.explicitDependencyAt(i)) == def.name()) {
                    logMessage(LogLevel::Warning,
                               "StageRegistry: stage %s depends on itself; the stage is kept by name only and is "
                               "never runnable",
                               quoted(def.name()).c_str());
                    const std::string name = def.name();
                    keepByName(StageId(d.first), name.c_str());
                    break;
                }
            }
        }
        // Pin the stored edges on either side of an unreadable stage: its empty definition
        // derives none of them, and dropping them would let a reader of its layers run.
        Pins nextPins;
        for (const auto& d : nextDefinitions) {
            const StageId id(d.first);
            for (int32_t i = 0; i < reader.dependencyCount(id); ++i) {
                const int32_t dependency = reader.dependencyAt(id, i).value();
                if (nextUnreadable.count(d.first) != 0 || nextUnreadable.count(dependency) != 0) {
                    nextPins.insert({d.first, dependency});
                }
            }
        }
        // A store can hold an unreadable stage's rows empty (one written before declares
        // were one transaction, by a writer stopped mid-rewrite); the pins already held for
        // a stage that is still unreadable stand, until it is redeclared. A writer that
        // redeclares an unreadable stage drops every pin around it, whatever the other end,
        // so a held pin with an end that was unreadable and now parses is dropped too. A
        // redeclaration commits its configuration and rows together, so reader and writer
        // end with the same pins (a commit landing during this restore pairs at most an old
        // configuration with new rows, and the next query restores again: SDS 04 4.3).
        const auto redeclared = [&](int32_t id) {
            return unreadable.count(id) != 0 && nextUnreadable.count(id) == 0;
        };
        for (const auto& p : pins) {
            const bool known = nextDefinitions.count(p.first) != 0 && nextDefinitions.count(p.second) != 0;
            if (known && !redeclared(p.first) && !redeclared(p.second) &&
                (nextUnreadable.count(p.first) != 0 || nextUnreadable.count(p.second) != 0)) {
                nextPins.insert(p);
            }
        }
        // No explicit dependency names its own stage now, so derivation cannot throw.
        const std::vector<Entry> nextEntries = entriesOf(nextDefinitions);
        Graph nextGraph = deriveWithPins(nextEntries, nextPins);
        Unresolved nextUnresolved = deriveUnresolved(nextEntries);
        detail::StalenessEdges nextEdges =
            buildStalenessEdges(nextEntries, nextGraph, nextPins, store.layers(), store.rasters(), details);

        // A definition whose record is unchanged is kept, and a replaced one retired
        // rather than destroyed, so a reference from definition() stays valid.
        retired.reserve(retired.size() + definitions.size());
        for (auto& d : nextDefinitions) {
            const auto held = definitions.find(d.first);
            const auto before = snapshot.find(d.first);
            if (held != definitions.end() && before != snapshot.end() && before->second == nextSnapshot[d.first] &&
                unreadable.count(d.first) == nextUnreadable.count(d.first)) {
                d.second = std::move(held->second);
            }
        }
        for (auto& d : definitions) {
            if (d.second) retired.push_back(std::move(d.second));
        }
        definitions = std::move(nextDefinitions);
        unreadable = std::move(nextUnreadable);
        snapshot = std::move(nextSnapshot);
        pins = std::move(nextPins);
        graph = std::move(nextGraph);
        stalenessEdges = std::move(nextEdges);
        unresolved = std::move(nextUnresolved);
        ++edgesGeneration;
    }

    static void warnUnreadable(const char* name, const char* cause) {
        logMessage(LogLevel::Warning,
                   "StageRegistry: the record of stage %s cannot be read (%s); the stage is kept by name only "
                   "and is never runnable",
                   quoted(name).c_str(), cause);
    }

    Store& store;
    mutable std::mutex mutex;
    Definitions definitions;
    // Stages restored by name only, whose record could not be read or derived.
    std::set<int32_t> unreadable;
    // Edges kept from the stored rows around unreadable stages, until they are redeclared.
    Pins pins;
    Graph graph;
    // Every detail string the staleness edges were ever built with, each once. Kept for the
    // life of the registry, like the retired definitions, so a string StalenessGraph handed
    // out survives every rebuild; it grows only by details not seen before.
    detail::DetailPool details;
    // The staleness edges of the held definitions, rebuilt with the graph. StalenessGraph
    // reads them under the mutex; their details point into `details`.
    detail::StalenessEdges stalenessEdges;
    // The unresolved dependsOn names of every held stage, derived with the graph.
    Unresolved unresolved;
    // Bumped whenever stalenessEdges is replaced: StalenessGraph keys the answers it
    // caches on it.
    uint64_t edgesGeneration = 0;
    std::unique_ptr<StalenessGraph> staleness;
    // The stored configuration of every stage at the last restore, by id: a read-only
    // registry compares it with the store to tell whether a writer changed the stages.
    std::map<int32_t, std::string> snapshot;
    // Definitions replaced by a refresh or a redeclare, kept for the life of the registry
    // so a reference from definition() never dangles.
    std::vector<std::unique_ptr<StageDefinition>> retired;
};

StageRegistry::StageRegistry(Store& store) : impl_(new Impl(store)) {
    Impl* impl = impl_.get();
    impl->staleness.reset(new StalenessGraph(std::make_unique<StalenessGraph::Impl>(
        impl->mutex, [impl] { impl->refreshIfReadOnly(); },
        [impl]() -> const detail::StalenessEdges& { return impl->stalenessEdges; },
        [impl]() -> const StageStateReader& { return impl->store.stageStates(); },
        [impl] { return impl->edgesGeneration; },
        [impl](StalenessGraph::Impl::CausesKey& key) {
            // The writer of a read-only store is another process, whose changes advance no
            // revision here, so a read-only registry has no key that follows them. A closed
            // store is not answered from the cache either: the read then throws IoError,
            // as causeCount() documents.
            if (impl->store.isReadOnly() || !impl->store.isOpen()) return false;
            key.edges = impl->edgesGeneration;
            key.states = impl->store.stageStateWriter().revision();
            return true;
        })));
    impl_->restore();
}

StageRegistry::~StageRegistry() = default;

StageId StageRegistry::declare(const StageDefinition& definition) {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    impl_->requireOpen("StageRegistry::declare");
    impl_->refreshIfReadOnly();
    Store& store = impl_->store;
    StageStateReader& reader = store.stageStates();
    StageId existing;
    const bool redeclared = reader.find(definition.name(), existing);
    impl_->validate(definition);
    const std::string record = detail::stageRecordJson(definition, store.layers(), store.rasters());

    // A declare computes everything aside, writes its rows in one batch, and adopts what it
    // computed once the batch commits: if anything throws before the commit returns, the
    // batch rolls back and the registry is as it was, so a declare is all or nothing. The
    // candidate graph is validated in memory, before anything is written. A redeclared
    // stage takes the place of its held definition.
    std::vector<Entry> candidate = impl_->entries();
    if (redeclared) {
        for (Entry& e : candidate) {
            if (e.id == existing.value()) e.definition = &definition;
        }
    } else {
        candidate.push_back({std::numeric_limits<int32_t>::max(), &definition});
    }
    // Redeclaring an unreadable stage drops the edges pinned around it.
    Pins candidatePins = impl_->pins;
    if (redeclared && impl_->unreadable.count(existing.value()) != 0) {
        for (auto it = candidatePins.begin(); it != candidatePins.end();) {
            const bool around = it->first == existing.value() || it->second == existing.value();
            it = around ? candidatePins.erase(it) : std::next(it);
        }
    }
    StageStateWriter& writer = store.stageStateWriter();
    const auto addWrittenLayers = [&](StageId stage) {
        for (int32_t i = 0; i < definition.writtenLayerCount(); ++i) {
            writer.addWrittenLayer(stage, definition.writtenLayerAt(i));
        }
    };

    if (redeclared) {
        const auto held = impl_->definitions.find(existing.value());
        if (held == impl_->definitions.end()) throw InvalidArgumentError("the stage has no definition in the registry");
        if (impl_->unreadable.count(existing.value()) == 0 && detail::sameDeclaration(*held->second, definition)) {
            // Nothing changes, but the written layers and every stage's rows are written
            // again, repeating harmlessly: a store written before declares were one
            // transaction can hold the partial rows of a declare that failed part way, and
            // this repairs them. A read-only store writes nothing. Derived from the held
            // definitions, which stay: sameDeclaration ignores list order, so the caller's
            // dependsOn positions can differ from the held ones that unresolved indexes.
            // The stage is not unreadable, so its candidate pins are the held ones.
            if (!store.isReadOnly()) {
                Impl::Derived next = impl_->derive(impl_->entries(), candidatePins);
                StageStateWriter::Batch batch(writer);
                addWrittenLayers(existing);
                impl_->persist(next.graph);
                batch.commit();
                impl_->adopt(next, candidatePins);
            }
            return existing;
        }
        Impl::Derived next = impl_->derive(candidate, candidatePins);
        // Stale: the stage itself, and every stage its forward answer lists under the held
        // edges and under the candidate's, each once, so a dependent it drops goes stale as
        // well as one it gains.
        std::vector<int32_t> stale{existing.value()};
        std::set<int32_t> marked{existing.value()};
        for (const detail::StalenessEdges* edges : {&impl_->stalenessEdges, &next.edges}) {
            for (const detail::StaleEdge* edge : edges->forward(existing.value())) {
                if (marked.insert(edge->to).second) stale.push_back(edge->to);
            }
        }
        // Allocated before the store is touched, so the swap after the commit cannot
        // throw: the copy, and room to retire the replaced definition rather than destroy
        // it, so references and strings handed out from it stay valid.
        auto copy = std::make_unique<StageDefinition>(definition);
        impl_->retired.reserve(impl_->retired.size() + 1);
        {
            // A read-only store opens no batch: its first write refuses with the IoError
            // it always has, and nothing is written.
            std::optional<StageStateWriter::Batch> batch;
            if (!store.isReadOnly()) batch.emplace(writer);
            for (const int32_t stage : stale) writer.setStale(StageId(stage), true);
            writer.setConfiguration(existing, record.c_str());
            runDeclareHook();
            addWrittenLayers(existing);
            impl_->persist(next.graph);
            if (batch) batch->commit();
        }
        held->second.swap(copy);
        impl_->retired.push_back(std::move(copy));  // capacity reserved above: cannot throw
        impl_->unreadable.erase(existing.value());
        impl_->adopt(next, candidatePins);
        return existing;
    }

    // Refused before anything is written; derived again below once the id is known.
    requireAcyclic(deriveWithPins(candidate, candidatePins), candidate);
    // Allocated before the store is touched, so the swap after the commit cannot throw:
    // the copy, in a map node parked under a placeholder key until the id is known.
    const int32_t placeholder = std::numeric_limits<int32_t>::min();
    impl_->definitions[placeholder] = std::make_unique<StageDefinition>(definition);
    auto node = impl_->definitions.extract(placeholder);
    std::optional<StageStateWriter::Batch> batch;  // none on a read-only store, as above
    if (!store.isReadOnly()) batch.emplace(writer);
    const StageId id = writer.declare(definition.name(), record.c_str());
    runDeclareHook();
    candidate.back().id = id.value();  // the new stage's entry, re-keyed from its placeholder
    Impl::Derived next = impl_->derive(candidate, candidatePins);
    addWrittenLayers(id);
    impl_->persist(next.graph);
    if (batch) batch->commit();
    node.key() = id.value();
    impl_->definitions.insert(std::move(node));
    impl_->adopt(next, candidatePins);
    return id;
}

void StageRegistry::markStale(StageId stage) {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    impl_->requireOpen("StageRegistry::markStale");
    if (impl_->store.isReadOnly()) throw IoError("StageRegistry::markStale: the workspace is read-only");
    impl_->markStaleLocked(stage);
}

void StageRegistry::markAffectedStale(StageId stage) {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    impl_->requireOpen("StageRegistry::markAffectedStale");
    if (impl_->store.isReadOnly()) throw IoError("StageRegistry::markAffectedStale: the workspace is read-only");
    impl_->markAffectedLocked(stage);
}

const StalenessGraph& StageRegistry::staleness() const noexcept { return *impl_->staleness; }

void StageRegistry::clearStale(StageId stage) {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    impl_->requireOpen("StageRegistry::clearStale");
    if (impl_->store.isReadOnly()) throw IoError("StageRegistry::clearStale: the workspace is read-only");
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

#undef SLIDEIO_REGISTRY_READ

// The dependency accessors read the registry's own graph, the one source (it holds the
// edges pinned around unreadable stages, which the stored rows also carry). In a
// read-only registry each first follows a live writer's changes (refreshIfReadOnly).
int32_t StageRegistry::dependencyCount(StageId stage) const {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    impl_->refreshIfReadOnly();
    return static_cast<int32_t>(impl_->edgesOf(stage).size());
}

StageId StageRegistry::dependencyAt(StageId stage, int32_t index) const {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    impl_->refreshIfReadOnly();
    const std::vector<Edge>& edges = impl_->edgesOf(stage);
    if (index < 0 || static_cast<size_t>(index) >= edges.size()) {
        throw InvalidArgumentError("the dependency index is out of range");
    }
    return StageId(edges[static_cast<size_t>(index)].dependency);
}

bool StageRegistry::isExplicitDependency(StageId stage, int32_t index) const {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    impl_->refreshIfReadOnly();
    const std::vector<Edge>& edges = impl_->edgesOf(stage);
    if (index < 0 || static_cast<size_t>(index) >= edges.size()) {
        throw InvalidArgumentError("the dependency index is out of range");
    }
    return edges[static_cast<size_t>(index)].isExplicit;
}

int32_t StageRegistry::unresolvedDependencyCount(StageId stage) const {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    impl_->refreshIfReadOnly();
    return static_cast<int32_t>(impl_->unresolvedOf(stage).size());
}

const char* StageRegistry::unresolvedDependencyAt(StageId stage, int32_t index) const {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    impl_->refreshIfReadOnly();
    const std::vector<int32_t>& positions = impl_->unresolvedOf(stage);
    if (index < 0 || static_cast<size_t>(index) >= positions.size()) {
        throw InvalidArgumentError("the unresolved dependency index is out of range");
    }
    return impl_->unresolvedName(stage, positions[static_cast<size_t>(index)]);
}

bool StageRegistry::isRunnable(StageId stage) const {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    impl_->refreshIfReadOnly();
    return impl_->blockerLocked(stage, nullptr) == RunBlocker::None;
}

RunBlocker StageRegistry::runBlocker(StageId stage, const char*& name) const {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    impl_->refreshIfReadOnly();
    return impl_->blockerLocked(stage, &name);
}

bool StageRegistry::isUnreadable(StageId stage) const {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    impl_->refreshIfReadOnly();
    impl_->edgesOf(stage);
    return impl_->unreadable.count(stage.value()) != 0;
}

const StageDefinition& StageRegistry::definition(StageId stage) const {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    impl_->refreshIfReadOnly();
    const auto found = impl_->definitions.find(stage.value());
    if (found == impl_->definitions.end()) throw InvalidArgumentError("the stage has no definition in the registry");
    return *found->second;
}

} // namespace slideio::processor
