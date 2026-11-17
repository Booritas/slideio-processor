// Requirements: UR-SEG-3, UR-QA-2, UR-QA-3
#pragma once

#include <pipeline/detail/fragment_merge.hpp>

#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/core/ids.hpp>
#include <slideio/processor/core/memory_budget.hpp>
#include <slideio/processor/pipeline/operation.hpp>
#include <slideio/processor/pipeline/seam_merger.hpp>
#include <slideio/processor/pipeline/tile_grid.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace slideio::processor::detail {

/// One fragment as the merger holds it until its object closes.
struct SeamFragment {
    int64_t tile = 0;                       ///< The tile that emitted it.
    Polygon polygon;                        ///< Level-0 geometry.
    std::vector<IntensityPartial> partials; ///< One per channel, distinct channels.
    Point64 anchor{0, 0};                   ///< polygon.anchor(), the canonical-order key.
};

/// A set of joined fragments: one object once it closes. Kept for the root of
/// its union-find tree only.
struct SeamComponent {
    LayerId layer;                         ///< The layer every member belongs to.
    std::vector<int64_t> members;          ///< Fragment ids.
    std::unordered_set<int64_t> openTiles; ///< Tiles held or expected that have not completed.
};

/// A seam contact waiting on the edge between two tiles for the fragment on the
/// other side.
struct SeamEdgeContact {
    int64_t fragment; ///< The fragment id.
    int64_t tile;     ///< The tile that emitted it.
    SeamContact contact;
};

/// A component taken out of the merger to be merged and emitted.
struct ClosedComponent {
    LayerId layer;
    std::vector<std::unique_ptr<SeamFragment>> fragments;
};

/// The engine's way into a SeamMerger (the executor and tests use it), so no
/// public signature grows.
struct SeamMergerAccess {
    /// Sets the restriction test applied to each merged object's anchor; an
    /// empty function admits everything. Called before the first fragment
    /// arrives; the filter is called from the threads that close objects, so
    /// it must be thread-safe.
    static void setFilter(SeamMerger& merger, std::function<bool(Point64)> filter);
};

} // namespace slideio::processor::detail

namespace slideio::processor {

/// The merger's state. Fragment ids are arrival sequence numbers. The mutex
/// guards everything but the counters, which are atomic so the noexcept
/// accessors can read them without it.
class SeamMerger::Impl {
public:
    Impl(const TileGrid& grid, MemoryBudgetPtr budget, std::string scratchDirectory);

    // Records a validated fragment whose seam contacts and expected neighbours
    // are already computed.
    void add(std::unique_ptr<detail::SeamFragment> fragment, LayerId layer,
             const std::vector<detail::SeamContact>& contacts, const std::vector<int64_t>& neighbours);
    // Marks a tile complete and takes out the components that close with it.
    std::vector<detail::ClosedComponent> complete(int64_t tileIndex);
    // Takes out every remaining component; InternalError when one is still open.
    std::vector<detail::ClosedComponent> takeRemaining();
    // Merges, measures, filters and emits one closed component.
    void emit(detail::ClosedComponent& component, ObjectSink& sink);
    // The filter in force, copied under the lock.
    std::function<bool(Point64)> currentFilter() const;

    // The tile across `contact` from `tileIndex`.
    int64_t neighbourAcross(int64_t tileIndex, const detail::SeamContact& contact) const;

    const TileGrid grid;
    const MemoryBudgetPtr budget;      // Reservations arrive with spilling.
    const std::string scratchDirectory;

    mutable std::mutex mutex;
    std::function<bool(Point64)> filter;

    std::atomic<int64_t> fragmentCount{0};
    std::atomic<int64_t> mergedObjectCount{0};
    std::atomic<int64_t> residentFragmentCount{0};
    std::atomic<int64_t> largestComponentFragmentCount{0};

private:
    int64_t find(int64_t fragment);
    void unite(int64_t a, int64_t b);
    detail::ClosedComponent takeComponent(int64_t root);
    int64_t edgeKey(int64_t a, int64_t b) const;

    std::vector<int64_t> parent_;                                  // Union-find over fragment ids.
    std::vector<std::unique_ptr<detail::SeamFragment>> fragments_; // Null once taken out.
    std::unordered_map<int64_t, detail::SeamComponent> components_; // Keyed by root.
    std::unordered_map<int64_t, std::vector<int64_t>> touching_;   // Tile -> fragments holding or expecting it.
    std::unordered_map<int64_t, std::vector<detail::SeamEdgeContact>> edges_; // Tile pair -> contacts.
    std::vector<char> completed_;                                  // Per tile.
};

} // namespace slideio::processor

namespace slideio::processor::detail {

inline void SeamMergerAccess::setFilter(SeamMerger& merger, std::function<bool(Point64)> filter) {
    std::lock_guard<std::mutex> lock(merger.impl_->mutex);
    merger.impl_->filter = std::move(filter);
}

} // namespace slideio::processor::detail
