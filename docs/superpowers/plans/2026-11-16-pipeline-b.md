# `pipeline` Module Implementation Plan B: running a pipeline

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make a `PipelineSpec` run: `Pipeline` with its runtime bindings and `validate()`, planar block reads from a SlideIO scene, chain evaluation per tile with halos, `WorkerPool` and `TileExecutor`, the three restriction points, the sinks, auxiliary rasters, the two-pass global-context protocol, and the whole-level rules adopted in plan A.

**Architecture:**
- **This is plan B of three.** Plan A is merged before it starts, and every name from A's Interfaces blocks is used as A defines it. Plan C adds `SeamMerger` and `asScene()`; this plan leaves two seams for it, named in its Interfaces blocks: the `seamFragment` callback in `detail::OperationContextState`, and `detail::RestrictionFilter::admits(anchor)`, which C's merged objects pass through.
- **One tile is evaluated by one function**, `detail::TileEvaluator::evaluate`, used both by the single-threaded path and by every worker. Determinism across worker counts (UR-QA-3) therefore reduces to "the evaluator is pure per tile, and everything combined across tiles is combined in tile order".
- **Validation reads no pixels.** Every check `validate()` makes uses scene metadata, the catalog's declarations and the bindings. A test proves it by counting the synthetic source's reads.

**Tech Stack:** as plan A.

**Spec:** `docs/superpowers/specs/sds/05-pipeline.md` §5.1 (the context the executor builds), §5.5, §5.6 (grid region and progress), §5.8 (canonical order), §5.9; the whole-level note `docs/superpowers/specs/2026-10-11-whole-level-operations.md` §3.3; plan A `docs/superpowers/plans/2026-11-16-pipeline-a.md` (P1–P17 bind here). `core`: `flow.hpp` (sinks, `TileMaskSource`, `TileRasterSource`), `raster_mask.hpp` (`RasterMask`, `rasteriseInto`), `memory_budget.hpp` (`makeMemoryBudget`), `control.hpp`, `error.hpp` (`ErrorContext`).

## Global Constraints

Plan A's Global Constraints hold unchanged. Added: commits go on branch `feat/pipeline-b`. Signatures are SDS-exact except the additions ruled below (`Pipeline::stageName`, P22).

## Rulings on spec gaps

Continuing plan A's numbering (A used P1–P12, P14, P16, P17; P13 and P15 were reserved for this plan).

| # | Gap | Ruling |
|---|---|---|
| P13 | Whole-level enforcement (adopted; the note's §3.3). | In `validate()`: (1) **Layout.** If any operation reports `TileRequirement::WholeLevel`, the run is laid out as a single tile covering the grid region, whatever the configured tile size (user decision: force, do not throw). (2) **Budget.** `needed = levelPixels × (Σ input plane bytes + Σ output plane bytes over the chain + Σ workingSetBytesPerLevelPixel)`, where `levelPixels` is the grid region's area at the level; if `needed > budget->ceilingBytes()`, throw `InvalidArgumentError` naming the first whole-level operation, the level, `needed` and the ceiling. (3) **Level.** See P21. Only whole-level chains get rules 1 and 2. |
| P15 | Canonical order across tiles. | `detail::OrderedFold<T>`: workers hand in `(tileIndex, T)`; the fold applies `combine` to the accumulated value strictly in ascending tile index, holding out-of-order results until their predecessors arrive (skipped tiles count as arrived). Used for every global-context operation (always, whether or not its arithmetic is order-independent, because the operation does not declare it) and for any `Reduction` whose `isOrderIndependent()` is false. An order-independent `Reduction` may combine in completion order. |
| P18 | Reading source pixels. | `detail::SceneReader::read(level, levelRect, planes) `: the rect is clipped to the level; the clipped part is read with `Scene::readResampledLevelBlockChannels(level, clipped, {w, h}, channels, buffer, size)`; everything outside the level is zero-filled by the reader itself, so the result never depends on SlideIO's background value. When every selected channel has one type, one call reads them all and the reader de-interleaves into planes; otherwise one call per channel. Plane types come from `dataTypeFromSlideio`; an `Unknown` type throws `UnsupportedDataTypeError` naming the channel. SlideIO exceptions (`std::exception`) become `IoError` with the cause. Buffers are owned by `detail::PlaneBuffers` (one `std::vector<uint8_t>` per plane, row stride `width × size`), which also produces the `BlockView` (its `slideRect` is the level-0 edge mapping of the rect, P9). |
| P19 | What `validate()` checks, in this order, before any pixel is read. | (a) the scene is non-null (`InvalidArgumentError`, also at construction); (b) the spec has at least one operation; (c) every operation name is registered (`InvalidArgumentError` naming it); (d) an explicit channel index is below the scene's channel count; (e) an explicit `analysisLevel` is in range; (f) the grid region (P23) is non-empty, else `InvalidArgumentError("the restriction does not intersect the scene")`; (g) types: walk the chain with the per-plane types — the scene's channel types for the first operation, then each operation's `outputType` — and for each operation and plane type, `detail::operationAccepts` must hold, else `UnsupportedDataTypeError` naming the operation and the type; a `Joint` operation with `requiresUniformTypes()` and mixed plane types throws `ChannelArityError` naming the operation and the channels; (h) target layers (SDS 05 §5.5): for an operation with `producesObjects()`, a `targetLayer` string parameter must name a layer bound with `bindLayer`; when it is absent and exactly one layer is bound, that layer; otherwise `InvalidArgumentError` naming the operation and the layer (or "no target layer"); (i) P13. Results (resolved level, target layers per operation, plane types per step, the grid) are cached until the next binding call or `validate()`. `run`/`runInto` call `validate()` first. |
| P20 | Which tile owns an object. | A finished object emitted by an operation through `objectSink()` is kept only when its **anchor** lies in the emitting tile's level-0 `tileRect` (half-open). An operation that sees halo pixels may therefore emit an object twice from two tiles without duplicating it in the store. Objects outside are dropped silently and counted (`detail::RunCounters::droppedOutsideTile`). This sits before the restriction filter (P24). |
| P21 | `analysisLevel = -1`. | The spec carries no requested output resolution, so "the coarsest adequate level" has nothing to be adequate to. **Ruling:** `-1` resolves to level 0 for a chain of `Halo` operations, and for a chain containing a `WholeLevel` operation to the **finest** level whose P13 budget fits, failing with P13's error when none does. A caller that wants a coarser level states it (`analysisLevel(n)` or `LevelSelector::levelForScale`). `analysisLevel()` before a successful `validate()` throws `InvalidArgumentError`. Flagged to the user as a decision taken. |
| P22 | Stage name for progress and errors. | Added: `Pipeline& stageName(const char* name);` (default `"pipeline"`; null or empty → `InvalidArgumentError`). It is the `stageName` passed to `ProgressObserver::onProgress` and put in every `TileError`'s `ErrorContext`. `StageRunner` sets it to the stage's name. |
| P23 | Grid region and tile size. | Level-0 region = scene rect ∩ ROI (if any) ∩ polygon `boundingBox()` (if any), mapped with `mapToLevel` and intersected with the level's extent. Tile size from `defaultTileSize(scene, level, config, w, h)` unless P13 forces one tile. A bound `TileMaskSource` never narrows the region (SDS 05 §5.6). |
| P24 | The restriction mask and the emission filter. | Per tile, before reading pixels, the worker builds the tile's mask over its **halo** rect at the level (`RasterMask(haloRectAtLevel, level, levelScale)`), only when the run is restricted: start all-set; intersect with the ROI rect (rasterised by its edges); with the polygon (`rasteriseInto`); with the bound `TileMaskSource` (`fillMask` on a second mask, then intersect). If the mask has no set pixel inside the **tile** rect, the tile is skipped: no pixel read, counted complete. `OperationContext::mask()` returns the tile's mask, or null when the run is unrestricted. `detail::RestrictionFilter` wraps the `ObjectSink`: it admits an object when `mask.testLevel0(anchor)` is true for the emitting tile's mask. For objects emitted outside a tile (plan C's merged objects) it exposes `admits(Point64 anchorLevel0)`, which builds a one-pixel mask at the anchor's level pixel through the same three sources. Filtering never alters pixels. |
| P25 | Evaluating the chain on one tile. | The chain's halo is the sum, edge by edge, of every operation's halo. Operation *k* runs on the tile grown by the halos of operations *k+1…n*; its input is the previous output (or the source read for *k = 1*) covering that region grown by its own halo. Each operation's context reports its own output region as `tileRectAtLevel` and its input region as `haloRectAtLevel`, so a context means the same thing at every position of the chain. A `PerChannel` operation is applied once per plane, on single-plane views; a `Joint` operation once on all planes, with `outputPlaneCount(n)` output planes. Output buffers are allocated by the evaluator from `outputType` before `apply`. The final output (exactly the tile rect) goes to `rasterSink` when bound. Only the operations with `producesObjects()` get a non-null `objectSink()`; every operation gets the edge and annotation sinks bound to the run. |
| P26 | Memory. | Before evaluating a tile, a worker reserves the tile's buffer bytes (source read, every intermediate, auxiliaries, mask) with `reserveOrWait(bytes, token)`, and releases them when the tile is done. A tile whose bytes exceed the ceiling on its own throws `InvalidArgumentError` from `validate()` naming the tile size and the ceiling, rather than waiting forever. Without `memoryBudget(...)`, the run uses `makeMemoryBudget(config.memoryCeilingBytes())`. |
| P27 | Workers, failure and cancellation. | `WorkerPool(workerCount, budget)`: `workerCount ≥ 1` (else `InvalidArgumentError`); threads start in the constructor and stop in `shutdown()` (idempotent; the destructor calls it). `TileExecutor::run` dispatches tile indices from an atomic counter in ascending order (completion order is free). The first tile failure stops dispatch, lets in-flight tiles finish, and rethrows as `TileError` with `ErrorContext{stageName, operationName (when an operation threw), cause = original what(), hasTile = true, tileX = column, tileY = row}`; a `ProcessorError` thrown by an operation keeps its own type only for `CancelledError`, which is rethrown as is. Cancellation is checked before each tile and between operations; it surfaces as `CancelledError`. Sinks are never flushed on the failure path (SDS 04 §4.4 records the stage failed). On success `runInto` flushes the object, edge, annotation and raster sinks in that order. Progress: `onProgress(stageName, done/total, done, total)` after each tile, total = `tileCount()` (SDS 05 §5.6), done counting skipped tiles; observer calls are serialised. |
| P28 | Global-context passes. | With global-context operations at chain positions g₁ < g₂ < …, the run makes one accumulation pass per gᵢ, then the final pass. Pass *i* evaluates operations 1…gᵢ−1 (already finalised) and calls `accumulate` on a **fresh instance per tile** of operation gᵢ (`OperationCatalog::create(name, parameters)`); the per-tile instances fold into the run's instance by P15, then `finalizeAccumulation()`. The final pass applies the whole chain with the finalised instances. `tilesTotal` counts every pass (`passes × tileCount`). |
| P29 | `run` without a sink. | `run(progress, token)` = `runInto(discard, progress, token)` with an internal sink that counts and drops. |

## Review Focus

1. **A ROI whose edge cuts through tiles, on a box-mean chain:** the output inside the ROI must equal the whole-level box mean cropped, pixel for pixel, at the ROI edge too (halo reads real pixels beyond the ROI). Pinned in Task 3.
2. **A mixed Byte/Float32 scene through a `PerChannel` chain and through a uniform-type `Joint` operation:** the first runs, the second fails in `validate()` with `ChannelArityError` and zero reads. Pinned in Task 2.
3. **An operation that throws on one tile of 64 with 4 workers:** `TileError` names that tile's column and row, the stage and the operation, the object sink was never flushed, and no worker is left running. Pinned in Task 4.
4. **A polygon restriction plus a `TileMaskSource` whose intersection is empty in most tiles:** skipped tiles read no pixels, progress still reaches `tileCount()`, and no object with an anchor outside the polygon is stored. Pinned in Task 5.
5. **A float global-context operation run with 1, 2, 4 and 8 workers:** the finalised value is bitwise identical. Pinned in Task 7.

---

### Task 1: Planar source reads

**Files:**
- Create: `src/pipeline/detail/scene_reader.hpp/.cpp`, `src/pipeline/detail/plane_buffers.hpp/.cpp`, `tests/pipeline/test_scene_reader.cpp`.

**Interfaces:**
- Consumes: plan A's synthetic scene, `detail::rectToLevel0`.
- Produces: `detail::PlaneBuffers` (`PlaneBuffers(int64_t width, int64_t height, const std::vector<DataType>& types, const std::vector<int32_t>& sourceChannels)`, `BlockView view(int32_t level, const Rect64& levelRect, double levelScale)`, `int64_t bytes() const`), `detail::SceneReader` (`SceneReader(slideio::ScenePtr)`, `std::vector<DataType> channelTypes() const`, `void read(int32_t level, const Rect64& levelRect, const std::vector<int32_t>& channels, PlaneBuffers& out) const`), per P18.

- [ ] **Step 1: Write the failing tests** (`// Requirements: UR-IMG-9, UR-IMG-4, UR-EXT-3`).

```cpp
TEST(SceneReader, ReadsUniformChannelsInOneCall) { /* 3 Byte channels: planes match f(c,x,y); source readCount()==1 */ }
TEST(SceneReader, ReadsMixedTypesOneChannelPerCall) { /* Byte + Float32: two reads; plane types Byte, Float32; values exact */ }
TEST(SceneReader, ZeroFillsOutsideTheLevel) { /* rect {-3,-2,10,10} on a 5x5 level: only the overlap carries f, the rest is 0 */ }
TEST(SceneReader, ReadsTheNamedLevelOnly) { /* read at level 2: the source saw only level-2 reads (UR-IMG-4) */ }
TEST(SceneReader, SlideioFailureIsIoError) { /* source throws std::runtime_error -> IoError with that cause */ }
TEST(PlaneBuffers, ViewCarriesGeometry) { /* view(level 1, {10,20,30,40}, 4.0005): slideRect is the edge-mapped rect, planes and strides right */ }
```

- [ ] **Step 2: Run it; see it fail to build.**
- [ ] **Step 3: Implement** per P18.
- [ ] **Step 4: Run** the suite; all PASS.
- [ ] **Step 5: Commit.** `feat(pipeline): planar block reads from a SlideIO scene`

### Task 2: `Pipeline`: construction, bindings, `validate()`

**Files:**
- Create: `include/slideio/processor/pipeline/pipeline.hpp`, `src/pipeline/pipeline.cpp`, `src/pipeline/detail/pipeline_impl.hpp`, `src/pipeline/detail/run_plan.hpp/.cpp`, `tests/pipeline/test_pipeline_validate.cpp`.

**Interfaces:**
- Consumes: plan A (spec, catalog, `operationAccepts`, `LevelSelector`, `TileGrid`, `defaultTileSize`), Task 1.
- Produces: `Pipeline` per SDS 05 §5.5 with P22's `stageName`, minus the bodies of `asScene`, `levelCount`, `rect` and `readBlock`, which throw `InternalError("not implemented until plan C")` here (declared now so the header is final). `detail::RunPlan` (built by `validate()`): `int32_t level; double levelScale; TileGrid grid; std::vector<OperationPtr> operations` (created from the spec through the catalog); `std::vector<std::vector<DataType>> planeTypes` (per step); `std::vector<LayerId> targetLayers` (per operation; invalid when none); `Halo chainHalo; std::vector<size_t> globalContextPositions; bool restricted; int64_t tileBytes`.

- [ ] **Step 1: Write the failing tests** (`// Requirements: UR-IMG-11, UR-ALG-3, UR-ALG-4, UR-EXE-1, UR-IMG-5, UR-STG-3`). Each validation test also asserts the synthetic source's `readCount() == 0`.

```cpp
TEST(PipelineValidate, UnknownOperationIsNamed) { /* spec.operation("test.V.nope") -> InvalidArgumentError containing it */ }
TEST(PipelineValidate, UnsupportedTypeNamesOperationAndType) { /* Threshold (Byte,Float32) on a UInt16 scene -> UnsupportedDataTypeError naming both */ }
TEST(PipelineValidate, TypesFollowTheChain) { /* BoxMean (Float32 out) then an op accepting only Float32: passes */ }
TEST(PipelineValidate, JointUniformOnMixedTypesIsArityError) { /* Byte+Float32 scene, a Joint requiresUniformTypes op -> ChannelArityError */ }
TEST(PipelineValidate, PerChannelAcceptsMixedTypes) { /* same scene, Threshold: validate passes */ }
TEST(PipelineValidate, ChannelAndLevelRange) { /* channel 5 of 3; analysisLevel 9 of 3: InvalidArgumentError */ }
TEST(PipelineValidate, RestrictionOutsideTheSceneFails) { /* roi beyond the scene -> "does not intersect" */ }
TEST(PipelineValidate, TargetLayerResolution) {
    // Components op, no targetLayer, one bound layer -> resolved to it;
    // two bound layers, no targetLayer -> InvalidArgumentError; targetLayer "x" unbound -> names "x"
}
TEST(PipelineValidate, WholeLevelForcesOneTileAndChecksBudget) {
    // a test WholeLevel op with workingSet 100 B/px: on a 1000x1000 level with a 2 GiB budget -> grid tileCount()==1;
    // with a 10 MB budget -> InvalidArgumentError naming the op, the level, the bytes and the ceiling
}
TEST(PipelineValidate, AutoLevel) { /* Halo chain -1 -> analysisLevel()==0; WholeLevel chain -1 picks the finest fitting level */ }
TEST(PipelineValidate, AnalysisLevelBeforeValidateThrows) {}
TEST(PipelineValidate, TileLargerThanTheCeilingFails) { /* P26 */ }
```

- [ ] **Step 2: Run it; see it fail to build.**
- [ ] **Step 3: Implement** per P13, P19, P21–P23, P26.
- [ ] **Step 4: Run** the suite; all PASS.
- [ ] **Step 5: Commit.** `feat(pipeline): Pipeline bindings and validation before any pixel is read`

### Task 3: Evaluating the chain on a tile, single-threaded

**Files:**
- Create: `src/pipeline/detail/tile_evaluator.hpp/.cpp`, `src/pipeline/detail/restriction.hpp/.cpp`, `tests/pipeline/test_tile_evaluator.cpp`.
- Modify: `src/pipeline/pipeline.cpp` (`runInto` with one worker, through the evaluator).

**Interfaces:**
- Consumes: Tasks 1–2, `detail::makeOperationContext`.
- Produces: `detail::TileEvaluator(const RunPlan&, const SceneReader&, Bindings)` with `TileOutcome evaluate(int64_t tileIndex, const CancellationToken&)`, where `TileOutcome` is `{bool skipped; int64_t bytesUsed;}`; `Bindings` holds the sinks, auxiliaries, mask source and the `seamFragment` callback (empty in this plan; plan C sets it). `detail::RestrictionFilter` with `bool admits(Point64 anchorLevel0) const` (P24) and the tile-mask builder `RasterMask buildTileMask(int64_t tileIndex) const`.

- [ ] **Step 1: Write the failing tests** (`// Requirements: UR-EXT-3, UR-IMG-5, UR-ALG-5, UR-EXE-1`).

```cpp
TEST(TileEvaluator, HaloMakesSeamsInvisible) {
    // BoxMean radius 3 on a 1000x700 level, tiles 128: the raster written to a capturing RasterSink, assembled,
    // equals BoxMean computed on the whole level in one tile, exactly (UR-EXT-3)
}
TEST(TileEvaluator, ZeroHaloDeclaredShowsTheSeam) {
    // a BoxMean variant declaring halo 0 but reading radius 3: differs from the whole-level result along tile seams
}
TEST(TileEvaluator, ChainHalosAdd) { /* BoxMean r2 then BoxMean r3 == two whole-level passes, exactly */ }
TEST(TileEvaluator, RoiMatchesWholeLevelCropped) { /* restrictTo a rect cutting through tiles: output inside equals whole-level cropped (UR-IMG-5) */ }
TEST(TileEvaluator, ObjectsAreOwnedByTheirAnchorTile) {
    // Components on a scene with blobs; tiles 64 with halo 8 (an op variant that sees halo): every object stored
    // once; count equals single-tile run's count of components wholly inside one tile plus the cut pieces
}
TEST(TileEvaluator, ContextGeometryAtEachChainPosition) {
    // a recording op at positions 1 and 2 of a chain with halo 2+3: tileRectAtLevel/haloRectAtLevel as P25 says
}
TEST(TileEvaluator, ThreeOperationChainHoldsNoIntermediateOnDisk) { /* scratch dir untouched after a 3-op run (UR-ALG-5) */ }
```

- [ ] **Step 2: Run it; see it fail to build.**
- [ ] **Step 3: Implement** per P20, P24 (mask built but only ROI here; Task 5 adds polygon and mask source tests), P25, P29.
- [ ] **Step 4: Run** the suite; all PASS.
- [ ] **Step 5: Commit.** `feat(pipeline): evaluate a chain per tile with summed halos`

### Task 4: `WorkerPool` and `TileExecutor`

**Files:**
- Create: `include/slideio/processor/pipeline/executor.hpp`, `src/pipeline/executor.cpp`, `tests/pipeline/test_executor.cpp`.
- Modify: `src/pipeline/pipeline.cpp` (`runInto` dispatches through `TileExecutor` with `config.workerCount()` workers).

**Interfaces:**
- Consumes: Tasks 2–3.
- Produces: SDS 05 §5.9 exactly, with P26 and P27. `TileExecutor::run(Pipeline&, ObjectSink&, ProgressObserver*, const CancellationToken&)` is what `Pipeline::runInto` calls; the executor reaches the run plan through `detail::PipelineAccess` (a friend struct in `pipeline_impl.hpp`), so no public signature grows.

- [ ] **Step 1: Write the failing tests** (`// Requirements: UR-EXE-2, UR-EXE-3, UR-EXE-4, UR-EXE-5, UR-QA-3`).

```cpp
TEST(TileExecutor, ResultsIdenticalAcrossWorkerCounts) {
    // BoxMean + Components on a 2000x1500 scene, tiles 128: raster tiles and the sorted set of stored objects
    // (id, polygon, measurements) are identical for 1, 2, 4 and 8 workers (UR-QA-3)
}
TEST(TileExecutor, SingleWorkerIsAllowed) { /* workerCount 1 runs; 0 -> InvalidArgumentError */ }
TEST(TileExecutor, FailureNamesTileStageAndOperation) {
    // an op throwing on tile (3,2) of 8x8 with 4 workers and stageName("nuclei"): TileError with hasTile,
    // tileX 3, tileY 2, stageName "nuclei", operationName the op, cause the original text; sink flush count 0;
    // the pool shuts down (no thread left: a second run on the same pipeline works)
}
TEST(TileExecutor, CancellationIsCancelledError) { /* cancel after 5 tiles: CancelledError; fewer than all tiles evaluated */ }
TEST(TileExecutor, ProgressReachesTotal) { /* calls monotonic, last (total,total), stageName passed, never concurrent */ }
TEST(TileExecutor, BudgetBoundsConcurrentTiles) {
    // a budget fitting two tiles and 8 workers: an instrumented op records max concurrent tiles == 2 (UR-EXE-2)
}
TEST(TileExecutor, SinksFlushedInOrderOnSuccess) { /* object, edge, annotation, raster */ }
```

- [ ] **Step 2: Run it; see it fail to build.**
- [ ] **Step 3: Implement** per P26–P27.
- [ ] **Step 4: Run** the suite; all PASS. Run the whole suite three times with `--gtest_repeat=3 --gtest_shuffle` on the executor tests to shake out ordering assumptions.
- [ ] **Step 5: Commit.** `feat(pipeline): worker pool and tile executor`

### Task 5: Restrictions end to end

**Files:**
- Modify: `src/pipeline/detail/restriction.cpp`, `src/pipeline/detail/tile_evaluator.cpp`.
- Create: `tests/pipeline/test_restriction.cpp`.

**Interfaces:**
- Consumes: Tasks 2–4; `rasteriseInto`; a test `TileMaskSource` (`testing_support::DiscMaskSource`: set inside a level-0 circle, counting `fillMask` calls).
- Produces: P24 complete, including `RestrictionFilter::admits` for anchors outside any tile (plan C's merged objects).

- [ ] **Step 1: Write the failing tests** (`// Requirements: UR-IMG-5, UR-STG-3`).

```cpp
TEST(Restriction, PolygonGridSpansItsBoundingBox) { /* tilesTotal equals the bbox grid's tile count, not the scene's */ }
TEST(Restriction, EmptyMaskTilesReadNoPixels) { /* polygon ∩ DiscMaskSource empty in most tiles: readCount() equals the non-empty tiles only; progress reaches tilesTotal */ }
TEST(Restriction, AnchorDecides) {
    // blobs straddling the polygon edge: stored iff mask.testLevel0(anchor); none stored with anchor outside (UR-STG-3)
}
TEST(Restriction, PixelsAreNeverAltered) { /* BoxMean output inside the polygon equals the unrestricted run there */ }
TEST(Restriction, MaskSourceDoesNotNarrowTheGrid) { /* only maskSource bound: tilesTotal == whole-scene grid */ }
TEST(Restriction, ContextMaskIsAdvisory) { /* an op ignoring mask() and one reading it store the same objects */ }
TEST(Restriction, AdmitsOutsideATile) { /* admits(anchor) agrees with the tile-mask answer for 1000 random anchors */ }
```

- [ ] **Step 2: Run it; see it fail.**
- [ ] **Step 3: Implement** the polygon and mask-source parts of P24.
- [ ] **Step 4: Run** the suite; all PASS.
- [ ] **Step 5: Commit.** `feat(pipeline): restrictions at grid, dispatch and emission`

### Task 6: Sinks and auxiliary rasters

**Files:**
- Modify: `src/pipeline/detail/tile_evaluator.cpp`, `src/pipeline/pipeline.cpp`.
- Create: `tests/pipeline/test_bindings.cpp`.

**Interfaces:**
- Consumes: Tasks 2–5; `TileRasterSource`, `RasterSink`, `EdgeSink`, `AnnotationSink` from `core/flow.hpp`.
- Produces: `rasterSink`, `edgeSink`, `annotationSink`, `auxiliaryRaster` per SDS 05 §5.5. Auxiliary: per tile a `BlockView` of the source's `planeCount()` planes of `planeType()` over the operation's halo rect, filled by `fillTile`; `auxiliary(name)` returns it. `auxiliaryRaster(name, nullptr)` removes the binding; same name twice replaces; null or empty name → `InvalidArgumentError`.

- [ ] **Step 1: Write the failing tests** (`// Requirements: UR-EXE-1, UR-RES-5, UR-PIX-6`).

```cpp
TEST(Bindings, RasterSinkGetsTheLastOutputPerTile) { /* tiles in sink union to the region; each slideRect is the edge-mapped tile rect; level = analysis level */ }
TEST(Bindings, NoRasterSinkDiscards) {}
TEST(Bindings, AuxiliaryCoversTheHaloRect) { /* a recording op sees auxiliary("coarse") with the same width/height/slideRect as input() */ }
TEST(Bindings, AuxiliaryReplaceAndRemove) {}
TEST(Bindings, EdgeAndAnnotationSinksReachOperations) { /* a test op asserting a parent and a label: both sinks receive them; null when unbound */ }
TEST(Bindings, OnlyObjectProducersGetAnObjectSink) {}
```

- [ ] **Step 2: Run it; see it fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the suite; all PASS.
- [ ] **Step 5: Commit.** `feat(pipeline): raster, edge and annotation sinks and auxiliary rasters`

### Task 7: Global-context passes and canonical order

**Files:**
- Create: `src/pipeline/detail/ordered_fold.hpp`, `tests/pipeline/test_global_context.cpp`, `tests/pipeline/test_support/global_ops.hpp/.cpp`.
- Modify: `src/pipeline/executor.cpp`, `src/pipeline/detail/tile_evaluator.cpp`.

**Interfaces:**
- Consumes: Tasks 2–6, `Histogram64`.
- Produces: P15 and P28. Test operations: `testing_support::FloatSumOperation` (global context; accumulates a `double` sum of all samples per tile in a deliberately order-sensitive way, e.g. summing 1e16 then small values; `apply` writes `sample − sum/count`) and `testing_support::OtsuOperation` (global context through a `Histogram64` of 256 bins; `apply` thresholds at the Otsu value).

- [ ] **Step 1: Write the failing tests** (`// Requirements: UR-PIX-5, UR-QA-3, UR-EXE-1`).

```cpp
TEST(GlobalContext, FloatAccumulationIsWorkerCountInvariant) { /* finalised sum bitwise equal for 1, 2, 4, 8 workers */ }
TEST(GlobalContext, StreamedOtsuEqualsInMemoryOtsu) { /* threshold equals the Otsu of a full in-memory histogram of the level (UR-PIX-5) */ }
TEST(GlobalContext, PassesAndProgress) { /* chain with two global ops: three passes; tilesTotal == 3 * tileCount */ }
TEST(GlobalContext, OrderedFoldHoldsOutOfOrderResults) { /* unit test of OrderedFold: inputs 3,1,0,2 combine as 0,1,2,3 */ }
TEST(GlobalContext, SkippedTilesDoNotStallTheFold) { /* restricted run with skipped tiles completes */ }
```

- [ ] **Step 2: Run it; see it fail.**
- [ ] **Step 3: Implement** per P15 and P28.
- [ ] **Step 4: Run** the suite; all PASS.
- [ ] **Step 5: Commit.** `feat(pipeline): two-pass global context with a canonical-order fold`

### Task 8: SDS fold and traceability

**Files:**
- Modify: `docs/superpowers/specs/sds/05-pipeline.md`, `docs/superpowers/specs/sds/04-workspace.md` (§4.4: `StageRunner` sets `stageName`; whole-level forcing), `docs/superpowers/specs/sds/08-class-diagrams.md`, `docs/traceability.md`.

- [ ] **Step 1:** Fold P13, P15, P18–P29 into SDS 05 §5.5, §5.6 and §5.9, stating behaviour, not rulings. §5.5 gains `stageName`, the validation order, the target-layer rule and the tile-ownership rule; P21 is stated with its reason.
- [ ] **Step 2:** SDS 04 §4.4 and SDS 08 D.6 updated to match.
- [ ] **Step 3:** Traceability rows for every new test file.
- [ ] **Step 4:** Docs target exits 0 with zero warnings; full build and tests pass.
- [ ] **Step 5: Commit.** `docs(pipeline): fold plan B rulings into SDS 05`
