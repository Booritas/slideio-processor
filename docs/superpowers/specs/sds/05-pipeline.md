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

struct Halo {                              // POD
    int32_t left, top, right, bottom;
    static Halo uniform(int32_t n) noexcept;
    static Halo none() noexcept;
    bool isEmpty() const noexcept;
};

class OperationContext {
public:
    const BlockView& input() const noexcept;
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

    /// The same tile in level-0 coordinates, for an operation that needs to
    /// relate itself to slide position.
    Rect64 tileRect() const noexcept;
    Rect64 haloRect() const noexcept;

    /// Level to level-0, through the library's single mapping (5.6). Emitted
    /// geometry must be in level-0 coordinates (UR-SEG-4); this is how an
    /// operation working on level pixels gets there.
    double  levelScale() const noexcept;
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

    /// Records a fragment touching a tile seam, for cross-tile merging.
    /// Level-0 coordinates, like every emitted polygon.
    void emitSeamFragment(const Polygon&, LayerId);
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

    virtual void apply(OperationContext&) = 0;

    // Global-context operations only; default implementations do nothing.
    virtual bool needsGlobalContext() const;
    virtual void accumulate(const OperationContext&);
    virtual void combine(const Operation& other);
    virtual void finalizeAccumulation();
};
using OperationPtr = std::shared_ptr<Operation>;
```

**Thread-safety:** `Operation` implementations must be reentrant — `apply` is
called concurrently on different tiles. `OperationContext` is ThreadConfined,
one per tile. **Requirements:** UR-EXT-1 to UR-EXT-4, UR-ALG-3, UR-ALG-4,
UR-IMG-11.

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
finished object.

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

The global-context group serves two-pass operations such as whole-slide Otsu:
`accumulate` runs over every tile, `combine` merges per-worker accumulators,
`finalizeAccumulation` computes the result, then `apply` runs with it
(UR-PIX-5, UR-EXE-1).

## 5.2 Operation parameters — `pipeline/operation_parameters.hpp`

```cpp
class OperationParameters {
public:
    OperationParameters();
    OperationParameters(const OperationParameters&);
    ~OperationParameters();

    OperationParameters& setInt(const char* key, int64_t);
    OperationParameters& setDouble(const char* key, double);
    OperationParameters& setBool(const char* key, bool);
    OperationParameters& setString(const char* key, const char*);
    OperationParameters& setDoubleArray(const char* key,
                                        const double* values, int32_t count);

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

## 5.3 Operation catalog — `pipeline/operation_catalog.hpp`

```cpp
class OperationFactory {
public:
    virtual ~OperationFactory();
    virtual OperationPtr create(const OperationParameters&) const = 0;
    virtual const char*  name() const = 0;
    virtual ChannelArity arity() const = 0;
    virtual bool         requiresUniformTypes() const = 0;
    virtual DataTypeSet  supportedTypes() const = 0;
    virtual Halo         halo() const = 0;
    virtual const char*  correctnessReference() const = 0;
    virtual const char*  complexityNote() const = 0;
};
using OperationFactoryPtr = std::shared_ptr<OperationFactory>;

class OperationCatalog {
public:
    static OperationCatalog& instance() noexcept;

    void registerFactory(OperationFactoryPtr);
    bool isRegistered(const char* name) const noexcept;

    OperationPtr create(const char* name, const OperationParameters&) const;

    int32_t     count() const noexcept;
    const char* nameAt(int32_t index) const;

    ChannelArity arity(const char* name) const;
    DataTypeSet  supportedTypes(const char* name) const;
    Halo         halo(const char* name) const;
    const char*  correctnessReference(const char* name) const;
    const char*  complexityNote(const char* name) const;

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

## 5.4 Pipeline specification — `pipeline/pipeline_spec.hpp`

```cpp
class PipelineSpec {
public:
    PipelineSpec();
    PipelineSpec(const PipelineSpec&);
    PipelineSpec& operator=(const PipelineSpec&);
    ~PipelineSpec();

    PipelineSpec& channels(const ChannelSelection&);
    PipelineSpec& restrictTo(const Rect64&);
    PipelineSpec& restrictToPolygon(const Polygon&);

    /// The pyramid level to read from; -1 (the default) lets `LevelSelector`
    /// choose the coarsest adequate one (UR-IMG-4). A property of the run,
    /// not of any one operation — the tile grid is laid out at this level
    /// (5.6), so a chain cannot mix levels.
    PipelineSpec& analysisLevel(int32_t);

    /// Appends an operation by catalog name. Nothing is resolved here; an
    /// unknown name fails at Pipeline::validate(), not at append.
    PipelineSpec& operation(const char* name, const OperationParameters&);

    /// Appends an already-constructed operation, lowered to its name and
    /// parameters. The typed factories of `algorithm` return `OperationPtr`,
    /// and this is how a C++ caller who used them builds a spec.
    PipelineSpec& then(const OperationPtr&);

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

    bool equals(const PipelineSpec&) const noexcept;
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

## 5.5 Pipeline — `pipeline/pipeline.hpp`

```cpp
class Pipeline {
public:
    Pipeline(slideio::ScenePtr, const Configuration&, const PipelineSpec&);
    ~Pipeline();

    Pipeline& memoryBudget(MemoryBudgetPtr);

    /// Where operations assert containment. Borrowed, may be null, must
    /// outlive the run. `StageRunner` passes the stage's `EdgeWriter`.
    Pipeline& edgeSink(EdgeSink*);

    /// Where operations attach labels and measurements to objects that
    /// already exist. Borrowed, may be null, must outlive the run.
    /// `StageRunner` passes the stage's `AnnotationWriter`.
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
    slideio::ScenePtr asScene() const;

    /// Eager: streams the whole slide or ROI.
    void runInto(ObjectSink&, ProgressObserver*, const CancellationToken&);
    void run(ProgressObserver*, const CancellationToken&);

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

A `Pipeline` is a spec bound to a scene, a configuration and a budget. It has
no configuration surface of its own beyond the six runtime bindings above:
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
| Grid construction | The grid spans the restriction's bounding box, not the scene (5.6) | Whole rows and columns of tiles never exist |
| Tile dispatch | A worker builds the tile's mask first and skips the tile when it is empty, reading no pixels | UR-IMG-5: "reads only tiles intersecting it" |
| Object emission | Each object is kept only if the mask is set at its **anchor** | UR-STG-3: "produces no objects outside those regions" |

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
object is in when the mask is set at its topmost-then-leftmost vertex — the
same point `ObjectId::fromAnchor` derives identity from (`01-core.md` 1.4,
decision 8). Centroid would need a computation the record already carries but
which no longer matches identity; intersection would admit objects mostly
outside; full containment would drop objects the restriction was meant to
include. Anchor is one `RasterMask::testLevel0` call, it cannot split an
object, and it gives the same answer in every worker regardless of which tile
produced the object, so UR-QA-3's worker-count invariance survives unchanged.

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

`validate()` raises `UnsupportedDataTypeError` or `ChannelArityError` before
any pixel is read, so a mis-specified stage fails in seconds rather than hours.
It also resolves every object-producing operation's `targetLayer` parameter
against `bindLayer`, raising `InvalidArgumentError` naming the operation and the
layer when a target is unnamed, unbound, or — where the run bound several
layers — omitted. `OperationContext::targetLayer()` (5.1) returns what it
resolved, so an operation author never resolves a layer name and the same
operation object behaves identically whether a stage or a direct caller ran it.

## 5.6 Tiling and level selection — `pipeline/tile_grid.hpp`

```cpp
class TileGrid {
public:
    /// The grid is laid out in LEVEL coordinates. `regionAtLevel` and the
    /// tile size are level-`level` pixels, so every tile boundary falls on an
    /// exact level pixel edge.
    TileGrid(const Rect64& regionAtLevel, int32_t level,
             int32_t tileWidth, int32_t tileHeight,
             const LevelSelector&);

    int32_t level() const noexcept;

    int64_t tileCount() const noexcept;
    int64_t columns() const noexcept;
    int64_t rows() const noexcept;

    /// Level coordinates: what an operation indexes, and what `BlockView`
    /// covers. Halo is in level pixels too.
    Rect64 tileRectAtLevel(int64_t index) const;
    Rect64 tileRectAtLevelWithHalo(int64_t index, const Halo&) const;

    /// Level-0 coordinates, derived from the level rect through the same
    /// mapping for every tile. Adjacent tiles therefore share an exact
    /// level-0 boundary with no gap and no overlap.
    Rect64 tileRect(int64_t index) const;
    Rect64 tileRectWithHalo(int64_t index, const Halo&) const;

    int64_t tileIndex(int64_t column, int64_t row) const;
    bool    isEdgeTile(int64_t index) const;
};

class LevelSelector {
public:
    explicit LevelSelector(slideio::ScenePtr);

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
    Polygon mapToLevel0(const Polygon&, int32_t level) const;
};

/// The tile size, in level pixels: the slide's native tiling for that level
/// when available and tileSizeFromSlide is set, otherwise the configured size.
void defaultTileSize(slideio::ScenePtr, int32_t level, const Configuration&,
                     int32_t& widthOut, int32_t& heightOut);
```

**Thread-safety:** Immutable. **Requirements:** UR-IMG-4, UR-EXE-6, UR-EXT-3,
**UR-SEG-3**, UR-QA-2, UR-QA-3, UR-IMG-5.

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

**What region the grid spans.** `Pipeline` intersects, in level-0 coordinates,
the scene rect with whichever of the spec's restrictions are present — the ROI
rectangle, the polygon's `boundingBox()` — and maps the result down with
`mapToLevel`. A bound `TileMaskSource` does **not** narrow the grid: it is
resolved per tile and has no bounding box to offer without the whole-mask
computation that `04-workspace.md` 4.4 exists to avoid. Restricting to a layer
therefore lays out a grid over the whole scene and skips empty tiles at
dispatch, which costs one spatial query per non-overlapping tile and no pixel
reads.

`tilesTotal` counts the grid, not the tiles that did work, so a heavily
restricted run reports fast progress through its empty region rather than
stalling at an unexplained count. `tilesCompleted` reaching `tilesTotal`
continues to mean the run is done.

Halo is declared, supplied and applied in **level** pixels. An operation's
neighbourhood is a property of its kernel, which works on the pixels it is
given — a 5-pixel radius means five of the pixels being processed, at any
level, and the framework never rescales it.

## 5.7 Seam merging — `pipeline/seam_merger.hpp`

```cpp
/// Partial intensity statistics for one fragment, carried because the pixels
/// are gone by the time the fragment's component closes. Integer count,
/// float sums; combined in canonical order (5.8).
struct IntensityPartial {                  // POD, exempt from pimpl per C-2
    int64_t count;
    double  sum, sumOfSquares, minimum, maximum;
    int32_t channel;
};

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
    void addFragment(int64_t tileIndex, const Polygon&, LayerId,
                     const IntensityPartial* partials, int32_t partialCount);

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

### Holding fragments, and why most of them are not held for long

An earlier draft said only that "fragments are spilled rather than allowed to
grow unbounded," which named a mechanism without specifying one. The bound it
implied was also far worse than it needed to be, because it assumed every
fragment is held until the entire tile pass ends.

**A component closes as soon as every tile bordering it has completed.** It
does not need the pass to finish — it needs its own neighbourhood to finish.
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

When `MemoryBudget::tryReserve` fails, the merger writes the fragments of its
**oldest-opened components** to `scratchDirectory` and keeps their union-find
entries and bounding boxes resident — those are small and fixed-size, so
merging decisions never require a read. Fragments come back only when their
component closes. Choosing oldest-first is deliberate: a long-open component is
by definition one whose tiles are not finishing soon, so it is the cheapest to
evict and the least likely to be wanted again this minute.

Spill files belong to the merger and to one run. They are removed on
destruction, including on the failure path, so a stage that throws leaves
nothing behind. `StageRunner` passes a subdirectory of the workspace, which
keeps UR-STG-9's "copy or archive as one unit" true and makes orphan cleanup a
matter of clearing one known directory when a workspace opens after a crash. A
`Pipeline` used directly takes the path from `Configuration`. Failures writing
or reading spill files surface as `IoError`, never as a silently dropped
fragment.

### What is not bounded by the budget

One merged object must materialise whole to be emitted. Peak memory is
therefore the working frontier plus the **largest single merged polygon**, and
no amount of spilling reduces the second term — a tissue contour at level 0 can
run to millions of points. `largestComponentFragmentCount` is exposed so a run
can report what it actually hit. In practice this is tens of megabytes and
comfortably inside a 2 GB ceiling; it is documented because it is the one part
of this component that does not scale down with the ceiling, and a caller
setting a very small budget should know which term will defeat them.

### Measurements across a seam

Geometric measurements — area, perimeter, centroid, bounding box — are
recomputed from the merged polygon at close, so they are exactly what
single-tile processing would produce and nothing needs carrying.

Intensity statistics cannot be: the pixels are gone once a tile is done. Each
fragment therefore carries `IntensityPartial` per channel, and the merger
combines them. `count` is integer and combines order-independently; `sum` and
`sumOfSquares` are floating point and **must be combined in canonical fragment
order** — by anchor, not by the order components happened to close — for
exactly the reason 5.8 gives for float reductions. Combining them in completion
order would make intensity statistics differ between runs at different worker
counts, and UR-QA-3 would fail intermittently and irreproducibly, which is the
hardest kind of failure to attribute to its cause.

## 5.8 Reductions — `pipeline/reduction.hpp`

```cpp
class Reduction {
public:
    virtual ~Reduction();
    virtual void accumulate(const BlockView&, int64_t tileIndex) = 0;
    virtual void combine(const Reduction& other) = 0;
    virtual bool isOrderIndependent() const = 0;
};

class Histogram64 : public Reduction {
public:
    Histogram64(int32_t binCount, double minValue, double maxValue);
    int64_t bin(int32_t index) const;
    int64_t totalCount() const;           // int64: UR-IMG-2
    bool    isOrderIndependent() const override;   // true
};
```

**Thread-safety:** ThreadSafe for `accumulate`; `combine` requires exclusive
access to the target. **Requirements:** UR-PIX-5, UR-QA-3, UR-IMG-2.

Integer accumulators — histogram bins, pixel counts, areas — combine
order-independently, so worker count cannot affect them, and
`isOrderIndependent()` returns `true`. Floating-point accumulators must return
`false`, and the framework then combines them in canonical tile order.

That distinction is not pedantry. Float addition is not associative, so a
float reduction combined in completion order produces results that differ
between runs on the same machine with the same input — and UR-QA-3 would fail
intermittently and irreproducibly across worker counts. Placing the rule in the
framework rather than in each algorithm is what makes it enforceable.

UR-PIX-5's acceptance criterion depends on this: a streamed whole-slide Otsu
must equal the value computed from a full in-memory histogram of the same
slide.

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

The executor calls `SeamMerger::notifyTileComplete` as each tile finishes, which
is what lets seam-crossing objects be emitted during the pass instead of piling
up until the end (5.7). Completion order therefore changes *when* a merged
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

    bool        supports(DataType) const noexcept;
    DataTypeSet supportedTypes() const noexcept;

    /// Throws UnsupportedDataTypeError naming the type and the backend.
    int32_t toBackendDepth(DataType) const;
    DataType fromBackendDepth(int32_t) const noexcept;
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
