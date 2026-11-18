// Requirements: UR-SEG-3, UR-QA-2, UR-QA-3
#pragma once

#include <pipeline/detail/fragment_merge.hpp>
#include <pipeline/detail/fragment_spill.hpp>

#include <slideio/processor/core/control.hpp>
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
#include <set>
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

/// Where one fragment is while its object is open. Its id is its index.
struct FragmentSlot {
    std::unique_ptr<SeamFragment> resident; ///< Null once written to the scratch file or taken.
    int64_t tile = 0;                       ///< The tile that emitted it.
    int64_t spillOffset = -1;               ///< Its record in the scratch file, once written.
    int64_t reservedBytes = 0;              ///< Held in the memory budget for it while resident.
    bool taken = false;                     ///< Its object has closed.
};

/// A set of joined fragments: one object once it closes. Kept for the root of
/// its union-find tree only.
struct SeamComponent {
    LayerId layer;                         ///< The layer every member belongs to.
    std::vector<int64_t> members;          ///< Fragment ids.
    std::unordered_set<int64_t> openTiles; ///< Tiles held or expected that have not completed.
    int64_t first = 0;                     ///< The smallest member id: when it opened.
    int64_t residentCount = 0;             ///< Members still in memory.
};

/// A seam contact waiting on the edge between two tiles for the fragment on the
/// other side.
struct SeamEdgeContact {
    int64_t fragment; ///< The fragment id.
    int64_t tile;     ///< The tile that emitted it.
    int64_t joinKey;  ///< The fragment's join key: it joins only an equal key.
    SeamContact contact;
};

/// A seam point of a fragment of an 8-connected layer, waiting between its
/// tile and another tile touching the point for a fragment sharing it.
struct SeamPointContact {
    int64_t fragment; ///< The fragment id.
    int64_t tile;     ///< The tile that emitted it.
    int64_t joinKey;  ///< The fragment's join key: it joins only an equal key.
    Point64 at;       ///< The point, in level-0 coordinates.
};

/// A seam point of a new fragment and one other tile touching it.
struct SeamPointNeighbour {
    Point64 at;   ///< The point, in level-0 coordinates.
    int64_t tile; ///< A tile other than the fragment's whose rect touches the point.
};

/// Bytes held in a memory budget, given back when the holder is destroyed.
class BudgetHold {
public:
    BudgetHold() = default;
    BudgetHold(BudgetHold&& other) noexcept : budget_(std::move(other.budget_)), bytes_(other.bytes_) {
        other.bytes_ = 0;
    }
    BudgetHold& operator=(BudgetHold&& other) noexcept {
        if (this != &other) {
            reset();
            budget_ = std::move(other.budget_);
            bytes_ = other.bytes_;
            other.bytes_ = 0;
        }
        return *this;
    }
    BudgetHold(const BudgetHold&) = delete;
    BudgetHold& operator=(const BudgetHold&) = delete;
    ~BudgetHold() { reset(); }

    /// Takes over bytes already reserved in `budget`.
    void add(const MemoryBudgetPtr& budget, int64_t bytes) noexcept {
        if (!budget_) {
            budget_ = budget;
        }
        bytes_ += bytes;
    }
    /// Releases what is held.
    void reset() noexcept {
        if (budget_ && bytes_ > 0) {
            try {
                budget_->release(bytes_);
            } catch (...) {
                // A failing budget lock cannot be reported from here.
            }
        }
        bytes_ = 0;
    }

private:
    MemoryBudgetPtr budget_;
    int64_t bytes_ = 0;
};

/// A fragment of a closed component that is still in the scratch file.
struct SpilledFragment {
    int64_t tile;   ///< The tile that emitted it.
    int64_t offset; ///< Its record in the scratch file.
};

/// A component taken out of the merger to be merged and emitted. The budget
/// bytes of its resident fragments are released when it is destroyed.
struct ClosedComponent {
    LayerId layer;
    std::vector<std::unique_ptr<SeamFragment>> fragments;
    std::vector<SpilledFragment> spilled;
    BudgetHold reserved;
};

/// The engine's way into a SeamMerger (the executor and tests use it), so no
/// public signature grows.
struct SeamMergerAccess {
    /// Sets the restriction test applied to each merged object's anchor; an
    /// empty function admits everything. Called before the first fragment
    /// arrives; the filter is called from the threads that close objects, so
    /// it must be thread-safe.
    static void setFilter(SeamMerger& merger, std::function<bool(Point64)> filter);
    /// Declares the objects of @p layer 8-connected: its fragments also join
    /// at a point they share on a seam (seamPoints), and its objects are merged
    /// with mergeFragments(..., true). Called before the first fragment of the
    /// layer arrives; layers not declared stay 4-connected.
    static void setEightConnected(SeamMerger& merger, LayerId layer);
};

} // namespace slideio::processor::detail

namespace slideio::processor {

/// The merger's state. Fragment ids are arrival sequence numbers. The mutex
/// guards everything but the counters, which are atomic so the noexcept
/// accessors can read them without it.
class SeamMerger::Impl {
public:
    Impl(const TileGrid& grid, MemoryBudgetPtr budget, std::string scratchDirectory);
    // Releases the budget bytes of every fragment still resident; the scratch
    // file, if any, is removed with it.
    ~Impl();

    // Records a validated fragment whose seam contacts and expected neighbours
    // are already computed, and reserves `bytes` for it, writing older objects
    // to the scratch file while the budget is short.
    // `points` are the fragment's seam points, each with a tile touching it, on
    // an 8-connected layer, and empty otherwise.
    // The fragment joins only fragments of its layer and `joinKey`.
    void add(std::unique_ptr<detail::SeamFragment> fragment, LayerId layer, int64_t joinKey,
             const std::vector<detail::SeamContact>& contacts,
             const std::vector<detail::SeamPointNeighbour>& points, const std::vector<int64_t>& neighbours,
             int64_t bytes);
    // Marks a tile complete and takes out the components that close with it.
    std::vector<detail::ClosedComponent> complete(int64_t tileIndex);
    // Takes out every remaining component; InternalError when one is still open.
    std::vector<detail::ClosedComponent> takeRemaining();
    // Merges, measures, filters and emits one closed component.
    void emit(detail::ClosedComponent& component, ObjectSink& sink);
    // The filter in force, copied under the lock.
    std::function<bool(Point64)> currentFilter() const;
    // Whether `layer` was declared 8-connected, read under the lock.
    bool isEightConnected(LayerId layer) const;

    // The tile across `contact` from `tileIndex`.
    int64_t neighbourAcross(int64_t tileIndex, const detail::SeamContact& contact) const;
    // The tiles other than `tileIndex` whose rects touch `point`, a point on
    // the boundary of tileIndex's rect: one across a seam, three at a corner.
    std::vector<int64_t> neighboursAt(int64_t tileIndex, Point64 point) const;

    const TileGrid grid;
    const MemoryBudgetPtr budget;
    const std::string scratchDirectory;

    mutable std::mutex mutex;
    std::function<bool(Point64)> filter;
    std::set<LayerId> eightConnected;

    std::atomic<int64_t> fragmentCount{0};
    std::atomic<int64_t> mergedObjectCount{0};
    std::atomic<int64_t> residentFragmentCount{0};
    std::atomic<int64_t> largestComponentFragmentCount{0};
    std::atomic<int64_t> spilledFragmentCount{0};
    std::atomic<int64_t> spilledBytes{0};

private:
    int64_t find(int64_t fragment);
    void unite(int64_t a, int64_t b);
    detail::ClosedComponent takeComponent(int64_t root);
    int64_t edgeKey(int64_t a, int64_t b) const;
    // Writes one resident fragment to the scratch file and releases its bytes.
    void spillFragment(int64_t fragment);
    // Writes every resident member of the component rooted at `root`.
    void spillComponent(int64_t root);
    // Reserves `bytes` for the new fragment `id`, spilling the oldest-opened
    // other components while the budget is short, then the fragment itself when
    // nothing else is left to spill. Never waits for memory.
    void reserveFor(int64_t id, int64_t bytes);

    std::vector<int64_t> parent_;                                  // Union-find over fragment ids.
    std::vector<detail::FragmentSlot> fragments_;                  // Indexed by fragment id.
    std::set<int64_t> residentByOpening_;                          // `first` of components with resident members.
    std::unique_ptr<detail::FragmentSpill> spill_;                 // Created at the first spill; never reset.
    std::unordered_map<int64_t, detail::SeamComponent> components_; // Keyed by root.
    std::unordered_map<int64_t, std::vector<int64_t>> touching_;   // Tile -> fragments holding or expecting it.
    std::unordered_map<int64_t, std::vector<detail::SeamEdgeContact>> edges_; // Tile pair -> contacts.
    std::unordered_map<int64_t, std::vector<detail::SeamPointContact>> points_; // Tile pair -> seam points.
    std::vector<char> completed_;                                  // Per tile.
};

} // namespace slideio::processor

namespace slideio::processor::detail {

inline void SeamMergerAccess::setFilter(SeamMerger& merger, std::function<bool(Point64)> filter) {
    std::lock_guard<std::mutex> lock(merger.impl_->mutex);
    merger.impl_->filter = std::move(filter);
}

inline void SeamMergerAccess::setEightConnected(SeamMerger& merger, LayerId layer) {
    std::lock_guard<std::mutex> lock(merger.impl_->mutex);
    merger.impl_->eightConnected.insert(layer);
}

} // namespace slideio::processor::detail
