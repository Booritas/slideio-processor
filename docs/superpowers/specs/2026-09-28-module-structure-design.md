# SlideIO Processor — Module Structure

**Status:** design approved, pending implementation planning
**Date:** 2026-09-28
**Companions:** [Software Design Specification](sds/README.md) ·
[Observation requirements draft](../../requirements-9a-observation-draft.md)

---

## 1. Purpose

This document fixes the top-level decomposition of `slideio-processor` into
modules: what each owns, what it depends on, and which requirements it carries.
It does not specify class-level APIs, schema DDL, or algorithm internals.

The decomposition is load-bearing beyond internal tidiness. Module boundaries
are also the ABI boundary (UR-API-2), the unit of the Python binding surface
(UR-API-1), and the thing that decides whether the OpenCV 4 to 5 migration is a
configuration change or a redesign (UR-BLD-4). Getting them wrong is expensive
to undo.

## 2. Intended outcome

Seven modules such that:

- A viewer process can observe and query results without linking the
  computational backend.
- The algorithm catalog can grow, shrink, or be split into its own package
  without touching anything above it — which matters while open question 7
  ("what is the v1 catalog, exactly?") remains unresolved.
- The transaction discipline protecting store atomicity exists in exactly one
  implementation, not once per algorithm author.
- OpenCV is confined to one module.

## 3. Decisions

Eight decisions were taken during design. Each is recorded with its reasoning
so it is not relitigated.

| # | Decision | Reasoning |
|---|---|---|
| 1 | **Seven modules**: `core`, `config`, `storage`, `workspace`, `pipeline`, `algorithm`, `reporting` | UR-EXT-4 draws a line between the framework (tiling, halos, memory, threading, progress, cancellation, persistence) and the catalog, which is the product. Section 10 of `requirements.md` defines the workspace as a separate durable concern. Neither fits inside the other five. |
| 2 | **Vocabulary split**: shipped standard vocabulary in `config`, per-store registry in `storage` | UR-RES-17 makes the effective vocabulary a property of the store — one per store, recording the version written against, extensible with local labels at any time. The shipped standard set is genuinely global and versioned independently. They are two different things. |
| 3 | **`reporting` is export-only**; GeoJSON import lives in `storage` | Import is an input, not a report, and UR-RES-19's strict/permissive policy has to run next to the vocabulary registry that enforces it. Duplicate-parser risk is avoided by placing the GeoJSON codec in `core`: `storage` uses the read side, `reporting` the write side. |
| 4 | **Storage API safe from any thread**: concurrent reads, serialised batched writes, thread-confined cursors | UR-EXE-3 has a worker pool emitting objects concurrently; UR-EXE-7 requires batched transactions; UR-QA-3 requires determinism. Serialising writes keeps the transaction discipline in one place. Shared cursors would make per-thread iteration order non-deterministic for no benefit. |
| 5 | **Live visibility at batch-commit granularity**; queries default to completed stages, observers opt in and handle retraction | Satisfies UR-STG-5 by query filter rather than rollback: later stages never see partial objects, but an observer is not a later stage. Avoids a provisional tier and the cost of promoting ten million rows at commit. |
| 6 | **In-store event journal as the source of truth**, with an optional local wake-up signal | Events committed in the same transaction as the data they describe cannot be half-delivered. Gives late join, replay, restart and writer-crash survival without a subscriber list, and makes transport an optimisation rather than an architectural commitment. |
| 7 | **`algorithm` emits through an abstract sink in `core`**, never linking `storage` | A write is an insert into `objects`, an insert into the R\*Tree and an append to `events` in one transaction. With a direct dependency every algorithm author — including third parties under UR-EXT-5 — could get that wrong. With the sink they cannot, because they never see a database. Also makes algorithms unit-testable against an in-memory sink. |
| 8 | **Object ids derived from the anchor pixel** — the topmost-then-leftmost pixel in level-0 slide coordinates | UR-QA-3 requires byte-identical exports across worker counts *and* tile sizes, and ids appear in exports. Any id keyed off tile index or write order is tile-size dependent (see 3.1). The anchor is a property of the geometry alone, so it is invariant by construction and needs no coordination at all. |
| 9 | **Two shared libraries in one Conan package**, split at the OpenCV line, plus a CLI executable | A single library would load OpenCV into the viewer process regardless of the module graph, since the dynamic linker pulls in every `NEEDED` entry. Splitting at the only heavyweight dependency that divides the graph cleanly gives deployment isolation without the versioning cost of a package per module (see section 5). |

### 3.1 Why tile-ordered ids fail

The natural scheme — release tiles to the writer in scan order and assign a
dense ascending counter — delivers worker-count invariance but not tile-size
invariance. Scan order over tiles is not scan order over anchors:

```
Object A anchored at (y=100, x=1500)
Object B anchored at (y=600, x=100)

tile size 512:    A -> tile(0,2)   B -> tile(1,0)
                  (0,2) released before (1,0)   =>  A gets the lower id

tile size 1024:   A -> tile(0,1)   B -> tile(0,0)
                  (0,0) released before (0,1)   =>  B gets the lower id
```

Same slide, same objects, different ids, in violation of UR-QA-3. Packing a
tile index into the id fails identically, since the tile index is itself a
function of tile size.

The anchor pixel is independent of tiling and of threading, and is unique
within a layer because a pixel belongs to one object. Sorting by id is
therefore spatial scan order, which an observing viewer gets for free.

### 3.2 Consequences of decision 8

- **Ids are sparse and large.** `id = (anchor_y << 31) | anchor_x` fits
  SlideIO's `int32` axis bounds inside 64 bits. Ids are opaque numbers, not
  1, 2, 3.
- **Journal events carry `bbox + count`, not an id range** — amending decision
  6's first sketch. Decision 5 already has the observer issuing a spatial query
  per event, so the range was never load-bearing.
- **The store key is composite.** Anchors are unique within a layer, but a cell
  and its nucleus can share a topmost-leftmost pixel, and one layer can be
  filled by two stages. Primary key is `(layer_id, anchor_id)`, and
  `core::ObjectId` is an opaque handle carrying both. Exports render it as two
  columns.
- **Writers stay fully parallel.** No reorder buffer, no frontier, no
  head-of-line stalling. Writes are serialised but not ordered.
- **Re-runs are stable.** Re-running a stage recomputes the same anchors and
  therefore the same ids, which is what UR-STG-6's replace semantics want.

## 4. Module graph

```
                          core                     (leaf: slideio + std only)
                        /   |   \
                       /    |    \
                  config    |     \
                  /   |     |      \
                 /    |     |       \
                /  pipeline |     storage
               /   /     \  |       /   \
              /   /       \ |      /     \
        algorithm          workspace     reporting
         (leaf)             (leaf)        (leaf)
```

| Module | Depends on | Third-party surface |
|---|---|---|
| `core` | — | SlideIO types only |
| `config` | core | — |
| `pipeline` | core, config | **OpenCV**, behind a backend adapter |
| `storage` | core, config | **SQLite** with R\*Tree |
| `algorithm` | core, config, pipeline | via pipeline only |
| `workspace` | core, config, pipeline, storage | — |
| `reporting` | core, config, storage | slideio-converter, Parquet writer |

Three properties this shape exists to produce:

**The viewer links four modules, not seven.** `core` + `config` + `storage` +
`reporting` gives subscription, spatial query, cursors, geometry and export.
No OpenCV, no workspace, no catalog.

This must be a *physical* property, not only a logical one. With a single
shared library the dynamic linker would load OpenCV into the viewer process
whatever the module graph says, and the benefit would be lost entirely. That is
what the two-library split in section 5 exists to deliver.

**`algorithm` is a leaf.** Operations self-register into an `OperationCatalog`
declared in `pipeline`; `workspace` resolves stages against that registry
rather than linking the catalog.

**OpenCV is confined to `pipeline`.** One module, one backend adapter, one
place where the UR-IMG-12 type matrix is generated and where UR-BLD-4's
migration lands.

### 4.1 Dependency rules

1. Dependencies flow strictly down the graph. No module includes a header from
   a module it does not link.
2. `<module>/detail/` is private. No module reaches into another's internals.
   Checked in CI.
3. `core` depends on nothing of ours and holds no state.
4. A new edge is a design change requiring a spec update, not a build fix.

## 5. Physical organisation

**Two shared libraries, one Conan package, one CLI executable.**

The seven modules are unchanged by this. They remain internal CMake targets
with the dependency rules of 4.1; this section only decides where the shared
object boundary falls.

### 5.1 The two libraries

The split follows OpenCV, the only heavyweight dependency that divides the
graph cleanly — and it divides it exactly where the two audiences are.

```
libslideio-processor-data      core  config  storage  reporting
  deps: slideio, SQLite (R*Tree), slideio-converter, parquet
  no OpenCV

libslideio-processor-engine    pipeline  algorithm  workspace
  deps: libslideio-processor-data, OpenCV
```

| Consumer | Links | Loads OpenCV |
|---|---|---|
| Viewer (`slideio-view`) | data | no |
| Python script | data + engine | yes |
| Export or reporting tool | data | no |
| `slideio-process` CLI | data + engine | yes |

A viewer needs subscription, spatial query, cursors, geometry and export. It
needs to *display* results, not produce them — so it has no reason to ship
OpenCV alongside a GUI that never calls into it.

**Consequence for stage state.** A viewer showing "nuclei: complete, cells:
running" must be able to read stage state without linking `engine`. UR-STG-4
already requires stage state to be queryable without running anything, and it
lives in the `stages` table, which `storage` owns. So **reading** stage state
is part of `storage`'s query surface; `workspace` owns only what *runs* or
*invalidates* — `StageRunner`, `RerunPolicy`, `StalenessGraph`,
`LayerLifecycle`.

### 5.2 Packaging

One Conan package with two components, `slideio-processor::data` and
`slideio-processor::engine`. Both libraries version in lockstep, so there is
one semver line (UR-API-6), one release, one licence audit (UR-BLD-6) — and two
link targets, so consumers take only what they need.

Public headers under `include/slideio/processor/<module>/`. Two ABI surfaces,
but versioned together, so there is one compatibility story rather than two.
UR-API-2's pimpl and opaque-handle rules apply at both.

**Additional constraint:** `algorithm` compiles against public headers only.
This makes UR-EXT-2 — "at least one built-in filter is demonstrably
implementable outside the library using only public headers" — true for the
entire catalog by construction rather than for one demonstration filter.

### 5.3 The CLI executable

`slideio-process`, in this repository, built only against public headers. Thin
by design: argument parsing over the public API, no logic of its own.

It earns its place four times over:

1. **It proves UR-API-2 continuously.** A header that leaks an STL type or a
   template into an exported signature breaks the CLI build in CI, on the
   commit that introduced it. Better enforcement than the documented review the
   requirement currently proposes.
2. **It drives the UR-QA-5 scale tests** — reference slide, reference object
   count, pixel count above 2^31 — with no Python dependency in CI.
3. **It supplies the second process** that UR-OBS-1 to UR-OBS-4 cannot be
   tested without. Shell-scripting two CLI invocations is far cheaper than
   orchestrating two Python interpreters.
4. **Users get a way to run a stage without writing code**, which matters for a
   library whose primary users are research software engineers.

### 5.4 Alternatives considered

**One library.** Rejected: the dynamic linker loads every `NEEDED` entry, so a
viewer would pull in OpenCV regardless of the module graph, and the isolation
claimed in section 4 would exist only on paper.

**A library per module.** Rejected: seven ABI surfaces, seven Conan components,
and a semver story where a `core` change ripples through six version numbers —
buying isolation the build graph already provides.

**An engine/catalog split** (framework in one library, catalog in another).
Rejected as the primary line: attractive while open question 7 is open, but it
does not separate OpenCV from the consumer surface, which is the split that
actually pays. The public-headers-only rule of 5.2 keeps it available as a
later subdivision of `engine`.

**A processing daemon or server process.** Rejected: decision 6 exists
specifically to remove the need for one. The journal lets the viewer and the
script coordinate through the store file, with no broker, no server lifecycle
and no backpressure problem. A daemon would reintroduce every failure mode the
journal design avoids, and no requirement calls for it.

## 6. Module specifications

### 6.1 `core` — the shared vocabulary

Types and interfaces. No state, no behaviour beyond what the types need.
Depends only on SlideIO's type headers and the standard library.

| Group | Contents | Requirements |
|---|---|---|
| Errors | `ProcessorError` base with a closed hierarchy: `InvalidArgument`, `UnsupportedDataType`, `ChannelArityError`, `StoreFormatError`, `StageError`, `TileError`, `Cancelled`, `IoError`. Each carries stage, operation, tile coordinates, underlying cause. | UR-API-5, UR-EXE-5 |
| Geometry | `Point64`, `Rect64` (also the bounding-box type), `Polygon` — one or more outer rings plus holes, membership by the even-odd rule, holes not tied to a particular outer (SDS 01 §1.3). All coordinates `int64_t`, level-0. | UR-IMG-2, UR-SEG-4 |
| GeoJSON codec | Reader and writer over geometry plus a property bag. One parser, one emitter, one place where QuPath compatibility is decided. | UR-RES-8, UR-RES-15 |
| Types | `DataType` covering all eleven SlideIO types; `DataTypeSet` as a bitmask for per-algorithm declarations; conversion descriptors with documented rounding and saturation. | UR-IMG-8, UR-IMG-11, UR-IMG-13 |
| Selection | `ChannelSelection` — explicit, ordered, never inferred from index. | UR-IMG-7, UR-ALG-2 |
| Identity | `ObjectId` (opaque, carrying layer plus anchor), `LayerId`, `StageId`, `LabelId` as distinct strong types. | UR-IMG-2, decision 8 |
| Object model | `ObjectRecord` — id, geometry, bbox, layer, producing stage, measurements, optional label. Parent is deliberately not a field. `Measurement` carries name, value and unit in both pixel and calibrated forms. | UR-RES-4, UR-SEG-5, UR-SEG-6 |
| Flow interfaces | `ObjectSink`, `ObjectSource`, `Cursor`. | decision 7, UR-RES-2 |
| Budget | `MemoryBudget` — declared here because UR-EXE-2's ceiling spans raster tiles, object accumulation and store buffers, and `pipeline` does not depend on `storage`. | UR-EXE-2 |
| Raster view | `BlockView` — non-owning contiguous buffer with shape, stride and data type, mappable to NumPy without a copy. | UR-API-3 |
| Control | `Progress`, `CancellationToken`. | UR-EXE-4 |
| Provenance | Source slide id, scene index, library version, per-stage configuration. | UR-RES-11 |
| Log facade | `log(level, ...)` with a pluggable sink, declared here so every module can log and *configured* by `config` — which is why the dependency points that way. | — |

**On UR-API-2.** These types are the public API, and the rule is no templates
and no STL in exported signatures. `core` therefore has two faces: public
pimpl'd handle classes whose accessors return scalars, `const char*`, or fill
caller-supplied buffers; and internal representations using the standard
library freely. `Polygon::ringCount()` / `Polygon::ringPoints(i, buf, cap)` on
the outside, `std::vector` on the inside. The same shape SlideIO already uses,
so it is familiar to users and proven bindable.

### 6.2 `config` — global settings and the shipped vocabulary

| Group | Contents | Requirements |
|---|---|---|
| `Configuration` | Memory ceiling (default 2 GB), worker count (default hardware concurrency, 1 permitted), tile size (default from the slide's native tiling), log file path, log level, event poll interval. | UR-EXE-2, UR-EXE-3, UR-EXE-6 |
| `StandardVocabulary` | Immutable, versioned independently of the store format, compiled in. Label id to string, vocabulary version. | UR-RES-17 |
| `BackendCapabilities` | Which `DataType`s the backend supports, generated at build time from the OpenCV version in use. Input to the UR-IMG-12 matrix. | UR-IMG-12, UR-BLD-4 |
| Logging setup | Installs the sink behind `core`'s facade from the configured path and level. | — |

**`Configuration` is an immutable value object.** A process-wide default exists
for convenience, but `Workspace`, `Pipeline` and `Store` each capture a copy at
construction and never re-read the global. Mutating the global afterwards
affects only objects constructed later.

This is not a style preference. A worker count or tile size changed mid-run
would change behaviour halfway through a stage, breaking UR-QA-3 in a way that
is miserable to diagnose — the same run producing different output depending on
when someone touched a setting. Immutability also makes thread-safety free,
since there is nothing mutable to race on, and lets a test hold two
configurations in one process.

### 6.3 `storage`

Owns the store file and everything durable in it. Implements `core`'s
`ObjectSink`, `ObjectSource` and `Cursor`.

**Schema groups:** `objects`, `object_rtree`, `parent_edges`, `derived_from`,
`layers`, `labels`, `stages`, `provenance`, `events`, `meta`. `meta` carries
the store format version and the vocabulary version; opening a store with an
incompatible format version fails naming both versions.

| Component | Responsibility | Requirements |
|---|---|---|
| `Store` | Open/create, format-version gate, WAL mode, per-thread connections, journal retention. | UR-RES-12, decisions 4 and 6 |
| `LayerRegistry` | The semantic tree rooted at the pixel layer (branching), plus unordered primitive layers. Validates that every object write names a declared layer. Queryable before any stage runs. | UR-STG-10, UR-RES-13 |
| `VocabularyRegistry` | Recorded standard version, workspace-local labels, standard / local / import-registered origin per label, unrecognised-label passthrough with identifier and string intact. | UR-RES-17, UR-RES-18 |
| `StoreSink` | The single write path. Batches objects and appends the journal row in one transaction. Serialises writes arriving from any thread. | UR-EXE-7, decisions 4 and 6 |
| `QueryBuilder` + `Cursor` | Filters: layer, stage, spatial rect/polygon, label, unlabelled, unattached, measurement predicate; completed-stages-only default with `includeInFlight()` opt-in. Forward-only, bounded memory. | UR-RES-2, UR-RES-3, UR-RES-4, UR-RES-5, decision 5 |
| `EdgeStore` | Parent edges as (child, parent, asserting stage), validating both endpoints semantic and the parent in a strictly lower layer. Derived-from many-to-many, valid when empty. | UR-RES-5, UR-RES-14, UR-RES-16 |
| `EventJournal` + `EventCursor` | Append-only sequenced events; cursor over `seq`; optional local wake-up signal. | decision 6, UR-OBS-1 to UR-OBS-7 |
| `GeoJsonImporter` | Reads via `core`'s codec, applies the strict/permissive policy, registers import-originated labels. | UR-RES-8, UR-RES-19, decision 3 |
| `StageStateReader` | Read-only access to stage names, states, configurations and completion times. Read side only — running and invalidating belong to `workspace`. Placed here so an observer can report stage state without linking `engine` (see 5.1). | UR-STG-4 |

**Why layer validation is here but layer declaration is in `workspace`.** The
hierarchy constrains edges the store must reject no matter who writes them —
including GeoJSON import and any future external tool. The rule therefore lives
with the data. `workspace` supplies the declaration API and the stage-to-layer
bindings on top.

#### 6.3.1 The event journal

Events are not messages sent to subscribers. They are rows appended to a table
inside the store, in the same transaction as the data they describe:

```
BEGIN
  INSERT INTO objects  ... (one UR-EXE-7 batch)
  INSERT INTO object_rtree ...
  INSERT INTO events   ... (one row, seq = N)
COMMIT
```

Because the event row and the objects it announces commit or fail together, a
subscriber can never see an event for objects that are not queryable, and
objects can never appear unannounced. There is no separate publish step that
can be interrupted.

Subscription is a cursor over `seq`, exactly like the object cursors. A
subscriber holds one integer and asks for events above it. That single property
produces late join, replay from any point, resumption after a subscriber
restart, survival of a writer crash, and any number of independent subscribers
the writer knows nothing about. A subscriber falling behind a fast writer
causes no backpressure and no drops; its next query simply returns more rows,
which it can coalesce.

Events carry enough to decide whether to look further — kind, stage, layer,
count, union bounding box — and not the geometry itself. An observer whose
viewport misses the bounding box does nothing; one that intersects it issues an
ordinary spatial query. The journal stays small and the expensive path stays
the one already designed.

### 6.4 `workspace`

Owns stage semantics. Holds no schema of its own; reads and writes through
`storage`.

| Component | Responsibility | Requirements |
|---|---|---|
| `Workspace` | Binds slide, scene, store and stage states; openable across processes and restarts. | UR-STG-1, UR-STG-9 |
| `StageDefinition` | Name, declared dependencies, declared written layers. | UR-STG-2, UR-STG-10 |
| `StageState` | pending / running / complete / failed, plus stale; configuration and completion time. Writes and transitions only — the read side is `storage::StageStateReader`, so an observer does not need this module (see 5.1). | UR-STG-4 |
| `StageRunner` | Resolves operations from `pipeline`'s registry, wires `StoreSink`, owns commit boundaries, progress and cancellation. Structured around a resumable unit of work even though v1 resumes only at stage granularity. | UR-STG-5, UR-STG-7, UR-EXE-4 |
| `RerunPolicy` | Replaces the objects a stage wrote and the edges it asserted; leaves other stages' objects untouched, including ones it had attached. | UR-STG-6 |
| `StalenessGraph` | Propagates along declared dependencies, the semantic layer ordering, and the derived-from relation. | UR-STG-8, UR-STG-11 |
| `LayerLifecycle` | Declares layers up front; drops primitive layers, recording retained versus dropped, without invalidating derived objects. | UR-STG-10, UR-STG-12 |

### 6.5 `pipeline` — the framework

Everything UR-EXT-4 promises to supply so operation authors write only per-tile
computation. The only module linking OpenCV.

| Component | Responsibility | Requirements |
|---|---|---|
| `Operation` | The public extension interface. Declares halo, channel arity, supported `DataTypeSet` and parameters; implements per-tile `apply`. Built-ins use this and nothing more. | UR-EXT-1, UR-EXT-2, UR-EXT-3 |
| `OperationCatalog` | Registry algorithms self-register into, so `workspace` resolves stages without linking `algorithm`. | section 4 |
| `TileGrid` | Tiling scheme, halo computation and supply at seams, ROI restriction, default tile size from the slide's native tiling. | UR-EXT-3, UR-IMG-5, UR-EXE-6 |
| `LevelSelector` | Reads from the coarsest pyramid level satisfying the requested resolution, never level 0 plus downsample. | UR-IMG-4 |
| `PipelineSpec` | What to compute, as a serialisable value: channels, ROI, and the operation chain as names plus parameter bags. No scene, store or budget. Round-trips through JSON, so a recorded stage configuration replays. Shared by `Pipeline` and `workspace::StageDefinition`, which is what keeps one builder rather than two. | UR-STG-4, UR-RES-11 |
| `LazyPipeline` | Composition evaluated per requested block, exposed as a `Scene` so a pipeline's output is a valid input. Construction computes no pixels. | UR-EXE-1, UR-PIX-6, UR-ALG-5 |
| `EagerRunner` | Streams the whole slide or ROI for global-context work and persisted results. | UR-EXE-1 |
| `MemoryBudgetEnforcer` | Honours `core::MemoryBudget` across raster tiles, object accumulation and store buffers together. | UR-EXE-2 |
| `WorkerPool` | Configurable, defaults to hardware concurrency, 1 permitted. | UR-EXE-3 |
| `Reduction` | Two-pass accumulate-then-apply for operations needing global context, such as streamed Otsu. | UR-PIX-5 |
| `SeamMerger` | Union-find over tile-seam fragments, producing objects identical to single-tile processing. Anchor-keyed, so merge order cannot affect ids. | **UR-SEG-3** |
| `Backend` | The OpenCV adapter. Type mapping, capability table, and the single place the 4-to-5 migration touches. | UR-BLD-3, UR-BLD-4, UR-IMG-12 |
| `Contracts` | Enforces declared types and arity before a run starts, raising errors naming the algorithm and the offending type or channels. | UR-IMG-11, UR-ALG-3, UR-ALG-4 |

**On `Reduction` and floating point.** Integer accumulators — histogram bins,
pixel counts, areas — combine order-independently, so worker count cannot
affect them. Floating-point accumulators must combine in canonical tile order,
because float addition is not associative and UR-QA-3 would otherwise fail
intermittently and irreproducibly across worker counts. This is a constraint on
every reduction in the catalog, not a property of any one algorithm, which is
why it lives in the framework.

**On `SeamMerger`.** This is the component that decides whether the library is
correct or merely plausible. UR-SEG-3 is called out in the requirements as the
hardest requirement in the document and the most common failure mode in tiled
WSI processing. It gets a dedicated test suite asserting identical object
counts and measurements across at least three tile sizes, not incidental
coverage.

### 6.6 `algorithm` — the catalog

A leaf, compiled against public headers only.

```
algorithm/
  pixel/     colour spaces (RGB, grey, HSV, Lab)           UR-PIX-1
             stain deconvolution (H&E, H-DAB, custom)      UR-PIX-2
             filters (Gaussian, median, Sobel, Scharr,
                      Laplacian, Canny, bilateral)         UR-PIX-3
             morphology (erode, dilate, open, close)       UR-PIX-4
             thresholding (fixed, Otsu, adaptive)          UR-PIX-5
             type conversion                               UR-IMG-13
  segment/   tissue versus background                      UR-SEG-1
             connected components                          UR-SEG-2
             contour extraction with holes                 UR-SEG-4
  measure/   area, perimeter, centroid, bbox,
             per-channel intensity statistics              UR-SEG-5
             physical-unit calibration                     UR-SEG-6
             extraction-time predicates                    UR-SEG-7
```

Every entry declares name, parameters, channel arity, supported types, halo,
and a documented correctness reference with a test asserting agreement within a
stated tolerance (UR-ALG-6). That last one matters here specifically because
the catalog takes no numerical compatibility with OpenCV and so inherits no
correctness proof from it. These declarations are the source for the generated
UR-IMG-12 support matrix, so published documentation cannot drift from runtime
behaviour.

Open question 7 remains open. This structure is what makes deferring it safe:
the catalog is a leaf behind a registry, so its contents change nothing above
it.

### 6.7 `reporting`

Export-only. Depends on `core`, `config`, `storage` — never on `pipeline`,
`algorithm` or `workspace`.

| Component | Responsibility | Requirements |
|---|---|---|
| `CsvWriter` | One row per object, documented column schema, loads in pandas. | UR-RES-9 |
| `ParquetWriter` | Binary columnar for result sets impractical as CSV. | UR-RES-10 |
| `GeoJsonWriter` | Streaming export via `core`'s codec, QuPath-compatible. | UR-RES-8 |
| `MaskWriter` | Label and probability maps to pyramidal TIFF via slideio-converter. Takes a `Scene`, hence no `pipeline` dependency. | UR-RES-7 |

Three rules across all four: every writer is driven by a `Cursor` over a query
and never materialises a result set; every export carries provenance
recoverable from the store alone; every export marks each label's origin as
standard or workspace-local and names the object's layer.

## 7. Cross-cutting concerns

### 7.1 Three independent version lines

| Version | Governs | Bumped when |
|---|---|---|
| Library semver | Public API compatibility | API changes |
| Store format | Whether a store can be opened | Schema changes |
| Vocabulary | Which labels are recognised | Standard labels added |

UR-RES-12, UR-RES-17 and UR-API-6 treat these separately and it would be a
mistake to collapse them. Adding a standard label must not make old stores
unopenable; a schema change must not force a major API version.

### 7.2 Thread-safety by module

| Module | Guarantee |
|---|---|
| `core` | Value-like types, thread-compatible: concurrent reads safe, no shared mutation |
| `config` | Immutable after construction — free |
| `storage` | Fully thread-safe: concurrent reads, serialised batched writes, thread-confined cursors |
| `pipeline` | Safe for concurrent execution across workers |
| `algorithm` | Operations must be reentrant; enforced by the `Operation` contract |
| `workspace` | Single-writer, matching Assumption 6 |
| `reporting` | Thread-compatible; one writer per export |

### 7.3 Determinism

UR-QA-3 requires byte-identical results across {1, 4, 8} workers and
{512, 1024, 4096} tile sizes. Four mechanisms deliver it:

1. Anchor-derived ids (decision 8) — invariant to both by construction.
2. Integer reductions, which combine order-independently.
3. Float reductions combining in canonical tile order.
4. `SeamMerger` results independent of merge order.

### 7.4 Binding surface

Pimpl and opaque handles at the library edge (UR-API-2); `Cursor` and
`EventCursor` shaped as iterators expressible in Python without materialising
the underlying result set (UR-API-4); `BlockView` mappable to NumPy without a
copy (UR-API-3).

### 7.5 Observation and privacy

The notification transport is local to the machine — a named semaphore on
Windows, an eventfd or Unix domain socket on POSIX — scoped by OS permissions.
The library opens no network socket.

This is deliberate. Whole-slide images routinely carry patient identifiers in
their metadata, and a localhost listener opened as a side effect of subscribing
to events is an exposure that costs nothing to avoid. Correctness never depends
on the signal: a subscriber that only polls observes every event, so the
transport can be omitted entirely on any platform.

## 8. Module to requirement traceability

| Module | Requirement groups |
|---|---|
| `core` | UR-IMG-2, UR-SEG-4, UR-SEG-5, UR-RES-4, UR-API-2, UR-API-3, UR-API-5 |
| `config` | UR-EXE-2, UR-EXE-3, UR-EXE-6, UR-RES-17, UR-IMG-12, UR-BLD-4 |
| `storage` | UR-RES-1 to UR-RES-6, UR-RES-8, UR-RES-11 to UR-RES-14, UR-RES-16 to UR-RES-19, UR-STG-4 (read side), UR-BLD-5, UR-OBS-1 to UR-OBS-7 |
| `pipeline` | UR-IMG-4, UR-IMG-5, UR-IMG-11, UR-EXE-1 to UR-EXE-6, UR-EXT-1 to UR-EXT-4, **UR-SEG-3** |
| `algorithm` | UR-PIX-1 to UR-PIX-6, UR-SEG-1, UR-SEG-2, UR-SEG-4 to UR-SEG-7, UR-ALG-1 to UR-ALG-8 |
| `workspace` | UR-STG-1 to UR-STG-12, UR-EXE-4 |
| `reporting` | UR-RES-7 to UR-RES-10, UR-RES-15 |

## 9. Consequences for `requirements.md`

The observer scenario is not covered by the current requirements document. A
draft new section and two amendments are in
[requirements-9a-observation-draft.md](../../requirements-9a-observation-draft.md),
for review and merge:

- **New section 9a**, UR-OBS-1 to UR-OBS-7.
- **Amendment to section 15**, which currently records concurrent readers
  alongside a writer as "desirable but not required in v1". UR-OBS-1 promotes
  that to required. Concurrent *writers* remain a non-goal.
- **Amendment to UR-STG-5**, whose "no partial objects visible to later stages"
  needs a note that an observer is not a later stage, with visibility governed
  by UR-OBS-4.

## 10. Open items

| # | Item | Owner |
|---|---|---|
| 1 | `core::ObjectId` as an opaque composite handle versus a single wide integer. Recorded as a consequence of decision 8, not re-opened. | design |
| 2 | ~~Requirements section 9a to be reviewed and merged.~~ **Done** — merged into `requirements.md` v0.2 on 2026-09-29 as §9a, with amendments A1–A3. | requirements |
| 3 | Open questions 3 and 4 in `requirements.md` remain load-bearing and unresolved: whether float intermediates fit SlideIO's `Scene` channel-type model, and whether SQLite with R\*Tree sustains the reference object count. Both should be spiked before implementation, as the requirements document itself notes. | implementation |
| 4 | Open question 7 — the v1 catalog contents — stays open by design. | product |

## 11. Not decided here

Class-level APIs, schema DDL, the tiling algorithm, the union-find
representation in `SeamMerger`, algorithm internals, the CLI's command surface,
and the internal CMake target layout within each of the two libraries. These
belong in the implementation plan.
