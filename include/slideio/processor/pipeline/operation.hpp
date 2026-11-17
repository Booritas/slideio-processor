// Requirements: UR-EXT-1, UR-EXT-2, UR-EXT-3, UR-EXT-4, UR-ALG-3, UR-ALG-4, UR-IMG-11
#pragma once

/**
 * @file operation.hpp
 * @brief The operation contract: ChannelArity, TileRequirement, Halo,
 * IntensityPartial, OperationContext and Operation.
 */

#include <slideio/processor/core/block_view.hpp>
#include <slideio/processor/core/channel_selection.hpp>
#include <slideio/processor/core/control.hpp>
#include <slideio/processor/core/data_type.hpp>
#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/core/ids.hpp>
#include <slideio/processor/core/raster_mask.hpp>
#include <slideio/processor/pipeline/export.hpp>
#include <slideio/processor/pipeline/operation_parameters.hpp>

#include <cstdint>
#include <memory>

namespace slideio::processor {

namespace detail {
struct OperationContextAccess;
} // namespace detail

/**
 * @brief How an operation consumes the selected channels.
 * @par Requirements
 * UR-ALG-3
 */
enum class ChannelArity : int32_t {
    PerChannel,   ///< Applied independently to each selected channel.
    Joint         ///< Consumes the selected channels together.
};

/**
 * @brief What an operation needs to see of a level to compute one tile.
 *
 * Halo is the ordinary case: a tile plus the declared border. WholeLevel means
 * the operation reads the whole analysis level as one tile, so the framework
 * must check before the run that the level fits the memory budget, using
 * Operation::workingSetBytesPerLevelPixel().
 * @par Requirements
 * UR-EXT-3, UR-EXT-4
 */
enum class TileRequirement : int32_t {
    Halo,       ///< A tile grown by the declared Halo.
    WholeLevel  ///< The whole analysis level as a single tile.
};

/**
 * @brief The border context, in level pixels, an operation needs on each side
 * of a tile.
 *
 * A plain data structure (C-2 exception): four widths and no invariant beyond
 * each being non-negative, which uniform() enforces.
 * @par Thread-safety
 * ThreadCompatible.
 * @par Requirements
 * UR-EXT-3
 */
struct SLIDEIO_PROCESSOR_ENGINE_EXPORTS Halo {
    // SDS 05 5.1 declares these as one line, `int32_t left, top, right, bottom;`;
    // they are split only so each member carries its own documentation.
    int32_t left;   ///< Width of the left border, in level pixels.
    int32_t top;    ///< Width of the top border, in level pixels.
    int32_t right;  ///< Width of the right border, in level pixels.
    int32_t bottom; ///< Width of the bottom border, in level pixels.
    /**
     * @brief Returns a halo of the same width on every side.
     * @param n The width; must not be negative.
     * @return {n, n, n, n}.
     * @throws InvalidArgumentError When @p n is negative: a negative halo is
     * refused, never clamped.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-EXT-3
     */
    static Halo uniform(int32_t n);
    /**
     * @brief Returns the empty halo.
     * @return {0, 0, 0, 0}.
     * @throws Nothing.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-EXT-3
     */
    static Halo none() noexcept;
    /**
     * @brief Tells whether the halo has no border on any side.
     * @return True when all four widths are zero.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-EXT-3
     */
    bool isEmpty() const noexcept;
};

/**
 * @brief Partial intensity statistics of one channel over one fragment of an
 * object.
 *
 * Carried with a fragment that touches a tile seam, because the pixels are
 * gone by the time the fragment's component closes. Integer count, float sums;
 * partials of one channel are combined by adding counts and sums and taking
 * the extreme minimum and maximum, in canonical fragment order. A plain data
 * structure, exempt from pimpl per C-2.
 * @par Thread-safety
 * ThreadCompatible.
 * @par Requirements
 * UR-SEG-3, UR-SEG-5
 */
struct SLIDEIO_PROCESSOR_ENGINE_EXPORTS IntensityPartial {
    // SDS 05 5.7 declares the doubles as one line; split only for documentation.
    int64_t count;        ///< Number of samples.
    double  sum;          ///< Sum of the samples.
    double  sumOfSquares; ///< Sum of the squares of the samples.
    double  minimum;      ///< Smallest sample.
    double  maximum;      ///< Largest sample.
    int32_t channel;      ///< The scene channel the samples come from.
};

/**
 * @brief Everything an operation sees of one tile: its pixels, its place on
 * the slide, the sinks it emits into and the run's controls.
 *
 * Only the framework builds one, one per tile; an operation receives it in
 * Operation::apply() and must not keep it beyond that call. Every pointer it
 * hands out is borrowed from the run and valid only during the call.
 * @par Thread-safety
 * ThreadConfined: one context per tile, used by the thread applying it.
 * @par Requirements
 * UR-EXT-1, UR-EXT-3, UR-EXT-4
 */
class SLIDEIO_PROCESSOR_ENGINE_EXPORTS OperationContext {
public:
    /**
     * @brief Destroys the context; it owns nothing the operation can see.
     * @throws Nothing.
     */
    ~OperationContext();
    OperationContext(const OperationContext&) = delete;
    OperationContext& operator=(const OperationContext&) = delete;

    /**
     * @brief Returns the tile's input pixels.
     * @return The view over the halo rect at the analysis level, one plane per
     * selected channel (one plane for a PerChannel operation); a null view
     * when the run supplied none.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-EXT-1, UR-EXT-3
     */
    const BlockView& input() const noexcept;
    /**
     * @brief Returns the buffer the operation writes its raster result into.
     * @return The view over the tile rect at the analysis level, typed by
     * Operation::outputType(); a null view when the run supplied none.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-EXT-1
     */
    BlockView&       output() noexcept;

    /// Where emitted objects go. Null for operations that produce only
    /// raster output.
    /// @return The sink, or null.
    /// @throws Nothing.
    /// @par Thread-safety
    /// The sink is ThreadSafe; the accessor is ThreadConfined.
    /// @par Requirements
    /// UR-EXT-4
    ObjectSink* objectSink() const noexcept;

    /// Where asserted containment goes (UR-RES-5). Null when the run has no
    /// store behind it — a directly-constructed Pipeline — so an operation
    /// that attaches must check before using it.
    ///
    /// The framework does not filter what reaches this sink: unlike
    /// objectSink(), an assertion is passed on as made, whatever its objects'
    /// anchors. An operation must therefore assert edges only for objects it
    /// emits through objectSink() that are anchored in tileRect() and admitted
    /// by mask(), so that every assertion names an object the run stores.
    /// @return The sink, or null.
    /// @throws Nothing.
    /// @par Thread-safety
    /// The sink is ThreadSafe: other tiles call it concurrently. The accessor
    /// is ThreadConfined.
    /// @par Requirements
    /// UR-RES-5
    EdgeSink* edgeSink() const noexcept;

    /// Where an operation attaches a label or a measurement to an object some
    /// earlier stage wrote (`03-storage.md` 3.7). Null on the same terms as
    /// `edgeSink`, and additionally null when the stage declared no
    /// `annotatesLayer`, so an operation that classifies must check.
    ///
    /// The framework does not filter what reaches this sink either. An
    /// operation must attach labels and measurements only to objects anchored
    /// in tileRect() and admitted by mask() (for an object it emits, exactly
    /// those objectSink() keeps), so that an object seen in several tiles'
    /// halos is annotated once, by the tile owning its anchor, and none outside
    /// the restriction is.
    /// @return The sink, or null.
    /// @throws Nothing.
    /// @par Thread-safety
    /// The sink is ThreadSafe: other tiles call it concurrently. The accessor
    /// is ThreadConfined.
    /// @par Requirements
    /// UR-EXT-4
    AnnotationSink* annotationSink() const noexcept;

    /// A raster bound to this run under `name`, covering this tile's halo
    /// rect at the run's analysis level — the same geometry as `input()`.
    /// Null when nothing was bound under that name, so an operation that
    /// wants coarse context must check and degrade rather than assume.
    /// @param name The UTF-8 binding name; null is never bound.
    /// @return The view, or null.
    /// @throws Nothing.
    /// @par Thread-safety
    /// ThreadConfined.
    /// @par Requirements
    /// UR-EXT-4
    const BlockView* auxiliary(const char* name) const noexcept;

    /// The layer this operation writes, already resolved from its
    /// `targetLayer` parameter against the bindings of the run. Invalid for
    /// operations that produce only raster output.
    /// @return The layer, or an invalid LayerId.
    /// @throws Nothing.
    /// @par Thread-safety
    /// ThreadConfined.
    /// @par Requirements
    /// UR-EXT-4
    LayerId targetLayer() const noexcept;

    /**
     * @brief Returns the pyramid level being read.
     * @return The analysis level, 0 for the full resolution.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-EXT-1
     */
    int32_t level() const noexcept;        // the pyramid level being read

    /// Level coordinates — the space `input()` and `output()` are indexed in,
    /// and the space the declared halo is measured in.
    /// @return The tile rect at the analysis level.
    /// @throws Nothing.
    /// @par Thread-safety
    /// ThreadConfined.
    /// @par Requirements
    /// UR-EXT-3
    Rect64 tileRectAtLevel() const noexcept;
    /// The tile rect grown by the halo, at the analysis level; not clipped
    /// to the level, so it can reach beyond it (those pixels read as zero).
    /// @return The halo rect at the analysis level.
    /// @throws Nothing.
    /// @par Thread-safety
    /// ThreadConfined.
    /// @par Requirements
    /// UR-EXT-3
    Rect64 haloRectAtLevel() const noexcept;

    /// The same tile in level-0 coordinates, for an operation that needs to
    /// relate itself to slide position.
    /// @return The tile rect in level-0 coordinates, mapped by its edges.
    /// @throws Nothing.
    /// @par Thread-safety
    /// ThreadConfined.
    /// @par Requirements
    /// UR-EXT-1
    Rect64 tileRect() const noexcept;
    /// The halo rect in level-0 coordinates, mapped by its edges.
    /// @return The halo rect in level-0 coordinates.
    /// @throws Nothing.
    /// @par Thread-safety
    /// ThreadConfined.
    /// @par Requirements
    /// UR-EXT-1
    Rect64 haloRect() const noexcept;

    /// Level to level-0, through the library's single mapping (5.6). Emitted
    /// geometry must be in level-0 coordinates (UR-SEG-4); this is how an
    /// operation working on level pixels gets there.
    /// @return The factor from the analysis level to level 0; at least 1.
    /// @throws Nothing.
    /// @par Thread-safety
    /// ThreadConfined.
    /// @par Requirements
    /// UR-SEG-4
    double  levelScale() const noexcept;
    /**
     * @brief Maps a point from the analysis level to level 0.
     * @param levelPoint The point in level coordinates.
     * @return {llround(x * levelScale()), llround(y * levelScale())}, the one
     * mapping every component of the library uses.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-SEG-4
     */
    Point64 toLevel0(Point64 levelPoint) const noexcept;
    /**
     * @brief Maps a polygon from the analysis level to level 0, vertex by
     * vertex through toLevel0(Point64), keeping rings, their order and which
     * are holes.
     * @param levelPolygon The polygon in level coordinates.
     * @return The polygon in level-0 coordinates.
     * @throws InvalidArgumentError When a mapped ring is no longer a valid ring
     * (Polygon::addRing).
     * @throws std::bad_alloc When memory is exhausted.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-SEG-4
     */
    Polygon toLevel0(const Polygon& levelPolygon) const;

    /**
     * @brief Returns the channels this run selected.
     * @return The selection; ChannelSelection::all() when the run supplied none.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-ALG-3
     */
    const ChannelSelection& channels() const noexcept;
    /**
     * @brief Returns the run's cancellation token, for a long apply() to poll.
     * @return The token; a never-cancelled token when the run supplied none.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadConfined; the token itself is ThreadSafe.
     * @par Requirements
     * UR-EXT-4
     */
    const CancellationToken& cancellation() const noexcept;

    /// The run's restriction, rasterised over this tile's halo rect, or null
    /// when the run is unrestricted (5.5). Level coordinates, like `input()`.
    ///
    /// Consulting it is OPTIONAL for objects. The framework filters what this
    /// operation emits through objectSink() whether or not it is read, so an
    /// operation that ignores it is correct and merely wasteful; an operation
    /// that reads it skips work it would otherwise do and discard. Edge and
    /// annotation assertions are not filtered: an operation making them reads
    /// it (see edgeSink() and annotationSink()). A null mask admits everything.
    /// @return The mask, or null.
    /// @throws Nothing.
    /// @par Thread-safety
    /// ThreadConfined.
    /// @par Requirements
    /// UR-EXT-4
    const RasterMask* mask() const noexcept;

    /// Records a fragment touching a tile seam, for cross-tile merging.
    /// Level-0 coordinates, like every emitted polygon.
    /// Equivalent to the four-argument form with no partials.
    /// @param fragment The fragment, in level-0 coordinates.
    /// @param layer The layer the merged object belongs to.
    /// @throws InternalError When this run has no seam merger.
    /// @par Thread-safety
    /// ThreadConfined; the merger behind it is ThreadSafe.
    /// @par Requirements
    /// UR-SEG-3, UR-SEG-4
    void emitSeamFragment(const Polygon& fragment, LayerId layer);
    /**
     * @brief Records a fragment touching a tile seam together with its
     * per-channel intensity partials, which the merger combines when the
     * object closes.
     * @param fragment The fragment, in level-0 coordinates.
     * @param layer The layer the merged object belongs to.
     * @param partials One partial per channel, read during this call only;
     * may be null when @p partialCount is 0.
     * @param partialCount Number of partials; not negative.
     * @throws InvalidArgumentError When @p partialCount is negative, or
     * @p partials is null while @p partialCount is positive.
     * @throws InternalError When this run has no seam merger.
     * @par Thread-safety
     * ThreadConfined; the merger behind it is ThreadSafe.
     * @par Requirements
     * UR-SEG-3, UR-SEG-4
     */
    void emitSeamFragment(const Polygon& fragment, LayerId layer,
                          const IntensityPartial* partials, int32_t partialCount);

private:
    friend struct detail::OperationContextAccess;
    OperationContext();

    class Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * @brief The per-tile computation an operation author writes; the framework
 * supplies tiling, halo, threading, budgeting and emission (UR-EXT-4).
 *
 * The public extension interface: built-in operations implement it with no
 * privileged access to internals (UR-EXT-2). The declaration methods name(),
 * arity(), requiresUniformTypes(), supportedTypes(), halo(), outputType(),
 * tileRequirement(), workingSetBytesPerLevelPixel(), producesObjects() and
 * outputPlaneCount() are the contract the framework checks before a run
 * starts; they must be constant for the life of the operation.
 * @par Thread-safety
 * Implementations must be reentrant: apply() is called concurrently on
 * different tiles.
 * @par Requirements
 * UR-EXT-1 to UR-EXT-4, UR-ALG-3, UR-ALG-4, UR-IMG-11
 */
class SLIDEIO_PROCESSOR_ENGINE_EXPORTS Operation {
public:
    /**
     * @brief Destroys the operation.
     * @throws Nothing.
     */
    virtual ~Operation();

    /**
     * @brief Returns the name the catalog resolves the operation by.
     * @return A UTF-8 name, never null, valid for the life of the operation.
     * @throws Nothing an implementation should let escape.
     * @par Thread-safety
     * Must be safe to call concurrently.
     * @par Requirements
     * UR-EXT-1
     */
    virtual const char*  name() const = 0;
    /**
     * @brief Returns how the operation consumes the selected channels.
     * @return PerChannel or Joint.
     * @throws Nothing an implementation should let escape.
     * @par Thread-safety
     * Must be safe to call concurrently.
     * @par Requirements
     * UR-ALG-3
     */
    virtual ChannelArity arity() const = 0;
    /**
     * @brief Tells whether a Joint operation needs every selected channel to
     * share one type.
     * @return True when mixed types are refused before the run.
     * @throws Nothing an implementation should let escape.
     * @par Thread-safety
     * Must be safe to call concurrently.
     * @par Requirements
     * UR-ALG-3
     */
    virtual bool         requiresUniformTypes() const = 0;
    /**
     * @brief Returns the input types the operation computes without narrowing,
     * saturating or losing precision.
     * @return The set; applying the operation to any other type is refused
     * with an error naming the operation and the type.
     * @throws Nothing an implementation should let escape.
     * @par Thread-safety
     * Must be safe to call concurrently.
     * @par Requirements
     * UR-IMG-11, UR-ALG-4
     */
    virtual DataTypeSet  supportedTypes() const = 0;
    /**
     * @brief Returns the border context the operation needs on each side of a
     * tile, in level pixels.
     * @return The halo; the framework supplies exactly this much.
     * @throws Nothing an implementation should let escape.
     * @par Thread-safety
     * Must be safe to call concurrently.
     * @par Requirements
     * UR-EXT-3
     */
    virtual Halo         halo() const = 0;
    /**
     * @brief Returns the type the operation writes for an input type.
     * @param inputType A type in supportedTypes().
     * @return The type of the output planes.
     * @throws Nothing an implementation should let escape.
     * @par Thread-safety
     * Must be safe to call concurrently.
     * @par Requirements
     * UR-IMG-11
     */
    virtual DataType     outputType(DataType inputType) const = 0;

    /// The parameter bag that reconstructs this operation. Must round-trip:
    /// `OperationCatalog::create(name(), parameters())` yields an equivalent
    /// operation. This is what lets a constructed operation be lowered into a
    /// PipelineSpec (5.4) and recorded against a stage (UR-STG-4).
    /// @return The bag, valid for the life of the operation.
    /// @throws Nothing an implementation should let escape.
    /// @par Thread-safety
    /// Must be safe to call concurrently.
    /// @par Requirements
    /// UR-STG-4, UR-EXT-1
    virtual const OperationParameters& parameters() const = 0;

    /**
     * @brief Computes one tile: reads context.input(), writes context.output()
     * and emits objects through the context.
     * @param context The tile's context; valid only during the call.
     * @throws ProcessorError Any subclass, to fail the tile.
     * @par Thread-safety
     * Called concurrently on different tiles; must be reentrant.
     * @par Requirements
     * UR-EXT-1, UR-EXT-4
     */
    virtual void apply(OperationContext& context) = 0;

    // Global-context operations only; default implementations do nothing.
    /**
     * @brief Tells whether the operation needs a whole-slide pass before
     * apply() (accumulate, combine, finalizeAccumulation).
     * @return False by default.
     * @throws Nothing.
     * @par Thread-safety
     * Must be safe to call concurrently.
     * @par Requirements
     * UR-EXT-4
     */
    virtual bool needsGlobalContext() const;
    /**
     * @brief Accumulates one tile into this operation's state.
     * Does nothing by default.
     *
     * Each tile accumulates into a fresh instance of its own, and that one
     * instance sees every plane of the tile: a PerChannel operation is called
     * once per plane, each call a single-plane input told apart by its
     * planeSourceChannel(); a Joint operation once, with all planes.
     * @param context The tile's context.
     * @throws ProcessorError Any subclass, to fail the run.
     * @par Thread-safety
     * Called on one instance from one thread at a time.
     * @par Requirements
     * UR-EXT-4
     */
    virtual void accumulate(const OperationContext& context);
    /**
     * @brief Merges another instance's accumulated state into this one. Does
     * nothing by default.
     * @param other An operation of the same kind.
     * @throws ProcessorError Any subclass, to fail the run.
     * @par Thread-safety
     * Not concurrent with accumulate() on either operand.
     * @par Requirements
     * UR-EXT-4
     */
    virtual void combine(const Operation& other);
    /**
     * @brief Computes the global result from the combined state, before
     * apply() runs. Does nothing by default.
     * @throws ProcessorError Any subclass, to fail the run.
     * @par Thread-safety
     * Called once, from one thread.
     * @par Requirements
     * UR-EXT-4
     */
    virtual void finalizeAccumulation();

    /**
     * @brief Tells whether the operation is applied per tile with a halo or
     * to the whole analysis level at once.
     * @return TileRequirement::Halo by default.
     * @throws Nothing.
     * @par Thread-safety
     * Must be safe to call concurrently.
     * @par Requirements
     * UR-EXT-3, UR-EXT-4
     */
    virtual TileRequirement tileRequirement() const;
    /**
     * @brief Returns the working memory a WholeLevel operation needs per pixel
     * of the analysis level, so the run can be refused before it starts when
     * the level does not fit the budget.
     * @return Bytes per level pixel; 0 by default.
     * @throws Nothing.
     * @par Thread-safety
     * Must be safe to call concurrently.
     * @par Requirements
     * UR-EXT-4
     */
    virtual int64_t workingSetBytesPerLevelPixel() const;
    /**
     * @brief Tells whether the operation emits objects, and therefore has a
     * target layer the run resolves from its "targetLayer" parameter.
     * @return False by default.
     * @throws Nothing.
     * @par Thread-safety
     * Must be safe to call concurrently.
     * @par Requirements
     * UR-EXT-4
     */
    virtual bool producesObjects() const;
    /**
     * @brief Returns how many output planes a Joint operation writes for a
     * number of input planes. Not consulted for PerChannel
     * operations, which yield one plane per input plane.
     * @param inputPlaneCount Number of input planes; at least 1.
     * @return @p inputPlaneCount by default.
     * @throws Nothing.
     * @par Thread-safety
     * Must be safe to call concurrently.
     * @par Requirements
     * UR-ALG-3
     */
    virtual int32_t outputPlaneCount(int32_t inputPlaneCount) const;
};
/** @brief Shared ownership of an operation, as the catalog and pipeline hold it. */
using OperationPtr = std::shared_ptr<Operation>;

} // namespace slideio::processor
