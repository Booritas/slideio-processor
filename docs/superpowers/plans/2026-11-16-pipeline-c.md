# `pipeline` Module Implementation Plan C: seam merging and the lazy scene

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Complete the `pipeline` module: `SeamMerger` (union-find over seam fragments, incremental close, spilling, canonical-order intensity partials), its wiring into the executor, the UR-SEG-3 suite proving identical objects across at least three tile sizes, and `Pipeline::asScene()` with `levelCount`/`rect`/`readBlock`.

**Architecture:**
- **This is plan C of three.** Plans A and B are merged before it starts. It fills the two seams B left: the `seamFragment` callback in `detail::OperationContextState` (A's P6, with partials) and `detail::RestrictionFilter::admits` (B's P24), through which merged objects pass.
- **Geometry first, then the component.** The exact union of fragments that abut along a seam is a pure function, `detail::mergeFragments`, tested on its own. `SeamMerger` adds bookkeeping around it: which fragments join, when a component may close, where fragments live while it is open.
- **Correctness is proven against single-tile processing**, using plan A's `testing_support::ComponentsOperation`, which finishes objects inside a tile and hands seam-touching ones to the merger. The suite asserts equality of ids, normalised polygons and measurements across tile sizes and worker counts.
- **The lazy scene is a SlideIO `ISceneSource`** over `Pipeline::readBlock`, built with `slideio::makeScene` (the scene-source design, `docs/superpowers/specs/2026-10-08-scene-source-design.md` §7).

**Tech Stack:** as plans A and B.

**Spec:** SDS 05 §5.5 (`asScene`, `levelCount`, `rect`, `readBlock`), §5.7 (all of it), §5.9 (the executor's calls into the merger); SDS 06 §6.8 (partials across a seam); the scene-source design §7; plans A and B (P1–P29 bind here).

## Global Constraints

Plan A's Global Constraints hold unchanged. Added: commits go on branch `feat/pipeline-c`. Signatures are SDS-exact except: `IntensityPartial` lives in `operation.hpp` (A's P6), and the additions ruled below.

## Rulings on spec gaps

| # | Gap | Ruling |
|---|---|---|
| P30 | When two fragments belong to one object. | A fragment's **seam contacts** are the maximal segments of its rings that lie on a seam line of the grid (a level-0 tile edge `x = tileRect(c).x` for an interior column boundary, likewise y), recorded as `(seam line, interval)` in level-0 integers. Two fragments in tiles on either side of one seam line join when they have contacts on that line whose intervals **overlap with positive length**. Touching only at a point (a corner) does not join: the merger implements 4-connectivity, matching `ComponentsOperation`; an operation that wants 8-connected objects must not split at corners. Joining is union-find with path compression; the representative of a component is irrelevant to the output (decision 8). |
| P31 | How fragments are merged into one polygon. | **Edge cancellation, exact, in integers.** Every directed ring edge of every fragment in the component is split at every fragment vertex lying on it (only seam edges can need splitting). Edge pairs that coincide with opposite directions are removed (they are the shared seam). The remaining directed edges are linked into rings; at a vertex with several outgoing edges, take the one making the sharpest right turn relative to the incoming direction (so touching rings separate rather than cross). Core does not fix ring orientation (`addRing` takes an explicit hole flag), so before cancelling, every input ring is oriented by its flag: outer rings to positive shoelace area in level-0 coordinates, holes to negative; after linking, positive rings are outer and negative are holes. The output has the single outer ring first, then the holes (`addRing(…, false)` then `addRing(…, true)`), the holes ordered by their first vertex after normalisation (y, then x), as `ComponentsOperation` orders them, so objects with two or more holes compare point for point; a component yielding more than one outer ring throws `InternalError`, since P30's positive-length joins make a component connected. Collinear vertices are removed, and each ring starts at its topmost-then-leftmost vertex: the same normalisation `ComponentsOperation` applies (A), so a merged object and a single-tile object compare equal point for point. A component whose edges do not form closed rings throws `InternalError` naming the layer and the component's bounding box. |
| P32 | When a component closes. | A fragment in tile *t* with a contact on seam line *s* **expects** the tile on the other side of *s* at that contact's position. A component may close when every tile any of its fragments expects has completed (`notifyTileComplete` received; a tile skipped by restriction counts as completed, so the executor notifies skipped tiles too) and every tile holding one of its fragments has completed. `notifyTileComplete(t, sink)` re-checks only the components touching *t*. A closed component is merged (P31), measured (P34), filtered (`RestrictionFilter::admits` on its anchor, through a filter the executor hands the merger), and emitted to `sink`, then forgotten. `resolve` closes whatever remains (an empty grid or a run whose tiles all completed leaves nothing; anything left at `resolve` is a bug and throws `InternalError`). |
| P33 | Holding and spilling fragments. | Each resident fragment's bytes (its ring points × 16 plus partials × `sizeof(IntensityPartial)` plus a fixed 64) are reserved with `MemoryBudget::tryReserve` when it arrives. When that fails, the merger spills the **oldest-opened** components (by the sequence number of their first fragment) until the reservation succeeds or nothing resident remains, then reserves with `reserveOrWait`. A spilled fragment is appended to one run file `<scratch>/seam-<random 32 hex>.bin` as `int32 ringCount; per ring: uint8 hole, int64 pointCount, int64 x,y…; int32 partialCount; partials` and its offset kept in memory; union-find entries, contacts and bounding boxes stay resident. Spilled fragments are read back when their component closes. The file is removed in the destructor, on the failure path too. Open, write or read failure throws `IoError` naming the file. `spilledFragmentCount`, `spilledBytes`, `residentFragmentCount`, `largestComponentFragmentCount` report what happened. The scratch directory: `StageRunner` passes its own; a `Pipeline` used directly creates `<config.scratchDirectory()>/pipeline-<random 32 hex>/` per run and removes it after (P36). |
| P34 | Measurements of a merged object. | The component's partials are combined with `detail::combinePartials` **in canonical fragment order** — ascending fragment anchor (y, then x), ties by tile index — per channel, never in arrival or close order. The merged `ObjectRecord` gets the merged polygon, the layer, and `applyStandardMeasurements` (A's P6a) of the polygon and the combined partials. Its id follows from its anchor (decision 8). **Equality across tile sizes:** for Byte samples the sums are exact in `double` (`count × max² < 2^53` holds for every component below about 1.4 × 10^11 pixels), so measurements are bitwise identical; the claim is not made for wider integer types (UInt16 breaks it at about 2.1 M pixels, which the level-spanning region exceeds), and `ComponentsOperation` admits only Byte and Float32 intensities; for float sample types the grouping of a float sum differs between tile sizes, so the suite asserts relative difference ≤ 1e-12 there and documents why. Worker-count invariance is exact for every type. |
| P35 | `levelCount`, `rect`, `readBlock`, and what level *k* of a pipeline is. | The pipeline's output pyramid mirrors the source scene's levels from the analysis level down: `levelCount() = sourceLevels − analysisLevel()`; output level *k* is the chain evaluated on source level `analysisLevel() + k`, with halos in that level's pixels (SDS 05 §5.6: never rescaled). `rect(k)` is the grid region (B's P23) computed at that source level, in that level's coordinates. `readBlock(k, rect, out)`: `out` must have the chain's final plane count and types and `rect`'s size (`InvalidArgumentError` otherwise); the rect may extend beyond `rect(k)`, and pixels outside `rect(k)` or outside the restriction mask are zero; tiles of a grid at level *k* that intersect `rect` are evaluated (B's `TileEvaluator`, without object, edge or annotation sinks, and without the raster sink) and copied. `readBlock` is const and ThreadSafe; it calls `validate()` lazily once, guarded by a `std::once_flag`. A chain with a global-context operation throws `InvalidArgumentError("global-context operations need runInto")` from `asScene`, `readBlock` and `levelCount`: the lazy path has no accumulation pass (UR-EXE-1 gives that to the eager one). |
| P36 | `asScene()`. | Returns `slideio::makeScene(source, metadata)` where the source is `detail::PipelineSceneSource` holding a copy of the `Pipeline`'s spec, scene, configuration and bindings that matter to pixels (mask source, auxiliaries; not sinks). Metadata: `name` = source scene name + `"/pipeline"`; `rect` = the source scene rect (level-0); `channelDataTypes` = the chain's final plane types mapped with `dataTypeToSlideio`; `channelNames` = `"c<k>"`; resolution, magnification and colour profile copied from the source; `levels` = one `LevelInfo` per output level (size from `rect(k)` extent, scale and magnification of the source level, tile size from `defaultTileSize`). `readLevelBlock` evaluates through `readBlock` and interleaves into SlideIO's buffer (SlideIO requires one type per multi-channel read, which its adapter enforces). `readBlock(sceneRect, blockSize, …)` picks the coarsest output level whose resolution is at least the requested one (`LevelSelector::levelForScale` on the ratio), reads it, and resizes to `blockSize` with nearest-neighbour (`cv::INTER_NEAREST`), so no value is invented, as storage's raster sources do. `supportsConcurrentReads()` is true. **Construction computes no pixels:** `asScene()` validates (metadata only) and builds the source; the test proves zero source reads. |
| P37 | Umbrella header. | `include/slideio/processor/pipeline/pipeline_all.hpp` includes every public pipeline header (`pipeline.hpp` is taken by the `Pipeline` class). A test includes only it and touches one name from each header. |

## Review Focus

1. **A ring-shaped object cut by four seams** (an annulus centred on a tile corner): one object, one outer ring and one hole, identical across tile sizes 64, 100 and 257. Pinned in Task 1 (geometry) and Task 5 (end to end).
2. **A tissue-like region spanning the whole level with a budget too small to hold its fragments:** fragments spill, the merged object is identical to the unspilled run, and no `seam-*.bin` file survives the run or a run that throws midway. Pinned in Task 3.
3. **Two objects touching only at a tile corner:** stay two objects at every tile size, as in the single-tile run. Pinned in Task 1 and Task 5.
4. **Worker count 1 vs 8 with float intensities:** merged measurements bitwise equal (canonical order). Pinned in Task 5.
5. **A lazy scene read concurrently from 8 threads at a coarse resolution:** pixels equal the eager run's raster output at that level, and constructing the scene read nothing. Pinned in Task 6.

---

### Task 0: Prerequisites from plan B's final review

Plan B leaves the run's state where only one eager run at a time can use it: `validate()` resets the plan in place, and a run swaps fresh global-context instances into `RunPlan::operations`. `asScene()` reads blocks concurrently and after later binding calls, so this task changes that before Task 2 or Task 6 builds on it. It changes no public signature.

**Files:**
- Modify: `src/pipeline/detail/pipeline_impl.hpp`, `src/pipeline/pipeline.cpp`, `src/pipeline/executor.cpp`, `src/pipeline/detail/tile_evaluator.hpp/.cpp`, `src/pipeline/detail/operation_context_state.hpp/.cpp`, `src/pipeline/detail/restriction.hpp/.cpp`; tests in `tests/pipeline/test_global_context.cpp`, `tests/pipeline/test_restriction.cpp`, `tests/pipeline/test_operation.cpp`.

**Interfaces:**
- (a) `Pipeline::Impl::plan` becomes `std::shared_ptr<const detail::RunPlan>`. `validate()` builds a new plan and swaps it in; it never resets or mutates one in place, and a binding call drops the pointer, never the plan another holder still uses.
- (b) Each run's global-context instances live in a run-local `std::vector<OperationPtr>` (the plan's operations with the fresh instances in their positions) handed to `TileEvaluator`, which takes the operations to apply as an argument instead of reading `RunPlan::operations`. Nothing a run creates is written into the plan.
- (c) `asScene()` captures a snapshot of the plan (the `shared_ptr` of (a)) together with the bindings P36 copies, so a later `validate()` or binding call on the `Pipeline` never changes a scene already handed out.
- (d) Global-context chains on the lazy path: P35's ruling stands — `asScene`, `readBlock` and `levelCount` refuse them with `InvalidArgumentError`; running their accumulation passes eagerly inside `asScene()` is not done, since it would make construction compute pixels. Confirm this before Task 6 and record any change as a ruling.
- (e) The `seamFragment` callback in `detail::OperationContextState` gains the emitting tile index: `std::function<void(const Polygon&, LayerId, const IntensityPartial*, int32_t, int64_t tileIndex)>`; the evaluator fills it in. `SeamMerger` needs it for P34's canonical fragment order (ties by tile index) and to tie fragments to `notifyTileComplete`.
- (f) `RestrictionFilter::admits(anchor)` must not call `TileMaskSource::fillMask` once per merged object: at 10 million objects that is 10 million callbacks. It answers from the mask of the tile holding the anchor (built once per tile and kept while that tile's components can still close), or from a cache keyed by tile; a test counts `fillMask` calls for many merged objects in one tile.

- [ ] **Step 1: Write the failing tests** (`// Requirements: UR-EXE-1, UR-QA-3, UR-STG-3`): a plan held across a `validate()` keeps its operations; two runs leave the plan's global-context positions holding the instances `validate()` created; the fragment callback receives the tile index; `admits` over many anchors in one tile calls `fillMask` once for that tile.
- [ ] **Step 2: Run them; see them fail.**
- [ ] **Step 3: Implement** (a)–(c), (e) and (f); write (d) into the plan's rulings if it changes.
- [ ] **Step 4: Run** the whole pipeline suite, and `TileExecutor|GlobalContext` with `--gtest_repeat=3 --gtest_shuffle`; all PASS.
- [ ] **Step 5: Commit.** `refactor(pipeline): immutable run plan and run-local global-context instances`

### Task 1: Exact fragment merging

**Files:**
- Create: `src/pipeline/detail/fragment_merge.hpp/.cpp`, `tests/pipeline/test_fragment_merge.cpp`.

**Interfaces:**
- Produces: `detail::SeamContact { bool vertical; int64_t line; int64_t begin; int64_t end; }`; `detail::seamContacts(const Polygon&, const TileGrid&, int64_t tileIndex) -> std::vector<SeamContact>`; `detail::contactsJoin(const SeamContact&, const SeamContact&) -> bool` (same orientation and line, overlap > 0); `detail::mergeFragments(const std::vector<const Polygon*>&) -> Polygon` per P31; `detail::normaliseRings(Polygon&)` (moved here from A's test support so both use one implementation; A's `testing_support::normaliseRing` then calls it — update the test helper).

- [ ] **Step 1: Write the failing tests** (`// Requirements: UR-SEG-3`).

```cpp
TEST(FragmentMerge, TwoHalvesOfARectangle) { /* {0,0,10,4} split at x=6: merged == the rectangle, 4 vertices */ }
TEST(FragmentMerge, FourQuartersAroundACorner) { /* a 10x10 square cut at x=5,y=5: one ring of 4 vertices */ }
TEST(FragmentMerge, AnnulusAcrossFourSeamsHasAHole) { /* square ring cut by both seam lines: one outer, one hole; area equals the unsplit annulus */ }
TEST(FragmentMerge, UShapeJoinsThroughTwoSeamsIntoOneRing) {}
TEST(FragmentMerge, CornerTouchDoesNotJoin) { /* contactsJoin false for intervals meeting at one point */ }
TEST(FragmentMerge, OutputIsNormalised) { /* no collinear vertices; ring starts at topmost-leftmost */ }
TEST(FragmentMerge, OpenEdgesAreInternalError) { /* fragments that do not abut exactly -> InternalError */ }
TEST(FragmentMerge, NonDyadicSeamsCoincideExactly) {
    // fragments produced through toLevel0 at scale 4.0005 on both sides of a seam merge (P9's edge mapping)
}
```

- [ ] **Step 2: Run it; see it fail to build.**
- [ ] **Step 3: Implement** per P30–P31.
- [ ] **Step 4: Run** the suite; all PASS.
- [ ] **Step 5: Commit.** `feat(pipeline): exact merging of seam fragments`

### Task 2: `SeamMerger` without spilling

**Files:**
- Create: `include/slideio/processor/pipeline/seam_merger.hpp`, `src/pipeline/seam_merger.cpp`, `tests/pipeline/test_seam_merger.cpp`.

**Interfaces:**
- Consumes: Task 1; `RestrictionFilter` (B); `applyStandardMeasurements`, `detail::combinePartials` (A).
- Produces: SDS 05 §5.7 exactly (with `IntensityPartial` from `operation.hpp`), plus `void setFilter(std::function<bool(Point64)>)` on a `detail::SeamMergerAccess` (not public), which the executor uses to apply the restriction. P32 and P34.

- [ ] **Step 1: Write the failing tests** (`// Requirements: UR-SEG-3, UR-QA-2, UR-QA-3`).

```cpp
TEST(SeamMerger, ClosesWhenItsNeighbourhoodCompletes) {
    // 3x3 grid; a two-fragment object across tiles 0|1: not emitted after notify(0), emitted at notify(1),
    // before tiles 2..8 complete (SDS 05 §5.7 incremental close)
}
TEST(SeamMerger, CompletionOrderDoesNotChangeTheResult) { /* 100 random completion orders: same emitted set */ }
TEST(SeamMerger, PartialsCombineInCanonicalOrder) {
    // float partials arriving in different orders give bitwise-identical intensity.<c>.mean
}
TEST(SeamMerger, FilterDropsByAnchor) { /* filter rejecting one anchor: that object never reaches the sink */ }
TEST(SeamMerger, CountersReport) { /* fragmentCount, mergedObjectCount, largestComponentFragmentCount */ }
TEST(SeamMerger, ConcurrentAddFragment) { /* 8 threads adding fragments of 1000 objects: 1000 objects emitted */ }
TEST(SeamMerger, ResolveWithOpenComponentIsInternalError) {}
```

- [ ] **Step 2: Run it; see it fail to build.**
- [ ] **Step 3: Implement** per P32, P34; the constructor takes the budget and scratch directory now, spilling arrives in Task 3.
- [ ] **Step 4: Run** the suite; all PASS.
- [ ] **Step 5: Commit.** `feat(pipeline): SeamMerger with incremental close`

### Task 3: Spilling

**Files:**
- Create: `src/pipeline/detail/fragment_spill.hpp/.cpp`, `tests/pipeline/test_seam_spill.cpp`.
- Modify: `src/pipeline/seam_merger.cpp`.

**Interfaces:**
- Produces: P33 complete.

- [ ] **Step 1: Write the failing tests** (`// Requirements: UR-EXE-2, UR-SEG-3`).

```cpp
TEST(SeamSpill, SmallBudgetSpillsAndStillMergesIdentically) { /* spilledFragmentCount() > 0; emitted objects equal an unbounded run */ }
TEST(SeamSpill, OldestComponentsSpillFirst) {}
TEST(SeamSpill, FilesRemovedOnDestructionAndOnFailure) { /* scratch dir empty after success and after a throwing sink */ }
TEST(SeamSpill, UnwritableScratchIsIoError) { /* scratch path is a file: IoError naming it */ }
TEST(SeamSpill, BudgetReleasedAtClose) { /* reservedBytes() returns to its starting value after resolve */ }
```

- [ ] **Step 2: Run it; see it fail.**
- [ ] **Step 3: Implement** per P33.
- [ ] **Step 4: Run** the suite; all PASS.
- [ ] **Step 5: Commit.** `feat(pipeline): spill long-open seam components to scratch`

### Task 4: Wiring the merger into the run

**Files:**
- Modify: `src/pipeline/executor.cpp`, `src/pipeline/detail/tile_evaluator.cpp`, `src/pipeline/pipeline.cpp`.
- Create: `tests/pipeline/test_seam_run.cpp`.

**Interfaces:**
- Consumes: Tasks 2–3; B's executor and evaluator.
- Produces: in `runInto`, when any operation `producesObjects()`, a `SeamMerger` over the run's grid, budget and scratch directory (P33's per-run directory for a direct `Pipeline`); the evaluator's `seamFragment` callback calls `addFragment(tileIndex, …)`; the executor calls `notifyTileComplete` after every tile, skipped ones included, and `resolve` at the end of the final pass, before the sinks are flushed; the merger's filter is B's `RestrictionFilter::admits`. Fragment polygons are not subject to B's P20 tile-ownership rule (they are partial by definition); the merged object's anchor is the only test.

- [ ] **Step 1: Write the failing tests** (`// Requirements: UR-SEG-3, UR-STG-3, UR-EXE-5`).

```cpp
TEST(SeamRun, ObjectsAcrossSeamsAreStoredOnce) { /* Components with seams=true: count equals the single-tile run's */ }
TEST(SeamRun, RestrictionAppliesToMergedObjects) { /* a merged object with anchor outside the polygon is not stored */ }
TEST(SeamRun, ScratchDirectoryRemovedAfterRun) { /* config scratch dir has no pipeline-* left, also after a TileError */ }
TEST(SeamRun, NoMergerForRasterOnlyChains) { /* BoxMean chain: no scratch dir created */ }
```

- [ ] **Step 2: Run it; see it fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the suite; all PASS.
- [ ] **Step 5: Commit.** `feat(pipeline): merge seam fragments during the tile pass`

### Task 5: The UR-SEG-3 suite

**Files:**
- Create: `tests/pipeline/test_seg3_tiling_invariance.cpp`, `tests/pipeline/test_support/blob_scene.hpp/.cpp` (a synthetic scene of blobs: discs, annuli, U-shapes, corner-touching pairs, one region spanning the level, values varying inside each blob; a Byte mask with a Byte or a Float32 intensity channel; dyadic and non-dyadic pyramids).

**Interfaces:**
- Consumes: Task 4; `ComponentsOperation` (the Byte suites use its one-plane form, the mask measured as its own intensity; the Float32 suite uses its two-plane form, plane 0 a Byte mask and plane 1 the Float32 intensity, so the float partials cross the seams).

- [ ] **Step 1: Write the tests** (`// Requirements: UR-SEG-3, UR-QA-2, UR-QA-3`). They should pass if Tasks 1–4 are right; any failure is a defect in those tasks, fixed there, not here.

```cpp
// The reference is a single-tile run (tile size = level extent).
// "Identical" compares, per object sorted by ObjectId: the id, the normalised polygon point for point,
// area, perimeter, centroid, bounding box, and intensity.<c>.{mean,stddev,min,max}.
TEST(Seg3, IdenticalAcrossThreeTileSizesByte) { /* tile sizes 64, 100, 257 at level 0: identical, exact */ }
TEST(Seg3, IdenticalAcrossTileSizesFloat) { /* two-plane ComponentsOperation: a Byte blob mask over Float32 intensities; geometry exact, intensities within 1e-12 relative (P34) */ }
TEST(Seg3, IdenticalOnANonDyadicLevel) { /* the 8001x6001 / 2000x1500 pyramid, level 1, tile sizes 64, 100, 257 */ }
TEST(Seg3, IdenticalAcrossWorkerCounts) { /* tile 100: 1, 2, 4, 8 workers identical, exact for both types */ }
TEST(Seg3, IdenticalWhenSpilling) { /* tile 64 with a budget that forces spills of the level-spanning region */ }
TEST(Seg3, CornerTouchingObjectsStaySeparate) { /* a pair touching at a seam corner: two objects at every tile size */ }
```

- [ ] **Step 2: Run them.** All PASS; if not, return to the owning task.
- [ ] **Step 3: Commit.** `test(pipeline): UR-SEG-3 tile-size and worker-count invariance suite`

### Task 6: The lazy scene

**Files:**
- Create: `src/pipeline/detail/pipeline_scene_source.hpp/.cpp`, `tests/pipeline/test_lazy_scene.cpp`.
- Modify: `src/pipeline/pipeline.cpp` (`asScene`, `levelCount`, `rect`, `readBlock`).

**Interfaces:**
- Consumes: B's evaluator; SlideIO `ISceneSource`, `SceneMetadata`, `makeScene`.
- Produces: P35 and P36.

- [ ] **Step 1: Write the failing tests** (`// Requirements: UR-EXE-1, UR-PIX-6, UR-ALG-5, UR-IMG-4`).

```cpp
TEST(LazyScene, ConstructionReadsNothing) { /* asScene(): source readCount()==0 */ }
TEST(LazyScene, PixelsEqualTheEagerRun) { /* BoxMean: scene->readBlock over the whole level equals the eager raster output */ }
TEST(LazyScene, IsAValidPipelineInput) { /* Pipeline over asScene() of a BoxMean pipeline, then Threshold: equals the two-op chain (UR-PIX-6) */ }
TEST(LazyScene, CoarseReadUsesACoarseLevel) { /* a 1/16 read: the source saw no level-0 read (UR-IMG-4) */ }
TEST(LazyScene, ConcurrentReads) { /* 8 threads reading random blocks: each equals the single-threaded read */ }
TEST(LazyScene, LevelsMirrorTheSourceFromTheAnalysisLevel) { /* analysisLevel 1 of 4: levelCount 3; rect(k) per P35 */ }
TEST(LazyScene, ReadBlockValidatesTheView) { /* wrong plane count or type -> InvalidArgumentError */ }
TEST(LazyScene, GlobalContextIsRefused) { /* chain with FloatSumOperation: asScene throws, naming runInto */ }
TEST(LazyScene, MixedTypeOutputPlanesAreSeparateChannels) { /* PerChannel on Byte+Float32: channel types Byte and Float32 */ }
```

- [ ] **Step 2: Run it; see it fail.**
- [ ] **Step 3: Implement** per P35–P36.
- [ ] **Step 4: Run** the suite; all PASS.
- [ ] **Step 5: Commit.** `feat(pipeline): a pipeline as a lazy SlideIO scene`

### Task 7: Umbrella header, docs, and the module's close

**Files:**
- Create: `include/slideio/processor/pipeline/pipeline_all.hpp`, `tests/pipeline/test_umbrella.cpp`.
- Modify: `docs/superpowers/specs/sds/05-pipeline.md`, `docs/superpowers/specs/sds/06-algorithm.md` (§6.8: the overload with partials and `applyStandardMeasurements`), `docs/superpowers/specs/sds/08-class-diagrams.md`, `docs/traceability.md`, `docs/project.md` (the three plans, the scene-source plan's `Pipeline::asScene()` now delivered), `README.md` (the engine library and its link rules).

- [ ] **Step 1:** Write the umbrella header and its test (`// Requirements: UR-API-2`).
- [ ] **Step 2:** Fold P30–P37 into SDS 05 §5.5 and §5.7; SDS 06 §6.8 and SDS 08 to match.
- [ ] **Step 3:** Traceability rows; `docs/project.md` and `README.md`.
- [ ] **Step 4:** Docs target exits 0 with zero warnings; full build and tests pass.
- [ ] **Step 5: Commit.** `docs(pipeline): seam merging and the lazy scene complete the module`
