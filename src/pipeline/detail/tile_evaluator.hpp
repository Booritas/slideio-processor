// Requirements: UR-EXT-3, UR-IMG-5, UR-ALG-5, UR-EXE-1, UR-STG-3
#pragma once

#include <pipeline/detail/operation_context_state.hpp>
#include <pipeline/detail/restriction.hpp>
#include <pipeline/detail/run_plan.hpp>
#include <pipeline/detail/scene_reader.hpp>

#include <slideio/processor/core/control.hpp>
#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/pipeline/operation.hpp>
#include <slideio/processor/pipeline/pipeline_spec.hpp>
#include <slideio/processor/pipeline/tile_grid.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace slideio::processor::detail {

/// What a run counts besides what reaches the sinks. Shared by every worker of
/// the run, hence atomic; reset at the start of each run.
struct RunCounters {
    std::atomic<int64_t> droppedOutsideTile{0};    ///< Objects whose anchor left the emitting tile.
    std::atomic<int64_t> droppedByRestriction{0};  ///< Objects whose anchor the tile's mask excludes.
    std::atomic<int64_t> droppedByFilter{0};       ///< Objects their producer's finishObject() discards.
    std::atomic<int64_t> tilesSkipped{0};          ///< Tiles whose mask is empty inside the tile.

    void reset() noexcept {
        droppedOutsideTile = 0;
        droppedByRestriction = 0;
        droppedByFilter = 0;
        tilesSkipped = 0;
    }
};

class FinishingSink;

/// The object sink an operation sees on one tile: an object is kept only when
/// its anchor lies in the tile's level-0 rect, then only when the tile's mask
/// is set there; a kept object's rings are normalised, the form a merged object
/// has, before it goes on to the target, which finishes it. The anchor is the
/// same before and after. An object emitted already normalised goes on as it
/// is, and one that is not is copied once, its geometry normalised in the
/// copy, which a FinishingSink target then finishes in place; so a tile object
/// is copied at most once on its way to the run's sink. Flushing is the run's
/// business, so flush() does nothing. One filter serves one tile, but an
/// operation may emit from threads of its own, so the count is atomic.
class TileObjectFilter : public ObjectSink {
public:
    /// `target`, `mask` and `counters` are borrowed; `mask` may be null.
    TileObjectFilter(ObjectSink* target, const Rect64& tileRect, const RasterMask* mask, RunCounters& counters);

    void emit(const ObjectRecord& record) override;
    void flush() override {}
    /// The objects passed on to the target, not those its finishing step discarded.
    int64_t emittedCount() const override { return emitted_.load(); }

private:
    ObjectSink* target_;
    FinishingSink* finishing_; // target_, when it is the run's finishing sink
    Rect64 tileRect_;
    const RasterMask* mask_;
    RunCounters& counters_;
    std::atomic<int64_t> emitted_{0};
};

/// The run's bindings the evaluator hands to operations. Every pointer is
/// borrowed and must outlive the evaluator; a null sink means unbound.
struct Bindings {
    ObjectSink* objectSink = nullptr;              ///< The run's object sink, behind the tile-ownership and restriction filters.
    EdgeSink* edgeSink = nullptr;
    AnnotationSink* annotationSink = nullptr;
    RasterSink* rasterSink = nullptr;
    std::vector<std::pair<std::string, TileRasterSource*>> auxiliaries;
    TileMaskSource* maskSource = nullptr;
    /// The seam merger's entry point, called with the emitting tile's index; empty
    /// when the run has none.
    SeamFragmentCallback seamFragment;
    const PipelineSpec* spec = nullptr;            ///< For the channels and the restriction; required.
    const LevelSelector* selector = nullptr;       ///< For the restriction's mapping; required.
    RunCounters* counters = nullptr;               ///< Required.
    /// The scene's level-0 pixel size in micrometres, what operations read as
    /// OperationContext::pixelSizeMicrons(); 0 when unknown.
    double pixelSizeMicrons = 0.0;
    /// Charged for the restriction's cached anchor masks (RestrictionFilter);
    /// null: they are not charged. Borrowed; must outlive the evaluator.
    MemoryBudget* memoryBudget = nullptr;
};

/// What evaluating one tile did.
struct TileOutcome {
    bool skipped;       ///< The restriction excludes the whole tile; no pixel was read.
    int64_t bytesUsed;  ///< Bytes of every buffer the tile allocated (source, intermediates, auxiliaries, mask).
};

/// One tile of a global-context accumulation pass: the operations
/// before `position` are applied (already finalised), then `accumulator`, a fresh
/// instance of operation `position`, accumulates their output in its place.
/// Nothing reaches a sink in such a pass and the operations after `position` do
/// not run.
struct Accumulation {
    size_t position;          ///< A chain position; below the operation count.
    Operation* accumulator;   ///< Borrowed; not null.
};

/// Evaluates the chain on one tile: builds the tile's mask,
/// reads the source over the tile grown by the chain's halo, applies each
/// operation on its own region, filters emitted objects by anchor and
/// hands the final output to the raster sink. Pure per tile: evaluate() may run
/// concurrently on different tiles, as far as the operations and sinks allow.
class TileEvaluator {
public:
    /// `operations` are the operations to apply, one per position of
    /// `plan.operations`: the plan's own, or a run's copy with its fresh
    /// global-context instances in their positions. The plan is only read.
    /// Throws InvalidArgumentError when a required binding is null or `operations`
    /// does not match the plan's chain, and what RestrictionFilter's constructor throws.
    TileEvaluator(const RunPlan& plan, std::vector<OperationPtr> operations, const SceneReader& reader,
                  Bindings bindings);

    /// Evaluates tile `tileIndex`. Throws CancelledError when `token` is
    /// cancelled (checked before the tile and between operations), and whatever
    /// the reader, an operation or a sink throws.
    /// When `runningOperation` is not null, it is set to the name of each operation
    /// while that operation runs and back to null after it, so a caller that
    /// catches can tell which operation threw (null: none did).
    /// When `accumulation` is not null, the tile is evaluated for that accumulation
    /// pass instead of the final one: same mask, reads and rects as a chain ending
    /// at `accumulation->position`, no sink is written, and a skipped tile is not
    /// counted in RunCounters::tilesSkipped (the final pass counts it).
    /// Throws InvalidArgumentError when `accumulation` names no operation or has a
    /// null accumulator.
    TileOutcome evaluate(int64_t tileIndex, const CancellationToken& token,
                         const char** runningOperation = nullptr,
                         const Accumulation* accumulation = nullptr) const;

    const RestrictionFilter& restriction() const noexcept;

private:
    const RunPlan& plan_;
    std::vector<OperationPtr> operations_;
    const SceneReader& reader_;
    Bindings bindings_;
    RestrictionFilter restriction_;
};

} // namespace slideio::processor::detail
