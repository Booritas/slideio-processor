# Restricted-Object Truncation Fix Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A restricted run must store every admitted object whole. The case to fix is an object whose anchor lies inside the mask but which reaches into a tile the restriction skipped. Today that object is stored truncated, silently, with wrong geometry and measurements.

**Architecture:**
- The seam merger stops treating a skipped tile as complete. It records the tile as *deferred*.
- After the final pass, the executor evaluates, on demand, every deferred tile that an open component still expects. It repeats this until no more tiles are demanded, then completes the remaining deferred tiles unevaluated, then resolves.
- The pass's concurrency is unchanged. The set of evaluated tiles is a deterministic fixpoint, so results stay identical across worker counts.
- All new merger and evaluator surface is in `src/pipeline/detail/`. No public header changes.

**Tech Stack:** C++17, CMake, GoogleTest, MSVC.

**Spec:** `docs/superpowers/specs/sds/05-pipeline.md`:
- §5.5 "The tile mask" (the limitation note at ~1286-1301 that this plan removes);
- the restriction rules at ~949-961 ("the third [object emission] is the authoritative one and the other two are optimisations that change only cost"; UR-IMG-5 "results match those obtained by processing the whole slide and cropping");
- the "skipped counts as complete" text at ~1278-1279, ~1723-1724 and ~2132-2134.

**Base:** branch `fix/restriction-truncation` from `feat/workspace-c` (PR #17), because the workspace reproducer lives there. Open the PR against `main` once #17 has merged; until then, against `feat/workspace-c`.

## Global Constraints

- Build `./build.sh --skip-slideio`; tests `./run-tests.sh` (all must pass); one test `ctest --test-dir build -C Release -R <name>`; docs `cmake --build build --target docs --config Release`.
- Zero compiler warnings and zero Doxygen warnings.
- No public header changes. New members go on `SeamMerger::Impl` through `detail::SeamMergerAccess` (`src/pipeline/detail/seam_merger_access.hpp`) and on `detail::TileEvaluator`.
- Results must stay identical across worker counts. `TileExecutor.ResultsIdenticalAcrossWorkerCounts`, `Seg3.IdenticalAcrossWorkerCounts` and `GlobalContext.FloatAccumulationIsWorkerCountInvariant` must pass unchanged.
- Unrestricted runs, runs with no object producer, accumulation passes and lazy `readBlock` behave exactly as before.
- Messages never carry pixel, coordinate or parameter values.
- Every source and test file starts with its `// Requirements: ...` line. `docs/traceability.md` lists every new test, one row per requirement, updated in place.
- Match the surrounding code's idiom and comment density. There is no Python; edit with the Edit tool or sed.

## Decisions

1. **Three tile states in the merger: pending, completed, deferred.**
   - `notifyTileSkipped(tile)` marks a tile deferred. It is not complete, so `add()` records expectations on it as on a pending tile. This removes the path where a skipped tile completed before its neighbour's fragment arrived and the expectation was silently dropped.
   - `takeAwaitedDeferred()` returns, in ascending order, the deferred tiles that some open component *with a pass fragment* expects, and marks them as *demanded*, so each is returned at most once.
   - A component has a pass fragment when one of its fragments came from a tile evaluated in the pass, not on demand. The flag is set when a fragment arrives from a tile that is not demanded, and a union has it when either side had it. A fragment from a demanded tile does not set it.
   - Why that is sound at integer scales: every tile the pass evaluated has completed before the first demand. An object with a pixel in such a tile holds that tile's fragment, so it reaches every skipped tile it crosses. An object lying wholly in skipped tiles has its anchor where the mask is zero, so the restriction drops it, whole or truncated. Without the rule, objects found in a demanded tile would pull in their own neighbours, and the demands would spread over dense objects; the dense-squares fixture went from 32 demanded tiles to 11. The non-integer-scale case is decision 5's (Task 3).
   - `completeDeferred()` completes every still-deferred tile without evaluation, then emits whatever closes.
2. **The executor's final pass** calls `notifyTileSkipped` for a tile `evaluate` reports as skipped, and `notifyTileComplete` otherwise. After the pass it loops:
   1. take the awaited deferred tiles;
   2. evaluate them with the skip bypassed, through the same worker pool, reservation and cancellation as the pass;
   3. `notifyTileComplete` each one;
   4. repeat until the list is empty.

   Then it calls `completeDeferred()`, then `resolve`. Each tile is evaluated at most once. Only objects reaching out of the tiles the pass evaluated demand tiles (decision 1), so the demands stop at the skipped tiles those objects touch; the objects a demanded tile finds on its own pull in nothing more. One very large object can still pull in many tiles. That costs time, never correctness, and UR-IMG-5's "reads only intersecting tiles" becomes a strong tendency rather than a guarantee. The SDS says so.
3. **A demanded tile is an ordinary evaluated tile**, except in three ways:
   - it writes no raster tile, so raster output is identical to before, when a skipped tile wrote nothing;
   - it does not report tile progress, because the pass already counted it done;
   - it is counted in a new `RunCounters::tilesEvaluatedOnDemand`, and stays counted in `tilesSkipped`.

   Its owned tile objects go through the normal `TileObjectFilter`. Its fragments go to the merger, and admission by anchor still decides.
4. **Accumulation passes are unchanged.** They still skip, so global statistics do not move.
5. **Adjacent risk: `skips()` and non-integer scales.** At a non-integer scale, a tile's first level-0 column or row maps to the previous level pixel, which may be set. So `skips()`, which tests only the tile rect, could skip a tile that owns an admissible object. Task 3 tests this. If the test fails, `skips()` tests the same rect `buildTileMask` builds (including that extra column or row) instead of the bare tile rect.

## Review Focus

1. **An object spanning three or more tiles beyond the mask edge (a chain).** It is stored whole, with every tile it touches evaluated on demand, and no others. Task 2, `SeamRun.RestrictedObjectSpanningSeveralSkippedTilesIsWhole`.
2. **The order of completion versus fragment arrival.** A skipped tile that finishes before the fragment expecting it arrives must still be demanded. Task 1, `SeamMerger.SkippedTileCompletedBeforeTheFragmentIsStillAwaited`.
3. **Worker counts 1, 4 and 8** give identical keys, geometry and measurements for a restricted run with boundary objects. Task 2, `SeamRun.RestrictedRunIsWorkerCountInvariant`.
4. **Raster output and progress.** A restricted run that also writes a raster writes the same tiles as before, and progress never exceeds the total. Task 2, `SeamRun.DemandedTilesWriteNoRasterAndReportNoProgress`.
5. **Cancellation during the on-demand loop** throws `CancelledError` and resolves nothing partial. Task 2, `SeamRun.CancellationDuringDemandedTiles`.

---

### Task 1: The merger's deferred tiles

**Files:**
- Modify: `src/pipeline/detail/seam_merger_access.hpp` (the Impl state, and the three `SeamMergerAccess` statics), `src/pipeline/seam_merger.cpp`
- Test: `tests/pipeline/test_seam_merger.cpp`

**Interfaces:**
- Produces (`detail::SeamMergerAccess`):
  - `static void notifyTileSkipped(SeamMerger&, int64_t tileIndex);`
  - `static std::vector<int64_t> takeAwaitedDeferred(SeamMerger&);`
  - `static void completeDeferred(SeamMerger&, ObjectSink& sink);`
- The tile state replaces the binary `completed_` with a small enum. `add()` records an expectation for any tile that is not completed. `complete(t)` is unchanged for evaluated tiles. Thread-safety matches `notifyTileComplete`.

- [ ] **Step 1: Write the failing tests:**
  - `SeamMerger.SkippedTileKeepsItsComponentOpen`. A fragment in tile 0 expects tile 1; tile 1 is skipped. After `notifyTileComplete(0)` nothing is emitted, and `takeAwaitedDeferred` returns `{1}`. Then `notifyTileComplete(1)` with the matching fragment emits one whole object.
  - `SeamMerger.SkippedTileCompletedBeforeTheFragmentIsStillAwaited` (Review Focus 2). Tile 1 is skipped first, then tile 0's fragment expecting tile 1 is added. `takeAwaitedDeferred` returns `{1}`.
  - `SeamMerger.AwaitedDeferredIsReturnedOnce`.
  - `SeamMerger.CompleteDeferredClosesWhatNothingAwaits`. A deferred tile nobody expects is completed by `completeDeferred`, emits nothing, and leaves no component open, so `resolve` succeeds.
  - `SeamMerger.CompletionOrderDoesNotChangeTheResult` is extended with skipped tiles.
- [ ] **Step 2: Run;** expect a compile failure. **Step 3: Implement.** **Step 4: Run** `-R SeamMerger`, then the full suite.
- [ ] **Step 5: Commit** `fix(pipeline): the seam merger defers skipped tiles instead of completing them`.

### Task 2: On-demand evaluation in the executor

**Files:**
- Modify: `src/pipeline/executor.cpp` (the final pass and the post-pass loop), `src/pipeline/detail/tile_evaluator.{hpp,cpp}` (forced evaluation, no raster write, the counter), `src/pipeline/executor.hpp` if it documents skipping (~117, 164, 180, 232)
- Test: `tests/pipeline/test_seam_run.cpp`

**Interfaces:**
- Consumes: Task 1.
- Produces:
  - `TileOutcome TileEvaluator::evaluate(int64_t tileIndex, const CancellationToken&, Operation*, EvaluationMode mode = EvaluationMode::Normal)`. `EvaluationMode::OnDemand` bypasses `skips`, writes no raster tile and increments `tilesEvaluatedOnDemand`. Keep the existing parameter types; only the trailing defaulted enum is added.
  - `RunCounters::tilesEvaluatedOnDemand`.
  - `runPass` (or a sibling helper) runs an explicit list of tile indices through the same workers, reservation and cancellation. For on-demand tiles it does not call `onProgress`.

- [ ] **Step 1: Write the failing tests.** Use `ts::makeSyntheticScene`, `ts::DiscMaskSource`, `test.components` with `seams=true`, 64-pixel tiles, and `sortedKeys` and `crossesSeam` from this file. "Reference" means the unrestricted run with its objects filtered by `DiscMaskSource::contains(anchor)`.
  - `SeamRun.RestrictedObjectCrossingIntoASkippedTileIsWhole`. A 41-pixel blob crosses the seam at x=64. The disc mask covers the blob's anchor but no pixel of tile x=64..127. The restricted keys (geometry and measurements, not ids only) equal the reference.
  - `SeamRun.RestrictedObjectSpanningSeveralSkippedTilesIsWhole` (Review Focus 1). A 200-pixel-wide blob runs from inside the mask across three skipped tiles. It equals the reference, and `tilesEvaluatedOnDemand == 3`.
  - `SeamRun.RestrictedRunIsWorkerCountInvariant` (Review Focus 3). Workers 1, 4 and 8 give identical keys.
  - `SeamRun.DemandedTilesWriteNoRasterAndReportNoProgress` (Review Focus 4). A chain that also writes a raster has the same `tilesWritten` as before the fix, which is the count of tiles that hold a mask pixel. The last progress `done` equals `total`.
  - `SeamRun.CancellationDuringDemandedTiles` (Review Focus 5). Cancel from inside the first on-demand tile's apply, using a probe operation or the token. The run throws `CancelledError`.
  - `SeamRun.UnrestrictedRunIsUnchanged`. With no mask, `tilesEvaluatedOnDemand == 0`, and the keys equal those of the current code. The existing seam-run tests cover this; add the counter assertion.
  - Check that `SeamRun.RestrictionAppliesToMergedObjects` and `GlobalContext.SkippedTilesDoNotStallTheFold` still pass unchanged.
- [ ] **Step 2: Run;** expect FAIL. Without the fix, the blob tests return the truncated object.
- [ ] **Step 3: Implement** decisions 2 and 3.
- [ ] **Step 4: Run** `-R "SeamRun|SeamMerger|TileExecutor|GlobalContext|Restriction"`, then again with `--repeat until-fail:3 -j8`, then the full suite.
- [ ] **Step 5: Commit** `fix(pipeline): evaluate skipped tiles an open object still expects`.

### Task 3: The non-integer-scale skip, the workspace reproducer and the docs

**Files:**
- Modify: `src/pipeline/detail/restriction.cpp` (only if the test fails), `tests/pipeline/test_restriction.cpp`
- Modify: `tests/workspace/test_stage_runner.cpp` (enable the reproducer)
- Modify: `docs/superpowers/specs/sds/05-pipeline.md`, `docs/project.md`, `docs/traceability.md`

**Interfaces:**
- Consumes: Tasks 1 and 2.

- [ ] **Step 1: Write the test** `Restriction.SkipAgreesWithTheBuiltMaskAtANonIntegerScale`. At a level whose scale is not an integer, use a mask whose only set level pixel is the one a tile's first column maps to. A tile-owned object anchored there must be emitted, with the same keys as the unrestricted run filtered by anchor.
- [ ] **Step 2: Run it.** If it fails, apply decision 5's fix and re-run. If it passes, record that the risk does not occur and keep the test as a guard.
- [ ] **Step 3:** Rename `StageRunner.DISABLED_RestrictedObjectCrossingIntoASkippedTileIsNotTruncated` to drop the `DISABLED_` prefix, then run it. It passes, with a disc 41 pixels wide.
- [ ] **Step 4: Docs.**
  - In SDS 05, replace the limitation note with the deferred-tile rule. Update every "a skipped tile counts as complete" sentence to the three-state model. State the cost trade-off from decision 2.
  - In `docs/project.md`, move the "must fix before release" entry to resolved, with this plan's name.
  - Update traceability (UR-IMG-5, UR-STG-3).
  - Build the docs.
- [ ] **Step 5: Run** the full suite, then **commit** `fix(pipeline): enable the restricted-truncation reproducer; SDS 05 deferred tiles`.
