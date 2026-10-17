# SDS 03 — `storage`

**Module:** `storage` · **Library:** `slideio-processor-data` · **Depends on:** `core`, `config`

The store file and everything durable in it: objects, edges, layers, labels,
stage state, provenance, and the event journal. Implements `core`'s
`ObjectSink`, `ObjectSource` and `Cursor`.

This is the module a viewer process links without OpenCV (design 5.1), and the
largest public surface in the library.

Conventions in [00-conventions.md](00-conventions.md).

---

## 3.1 Enumerations — `storage/types.hpp`

```cpp
enum class OpenMode : int32_t {
    ReadOnly,        // no writer; safe alongside another process's writer
    ReadWrite,       // exclusive writer; readers may attach concurrently
    CreateNew        // fails if the path exists
};

enum class LayerKind     : int32_t { Semantic, Primitive };
enum class LabelOrigin   : int32_t { Standard, WorkspaceLocal, ImportRegistered };
enum class ImportPolicy  : int32_t { Strict, Permissive };
enum class StageStatus   : int32_t { Pending, Running, Complete, Failed };

enum class EventKind : int32_t {
    LayerDeclared, ObjectsAdded, EdgesAsserted, ObjectsAnnotated,
    LabelAdded, StageStateChanged, StageDiscarded, LayerDropped,
    RasterDeclared, RasterWritten, RasterDropped
};
```

## 3.2 Store — `storage/store.hpp`

```cpp
class Store;
using StorePtr = std::shared_ptr<Store>;

class Store {
public:
    static StorePtr open(const char* path, OpenMode, const Configuration&);
    ~Store();

    const char* path() const noexcept;
    int32_t formatVersion() const noexcept;
    OpenMode mode() const noexcept;
    bool     isReadOnly() const noexcept;

    LayerRegistry&       layers() noexcept;
    const LayerRegistry& layers() const noexcept;
    VocabularyRegistry&  vocabulary() noexcept;
    ObjectStore&         objects() noexcept;
    EdgeStore&           edges() noexcept;
    AnnotationStore&     annotations() noexcept;
    RasterRegistry&      rasters() noexcept;
    EventJournal&        events() noexcept;
    StageStateReader&    stageStates() noexcept;

    Provenance provenance() const;
    void       setProvenance(const Provenance&);

    /// Flushes pending batches and releases the connection. Idempotent.
    /// Errors during teardown are reported here rather than in the destructor.
    void close();
};

int32_t storeFormatVersion() noexcept;
```

**Thread-safety:** ThreadSafe. All methods callable from any thread with no
external locking. Sub-object references remain valid until `close()`.
**Requirements:** UR-RES-12, UR-BLD-5, UR-OBS-1, decision 4.

`open` gates on the format version and throws `StoreFormatError` naming both
the store's version and the library's when they are incompatible, rather than
misreading the file (UR-RES-12).

Opened in WAL mode with one connection per accessing thread. That is what makes
UR-OBS-1 achievable: a `ReadOnly` store in a viewer process reads concurrently
with a `ReadWrite` store in a script process, neither blocking the other, each
reader seeing a consistent snapshot rather than a partially applied transaction.

`ReadWrite` takes an exclusive writer lock. A second `ReadWrite` open throws
`IoError` naming the holding process where the platform allows it — concurrent
writers remain out of scope (Assumption 6, §15).

## 3.3 Layer registry — `storage/layer_registry.hpp`

```cpp
class LayerRegistry {
public:
    /// Declares a semantic layer beneath `parent`. Pass pixelLayer() for a
    /// layer rooted directly at the pixel layer. Branching is permitted.
    LayerId declareSemantic(const char* name, LayerId parent);
    LayerId declarePrimitive(const char* name);

    LayerId pixelLayer() const noexcept;

    int32_t count() const noexcept;
    LayerId at(int32_t index) const;
    bool    find(const char* name, LayerId& out) const noexcept;

    const char* name(LayerId) const;
    LayerKind   kind(LayerId) const;
    bool        exists(LayerId) const noexcept;

    // Semantic hierarchy. Throw InvalidArgumentError for primitive layers.
    LayerId parent(LayerId) const;
    int32_t depth(LayerId) const;
    int32_t childCount(LayerId) const;
    LayerId childAt(LayerId, int32_t index) const;

    /// True if `descendant` lies strictly above `ancestor` in the hierarchy,
    /// at any distance. The predicate UR-RES-14 edges are validated against.
    bool isAbove(LayerId descendant, LayerId ancestor) const;

    bool isDropped(LayerId) const noexcept;

    /// Stages declared to write this layer, queryable before any stage runs.
    int32_t writerStageCount(LayerId) const;
    StageId writerStageAt(LayerId, int32_t index) const;

    /// The coarsest pyramid level any completed stage used to write this
    /// layer; 0 for a layer written only at full resolution, by import, or
    /// not yet written. Geometry in this layer carries level-0 coordinates
    /// whose precision is that of this level.
    int32_t coarsestAnalysisLevel(LayerId) const noexcept;

    /// Records the level a stage analysed at. Called by the framework when a
    /// writing stage completes; keeps the maximum over all writers.
    void recordAnalysisLevel(LayerId, int32_t level);
};
```

**Thread-safety:** ThreadSafe. **Requirements:** UR-STG-10, UR-RES-13,
UR-RES-14, UR-STG-12, UR-SEG-4, UR-IMG-4.

Semantic layers form a branching tree rooted at the pixel layer — `vessels` and
`glands` may both sit above `tissue` without relating to each other.
`declarePrimitive` takes a name alone: primitive layers are arbitrary, any
number, in no order, outside the hierarchy.

`declareSemantic` and `declarePrimitive` append a `LayerDeclared` event to the
journal in the same transaction, so a subscribed viewer learns of a new layer
as it is declared (UR-OBS-5).

**`coarsestAnalysisLevel` is how a consumer learns what a boundary is worth.**
UR-SEG-4 puts every polygon in level-0 coordinates, and UR-IMG-4 requires
reading from the coarsest level that satisfies the request — so a `tissue`
layer detected at level 4 stores level-0 integers whose true precision is ±16
level-0 pixels. The coordinates look exact and are not, and nothing else in the
store says so. The level was always *recoverable* — it sits in the operation's
parameter bag, inside the `PipelineSpec`, inside the stage's configuration JSON
— but a viewer asking "how sharp is this outline?" should not have to parse a
stage record to find out, and the mask dilation of `04-workspace.md` 4.4 needs
the number directly.

It is the **maximum** over all writers, not the last one: a layer filled by a
level-4 detection stage and by a hand-drawn import is only as precise as its
coarsest contributor, and a bound that is sometimes optimistic is worse than
no bound. Levels are recorded on stage completion, so the value is 0 until
something has actually run — which is correct, not a placeholder, since an
empty layer has no geometry to qualify.

This records precision; it does not improve it. A level-4 contour mapped to
level 0 is a staircase with 16-pixel treads, and no later stage can recover
what was never sampled.

Writing an object to an undeclared layer throws `InvalidArgumentError` naming
the layer, per UR-STG-10's acceptance criterion.

`isAbove` is the single place the containment ordering is interpreted. Putting
it here rather than in `workspace` is deliberate: the rule constrains edges the
store must reject no matter who writes them, including GeoJSON import and any
future external tool, so it lives with the data.

## 3.4 Vocabulary registry — `storage/vocabulary_registry.hpp`

```cpp
class VocabularyRegistry {
public:
    /// The standard vocabulary version this store was written against, which
    /// may be older or newer than the linked library's.
    int32_t recordedStandardVersion() const noexcept;

    /// Adds a workspace-local label. Legal at any time, including after
    /// stages have completed. Never requires a format migration.
    LabelId addLocal(const char* name);

    bool        find(const char* name, LabelId& out) const noexcept;
    const char* name(LabelId) const;
    LabelOrigin origin(LabelId) const;

    /// False for a label from a newer standard vocabulary than this library
    /// knows. Its identifier and string are still returned intact.
    bool isRecognised(LabelId) const noexcept;

    int32_t count() const noexcept;
    LabelId at(int32_t index) const;

    ImportPolicy importPolicy() const noexcept;
    void         setImportPolicy(ImportPolicy);
};
```

**Thread-safety:** ThreadSafe. **Requirements:** UR-RES-17, UR-RES-18,
UR-RES-19.

Labels are stored as integer identifiers, never as strings on the object row —
UR-RES-17 requires the per-object cost of a label not scale with the length of
the label string, and its acceptance test doubles every label string and
asserts store size is substantially unchanged.

`isRecognised` returning `false` is not an error. A store written against a
newer vocabulary reports its unknown labels as unrecognised with identifier and
string intact, rather than dropping or renaming them (UR-RES-17).

The contents of the standard vocabulary are in `02-config.md` 2.2 — ten
labels in v1, resolving open question 8. This registry holds the *effective*
vocabulary of one store: those ten, plus whatever `addLocal` and import have
added to it.

**`find` matches case-insensitively, and that is load-bearing rather than
convenient.** The standard vocabulary is spelled in British English, as the
requirements are — `tumour` — while QuPath emits `Tumor`. UR-RES-8's acceptance
criterion is a QuPath round trip, so a case- and spelling-sensitive lookup
would send an ordinary exported file down the `Strict` rejection path for a
reason that has nothing to do with the user's data. Matching folds case and
consults a small alias table shipped with the vocabulary; `name()` always
returns the canonical spelling, so nothing downstream sees the variant.

`addLocal` appends a `LabelAdded` event and does not touch any previously
written object, which is what lets a label added after the `nuclei` stage
completed be applied by a later stage with the existing nuclei unchanged
(UR-RES-18).

`ImportPolicy` governs GeoJSON import only (3.11). Both the policy in force and
its outcome are recoverable from the store, per UR-RES-19.

## 3.5 Object store and writer — `storage/object_store.hpp`

```cpp
class ObjectWriter;
using ObjectWriterPtr = std::shared_ptr<ObjectWriter>;

class ObjectWriter : public ObjectSink {
public:
    ~ObjectWriter() override;

    /// Writes the record with this writer's `stage()` as its provenance. The
    /// record's own `stage()` is not read and cannot override it; the layer,
    /// geometry, label and measurements come from the record.
    void emit(const ObjectRecord&) override;
    void flush() override;
    int64_t emittedCount() const override;

    int64_t batchSizeObjects() const noexcept;
    void    setBatchSizeObjects(int64_t);

    /// The stage every object emitted through this writer is attributed to.
    StageId stage() const noexcept;
};

class ObjectStore : public ObjectSource {
public:
    /// A writer that attributes everything it emits to `stage`. Any number may
    /// be held concurrently, from any threads; writes are serialised internally
    /// into batched transactions.
    ObjectWriterPtr writerForStage(StageId);

    CursorPtr query(const ObjectQuery&) override;
    int64_t   count(const ObjectQuery&) const override;
    bool      get(ObjectId, ObjectRecord& out) const override;

    /// Removes every object a stage wrote. Used by re-run and by failure
    /// handling; appends StageDiscarded.
    void discardStage(StageId);

    int64_t totalObjectCount() const;
    int64_t objectCountInLayer(LayerId) const;
};
```

**Thread-safety:** ThreadSafe. Returned `CursorPtr` is ThreadConfined.
**Requirements:** UR-RES-1, UR-RES-2, UR-RES-3, UR-EXE-7, UR-STG-6, decisions
4, 5, 6, 8.

**The write path is the whole point of this class.** One `emit` call inserts
nothing by itself. Objects accumulate until `batchSizeObjects` is reached, then
one transaction inserts the object rows, the R\*Tree rows, and a single
`ObjectsAdded` journal row — together, or not at all:

```
BEGIN
  INSERT INTO objects      ... (batch)
  INSERT INTO object_rtree ... (batch)
  INSERT INTO events       ... (one row: layer, stage, count, union bbox)
COMMIT
```

That atomicity is what makes the journal usable as a source of truth (UR-OBS-2):
a subscriber can never see an event for objects that are not yet queryable, and
objects can never appear unannounced. There is no separate publish step that a
crash can interrupt.

Writers do not need to be ordered, only serialised. Object identifiers come
from `ObjectRecord::id()`, which derives from the geometry's anchor — so a
worker computes its own identifiers and hands finished records to the writer
whenever it is ready. No reorder buffer, no scan-order frontier, no head-of-line
stalling, and identifiers identical across worker counts and tile sizes
(decision 8, UR-QA-3).

`emit` throws `InvalidArgumentError` if the record names an undeclared layer,
an unknown label, or carries empty geometry.

**Objects within one layer may not overlap at their anchor, and `emit`
enforces it.** An anchor is the topmost-then-leftmost pixel of a shape
(decision 8), so two objects in one layer sharing that pixel derive the same
`ObjectId` — which is not a near-miss but a collision of the identity the
entire re-run, provenance and export story rests on. `emit` therefore throws
`InvalidArgumentError` naming both the layer and the anchor rather than
silently replacing, dropping or duplicating.

This is stated because nothing previously stated it and a benchmark found it:
a spike run placing ten million anchors at random produced an 0.81%
primary-key collision rate. That *rate* is an artifact — random placement
manufactures coincidences a real segmentation does not — but the invariant it
exposed is real, and no requirement forbids overlapping objects. A caller that
genuinely needs overlapping shapes puts them in separate layers, which costs a
declaration and preserves every identity guarantee.

**The writer owns the stage; the record owns everything else.** `stage` is the
one column a producer cannot supply: `emit` takes it from `stage()` and ignores
whatever the record holds, which is why `ObjectRecord` has no `setStage`
(`01-core.md` 1.6). Attribution is therefore a property of the handle a caller
was given rather than a value it fills in, and three guarantees rest on that
being unforgeable rather than merely conventional:

- `discardStage` and `discardEdgesOfStage` delete by stage. A record able to
  misattribute itself would let one stage's re-run delete another's objects, or
  survive its own (UR-STG-6).
- `StageVisibility::CompletedOnly` filters by the producing stage's status. A
  wrong stamp makes an in-flight object visible to a later stage, breaking
  UR-STG-5 by the query filter that is supposed to enforce it.
- The provenance UR-STG-4 records would answer "which stage produced this
  object" with whatever the producer asserted.

`GeoJsonImporter` (3.11) takes its `StageId` the same way and for the same
reason — imported objects are attributed to the import, not to whatever a file
claims.

An object read from the store and emitted again is attributed to the emitting
stage, not the original one. That is the required behaviour, not a caveat: the
new row is this stage's output and must be discarded with it.

`query` honours the `StageVisibility` of the query: `CompletedOnly` — the
default — filters to objects of completed stages, so a later stage never sees
another's partial output and UR-STG-5 holds by query filter rather than by
rollback. `IncludeInFlight` is the observer's opt-in (UR-OBS-4).

No method returns all objects at once. UR-RES-6 permits materialisation only as
an explicit, filter-bounded call, and it is provided by `reporting`.

## 3.6 Edge store — `storage/edge_store.hpp`

```cpp
class EdgeWriter;
using EdgeWriterPtr = std::shared_ptr<EdgeWriter>;

class EdgeWriter : public EdgeSink {
public:
    ~EdgeWriter() override;

    /// Asserts that `child` is contained by `parent`. Both must lie in
    /// semantic layers, with parent strictly lower. Neither object is
    /// modified. Legal in either creation order.
    void assertParent(ObjectId child, ObjectId parent) override;

    /// Many-to-many, optional, valid when empty. One primitive may contribute
    /// to several objects and one object may be built from many primitives.
    void assertDerivedFrom(ObjectId derived, ObjectId primitive) override;

    void flush() override;
    int64_t assertedCount() const override;

    int64_t batchSizeEdges() const noexcept;
    void    setBatchSizeEdges(int64_t);

    /// The stage every edge asserted through this writer is attributed to.
    StageId stage() const noexcept;
};

class EdgeStore {
public:
    /// A writer that attributes every edge it asserts to `stage`. Any number
    /// may be held concurrently, from any threads; writes are serialised
    /// internally into batched transactions.
    EdgeWriterPtr writerForStage(StageId);

    bool      parentOf(ObjectId child, ObjectId& out) const;
    CursorPtr childrenOf(ObjectId parent) const;
    bool      isAttached(ObjectId child) const;
    CursorPtr originsOf(ObjectId derived) const;
    CursorPtr derivedFrom(ObjectId primitive) const;

    /// Removes only the edges a stage asserted. Objects are untouched,
    /// including objects of other stages that this stage had attached.
    void discardEdgesOfStage(StageId);

    int64_t parentEdgeCount() const;
    int64_t derivedFromEdgeCount() const;
};
```

**Thread-safety:** `EdgeStore` is ThreadSafe; `EdgeWriter` is ThreadSafe for
`assertParent` and `assertDerivedFrom`, and `flush` requires exclusive access.
**Requirements:** UR-RES-5, UR-RES-14, UR-RES-16, UR-STG-4, UR-STG-6.

Edges are first-class records of the form *(child, parent, asserting stage)*,
never a property of the object. That is what lets a `nuclei` stage run before
any cell exists, writing unattached nuclei, and a later `cells` stage assert the
nucleus-to-cell edges without rewriting the nuclei (UR-RES-5).

**The writer owns the asserting stage**, exactly as `ObjectWriter` owns the
producing stage (3.5). An earlier draft put `assertParent` and
`assertDerivedFrom` on `EdgeStore` with a trailing `StageId assertingStage`
argument, which left the edge path with the forgeable attribution the object
path had already given up: `discardEdgesOfStage` deletes by that field, so a
caller passing another stage's id could delete edges belonging to a stage it
does not own, or leave its own behind for a re-run to duplicate (UR-STG-6).
Attribution is now a property of the handle in both stores, and there is no
overload that takes a stage.

`EdgeWriter` batches the way `ObjectWriter` does — edges accumulate until
`batchSizeEdges`, then one transaction inserts the edge rows and a single
`EdgesAsserted` journal row, together or not at all. The default comes from
`Configuration::objectBatchSize()`; edges are small and arrive at a comparable
rate, so a separate configuration key would be a knob nobody could reason
about. A caller who has a reason can still set it per writer.

**Ordering against the object writer matters, and did not before.** An edge's
endpoints must be committed before the edge is, or a crash between the two
batches leaves an edge pointing at nothing. `flush` therefore verifies both
endpoints exist and throws `InvalidArgumentError` naming the missing one, and a
stage emitting objects and edges in the same pass flushes its `ObjectWriter`
first — `StageRunner` does this (`04-workspace.md` 4.4). This is the one cost of
batching edges rather than writing them individually, and it is worth paying:
the reference object count implies an edge count of the same order, and a
transaction each would dominate the run.

`assertParent` throws `InvalidArgumentError` naming both layers when the parent
is in the same or a higher layer, or when either endpoint is in a primitive
layer — a primitive layer has no position in the hierarchy (UR-RES-14). The
parent need not be in the immediately adjacent layer: attaching a nucleus
directly to a tissue region succeeds when the `cells` layer is skipped.

**Containment is asserted, never derived.** The only checks are the layer-order
checks above: no component performs a point-in-polygon test to validate, infer,
or reject a parent edge, and none may be added. The hierarchy is what a stage
asserted, and the geometry is evidence for it rather than the definition of it.

This is not a shortcut, it is the only rule that survives multi-level analysis.
A `tissue` layer detected at level 4 has a boundary good to ±16 level-0 pixels
(3.3), while nuclei segmented at level 0 have boundaries good to one. Near the
margin the two disagree by construction, and a nucleus genuinely inside the
tissue can sit outside the mapped tissue polygon. Deriving containment
geometrically would make membership depend on which level each layer happened
to be analysed at — and would silently reclassify objects when a stage is
re-run at a different level, while their edges said otherwise.

A consumer is free to run its own point-in-polygon test and is warned that it
may legitimately disagree with the hierarchy; `coarsestAnalysisLevel` on both
layers is what tells it by how much. Where the two differ, **the edge is
authoritative** — for queries, for `childrenOf`, for export, and for every
count the library reports.

**A processing restriction is not an exception to this.** A stage restricted to
a layer is masked by that layer's geometry (`05-pipeline.md` 5.5), which is a
geometric test and looks at first like the thing this rule forbids. It is a
different question: the mask decides *what gets computed*, and leaves the
hierarchy to whatever the stage then asserts. An object the mask admitted has
no parent edge by virtue of having been admitted, and an object inside a
region it was never restricted to is not thereby outside anything. The rule
above governs edges; nothing governs edges but assertion.

An object with no parent edge is **unattached**, which is a valid state at any
layer, not an inconsistency, and `ObjectQuery::attachment(UnattachedOnly)`
queries for it.

`derivedFrom` is deliberately separate from parent edges and survives its
primitive layer being dropped: after the drop, the derived objects remain
complete and queryable and report no origins (UR-RES-16, UR-STG-12).

Layer validation happens at `assertParent`, not at `flush`, so a
hierarchy violation is reported at the call that made it rather than hundreds
of edges later. Endpoint existence is the opposite case and is necessarily
deferred: the object it names may still be sitting in an unflushed object
batch, so checking it eagerly would reject edges that are perfectly valid by
the time they commit.

## 3.7 Annotation store — `storage/annotation_store.hpp`

```cpp
class AnnotationWriter;
using AnnotationWriterPtr = std::shared_ptr<AnnotationWriter>;

class AnnotationWriter : public AnnotationSink {
public:
    ~AnnotationWriter() override;

    /// Attaches `label` to an object that already exists, attributed to this
    /// writer's `stage()`. The object row is not touched: the label it
    /// carries, if any, remains the producing stage's own assertion
    /// (`01-core.md` 1.6). Idempotent — the same stage asserting the same
    /// label on the same object twice is one row, not two.
    void assertLabel(ObjectId, LabelId) override;

    /// Attaches a named measurement, attributed to this writer's `stage()`.
    /// Idempotent for an identical value; asserting the same name twice with
    /// different values throws `InvalidArgumentError` rather than letting
    /// arrival order decide (UR-QA-3).
    void assertMeasurement(ObjectId, const char* name, double value,
                           MeasurementUnit) override;

    void flush() override;
    int64_t assertedCount() const override;

    int64_t batchSizeAnnotations() const noexcept;
    void    setBatchSizeAnnotations(int64_t);

    /// The stage every annotation asserted through this writer is attributed to.
    StageId stage() const noexcept;
};

class AnnotationStore {
public:
    /// A writer that attributes every annotation it asserts to `stage`. Any
    /// number may be held concurrently, from any threads; writes are
    /// serialised internally into batched transactions.
    AnnotationWriterPtr writerForStage(StageId);

    /// The object's **effective label set**: the producing stage's label from
    /// the object row first, when it has one, then every asserted label in
    /// ascending `LabelId` order. The order is fixed rather than insertion
    /// order because annotations arrive from workers in no particular
    /// sequence and exports must be byte-identical across runs (UR-QA-3).
    int32_t labelCount(ObjectId) const;
    LabelId labelAt(ObjectId, int32_t index) const;

    /// Who asserted the label at `index` — the producing stage for the row
    /// label, the annotating stage for the rest.
    StageId labelAssertedBy(ObjectId, int32_t index) const;

    /// Measurements asserted by stages other than the producer. The
    /// producer's own stay on the record (`01-core.md` 1.6); these are the
    /// additions, in ascending name order.
    int32_t     assertedMeasurementCount(ObjectId) const;
    Measurement assertedMeasurementAt(ObjectId, int32_t index) const;
    StageId     measurementAssertedBy(ObjectId, int32_t index) const;

    /// Resolves the one thing the effective sets leave ambiguous: two stages
    /// asserting the same measurement name on one object. False when that
    /// stage asserted no measurement of that name.
    bool assertedMeasurement(ObjectId, const char* name, StageId,
                             Measurement& out) const;

    /// Removes only the annotations a stage asserted. Objects are untouched,
    /// including the objects of other stages that this stage had annotated.
    void discardAnnotationsOfStage(StageId);

    int64_t labelAnnotationCount() const;
    int64_t measurementAnnotationCount() const;
};
```

**Thread-safety:** `AnnotationStore` is ThreadSafe; `AnnotationWriter` is
ThreadSafe for `assertLabel` and `assertMeasurement`, and `flush` requires
exclusive access — exactly `EdgeWriter`'s split, for exactly its reason.
**Requirements:** UR-RES-4, UR-RES-17, UR-RES-18, UR-STG-4, UR-STG-6, UR-QA-3.

**An annotation is an assertion about an object, not a part of it.** This is
the same move 3.6 makes for containment, applied to classification: a record of
the form *(object, asserting stage, claim)*, stored separately, deleted by the
stage that made it. It exists because the design had no way to express the
commonest workflow in the domain — detect cells in one stage, decide what they
are in another. An object row has one `stage` column, which conflates *who
found this shape* with *who said something about it*; as long as a label lived
on that row, the second stage had nowhere to put its answer.

**The writer owns the asserting stage**, as `ObjectWriter` and `EdgeWriter` do
(3.5, 3.6). There is no overload taking a `StageId`, and for the same reason:
`discardAnnotationsOfStage` deletes by that field, so a forgeable attribution
would let one stage delete another's work or leave its own behind for a re-run
to duplicate (UR-STG-6).

**The object row keeps its label, and that is not a half-measure.** A stage
that detects and classifies in one pass — most of them — should not write two
rows to say one thing, and the label it sets is already correctly attributed,
because the producing stage *is* the asserting stage. The row label is
therefore an annotation stored inline for the case where the two stages
coincide. Nothing competes: the effective label set is the row label, when
present, together with the asserted ones, and no precedence rule is needed
because none of them overrides another. What changed is the cardinality a
*consumer* sees — `hasLabel()` on a record still answers for the producer
alone, and the effective set is what queries and exports use.

**Two deletes, and they are deliberately not symmetric.** `discardStage`
(3.5) removes a stage's objects, and annotations on a removed object go with
it: the claim has no subject any more. `discardAnnotationsOfStage` removes a
stage's claims and leaves every object in place. A re-run of an annotating
stage takes the second path and disturbs nothing it did not write. A re-run of
a *producing* stage takes the first, which destroys the annotations downstream
stages had attached — and that is correct rather than a loss, because those
stages read the layer and are therefore already stale by the `LayerRead` path
(`04-workspace.md` 4.5), so each must re-run and re-assert. The alternative —
orphaned annotations waiting for identical geometry to reappear — would make a
stale annotation indistinguishable from a current one.

**Validation mirrors `emit`.** `assertLabel` throws `InvalidArgumentError` for
an unknown label and for an object that does not exist; the latter is a foreign
key on `(layer_id, anchor_id)`, so it costs an indexed lookup inside the batch
transaction rather than a separate round trip. A stage may only annotate layers
it declared with `annotatesLayer` (`04-workspace.md` 4.2), checked by
`StageRunner` before the run so the failure arrives before the work does — the
same two-place treatment `writesLayer` already gets.

**Visibility follows the asserting stage, not the object's.** An annotation
becomes queryable under the rules of 3.5: `StageVisibility::CompletedOnly` by
default, so a later stage never sees a running annotator's partial output
(UR-STG-5), and an observer opts in and accepts retraction (UR-OBS-4). A stage
that is complete but *stale* keeps its annotations visible, because stale is
not discarded — the flag says the answer may be out of date, and
`StageStateReader::isStale` (3.10) is how a consumer finds that out, exactly as
for objects.

Batched and journalled like everything else here: one transaction inserts the
annotation rows and a single `ObjectsAnnotated` event, so UR-OBS-2's guarantee
that no event outruns its data holds for this path too.

**Both annotation tables are `WITHOUT ROWID`, keyed by their natural key, and
measurement names are interned.** `label_assertions` is keyed
`(object_id, stage_id, label_id)`; `measurement_assertions` is keyed
`(object_id, stage_id, name_id)`, where `name_id` references a
`measurement_names` table instead of the name being repeated on every row.

**That is the opposite of what 3.12 concludes for `objects`, and the inversion
is the point.** An object row carries a geometry blob of a couple of hundred
bytes, so a unique index duplicates only the key and the gain on insert order
dominates. An annotation row is three integers, so a unique index duplicates
very nearly the whole row and you maintain two B-trees to hold it. Measured at
ten million assertions (2026-10-10):

| `label_assertions` scheme | Ascending | Shuffled | Store |
|---|---|---|---|
| **`WITHOUT ROWID`, natural key** | **56.4 s** | **23.1 min** | **+137 MB** (13.8 B/row) |
| rowid key + unique index | 61.8 s | 29.0 min | +352 MB (35.5 B/row) |

The clustered form wins on both orders and is **2.6× smaller**. Taking 3.12's
rule as a general one would have cost 215 MB per slide and time on every pass.

**Interning measurement names halves the store.** Thirty million measurement
assertions — three per object — cost **1,586 MB** with the name as TEXT on
every row against **778 MB** interned, a saving of **808 MB per slide**, with
both figures reproducing byte for byte across four runs. Where the timing was
reproducible the interned form was also about **twice as fast**, the same
factor as the size and the expected one for a pass whose cost is bytes written.
This is UR-RES-17's reasoning, which 3.4 applies to labels and which nothing
had yet applied to measurement names. **It applies equally to `measurements`**,
the producing stage's own table, which stores names the same way; the
assertion table was measured because it is the one with a ten-million-row pass
behind it.

**Write order dominates every schema choice here by an order of magnitude.**
The same ten million label assertions cost 56 seconds written in ascending
object order and 23 minutes written in random order — **24.6×**, the largest
single factor measured anywhere in this design. UR-EXE-8's budget survives the
first comfortably and fails on the second, so delivering annotations in object
order is a correctness-of-schedule property of an annotating stage rather than
a tuning option. `04-workspace.md` 4.4 does not state it, and should.

> The realistic order is neither bound, and it behaves like the good one.
> Workers complete tiles out of sequence, but a tile's objects are contiguous
> in `object_id`, so the store sees a few thousand ascending runs rather than
> ten million random probes. Measured: 7,745 runs of about 1,282 objects took
> **36.6 seconds**, indistinguishable from the ascending figure once drive
> variance is allowed for. The 24.6× penalty requires genuinely random arrival,
> which no tile-sweeping stage produces.

> **The constraint is stated because nothing enforces it, not because the
> current schedule violates it.** A stage that annotates by walking a spatial
> query, or one that reorders work across tiles, lands in the shuffled case and
> loses the budget. Per-batch sorting is not a remedy: 3.12 measured it
> worthless for the objects table, and nothing here suggests it would do
> better.

## 3.8 Raster store — `storage/raster_store.hpp`

```cpp
enum class RasterCompression : int32_t { None, Lzw, Deflate };

/// Declared before anything runs, like a layer (UR-STG-10): the dependency
/// graph must resolve before the first stage starts, and a reader declared
/// ahead of its writer must have something to name.
class RasterDeclaration {
public:
    RasterDeclaration(const char* name, DataType, int32_t planeCount);
    RasterDeclaration& compression(RasterCompression);   // default Deflate
    RasterDeclaration& description(const char*);
};

class RasterWriter;
using RasterWriterPtr = std::shared_ptr<RasterWriter>;

class RasterWriter : public RasterSink {
public:
    ~RasterWriter() override;

    /// Accumulates one tile at the run's analysis level. Tiles may arrive in
    /// any order and need not cover the raster; unwritten area reads as zero.
    void writeTile(const BlockView&) override;

    /// Completes the raster: builds the pyramid down from the written level
    /// and publishes it. Until this returns, the raster is not readable and
    /// `RasterRegistry::isPopulated` is false.
    void flush() override;
    int64_t tilesWritten() const override;

    RasterId raster() const noexcept;
    StageId  stage() const noexcept;
};

class RasterRegistry {
public:
    RasterId declare(const RasterDeclaration&);

    bool        find(const char* name, RasterId& out) const noexcept;
    const char* name(RasterId) const;
    DataType    planeType(RasterId) const;
    int32_t     planeCount(RasterId) const;
    bool        exists(RasterId) const noexcept;

    /// False until some stage has written and flushed it. A raster that was
    /// declared and never written is legal and reads as zero.
    bool    isPopulated(RasterId) const noexcept;
    StageId writerStage(RasterId) const;
    int32_t writtenAtLevel(RasterId) const;
    int64_t bytesOnDisk(RasterId) const;

    /// A writer that attributes the raster to `stage`. One at a time per
    /// raster: unlike objects, a raster has a single producing stage, because
    /// two stages writing overlapping tiles of one image have no merge rule.
    RasterWriterPtr writerForStage(StageId, RasterId);

    /// Reads a populated raster one tile at a time, at whatever level and
    /// region the view carries — the same interface a layer rasterises
    /// through (`01-core.md` 1.9), so a consumer cannot tell whether the
    /// coarse product it reads was pixels or objects.
    TileRasterSourcePtr sourceFor(RasterId) const;

    /// Removes the raster a stage wrote: deletes the file and clears the
    /// population state. The declaration survives, as a layer's does.
    void discardRasterOfStage(StageId);

    int32_t count() const noexcept;
    RasterId at(int32_t index) const;
    int64_t  totalBytesOnDisk() const;
};
```

**Thread-safety:** `RasterRegistry` is ThreadSafe; `RasterWriter` is
ThreadSafe for `writeTile`, and `flush` requires exclusive access.
**Requirements:** UR-RES-7, UR-STG-4, UR-STG-6, UR-STG-9, UR-STG-10.

**A raster is not a layer, and `RasterId` is not a `LayerId`.** Twenty-five
functions in this specification take a `LayerId`, and about half of them —
`parent`, `depth`, `childCount`, `isAbove`, `objectCountInLayer`,
`ObjectId::fromAnchor`, `ObjectQuery::layer`, `restrictToLayer`,
`annotatesLayer` — mean nothing for an image. Admitting rasters under the same
type would give every one of them a runtime precondition that the type system
does not express and a caller discovers by hitting it: `ObjectQuery().layer(
probabilityMap)` would return an empty cursor rather than refuse. A `LayerId`
therefore keeps its single meaning — it names a collection of objects — and
rasters get their own identifier and their own registry. The cost is a
parallel registry of about a dozen functions; the alternative was a dozen
preconditions.

**The pixels live beside the store, not in it.** A level-0 `Float32`
probability map for the reference slide is 8×10⁹ pixels, 32 GB, which belongs
in neither a SQLite file nor a backup of one. Each raster is a pyramidal TIFF
in a subdirectory of the workspace — *not* the scratch subdirectory, which
`Workspace::create` and `open` clear (`04-workspace.md` 4.1) — and the store
holds one row naming it. Reading it back is SlideIO opening a TIFF, which the
library already depends on.

This module therefore gains a dependency on SlideIO that it did not have, for
raster I/O only. It is a module-level edge, not a library-level one: the data
library already links SlideIO through `reporting`'s `MaskWriter`. The
alternative placement was `reporting`, and it fails on the same point
`makeLayerRasterSource` failed on — `workspace` does not depend on `reporting`
and must not start to. Persistence with stage attribution, lifecycle and
discard is this module's subject; `reporting` exports finished results to
consumers.

**One writing stage per raster, enforced.** Objects from two stages coexist in
a layer because they are distinct rows. Tiles from two stages overlap in one
image and there is no merge rule, so `writerForStage` refuses a second stage
while a raster is populated by a first. A caller wanting two contributions
declares two rasters.

### Writing a raster is the one durable operation that is not a transaction

Everything else here commits in a single SQLite transaction, which is what
UR-OBS-2 rests on: an event and its data commit or fail together. A raster is
a file, and no file write is atomic with a transaction. This is stated rather
than assumed, because the failure modes are real and the recovery has to be
designed:

1. `writerForStage` allocates a **generated file name** and appends
   `RasterDeclared` if this is the first write. The name is never derived from
   the raster's name, so a re-run cannot collide with the file its previous
   attempt left behind.
2. Tiles accumulate into that file. The registry row still says
   `isPopulated == false`, so nothing reads it.
3. `flush` builds the pyramid, closes the file, and **then** commits one
   transaction setting the row's path, size, level and population flag, and
   appending `RasterWritten`.

The window is between 2 and 3. A crash there leaves a complete or partial file
that no row references, and `isPopulated` stays false — so a reader sees a
declared, unpopulated raster, which is a legal state, rather than a corrupt
one. **`Workspace::open` sweeps unreferenced raster files**, the way it already
clears the scratch subdirectory, and reports the bytes reclaimed. Discard runs
the same way in reverse: clear the row and commit first, delete the file
after, so a crash leaves an orphan for the sweep rather than a row pointing at
nothing.

The asymmetry is deliberate. An orphaned file costs disk and is recoverable; a
row pointing at a missing file is a store that lies about its own contents.

**Size is reported because nothing else governs it.** `bytesOnDisk` and
`totalBytesOnDisk` exist because the memory ceiling of UR-EXE-2 has no disk
counterpart anywhere in this design, and raster layers are the first feature
able to add tens of gigabytes to a workspace without the caller doing anything
that looks expensive.

**Archiving now means the directory, not the file.** UR-STG-9's unit was a
single `.store`; with rasters the workspace directory is the artifact, and
copying the store file alone yields a workspace whose rasters are all
declared and unpopulated. That is a consequence worth stating plainly rather
than discovering: it is legal, it is detectable, and it is not what the person
copying the file intended.

## 3.9 Event journal — `storage/event_journal.hpp`

```cpp
class Event {
public:
    int64_t   sequence() const noexcept;
    EventKind kind() const noexcept;
    int64_t   timestampUnixMs() const noexcept;

    StageId stage() const noexcept;        // invalid where not applicable
    LayerId layer() const noexcept;
    LabelId label() const noexcept;

    int64_t objectCount() const noexcept;  // ObjectsAdded / EdgesAsserted
    Rect64  boundingBox() const noexcept;  // union bbox of the batch
    StageStatus stageStatus() const noexcept;  // StageStateChanged
};

class EventCursor {
public:
    virtual ~EventCursor();

    /// Waits up to timeoutMs for the next event. Returns false on timeout.
    /// Pass 0 to poll without blocking, -1 to wait indefinitely.
    virtual bool next(int32_t timeoutMs) = 0;
    virtual const Event& current() const = 0;

    /// The sequence last returned. Persist it to resume in a later process.
    virtual int64_t position() const = 0;
    virtual void close() = 0;
};
using EventCursorPtr = std::shared_ptr<EventCursor>;

class EventJournal {
public:
    static const int64_t kFromBeginning;   // replay everything retained
    static const int64_t kFromNow;         // only events after subscribing

    int64_t oldestSequence() const;
    int64_t newestSequence() const;

    /// Begins at the first event with sequence > fromSequence. Throws
    /// InvalidArgumentError naming both positions if it has been truncated away.
    EventCursorPtr subscribe(int64_t fromSequence);

    /// Discards events below `sequence`. Objects, edges and stage state are
    /// unaffected.
    void truncateBelow(int64_t sequence);
    int64_t eventCount() const;
};
```

**Thread-safety:** `EventJournal` is ThreadSafe; `EventCursor` is
ThreadConfined. **Requirements:** UR-OBS-2 to UR-OBS-7, decision 6.

Events are rows in the store, not messages sent to subscribers. Subscription is
a cursor over `seq` — a subscriber holds one integer and asks for events above
it. That single property produces late join, replay from any point, resumption
after a subscriber restart, survival of a writer crash, and any number of
independent subscribers the writer knows nothing about. There is no subscriber
list, so an additional observer costs the writer nothing.

Events carry enough to decide whether to look further — kind, stage, layer,
count, union bounding box — and never geometry. A viewer whose viewport misses
the bounding box does nothing; one that intersects it issues an ordinary
spatial query through `ObjectStore`. The journal stays small and the expensive
path stays the one already designed (UR-OBS-5).

A subscriber falling behind a fast writer causes no backpressure and no drops.
Its next `next()` simply has more rows waiting, which it may coalesce — a
thousand `ObjectsAdded` events for one layer become one redraw.

`next(timeoutMs)` waits on the local signal when `eventSignalEnabled` and falls
back to the poll interval otherwise. **Correctness never depends on the
signal**: a missed signal makes a subscriber late, never wrong, and a
polling-only subscriber observes the identical sequence (UR-OBS-6). The
transport is local to the machine — a named semaphore on Windows, an eventfd
or Unix domain socket on POSIX — and the library opens no network socket.

## 3.10 Stage state reader — `storage/stage_state_reader.hpp`

```cpp
class StageStateReader {
public:
    int32_t count() const noexcept;
    StageId at(int32_t index) const;
    bool    find(const char* name, StageId& out) const noexcept;

    const char* name(StageId) const;
    StageStatus status(StageId) const;
    bool        isStale(StageId) const;
    const char* configuration(StageId) const;     // JSON, "" if none
    int64_t     completedAtUnixMs(StageId) const; // 0 if never completed

    int32_t dependencyCount(StageId) const;
    StageId dependencyAt(StageId, int32_t index) const;
    int32_t writtenLayerCount(StageId) const;
    LayerId writtenLayerAt(StageId, int32_t index) const;
};
```

**Thread-safety:** ThreadSafe. **Requirements:** UR-STG-4, UR-STG-2,
UR-STG-10.

Read side only. Declaring, running and invalidating stages belong to
`workspace`; this exists so an observer can report "nuclei: complete, cells:
running" without linking `slideio-processor-engine` and therefore without
loading OpenCV (design 5.1).

UR-STG-4 requires stage state be queryable without running anything, which this
satisfies directly — a `ReadOnly` store answers every method here.

## 3.11 GeoJSON import — `storage/geojson_import.hpp`

```cpp
class ImportResult {
public:
    int64_t objectsImported() const noexcept;
    int32_t labelsRegistered() const noexcept;
    int32_t featuresSkipped() const noexcept;
    ImportPolicy policyApplied() const noexcept;
};

class GeoJsonImporter {
public:
    GeoJsonImporter(Store&, LayerId targetLayer, StageId assertingStage);
    ~GeoJsonImporter();

    ImportResult import(const char* path);
    ImportResult import(const char* path,
                        ProgressObserver*,
                        const CancellationToken&);
};

/// Reads the first polygon of a GeoJSON file as a processing ROI. No store
/// is involved; the result is passed to PipelineSpec::restrictToPolygon.
Polygon importRoiPolygon(const char* path);
```

**Thread-safety:** ThreadConfined. **Requirements:** UR-RES-8, UR-RES-13,
UR-RES-19, UR-IMG-5, decision 3.

Import lives here rather than in `reporting` because it is an input, not a
report, and because UR-RES-19's policy has to run next to the vocabulary
registry that enforces it (decision 3). It reads through `core::GeoJsonReader`,
so there is one GeoJSON parser in the library, not two.

Under `Strict`, a label belonging to neither the standard nor the workspace
vocabulary aborts the import with a diagnostic naming the label. Under
`Permissive`, it is registered as workspace-local with origin
`ImportRegistered`, so a consumer can later tell a label that was declared from
one that merely arrived (UR-RES-19).

Imported objects carry the target layer as a property of the object itself, so
they are returned by a layer query alongside objects written by a stage even
though no stage produced them (UR-RES-13).

`importRoiPolygon` is the UR-IMG-5 path: a QuPath-exported annotation becomes a
processing ROI without any store being involved.

## 3.12 Schema and format

Written for reference; the DDL itself belongs to the implementation plan.

| Table | Holds |
|---|---|
| `meta` | Store format version, vocabulary version, provenance |
| `layers` | Name, kind, parent, depth, dropped flag |
| `labels` | Name, origin, recognised flag, `ontology_ref` (reserved, empty in v1 — 02 §2.2) |
| `stages` | Name, status, stale flag, configuration, completion time |
| `stage_deps` | Declared dependencies |
| `stage_layers` | Declared written layers |
| `objects` | Monotonic rowid key, unique index on `(layer_id, anchor_id)`, stage, geometry, bbox, label |
| `object_rtree` | R\*Tree index over bounding boxes |
| `measurements` | Per-object interned name, value and unit, from the producing stage |
| `measurement_names` | Interned measurement names, shared by `measurements` and `measurement_assertions` (3.7) |
| `label_assertions` | Object, asserting stage, label — `WITHOUT ROWID`; labels attached after the object was written (3.7) |
| `measurement_assertions` | Object, asserting stage, interned name, value, unit — `WITHOUT ROWID` (3.7) |
| `parent_edges` | Child, parent, asserting stage |
| `derived_from` | Derived, primitive, asserting stage |
| `rasters` | Name, data type, plane count, writing stage, level, file path, bytes, populated flag (3.8) |
| `events` | Sequence, kind, stage, layer, count, bbox, timestamp |

**The identifier a caller sees and the key the table is ordered by are not the
same thing.** `ObjectId` stays exactly what decision 8 defines — layer plus
geometry-derived anchor, stable across tile sizes and worker counts, the only
identifier ever exported or compared (UR-QA-3). The `objects` table is ordered
by a **monotonic rowid**, with a unique index on `(layer_id, anchor_id)`
carrying the logical identity.

That split is a schedule requirement, not a refinement. Inserting ten million
objects keyed physically by a random 64-bit anchor measured **31.6 minutes**
against **7.7 minutes** for monotonic keys — a 4.1× penalty from scattering
random writes across a 2.9 GB table rather than appending to its end — and
UR-EXE-8 allows 30 minutes for segmentation *and* classification together. The
anchor-keyed scheme exceeds the whole budget before a pixel is read.

**The scheme specified above has since been measured against the alternative it
replaces, and it holds.** A second run (2026-10-10, ten million objects, same
slide, same seed and same batch size, so every variant saw an identical anchor
sequence — confirmed by an identical collision count of 71,228 in all three):

| Scheme | Insert | Rate | Store |
|---|---|---|---|
| Anchor **is** the primary key | 25.9 min | 6,435 obj/s | 3.126 GB |
| Anchor PK, each batch sorted by anchor | 30.7 min | 5,432 obj/s | 3.113 GB |
| **Monotonic rowid PK + `UNIQUE(layer_id, anchor_id)`** | **10.8 min** | **15,409 obj/s** | **3.023 GB** |

The unique index gives back **2.39×** of the penalty, which is what this
measurement existed to ask. It costs nothing in space — the store is 103 MB
*smaller* than the anchor-keyed one, because random inserts leave primary-key
pages roughly two-thirds full while appended rowid pages pack nearly full. The
geometry join improves for the same clustered-read reason, by **1.7–2.0×**
measured as join cost minus R\*Tree cost within a single store. That is the only
cross-store query figure worth quoting: the three were read in write order and
differ in page-cache warmth by up to 7× on an R\*Tree table that is
byte-identical in all of them.

**Sorting each batch by anchor before insert is not a cheaper partial measure
and should not be attempted.** It measured 18% *slower* than not sorting — ten
thousand keys sorted within a batch spanning the whole 64-bit space still land
on ten thousand different pages, so the sort buys no locality and costs its own
time. Recorded because an earlier version of this section proposed it.

**The one price is the point lookup, and it is affordable.** `get(ObjectId)`
resolves through the index and then the table, paying one extra B-tree descent:
**11.3 µs against 5.1 µs**, stable across repeated runs and therefore structural
rather than cache. Six microseconds on an incidental path buys fifteen minutes
on the bulk one. The first attempt to measure this found no difference and was
wrong — it probed synthetic anchors, hit 31 of 20,000, and so timed the miss
path, where the extra hop never runs.

Every figure above comes from a DRAM-less consumer SATA SSD near capacity
rather than UR-EXE-8's reference NVMe. They are a floor, and the ratios travel
better than the absolutes.

**The annotation pass has since been measured, and it is cheap when it is
ordered.** Ten million label assertions cost 56 seconds in ascending object
order against the object pass's 10.8 minutes, so the earlier reading of this
section — that the two passes would consume 21.6 of UR-EXE-8's 30 minutes and
leave about 8 for everything else — was wrong. Ordered, the second pass is a
rounding error on the first. It becomes 23 minutes, and the budget fails, only
when the annotations arrive in random object order. The schema findings are in
3.7.

**Store size at the reference object count**, measured rather than estimated:
3.0 GB of objects and R\*Tree, 137 MB for one label assertion per object, and
778 MB for three interned measurement assertions per object — about **3.9 GB
per slide** before rasters. No requirement bounds that; the absence of a disk
budget anywhere, against UR-EXE-2's memory ceiling, remains an open action.

The realistic write order — tiles completed out of sequence, objects contiguous
within a tile — has since been measured at **36.6 seconds**, which is the
ascending case rather than the shuffled one. The budget holds with room.

Open question 5 in `requirements.md` — how geometry is encoded, WKB versus a
compact custom encoding versus simplified contours — is unresolved and drives
store size, query cost and export speed at ten million objects. It does not
affect any signature here.

Open question 4 — whether SQLite with R\*Tree sustains the reference object
count — is **closed yes**, conditional on the physical-key scheme above. The
spike measured query cost scaling with result size rather than store size,
realistic viewer viewports at 2 to 39 ms including geometry, and UR-OBS-1
holding exactly: 69,477 queries from a second process during an active
ten-million-object write, zero errors, no measurable writer slowdown. The event
journal and the concurrent-reader guarantee rest on the same SQLite instance
and are covered by the same result.
