// Requirements: UR-SEG-3, UR-QA-2, UR-QA-3
#pragma once

/**
 * @file seam_merger.hpp
 * @brief SeamMerger: joins the fragments of objects cut by tile seams into
 * whole objects and emits them as their neighbourhoods complete.
 */

#include <slideio/processor/core/control.hpp>
#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/core/ids.hpp>
#include <slideio/processor/core/memory_budget.hpp>
#include <slideio/processor/pipeline/export.hpp>
#include <slideio/processor/pipeline/operation.hpp>
#include <slideio/processor/pipeline/tile_grid.hpp>

#include <cstdint>
#include <memory>

namespace slideio::processor {

namespace detail {
struct SeamMergerAccess;
} // namespace detail

// IntensityPartial is declared in operation.hpp (5.1).

/**
 * @brief Joins the fragments of objects that cross tile seams into whole
 * objects, measures them and emits them.
 *
 * Operations emit an object that touches a seam of the tile grid as a
 * fragment, in level-0 coordinates, with one IntensityPartial per channel. A
 * fragment's seam contacts are the stretches of its boundary lying on a seam
 * line; two fragments in the tiles on either side of a seam, of the same
 * layer, belong to one object when their contacts on that seam overlap with
 * positive length. Touching at a single point (a corner) does not join:
 * objects are 4-connected. Joining is exact, in integers, with no tolerance.
 *
 * A fragment expects the tile on the other side of each of its contacts. An
 * object (a connected set of fragments) closes when every tile holding one of
 * its fragments and every tile one of them expects has been reported complete
 * through notifyTileComplete(); it does not wait for the rest of the grid. A
 * closed object is merged into one polygon (one outer ring, then its holes,
 * normalised as object operations emit them), measured with
 * applyStandardMeasurements() from that polygon and the fragments' partials
 * combined in canonical fragment order (ascending fragment anchor, y then x,
 * ties by tile index, never arrival or completion order), checked against the
 * run's restriction by its anchor, and emitted. Its identifier follows from
 * its layer and the anchor of the merged polygon (ObjectRecord::id()), so it
 * is the one a single-tile run would give the same object, whatever order
 * fragments arrive and tiles complete in. Floating-point sums are combined in
 * that canonical order, so measurements do not depend on the worker count.
 *
 * Merged objects are emitted from the thread calling notifyTileComplete() or
 * resolve(), outside the merger's lock, so two threads completing tiles emit
 * concurrently: the sink must be ThreadSafe, as every object sink of a run is.
 *
 * Peak memory is the fragments of open objects plus the largest single merged
 * polygon; largestComponentFragmentCount() reports the largest object met.
 * Each fragment held in memory reserves its bytes in the run's memory budget (16
 * per ring point, one IntensityPartial per partial, and 64 more), given back
 * when its object closes or the merger is destroyed. When the budget is short,
 * the fragments of the objects that opened first are written to one scratch
 * file, `seam-<32 random lowercase hex digits>.bin` in the scratch directory,
 * until the new fragment fits; if it still does not, addFragment() waits for
 * the run to give memory back. A fragment larger than the whole budget goes to
 * the file directly. Fragments written out are read back when their object
 * closes; joining, closing and the results are the same as when everything
 * stays in memory. spilledFragmentCount() and spilledBytes() report the
 * writing; the file is removed when the merger is destroyed, on failure too.
 * @par Thread-safety
 * ThreadSafe.
 * @par Requirements
 * UR-SEG-3, UR-QA-2, UR-QA-3
 */
class SLIDEIO_PROCESSOR_ENGINE_EXPORTS SeamMerger {
public:
    /// `scratchDirectory` is where fragments spill. Must exist and be
    /// writable; the merger owns every file it creates there and removes
    /// them on destruction.
    ///
    /// The merger keeps a copy of the run's tile grid and shares the run's
    /// memory budget.
    /// @param scratchDirectory UTF-8 path of an existing directory.
    /// @throws InvalidArgumentError When @p budget or @p scratchDirectory is
    /// null, or @p scratchDirectory is not an existing directory.
    /// @throws std::bad_alloc When memory is exhausted.
    /// @par Thread-safety
    /// Safe to call concurrently.
    /// @par Requirements
    /// UR-SEG-3
    SeamMerger(const TileGrid&, MemoryBudgetPtr,
               const char* scratchDirectory);
    /// Destroys the merger and every fragment still held, closed or not,
    /// releases the memory reserved for them and removes the scratch file.
    /// @throws Nothing.
    /// @par Thread-safety
    /// Not concurrently with any other member.
    /// @par Requirements
    /// UR-SEG-3
    ~SeamMerger();

    /** @brief Deleted: a merger is not copyable (nor movable). */
    SeamMerger(const SeamMerger&) = delete;
    /** @brief Deleted: a merger is not copy-assignable (nor move-assignable). */
    SeamMerger& operator=(const SeamMerger&) = delete;

    /// Records a fragment touching a tile boundary. Called from worker
    /// threads, in any order.
    ///
    /// The fragment is joined at once with the fragments already held that
    /// it touches across a seam; nothing is emitted until its tile and the
    /// tiles it expects complete. A fragment arriving for a tile already
    /// reported complete is held until resolve(). The polygon, in level-0
    /// coordinates, is copied; the layer is the merged object's and must be
    /// valid.
    /// @param tileIndex The index in the grid of the tile that emitted it.
    /// @param partials One partial per channel, read during this call only;
    /// may be null when @p partialCount is 0.
    /// @param partialCount Number of partials; not negative.
    /// @throws InvalidArgumentError When @p tileIndex is outside the grid,
    /// the fragment is empty, the layer is invalid, @p partialCount is
    /// negative, or @p partials is null while @p partialCount is positive;
    /// nothing is recorded.
    /// @throws IoError When the scratch file cannot be created or written,
    /// naming the file; the fragment is recorded but its memory is not
    /// reserved, and the run cannot continue.
    /// @throws InternalError When the memory budget's lock fails.
    /// @throws std::bad_alloc When memory is exhausted.
    /// @par Thread-safety
    /// Safe to call concurrently. May block while the memory budget is short
    /// and nothing more can be written out.
    /// @par Requirements
    /// UR-SEG-3, UR-EXE-2
    void addFragment(int64_t tileIndex, const Polygon&, LayerId,
                     const IntensityPartial* partials, int32_t partialCount);

    /// Reports that a tile is finished. Components all of whose bordering
    /// tiles have completed are merged and emitted to `sink` immediately,
    /// which is what keeps the resident set proportional to the working
    /// frontier rather than to the whole slide.
    ///
    /// Only the objects touching @p tileIndex are re-examined. A tile skipped
    /// by the run's restriction must be reported too. Reporting a tile twice
    /// does nothing more.
    /// @param tileIndex The index in the grid of the completed tile.
    /// @param sink Receives the objects that close; ThreadSafe.
    /// @throws InvalidArgumentError When @p tileIndex is outside the grid.
    /// @throws InternalError When a closed object's fragments do not merge
    /// into one polygon; the message names the layer and the fragment and
    /// ring counts, never coordinates.
    /// @throws IoError When a closed object's fragments cannot be read back
    /// from the scratch file, naming the file.
    /// @throws ProcessorError Whatever @p sink throws.
    /// @throws std::bad_alloc When memory is exhausted.
    /// @par Thread-safety
    /// Safe to call concurrently.
    /// @par Requirements
    /// UR-SEG-3, UR-QA-3
    void notifyTileComplete(int64_t tileIndex, ObjectSink& sink);

    /// Merges and emits whatever remains after the tile pass.
    ///
    /// Every object whose tiles have all completed is emitted. After a pass
    /// that reported every tile, nothing remains; an object some tile of
    /// which was never reported is a broken run and is not emitted. Progress
    /// is reported to the observer, which may be null, under the stage name
    /// "seam merge", one unit per object. The token is checked before each
    /// object. The sink receives the remaining objects and must be ThreadSafe.
    /// @throws InternalError When an object remains whose tiles have not all
    /// completed, naming its layer, or when a remaining object's fragments do
    /// not merge (as notifyTileComplete()).
    /// @throws IoError When a remaining object's fragments cannot be read back
    /// from the scratch file, naming the file.
    /// @throws CancelledError When the token is cancelled.
    /// @throws ProcessorError Whatever the sink throws.
    /// @throws std::bad_alloc When memory is exhausted.
    /// @par Thread-safety
    /// Safe to call concurrently, but meant to be called once, after every
    /// addFragment() and notifyTileComplete() call has returned.
    /// @par Requirements
    /// UR-SEG-3
    void resolve(ObjectSink&, ProgressObserver*, const CancellationToken&);

    /// The fragments recorded so far.
    /// @return The count.
    /// @throws Nothing.
    /// @par Thread-safety
    /// Safe to call concurrently.
    /// @par Requirements
    /// UR-SEG-3
    int64_t fragmentCount() const noexcept;
    /// The objects merged so far, including those the restriction then
    /// dropped.
    /// @return The count.
    /// @throws Nothing.
    /// @par Thread-safety
    /// Safe to call concurrently.
    /// @par Requirements
    /// UR-SEG-3
    int64_t mergedObjectCount() const noexcept;

    /// The fragments currently held in memory, waiting for their objects to
    /// close.
    /// @return The count.
    /// @throws Nothing.
    /// @par Thread-safety
    /// Safe to call concurrently.
    /// @par Requirements
    /// UR-SEG-3
    int64_t residentFragmentCount() const noexcept;
    /// The fragments written to the scratch file so far, each counted once.
    /// @return The count.
    /// @throws Nothing.
    /// @par Thread-safety
    /// Safe to call concurrently.
    /// @par Requirements
    /// UR-SEG-3
    int64_t spilledFragmentCount() const noexcept;
    /// The bytes written to the scratch file so far: the size of its records.
    /// @return The count.
    /// @throws Nothing.
    /// @par Thread-safety
    /// Safe to call concurrently.
    /// @par Requirements
    /// UR-SEG-3
    int64_t spilledBytes() const noexcept;
    /// The most fragments any one object held, closed or still open: what
    /// sets the size of the largest polygon materialised at once.
    /// @return The count; 0 before any fragment.
    /// @throws Nothing.
    /// @par Thread-safety
    /// Safe to call concurrently.
    /// @par Requirements
    /// UR-SEG-3
    int64_t largestComponentFragmentCount() const noexcept;

private:
    friend struct detail::SeamMergerAccess;
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace slideio::processor
