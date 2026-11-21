# SlideIO Processor

## Goal

An open-source, cross-platform C++ library providing **a catalog of
image-processing algorithms for images larger than memory** — in the spirit of
OpenCV, but where every algorithm is scale-aware by construction.

The user picks algorithms and composes them; the library handles tiling, halos,
memory budgeting, threading and persistence. An algorithm behaves the same on a
1,000-pixel thumbnail as on a 10-billion-pixel slide, and the caller writes the
same code either way.

The library builds on the [SlideIO](https://github.com/Booritas/slideio) reader
library, which supplies format decoding, zoom pyramids and the `Scene` raster
abstraction. `slideio-processor` is a separate project, versioned and released
independently.

**Research use only. Not for diagnostic use.**

## No fixed limits

The library imposes no limit of its own on image dimensions, pixel count,
channel count, or object count. Any limit is one inherited from SlideIO, the
platform, or available storage.

All eleven of SlideIO's channel data types are supported — `Byte`, `Int8`,
`UInt16`, `Int16`, `UInt32`, `Int32`, `UInt64`, `Int64`, `Float16`, `Float32`,
`Float64` — including scenes whose channels differ in type from one another.
Restrictions belong to individual algorithms, never to the library.

The figures below are **verification anchors, not ceilings**: they fix what the
test suite must demonstrate, and nothing in the design may treat them as bounds.

| Anchor | Value |
|---|---|
| **Reference slide** | 100,000 x 80,000 px, 3 x 8-bit channels (~24 GB uncompressed, 8x10^9 px) |
| **Reference object count** | 10 million objects in a single stage |
| **Memory ceiling** | Configurable, independent of both — default 2 GB |

## Key decisions

| Decision | Choice |
|---|---|
| **Relationship to SlideIO** | Separate library depending on SlideIO's public reader API, plus one additive contribution to it: the scene-source seam that makes a computed scene constructible from outside the tree (see *Raster execution*). No SlideIO internal header is consumed. Until a SlideIO release carries the seam, SlideIO is a git submodule (`extern/slideio`) pinned to a commit, installed locally and found with `find_package(slideio)` as a released package would be. |
| **What the library is** | A catalog of algorithms. The framework supplies scale; the catalog is the product. |
| **OpenCV compatibility** | Same spirit only — own names, parameters and numerics. Each algorithm therefore carries its own documented correctness reference. |
| **Channel contract** | The caller selects channels. Each algorithm declares its arity: *per-channel* (independent, mixed types always fine) or *joint* (may require uniform types, and must say so). |
| **Arithmetic** | 64-bit throughout for pixel counts, areas, indices and accumulators — 32-bit overflows at 2.1x10^9, which one reference slide already exceeds. |
| **Pixel types** | All 11 SlideIO types carried losslessly. Support declared per algorithm and matrix generated from the backend in use. |
| **Backend** | OpenCV 4 now, **migrating to OpenCV 5** when it reaches the Conan server — which closes the `UInt32`/`Int64`/`UInt64` gap with no requirement or API change. |
| **Raster execution** | Lazy pipeline exposed as a `Scene`, plus an explicit eager pass for global-context work. Reaching this required a **public extension seam in SlideIO** — `ISceneSource`, `SceneMetadata`, `makeScene` — because `Scene` is constructible only from a `CVScene`, whose header is deliberately not installed. The seam keeps OpenCV out of SlideIO's public headers, so the two projects stay independently releasable (open question 3). |
| **Object results** | Persistent, spatially-indexed store (SQLite + R\*Tree); read through streaming cursors, never materialised whole. Object identity is the geometry-derived anchor; the table is physically ordered by a monotonic rowid, because keying it by the anchor costs 4.1× on insert. |
| **Performance** | 30 minutes for segmentation and classification of the reference slide at the reference object count, on a named reference configuration (UR-EXE-8). One slide per workspace, so batch runs as independent single-writer stores. |
| **Multi-stage** | A durable *workspace* holds the object store and per-stage state. Stages are named, declare dependencies, and are individually re-runnable. |
| **Interruption** | Durable at stage boundaries in v1; format and execution model designed so tile-level checkpointing can be added without a breaking change. |
| **Object relations** | Parent/child, so stages attach to earlier stages (nucleus to cell to tissue region). |
| **Layers** | Objects belong to declared **layers**. Semantic layers — pixel layer, tissue, cells, nuclei — form a branching containment hierarchy and are the only participants in parent edges; primitive layers (superpixels, basins, fragments) are arbitrary, unordered, droppable, and linked to semantic objects by a many-to-many derived-from relation. |
| **Class labels** | A shipped standard vocabulary, documented and versioned; a workspace may add labels of its own at any time, marked as local wherever they are reported. |
| **Interop** | GeoJSON (QuPath-compatible), CSV/Parquet tables, pyramidal masks. |
| **Extensibility** | Public `Operation` interface with explicit halo, channel-arity and data-type contracts. |
| **Python** | Binding-friendly C++ API; bindings ship in a separate repository. |

## Documents

- [User Requirements](requirements.md) — the full specification: 90 numbered,
  testable requirements with acceptance criteria, non-goals, assumptions and
  open questions.
- [Module Structure](superpowers/specs/2026-09-28-module-structure-design.md) —
  the seven-module decomposition, its dependency rules, and the nine design
  decisions behind it.
- [Software Design Specification](superpowers/specs/sds/README.md) — the public
  API of all seven modules, one document per module plus shared conventions,
  with [class diagrams](superpowers/specs/sds/08-class-diagrams.md) of all
  seven and of the dependencies between them.
- [Observation requirements (draft)](requirements-9a-observation-draft.md) —
  UR-OBS-1..7 for live observation of a running analysis by a separate viewer
  process, plus two amendments to `requirements.md`. **Merged** into
  `requirements.md` v0.2 as §9a; the draft is kept only for its traceability table.
- [Scene source design](superpowers/specs/2026-10-08-scene-source-design.md) —
  the SlideIO addition (`ISceneSource`, `SceneMetadata`, `makeScene`) that makes
  "lazy pipeline exposed as a `Scene`" achievable, and the `Pipeline::asScene()`
  implementation on top of it. Resolves open question 3.
- [Scene source implementation plan](superpowers/plans/2026-10-08-scene-source.md) —
  eight TDD tasks delivering the SlideIO half, **delivered** as SlideIO PR #86
  (merged 2026-10-17). Excludes `Pipeline::asScene()`,
  which waited on the `pipeline` module existing; `Pipeline::asScene()` is
  **delivered** by pipeline plan C, built on that seam.
- [Storage implementation plan A](superpowers/plans/2026-10-20-storage-a.md) —
  plan A of three for the `storage` module: the SQLite store file and its schema,
  the layer and vocabulary registries, the object writer and queries, stage
  state and the event journal with cross-process observation. Edges, annotations,
  GeoJSON import and rasters follow in plans B and C. **Delivered** as PR #7
  (merged 2026-11-03).
- [Storage implementation plan B](superpowers/plans/2026-11-03-storage-b.md) —
  plan B of three for the `storage` module: parent and derived-from edges, label and
  measurement annotations, the effective-label raster, primitive-layer drop and
  GeoJSON import. **Delivered** with plan C as PR #8 (merged 2026-11-16).
- [Storage implementation plan C](superpowers/plans/2026-11-03-storage-c.md) —
  plan C of three for the `storage` module: persisted rasters (declaration, writer,
  registry, pyramidal BigTIFF files through libtiff), their journal events, the
  unreferenced-file sweep, and the freeze of store format v1. Completes the module.
  **Delivered** as PR #8 (merged 2026-11-16).
- [Pipeline implementation plan A](superpowers/plans/2026-11-16-pipeline-a.md) —
  plan A of three for the `pipeline` module: the engine library and the OpenCV
  backend adapter, the operation contract, parameters and catalog with its support
  matrix, `PipelineSpec` and its JSON record, level selection and the tile grid, and
  reductions. Adopts the whole-level operations proposal. **Delivered** as PR #9
  (merged 2026-11-16).
- [Pipeline implementation plan B](superpowers/plans/2026-11-16-pipeline-b.md) —
  plan B of three: `Pipeline` validation, planar block reads, per-tile chain
  evaluation, the worker pool and tile executor, restrictions, sinks, auxiliary
  rasters and the two-pass global context. **Delivered** as PR #10 (merged
  2026-11-17).
- [Pipeline implementation plan C](superpowers/plans/2026-11-16-pipeline-c.md) —
  plan C of three: `SeamMerger` (seam joining, incremental close, spilling that
  never waits for memory), the UR-SEG-3 tiling-invariance suite,
  `Pipeline::asScene()` with nested pipelines sharing one budget, and the
  `pipeline_all.hpp` umbrella header. Completes the module. **Delivered** as
  PR #11 (merged 2026-11-18).
- [Algorithm implementation plan A](superpowers/plans/2026-11-18-algorithm-a.md) —
  plan A of three for the `algorithm` module: the module and its registry, and
  the point operations — fixed, Otsu and adaptive thresholds, explicit type
  conversion, colour space conversion and stain deconvolution. Filters and
  morphology follow in plan B, segmentation and measurements in plan C.
  **Delivered** as PR #12 (merged 2026-11-18).
- [Algorithm implementation plan B](superpowers/plans/2026-11-18-algorithm-b.md) —
  plan B of three: the eleven filter and morphology operations — Gaussian,
  median, Sobel, Scharr, Laplacian, Canny with its declared hysteresis bound,
  bilateral, and erode, dilate, open and close with structuring elements.
  **Delivered** as PR #13 (merged 2026-11-18).
- [Operation working set](superpowers/plans/2026-11-18-operation-working-set.md) —
  every operation declares the working memory one tile needs beyond the
  framework's buffers, and the per-tile memory reservation counts it, so the
  memory ceiling bounds operations' own scratch too. Precedes algorithm plan C.
  Its follow-up, [accumulation state and sequential OpenCV](superpowers/plans/2026-11-18-accumulation-state-and-sequential-opencv.md),
  makes global-context operations declare the state they accumulate, which the
  run reserves and each accumulation pass bounds, and runs the engine's OpenCV
  single-threaded, so Canny's and morphology's working sets no longer depend on
  the host's core count.
  **Delivered** as PR #14 (merged 2026-11-18).
- [Algorithm implementation plan C](superpowers/plans/2026-11-18-algorithm-c.md) —
  plan C of three: connected components, contours, marker-controlled watershed
  and tissue detection, with per-object measurements in pixel and calibrated
  units; objects pass through their producer's finishing step, operations see
  the scene's pixel size, and the merger joins 8-connected objects at tile
  corners and along seams, and joins fragments only within one join key, so
  regions touching across a seam stay apart. The segmenters measure at most
  three channels besides the one they segment. The catalog closes at
  twenty-one operations. Segmenters measure the scene's samples
  (`OperationContext::sourceInput()`), not planes earlier steps transformed.
  **Delivered** as PR #15 (merged 2026-11-19).
- **Plan C review follow-up** — the PR #15 review findings deferred to one PR
  after #15 merges, each a spec or public-API change rather than a defect:
  1. `segment.connected_components` and `segment.contours` declare a halo of
     1 they never read; drop it, amending SDS 06 and re-checking the working
     sets and tiling tests.
  2. The level-0 pixel-size rule exists twice (`detail::levelZeroPixelSizeMicrons`
     and `scenePixelSizeMicrons` in `measure.cpp`); move it to one helper in
     core.
  3. The tile filter avoids a copy only when its target is the run's
     `FinishingSink`, found by `dynamic_cast`; give `ObjectSink` an
     ownership-taking emit instead.
  4. `FinishingSink` copies every record even when the producer keeps the
     default `finishObject()`; let an operation declare that it has no
     finishing step, so the copy is skipped.
  5. Seam edge contacts (`SeamEdgeContact`) stay in memory after their
     fragment is spilled but are not charged to the budget, as seam points now
     are.
  6. Joining scans every waiting edge contact of a tile pair under the
     merger's lock; index them by seam line and position.
  The `ObjectSink` and `Operation` vtables change: the new virtuals are
  appended after the existing ones, acceptable before the first release, as
  there are no published binaries.
  **Implemented** and merged into `main` (1d03935) without a PR; see
  [the plan](superpowers/plans/2026-11-19-plan-c-followup.md), whose Rulings
  section records the deviations.
- [Whole-level operations](superpowers/specs/2026-10-11-whole-level-operations.md)
  — an operation cannot declare that it needs the analysis level in one block,
  and §6.7's coarse watershed already depends on getting it. Proposes
  `tileRequirement()`, a declared per-pixel working set, and enforcement in
  `Pipeline::validate()`. **Adopted** with pipeline plan A, which amends SDS 05
  and 08; the enforcement lands in plan B, and a whole-level chain is forced to
  one tile.
- [Workspace implementation plan A](superpowers/plans/2026-11-19-workspace-a.md) —
  plan A of three for the `workspace` module: `Workspace` (the directory binding one
  slide scene to a store, the writer lock, the read-only open), `LayerLifecycle`,
  `StageDefinition` with its durable record, and `StageRegistry` with derived
  dependencies, redeclaration, staleness along the effective dependencies and
  restore at open. Plan B is `StageRunner` (SDS 04 §4.4); plan C is
  `StalenessGraph` (§4.5, adding the layer-ordering and derived-from paths and
  `StageRegistry::staleness()`) and `declareCoarseToFine`.
  **Implemented**; merged and pushed to `main` (48a2955).
- [Workspace implementation plan B](superpowers/plans/2026-11-20-workspace-b.md) —
  plan B of three for the `workspace` module: `StageRunner` (SDS 04 §4.4). It
  gates a run before writing anything (read-only, one runner per workspace,
  unreadable record, incomplete or stale dependency, layer binding), binds the
  layer restriction and the auxiliary and stored rasters, flushes objects,
  edges, annotations and the raster in that order, and records the analysis
  level, `Complete` and the cleared stale flag on success. On failure or
  cancellation it discards the stage's output and records `Failed`, best
  effort, so the run's own exception is the one that propagates; on a re-run,
  including a stage left `Running` by a dead process, it marks the direct
  dependents stale, sets the stage `Running`, and only then discards the prior
  output. Annotation-target checks are deferred (decision 7).
  **Implemented**; merged to `main` (merge commit 491e2a9).
  - **Resolved (test seams): the test-only seams were exported from release builds.**
    The CMake option `SLIDEIO_PROCESSOR_TEST_SEAMS` (default ON) compiles the
    `detail::*ForTesting` functions, `detail::trackForCloseTesting` and the branches
    that read them out of a build made with `./build.sh --no-test-seams`;
    `scripts/check-no-test-seams.sh` checks the export tables (SDS 00 C-15). Six more
    engine functions only tests call across the library boundary are exported only with
    the option ON (`SLIDEIO_PROCESSOR_ENGINE_TEST_EXPORTS`), and the check covers them.
- [Workspace implementation plan C](superpowers/plans/2026-11-20-workspace-c.md) —
  plan C of three for the `workspace` module: `StalenessGraph` (SDS 04 §4.5) over
  five edge paths (layer read, raster read, layer ordering, derived-from, explicit
  dependency) with forward answers and durable reverse causes,
  `StageRegistry::staleness()` and `markAffectedStale` (re-run step 1 now follows
  all five paths), a `dependsOn` that may name a stage declared later
  (`unresolvedDependencyCount/At`), `declareCoarseToFine`, and the follow-ups of
  plans A and B (a read-only refresh never shrinks to empty and keeps the pins of a
  still-unreadable stage, `IoError` from `markStale` and `clearStale` on a
  read-only registry, the operation name on type and arity errors, a disc in the
  dilation band of the coarse-restriction test). The final review made the
  staleness graph acyclic (a layer-ordering edge that would close a cycle is
  suppressed), so re-running every stale stage settles.
  **Implemented**; merged to `main` via PR #17 (merge commit 8e0b2f3).
  - **Resolved (mask-source restrictions): restricted objects crossing into a
    skipped tile were stored truncated.** A pre-existing pipeline defect the
    final review found: `RestrictionFilter::skips` skipped a tile with no mask
    pixel, and the seam merger closed the component without that tile's
    fragments. Fixed for restrictions whose grid spans the scene (mask sources,
    such as the workspace restrict-to-layer path) by the
    [restricted-object truncation plan](superpowers/plans/2026-11-20-restriction-truncation.md):
    a skipped tile is deferred, and after the final pass each deferred tile an
    open object reaching out of an evaluated tile expects is evaluated on demand
    (SDS 05 §5.5, "The tile mask"). The ROI and polygon edge route was fixed afterwards:
    see the next entry. Cost: UR-IMG-5's "reads only intersecting tiles"
    is now a strong tendency, not a guarantee. Reproducer
    `StageRunner.RestrictedObjectCrossingIntoASkippedTileIsNotTruncated` is
    enabled. **Effect on stored results (QA/regulatory):** the object
    identifiers of affected objects change, because a whole object's anchor
    differs from the truncated one's; counts can fall, because a truncated
    object admitted by its old anchor is dropped when the whole object's anchor
    lies in a skipped tile; and results previously stored by restricted stages
    are affected and are not marked stale. Re-run restricted stages and record
    the change in the verification evidence.
  - **Resolved (ROI and polygon restrictions): an object crossing the ROI or
    polygon grid-region edge was stored truncated.** The same defect as the
    mask-source one, on the route the first fix left open: the tile grid spanned
    the ROI's or the polygon's bounding box, so the region's outer boundary was
    not a seam and an object anchored inside that crossed the edge was stored
    cut there, or an object whose true anchor lay outside was stored with the
    cut part's anchor. Fixed by the
    [ROI and polygon edge truncation plan](superpowers/plans/2026-11-20-roi-edge-truncation.md):
    an object-producing tiled run's grid spans the level, the region becomes a
    `RunPlan` field bounding `rect`, the raster written and accumulation, tiles
    outside the region are skipped without a mask, and the deferred-tile
    machinery above carries objects across the edge (SDS 05 §5.5, "The region
    edge"). Chains with a WholeLevel operation still stop at the region edge by
    design. Reproducer `SeamRun.RoiEdgeObjectIsNotTruncated` is enabled, with
    polygon, scene-edge and ROI-plus-mask-source cases. **Effect on stored
    results (QA/regulatory):** the identifiers of affected objects change,
    because a whole object's anchor differs from the truncated one's; objects
    previously stored cut become whole; objects whose true anchor is outside
    the ROI or polygon are no longer stored, so counts can fall; `tilesTotal`
    grows for ROI and polygon object runs, because it counts the level grid;
    and results previously stored by ROI- or polygon-restricted stages are
    affected and are not marked stale. Re-run those stages and record the
    change in the verification evidence.
    Also changed for ROI and polygon object runs: `tilesSkipped` now counts the
    out-of-region tiles; the per-tile memory check uses a full tile, so a tiny
    ROI on an object chain can now be refused by the memory ceiling where it was
    accepted before (a stage that ran earlier can fail validation, with the
    same message); and a float accumulation (a global-context reduction) folds
    over different tile boundaries, so its result, and any objects thresholded
    by it, can differ in the last bits from earlier runs.
  - **Resolved (durable stale-flag provenance): a stage that never completed
    could not be told redeclared from marked by a source's re-run.** The store
    now records when each stale flag was set (`StageStateReader::staleSinceUnixMs`,
    [stale-since plan](superpowers/plans/2026-11-21-stale-since.md)), and
    `StalenessGraph`'s causes name the source of a never-completed stage from it
    (SDS 04 §4.5, "The reverse answer"). **Effect on stored workspaces
    (QA/regulatory):** store format version 2. A version 1 store is upgraded in
    place on its first writable open by this library: an ordinary
    `Workspace::open` (as against `Workspace::openReadOnly`) and `Store::open` in
    `ReadWrite` mode are writable opens. Older library versions then refuse it,
    and a read-only open (`Workspace::openReadOnly`, `Store::open` in `ReadOnly`
    mode) leaves it at version 1. Causes reported for
    stages that never completed change from the ambiguous detail ("the stage has
    not completed: it was redeclared or marked stale directly, or a source stage
    re-ran") to a named source or the plain "redeclared" detail, except for flags
    set before the upgrade, which keep the ambiguous detail. Archive a copy of a
    version 1 store before opening it writable if it must stay readable by an
    older release.
  - **Resolved (ROI restrictions): ROI admission was not exact at levels above
    0.** (The widened ROI mask described below is superseded in part by the
    anchor-level-pixel entry after it: the ROI mask is `mapToLevel(roi)` again.) The ROI's anchor test floored the anchor to a level pixel and read the
    level mask, whose edges come from the `llround` level mapping of the
    rectangle. At a non-integer scale the two disagreed by up to one level-0
    column or row at the ROI's edges: an anchor on the ROI's first column could
    be refused and one just past its far edge admitted. At an integer scale
    above 1 the rule was not exact either, contrary to what this entry said
    before: an ROI edge inside a level pixel also admitted anchors up to one
    level pixel outside it. Fixed by the
    [exact ROI admission plan](superpowers/plans/2026-11-21-roi-exact-admission.md):
    an ROI is tested in level 0, at the anchor, half-open, in
    `RestrictionFilter::inRoi`, applied by `admits` (so by the seam merger) and
    by the tile object filter; the level mask's ROI part is widened to cover
    every level pixel holding an ROI anchor, so skipping stays sound. Level 0
    and runs restricted only by a polygon or a mask source are unchanged; the
    ROI mask an operation sees through `mask()` can be one level pixel wider on
    its left and top edges at a non-integer scale (advisory, SDS 05 §5.5). **Effect on stored
    results (QA/regulatory):** at levels above 0, objects anchored just outside
    an ROI edge are no longer stored, and objects on its first column or row
    are no longer dropped, so counts at an ROI's edges can change. Re-run
    ROI-restricted stages at levels above 0 and record the change in the
    verification evidence.
    At a non-integer scale, when the ROI's mapped start falls on a tile
    boundary, the strip of tiles just before the ROI is now evaluated instead of
    skipped, so `tilesSkipped` and `droppedByRestriction` can change for such
    runs; stored results are unaffected by that.
  - **Resolved (polygon and mask-source restrictions): the level pixel of an
    anchor.** A polygon or a mask source was tested at `floor(anchor / scale)`,
    while the level-to-level-0 mapping uses `llround(L * scale)` for pixel
    corners, so at a non-integer scale a mask-source restriction (the workspace
    restrict-to-layer path) could test an object's anchor one level pixel to its
    left or above. Fixed by the
    [anchor level pixel plan](superpowers/plans/2026-11-21-anchor-level-pixel.md):
    `RasterMask::testLevel0` tests the level pixel that is the inverse of the
    mapping (the largest `L` with `llround(L * scale) <= x`), and
    `RestrictionFilter` finds the same pixel. The floor-only widening of the
    tile mask and of the ROI mask that compensated for the old rule is removed
    (SDS 05 §5.5). **Effect on stored results (QA/regulatory):** at levels whose
    scale is not an integer, runs restricted by a polygon or a mask source
    (workspace restrict-to-layer) can keep or drop different objects along the
    restriction's edges. The mask an ROI-restricted operation sees loses the
    extra left/top column or row added on 2026-11-21, and `tilesSkipped` can
    change. At such levels a global-context operation that honours `mask()`
    (`threshold.otsu`) in an ROI-restricted run accumulates over one column and
    one row fewer at the ROI's left and top edges than on main since e45ce9b, so
    its threshold and the objects it yields can change; re-run those stages too
    and record the change in the verification evidence. Level 0 and integer
    scales are unchanged. Re-run affected stages and record the change in the
    verification evidence; stored results are not marked stale automatically.
  - **Resolved (stage declarations): a declare was not one transaction.** Each
    of its writes committed on its own, so a reader refreshing between a
    redeclaration's new record and its dependency rows could re-pin a row the
    writer then dropped and keep disagreeing with the writer, and a declare
    that failed part way left the stage row, the record or stale flags behind.
    Fixed by the
    [single-transaction declare plan](superpowers/plans/2026-11-21-single-transaction-declare.md):
    `StageStateWriter::Batch` groups a thread's writes into one transaction
    (SDS 03 §3.10a), and `StageRegistry::declare` computes everything aside,
    writes its rows in one batch and adopts what it computed only after the
    commit, so a failed declare changes nothing in the store or in memory (SDS
    04 §4.3). `markStale`, `clearStale`, `StageRunner`'s writes and
    `declareCoarseToFine` across its two declarations keep their separate
    commits. Stored results are unaffected.
- [Reporting implementation plan A](superpowers/plans/2026-11-21-reporting-a.md) —
  plan A for the `reporting` module (SDS 07): the module in the data library
  (core, config and storage only), `ExportResult`, `materialise` (UR-RES-6),
  `ColumnSchema` with per-store column resolution, and `CsvWriter` (UR-RES-9,
  UR-RES-11, UR-RES-15, UR-SEG-6, UR-QA-3): '#' provenance lines, the effective
  label set with workspace-local flags, parent ids, WKT geometry, calibrated
  measurement twins, and a `.partial` file renamed into place only on success.
  Storage gains `ObjectStore::measurementNameCount/At` (SDS 03, SDS 08).
  **Deferred: Parquet (UR-RES-10).** By user decision on 2026-11-21 the Parquet
  writer (SDS 07 §7.4) waits until its dependency is chosen; UR-RES-10 is not
  met by plan A. GeoJSON export and mask rasters are plans B and C.
  **In progress** on `feat/reporting-a`.
  - **Open (asserted measurements are not exported).** CSV cells come from the
    object row's own measurements only; values a later stage asserted
    (`AnnotationStore::assertedMeasurement*`) are left out. Asserted names share
    `measurement_names`, so `allMeasurements()` can produce a column that is
    empty in every row. Fix: read the asserted value when the row lacks the name
    (or exclude asserted-only names), and test both (SDS 07 §7.2).
  - **Open (per-row store lookups in the CSV export).** Each row runs
    `AnnotationStore::labelCount`/`labelAt`, which re-run the effective-labels
    query, and `EdgeStore::parentOf`, which prepares a fresh join
    (`src/storage/annotation_store.cpp`, `src/storage/edge_store.cpp`): about
    3–4 statement compiles per row with labels and parents on. Correct, but slow
    at 10 million objects (UR-RES-8). Fix: a storage-side batched read alongside
    the object cursor, or cached prepared statements; then a 1-million-object
    benchmark.
  - **Open (PHI/PII: slide identifier in export provenance).** The CSV
    provenance line `slide_id` is the slide path as the store holds it (the
    workspace stores the path it opened), and a path may carry identifiers
    (accession number, medical record number, patient name, user name) that are
    PHI or personal data under HIPAA and GDPR. Today the only control is
    `provenanceHeader(false)`, which drops all provenance (UR-RES-11). Fix: an
    option to redact the slide identifier, or to write only its basename or a
    hash, so provenance can travel without the path (SDS 07 §7.3). Belongs with
    the other pre-release PHI items.
- [Multiresolution segmentation](superpowers/specs/2026-10-11-multiresolution-segmentation.md)
  — whether Baatz & Schäpe region merging fits this design. It does, seeded
  from a partition rather than from pixels: the fusion factor depends only on
  additive per-segment aggregates, so growth needs no pixels and the existing
  global-context contract carries it at level 0. Design note; nothing amended.
- [Label rasters as geometry](superpowers/specs/2026-10-11-label-raster-geometry.md)
  — whether objects should be stored as an id-filled raster in HDF5 instead of
  polygons. Yes as a persisted companion to the object table: the anchor raster
  already exists per tile and pixel-local operations become trivial. No as a
  replacement, and no to HDF5. Design note; nothing amended.
- [Storage measurements](superpowers/specs/2026-10-10-storage-measurements.md)
  — the two measurements the spike left open, taken: the physical key scheme
  (2.39×), the annotation schemas (`WITHOUT ROWID`, interned measurement names,
  808 MB per slide), and the factor that dwarfs them both — annotation write
  order, at 24.6×. Raw logs and benchmark sources attached. SDS 03 is now
  freezable.
- [Spike findings, open questions 3 and 4](superpowers/specs/2026-10-07-spike-q3-q4-findings.md)
  — the two load-bearing questions, measured. Question 3 resolved **no**, and
  the "lazy pipeline exposed as a `Scene`" decision above now rests on the
  scene-source seam rather than on implementing `Scene` from outside; question 4
  resolved **yes**, with a 4.1x insert cost that made the physical-key change of
  SDS 03 §3.12 mandatory once UR-EXE-8 set a 30-minute bar. Consequences applied;
  two measurements remain open and are named in SDS 03 §3.12.
