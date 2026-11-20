# SDS 04 — `workspace`

**Module:** `workspace` · **Library:** `slideio-processor-engine` · **Depends on:** `core`, `config`, `pipeline`, `storage`

Stage semantics: declaration, execution, re-run, staleness, layer lifecycle.
Holds no schema of its own — everything durable goes through `storage`.

Conventions in [00-conventions.md](00-conventions.md).

---

## 4.1 Workspace — `workspace/workspace.hpp`

```cpp
class Workspace;
using WorkspacePtr = std::shared_ptr<Workspace>;

class Workspace {
public:
    /// Creates a workspace binding a slide scene to a new store.
    static WorkspacePtr create(const char* workspacePath,
                               const char* slidePath,
                               int32_t sceneIndex,
                               const Configuration&);

    /// Opens an existing workspace, in this or any later process.
    static WorkspacePtr open(const char* workspacePath, const Configuration&);

    /// Opens without a writer lock: stage state and objects are readable,
    /// nothing may be run. Safe alongside another process's writer.
    static WorkspacePtr openReadOnly(const char* workspacePath,
                                     const Configuration&);
    ~Workspace();

    const char* path() const noexcept;
    const char* slidePath() const noexcept;
    int32_t     sceneIndex() const noexcept;
    bool        isReadOnly() const noexcept;

    StorePtr          store() noexcept;
    std::shared_ptr<slideio::Scene> scene() noexcept;  // SlideIO's ScenePtr is a macro
    const Configuration& configuration() const noexcept;

    /// `configuration()` with `scratchDirectory` pointed at a subdirectory of
    /// this workspace. What `StageRunner` hands to `Pipeline`, so seam spill
    /// files live inside the artifact rather than beside it (UR-STG-9).
    const Configuration& scratchConfiguration() const noexcept;
    MemoryBudgetPtr   memoryBudget() noexcept;

    LayerRegistry&   layers() noexcept;
    StageRegistry&   stages() noexcept;
    LayerLifecycle&  layerLifecycle() noexcept;

    void close();
};
```

**Thread-safety:** ThreadSafe for accessors; single-writer for execution
(Assumption 6). **Requirements:** UR-STG-1, UR-STG-9, UR-EXE-2.

A workspace is a directory whose location is a single path, so a complete
analysis copies or archives as one unit (UR-STG-9). It holds the store, the
recorded state of every stage, and the binding to the slide and scene.

**Directory layout.** The store is `<workspace>/workspace.store`; its sidecars
(`-wal`, `-shm`, `-writer.lock`) and its raster directory
`workspace.store-rasters/` sit beside it, so inside the workspace. Scratch is
`<workspace>/scratch/`. `create` makes the directory, and only it: the parent
must exist, and a missing parent is an `IoError` naming the path, with nothing
made. It throws `IoError` when the path exists and is not an empty directory; it
opens the slide and checks the scene index (`InvalidArgumentError` when out of
range) before making anything. A `create` that fails later removes what it made,
and only that, tracked rather than inferred: the store, its sidecars, its raster
directory and scratch when it created the store, and the directory when it
created the directory and nothing else is left in it. So a retry can succeed, and
a `create` racing another on the same path — which fails, finding the directory
or the store already made — never removes the winner's workspace.

**Slide binding.** The binding is the store's `Provenance`: `slideId` holds the
slide path (UTF-8) and `sceneIndex` the scene. The path is stored absolute and
lexically normalised (`std::filesystem::absolute(u8path(slidePath)).lexically_normal()`),
so a relative path given to `create` still reopens from any later working
directory; `slidePath()` returns the stored form. `open` and
`openReadOnly` reopen the slide from it with `slideio::openSlide(path, "")`; a
slide that no longer opens, or no longer has the scene, is an `IoError` naming
the path. The workspace keeps the slide object alive as long as its scene, and
`close` releases both, so a scene obtained from `scene()` must not be used after
`close`.

**Classification.** `Provenance.slideId` holds the slide path, stored absolute,
and the workspace's error and log messages name it (a slide that does not open,
a scene out of range, a failed create or close). Slide paths can embed patient
identifiers or accession numbers, so a workspace store, and any log that
carries workspace messages, is classed Confidential (HIPAA Security Rule
§164.312(b); GDPR Art. 5(1)(c)). The library does not redact the path.

`create` and `open` clear the scratch subdirectory that `scratchConfiguration`
points at. Seam spill files (5.7) are owned by one run and removed when it
ends, so anything still there was left by a process that died mid-stage —
whose stage is recorded not complete and whose objects were discarded anyway.
Clearing at open is what keeps a crashed run from growing the archive
indefinitely, and it is safe precisely because nothing durable is ever written
there.

`open` also calls `store()->rasters().removeUnreferencedFiles()` and reports the
bytes reclaimed, beside its scratch clear: a raster file that no row names is what
a crashed run or a failed delete left behind, and the sweep removes it (03 3.8).
Only names the library generates are touched, and a file a live writer owns is
spared.

`openReadOnly` is the observer's entry point when it wants stage state and
objects together rather than opening the store directly. It takes no writer
lock and is safe alongside a running script process (UR-OBS-1). Its stage
registry follows that process's declarations (4.3).

`memoryBudget()` returns the single `MemoryBudget` instance,
`makeMemoryBudget(configuration().memoryCeilingBytes())`, made once per
`Workspace` and handed to every pipeline it runs, so UR-EXE-2's ceiling spans
raster tiles and object accumulation across them together rather than one
pipeline alone. `storage` takes no budget today, so store buffers are not
reserved against this instance.

A workspace holds **exactly one** slide — open question 9, closed by UR-EXE-9.
Batch processing needs N slides to be N independent single-writer stores, which
satisfies Assumption 6 with no coordination between them; a shared multi-slide
workspace would serialise every writer and make batch concurrency unavailable.
It also keeps the archiving unit coherent now that a workspace directory holds
persisted rasters beside its store (`03-storage.md` 3.8): one directory is one
slide's results.

## 4.2 Stage definition — `workspace/stage_definition.hpp`

```cpp
class StageDefinition {
public:
    explicit StageDefinition(const char* name);
    StageDefinition(const StageDefinition&);
    StageDefinition& operator=(const StageDefinition&);
    ~StageDefinition();

    /// Declares a layer this stage reads — by spatial query, to assert edges
    /// against, or for any other purpose. Callable more than once. Stage
    /// dependencies are derived from this: a stage depends on every stage
    /// that writes a layer it reads (4.3).
    StageDefinition& readsLayer(LayerId);

    /// Declares a layer this stage may write. Callable more than once: one
    /// stage may emit several layers in a single pass.
    StageDefinition& writesLayer(LayerId);

    /// Declares a layer whose objects this stage may annotate — attach labels
    /// or measurements to objects an earlier stage wrote (`03-storage.md`
    /// 3.7). Callable more than once. Implies `readsLayer` for that layer,
    /// never `writesLayer`: annotating adds no objects to it, and a stage
    /// that annotates must not be treated as one of its writers (4.3).
    StageDefinition& annotatesLayer(LayerId);

    /// An ordering edge that is *not* a data dependency — rarely needed, and
    /// never required for a stage that simply consumes an earlier stage's
    /// layer. Prefer `readsLayer`.
    StageDefinition& dependsOn(const char* stageName);

    /// What this stage computes: channels, ROI or polygon, and the operation
    /// chain. Copied; see `pipeline::PipelineSpec` (5.4).
    StageDefinition& pipeline(const PipelineSpec&);

    /// Declares the persisted raster this stage writes (`03-storage.md` 3.8).
    /// At most one — a raster has a single producing stage, because two
    /// stages writing overlapping tiles of one image have no merge rule.
    StageDefinition& writesRaster(RasterId);

    /// Binds a persisted raster as an auxiliary input under `bindingName`,
    /// read at this stage's analysis level (5.1). The dependency on the stage
    /// that writes it is derived from this, as `readsLayer` derives one for
    /// objects (4.3).
    StageDefinition& readsRaster(const char* bindingName, RasterId);

    /// Binds an earlier stage's layer as an auxiliary raster input, readable
    /// by operations under `bindingName` at this stage's analysis level
    /// (5.1, 5.5) — the coarse half of a coarse-to-fine pair. Implies
    /// `readsLayer`. Callable more than once with different names. A binding
    /// name names one input across `readsRaster` and `readsLayerAsRaster`:
    /// reusing it for another target is an `InvalidArgumentError` naming the
    /// stage and the binding, and an exact repeat is a no-op.
    StageDefinition& readsLayerAsRaster(const char* bindingName, LayerId,
                                        LayerRasterContent);

    /// Restricts processing to the objects of an earlier stage's layer, so a
    /// cells stage can be confined to regions a background stage found.
    /// Implies `readsLayer` for that layer.
    StageDefinition& restrictToLayer(LayerId);

    const char* name() const noexcept;

    const PipelineSpec& pipeline() const noexcept;
    int32_t readLayerCount() const noexcept;   // includes restriction and
    LayerId readLayerAt(int32_t index) const;  // annotated layers
    int32_t writtenLayerCount() const noexcept;
    LayerId writtenLayerAt(int32_t index) const;
    int32_t annotatedLayerCount() const noexcept;
    LayerId annotatedLayerAt(int32_t index) const;
    bool     writesAnyRaster() const noexcept;
    RasterId writtenRaster() const noexcept;
    int32_t  readRasterCount() const noexcept;
    const char* readRasterBindingAt(int32_t index) const;
    RasterId    readRasterAt(int32_t index) const;

    int32_t rasterLayerCount() const noexcept;
    const char* rasterBindingAt(int32_t index) const;
    LayerId     rasterLayerAt(int32_t index) const;
    LayerRasterContent rasterContentAt(int32_t index) const;
    int32_t explicitDependencyCount() const noexcept;
    const char* explicitDependencyAt(int32_t index) const;
    bool    hasLayerRestriction() const noexcept;
    LayerId layerRestriction() const noexcept;
};
```

**Thread-safety:** ThreadConfined. **Requirements:** UR-STG-2, UR-STG-3,
UR-STG-4, UR-STG-10, UR-IMG-5.

**A stage is a `PipelineSpec` plus the things only a workspace can supply.**
An earlier draft gave `StageDefinition` its own builder — `operation`,
`channels`, `restrictToRoi`, `restrictToPolygon` — duplicating `Pipeline`'s
almost method for method, and `StageRunner` spent its first job copying one
into the other. The two have now been split along the line that actually
divides them: `PipelineSpec` is *what to compute*, and it is a value that needs
no workspace; `StageDefinition` is *the durable identity of computing it*, and
every remaining member here needs one.

What is left is the whole of the answer to "what does the stage add over the
pipeline":

| Member | Why it cannot live in the spec |
|---|---|
| `name` | The spec is anonymous. Identity is the stage's contribution. |
| `readsLayer` / `writesLayer` | `LayerId`s exist only in a store. |
| `annotatesLayer` | Same, and the annotation writer is the store's. |
| `readsLayerAsRaster` | Same, and rasterising a layer requires querying objects. |
| `writesRaster` / `readsRaster` | `RasterId`s exist only in a store, and the writer is the store's. |
| `restrictToLayer` | Resolving a layer to a mask requires querying objects. |
| `dependsOn` | Names other stages; meaningless without a registry. |

**A stage reads a coarse product the same way whether it is pixels or
objects.** `readsRaster` and `readsLayerAsRaster` both lower to a
`TileRasterSource` bound under a name, and `OperationContext::auxiliary` is the
single read path. An operation therefore cannot tell — and must not care —
whether the coarse pass it refines emitted polygons that were rasterised back
or pixels that were stored as pixels. That symmetry is the point: a product
with a contour takes the object route and keeps its queryability, a product
without one takes the raster route, and the consumer is written once.

**`readsLayerAsRaster` is the other read-only declaration**, and it lowers the
way `restrictToLayer` does: into a per-tile source built here, bound on the
`Pipeline`, never computed whole. The difference is what the fine stage gets —
a restriction says where to compute, a raster says what the coarse pass found
there. Both imply `readsLayer`, so a coarse-to-fine pair is an ordinary
dependency with ordinary staleness: re-running the coarse stage marks the fine
one stale by `LayerRead` (4.5), which is correct, because a refinement is only
as current as the thing it refined.

**`annotatesLayer` is a read declaration, not a write one**, and the
distinction carries weight rather than tidiness. A stage that annotates
`cells` must run after whatever wrote `cells`, which `readsLayer` already
derives (4.3), and must go stale when that stage re-runs, which the
`LayerRead` path already propagates (4.5) — so the dependency and the
invalidation both come for free, correctly, with no new graph edge. Treating it
as a write instead would be wrong twice over: `LayerRegistry` would record the
annotator among the layer's writers and fold its analysis level into the
layer's stated precision, and `StageRunner`'s re-run path would call
`discardStage` on a stage that wrote no objects there — deleting, at best,
nothing it owned. An annotator is a reader that leaves a note.

**Dependencies are declared as data, not as stage names.** An earlier draft had
`dependsOn("background")` alongside `restrictToLayer(tissue)` — the same fact
in two vocabularies, with nothing checking that they agreed. Declaring
`readsLayer` instead lets the registry derive the dependency, and closes two
holes the pair left open:

- A stage with `restrictToLayer(tissue)` and no `dependsOn` was perfectly
  legal, immediately runnable, and would run against an empty `tissue` layer,
  producing nothing and reporting success.
- A `dependsOn` naming a stage that was later renamed, or misspelled from the
  start, had nothing to validate against.

Reads had to become explicit anyway. `restrictToLayer` covers only the case
where a stage confines its work to a layer; UR-STG-3 also requires a stage be
able to read an earlier layer by spatial query and assert edges against it
without restricting to it — a `cells` stage attaching pre-existing nuclei reads
`nuclei` and is restricted by `tissue`. That read was previously undeclarable,
and `dependsOn` was the patch over the gap.

`dependsOn` survives for ordering that is genuinely not a data dependency —
two stages writing the same layer where one must go first, for instance — and
UR-STG-2's "shall declare the earlier stages they depend on" is still
satisfiable literally. It should be rare, and a definition using it where
`readsLayer` would do is a smell.

Operations are named rather than passed as objects, and resolved against
`pipeline::OperationCatalog` at run time. That is what keeps `workspace` from
linking `algorithm`: the catalog is a leaf nothing depends on, so its contents
can change without touching this module (design 4). The spec preserves that —
it stores names and parameter bags, never `OperationPtr`s, even when built
through `PipelineSpec::then` from a typed factory.

`writesLayer` is the authority on which layers a stage may write, and
`writtenLayerCount` / `writtenLayerAt` are what `StageRunner` validates each
operation's declared target against (4.4). Multiple calls are legal because a
stage is not one layer's worth of work: `requirements.md` §10 gives a single
stage emitting cells and their nuclei in one pass as the case that separates the
two concepts.

An object-producing operation names its target layer as a string in its
parameters — `setString("targetLayer", "nuclei")`, or the typed
`targetLayer(const char*)` setter on the operation's parameter class (6.7) — and
the runner resolves it. Putting the layer in the parameter bag rather than in the
operation's constructor is what makes these operations constructible through
`OperationCatalog::create`, which is the only route a stage has. A `LayerId`
could not travel that way, and could not survive the record (below) either,
since it is an opaque handle meaningful in one store only.

The read accessors exist because `StageRunner` must walk the definition without
re-parsing the JSON, and because a caller can inspect a declared stage before
running it.

**The record is made by the registry, not by the definition.** An earlier draft
gave `StageDefinition` a `configurationJson`, but a definition holds `LayerId`s
and `RasterId`s, which name nothing without a store, while the record must carry
names. `StageRegistry::declare` therefore produces the record from the definition
and the store's layer and raster registries, and `StageRegistry::configuration`
reads it back. The record is stored against the stage and is recoverable from
the store alone, without the code that produced it (UR-RES-11, UR-STG-4). It
carries every declaration by name, so a definition is rebuilt from it alone. It
embeds `PipelineSpec::toJson` under a `pipeline` key and adds the
workspace-level fields around it:

```json
{ "stageRecordVersion": 1,
  "stage": "nuclei",
  "readsLayers": ["tissue"],
  "writesLayers": ["nuclei"],
  "annotatesLayers": [],
  "restrictToLayer": "tissue",
  "readsLayersAsRaster": [ { "binding": "coarse", "layer": "nests_coarse",
                             "content": "ObjectAnchor" } ],
  "writesRaster": null,
  "readsRasters": [ { "binding": "prob", "raster": "probability" } ],
  "dependsOn": [],
  "pipeline": { "channels": …, "operations": [ … ] } }
```

`restrictToLayer` and `writesRaster` are `null` when absent. `readsLayers` lists
only the explicit `readsLayer` calls: the reads that `annotatesLayer`,
`restrictToLayer` and `readsLayersAsRaster` imply are derived again when the
record is loaded, so a restriction alone yields `"readsLayers": []`. A
`stageRecordVersion` other than 1, a malformed record or a name the store does
not hold is an `InvalidArgumentError` on load. Two definitions declare the same
stage when their names, pipelines (`PipelineSpec::equals`) and every declaration
list and optional are equal, each list compared without regard to order; that
comparison, not the record's text, is what decides whether a redeclaration is a
reconfiguration, so formatting and ordering never mark a stage stale.
`StageDefinition` gains a copy assignment beside its copy constructor, since a
definition is a value a caller builds, stores and replaces.

Derived dependencies are deliberately absent from the record. They are a
function of the other stages present, so storing them would freeze a snapshot
of a graph that changes as stages are declared — and a record replayed into a
different workspace would carry dependencies on stages that may not exist
there. What the stage itself asserts is what it reads and writes; the graph is
recomputed from that.

Because `PipelineSpec::fromJson` reads that inner object back, a recorded stage
is now replayable and not merely legible — the half of UR-RES-11 that the
previous split could not deliver, since nothing could reconstruct a pipeline
from the record. Layers and stages appear by name, so the record also survives
being carried to a different workspace.

`restrictToLayer` is the UR-STG-3 path: a stage reads the objects of any
completed earlier stage, including by spatial query, and confines its work to
them. Its acceptance criterion is that a `cells` stage restricted to regions
from a `background` stage produces no objects outside those regions.

### Declaring a coarse-to-fine pair

```cpp
class CoarseToFineSpec {
public:
    CoarseToFineSpec(const char* coarseStageName, const char* fineStageName);
    CoarseToFineSpec(const CoarseToFineSpec&);
    ~CoarseToFineSpec();

    /// The coarse half: what it computes, and the layer it writes. Its
    /// `PipelineSpec` carries the coarse `analysisLevel` (5.4).
    CoarseToFineSpec& coarse(const PipelineSpec&, LayerId writes);

    /// The fine half. Its `analysisLevel` may stay -1 for automatic
    /// selection; when both are explicit the coarse one must be strictly
    /// coarser.
    CoarseToFineSpec& fine(const PipelineSpec&, LayerId writes);

    /// The name the fine stage's operations address the coarse raster by
    /// (`05-pipeline.md` 5.1), and what each pixel carries. Defaults to
    /// "coarse" and `LayerRasterContent::ObjectAnchor` — the only content
    /// that distinguishes one coarse region from another, and so the only
    /// one a refinement can usually act on (`01-core.md` 1.9).
    CoarseToFineSpec& binding(const char* name, LayerRasterContent);

    /// Also restrict the fine stage to the coarse layer, so it reads no tile
    /// the coarse pass found nothing in. Default true.
    CoarseToFineSpec& restrictFineToCoarse(bool);
};

/// Declares the two stages, wired. The fine one gets `readsLayerAsRaster`
/// against the coarse one's layer — and so, through the implied `readsLayer`,
/// the dependency and the staleness edge (4.3, 4.5) — plus the restriction
/// unless it was turned off. Throws `StageError` naming the offending field
/// when the names match, the layers match, or the levels are not ordered.
void declareCoarseToFine(StageRegistry&, const CoarseToFineSpec&,
                         StageId& coarseOut, StageId& fineOut);
```

**Thread-safety:** `CoarseToFineSpec` is ThreadConfined.
**Requirements:** UR-SEG-8, UR-IMG-4, UR-STG-10.

Written out, the pair it replaces is five declarations and one string that has
to agree with a parameter elsewhere:

```cpp
StageDefinition c("nests_coarse");
c.pipeline(coarseSpec).writesLayer(nestsCoarse);
StageDefinition f("nests");
f.pipeline(fineSpec).writesLayer(nests)
 .readsLayerAsRaster("coarse", nestsCoarse, LayerRasterContent::Label)
 .restrictToLayer(nestsCoarse);
```

**What this saves is not typing.** It is three couplings a caller gets wrong
separately and silently:

- The **binding name** in `readsLayerAsRaster` must match the
  `auxiliaryName` parameter of the operation that reads it. A mismatch is not
  a compile error and not a validation error — `OperationContext::auxiliary`
  simply returns null, which 5.1 requires every reader to tolerate, so a
  refinement operation degrades to doing nothing useful on a run that looks
  successful.
- The **level ordering** must actually be coarse-to-fine. Nothing else checks
  it: two stages with the levels the wrong way round are a legal pipeline
  that reads a coarse result nobody computed coarsely, and the results are
  merely poor rather than erroneous.
- The **restriction** should follow the same layer as the raster. Pointing it
  at a different layer is legal, occasionally intended, and usually a typo.
- The **content** should be `ObjectAnchor`. `Label` is the plausible-looking
  wrong answer: segmentation operations emit unlabelled objects, so it
  rasterises zero everywhere and the refinement sees an empty plane on a run
  that reports success. The helper defaults to `ObjectAnchor` rather than
  inheriting `LayerRasterContent`'s own default for exactly that reason.

`declareCoarseToFine` makes all three true by construction and throws on the
one it can check, which is the ordering.

**It is sugar over `StageDefinition`, not a second path.** It declares two
ordinary stages, and nothing downstream can tell: dependency derivation,
staleness, re-run, discard and the stage record all behave exactly as they
would for two stages declared by hand (4.3, 4.5), because that is what they
are. In particular re-running the coarse stage marks the fine one stale by
`LayerRead` and no other mechanism is involved.

**It declares rather than handing back two definitions to adjust.** A caller
who needs to add `dependsOn`, a second `annotatesLayer` or another
`readsLayer` to either half should write the two stages out; the helper covers
the shape that is common, not the shape that is general, and a half-helper
returning definitions would have to re-expose every member of
`StageDefinition` to stay useful. The two `StageId`s come back because a
caller usually wants to run them, and because it keeps the helper honest about
having produced nothing but stages.

**A pair, deliberately, not a chain.** Three levels is not two applications of
this — the middle stage is both a reader and a writer of coarse rasters, and
the binding names stop being derivable from the pair. A chain is declared
directly, which costs the lines this helper saves and buys the clarity it
would otherwise have to invent.

## 4.3 Stage registry — `workspace/stage_registry.hpp`

```cpp
class StageRegistry {
public:
    /// Declares or redeclares a stage. Redeclaring with a different
    /// configuration marks it and its dependents stale.
    StageId declare(const StageDefinition&);

    int32_t count() const noexcept;
    StageId at(int32_t index) const;
    bool    find(const char* name, StageId& out) const noexcept;

    const char* name(StageId) const;
    StageStatus status(StageId) const;
    bool        isStale(StageId) const;
    const char* configuration(StageId) const;
    int64_t     completedAtUnixMs(StageId) const;

    /// The effective dependencies: every stage writing a layer this stage
    /// reads, plus any explicit `dependsOn`. Recomputed as stages are
    /// declared, so a writer declared after its reader still counts.
    int32_t dependencyCount(StageId) const;
    StageId dependencyAt(StageId, int32_t index) const;
    bool    isExplicitDependency(StageId, int32_t index) const;

    /// True when every effective dependency is complete and not stale.
    bool isRunnable(StageId) const;

    /// True for a stage kept by name only at restore, because its record could
    /// not be read. Such a stage is never runnable; `StageRunner` refuses it.
    bool isUnreadable(StageId) const;

    /// The definition the stage was declared with, as the registry holds it
    /// (restored from its record at open). Empty but for the name when the
    /// record could not be restored.
    const StageDefinition& definition(StageId) const;

    /// Marks `stage` and every stage its forward answer lists stale, by the five
    /// paths of 4.5, each once. **Transitive**, and there is no non-propagating form:
    /// a caller who wanted one would be asking for exactly the UR-STG-8
    /// failure — a dependent left current whose parents have been replaced.
    void markStale(StageId);

    /// Clears the flag on `stage` alone. **Not** transitive. A stage becomes
    /// current by being re-run, which is `StageRunner`'s business; clearing a
    /// dependent's flag without re-running it would assert a coherence
    /// nothing established.
    void clearStale(StageId);

    /// The derived graph
    /// behind `markStale` and `isRunnable`: what a re-run would invalidate,
    /// and why a stage is stale now (4.5). Recomputed with the dependency
    /// set on every `declare`; the reference stays valid for the life of the
    /// registry.
    const StalenessGraph& staleness() const noexcept;
};
```

**Thread-safety:** ThreadSafe. **Requirements:** UR-STG-2, UR-STG-4,
UR-STG-8, UR-STG-11, UR-OBS-1.

The read side duplicates `storage::StageStateReader` deliberately: an observer
gets the same information without linking this module, while a producer gets it
alongside declaration and staleness control. Both read the same `stages` table.

**The dependency graph is derived, not declared.** Stage *B* depends on stage
*A* when *A* declares a `writesLayer` that *B* declares a `readsLayer`, and the
registry computes that from the declarations it holds. Explicit `dependsOn`
edges are added to the same set, and `isExplicitDependency` distinguishes them
for anyone reporting the graph.

Derivation is order-independent: dependencies are recomputed on every
`declare`, so declaring a reader before its writer is legal and resolves once
the writer appears. That matters because a script does not always declare
stages in execution order, and requiring it to would reintroduce by convention
the ordering the derivation exists to remove.

`declare` rejects two things it previously could not see:

- **A read of an undeclared layer** — `InvalidArgumentError`, the same
  treatment `writesLayer` gets, since UR-STG-10 requires layers be declared up
  front.
- **A cycle** in the effective graph — `StageError` naming the stages in it.
  Two stages each reading a layer the other writes is a deadlock that the
  hand-written `dependsOn` form could also express and that nothing checked.

A layer read but written by no stage is **not** an error: UR-RES-8 imports fill
layers with no stage at all, and a reader of such a layer simply has no
dependency to wait on.

**One writer for the stale flag, one reader for the reasons.** The flag is
durable — it is a column in the `stages` table (`03-storage.md` 3.12), read
back by `StageStateReader::isStale` from a process that never links this
module. `StageRegistry` is the only thing that writes it. `StalenessGraph`
(4.5) computes *which* flags a change implies and explains *why*, and writes
nothing.

An earlier draft had both classes mutating: `markStale` here and
`propagateFrom` on the graph, with nothing saying whether either propagated.
That pair is worse than either alone. A caller reaching for the obvious name
and getting the non-propagating one invalidates a stage and leaves its
dependents current, which is silent, survives into the store, and is the exact
condition UR-STG-8 exists to prevent. Mutation now lives where the durable
state lives, and the graph that decides the extent of it is reachable through
`staleness()` rather than being a class nothing could obtain.

"A different configuration" in `declare` means `PipelineSpec::equals` over the
pipeline, plus an exact comparison of the read-layer, written-layer,
layer-restriction and explicit-dependency fields. Comparing the serialised JSON
instead would make staleness depend on key order and float formatting, so a
redeclaration that changed nothing could mark a slide's worth of downstream
work stale.

**Staleness follows the staleness graph (plan C, decisions 1 and 2).**
`markStale` walks the forward answer of 4.5 from the stage: the layer-read,
raster-read, layer-ordering, derived-from and explicit-dependency edges, not
the effective dependencies alone. It sets the flag through
`StageStateWriter::setStale` on the stage and on every stage the answer lists,
each once, so a cycle in the staleness graph terminates; `clearStale` sets it
on that stage alone. A redeclaration marks along the old edges first, then
writes the record, re-derives, and marks again along the new ones, so a
failure part way never leaves a changed stage current. The staleness edges
are rebuilt in memory with the dependency graph on every declare and restore,
from the held definitions and never from `LayerRegistry::writerStageAt`, which
keeps a writer a redeclaration dropped. They are not persisted: the dependency
rows, `isRunnable` and the cycle check are unchanged, because `LayerOrdering`
and `DerivedFrom` do not gate running.

**Plan A: persisted dependencies and restore (decision 7).** Every `declare`
rewrites the dependency rows of every stage (`clearDependencies`, then
`addDependency`) and records the declaring stage's written layers
(`addWrittenLayer`), so `StageStateReader` and `LayerRegistry::writerStageAt`
show the derived graph without linking this module, and a later declare repairs
the rows of an earlier one that failed part way. A redeclaration identical to
the held one does the same in a writable workspace (both writes repeat
harmlessly) and marks nothing stale, so declaring the stage again repairs its
written-layer rows too; in a read-only workspace it writes nothing. The
in-memory graph changes only after the whole rewrite succeeds. `declare`,
`markStale` and `clearStale` check first that the store is open, so a closed
workspace is an `IoError` rather than a misleading complaint about an
undeclared layer. The registry is complete when the workspace opens: its
constructor reads every stage row, rebuilds the definition with
`stageFromRecord`, and derives the graph from the definitions without writing
anything, so `openReadOnly` works. A declare in a reopened workspace therefore
derives from, and cycle-checks against, the stages persisted earlier. A row
whose record does not parse (an `InvalidArgumentError` or `InternalError` from
`stageFromRecord`; any other failure, an `IoError` in particular, propagates),
or whose explicit `dependsOn` names a stage that is not stored or the stage
itself, is kept by name only: a warning naming the stage is logged, the stage
has an empty definition, so it derives no reads or writes of its own, and it is
never runnable. Redeclaring it with a real definition repairs it and marks it
stale. Layers a stage wrote under an earlier declaration stay recorded as its
written layers, because storage has no way to remove one.

The edges around an unreadable stage are not lost. At restore the stored
dependency rows on either side of it — its own and other stages' dependencies
on it — are pinned into the graph as non-explicit edges (storage records no
explicit flag), and every later `declare` carries the pins, so a reader of a
broken writer's layer stays blocked, the writer never being runnable, instead of
becoming runnable. Redeclaring the stage successfully drops its pins and normal
derivation resumes. `dependencyCount`, `dependencyAt`, `isExplicitDependency`
and `isRunnable` all read this one in-memory graph; only the status and stale
flag of each dependency come from storage. Restore writes nothing.

**A read-only registry follows a live writer (UR-OBS-1).** An observer opens
with `openReadOnly` while the script process keeps declaring, so the graph
restored at open goes out of date while `count`, `at` and `configuration`,
which read the `stages` table directly, do not. Before `declare`,
`dependencyCount`, `dependencyAt`, `isExplicitDependency`, `isRunnable` and
`definition` answer, a read-only registry compares the stored stage count and
each stage's stored configuration with those it last restored; every declare
or redeclare changes one of them. When either differs it restores again, under
its mutex, building aside and swapping in, so a failure leaves the previous
state. Storage offers no cheaper change signal through its API. Stages are
never removed, so a stored count below the one restored means the store
cannot be read (it is closed): the registry then answers from what it last
read. A definition a refresh replaces is kept for the life of the registry,
so a reference from `definition()` never dangles. The writer registry is the
only writer, so it never re-reads.

## 4.4 Stage runner — `workspace/stage_runner.hpp`

```cpp
class StageRunner {
public:
    StageRunner(Workspace&, StageId);
    ~StageRunner();

    StageRunner& progress(ProgressObserver*);          // nullptr = none
    StageRunner& cancellation(const CancellationToken&);

    /// Runs the stage to completion. Throws StageError on failure, naming the
    /// stage and, for a tile-level fault, the tile.
    void run();

    bool    isRerun() const noexcept;
    int64_t objectsWritten() const noexcept;
    int64_t edgesAsserted() const noexcept;
};
```

**Thread-safety:** ThreadConfined; one runner at a time per workspace.
**Requirements:** UR-STG-3, UR-STG-5, UR-STG-6, UR-STG-7, UR-STG-10, UR-EXE-1,
UR-EXE-4, UR-EXE-5.

`run()` binds the definition's layer restriction to a mask source, constructs
a `Pipeline` from the definition's `PipelineSpec`, binds the stage's declared
layers and auxiliary rasters, and validates, all at the gate, before anything
is written. Only then does it prepare a re-run, wire the workspace's writers as
the sinks, and execute eagerly across the tile grid:

```cpp
PipelineSpec spec = def.pipeline();
LayerRegistry& layers = ws_.layers();

// ---- at the gate: build, bind and validate; nothing here writes ----
TileMaskSourcePtr mask;                            // null when unrestricted
if (def.hasLayerRestriction()) {                   // the one thing a spec
    LayerId src = def.layerRestriction();          // cannot express alone
    int32_t srcLevel = layers.coarsestAnalysisLevel(src);
    double  halfStep = LevelSelector(ws_.scene()).scaleOfLevel(srcLevel) / 2.0;
    mask = makeLayerMaskSource(ws_.store()->objects(), src, halfStep);
}

std::vector<TileRasterSourcePtr> aux;              // coarse-to-fine inputs
for (int32_t i = 0; i < def.rasterLayerCount(); ++i)
    aux.push_back(makeLayerRasterSource(ws_.store()->objects(),
                                        def.rasterLayerAt(i),
                                        def.rasterContentAt(i)));

std::vector<TileRasterSourcePtr> stored;           // persisted rasters (3.8)
for (int32_t i = 0; i < def.readRasterCount(); ++i)
    stored.push_back(ws_.store()->rasters().sourceFor(def.readRasterAt(i)));

Pipeline p(ws_.scene(), ws_.scratchConfiguration(), spec);   // scratch inside
p.memoryBudget(ws_.memoryBudget())                           // the workspace
 .stageName(def.name())                                     // progress, errors
 .maskSource(mask.get());                        // null when unrestricted
for (int32_t i = 0; i < def.writtenLayerCount(); ++i)
    p.bindLayer(layers.name(def.writtenLayerAt(i)), def.writtenLayerAt(i));
for (int32_t i = 0; i < def.rasterLayerCount(); ++i)             // coarse-to-
    p.auxiliaryRaster(def.rasterBindingAt(i), aux[i].get());     // fine (5.5)
for (int32_t i = 0; i < def.readRasterCount(); ++i)              // stored
    p.auxiliaryRaster(def.readRasterBindingAt(i),                // rasters
                      stored[i].get());                          // (3.8)
p.validate();                                      // a refusal: nothing changed

// ---- the run: re-run preparation (below), then the writers ----
auto sink  = ws_.store()->objects().writerForStage(id_);
auto edges = ws_.store()->edges().writerForStage(id_);
auto notes = ws_.store()->annotations().writerForStage(id_);   // 3.7

RasterWriterPtr pixels;                            // null unless declared
if (def.writesAnyRaster())                                       // 3.8
    pixels = ws_.store()->rasters().writerForStage(id_, def.writtenRaster());

p.edgeSink(edges.get())                                      // UR-RES-5
 .annotationSink(notes.get())                                // 3.7
 .rasterSink(pixels.get());                        // null unless declared

p.runInto(*sink, progress_, cancel_);

sink->flush();        // endpoints first
edges->flush();       // then the edges naming them (3.6)
notes->flush();       // then the annotations naming them (3.7)
if (pixels) pixels->flush();   // last: builds the pyramid and publishes (3.8)

for (int32_t i = 0; i < def.writtenLayerCount(); ++i)   // on success only
    layers.recordAnalysisLevel(def.writtenLayerAt(i), p.analysisLevel());

ws_.store()->stageStateWriter().setStatus(id_, StageStatus::Complete);
ws_.stages().clearStale(id_);                          // current again
```

The raster flush is last and is the one that is not cheap: it builds the
pyramid down from the analysis level and only then commits the row that makes
the raster readable. A stage that fails before it leaves a declared,
unpopulated raster — a legal state — rather than a half-written one
(`03-storage.md` 3.8).

**Flush order is the same argument three times.** Objects commit first
because edges and annotations both reference them by identifier, and a batch
that named an object not yet in the store would break the foreign key that
`assertParent` and `assertLabel` are validated against. Annotations go last
rather than beside the edges for no deeper reason than that nothing references
*them*; the ordering between the two is free, and fixing it costs nothing and
removes a question.

The analysis level is recorded against each written layer **on success only**,
and after the flushes — a stage that fails has written no geometry and must not
coarsen a layer's stated precision. The level `LayerRegistry` records is the
maximum ever recorded, and discards do not lower it, so re-running this stage
at a finer level does not improve the recorded figure (`03-storage.md` 3.3).
Erring coarse is the safe side: a consumer that dilates by the recorded level
over-includes rather than missing objects.

**What success records, in this order, after the flushes:** the analysis level
of each written layer, then `setStatus(Complete)`, then
`StageRegistry::clearStale(id)`. The level goes first because a stage is
`Complete` only once everything it claims is recorded, and the stale flag is
cleared last because a stage that is `Complete` and still flagged is the
conservative state, while one cleared before it is `Complete` would be the
dangerous one.

**The counters.** `objectsWritten()` and `edgesAsserted()` are the
`emittedCount()` of the run's `ObjectWriter` and the `assertedCount()` of its
`EdgeWriter`, read after the flushes, so they count what was committed. Both
are 0 before `run()` and after a failed run.

**The restriction is resolved per tile, never whole.** `makeLayerMaskSource`
(`01-core.md` 1.9) hands the pipeline an object that answers one tile at a
time: query the source layer for the objects covering this tile, rasterise
them, widen by the source layer's half-step. Nothing about the restriction is
computed here, and no representation of the whole restricted region ever
exists.

An earlier draft did compute one, eagerly, as a polygon:
`spec.restrictToPolygon(dilate(unionOfLayer(ws_, src), halfStep))`. That works
for `tissue`, which unions to two or three rings. It does not survive
`restrictToLayer(cells)`, which unions to ten million disjoint rings — several
GB of geometry built before the first tile is read, against a 2 GB ceiling,
and then a point-in-polygon test against all of it per emitted object. The
union function it called, `unionOfLayer`, was never declared in any document,
which was the signal that the case had not been thought through. It is not
needed and does not exist. `core::dilate(Polygon, double)` was its only other
caller and is gone too (`01-core.md` 1.3).

**Layer binding.** At the gate, under the run slot and before anything is
written, the stage's bound pipeline is validated, so binding errors arrive
before any write. The check is the pipeline's alone (`Pipeline::validate()`,
5.5): for every operation that produces objects, its `targetLayer`, when present
and non-empty, must name one of the bound (that is, the stage's written) layers;
when absent or empty, exactly one layer must be bound; a non-string
`targetLayer` is refused, and so are two operations targeting one layer.
Raster-only operations are not consulted. A failure is raised as a `StageError`
naming the stage (`ErrorContext::stageName`), with the pipeline's error, which
names the operation and the layer, as its `cause`, and the operation name copied
when the original carries one. The stage is left as it was: no status change,
no discard, no stale marking. The runner keeps no copy of the resolution, so
the two cannot disagree.

**Annotation targets are not checked here yet.** No operation in the catalog
names an annotation target layer today, and `storage` refuses an undeclared
layer at emit, which is where the invariant lives. The runner check arrives with
the first operation that takes such a parameter.

**The mask source outlives the run.** `makeLayerMaskSource` borrows the object
store, and the `Pipeline` borrows the mask source, so the runner holds it in
`run()`'s scope across `runInto`. The half-step comes from
`coarsestAnalysisLevel(src)`, which is 0 for a layer written only at full
resolution; a stage that was never run contributes no level, and in that case
the stage is not runnable anyway, as its dependency is incomplete.

**The mask over-includes, never under-includes.** A layer analysed at level 4
has a boundary good to ±16 level-0 pixels (`03-storage.md` 3.3), so masking to
its geometry verbatim would silently drop objects that genuinely lie inside the
region — a nucleus 5 px inside true tissue can fall outside a staircase
boundary with 16-pixel treads. Two things now make the error one-sided, where
polygon dilation used to be the only one:

- The fill rule sets any pixel the geometry touches at all (`01-core.md` 1.17),
  so the mask is conservative to within one level pixel for free. This is the
  whole margin for a `cells` layer written at level 0, where the half-step is 0
  and the old polygon dilation did **nothing** — the case that most needed a
  margin was the case that got none.
- `dilateMask` adds `ceil(halfStep / levelScale)` level pixels on top, which is
  what covers a coarse source layer's staircase.

Over-inclusion is the cheaper error by a wide margin. An extra object at the
margin is visible, measurable and removable downstream; an object never
computed leaves no trace that anything is missing, and the count is simply
wrong with nothing to indicate it.

Note what this means for UR-STG-3's acceptance criterion in the multi-level
case: "produces no objects outside those regions" is satisfied against the
**dilated** mask, which is the region the stage was actually restricted to. A
test asserting against the undilated geometry would fail by design, and should
assert the one-sided property instead — no object of the restricted stage lies
outside the dilated region, and none that lies inside the undilated region is
missing.

Both writers are obtained for `id_` and neither takes a stage per call, so
nothing this stage writes can be attributed to another — the property
`discardStage` and `discardEdgesOfStage` rely on when a re-run replaces this
stage's output (3.5, 3.6, UR-STG-6).

The flush order is not incidental. An edge may name an object still sitting in
an unflushed object batch, so `EdgeWriter::flush` verifies its endpoints exist
and throws if one does not; flushing the objects first is what makes that check
pass for edges the stage legitimately asserted. Getting it backwards surfaces as
a `InvalidArgumentError` at the end of an otherwise complete run, which is
recoverable but wasteful, and the failure path then discards the stage exactly
as any other fault does.

Operation resolution, type and arity checking, and target-layer resolution all
happen inside `Pipeline::validate()` (5.5), which the runner calls at the gate
on the stage's fully bound pipeline, so that any definition error (an
unregistered operation, a binding error, a bad auxiliary binding) names the
stage and is refused before anything is written. The runner's remaining job is
the stage's own: gating on dependencies and staleness, discarding prior output
on re-run, recording state, and handling failure.

What validation at the gate does not see is the sinks: `validate()` does not
consult them, and the raster writer is obtained only after the re-run
preparation, because `RasterRegistry::writerForStage` creates the raster's
directory (3.8). The gate therefore makes the one refusal of that writer that
needs only reads itself (case 4 below: a raster another stage populated). What
remains inside the run, recorded `Failed`, is a second live writer of the
raster, a populated row whose file is missing (writerForStage still refuses it),
and a chain whose output the raster cannot take: `Pipeline` does not expose the
plan's output type, so that check is a follow-up. The object, edge and
annotation writers write nothing until given something.

**Gating comes before anything is written.** `run()` first throws `IoError`
when the workspace is read-only; that check needs no run slot. It then throws
`StageError`, with the stage's name in `ErrorContext.stageName` and in the
message, in five cases, checked in this order:

1. another `StageRunner` of the same workspace is inside `run()`;
2. the stage's record is unreadable (`StageRegistry::isUnreadable`);
3. a dependency is not `Complete`, or is stale: the message names the first
   such dependency in `dependencyAt` order;
4. the stage writes a raster that another stage populated
   (`RasterRegistry::isPopulated` and `writerStage`, reads only): the message
   names the raster and that stage, which alone may write it until it is
   discarded (ruling S47);
5. the stage's bound pipeline cannot be built (an auxiliary source that cannot
   be opened, say) or fails `Pipeline::validate()` (the layer binding below
   among others): the `cause` is the original error, the pipeline's naming the
   operation and the layer, and `ErrorContext.operationName` is copied when the
   original carries it (`validate()` sets it on every refusal of one operation).

An error of the reads the gate makes (the stage's status, its record, its
dependencies, on a closed workspace say) is raised the same way, as a
`StageError` naming the stage with the original as its `cause`, so every error
leaving `run()` but `std::bad_alloc` names the stage.

A stale stage whose dependencies are current is runnable: re-running it is how
it becomes current. The first case is a run slot, a `std::atomic<bool>` in
`Workspace::Impl` taken by a RAII holder (`workspace/detail/run_slot.hpp`) for
the length of `run()` and released on every path out of it, so a second runner
fails fast instead of interleaving two runs' writers. The slot is taken before
the dependency check, so a re-run of a dependency cannot slip in between the
check and this run. Every refusal leaves the stage exactly as it was: no
status change, no discard, no stale marking. The messages name the stage, the
dependency and the operation; the runner adds no slide path to any message,
and a `StageError` that wraps another error carries the original's text
unchanged in its `cause` (UR-STG-10).

`isRerun()` is true when the stage's status at the start of the last `run()`
was not `Pending`: `Complete`, `Failed`, or `Running`. It is read first thing in
`run()`, so a refused run updates it too, and again once the run slot is held;
before any `run()` it answers for the status at construction. `Running` is what a process that
died mid-run leaves behind, and because the workspace has a single writer lock
no live process can own it.

**Layer binding** is where UR-STG-10's "each stage shall declare the layers it
writes" becomes enforceable rather than documentary:

1. For every layer in `writesLayer()`, the runner calls
   `Pipeline::bindLayer(registry.name(id), id)` (5.5), giving the pipeline the
   only name-to-`LayerId` mapping it will have.
2. For every **object-producing** operation — one that declares it emits
   objects — `Pipeline::validate()` reads the `targetLayer` string from its
   parameters. A non-empty name that is not among the bound layers is refused,
   and the runner raises it as a `StageError` naming the stage, with the
   pipeline's message naming the operation and the layer — so a stage that
   declares `nuclei` and runs an operation targeting `cells` is refused at the
   gate, before anything is written, not after an hour of tiles.
3. An empty or absent name on such an operation resolves to the single bound
   layer. Where the stage bound none, or more than one, `validate()` refuses
   instead of choosing — a stage emitting cells and nuclei in one pass must say
   which operation writes which.

Raster-only operations are not consulted, and a stage whose chain produces no
objects may legally declare no layer at all. All three steps run at the gate
(case 5 above): step 1 when the runner binds its `Pipeline`, steps 2 and 3 in
the `validate()` that follows, together with every other check `validate()`
makes, an unregistered operation's among them.

**The stage name travels with the run.** `StageRunner` passes `def.name()` to
`Pipeline::stageName` (5.5), so the `stageName` of every `onProgress` call and
the stage in every `TileError`'s context are the stage's own, which is what lets
the `StageError` it raises name the stage and the tile (UR-EXE-5).

**Whole-level chains need nothing here.** When the chain contains a
`WholeLevel` operation, `Pipeline::validate()` (5.5) lays the run out as a
single tile and checks the level against the memory budget before any pixel is
read. A level that does not fit surfaces as an `InvalidArgumentError` from
`p.validate()` at the gate, which the runner raises as a `StageError` refusal:
the stage has written nothing and is left as it was.

The same `validate()` covers the direct-pipeline path, where no stage performed
step 1: an operation whose target resolves to nothing is refused there too.

That a stage cannot write an undeclared layer is therefore checked twice, in
two modules: by the pipeline against the stage's bound layers, and in `storage`
at `emit` time against the registry (`03-storage.md` 3.3). The second is the
invariant — it holds for GeoJSON import and any future writer — and the first
exists to make the failure arrive before the work does.

**On failure or cancellation** the stage is recorded `Failed`, its objects,
edges, annotations and raster are discarded, and a `StageDiscarded` event is
appended by each discard. The workspace is left in its prior consistent state
and no partial objects are visible to later stages (UR-STG-5). An observer that
had opted into in-flight objects sees the discard event and retracts them
(UR-OBS-4). The exceptions the run can raise, from the re-run preparation, the
tile run or the flushes, leave `run()` as follows (the gate's validation maps
them the same way, but records nothing, as it has changed nothing):

- `CancelledError` is re-thrown as a `CancelledError` carrying the stage name
  (UR-EXE-4).
- A `StageError` whose `stageName` is this stage passes through unchanged: it
  already names its stage. One naming another stage (for instance a nested
  runner's refusal, should it arrive unwrapped) is wrapped like any other
  `ProcessorError`, so the error leaving `run()` always names this stage. The
  gate's own refusals never reach this path; they leave the stage as it was.
- `std::bad_alloc` is re-thrown unchanged (C-6).
- Every other `ProcessorError` becomes a `StageError` with `stageName` the
  stage and `cause` the original's `what()`; the original's operation name and
  tile (`hasTile`, `tileX`, `tileY`) are copied when it has them, so a tile
  fault reads "stage nuclei, operation, tile (X, Y)" (UR-EXE-5).
- An exception that is not a `ProcessorError` is translated as `error.hpp`'s
  exception stance (C-6) requires: `std::system_error` (with
  `std::filesystem::filesystem_error`) and `std::ios_base::failure` become
  `IoError`, any other `std::exception` becomes `InternalError`, each with
  `stageName` the stage and `cause` the original's `what()`; an exception of a
  type that is not a `std::exception` becomes an `InternalError` naming the
  stage, with no cause.

Before any of these leaves `run()` the runner records the failure, best effort:
it abandons the raster writer (dropping it, which abandons its unflushed tiles;
the writers of objects, edges and annotations are already gone by then, and
flush what they hold as they are destroyed, which is why the discards come
after), runs the four discards, and sets the stage `Failed`. Each step is tried
whatever the one before did. A step that itself throws is logged at `Error`,
naming the stage, and the original exception is the one that propagates: a
failure while recording a failure never replaces the failure being reported.
The analysis level is never recorded on failure, and the run slot is released
on every path, so a later `run()` of a runnable stage proceeds.
The re-run preparation below is inside the same guarded region: a failure of
`setStatus(Running)` or of a discard is recorded and raised like any other, so a
stage never stays `Complete` with part of its output gone. The runner adds no
slide path to the messages it builds; a wrapped error's `cause` carries the
original error's text unchanged. If `setStatus(Complete)` or `clearStale` throws after the analysis
level was recorded, the level stays recorded while the stage is `Failed`; the
level is the coarsest any writer used, so keeping it errs on the side of the coarser precision.

**On re-run** — `isRerun()` true — the objects the stage previously wrote, the
edges it previously asserted and the annotations it previously made are all
replaced, not duplicated (UR-STG-6). The order is fixed:

1. call `StageRegistry::markStale` on every stage that depends directly on this
   one (found by scanning the stages' `dependencyAt`), because its inputs are
   about to be replaced whatever this run's outcome; `markStale` is a locking
   public call of the registry and the runner is outside it, so there is no
   re-entrancy;
2. set the stage `Running`;
3. only then discard the prior output with `discardStage`,
   `discardEdgesOfStage`, `discardAnnotationsOfStage` and
   `discardRasterOfStage`.

The order makes the preparation crash-safe. A crash after step 1 leaves the
stage `Complete` with its output intact and its dependents merely stale, which
a re-run of them fixes; a crash after step 2 or during step 3 leaves the stage
`Running`, which the next run treats as a re-run. No point leaves the stage
`Complete` with part of its output gone, or its dependents looking current.
`isRerun()` is decided before step 1. Steps 2 and 3 sit inside the run's guarded
region, so a failure in either is recorded `Failed` as above.

**A stage may read the raster it writes** (`readsRaster` and `writesRaster`
naming one raster); the declaration is legal. The gate opened that raster's
source on its prior file, which a re-run must neither read (the re-run would
see its own earlier output, and two runs over the same input would differ,
against UR-STG-6) nor hold open while step 3 deletes the file (on Windows the
delete would fail). So on a re-run, just before step 3, the runner unbinds and
drops each such source, and after step 3 re-acquires it with
`RasterRegistry::sourceFor` and binds it again: it then reads the raster as it
is after the discard, unpopulated, so as zero. Rebinding resets the pipeline's
plan, and `runInto` validates again.

A stage left `Running` by a dead process is a re-run too (decision 2), so what
that process had flushed is discarded and the next run ends with exactly the
objects of a clean run. A dependent made stale here is refused (`StageError`
naming the dependency) until the dependency completes again; it then runs, and
`clearStale` makes it current.

The discards are separate calls against separate tables because they delete by
different ownership: the first takes this stage's objects *and* every
annotation any stage hung on them, while the third takes this stage's claims
about objects it does not own and leaves those objects untouched. Objects
written by other stages are untouched, including objects this stage had
attached: re-running a `cells` stage that had attached existing nuclei leaves
those nuclei unchanged, and leaves them unattached until the re-run asserts its
edges. Because identifiers derive from geometry (decision 8), a re-run over
unchanged input reproduces the same identifiers, so the replacement is exact.

**On resumability** — v1 resumes only at stage granularity, but execution is
structured around a resumable unit of work: the runner passes everything
through `Pipeline::runInto`, whose unit is the tile, and keeps no per-tile state
of its own in v1 (UR-STG-7). The stage record reserves representation for
per-tile completion, and no signature here would need to change to add
tile-level checkpointing; `run()` would consult the reserved per-tile state and
skip completed units. No schema is added for it now.

Cancellation takes effect promptly, at the next tile boundary, and leaves the
workspace in the state UR-STG-5 requires.

## 4.5 Staleness — `workspace/staleness_graph.hpp`

```cpp
enum class StalenessReason : int32_t {
    None, LayerRead, RasterRead, LayerOrdering, DerivedFrom,
    StageDependency, Reconfigured
};

/// A read-only view over the graph `StageRegistry` derives (4.3). It answers
/// questions and changes nothing; `StageRegistry::markStale` is what acts on
/// the answers.
class StalenessGraph {
public:
    /// Forward: what re-running `stage` would invalidate, and why each one.
    /// The reason belongs to the **pair**, not to the affected stage — one
    /// stage is commonly reached down more than one path.
    int32_t         affectedCount(StageId) const;
    StageId         affectedAt(StageId, int32_t index) const;
    StalenessReason reasonAt(StageId, int32_t index) const;
    const char*     reasonDetailAt(StageId, int32_t index) const;

    /// Reverse: why `stage` is stale now. Zero when it is not stale. For a
    /// `Reconfigured` cause the stage is its own cause, which is reported as
    /// such rather than omitted.
    int32_t         causeCount(StageId) const;
    StageId         causeAt(StageId, int32_t index) const;
    StalenessReason causeReasonAt(StageId, int32_t index) const;
    const char*     causeReasonDetailAt(StageId, int32_t index) const;
};
```

**Thread-safety:** ThreadSafe, under `StageRegistry`'s lock — this is a view
over that registry's derived state, not an independent object. A reference from
`staleness()` stays valid across a `declare`; the graph behind it is recomputed
in place. **Requirements:** UR-STG-8, UR-STG-11.

Propagation follows five paths: the read-write relation over layers, the same
relation over **rasters**, the semantic layer ordering, the derived-from
relation, and any explicit `dependsOn`. Re-running the stage that writes `tissue` marks `cells` and
`nuclei` stale whether or not anything was declared between them, and
re-running a stage that writes a primitive layer marks the semantic layers
derived from it stale (UR-STG-11).

**The five edges (plan C, decision 1).** For stages U and V (U ≠ V) there is
an edge U → V, one per reason, when:

- `LayerRead`: V reads (`readLayerAt`) a layer U writes;
- `RasterRead`: V reads (`readRasterAt`) the raster U writes;
- `LayerOrdering`: V writes a semantic layer strictly above
  (`LayerRegistry::isAbove(descendant, ancestor)`) a layer U writes, so the
  `tissue` writer reaches the `cells` and `nuclei` writers;
- `DerivedFrom`: V reads a *primitive* layer U writes and V writes at least
  one semantic layer; such a pair is reported as `DerivedFrom` instead of
  `LayerRead`, per pair: when V also reads a semantic layer U writes, only
  `DerivedFrom` is reported for U → V. A stage can only assert derived-from
  edges onto objects it has
  read, so a reads-based relation finds every derivation a run can produce;
  storage has no layer-level derived-from relation, and object-level edges
  are not consulted;
- `StageDependency`: V's explicit `dependsOn` names U.

The edges come from the in-memory definitions, never from
`LayerRegistry::writerStageAt`, which keeps a writer a redeclaration dropped.
An unreadable stage contributes its pinned edges with reason
`StageDependency`.

The staleness graph may contain cycles: a `cells` stage that reads `nuclei`,
with `nuclei` below `cells`, reaches the `nuclei` writer by `LayerOrdering` and
is reached from it by `LayerRead`. Only the effective-dependency graph
(`LayerRead`, `RasterRead`, `StageDependency`) must stay acyclic, as in 4.3.
`isRunnable` and the persisted dependency rows are unchanged: `LayerOrdering`
and `DerivedFrom` do not gate running.

**The forward answer (plan C, decision 2).** `affectedCount`, `affectedAt`,
`reasonAt` and `reasonDetailAt` list one entry per edge U → V in the subgraph
reachable from the stage S, with V the affected stage: breadth first from S,
the edges of one source ordered by ascending V and then by reason; a stage
reached down two paths appears twice, and S itself appears only when a cycle
leads back to it. Each detail names the source stage and the artifact, for
example `reads layer "tissue" written by stage "background"` or
`writes layer "nuclei" above layer "tissue" written by stage "background"`.
The accessors take the registry's mutex, and a read-only registry first
follows a live writer. An unknown stage or an index out of range is an
`InvalidArgumentError`. The detail strings live in storage the graph owns,
rebuilt with the edges, and stay valid until the next `declare()` on this
registry, or a read-only refresh that re-reads a writer's changes (every
declare rebuilds them, changed or not); a caller holding one longer copies it.

`RasterRead` is the one genuinely new edge the raster store adds. Every other
declaration in 4.2 reduces to `readsLayer` — `annotatesLayer`,
`restrictToLayer` and `readsLayerAsRaster` all do — so they travel the
`LayerRead` path and needed nothing here. A raster has no layer to reduce to,
so re-running the stage that writes one must reach its readers by an edge of
its own. The reason is reported separately rather than folded into
`LayerRead` because the two name different artifacts, and a person asking why
a stage is stale is better served by "the probability map was rebuilt" than by
a layer that was never involved.

An annotating stage needs no fifth path. `annotatesLayer` implies
`readsLayer` (4.2), so a stage that classifies `cells` is stale by `LayerRead`
the moment the stage writing `cells` re-runs — which is also the moment its
annotations were deleted along with their objects (`03-storage.md` 3.7). The
flag and the data therefore change together, and a stale classifier is one
whose claims are genuinely gone rather than merely suspect.

Three of the four are now the same kind of edge — a relation over layers — and
that is the point of deriving dependencies (4.3) rather than hand-declaring
them. `LayerRead` replaced `StageDependency` as the ordinary reason a stage is
stale, and `StageDependency` remains only for the explicit ordering edges,
which should be rare. A reason is worth reporting precisely because the four
paths reach different stages: `LayerOrdering` catches a `nuclei` stage that
never read `tissue` but sits above it in the hierarchy, and no reads-based
analysis would find it.

**Both directions are specified because both are asked.** `markStale` walks
the forward direction to decide the extent of an invalidation. A person looking
at a workspace asks the reverse — *why* is `cells` stale — and that question
has no answer from the flag alone, which is the whole reason this class reports
reasons rather than booleans. `LayerOrdering` in particular names a
relationship the user never declared and may not remember creating.

**There is no `requireNoStaleDependents`.** An earlier draft carried one,
described as "the UR-STG-8 gate". UR-STG-8 offers two arms — invalidate the
dependents, *or* refuse to proceed until they are invalidated — and this
library takes the first, in `StageRegistry::markStale`. The gate was the other
arm, left in place after the choice was made: nothing in 4.4's `run()` called
it, nothing in 4.7 called it, and the place its acceptance criterion points at
— a consumer querying `cells` and being told it is stale — is served by
`StageStateReader::isStale` from `storage`, which does not link this module and
could not have called it. A method on a permanent ABI that no caller can reach
is removed rather than carried.

UR-STG-8's acceptance therefore reads against the durable flag: re-running
`nuclei` marks a dependent `cells` stale, and a consumer querying `cells`
afterwards sees `isStale` true rather than objects whose parents are gone.

## 4.6 Layer lifecycle — `workspace/layer_lifecycle.hpp`

```cpp
class LayerLifecycle {
public:
    /// Declares a semantic layer. Delegates to the store's registry and
    /// records the declaration in the workspace.
    LayerId declareSemantic(const char* name, LayerId parent);
    LayerId declarePrimitive(const char* name);

    /// Discards every object of a primitive layer. Objects derived from it
    /// remain complete and queryable. Marks nothing stale.
    void drop(LayerId primitiveLayer);

    bool    isDropped(LayerId) const noexcept;
    bool    wasEverPopulated(LayerId) const noexcept;
    int64_t droppedObjectCount(LayerId) const noexcept;
};
```

**Thread-safety:** ThreadSafe. **Requirements:** UR-STG-10, UR-STG-12,
UR-RES-16.

`drop` throws `InvalidArgumentError` for a semantic layer — only primitive
layers are droppable, because only they sit outside the containment hierarchy.

Dropping does not invalidate the objects derived from the layer and does not
mark any semantic layer stale (UR-STG-12). Its acceptance criterion is that
dropping a superpixel layer holding more objects than the reference object
count leaves every semantic layer complete and queryable.

`wasEverPopulated` distinguishes a layer that was dropped from one that never
ran — UR-STG-12 requires the workspace report which, and the two are not the
same state.

## 4.7 Typical sequence

Not API, but the order these pieces are used in, because several constraints
only make sense together:

```cpp
auto ws = Workspace::create("analysis.swp", "slide.svs", 0, cfg);

// Layers are declared up front, before any stage runs (UR-STG-10).
auto pixel  = ws->layers().pixelLayer();
auto tissue = ws->layerLifecycle().declareSemantic("tissue", pixel);
auto cells  = ws->layerLifecycle().declareSemantic("cells",  tissue);
auto nuclei = ws->layerLifecycle().declareSemantic("nuclei", cells);

// What to compute is a value, built the same way whether a stage runs it
// or a direct caller does.
PipelineSpec bgSpec;
bgSpec.operation("segment.tissue_detect", tissueParams);

// One declared layer, so no operation needs to name a target.
StageDefinition bg("background");
bg.pipeline(bgSpec).writesLayer(tissue);
auto bgId = ws->stages().declare(bg);

PipelineSpec nucSpec;
nucSpec.then(makeStainDeconvolution(StainVectors::hematoxylinEosin()))
       .then(makeGaussianBlur(1.2, 0))
       .operation("segment.connected_components", nucParams);

// restrictToLayer implies readsLayer, so the dependency on "background" —
// the stage that writes tissue — is derived. Nothing names it.
StageDefinition nuc("nuclei");
nuc.restrictToLayer(tissue)
   .pipeline(nucSpec).writesLayer(nuclei);
auto nucId = ws->stages().declare(nuc);

StageRunner(*ws, bgId).progress(&observer).run();
StageRunner(*ws, nucId).progress(&observer).run();
```

`nucSpec` mixes both construction paths on purpose: the typed factories give
compile-time parameter checking, `operation` takes what a script or a
deserialised configuration supplies, and the spec records all three entries
identically (5.4).

A stage emitting two layers in one pass names the target on each operation
instead, and the runner checks both against the declaration:

```cpp
OperationParameters cellParams, nucParams;
cellParams.setString("targetLayer", "cells");
nucParams .setString("targetLayer", "nuclei");

PipelineSpec bothSpec;
bothSpec.operation("segment.connected_components", cellParams)
        .operation("segment.connected_components", nucParams);

StageDefinition both("cells_and_nuclei");
both.restrictToLayer(tissue)
    .pipeline(bothSpec)
    .writesLayer(cells).writesLayer(nuclei);
```

Omitting `targetLayer` there throws `StageError` rather than picking one (4.4).

The same `PipelineSpec` runs outside a workspace unchanged — no store, no
stage, no durability — which is the lazy path of UR-EXE-1:

```cpp
Pipeline preview(scene, cfg, nucSpec);
auto previewScene = preview.asScene();   // no pixels computed
```

The `nuclei` stage may legally run before any `cells` stage exists, writing
unattached nuclei that a later `cells` stage attaches by asserting edges
(UR-RES-5). The layer ordering constrains what contains what; it never
constrains what runs when.

That later `cells` stage is the case `readsLayer` exists for. It is restricted
by `tissue` and reads `nuclei` in order to attach them, without confining its
work to them:

```cpp
StageDefinition cel("cells");
cel.restrictToLayer(tissue)     // confines the work — implies readsLayer
   .readsLayer(nuclei)          // consulted, not confining
   .pipeline(cellSpec).writesLayer(cells);
```

Its derived dependencies are both writers: the `background` stage and the
`nuclei` stage. Re-running either marks it stale, and `isRunnable` waits for
both — none of which anyone had to write down.
