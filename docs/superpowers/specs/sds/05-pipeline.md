# SDS 05 — `pipeline`

**Module:** `pipeline` · **Library:** `slideio-processor-engine` · **Depends on:** `core`, `config`

The framework UR-EXT-4 promises: tiling, halo supply, memory budgeting,
threading, progress, cancellation and object emission, so an operation author
writes only per-tile computation. The only module that links OpenCV.

Conventions in [00-conventions.md](00-conventions.md).

---

## 5.1 Operation contract — `pipeline/operation.hpp`

```cpp
enum class ChannelArity : int32_t {
    PerChannel,   // applied independently to each selected channel
    Joint         // consumes the selected channels together
};

/// What an operation needs to see of a level to compute one tile.
enum class TileRequirement : int32_t {
    Halo,       // a tile grown by the declared Halo
    WholeLevel  // the whole analysis level as a single tile
};

struct Halo {                              // POD
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
    static Halo uniform(int32_t n);        // n >= 0, else InvalidArgumentError
    static Halo none() noexcept;
    bool isEmpty() const noexcept;
};

/// Partial intensity statistics for one fragment, carried because the pixels
/// are gone by the time the fragment's component closes. Integer count,
/// float sums; combined in canonical order (5.8).
struct IntensityPartial {                  // POD, exempt from pimpl per C-2
    int64_t count;
    double  sum;
    double  sumOfSquares;
    double  minimum;
    double  maximum;
    int32_t channel;
};

/// What the framework tells an operation about the run when it finishes one
/// of the operation's objects.
struct ObjectFinishContext {               // POD, exempt from pimpl per C-2
    double  pixelSizeMicrons;              // level-0 pixel size; 0 when unknown
    int32_t level;                         // the run's analysis level
};

class OperationContext {
public:
    ~OperationContext();
    OperationContext(const OperationContext&) = delete;
    OperationContext& operator=(const OperationContext&) = delete;

    const BlockView& input() const noexcept;
    /// The tile's samples as read from the scene, before any operation of the
    /// chain: the same halo rect as input(), one plane per selected channel
    /// whatever the arity. input() itself when the run supplied none, as a
    /// call outside a chain does. Segmenters measure intensities here
    /// (`06-algorithm.md` 6.7).
    const BlockView& sourceInput() const noexcept;
    BlockView&       output() noexcept;

    /// Where emitted objects go. Null for operations that produce only
    /// raster output.
    ObjectSink* objectSink() const noexcept;

    /// Where asserted containment goes (UR-RES-5). Null when the run has no
    /// store behind it — a directly-constructed Pipeline — so an operation
    /// that attaches must check before using it.
    EdgeSink* edgeSink() const noexcept;

    /// Where an operation attaches a label or a measurement to an object some
    /// earlier stage wrote (`03-storage.md` 3.7). Null on the same terms as
    /// `edgeSink`, and additionally null when the stage declared no
    /// `annotatesLayer`, so an operation that classifies must check.
    AnnotationSink* annotationSink() const noexcept;

    /// A raster bound to this run under `name`, covering this tile's halo
    /// rect at the run's analysis level — the same geometry as `input()`.
    /// Null when nothing was bound under that name, so an operation that
    /// wants coarse context must check and degrade rather than assume.
    const BlockView* auxiliary(const char* name) const noexcept;

    /// The layer this operation writes, already resolved from its
    /// `targetLayer` parameter against the bindings of the run. Invalid for
    /// operations that produce only raster output.
    LayerId targetLayer() const noexcept;

    int32_t level() const noexcept;        // the pyramid level being read

    /// Level coordinates — the space `input()` and `output()` are indexed in,
    /// and the space the declared halo is measured in.
    Rect64 tileRectAtLevel() const noexcept;
    Rect64 haloRectAtLevel() const noexcept;
    /// The whole analysis level, {0, 0, width, height}; the halo rect is not
    /// clipped to it. Empty for a context built outside a run.
    Rect64 levelRectAtLevel() const noexcept;

    /// The same tile in level-0 coordinates, for an operation that needs to
    /// relate itself to slide position.
    Rect64 tileRect() const noexcept;
    Rect64 haloRect() const noexcept;

    /// Level to level-0, through the library's single mapping (5.6). Emitted
    /// geometry must be in level-0 coordinates (UR-SEG-4); this is how an
    /// operation working on level pixels gets there.
    double  levelScale() const noexcept;

    /// The level-0 pixel size in micrometres, from the scene's resolution;
    /// 0 when unknown or anisotropic. The same at every analysis level.
    double  pixelSizeMicrons() const noexcept;

    Point64 toLevel0(Point64 levelPoint) const noexcept;
    Polygon toLevel0(const Polygon& levelPolygon) const;

    const ChannelSelection& channels() const noexcept;
    const CancellationToken& cancellation() const noexcept;

    /// The run's restriction, rasterised over this tile's halo rect, or null
    /// when the run is unrestricted (5.5). Level coordinates, like `input()`.
    ///
    /// Consulting it is OPTIONAL. The framework filters what this operation
    /// emits whether or not it is read, so an operation that ignores it is
    /// correct and merely wasteful; an operation that reads it skips work it
    /// would otherwise do and discard.
    const RasterMask* mask() const noexcept;

    /// Adds to the run's total the objects this call dropped as truncated in its
    /// view of the tile; a negative count throws InvalidArgumentError. ThreadSafe.
    void countTruncatedObjects(int64_t count);

    /// Records a fragment touching a tile seam, for cross-tile merging.
    /// Level-0 coordinates, like every emitted polygon.
    /// Equivalent to the four-argument form with no partials.
    void emitSeamFragment(const Polygon& fragment, LayerId layer);
    void emitSeamFragment(const Polygon& fragment, LayerId layer,
                          const IntensityPartial* partials, int32_t partialCount);
    /// As above, with the identity of the region the fragment belongs to:
    /// fragments join only when layer and join key are both equal. The
    /// other two forms mean join key 0.
    void emitSeamFragment(const Polygon& fragment, LayerId layer,
                          const IntensityPartial* partials, int32_t partialCount,
                          int64_t joinKey);
};

class Operation {
public:
    virtual ~Operation();

    virtual const char*  name() const = 0;
    virtual ChannelArity arity() const = 0;
    virtual bool         requiresUniformTypes() const = 0;
    virtual DataTypeSet  supportedTypes() const = 0;
    virtual Halo         halo() const = 0;
    virtual DataType     outputType(DataType inputType) const = 0;

    /// The parameter bag that reconstructs this operation. Must round-trip:
    /// `OperationCatalog::create(name(), parameters())` yields an equivalent
    /// operation. This is what lets a constructed operation be lowered into a
    /// PipelineSpec (5.4) and recorded against a stage (UR-STG-4).
    virtual const OperationParameters& parameters() const = 0;

    virtual void apply(OperationContext& context) = 0;

    // Global-context operations only; default implementations do nothing.
    virtual bool needsGlobalContext() const;
    virtual void accumulate(const OperationContext& context);
    virtual void combine(const Operation& other);
    virtual void finalizeAccumulation();

    // Layout and output declarations; the defaults are those of an ordinary
    // tiled, raster-only operation.
    virtual TileRequirement tileRequirement() const;                  // Halo
    virtual int64_t workingSetBytesPerLevelPixel() const;             // 0
    virtual bool producesObjects() const;                             // false
    virtual int32_t outputPlaneCount(int32_t inputPlaneCount) const;  // inputPlaneCount
    virtual bool isDeterministic() const;                             // true
    virtual const char* origin() const;                               // ""
    virtual int64_t workingSetBytesPerTile() const;                   // 0
    virtual int64_t accumulatedStateBytes(int32_t planeCount) const;  // 0
    virtual bool objectsAreEightConnected() const;                    // false

    // Object-producing operations only: called once for every object the
    // operation produces, before it reaches the run's sink. The default keeps
    // the object unchanged.
    virtual bool finishObject(ObjectRecord& record,
                              const ObjectFinishContext& context) const;  // true
    // Whether finishObject may change or discard an object; when false the
    // run passes the operation's objects on without calling it.
    virtual bool finishesObjects() const;                             // true
};
using OperationPtr = std::shared_ptr<Operation>;
```

**Thread-safety:** `Operation` implementations must be reentrant — `apply` is
called concurrently on different tiles, and `finishObject` concurrently from
the threads evaluating tiles and merging fragments. `OperationContext` is
ThreadConfined, one per tile. **Requirements:** UR-EXT-1 to UR-EXT-4,
UR-ALG-3, UR-ALG-4, UR-IMG-11, UR-EXE-2, UR-SEG-3, UR-SEG-5, UR-SEG-7.

This is the public extension interface, and built-in operations use it with no
privileged access to internals (UR-EXT-2). Its acceptance criterion is that at
least one built-in filter is demonstrably implementable outside the library
using only public headers — which `algorithm` satisfies for the entire catalog
by compiling against public headers only (design 5.2).

The four declaration methods are the contract the framework enforces before a
run starts:

- `arity()` — `PerChannel` always accepts a selection mixing `Byte` and
  `Float32`, since each channel is processed alone. `Joint` may restrict, and
  `requiresUniformTypes()` says whether it does (UR-ALG-3).
- `supportedTypes()` — applying the operation to an undeclared type raises an
  error naming both the operation and the type (UR-IMG-11). No operation may
  silently narrow, saturate or lose precision.
- `halo()` — the framework supplies exactly this much border context, so
  neighbourhood operations are correct at tile seams (UR-EXT-3). Declaring a
  halo of 0 for an operation that needs one produces a detectable seam, which
  is the test that confirms the mechanism is what provides correctness.

Further declarations have defaults, so an ordinary tiled operation that only
produces a raster overrides none of them:

- `tileRequirement()` — `Halo` says the operation needs its tile plus
  `halo()`; `WholeLevel` says it needs the analysis level in one block,
  because its result is not decomposable (flooding that propagates the length
  of a component, a region-adjacency graph). An enum rather than a `bool`, so
  a third layout can arrive without another virtual, and not a sentinel inside
  `Halo`, which is used arithmetically on every rect path. For a chain
  containing a `WholeLevel` operation, `Pipeline::validate()` (5.5) lays the
  run out as a single tile whatever the configured tile size, and refuses it
  before any pixel is read when the level does not fit the memory budget
  ([whole-level operations](../2026-10-11-whole-level-operations.md) §3.3).
  Forcing one tile rather than throwing on a configured size that does not
  cover the level is deliberate: the default size comes from
  `tileSizeFromSlide`, which is not a choice about this operation.
- `workingSetBytesPerLevelPixel()` — the operation's own working memory per
  pixel of the rect one `apply()` call is given, excluding the input and output
  blocks the framework already counts. One meaning serves both layouts: for a
  `Halo` operation the rect is the tile grown by the halo, for a `WholeLevel`
  operation it is the level. A `PerChannel` operation counts one plane, since
  it is applied one plane at a time, and the same figure covers `accumulate()`
  in a global-context pass. It is what turns "fits at level 4" into a check
  with numbers in it, and what each tile's memory reservation counts (5.5).
- `producesObjects()` — true when the operation emits objects, and therefore
  has a target layer: the string parameter `"targetLayer"`, which
  `validate()` resolves against `Pipeline::bindLayer` (5.5) and
  `OperationContext::targetLayer()` returns.
- `outputPlaneCount(n)` — how many planes a `Joint` operation writes for `n`
  input planes. Not consulted for `PerChannel` operations, which are applied
  one plane at a time and so yield one plane per input plane. An operation
  that cannot take `n` planes throws from it, `InvalidArgumentError` or, for
  an operation whose planes have fixed meanings such as RGB,
  `ChannelArityError`, naming the operation; `validate()` asks it for every
  `Joint` step, so the run is then refused before any pixel is read (5.5).
- `workingSetBytesPerTile()` — the operation's own working memory per
  `apply()` or `accumulate()` call that does not scale with the pixels: a
  window's copy, a histogram's bins, a kernel's coefficients. Like the
  per-pixel figure it excludes the framework's blocks and may depend on the
  parameters. Together the two bound what one call holds beyond its input and
  output: the rect's area × `workingSetBytesPerLevelPixel()` +
  `workingSetBytesPerTile()`.
- `accumulatedStateBytes(planeCount)` — for a global-context operation, the
  bytes one instance holds between calls: after `accumulate()` on any number of
  tiles of `planeCount` planes, and after `combine()` with any number of
  instances of the same parameters. `planeCount` is the plane count of the
  operation's input step in the run's plan. The figure excludes what the two
  working-set figures cover during a call, and defaults to 0. The run reserves
  it for the instances it keeps (5.5, "Memory"; 5.9) and is refused before it
  starts when it does not fit the memory ceiling.
- `objectsAreEightConnected()` — for an operation producing objects, true
  when two of its pixels touching only at a corner belong to one object. The
  executor tells the seam merger, per target layer, so that fragments of such
  an operation whose pixels meet diagonally across a seam join (5.7). False by
  default: objects are 4-connected.

The working-set figures are upper bounds, not estimates: an operation that
allocates more than it declares lets a run exceed its memory ceiling. A
negative figure counts as 0. An operation that calls into a library whose
allocations it cannot see, such as the backend's internal buffers, declares
that library's share from the library's source; when the backend gives each of
its threads its own buffers, the figure includes one share per backend thread.
The two working-set figures are per call; memory an operation keeps between
calls is what `accumulatedStateBytes()` declares, an upper bound the
reservation trusts. A global-context operation whose state grows with the
tiles it has seen cannot declare an honest bound and must keep its state
bounded.

Every declaration, these included, must be constant for the life of the
operation; the framework reads them once, before the run.

**Every object passes through its producer's `finishObject`.** An object found
whole in one tile and the same object merged from seam fragments (5.7) reach
the run's sink by different roads, so whatever the producer does to its objects
— a selected set of measurements, calibrated units, an area filter, holes
removed — must be done in one place both roads pass, or the two would disagree
and UR-SEG-3 would fail. That place is `finishObject`. The framework calls it
once per object after the rings are normalised (5.5) and the standard
measurements applied (below), and after the object has passed the anchor tests
that decide whether it is stored at all — tile ownership and the restriction
for a tile object, the restriction for a merged one (5.5, 5.7). The order
cannot change a result, because finishing never moves the anchor; it only
spares finishing the objects that are dropped anyway. It may add or remove
measurements and remove hole rings; it must not change the outer ring, whose
topmost-then-leftmost vertex is the object's identity, nor the layer. Returning
false discards the object, counted in the run's `droppedByFilter` counter.
`ObjectFinishContext` carries what the operation cannot know from the record
alone: the level-0 pixel size and the analysis level. The default returns true
and changes nothing, so an operation that emits finished measurements itself is
unaffected. An operation whose `finishesObjects()` returns false declares that
it has no finishing step: the framework reads it once per producer, when it
builds the finishing wrapper, and passes that operation's objects on as it
passes those of a layer with no producer — without calling `finishObject`,
without copying the record, and without counting anything in
`droppedByFilter`. It is true by default, so an operation that overrides
`finishObject` alone is still finished. Because a merged object is finished by
the operation producing its layer, **a layer has at most one producer in a
run**, which `validate()` checks
(5.5); the framework routes each record by its layer to that producer, through
one wrapper of the run's sink (`detail::FinishingSink`) that the tile filter
and the merger both emit into. The merger hands each merged record over with
`ObjectSink::emitOwned` (`01-core.md` 1.9), so the finishing step finishes that
record in place; the tile filter does the same with any record it owns (5.5),
and counts an object only when the finishing step's `emitKept` or `emitOwned`
returns true, that is, when the object was not discarded.

**`pixelSizeMicrons()`** is the scene's level-0 pixel size in micrometres:
the mean of SlideIO's x and y resolution × 10⁶ when both are finite, positive
and agree to within 10⁻⁶ relative, else 0 — unknown or anisotropic pixels; that
rule is `pixelSizeMicronsFromResolution` (`01-core.md` 1.5).
The executor and the lazy path fill it from the run's scene, so calibration
needs no parameter and a recorded configuration stays slide-independent. It
is the same at every analysis level; a level pixel is `levelScale()` times
as large. `algorithm`'s `sceneIsCalibrated` and `scenePixelSizeMicrons`
(`06-algorithm.md` 6.8) apply the same rule.

`Halo::uniform` throws `InvalidArgumentError` for a negative width and is
therefore not `noexcept`: a negative halo is refused, never silently clamped.
`Halo::none()` is all zeros, and `isEmpty()` is true when all four widths are
zero. The members of `Halo` and `IntensityPartial` are declared one per line
only so that each carries its own documentation; the layout is the same.

**Only the framework builds an `OperationContext`.** It is pimpl'd and not
copyable, and its constructor is private to the library, so an operation can
receive one but never make one. Every pointer it hands out is borrowed from the
run and valid only during the call. Where the run supplied nothing, the
accessors still return something safe: a null `BlockView` for `input()` and
`output()`, `ChannelSelection::all()` for `channels()`, and a token that is
never cancelled for `cancellation()`. `auxiliary(nullptr)` returns null.
`toLevel0` maps through the single mapping of 5.6, and the polygon overload
maps vertex by vertex, keeping rings, their order and which are holes.

`parameters()` costs an extension author one stored member — the bag their
factory was handed — and it is the pivot the whole recording story turns on.
Without it a constructed `OperationPtr` is opaque: it can be executed but not
described, so a pipeline assembled from the typed factories of 6.2 could never
be written to a stage record, and the two construction paths of 6.2 could not
be shown equivalent. A conformance test round-trips every catalog operation
through `create(name(), parameters())` and asserts the declarations match.

**Two coordinate spaces, both named.** An operation walks `input()` in level
pixels and must emit in level-0 (UR-SEG-4), so the conversion exists whether
or not the API admits it. Naming both spaces in the accessors — `tileRect`
versus `tileRectAtLevel` — is cheaper than a single ambiguous `tileRect` whose
meaning depends on the run, and `toLevel0` means the conversion has one
implementation rather than one per operation author. That matters beyond
tidiness: `SeamMerger` merges fragments by exact vertex coincidence, so two
authors rounding differently at a shared seam produce objects that never join
(5.6).

`emitSeamFragment` is how an object-producing operation hands a
boundary-touching region to `SeamMerger` (5.7) instead of emitting it as a
finished object. A fragment brings its intensity partials with it, because the
pixels are gone by the time its component closes (5.7), so the four-argument
form carries one `IntensityPartial` per channel; the two-argument form passes
none. The five-argument form adds a **join key**, the identity of the region
the fragment belongs to: the merger joins two fragments only when their layers
and their keys are both equal, so distinct regions touching across a seam —
neighbouring labels of a label raster, neighbouring watershed basins — stay
distinct objects. The key must name the region the same way in every tile, from
its samples or from level coordinates, never from a tile-local index. An
operation whose regions are separated by background uses 0, which is what the
other two forms pass. `IntensityPartial` is declared here rather than in
`seam_merger.hpp` because the context, the merger and the measurement formula
below all need it. A negative `partialCount`, or null `partials` with a
positive count, throws `InvalidArgumentError`. A run with no seam merger throws
`InternalError` from either form, so an operation emitting fragments into a run
that cannot merge them fails rather than losing objects. Where no object is
wanted by design, fragments are dropped silently instead: in a lazy read
(`readBlock`, `asScene`), which calls no sink, and in a global-context
accumulation pass, which writes none.

**`mask()` is an optimisation, never a correctness obligation.** Making it
advisory is deliberate: UR-EXT-2 requires a built-in operation to be
implementable outside the library with no privileged access, and an extension
author who has never heard of restrictions must not be able to produce a stage
that violates UR-STG-3 by forgetting a check. Correctness therefore lives in
one framework-owned place (5.5) and performance lives here, which also means
the catalog can adopt mask-awareness operation by operation without any
intermediate state being wrong.

It covers the **halo** rect rather than the tile rect, because that is the
region `input()` spans. A mask stopping at the tile edge would leave a
neighbourhood operation unmasked exactly where seam correctness matters.

**Edges and annotations are not filtered.** The framework's ownership and
restriction filters (5.5) apply to `objectSink()` only; what an operation hands
to `edgeSink()` or `annotationSink()` is passed on as made. An operation must
therefore assert edges, and attach labels and measurements, only for objects
anchored in its `tileRect()` and admitted by `mask()` — for objects it emits,
exactly those `objectSink()` keeps — so that an object seen in several tiles'
halos is asserted once, by the tile owning its anchor, and nothing outside the
restriction is. For these two sinks, reading `mask()` is not optional. Both
sinks are called concurrently from the run's workers and must be ThreadSafe.

The global-context group serves two-pass operations such as whole-slide Otsu:
`accumulate` runs over every tile, `combine` merges the accumulators,
`finalizeAccumulation` computes the result, then `apply` runs with it
(UR-PIX-5, UR-EXE-1). Each tile accumulates into a fresh instance of its own
(5.8), and that instance sees every plane of the tile: a `PerChannel` operation
is called once per plane, each call a single-plane input told apart by its
`planeSourceChannel`, and a `Joint` operation once with all planes.

### One formula for object measurements — `pipeline/standard_measurements.hpp`

```cpp
void applyStandardMeasurements(const Polygon& level0Polygon,
                               const IntensityPartial* partials,
                               int32_t partialCount,
                               MeasurementSet& out);
```

**Thread-safety:** Reentrant; `out` is ThreadCompatible. **Requirements:**
UR-SEG-3, UR-SEG-5.

UR-SEG-3 requires an object finished inside one tile and the same object merged
from seam fragments (5.7) to carry identical measurements. That holds only if
both are measured by one formula, and since `algorithm` compiles against
public headers only, the formula is public. It sets `area` from
`Polygon::area()` in `SquarePixels` and `perimeter` from
`Polygon::perimeter()` in `Pixels`, both in level-0 pixels, and for each
partial with a positive count, in `Intensity`: `intensity.<channel>.mean` =
sum / count, `intensity.<channel>.stddev` =
√max(0, sumOfSquares / count − mean²) (population), `intensity.<channel>.min`
and `intensity.<channel>.max`. A partial with count 0 contributes nothing.
Partials of one channel must already be combined — counts and sums added,
minimum and maximum taken — and a repeated channel throws
`InvalidArgumentError`, as do a negative count and null partials with a
positive count; `out` is then unchanged. Measurements of other names in `out`
are kept and those of the same name replaced. Calibrated units, and every
measurement beyond these, are added by the producer's `finishObject`
(`06-algorithm.md` 6.8), which the framework calls after this function on both
paths.

## 5.2 Operation parameters — `pipeline/operation_parameters.hpp`

```cpp
class OperationParameters {
public:
    OperationParameters();
    OperationParameters(const OperationParameters& other);
    OperationParameters(OperationParameters&& other) noexcept;
    OperationParameters& operator=(const OperationParameters& other);
    OperationParameters& operator=(OperationParameters&& other) noexcept;
    ~OperationParameters();

    OperationParameters& setInt(const char* key, int64_t value);
    OperationParameters& setDouble(const char* key, double value);
    OperationParameters& setBool(const char* key, bool value);
    OperationParameters& setString(const char* key, const char* value);
    OperationParameters& setDoubleArray(const char* key, const double* values, int32_t count);

    bool getInt(const char* key, int64_t& out) const noexcept;
    bool getDouble(const char* key, double& out) const noexcept;
    bool getBool(const char* key, bool& out) const noexcept;
    const char* getString(const char* key, const char* fallback) const noexcept;
    int32_t getDoubleArray(const char* key, double* buffer, int32_t capacity) const;

    int32_t count() const noexcept;
    const char* keyAt(int32_t index) const;
    int32_t toJson(char* buffer, int32_t capacity) const;
};
```

**Thread-safety:** ThreadCompatible. **Requirements:** UR-STG-4, UR-RES-11.

A typed key-value bag rather than per-operation structs, because operations are
resolved by name from the catalog and their parameters must round-trip through
the stage configuration recorded in the store. `toJson` produces that record.

**Keys and values.** Keys are UTF-8, non-null and non-empty; a setter given
anything else throws `InvalidArgumentError` naming the setter. Entries are kept
in ascending bytewise key order, so `keyAt(i)` ascends and two bags with the
same entries are equal, and serialise identically, whatever order they were set
in. Setting an existing key replaces it, type included. `setDouble` and
`setDoubleArray` refuse a NaN or infinite value with `InvalidArgumentError`
naming the key, because JSON cannot hold one and a recorded stage must replay.
`setDoubleArray` accepts `count == 0`, an empty array, and refuses a negative
count or null `values` with a positive count, leaving the bag unchanged.
`setString` reads a null value as empty. The moved-from bag is empty and
reusable.

**Getters never coerce.** `getInt` on a key holding a double returns false and
leaves `out` untouched, and the same holds for every typed getter. `getString`
returns `fallback` when the key is absent or not a string, and its pointer is
valid until the next non-const call. `getDoubleArray` follows C-5: it returns
the stored count, fills up to `capacity`, and a null buffer queries; an absent
or differently typed key returns 0. `keyAt` out of range throws
`InvalidArgumentError`.

**The JSON form** is one object, keys in stored order, no insignificant
whitespace. An integer is written as a JSON integer and a double always with a
fraction or exponent, in the shortest form that round-trips, so the type
survives the trip: `1.0` reads back as a double, not an integer. Booleans,
strings and arrays of such numbers are written as JSON writes them. Reading
inverts it: a JSON integer reads as an integer, a number with a fraction or
exponent as a double, and an array, whose every element must be a number, as an
array of double. An integer outside the signed 64-bit range throws
`InvalidArgumentError` rather than being read as a double, which would change
its type. `PipelineSpec::equals` (5.4) holds two bags equal when they have the
same keys with the same types and values, doubles compared with `==` and arrays
element-wise.

## 5.3 Operation catalog — `pipeline/operation_catalog.hpp`

```cpp
class OperationFactory {
public:
    virtual ~OperationFactory();
    virtual OperationPtr create(const OperationParameters& parameters) const = 0;
    virtual const char*  name() const = 0;
    virtual ChannelArity arity() const = 0;
    virtual bool         requiresUniformTypes() const = 0;
    virtual DataTypeSet  supportedTypes() const = 0;
    virtual Halo         halo() const = 0;
    virtual const char*  correctnessReference() const = 0;
    virtual const char*  complexityNote() const = 0;

    // Mirrors of the Operation declarations, with the same defaults, so the
    // catalog can answer before constructing anything.
    virtual TileRequirement tileRequirement() const;                  // Halo
    virtual int64_t workingSetBytesPerLevelPixel() const;             // 0
    virtual bool producesObjects() const;                             // false
    virtual int32_t outputPlaneCount(int32_t inputPlaneCount) const;  // inputPlaneCount
    virtual int64_t workingSetBytesPerTile() const;                   // 0
    virtual int64_t accumulatedStateBytes(int32_t planeCount) const;  // 0
};
using OperationFactoryPtr = std::shared_ptr<OperationFactory>;

class OperationCatalog {
public:
    static OperationCatalog& instance() noexcept;

    void registerFactory(OperationFactoryPtr factory);
    bool isRegistered(const char* name) const noexcept;

    OperationPtr create(const char* name, const OperationParameters& parameters) const;

    int32_t     count() const noexcept;
    const char* nameAt(int32_t index) const;

    ChannelArity arity(const char* name) const;
    DataTypeSet  supportedTypes(const char* name) const;
    Halo         halo(const char* name) const;
    const char*  correctnessReference(const char* name) const;
    const char*  complexityNote(const char* name) const;

    TileRequirement tileRequirement(const char* name) const;
    int64_t workingSetBytesPerLevelPixel(const char* name) const;
    bool producesObjects(const char* name) const;
    bool isDeterministic(const char* name) const;
    int32_t outputPlaneCount(const char* name, int32_t inputPlaneCount) const;
    int64_t workingSetBytesPerTile(const char* name) const;
    int64_t accumulatedStateBytes(const char* name, int32_t planeCount) const;

    /// Emits the operation-by-type support matrix as CSV, generated from the
    /// same declarations the runtime enforces.
    int64_t writeSupportMatrix(const char* path) const;
};
```

**Thread-safety:** ThreadSafe. **Requirements:** UR-IMG-12, UR-ALG-4,
UR-ALG-6, UR-ALG-8, UR-EXT-5.

The registry is what keeps `algorithm` a leaf: `workspace` resolves stages
against it by name and never links the catalog (design 4). A user-defined
operation registers the same way a built-in does (UR-EXT-5).

`writeSupportMatrix` is the build step UR-IMG-12 requires — the published
matrix is generated from the same declarations the runtime enforces, so
documentation cannot drift from behaviour, and a test asserts the two agree for
every operation and type pair.

`correctnessReference` is mandatory on every factory because UR-ALG-6 requires
each algorithm to name its reference — an analytic result, a published
implementation, or a peer-reviewed definition. The catalog takes no numerical
compatibility with OpenCV, so nothing external supplies that proof.

**Registration.** `instance()` is a function-local static. `registerFactory`
throws `InvalidArgumentError` for a null factory, a null or empty `name()`, a
null or empty `correctnessReference()`, a null `complexityNote()` (which may be
empty), or a name already registered, and the message names it. A factory is
never removed, so a pointer a query returns stays valid for the life of the
process. Names are kept in ascending bytewise order, so `nameAt(i)` does not
depend on registration order, though positions shift when a smaller name is
registered later. Every by-name query and `create` throws `InvalidArgumentError`
naming a null or unknown name.

**Determinism.** `OperationFactory::isDeterministic()` is `true` by default; an
operation whose results may vary between runs (a GPU model) overrides it to
return `false`. `OperationCatalog::isDeterministic(name)` answers for a
registered name and throws `InvalidArgumentError` for a null or unknown one. The
stage record lists such operations (SDS 04, 4.2).

**Origin.** `OperationFactory::origin()` says where an operation's code comes
from, for provenance. It is empty by default, which is what the library's own
operations return, and is never null; the returned text must stay valid for the
life of the factory, because the catalog and the stage-record writer keep the
pointer rather than a copy; an extension whose code lives elsewhere (the
Python bindings' stand-in factory, for example) overrides it. It must be safe to
call concurrently. `OperationCatalog::origin(name)` answers for a registered name
and throws `InvalidArgumentError` for a null or unknown one. The stage record
lists the non-empty origins (SDS 04, 4.2).

**What `create` checks, and which declarations the factory only predicts.**
`create` throws `InternalError` when the factory returns null, and also when the
operation's `name()`, `arity()`, `requiresUniformTypes()`, `supportedTypes()` or
`producesObjects()` differs from the factory's, naming the field. Those five do
not depend on the parameters, so the factory can state them exactly. `halo()`,
`tileRequirement()`, `workingSetBytesPerLevelPixel()`, `outputPlaneCount()`,
`workingSetBytesPerTile()` and `accumulatedStateBytes()` can — a box filter's
halo is its `radius` parameter, a median's window copy grows with its kernel,
a histogram's state with its bin count — and a factory sees no
parameters when asked. On a factory, and through the matching catalog queries,
those six therefore describe the operation built from **default** parameters,
and are not compared; the operation instance is authoritative for its own
parameters, and `Pipeline::validate()` asks the instance. An exception other
than a `ProcessorError` thrown by a factory surfaces as `InternalError` with the
original kept as `cause()`.

**The support matrix.** CSV, UTF-8, `\n` line endings. The header is
`operation,Byte,Int8,UInt16,Int16,UInt32,Int32,UInt64,Int64,Float16,Float32,Float64`,
then one row per operation in `nameAt` order, each cell `yes` or `no`. A cell is
`yes` exactly when the operation declares the type **and** the backend computes
it (5.10), and that test is the same internal function `validate()` applies, so
the published matrix and the runtime cannot disagree. On OpenCV 4 the `UInt32`,
`UInt64` and `Int64` columns are therefore `no` for every operation. The path is
UTF-8, taken through `std::filesystem::u8path`; the file is replaced if it
exists; the return value is the bytes written; a null or empty path throws
`InvalidArgumentError`, and a failure to open or write throws `IoError` naming
the path.

## 5.4 Pipeline specification — `pipeline/pipeline_spec.hpp`

```cpp
class PipelineSpec {
public:
    PipelineSpec();
    PipelineSpec(const PipelineSpec& other);
    PipelineSpec(PipelineSpec&& other) noexcept;
    PipelineSpec& operator=(const PipelineSpec& other);
    PipelineSpec& operator=(PipelineSpec&& other) noexcept;
    ~PipelineSpec();

    PipelineSpec& channels(const ChannelSelection& selection);
    PipelineSpec& restrictTo(const Rect64& roi);
    PipelineSpec& restrictToPolygon(const Polygon& polygon);

    /// The pyramid level to read from; -1 (the default) lets `LevelSelector`
    /// choose the coarsest adequate one (UR-IMG-4). A property of the run,
    /// not of any one operation — the tile grid is laid out at this level
    /// (5.6), so a chain cannot mix levels.
    PipelineSpec& analysisLevel(int32_t level);

    /// Appends an operation by catalog name. Nothing is resolved here; an
    /// unknown name fails at Pipeline::validate(), not at append.
    PipelineSpec& operation(const char* name, const OperationParameters& parameters);

    /// Appends an already-constructed operation, lowered to its name and
    /// parameters. The typed factories of `algorithm` return `OperationPtr`,
    /// and this is how a C++ caller who used them builds a spec.
    PipelineSpec& then(const OperationPtr& operation);

    int32_t     operationCount() const noexcept;
    const char* operationName(int32_t index) const;
    const OperationParameters& operationParameters(int32_t index) const;

    const ChannelSelection& channels() const noexcept;
    int32_t analysisLevel() const noexcept;        // -1 = auto
    bool   hasRoi() const noexcept;
    Rect64 roi() const noexcept;
    bool   hasPolygon() const noexcept;
    const Polygon& polygon() const noexcept;

    /// The recorded form. `fromJson` reconstructs a spec from it, so a stage
    /// configuration read out of a store replays without the code that wrote
    /// it (UR-RES-11).
    int32_t toJson(char* buffer, int32_t capacity) const;
    static PipelineSpec fromJson(const char* json);

    bool equals(const PipelineSpec& other) const noexcept;
};
```

**Thread-safety:** ThreadConfined to build; Immutable in effect once handed to
a `Pipeline`, which copies it. **Requirements:** UR-STG-4, UR-RES-11, UR-EXE-1,
UR-IMG-5.

**What a pipeline is, separated from the running of one.** `Pipeline` used to
carry both: a fluent builder (`select`, `restrictTo`, `then`) and an executor,
while `workspace::StageDefinition` carried a second, near-identical builder
whose only difference was that it named operations instead of holding them.
`StageRunner` existed in part to translate one into the other. Two builders for
one idea is how they drifted — `StageDefinition` grew `restrictToLayer` and a
JSON record, `Pipeline` grew `then(OperationPtr)`, and neither could express
what the other could.

The spec is a value: no scene, no store, no budget, no threads, nothing
resolved. That is what makes the three things below possible, none of which
worked when the configuration lived inside a half-built executor.

- **`toJson` / `fromJson` is a real round trip.** UR-STG-4 requires a stage's
  configuration be recorded, and UR-RES-11 requires it be recoverable from the
  store alone. Previously `StageDefinition::configurationJson` wrote a record
  that nothing could read back into a runnable form. A recorded spec now
  reconstructs and re-runs, which is what makes a stored analysis reproducible
  rather than merely documented.
- **`equals` gives staleness a definition.** `StageRegistry::declare` marks a
  stage stale when redeclared "with a different configuration" (4.3). Comparing
  serialised JSON makes that answer depend on key order and float formatting;
  comparing specs does not.
- **One builder.** `operation(name, params)` and `then(OperationPtr)` produce
  the same entry, because `Operation::parameters()` (5.1) lowers a constructed
  operation to the pair that reconstructs it. A caller using the typed factories
  of 6.2 and a caller naming operations from the catalog now build the identical
  spec, and a stage is not a second dialect.

**The two restrictions compose, and each replaces itself.** A spec may carry a
rectangle, a polygon, both or neither. Both means **intersection** — the run
covers what satisfies the two, which is the only reading under which adding a
restriction cannot enlarge a run. Calling either setter twice **replaces** that
restriction rather than accumulating: `hasRoi`/`roi` and `hasPolygon`/`polygon`
are single-valued, and a caller wanting several regions passes one polygon with
several outer rings (`01-core.md` 1.3), which is now explicitly legal.

`restrictToLayer` is deliberately **not** here: resolving a layer to the region
its objects occupy needs a store, which `pipeline` has no access to and must
not acquire. It stays on `StageDefinition` (4.2), and it does **not** lower
into this class at all. `StageRunner` binds it as a `TileMaskSource` on the
`Pipeline` instead (4.4, 5.5), the way it binds an `EdgeSink`.

That split is what keeps both halves of their contracts. A layer restriction
cannot be a spec field, because the spec is a value that must serialise and
replay from a store alone (UR-RES-11) while a mask source is a live object over
a live store. A layer restriction equally cannot be dropped from the stage
record, because `equals` defines staleness. It is recorded where it is declared
— `StageDefinition`'s JSON carries `restrictToLayer` by layer name (4.2) — and
bound where it is executed.

`channels` is an overload pair — fluent setter and getter on one class — rather
than the `setChannels` C-9 asks for. This class is simultaneously the builder
C-10 describes and the value that builder produces, and splitting it into a
`PipelineSpecBuilder` and a `PipelineSpec` the way `config` splits its pair
would double the type count to remove an overload that is unambiguous at every
call site. Noted here so a reviewer reads it as a decision.

**What the builder refuses.** `restrictTo` with an empty rectangle and
`restrictToPolygon` with an empty polygon throw `InvalidArgumentError`, since
an empty restriction would describe a run over nothing. So do `analysisLevel`
below −1, `operation` with a null or empty name, `then` with a null pointer or
an operation whose name is null or empty, and `operationName` or
`operationParameters` out of range. An unknown name does not throw at append;
it fails at `validate()`, as above. Without a restriction, `roi()` returns an
empty rectangle and `polygon()` an empty polygon. A moved-from spec is reset to
the default: all channels, level −1, no restriction, no operations.

**The recorded form, version 1**, is one object:

```text
{"specVersion":1,"channels":"all"|[c,…],"analysisLevel":L,
 "roi":[x,y,w,h],"polygon":[{"hole":bool,"points":[[x,y],…]},…],
 "operations":[{"name":"…","parameters":{…}},…]}
```

`roi` and `polygon` are present only when set, coordinates are JSON integers,
and each `parameters` member is the bag's own JSON form (5.2). `fromJson`
throws `InvalidArgumentError` for null text, a parse error (the message carries
RapidJSON's offset), a missing member or one of the wrong type or value (the
message names it), or a `specVersion` other than 1.

**What `equals` compares.** The channels — all against an explicit list, and
an explicit list element by element, **in order**, because the order of the
selection is the order of the planes an operation receives — the level, the
presence and value of each restriction, a polygon ring by ring with hole flags,
and the operations by name and by parameter equality (5.2). Parameter
insertion order never matters, because a bag has none.

## 5.5 Pipeline — `pipeline/pipeline.hpp`

```cpp
class Pipeline {
public:
    Pipeline(std::shared_ptr<slideio::Scene>, const Configuration&, const PipelineSpec&);
    ~Pipeline();

    Pipeline& memoryBudget(MemoryBudgetPtr);

    /// The name reported as `stageName` to `ProgressObserver::onProgress` and
    /// put in every `TileError`'s context. Default "pipeline"; null or empty
    /// throws `InvalidArgumentError`. `StageRunner` passes the stage's name.
    Pipeline& stageName(const char* name);

    /// Where operations assert containment. Borrowed, may be null, must
    /// outlive the run. `StageRunner` passes the stage's `EdgeWriter`.
    /// Called concurrently from the workers; flushed once, after they stop,
    /// on success only.
    Pipeline& edgeSink(EdgeSink*);

    /// Where operations attach labels and measurements to objects that
    /// already exist. Borrowed, may be null, must outlive the run.
    /// `StageRunner` passes the stage's `AnnotationWriter`. Called
    /// concurrently from the workers; flushed once, after they stop, on
    /// success only.
    Pipeline& annotationSink(AnnotationSink*);

    /// Where the chain's final raster output goes, one tile at a time.
    /// Borrowed, may be null, must outlive the run. `StageRunner` passes the
    /// stage's `RasterWriter` when it declared `writesRaster`; a run with
    /// none discards its final raster, as every run did before.
    Pipeline& rasterSink(RasterSink*);

    /// A second raster input, addressed by operations as `name` and read at
    /// the run's analysis level. Borrowed, may be null, must outlive the run.
    /// `StageRunner` passes one per `readsLayerAsRaster` declaration (4.2);
    /// a direct caller passes its own or none. Callable more than once with
    /// different names; the same name twice replaces.
    Pipeline& auxiliaryRaster(const char* name, TileRasterSource*);

    /// The run's restriction to the objects of some region, resolved one
    /// tile at a time (`01-core.md` 1.9). Borrowed, may be null, must
    /// outlive the run. `StageRunner` passes a source over the stage's
    /// restriction layer; a direct caller passes null or its own.
    Pipeline& maskSource(TileMaskSource*);

    /// Binds a layer name to an identifier for this run. Object-producing
    /// operations name their target layer as a string; this is the only
    /// mapping the pipeline has. `StageRunner` calls it once per declared
    /// layer; a direct caller calls it itself.
    Pipeline& bindLayer(const char* name, LayerId);

    /// The spec this pipeline was constructed from, copied at construction.
    const PipelineSpec& spec() const noexcept;

    /// The pyramid level this run reads from, chosen by `LevelSelector`
    /// (5.6). Valid after validate(); recorded against every layer the run
    /// writes, so a consumer can tell what the geometry's precision is
    /// (`03-storage.md` 3.3).
    int32_t analysisLevel() const;

    /// Lazy: evaluated per requested block. Construction computes no pixels,
    /// and the result is a valid input to another Pipeline.
    std::shared_ptr<slideio::Scene> asScene() const;

    /// Eager: streams the whole slide or ROI.
    void runInto(ObjectSink&, ProgressObserver*, const CancellationToken&);
    void run(ProgressObserver*, const CancellationToken&);
    /// What the last run's operations reported through countTruncatedObjects():
    /// one per dropped tile view, not per distinct object; 0 before the first run.
    int64_t truncatedObjectCount() const noexcept;

    int32_t levelCount() const;
    Rect64  rect(int32_t level) const;
    void    readBlock(int32_t level, const Rect64&, BlockView& out) const;

    /// Validates declared types, arity and halo against the scene. Called by
    /// run(); exposed so a caller can check before committing to a long run.
    void validate() const;
};
```

**Thread-safety:** ThreadConfined for configuration; the scene returned by
`asScene()` is ThreadSafe for reads. **Requirements:** UR-EXE-1, UR-ALG-1,
UR-ALG-5, UR-PIX-6, UR-IMG-5, UR-STG-3.

Every `pipeline` signature that takes or returns a scene spells the type
`std::shared_ptr<slideio::Scene>`, never `slideio::ScenePtr`: SlideIO defines
`ScenePtr` as a preprocessor macro rather than a type alias, so the qualified
name expands to `slideio::std::shared_ptr<…>` and does not compile. The type is
the same one SlideIO hands out.

A `Pipeline` is a spec bound to a scene, a configuration and a budget. It has
no configuration surface of its own beyond the runtime bindings and the stage
name above:
everything about *what* to compute comes from the spec, and everything here is
about *where and with what resources*. Building one reads:

```cpp
PipelineSpec spec;
spec.channels(selection)
    .restrictTo(roi)
    .then(makeGaussianBlur(1.2, 0))          // typed factory
    .operation("pixel.threshold_otsu", op);  // or by catalog name

Pipeline p(scene, cfg, spec);
p.memoryBudget(budget).bindLayer("nuclei", nuclei);
p.run(&progress, token);
```

Two execution modes, as UR-EXE-1 requires. `asScene()` builds a lazy pipeline
whose construction performs no pixel computation — demonstrated by
instrumentation, per the acceptance criterion — and whose output is itself a
valid input scene (UR-PIX-6). `runInto` is the eager pass for global-context
work and persisted results.

**Both ends of the pipeline convert between SlideIO's layout and this
library's.** A `slideio::Scene` block read delivers one interleaved buffer of
one data type, because a `cv::Mat` carries a single depth for all its
channels, while `BlockView` is planar with a `DataType` per plane
(`01-core.md` 1.8). So `readBlock` reads a mixed-type source scene one channel
per call and assembles the planes — which is what makes UR-IMG-9's mixed types
processable at all, rather than a property the planar layout happens to have.
`asScene()` runs the same conversion in reverse, interleaving the result for
each read; [the scene-source design](../2026-10-08-scene-source-design.md) §7
has the detail and the reason it costs no extra pass.

**No operation knows about `rasterSink`.** It takes the chain's last
`output()` per tile, exactly as `runInto` takes emitted objects, so persisting
a probability surface changes no operation and needs nothing in
`OperationContext`. An operation that wants its raster kept simply ends a
chain whose stage declared `writesRaster`.

### Reading a coarser pass as pixels

`auxiliaryRaster` is what makes multi-resolution processing a first-class
pattern rather than something a caller assembles outside the library. A chain
cannot mix levels — the grid is laid out at one analysis level (5.6), and 6.7
states the consequence as "a chain that needs two levels is two stages" — so
the coarse and fine halves are two stages by construction. What was missing
was a way for the second to *read* the first at pixel granularity:
`restrictToLayer` lowers to a `TileMaskSource` and answers only whether to
compute here.

**The auxiliary is resolved through the run's own level mapping, never its
own.** A `TileRasterSource` is handed a `BlockView` that already carries the
tile's level, level rect and level-0 rect, so it rasterises into the
coordinates the run is already using. Letting an auxiliary bring its own
mapping is the exact bug 5.6 removed from the tile grid: a non-dyadic pyramid
reporting a 4.0001 downsample factor would put the coarse raster up to a level
pixel out of register with the slide, and a refinement reading one pixel off
its own evidence fails in the way that looks like a tuning problem.

**A name, rather than extra channels.** Widening `ChannelSelection`
(`01-core.md` 1.10) to carry a source identifier would reach every operation
signature in the catalog and every recorded configuration, to express
something only a few operations want. A named binding sits beside `edgeSink`,
`annotationSink` and `maskSource` — runtime wiring the spec does not describe
— and costs an operation that ignores it nothing.

### Where a restriction is checked

A restriction — a rectangle, a polygon, or a bound `TileMaskSource` — is
applied at three points, and it matters that they are three rather than one:

| Point | What it does | Why there |
|---|---|---|
| Grid construction | An object-producing tiled run's grid spans the level, others span the restriction's bounding box (5.6); tiles outside the region are skipped without a mask | Whole rows and columns of tiles never exist for a run that cannot produce objects; objects crossing the region edge meet real tiles beyond it |
| Tile dispatch | A worker builds the tile's mask first and skips the tile when it is empty, reading no pixels; an object reaching into a skipped tile gets it evaluated on demand (5.5) | UR-IMG-5: "reads only tiles intersecting it", as a strong tendency |
| Object emission | Each object is kept only if its **anchor** lies in the ROI (decided exactly, in level 0) and the mask is set there | UR-STG-3: "produces no objects outside those regions" |

The third is the authoritative one and the other two are optimisations that
change only cost. That separation is what lets both acceptance criteria hold at
once, and they do not hold together under either simpler arrangement:

- **Culling tiles alone is not enough.** A tile straddling the boundary is
  processed whole, so objects land outside the restriction and UR-STG-3 fails.
  The finer the mask the worse it gets, which is exactly backwards.
- **Masking the input pixels is wrong.** Filling the block outside the mask
  with a background value before `apply()` would make a blur, a morphological
  operation or a whole-slide Otsu see fabricated pixels, and UR-IMG-5's other
  half — "results match those obtained by processing the whole slide and
  cropping" — would fail near every boundary. Pixels are never altered by a
  restriction; only emission is.

**The filter sits downstream of `SeamMerger`, not inside the tile loop.** An
object straddling both a tile seam and the mask boundary is emitted by the
merger after its tiles close (5.7), never by the operation. Filtering fragments
as they were produced would drop some of a component and keep the rest, which
is the one-nucleus-becomes-two failure of 5.6 arriving by a different road. The
`ObjectSink` the executor is given is therefore wrapped, and the wrapper tests
finished objects only.

**The test is the anchor, and that is a deliberate choice among four.** An
object is in when (for an ROI, see below) the mask is set at its topmost-then-leftmost vertex — the
same point `ObjectId::fromAnchor` derives identity from (`01-core.md` 1.4,
decision 8). Centroid would need a computation the record already carries but
which no longer matches identity; intersection would admit objects mostly
outside; full containment would drop objects the restriction was meant to
include. Anchor is one `RasterMask::testLevel0` call, it cannot split an
object, and it gives the same answer in every worker regardless of which tile
produced the object, so UR-QA-3's worker-count invariance survives unchanged.

An ROI is tested exactly: in level 0, at the anchor, half-open
(`x <= anchor.x < x + width`, likewise in y), whatever the analysis level. The
mask built from an ROI is a skipping aid and what an operation sees through
`Operation::mask()`, and it is the level mapping of the rectangle,
`mapToLevel(roi)`, which holds the level pixel of every ROI anchor at every
scale. A polygon or a mask source is tested on the level mask, at the level
pixel the object was found in: the inverse of the level mapping, the largest
`L` with `llround(L × scale) <= anchor` (`RasterMask::testLevel0`, 1.17). An
ROI combined with one of them must pass both.

One consequence worth stating because a test will find it: an object whose
anchor falls just outside the mask is dropped whole, including the part inside.
With the fill rule's sub-pixel conservatism and `dilateMask`'s margin
(`01-core.md` 1.17) the mask is wider than the region a caller drew, so this
costs objects that genuinely straddle the drawn boundary and nothing else — and
UR-STG-3 asks for exactly that.

**This is not containment derivation.** `03-storage.md` 3.6 forbids any
component from running a point-in-polygon test "to validate, infer, or reject a
parent edge", and that rule is untouched here. A mask decides *what gets
computed*; a parent edge records *what a stage asserted*. The two will read as
contradictory to anyone who meets them in the wrong order, which is why both
sections now say so.

A three-operation chain runs within the memory ceiling with no intermediate
written to disk (UR-ALG-5). The same call works unchanged on a thumbnail and on
the reference slide; only the input differs (UR-ALG-1).

### What `validate()` checks

`validate()` runs these checks, in this order, before any pixel is read, so a
mis-specified stage fails in seconds rather than hours. Each failure is an
`InvalidArgumentError` unless another type is named.

1. The scene is non-null (also checked at construction).
2. The spec has at least one operation.
3. Every operation name is registered in the catalog; the message names it.
4. An explicit channel index is below the scene's channel count.
5. An explicit `analysisLevel` is within the scene's levels.
6. The region (5.6) is non-empty; otherwise the message is "the
   restriction does not intersect the scene".
7. Types. The check walks the chain with the per-plane types: the scene's
   channel types for the first operation, then each operation's `outputType`.
   For every operation and plane type `detail::operationAccepts` (5.3) must
   hold, else `UnsupportedDataTypeError` names the operation and the type. A
   `Joint` operation whose `requiresUniformTypes()` is true and whose planes
   have mixed types throws `ChannelArityError` naming the operation and the
   channels. A `Joint` operation's `outputPlaneCount` is then asked for its
   input's plane count, and what it throws refuses the run: a segmenter that
   measures intensity throws `InvalidArgumentError` for more than three
   measured planes besides the one it segments, and tissue detection
   `ChannelArityError` for other than three (`06-algorithm.md` 6.7).
8. Target layers (below), then the layout of objects (below).
9. Every bound auxiliary raster declares at least one plane of a type with a
   size; otherwise the message names its binding. An auxiliary that would
   fail on its first tile is refused here instead.
10. Whole-level layout, budget and level, then the per-tile memory check (below).

The results (resolved level, target layer per operation, plane types per step,
the grid) are cached until the next binding call or the next `validate()`.
`run` and `runInto` call `validate()` first, and `analysisLevel()` before a
successful `validate()` throws `InvalidArgumentError`.

**Target layers.** For an operation whose `producesObjects()` (5.1) is true,
the `"targetLayer"` string parameter must name a layer bound with `bindLayer`.
When it is absent and exactly one layer is bound, that layer is the target.
Otherwise `validate()` raises `InvalidArgumentError` naming the operation and
the layer, or "no target layer" when none was named.
`OperationContext::targetLayer()` (5.1) returns what was resolved, so an
operation author never resolves a layer name and the same operation object
behaves identically whether a stage or a direct caller ran it. Operations that
do not produce objects are not consulted.

**One producer per layer.** Two operations producing objects that resolve to
the same layer are refused, "the operation and operation X both target layer
\"L\"", with Y in `ErrorContext::operationName` — names only. A layer's objects are finished by the operation producing it
(5.1), and a merged object carries no trace of which operation emitted its
fragments, so a second producer would leave the finisher ambiguous.

**Nothing grows an object producer's tiles.** An operation producing objects
finds its seams on its own tile rect, and a later operation's halo would grow
that rect, so its fragments would miss the seams the merger joins on. A chain
in which an operation producing objects is followed by any operation with a
non-zero halo is therefore refused, "the operation produces objects and must
not be followed by a halo", with X in `ErrorContext::operationName`. A point operation after it is accepted.

**A per-channel object producer gets one plane.** A `PerChannel` operation
producing objects is applied once per plane and emits into its one layer, so
over several planes it would store every object once per plane under the same
`ObjectId` — a label raster stored as identical channels, for one, would give
each region twice. A chain giving such an operation more than one plane is
refused, "the operation produces objects and runs per channel, so it must be
given exactly one channel", with X in `ErrorContext::operationName` — the name
only. A `Joint` producer is unaffected.

**Stage name.** `stageName(name)` sets the name the run reports: the
`stageName` of every `ProgressObserver::onProgress` call and the stage in every
`TileError`'s `ErrorContext`. The default is `"pipeline"`; a null or empty name
throws `InvalidArgumentError`. `StageRunner` sets it to the stage's name
(`04-workspace.md` 4.4).

**Whole-level chains.** If any operation reports `TileRequirement::WholeLevel`,
the run is a single tile covering the region, whatever the configured tile
size. The configured size is the `tileSizeFromSlide` default, not a choice about
this operation, so the chain is laid out for what it needs rather than refused.
`validate()` then checks the level against the memory ceiling:

```
needed = levelPixels × (Σ input plane bytes + Σ output plane bytes over the chain
                        + Σ workingSetBytesPerLevelPixel)
         + max workingSetBytesPerTile
```

where `levelPixels` is the area of the region at the analysis level. The
sum of every operation's per-pixel working set is a deliberately conservative
bound — the operations run one after another, so only one of them holds its
working set at a time — and the per-call figures add only the largest. When
`needed` exceeds `MemoryBudget::ceilingBytes()`, `InvalidArgumentError` names
the first whole-level operation, the level, `needed` and the ceiling. When the
plane bytes alone fit the ceiling, so that the working sets decide, the message
also names the operation with the largest single working set (its level area ×
per-pixel figure + per-call figure), even when it is the sum of the working
sets, not that one, that exceeds the ceiling. The run's single tile is then
also subject to the per-tile check under "Memory" below, which charges the
largest working set, the whole-level operation's included, over the level as
any tile charges its operations'. A chain of `Halo` operations is subject to
neither whole-level rule.

**The analysis level.** An explicit `analysisLevel(n)` is used as given, and
`analysisLevel(-1)`, the default, resolves automatically. The spec carries no
requested output resolution, so "the coarsest adequate level" would have nothing
to be adequate to; the automatic choice is instead the finest level the chain
can run at. That is level 0 for a chain of `Halo` operations. For a chain with
a `WholeLevel` operation it is the finest level at which both the whole-level
budget and the per-tile bytes (below) fit the ceiling, skipping levels whose
mapped region is empty. When no level fits, the coarsest level decides the
error: if its whole-level budget does not fit, the whole-level budget error is
reported; if the budget fits but its single tile's bytes do not, the per-tile
error is reported. Because the single tile's bytes include the whole-level
operation's working set, the automatic choice can land on a coarser level than
the whole-level budget alone would allow. A caller that wants a coarser level
states it, with `analysisLevel(n)`
or `LevelSelector::levelForScale`.

**Memory.** Before it reads a tile a worker reserves that tile's bytes with
`reserveOrWait` and releases them when the tile is done. The bytes are the
source read, the reader's interleave buffer (as large as all the source planes
when one call reads every channel, else as large as the widest source plane),
every intermediate, the auxiliary rasters (once, over the tile
grown by the chain's halo) and the restriction mask, which is 1 byte per pixel
for a rectangle-only restriction and 2 when a polygon or a `TileMaskSource` is
involved. To those framework buffers it adds the working set of the operation
that declares the largest one:

```
haloArea = (tile width + chain halo left + right) × (tile height + chain halo top + bottom)
tileNeed = haloArea × framework bytes per pixel
         + max over operations of (haloArea × workingSetBytesPerLevelPixel
                                   + workingSetBytesPerTile)
```

The operations run one after another on a tile, so a tile holds one working
set at a time, and the largest bounds them all. The tile grown by the chain's
halo bounds every operation's own input rect, so it is the area each per-pixel
figure is charged against. Negative declarations count as 0 and the arithmetic
saturates. A whole-level chain's single tile is the level, so its reservation
charges the largest working set, the whole-level operation's among them, over
the level. The
eager run and the lazy mode reserve the same `tileNeed`. A tile whose bytes
exceed the ceiling on its own throws `InvalidArgumentError` from `validate()`,
naming the tile size, the tile size grown by the chain's halo, the bytes and
the ceiling, instead of waiting forever; when the framework buffers alone fit,
so that a working set decides, the message also names the operation with the
largest working set.

The working-set figures do not cover memory an operation keeps between calls.
A global-context operation's instances do: the run's instance and the per-tile
instances the ordered fold holds while they wait for their predecessors (5.8),
each with the state `accumulatedStateBytes(planeCount)` declares, such as a
thresholding operation's per-channel histograms. `planeCount` is the plane
count of the operation's input step, and the plan records the figure of every
global-context operation, clamped at 0. The state is accounted in three ways.

- `validate()` refuses a run when one tile's bytes, the sum of those states and
  one more instance of the largest exceed the ceiling (the sum saturates).
  The `InvalidArgumentError` names the operation with the largest state, the
  bytes and the ceiling. With no global-context operation the term is 0.
- The run reserves the sum of the states, for the run's instances, and the most
  the tile instances of any one accumulation pass may hold, with one
  `reserveOrWait` before its first pass, and releases it when `run()` returns,
  on every path (5.9).
- A pass admits at most as many live tile instances, in flight or held by the
  fold, as that figure allows: twice the worker count, or fewer when what the
  ceiling leaves beside one tile and the sum of the states holds fewer (5.8).

An instance's state beyond its declaration is not covered by the ceiling.

`memoryBudget(nullptr)` restores the default budget,
`makeMemoryBudget(config.memoryCeilingBytes())`, which is also what a run uses
when none was bound.

**A thread holding a tile reservation never waits for more memory.** While a
worker evaluates a tile it is marked, thread-locally, as holding that tile's
reservation (`detail::TileReservationMark`, which nests and is restored on every
exit path). A lazy read (below) on a marked thread — a worker of one pipeline
whose input is another pipeline's `asScene()` — reserves its inner tile's bytes
with `tryReserve` and, when they do not fit, evaluates the inner tile
unreserved. An unmarked thread, such as a viewer reading the scene, keeps the
waiting `reserveOrWait`. Without the rule two pipelines sharing one budget can
deadlock: every outer worker holds its tile's bytes and waits for inner bytes
that only those same workers' tiles would free. The cost is bounded and stated:
the ceiling can be overshot by at most one inner tile per outer worker, for as
long as that inner tile is evaluated. The seam merger follows the same rule from
the other side: it never waits for memory at all (5.7). The mark is
thread-local, so it does not follow work an operation hands to other threads
(a parallel loop reading a lazy auxiliary, say): those threads are unmarked and
wait as any other. A lazy read on an unmarked thread that waits for memory has
no cancellation token, so it cannot be cancelled; it returns once the bytes
fit.

**Bindings.** `auxiliaryRaster(name, nullptr)` removes that binding, and the
same name twice replaces. Every binding call invalidates the cached result of
`validate()`.

**Reading source pixels.** `detail::SceneReader` reads a rect of one level as
planes. The rect is clipped to the level, the clipped part is read with
`Scene::readResampledLevelBlockChannels`, and everything outside the level is
zero-filled by the reader itself, so a result never depends on SlideIO's
background value. When every selected channel has one type, one call reads them
all and the reader de-interleaves them into planes; otherwise it makes one call
per channel. A channel whose SlideIO type has no `DataType` is reported as
`Unknown`; validation and reads reject it, with `UnsupportedDataTypeError`
naming the channel, only when that channel is selected. Every SlideIO failure,
including one while describing the scene or reporting a level, surfaces as
`IoError` carrying the cause; on this path that holds for the level table
`LevelSelector` reads too, which on its own reports such a failure as
`InternalError` (5.6). The buffers are owned by `detail::PlaneBuffers`, one array per plane with
a row stride of width × sample size, which also produces the `BlockView`; its
`slideRect` is the level-0 edge mapping of the rect (5.6).

### Evaluating the chain on one tile

The chain's halo is the sum, edge by edge, of every operation's halo. Operation
*k* runs on the tile grown by the halos of operations *k+1…n*, and its input is
the previous output (the source read for *k = 1*) covering that region grown by
its own halo. Each operation's context reports its own output region as
`tileRectAtLevel` and its input region as `haloRectAtLevel`, so a context means
the same thing at every position of the chain.

A `PerChannel` operation is applied once per plane, on single-plane views. A
`Joint` operation is applied once on all planes and produces
`outputPlaneCount(n)` output planes. The evaluator allocates the output buffers
from `outputType` before `apply`. For a `Joint` operation every output plane
takes `outputType` of the type of its first input plane, and output plane *i*
carries the source channel of input plane *i* (the last input plane's when there are more
outputs than inputs). The final output, exactly the tile rect, goes to
`rasterSink` when one is bound.

**Pixels outside the analysis level are zero in every intermediate.** The
source read zero-fills outside the level, and between operations the evaluator
zeroes the part of each intermediate that lies outside the level as well. An
operation near the level border therefore sees the same neighbourhood in a tiled
run as in a whole-level pass, so a tiled run equals the whole-level pass at the
level border too (UR-IMG-5, UR-SEG-3).

**Auxiliary rasters** are filled once per tile over the first operation's input
rect, and each operation sees a view over its own input (halo) rect.

**The source read stays alive for the whole chain.** Each operation sees it as
`sourceInput()`, a view over its own input rect, so a segmenter late in a chain
measures the scene's samples rather than what earlier operations made of them
(`06-algorithm.md` 6.7). This needs no larger reservation: the tile reserves
the buffers of every step at once (5.5), the source's included.

Only operations whose `producesObjects()` is true get a non-null `objectSink()`;
every operation gets the edge and annotation sinks bound to the run.

**Which tile owns an object.** A finished object emitted through `objectSink()`
is kept only when its anchor lies in the emitting tile's level-0 `tileRect`
(half-open). An operation that sees halo pixels may therefore emit the same
object from two tiles without duplicating it in the store; an object outside is
dropped silently and counted. This ownership test comes before the restriction
filter. An object that passes both has its rings normalised by the framework
(`detail::normaliseRings`, as a merged object's are, 5.7), so operations emit
rings in any rotation — the outer ring positive (clockwise on screen, y down),
holes negative — and a polygon already in normal form is passed on without
being copied again. An operation that emits with `emitOwned()` hands its record
over: the filter normalises that record in place (copying only its polygon)
and the finishing step finishes it, so the record itself is not copied; one
emitted with `emit()` is copied once, to be normalised or else to be finished.
It then goes to its producer's `finishObject` (5.1) and, unless that discards
it, to the run's sink. Every object a tile keeps is normalised this way, in a
lazy read (below) as in a run, so a degenerate ring — all its vertices
collinear, or a spike of zero area — fails the run, or the read, with
`InvalidArgumentError` instead of being stored.

### The tile mask

Per tile, before any pixel is read, the worker builds the tile's mask over its
**halo** rect at the level, and only when the run is restricted: all pixels set,
intersected with the ROI's level rectangle (`mapToLevel(roi)`, 5.5), then with the polygon, then with the bound
`TileMaskSource` (which fills a second mask that is intersected in). A tile
owns the objects anchored in its level-0 rect (`llround` of the level rect),
and an anchor is tested at the level pixel that is the inverse of that mapping,
so every anchor a tile owns has its level pixel inside the tile's rect, at any
scale and with any halo: the mask is exactly the halo rect. When the mask has no
set pixel inside the tile's rect, the tile is skipped: no pixel is
read and it is deferred, not complete (below). `OperationContext::mask()`
returns the tile's
mask, or null when the run is unrestricted. `detail::RestrictionFilter` wraps
the `ObjectSink` and admits an object when its anchor is in the ROI (`inRoi`) and the mask is set at it; for
objects emitted outside a tile, such as merged objects (5.7), it exposes
`admits(anchor)`, which builds a one-pixel mask at the anchor's level pixel
from the same three sources. Filtering never alters pixels.

**A skipped tile is deferred, not complete.** An admitted object can reach past
the mask's edge into a tile that holds no set mask pixel, and that tile
produces no fragments of it. The seam merger (5.7) therefore tracks four
states per tile, named as the enum names them. *Pending* is the initial state.
*Deferred* is a tile the restriction skipped: the final pass calls
`notifyTileSkipped` for it instead of `notifyTileComplete`, so it is not
complete, and a fragment beside it still records that it expects it.
*Demanded* is a deferred tile that `takeAwaitedDeferred` has returned, marked so
that it is returned at most once; it is not terminal, because it is still to be
evaluated. *Completed* is the end state, reached by `notifyTileComplete`, for an
evaluated tile (a demanded one included), or by `completeDeferred` for a
deferred tile left over. After the final pass the
executor loops: it takes the deferred tiles that some open component expects,
evaluates each once with the skip bypassed, through the same workers,
reservation and cancellation as the pass, reports it complete, and repeats
until none is awaited. It then calls `completeDeferred`, which completes every
deferred tile still left without evaluating it, and `resolve`. A demanded tile
is an ordinary evaluated tile except that it writes no raster tile, reports no
progress (the pass already counted it) and is counted in
`RunCounters::tilesEvaluatedOnDemand`, while staying in `tilesSkipped`. Its
tile objects pass the normal object filter, and admission by anchor still
decides what is stored. Accumulation passes still skip and are unchanged, so
global statistics do not move. The skip test uses the tile rect.

**Pass-fragment pruning, and its precondition.** Only a component holding a
fragment from a tile evaluated in the pass (a *pass fragment*) demands tiles; a
fragment from a demanded tile does not set the flag, and a union has it when
either side had it. Without this, every object a demanded tile found would pull
in its own neighbours and the demands would spread over dense objects. It is
sound because the pass completes every tile it evaluates before the first
demand, so an object with a pixel in an evaluated tile holds that tile's
fragment and reaches every skipped tile it crosses, while an object lying
wholly in skipped tiles has its anchor where the mask is zero and the
restriction drops it, whole or truncated. The precondition is that `skips()`
means the mask is zero over everything the tile owns, which is its rect at
any scale (`Restriction.SkipAgreesWithTheBuiltMaskAtANonIntegerScale` and its
halo and row variants).
The reproducer is `StageRunner.RestrictedObjectCrossingIntoASkippedTileIsNotTruncated`
(`tests/workspace/test_stage_runner.cpp`).

**What the pruning rests on: the anchor.** The argument that an object lying
wholly in skipped tiles is dropped needs the anchor to be a vertex of the
object's own boundary, its topmost-then-leftmost pixel corner. Only then does a
whole object admitted by its anchor have a pixel in the tile holding that
anchor, and hence a fragment there. An anchor defined any other way, such as a
centroid or a bounding-box corner, could lie in a skipped tile the object has no
fragment in, and this pruning would lose the object.

**The region edge.** An ROI's or a polygon's region is not the grid of an
object-producing tiled run: that grid spans the level, so a tile straddling the
region edge is a full tile and its seams to the tiles beyond are real seams
(5.7). Tiles whose rect misses the region are skipped without building a
mask, and an object anchored inside the region that reaches into one gets it
evaluated on demand, as for a mask-source restriction. The skip tests the tile
rect against the region, widened by one level pixel on every side at a
non-integer level scale (at an integer scale it is not widened): the polygon's
conservative raster sets a pixel whose real square touches the polygon, which
can lie one pixel outside the bounding box's outward mapping, on any side. The test is
only a shortcut; the anchor filter decides what is stored, so an object
crossing the ROI or bounding-box edge is stored whole when its anchor is
admitted and not at all otherwise. A chain with a WholeLevel operation is out of
this rule: it analyses the region as one image, so its objects stop at the
region edge, because the chain never sees beyond the region.

**Effect on stored results.** Objects of a restricted run that cross into a
skipped tile are now stored whole, so against results stored before the fix:
the identifier of an affected object changes, because a whole object's anchor
differs from the truncated one's; counts can fall, because a truncated object
admitted by its old anchor is dropped when the whole object's anchor lies in a
skipped tile; and results previously stored by restricted stages are affected
and are not marked stale. Re-run restricted stages and record the change in the
verification evidence.

**Cost.** Evaluating tiles on demand costs time, never correctness, and one
very large object can pull in many tiles. UR-IMG-5's "reads only tiles
intersecting it" is therefore a strong tendency, not a guarantee: a skipped
tile is read when an object reaching out of an evaluated tile expects it.

### The lazy mode: `levelCount`, `rect`, `readBlock`, `asScene`

**The output pyramid mirrors the source from the analysis level down.**
`levelCount()` is the source scene's level count minus `analysisLevel()`; output
level *k* is the chain evaluated on source level `analysisLevel() + k`, laid out
as `validate()` would lay the run out at that level, with every halo in that
level's pixels — never rescaled (5.6). `rect(k)` is the region (5.6) at
that source level, in its coordinates. The plan of every output level is
resolved once, on the first lazy call, from a snapshot of the bindings that
matter to pixels — spec, scene, configuration, budget, mask source and
auxiliary rasters; never the sinks. That first call validates when no
validation result is held; the build runs under a mutex, so concurrent first
calls build it once, and a build that throws leaves nothing, so the next call
tries again. Any binding call drops the snapshot. A chain containing a
global-context operation throws `InvalidArgumentError("global-context
operations need runInto")` from `levelCount`, `rect`, `readBlock` and
`asScene`: a block evaluated on demand has no accumulation pass, which is the
eager mode's job (UR-EXE-1).

**`readBlock(level, rect, out)`** requires `out` to have the chain's final plane
count and types and `rect`'s size (`InvalidArgumentError` otherwise). The rect
may extend beyond `rect(level)`; pixels outside it, or outside the restriction,
read as zero. Every tile of that level's grid meeting the rect is evaluated on
the calling thread by the same tile evaluator the eager run uses — with no
object, edge or annotation sink, and with seam fragments dropped — and its
final output is copied into `out`. Each tile reserves its bytes from the budget
while it is evaluated, under the rule above for marked threads. It is const and
ThreadSafe. A tile failure surfaces as `TileError`, as in a run.

**`asScene()` computes no pixels.** It validates (metadata only), builds the
lazy source (`detail::PipelineSceneSource`, a SlideIO `ISceneSource`) and
returns `slideio::makeScene(source, metadata)`; a test proves zero source
reads at construction. The scene keeps the snapshot and may outlive the
`Pipeline`; the mask source and auxiliaries stay borrowed. Its description:

| Field | Value |
|---|---|
| `name` | the source scene's name + `"/pipeline"` |
| levels | one per output level; **level 0 is the analysis level, at scale 1**, and level *k* has source level `analysisLevel() + k`'s size and magnification, its scale relative to the analysis level's, and `defaultTileSize` (5.6) |
| `rect` | the source scene's origin with the analysis level's size, so the source rect itself when the analysis level is 0 |
| resolution, magnification | the source's, converted to the analysis level |
| colour profile | the source's |
| channels | one per final plane, named `"c0"`, `"c1"`, …, typed with `dataTypeToSlideio` |

Level 0 must be the analysis level because SlideIO requires a scene's level 0
to have scale 1 and to match its rect; describing a pipeline analysed at level
2 with the source's level-0 rect would be refused by `makeScene`. A level read
evaluates through `readBlock` and interleaves into SlideIO's buffer; channels
of different types are read one type per call, as SlideIO requires. A
scene-rectangle read picks the coarsest output level whose resolution is at
least the one requested on both axes, reads it and resamples to the block size
nearest-neighbour (the mapping of `cv::INTER_NEAREST`), so no value is
invented. The source declares concurrent reads.

### Running

`run(progress, token)` is `runInto` with an internal sink that counts and drops
what it is given. The object, edge, annotation and raster sinks are called
concurrently from the run's worker threads, so every implementation bound to
a run must be ThreadSafe; the internal sink of `run` counts atomically for the
same reason. Edge and annotation emissions pass through no filter (5.1).
`runInto` flushes the object, edge, annotation and raster sinks, once each and
in that order, after every worker has stopped, and only on success (5.9).

**Seam merging inside a run.** When any operation of the chain produces
objects, the executor's run owns a `SeamMerger` (5.7) for that run and routes
every `emitSeamFragment` to it; fragments bypass the ownership and restriction
filters, being partial by definition, and the merged object's anchor is tested
instead, through `RestrictionFilter::admits` handed to the merger. Merged
objects reach the same `ObjectSink` as single-tile ones, during the pass, before
any flush. The merger works in a scratch directory of the run's own,
`pipeline-<32 random lowercase hex digits>` under
`Configuration::scratchDirectory()` (the platform's temporary directory when
that is empty), created when the run starts and removed with everything in it
on every way out of the run, failure and cancellation included. That is what a
`Pipeline` used directly does; `StageRunner` is to pass a subdirectory of the
workspace instead (5.7). A chain producing no objects gets neither merger nor
directory.

**Truncated objects.** An operation that drops an object it cannot deliver whole
reports it with `OperationContext::countTruncatedObjects(count)`. The count
adds to an atomic member of the run's counters, which every worker of the run
shares and the start of each run resets, so the total is the sum over all tiles
and workers. Only the final pass counts, tiles a restricted run evaluates on
demand included (they belong to the final pass). In a global-context
accumulation pass the context has no counter, so the call validates its
argument and drops it; a lazy read (a block of `Pipeline::asScene()`) counts
into run counters of its own, which it discards. The total counts dropped tile
views, not distinct objects: an operation reports what it dropped in one
tile's view, so each truncated object counts at least once, one dropped in
several tiles' views (`LabelImageOperation`: a part of it whose own anchor
another tile owns) counts once per view, and the figure depends on the tile
size; 0 means nothing was dropped. A negative
count throws `InvalidArgumentError`, which fails the run as any operation error
does (a `TileError` naming the operation). `Pipeline::truncatedObjectCount()`
reads the total after `runInto` or `run` and is 0 before the first run; it
keeps what a failed run had counted. `StageRunner::truncatedObjects()` (SDS 04)
copies it after a successful run and is 0 before, and after a failed run; a
second `run()` reports its own count, not a sum.

**The level rect.** `OperationContext::levelRectAtLevel()` is the whole analysis
level, `{0, 0, width, height}` in level pixels, which the tile evaluator copies
from the run plan into every context it builds; a context built outside a run
(a test's direct application) reports an empty rect, read as "level unknown".
The halo rect is not clipped to the level, so an operation that must tell a side
of its halo rect lying inside the level (beyond which the slide goes on) from one
on or beyond the level's edge (beyond which nothing is) compares the two.

**Anchor ownership instead of seam joining (`LabelImageOperation`).** The seam
merger joins fragments of one region across tiles by layer and join key. That
works when a region's identity is the same in every tile: a foreground pixel
(`segment.connected_components`), a sample value (`segment.contours`). A model's
label image (SDS 06, 6.7) numbers its objects tile by tile, so one object can
carry different values on either side of a seam and one value can name two
objects; no join key names it the same way in both tiles. `LabelImageOperation`
therefore never emits a seam fragment. Over the halo rect it labels each maximal
4-connected set of one non-zero value as one region, traces it, and keeps it only
in the tile whose half-open tile rect holds its anchor, the topmost-then-leftmost
vertex of its first outer ring, the point `ObjectId::fromAnchor` uses; every
other tile seeing the region ignores it, so it is emitted once, whole, through
`objectSink()`. The tile filter's own anchor test then admits it as it does every
single-tile object. A region reaching a side of the halo rect that lies strictly
inside the level may continue beyond what the tile saw: it is dropped and
counted with `countTruncatedObjects`, once per tile view that drops a part of
it (see **Truncated objects** above). A side on or beyond the level's edge never
truncates, and labels predicted beyond the level are cleared first, so an object
at the level's edge is stored cut at the edge and is complete. The halo is thus
the largest object extent, below and beside its anchor, the operation delivers;
the working set grows with it and validation refuses one beyond the memory
ceiling before any tile runs. The objects are those a single tile covering the
level gives, whatever the tile size and worker count, as long as the model's
regions are (UR-QA-3); seam joining keeps its place for operations whose
regions have a tile-independent identity.

`runInto` and `run` throw what `validate()` throws; a `TileError` for a failed
tile (`hasTile()` true), or for a failure creating or finalising a
global-context operation or a failure of the seam merge after the last tile
that is neither a `ProcessorError` nor `std::bad_alloc`, such as a standard
exception from the object sink (`hasTile()` false); `CancelledError` when the token
is cancelled or an operation throws it; `IoError` when the run's scratch
directory cannot be created or a spilled fragment cannot be read back; and
`InternalError` when a tile failed and its failure could not be recorded, or
when seam fragments do not merge into one object. Nothing is flushed on any of
these.

## 5.6 Tiling and level selection — `pipeline/tile_grid.hpp`

```cpp
namespace slideio {
class Scene;
} // namespace slideio

class TileGrid {
public:
    /// The grid is laid out in LEVEL coordinates. `regionAtLevel` and the
    /// tile size are level-`level` pixels, so every tile boundary falls on an
    /// exact level pixel edge.
    TileGrid(const Rect64& regionAtLevel, int32_t level,
             int32_t tileWidth, int32_t tileHeight,
             const LevelSelector& selector);
    TileGrid(const TileGrid& other) noexcept;
    TileGrid(TileGrid&& other) noexcept;
    TileGrid& operator=(const TileGrid& other) noexcept;
    TileGrid& operator=(TileGrid&& other) noexcept;
    ~TileGrid();

    int32_t level() const noexcept;

    int64_t tileCount() const noexcept;
    int64_t columns() const noexcept;
    int64_t rows() const noexcept;

    /// Level coordinates: what an operation indexes, and what `BlockView`
    /// covers. Halo is in level pixels too.
    Rect64 tileRectAtLevel(int64_t index) const;
    Rect64 tileRectAtLevelWithHalo(int64_t index, const Halo& halo) const;

    /// Level-0 coordinates, derived from the level rect through the same
    /// mapping for every tile. Adjacent tiles therefore share an exact
    /// level-0 boundary with no gap and no overlap.
    Rect64 tileRect(int64_t index) const;
    Rect64 tileRectWithHalo(int64_t index, const Halo& halo) const;

    int64_t tileIndex(int64_t column, int64_t row) const;
    bool    isEdgeTile(int64_t index) const;
};

class LevelSelector {
public:
    explicit LevelSelector(std::shared_ptr<slideio::Scene> scene);
    LevelSelector(const LevelSelector& other) noexcept;
    LevelSelector(LevelSelector&& other) noexcept;
    LevelSelector& operator=(const LevelSelector& other) noexcept;
    LevelSelector& operator=(LevelSelector&& other) noexcept;
    ~LevelSelector();

    /// The coarsest level whose resolution still satisfies `scale`, so a
    /// request at 1/16 never decodes level 0 and downsamples.
    int32_t levelForScale(double scale) const;
    int32_t levelForSize(int64_t targetWidth, int64_t targetHeight) const;
    double  scaleOfLevel(int32_t level) const;

    /// Level-0 to level. Rounds outward: the result covers every level-0
    /// pixel of the input, so a mapped ROI never loses a boundary pixel.
    Rect64  mapToLevel(const Rect64& level0Rect, int32_t level) const;

    /// Level to level-0. The single mapping implementation in the library:
    /// `llround(v * scaleOfLevel(level))` per ordinate, monotonic and
    /// deterministic, so one level coordinate always yields one level-0
    /// coordinate no matter which tile or thread asks.
    Rect64  mapToLevel0(const Rect64& levelRect, int32_t level) const;
    Point64 mapToLevel0(Point64 levelPoint, int32_t level) const noexcept;
    Polygon mapToLevel0(const Polygon& levelPolygon, int32_t level) const;
};

/// The tile size, in level pixels: the slide's native tiling for that level
/// when available and tileSizeFromSlide is set, otherwise the configured size.
void defaultTileSize(std::shared_ptr<slideio::Scene> scene, int32_t level, const Configuration& config,
                     int32_t& widthOut, int32_t& heightOut);
```

**Thread-safety:** Immutable. **Requirements:** UR-IMG-4, UR-EXE-6, UR-EXT-3,
**UR-SEG-3**, UR-QA-2, UR-QA-3, UR-IMG-5, UR-IMG-1.

All tile indices and coordinates are `int64_t`: the reference slide at 512 px
tiles is already ~30,000 tiles, and nothing here may cap image size (UR-IMG-1).

**The grid is built at the analysis level, not at level 0.** An earlier draft
took a level-0 region and tile size and mapped each tile down to the level
being read. That is correct only when the level's downsample factor is an exact
integer. `scaleOfLevel` returns a `double` because real pyramids are not
obliged to be dyadic — SVS levels routinely report factors like 4.0001 — and a
level-0 tile boundary then lands part-way through a level pixel. Two adjacent
tiles either both claim that pixel or neither does, and after mapping back
their fragments are offset by up to one level pixel, which is `scale` level-0
pixels.

`SeamMerger` joins fragments by exact vertex coincidence at the seam. Offset
fragments do not coincide, so they do not merge: one nucleus becomes two
objects, each with its own identifier and its own measurements. The count is
*nearly* right, which is the worst failure mode available — UR-SEG-3 is the
requirement the requirements document calls the hardest in the paper and the
most common failure of tiled WSI processing, and this defeats it silently on
exactly the slides whose pyramids are least regular.

Laying the grid out in level coordinates removes the possibility rather than
narrowing it. Tile boundaries are integers in the space the pixels actually
live in; the level-0 rectangles are then *derived* from those integers through
one mapping, so tile *n*'s right edge and tile *n+1*'s left edge are the same
number by construction, not by rounding agreeing twice. No tolerance is
introduced anywhere, so UR-QA-3's worker-count invariance and UR-QA-2's
tiling invariance both continue to hold by exact equality.

The ROI keeps its user-facing space: UR-IMG-5 specifies a rectangle in slide
coordinates, `PipelineSpec::restrictTo` takes level-0, and `Pipeline` maps it
down with `mapToLevel`, which rounds **outward** so no level-0 pixel of the
requested region is dropped on the way in.

**The region a restriction bounds.** `Pipeline` intersects, in level-0 coordinates,
the scene rect with whichever of the spec's restrictions are present — the ROI
rectangle, the polygon's `boundingBox()` — and maps the result down with
`mapToLevel`. A bound `TileMaskSource` does **not** narrow the grid: it is
resolved per tile and has no bounding box to offer without the whole-mask
computation that `04-workspace.md` 4.4 exists to avoid. Restricting to a layer
therefore lays out a grid over the whole scene and skips empty tiles at
dispatch, which costs one spatial query per non-overlapping tile and no pixel
reads.

The region is intersected with the level's extent after mapping, and its tile
size is `defaultTileSize` unless the chain contains a `WholeLevel` operation, in
which case the grid is one tile covering the region (5.5). When
`analysisLevel` is left automatic, a level whose mapped region is empty is never
chosen for a whole-level chain.

`tilesTotal` counts the grid, not the tiles that did work, so a heavily
restricted run reports fast progress through its empty region rather than
stalling at an unexplained count. `tilesCompleted` reaching `tilesTotal`
does not mean the run is done: tiles demanded by the seam merger (5.5) and the
merging run after the last progress call, possibly in several rounds.

The grid of an object-producing tiled run spans the level; any other run's grid
spans that region. The region (the scene, the ROI and the polygon's
bounding box intersected, at the level) is a `RunPlan` field and still bounds
`rect(k)`, the raster tiles written (a tile writes only its part inside the
region), and the pixels an accumulation pass applies to (a straddling tile
applies over its part in the region, so a reduction that ignores the mask
counts exactly the region's pixels). A final-pass tile applies over its whole
rect. `tilesTotal` counts the level grid for such a run, so an ROI- or
polygon-restricted object run reports more tiles than before, the out-of-region
ones as fast progress. The per-tile memory check uses a full tile
(`min(tile, level)` per side), because a straddling tile is full size.

Halo is declared, supplied and applied in **level** pixels. An operation's
neighbourhood is a property of its kernel, which works on the pixels it is
given — a 5-pixel radius means five of the pixels being processed, at any
level, and the framework never rescales it.

### The pyramid, exactly

`LevelSelector` copies the scene's level sizes at construction and keeps no
reference to the scene, so it and every grid built from it are plain values;
copies share only immutable state. A null scene, or a level reporting a
non-positive size, throws `InvalidArgumentError`; a failure inside SlideIO
surfaces as `InternalError` with the cause kept. The level count is the scene's
zoom-level count, and a scene that reports none has one level, level 0, the
scene rect. The scene type is spelled out for the reason given in 5.5.

**Two scale vocabularies, and they point in opposite directions.**
`scaleOfLevel(L)` is the level-to-level-0 factor,
`double(width(0)) / double(width(L))`: 1 for level 0, at least 1 elsewhere, and
not necessarily a power of two (4.0005 for a 2000-pixel level under an
8001-pixel level 0). It is the one factor per level that `RasterMask` carries.
`levelForScale(scale)` takes a **resolution fraction** instead, in (0, 1],
where 1/16 means a sixteenth of full resolution, and returns the coarsest level
`L` with `1 / scaleOfLevel(L) >= scale × (1 − 10⁻⁹)`, or 0 when no coarser level
qualifies. The tolerance absorbs only floating-point rounding, so a level
answers a request for exactly its own resolution; it is far too small to admit
a level that is genuinely coarser than asked, such as one with factor 4.0001 for
a request of 1/4.
`levelForSize(w, h)` returns the coarsest level whose width is at least `w` and
height at least `h`, or 0. An out-of-range level, a non-positive or non-finite
scale and a non-positive size throw `InvalidArgumentError`.

**The single mapping** is `llround(v × scaleOfLevel(level))` per ordinate, and
it has one implementation, which `mapToLevel0`, `OperationContext::toLevel0`
and `TileGrid::tileRect` all call. **A rectangle maps by its edges**, not by
its origin and size: `x0 = map(x)`, `x1 = map(x + width)`, width `x1 − x0`, and
likewise for y. Two rectangles that share a level edge therefore share the
level-0 edge exactly, which mapping a width separately would not guarantee. The
point overload is `noexcept`, so an in-range level is its precondition; given
an out-of-range level a debug build asserts and a release build returns the
point unchanged. The polygon overload maps
every vertex and keeps ring order and hole flags.

`mapToLevel` rounds outward and is the inverse of the single mapping, taken
in integers: `x0` is the largest level ordinate whose level-0 image is at most
`x`, and `x1` the smallest whose level-0 image is at least `x + width`; likewise
for y. A `double` division only seeds the search, and a bounded number of
±1 steps against `map` decides the answer, so the result is the same on every
platform (no `long double`, whose width differs between MSVC, x86-64 Linux and
arm64 Linux), and `mapToLevel0(mapToLevel(r))` contains `r` by construction.
On a 100000-wide level 0 over a 33333-wide level 1, the full width maps to
exactly 33333. It throws `InvalidArgumentError` for a negative width or height
and when the far edge or the result does not fit `int64_t`. The result is not
clipped to the level.

### The grid, exactly

Tile sides must be at least 1, the level in range, and the tile count must fit
`int64_t`; otherwise the constructor throws `InvalidArgumentError`. The region
is not checked against the level's size, and a region with a non-positive side
has no tiles. Tiles are numbered row-major from the region's top-left,
`tileIndex(c, r) = r × columns() + c`, and the last column and row are clipped
to the region. `isEdgeTile` is true in the first or last column or row. An
out-of-range index, column or row throws `InvalidArgumentError`, as does a halo
with a negative side.

**The halo rect is not clipped.** `tileRectAtLevelWithHalo` is the tile grown
by the halo on each side, whatever lies there. Halo beyond the level reads as
zeros, and halo beyond the region but inside the level reads **real pixels**.
The second is what UR-IMG-5's "results match those obtained by processing the
whole slide and cropping" needs: a blur at the edge of an ROI must see the
pixels beyond the ROI, exactly as it would have in the whole-slide run.
`tileRect` and `tileRectWithHalo` are the level rects mapped by their edges.

`defaultTileSize` takes the level's native tile size when
`tileSizeFromSlide()` is set and both of its sides are positive, and otherwise
`tileWidth()` × `tileHeight()` from the configuration. A null scene or an
out-of-range level throws `InvalidArgumentError` and leaves the outputs
unchanged.

## 5.7 Seam merging — `pipeline/seam_merger.hpp`

```cpp
// IntensityPartial is declared in operation.hpp (5.1).

class SeamMerger {
public:
    /// `scratchDirectory` is where fragments spill. Must exist and be
    /// writable; the merger owns every file it creates there and removes
    /// them on destruction.
    SeamMerger(const TileGrid&, MemoryBudgetPtr,
               const char* scratchDirectory);
    ~SeamMerger();

    /// Records a fragment touching a tile boundary. Called from worker
    /// threads, in any order.
    /// Joins only fragments of its layer and join key (5.1).
    void addFragment(int64_t tileIndex, const Polygon&, LayerId,
                     const IntensityPartial* partials, int32_t partialCount,
                     int64_t joinKey = 0);

    /// Reports that a tile is finished. Components all of whose bordering
    /// tiles have completed are merged and emitted to `sink` immediately,
    /// which is what keeps the resident set proportional to the working
    /// frontier rather than to the whole slide.
    void notifyTileComplete(int64_t tileIndex, ObjectSink& sink);

    /// Merges and emits whatever remains after the tile pass.
    void resolve(ObjectSink&, ProgressObserver*, const CancellationToken&);

    int64_t fragmentCount() const noexcept;
    int64_t mergedObjectCount() const noexcept;

    int64_t residentFragmentCount() const noexcept;
    int64_t spilledFragmentCount() const noexcept;
    int64_t spilledBytes() const noexcept;
    int64_t largestComponentFragmentCount() const noexcept;
};
```

**Thread-safety:** ThreadSafe. **Requirements:** **UR-SEG-3**, UR-QA-2,
UR-QA-3.

This is the component that decides whether the library is correct or merely
plausible. UR-SEG-3 is identified in the requirements as the hardest
requirement in the document and the most common failure mode in tiled WSI
processing, and its acceptance criterion is a dedicated test suite asserting
that object count, area, perimeter, centroid and intensity statistics are
identical across at least three tile sizes.

Union-find over fragments keyed by seam position. Because identifiers derive
from each merged object's anchor rather than from merge order (decision 8), the
order in which fragments are joined cannot affect the result — which is what
makes UR-QA-3's worker-count invariance hold through this component rather than
despite it.

Keying works by **exact** coincidence, with no tolerance, and that is only
sound because the grid is laid out at the analysis level (5.6): a seam is an
integer boundary in the space the pixels live in, and both neighbouring tiles
derive their level-0 edge from that one integer through one mapping. A grid
defined at level 0 and mapped down would leave the two edges up to a level
pixel apart on a non-dyadic pyramid, and fragments that should join would not —
splitting one object into two, each plausible, with no error raised.

### When two fragments are one object

A fragment's **seam contacts** are the maximal stretches of its rings lying on a
seam line of the grid — the level-0 edge `tileRect(c).x` of an interior column
boundary, likewise `y` for a row boundary; the outer boundary of the grid
is not a seam — recorded as a line and a half-open interval in level-0
integers. Two fragments of the same layer **and the same join key** (5.1), in
the tiles on either side of one seam line, join when they have contacts on
that line whose intervals **overlap with positive length**. Touching at a
single point does not join them on an ordinary layer: objects are
4-connected.
The contacts wait per pair of side-adjacent tiles, until both have completed,
on two sides — the lower tile index's and the higher's — each ordered by the
interval's begin and keeping its longest interval. A new contact looks only
at the other side, from the contacts beginning less than that longest
interval before its own begin up to the last beginning before its end: those
are the only ones its interval can overlap, so it does not scan every contact
waiting on the seam.

**8-connected layers.** On a layer whose producer declares
`objectsAreEightConnected()` (5.1), which the executor passes to the merger
per layer before the first fragment arrives, two fragments of different tiles
with the same key also join when they **share a ring vertex lying on a seam
line** — at a tile corner, with the diagonally adjacent tile, or at a point
along a seam between two edge-adjacent tiles. Each fragment then holds a pixel
touching that point, and any two pixels touching one point are 8-neighbours.
Together with the positive-length contacts this finds every pair of fragments
with 8-neighbouring pixels across a seam: a diagonal pair whose point is not a
vertex of one of the fragments has a 4-neighbour pair beside it. On such a
layer a fragment also expects every other tile touching one of its seam
vertices — three at a corner.
The points wait per pair of tiles, indexed by point, so a new fragment finds
the ones it shares without scanning the others, and they are dropped when
both tiles have completed.

**Join keys.** Without them the merger would join any two same-layer
fragments touching across a seam, so two regions that touch — neighbouring
labels, neighbouring watershed basins — would leave as one object, and
UR-SEG-8 and UR-QA-2 would fail. A key names the region a fragment belongs to
the same way in every tile; fragments of different keys never join, however
they touch, edge or point alike. The key waits with each contact in the
merger's memory, so a fragment written to the spill file keeps its key and the
spill record (below) does not carry it.

Joining is union-find; which fragment represents a component never reaches
the output, because identity comes from the merged object's anchor
(decision 8).

### When an object closes

A fragment in tile *t* with a contact on seam line *s* **expects** the tile on
the other side of *s* at that contact. A component closes when every tile
holding one of its fragments and every tile one of them expects has completed —
reported through `notifyTileComplete`, which the executor calls for every tile
the pass evaluated and for every deferred tile evaluated on demand. **A tile
skipped by the restriction is not complete**: it is deferred (5.5), and
completes only when it is evaluated on demand or `completeDeferred` completes
it. `notifyTileComplete(t, sink)`
re-examines only the components touching *t*; reporting a tile twice does
nothing more. A closed component is merged, measured, tested against the
restriction at its anchor (through the filter the executor hands the merger;
5.5) and emitted to `sink`, then forgotten. In a run the executor's `sink` is
the finishing wrapper of 5.1, so a merged object goes through its producer's
`finishObject` exactly as a tile object does. Objects are emitted from the
thread that completed the tile, outside the merger's lock, so the sink must be
ThreadSafe, as every object sink of a run is.

`resolve` runs after the pass and emits whatever remains — after a pass that
reported every tile, nothing does, except a fragment that arrived for a tile
already reported complete, which is held until then. A component still open at
`resolve`, some tile it holds or expects never having been reported, is a
broken run: `resolve` throws `InternalError` naming the layer and counts. That
is why the executor **does not call `resolve` after a cancellation or a tile
failure**: the run is failing anyway, and the merger's destructor releases
every reservation it holds and removes its spill file, so a failed run leaves
nothing behind.

### Merging fragments into one polygon

Merging is **edge cancellation, exact, in integers**. Core's `addRing` takes an
explicit hole flag rather than fixing orientation, so every input ring is first
oriented by its flag: outer rings to positive shoelace area in level-0
coordinates, holes to negative, putting the object on the same side of every
edge. Every axis-parallel ring edge is split at every fragment vertex lying
inside it (seam lines are axis-parallel, so no other edge can need it);
directed edges that coincide with opposite directions — the shared seams —
cancel; the rest are linked into rings, taking at a vertex with several
outgoing edges the one turning most sharply right, so rings touching at a point
separate rather than cross. On an 8-connected layer the linker takes the one
turning most sharply **left** instead, so an object whose pixels touch
diagonally keeps one outer ring passing twice through that point (which
`Polygon::addRing` accepts), while holes touching at a point stay apart; an
operation's own tracer applies the same rule, so an object found in one tile
and the same object merged agree. Positive rings are outer, negative ones
holes. The result is normalised by `detail::normaliseRings`, as the framework
normalises every tile object (5.5): collinear vertices removed, each ring
starting at its topmost-then-leftmost vertex, the single outer ring first,
then the holes ordered by first vertex (y, then x). A merged object and the
same object found whole in one tile therefore compare equal point for point.

A component whose edges do not close into rings, whose fragments overlap, or
that yields other than exactly one outer ring — the joins make a component
4-connected, or 8-connected on such a layer, so more than one is a defect —
throws `InternalError`.
The message names the layer and the fragment and ring counts, **never
coordinates or a bounding box**, which could carry clinical data.

### Holding fragments, and why most of them are not held for long

An earlier draft said only that "fragments are spilled rather than allowed to
grow unbounded," which named a mechanism without specifying one. The bound it
implied was also far worse than it needed to be, because it assumed every
fragment is held until the entire tile pass ends.

**A component closes as soon as every tile it holds or expects has
completed** (above). It does not need the pass to finish — it needs its own neighbourhood to finish.
`notifyTileComplete` is where that is decided, and it is why the interface
takes an `ObjectSink`: most merged objects are emitted during the tile pass,
not after it. Tiles complete in any order (5.9), so the rule is stated over
completion rather than over a sweep front, and it holds whatever order the
executor happens to produce.

The difference is the whole memory argument. At the reference scale — 10M
objects over 8×10⁹ px, so ~28 px across, at 1024 px tiles — about 5% of objects
touch a seam, giving on the order of 10⁶ fragments. Held to the end of the pass
at roughly 700 B–1 KB each (geometry plus partials), that is ~700 MB against a
2 GB ceiling, and at 512 px tiles it roughly doubles and breaks it. A nucleus
spans two to four tiles, so under incremental close its fragments live only
until those tiles finish: the resident set is proportional to the frontier of
in-flight tiles, which is bounded by worker count, not by slide size.

This also removes the tension between UR-SEG-3 and UR-EXE-2 that the
end-of-pass design created. UR-SEG-3's acceptance criterion requires identical
results across at least three tile sizes, so the test suite deliberately runs
small tiles — which under the old scheme was the configuration most likely to
exhaust the ceiling. The test that proves correctness is no longer the test
most likely to run out of memory.

### Spilling

What remains open for a long time is not the common case but the large one: a
tissue region can span an entire slide, and its component stays open until
nearly every tile is done. Those components are few and individually large,
which is the shape spilling suits.

Each fragment held in memory reserves its bytes in the run's budget when it
arrives: 16 per ring point, `sizeof(IntensityPartial)` per partial, and a fixed
64. The bytes are given back once its component has closed and been merged and measured,
when it is spilled, or when the merger is destroyed.

**`addFragment` never waits for memory.** It reserves with
`MemoryBudget::tryReserve`. When that fails, it writes out the fragments of the
**oldest-opened** other component still in memory (by the sequence number of
its first fragment) and tries again, until the reservation succeeds or nothing
else is left in memory; then it writes out the incoming fragment itself. A
fragment larger than the whole ceiling goes straight to the file. Waiting is
never right here: `addFragment` is called from inside a tile, whose own bytes
are part of what is taken, and the memory still taken may be held by the very
workers that would have to finish to free it, so a wait could never end. Union-
find entries, contacts and per-component bookkeeping stay resident, so joining
and closing decisions never require a read.
Choosing oldest-first is deliberate: a long-open component is by definition one
whose tiles are not finishing soon, so it is the cheapest to evict and the
least likely to be wanted again this minute.

What waits on the seams is not small in total: a fragment waits on each seam
it lies on with one edge contact per maximal interval, and on an 8-connected
layer also at each of its seam vertices for each other tile touching it, so a
jagged outline along a long seam leaves many. Edge contacts and seam points
stay in memory when their fragment is written out, so the budget holds their
bytes separately, with the tile pair they wait between:
`detail::kSeamEdgeContactBytes` per waiting edge contact and
`detail::kSeamPointContactBytes` per waiting point, from arrival until both
tiles of the pair complete. They are reserved the same way: while `tryReserve`
fails, the oldest-opened components are written out. When nothing is left to
write out, they are kept without a reservation, since joining needs them, and
the merger counts those bytes; this happens only when the workers' own tiles
hold the whole budget.

Spilled fragments are appended to one file per merger,
`seam-<32 random lowercase hex digits>.bin` in the scratch directory, created on
the first spill. A record is `int32 ringCount`; per ring `uint8 hole, int64
pointCount`, then the `int64 x, y` pairs; `int32 partialCount` and the partials.
Only its offset stays in memory. Spilled fragments are read back when their
component closes; that read-back is **not reserved**, since it is transient,
lasting only while the object is merged and measured, and refusing it would
leave the object nowhere to go. Joining, closing and the results are the same
whether or not anything was spilled. `spilledFragmentCount`, `spilledBytes`,
`residentFragmentCount` and `largestComponentFragmentCount` report what
happened.

Spill files belong to the merger and to one run. They are removed on
destruction, including on the failure path, so a stage that throws leaves
nothing behind. `StageRunner` is to pass a subdirectory of the workspace, which
keeps UR-STG-9's "copy or archive as one unit" true and makes orphan cleanup a
matter of clearing one known directory when a workspace opens after a crash. A
`Pipeline` used directly gives the merger a directory of the run's own under
`Configuration::scratchDirectory()`, removed when the run ends (5.5). Failure to
create, write or read the spill file surfaces as `IoError` naming the file,
never as a silently dropped fragment. A spill file holds derived object
geometry and intensity statistics, so the scratch directory should be
access-controlled like the results. A failure to remove the file or the run's
directory is ignored, leaving them behind; removal cannot be reported from a
destructor.

### What is not bounded by the budget

One merged object must materialise whole to be emitted. Peak memory is
therefore the working frontier plus the **largest single merged polygon**, and
no amount of spilling reduces the second term — a tissue contour at level 0 can
run to millions of points. `largestComponentFragmentCount` is exposed so a run
can report what it actually hit. In practice this is tens of megabytes and
comfortably inside a 2 GB ceiling; it is documented because it is the one part
of this component that does not scale down with the ceiling, and a caller
setting a very small budget should know which term will defeat them.

Two smaller terms are not reserved either. The merger keeps a union-find entry
and a bookkeeping slot for every fragment it has ever recorded — about 50 bytes
each — for the whole run, whether the fragment is resident, spilled or already
closed, so this term grows with the number of seam fragments the run emits
(about 50 MB for a million fragments). And a lazy read of a pipeline's scene on
a thread that already holds a tile reservation (5.5, `memoryBudget()`)
evaluates its tile unreserved when the bytes do not fit at once, so a run whose
input or auxiliary is such a scene can exceed the ceiling by up to one nested
tile's bytes per worker reading it.

### Measurements across a seam

Geometric measurements — area, perimeter, centroid, bounding box — are
recomputed from the merged polygon at close, so they are exactly what
single-tile processing would produce and nothing needs carrying: area and
perimeter by `applyStandardMeasurements`, the rest by the producer's
`finishObject` (5.1), which sees the merged polygon as it sees a tile object's.

Intensity statistics cannot be: the pixels are gone once a tile is done. Each
fragment therefore carries `IntensityPartial` per channel, and the merger
combines them with `detail::combinePartials`, per channel. `count` is integer
and combines order-independently; `sum` and `sumOfSquares` are floating point
and **must be combined in canonical fragment order** — ascending fragment
anchor (y, then x), ties by tile index, never arrival or close order — for
exactly the reason 5.8 gives for float reductions. Combining them in completion
order would make intensity statistics differ between runs at different worker
counts, and UR-QA-3 would fail intermittently and irreproducibly, which is the
hardest kind of failure to attribute to its cause.

The merged polygon and the combined partials then go through
`applyStandardMeasurements` (5.1), the same formula an operation applies to an
object it finished inside one tile, so the two paths cannot disagree on a
definition. The merged `ObjectRecord` carries the merged polygon, the layer and
those measurements; its identifier follows from its layer and anchor (decision
8), so it is the one a single-tile run would give the same object.

**What "identical across tile sizes" means, exactly.** Changing the tile size
regroups the float sums, so the claim depends on the sample type:

- **Byte** samples: every partial sum is an integer below 2⁵³ in `double`
  (`count × 255² < 2⁵³` holds for any component below about 1.4 × 10¹¹
  pixels), so the sums are exact and the measurements bitwise identical across
  tile sizes.
- **Float32** samples: the grouping of a float sum differs between tile sizes,
  so the UR-SEG-3 suite asserts a relative difference of at most 10⁻¹² for
  intensity statistics, and says why. Geometry is still exact.
- Wider integer types are not claimed: UInt16 loses exactness at about 2.1 M
  pixels, which a level-spanning region exceeds. `ComponentsOperation` admits
  only Byte and Float32 intensities. The catalog's segmenters measure every
  type (`06-algorithm.md` 6.7); for an integer type the sums stay exact, and the
  measurements bitwise identical, while they are below 2⁵³, which the tests'
  objects are.

Worker-count invariance (UR-QA-3) is exact for every type, because the
canonical order does not depend on the schedule.

## 5.8 Reductions — `pipeline/reduction.hpp`

```cpp
class Reduction {
public:
    virtual ~Reduction();
    virtual void accumulate(const BlockView& block, int64_t tileIndex) = 0;
    virtual void combine(const Reduction& other) = 0;
    virtual bool isOrderIndependent() const = 0;

protected:
    Reduction();
    Reduction(const Reduction& other);
    Reduction& operator=(const Reduction& other);
};

class Histogram64 : public Reduction {
public:
    Histogram64(int32_t binCount, double minValue, double maxValue);
    Histogram64(const Histogram64& other);
    Histogram64(Histogram64&& other) noexcept;
    Histogram64& operator=(const Histogram64& other);
    Histogram64& operator=(Histogram64&& other) noexcept;
    ~Histogram64() override;

    void accumulate(const BlockView& block, int64_t tileIndex) override;
    void combine(const Reduction& other) override;
    bool isOrderIndependent() const override;   // true

    int64_t bin(int32_t index) const;
    int64_t totalCount() const;           // int64: UR-IMG-2
};
```

**Thread-safety:** ThreadSafe for `accumulate`; `combine` requires exclusive
access to the target. **Requirements:** UR-PIX-5, UR-QA-3, UR-IMG-2.

Integer accumulators — histogram bins, pixel counts, areas — combine
order-independently, so worker count cannot affect them, and
`isOrderIndependent()` returns `true`. Floating-point accumulators must return
`false`, and the framework then combines them in canonical tile order.

**Canonical order, exactly.** `detail::OrderedFold<T>` is the one mechanism.
Workers hand in `(tileIndex, T)`, and the fold applies `combine` to the
accumulated value strictly in ascending tile index, holding an out-of-order
result until its predecessors have arrived. A tile the restriction skips counts
as arrived, so the fold never waits on it. How many results are held at once is
bounded by the pass's instance slots, not by how far completion runs ahead of
the slowest tile. `detail::InstanceSlots` admits at most *cap* live tile
instances of the pass, in flight or held by the fold: for an operation with
state *s* the cap is `min(2 × workers, (ceiling − tileBytes − Σ state) / s)`,
and `2 × workers` when *s* is 0. Validation (5.5) guarantees the cap is at
least 1. A worker takes a slot before it is handed a tile index, so a tile
that is waiting for a slot has read nothing, and the slot is released after the
fold combines the tile's instance and destroys it, or when the restriction
skips the tile. A worker waiting for a slot leaves when another tile fails (the
pass's stop flag) or on cancellation, as `CancelledError`. A held per-tile
instance lives outside the per-tile memory reservation, which is released when
its tile finishes; the run's reservation (5.9) counts the instances instead. A
`combine` that throws fails the
pass; it runs on whichever arrival unblocks the fold, so the `TileError` names
that arriving tile, not necessarily the tile whose result failed to combine. The fold is used for every
global-context operation, always, because an operation does not declare whether
its arithmetic is order-independent, and for any `Reduction` whose
`isOrderIndependent()` is false. An order-independent `Reduction` may combine in
completion order.

**Global-context passes.** With global-context operations at chain positions
g₁ < g₂ < …, a run makes one accumulation pass per gᵢ, then the final pass.
Pass *i* evaluates operations 1…gᵢ−1, which are already finalised, and calls
`accumulate` on a fresh instance of operation gᵢ per tile
(`OperationCatalog::create(name, parameters)`). The per-tile instances fold into
the run's instance in canonical order, then `finalizeAccumulation()` is called.
The final pass applies the whole chain with the finalised instances, and
`tilesTotal` counts every pass (passes × `tileCount`). Each run creates fresh
global-context instances, so repeated runs never accumulate into each other. A
failure while creating them, or in `finalizeAccumulation`, is a `TileError` with
no tile. A failure or a cancellation in an accumulation pass ends the run before
the final pass, with no sink written or flushed.

That distinction is not pedantry. Float addition is not associative, so a
float reduction combined in completion order produces results that differ
between runs on the same machine with the same input — and UR-QA-3 would fail
intermittently and irreproducibly across worker counts. Placing the rule in the
framework rather than in each algorithm is what makes it enforceable.

UR-PIX-5's acceptance criterion depends on this: a streamed whole-slide Otsu
must equal the value computed from a full in-memory histogram of the same
slide.

`Reduction`'s copy members are protected, so a derived class can be copied
while a `Reduction&` cannot be sliced.

**`Histogram64`, exactly.** The constructor requires `binCount >= 1`, finite
bounds with `minValue < maxValue`, and a span `maxValue − minValue` that is
itself finite, else `InvalidArgumentError`. `accumulate` reads every plane of
the view, each sample as a `double` by its plane's `DataType` — `Float16`
included, through the engine's own half-to-double conversion, since `core`
has none public — and bins `v` into
`floor((v − min) / (max − min) × binCount)`; `v == max` goes in the last bin,
and `v` outside `[min, max]` or NaN is ignored and not counted. A null view or a
plane of an unsupported type throws `InvalidArgumentError`. `tileIndex` is
unused. The bins are atomic, updated with relaxed ordering, so `accumulate` is
ThreadSafe and its result depends on neither thread count nor order; readers
need the accumulation to have finished. `combine` requires another
`Histogram64` with the same three parameters, else `InvalidArgumentError`, and
adds the bins. `bin(i)` out of range throws `InvalidArgumentError`, and
`totalCount()` is the sum of the bins. Copies copy the bin values. A moved-from
histogram may only be destroyed or assigned to; every other call on it throws
`InvalidArgumentError`.

## 5.9 Worker pool and execution — `pipeline/executor.hpp`

```cpp
class WorkerPool {
public:
    WorkerPool(int32_t workerCount, MemoryBudgetPtr);
    ~WorkerPool();

    int32_t workerCount() const noexcept;
    void    waitIdle();
    void    shutdown();
};

class TileExecutor {
public:
    TileExecutor(const TileGrid&, WorkerPool&, const Configuration&);

    void run(Pipeline&, ObjectSink&, ProgressObserver*,
             const CancellationToken&);

    int64_t tilesCompleted() const noexcept;
    int64_t tilesTotal() const noexcept;
};
```

**Thread-safety:** ThreadSafe. **Requirements:** UR-EXE-2, UR-EXE-3,
UR-EXE-4, UR-EXE-5.

Worker count defaults to hardware concurrency, with 1 available by
configuration for single-threaded operation (UR-EXE-3). Throughput on the
reference slide must improve materially from 1 to 4 workers while results stay
identical.

A tile-level failure surfaces a `TileError` naming the tile and the stage, and
does not corrupt the workspace or silently truncate results (UR-EXE-5). The
stage is recorded failed, not complete.

Tiles are dispatched and completed in any order. Nothing downstream depends on
that order, because identifiers come from geometry and reductions either combine
order-independently or are canonically ordered.

**Workers.** `WorkerPool(workerCount, budget)` requires `workerCount >= 1`, else
`InvalidArgumentError`. Its threads start in the constructor and stop in
`shutdown()`, which is idempotent and is called by the destructor.
`TileExecutor::run` hands out tile indices from an atomic counter in ascending
order, so dispatch is ordered and completion is free.

**Reservations of a global-context run.** Beside each tile's own reservation
(5.5), a run with global-context operations takes one run-level reservation of
`Σ state + max over passes (cap × state)`, where *state* is the operation's
`accumulatedStateBytes()` and *cap* is the pass's bound on live tile instances
(5.8). It makes one `reserveOrWait` before the first pass and holds the bytes
until `run()` returns, on every path; no pass reserves anything of its own.
Concurrent runs with global-context operations whose executors share one
budget, through one pool, whose ceiling is below the sum of their run
reservations plus one tile may wait on each other until cancelled.

**Failure.** The first tile failure stops dispatch, lets in-flight tiles finish,
and is rethrown as `TileError` with an `ErrorContext` of the stage name, the
operation name when an operation threw, the original `what()` as the cause, and
the tile's column and row. A `ProcessorError` thrown by an operation is
translated the same way; only `CancelledError` keeps its own type and is
rethrown as is. If a failure cannot even be recorded, the run ends with
`InternalError` rather than reporting success. Sinks are never flushed on the
failure path, which is what lets the workspace record the stage failed (`04-workspace.md`
4.4). On success `runInto` flushes the object, edge, annotation and raster sinks
in that order.

**Cancellation** is checked before each tile and between operations, and
surfaces as `CancelledError`.

**Sinks and the pipeline during a run.** The object sink and the pipeline's
edge, annotation and raster sinks are called concurrently from the workers, so
each must be ThreadSafe; `TileExecutor::run` flushes none of them. `run`
updates the pipeline's per-run state — its validation result, its
global-context instances and its counters — so no other thread may use that
pipeline, through the same executor or another, while `run` is in progress.

**Progress.** `onProgress(stageName, done / total, done, total)` is called after
each tile, with `total` equal to `tileCount()` (5.6) for a single pass and
`done` counting skipped tiles. Observer calls are serialised.

The executor calls `SeamMerger::notifyTileComplete` as each tile of the final
pass that was evaluated finishes, and `notifyTileSkipped` for a tile the
restriction skipped, which is what lets seam-crossing objects be emitted during
the pass instead of piling up until the end (5.7). After the pass it evaluates
the deferred tiles an open object expects, completes the rest with
`completeDeferred` (5.5), and calls `resolve`, on success only: after a
cancellation or a tile failure the merger is destroyed with whatever it still
holds. When the run is restricted, the tile masks the restriction filter caches
to test merged anchors are released as tiles settle — a tile's mask once it and
its up to eight neighbours have completed — so the cache does not grow to one
byte per level pixel; a released mask is rebuilt if asked for again, so
releasing never changes an answer. The cache is not bounded by the tiles in
progress: a tile settles only when the tile below and to its right completes,
about one grid row later, so it holds about one row of tile masks (columns ×
tile width × tile height bytes at the analysis level). Each cached mask reserves
its bytes from the run's budget with `tryReserve`, never waiting, and gives them
back when it is dropped; a mask that does not fit is not cached, and the anchor
is tested on a one-pixel mask built through the same sources instead, with the
same answer. Completion order therefore changes *when* a merged
object is emitted but never *what* it is: a component closes only once every
tile bordering it is done, and its intensity partials are combined in canonical
fragment order regardless of the order they arrived.

## 5.10 Backend adapter — `pipeline/backend.hpp`

```cpp
class Backend {
public:
    static const Backend& instance() noexcept;

    const char* name() const noexcept;
    const char* version() const noexcept;

    bool        supports(DataType type) const noexcept;
    DataTypeSet supportedTypes() const noexcept;

    /// Throws UnsupportedDataTypeError naming the type and the backend.
    int32_t toBackendDepth(DataType type) const;
    DataType fromBackendDepth(int32_t depth) const noexcept;

    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;
};
```

**Thread-safety:** Immutable. **Requirements:** UR-BLD-3, UR-BLD-4,
UR-IMG-11, UR-IMG-12.

The single place the OpenCV 4 to 5 migration touches. No OpenCV type appears in
any public header — `cv::Mat` is confined to the implementation, and
`BlockView` is what crosses the boundary.

Nothing here or anywhere else in the public API names the OpenCV 4 type set as
a fixed limitation. UR-BLD-4's acceptance criterion is a review confirming
exactly that, and the generated support matrix of UR-IMG-12 derives from the
backend in use, so migration widens declarations without changing a
requirement, an API, or a stored artifact.

`name()` is `"OpenCV"`. `version()` is OpenCV's own version string, read once
and cached for the life of the process. The depth mapping is:

| `DataType` | OpenCV depth | Available |
|---|---|---|
| `Byte` | `CV_8U` | always |
| `Int8` | `CV_8S` | always |
| `UInt16` | `CV_16U` | always |
| `Int16` | `CV_16S` | always |
| `Int32` | `CV_32S` | always |
| `Float16` | `CV_16F` | always |
| `Float32` | `CV_32F` | always |
| `Float64` | `CV_64F` | always |
| `UInt32` | `CV_32U` | OpenCV 5 and later |
| `UInt64` | `CV_64U` | OpenCV 5 and later |
| `Int64` | `CV_64S` | OpenCV 5 and later |

The last three rows are compiled in only when OpenCV's major version is 5 or
more, so on the pinned OpenCV 4 they do not exist and the types are not
supported. `supportedTypes()` is exactly the mapped set, and it **equals
`BackendCapabilities::instance().supportedTypes()`**, which `config` derives
without linking OpenCV (`02-config.md`); a test asserts the two agree, so the
data library and the engine can never report different type sets.
`toBackendDepth` of a type outside the set, `Unknown` included, throws
`UnsupportedDataTypeError` whose message names the type and `"OpenCV"`.
`fromBackendDepth` of a depth the table does not hold returns
`DataType::Unknown`.

## 5.11 The engine library — `slideio-processor-engine`

`pipeline` and `algorithm` are the modules of the second shared library of
design 5.1, `slideio-processor-engine`; `workspace` joins it later. It is
assembled from object libraries, as `slideio-processor-data` is:
`src/pipeline/` builds the OBJECT library `slideio-processor-pipeline`,
`src/algorithm/` builds `slideio-processor-algorithm`, and the shared library
links both. Linking the algorithm objects into the library, rather than
archiving them, keeps the file-scope object that registers the built-in
operations at load (`06-algorithm.md` 6.1).

`algorithm` is built as a third-party operation would be: its include path is
`include/`, SlideIO's headers and its own `src/algorithm/` as the root of its
`detail/` headers, and not `src/`, so it cannot reach `pipeline`'s private
headers (UR-EXT-2). It defines `SLIDEIO_PROCESSOR_ENGINE_API` privately, as
`pipeline` does, because its objects are compiled into the engine.

**Requirements:** UR-API-1, UR-BLD-3, UR-BLD-4, UR-EXT-2.

**Exports.** `pipeline/export.hpp` defines `SLIDEIO_PROCESSOR_ENGINE_EXPORTS`,
which every exported class, free function and interface of the library
carries. On Windows it is `__declspec(dllexport)` while the library is built,
which the build signals by defining `SLIDEIO_PROCESSOR_ENGINE_API` privately on
the library and its object libraries, and `__declspec(dllimport)` for
consumers; elsewhere it is `__attribute__((visibility("default")))`.

**Links.** The engine links `slideio-processor-data` and `slideio::slideio`
**publicly**, because its public headers name the data library's types and
SlideIO's `Scene`, and links `opencv_core` and `opencv_imgproc` **privately**,
from the static Conan build; so does the `algorithm` object library. OpenCV
is confined to the engine library rather than to `pipeline`, because the
catalog's neighbourhood kernels, labelling and watershed (`06-algorithm.md`
6.4, 6.7) need it. SlideIO's own libraries carry a second, static
copy of OpenCV. The two copies never meet, because SlideIO's public API is
buffer-based — a `void*` and a size — so no `cv::` object ever crosses it. On
Linux the engine links with `-Wl,--exclude-libs,ALL`, as the data library
does, so its static OpenCV neither interposes on SlideIO's copy nor is
interposed by it. The data library's configure-time guard, which fails the
build if `config`, `storage` or the data library ever lists OpenCV or a
`slideio::` target, is unchanged and still holds.

**No OpenCV in a public header, checked by the build.** The target
`pipeline_headers_are_opencv_free` compiles every header under
`include/slideio/processor/pipeline/` and `include/slideio/processor/algorithm/`
on its own, with the SlideIO and data
include paths but **no OpenCV include path**. It is part of the default build,
so a header that reaches OpenCV fails the build rather than a review. A second
object library compiles a third-party operation against `include/` and
SlideIO's headers alone, with no `src/` and nothing linked, which is the
standing proof of UR-EXT-2.

**Umbrella header.** `pipeline/pipeline_all.hpp` includes every public header of
the module; the name `pipeline.hpp` belongs to the `Pipeline` class, so the
umbrella cannot use the module name as `core.hpp`, `config.hpp` and
`storage.hpp` do. It is compiled by the same OpenCV-free header check, and a
test that includes nothing else touches one name from each header (UR-API-2).

**Tests that load SlideIO.** The engine's tests load SlideIO's shared
libraries, which live in SlideIO's install tree rather than beside the test.
On Windows each test has that directory prepended to `PATH` through
`ENVIRONMENT_MODIFICATION`, the directory taken from the imported
`slideio::slideio` target's `IMPORTED_LOCATION_RELEASE`, falling back to
`IMPORTED_LOCATION`. Tests are discovered with `DISCOVERY_MODE PRE_TEST`, so the
build never runs the executable; but discovery at test time runs outside
`ENVIRONMENT_MODIFICATION`, so the same prepend also goes through the target's
`TEST_LAUNCHER` (`cmake -E env --modify PATH=path_list_prepend:<dir> --`),
which wraps discovery and every run alike. `TEST_LAUNCHER` needs CMake 3.29 or
later on Windows. On Linux and macOS the imported library's RPATH suffices.
