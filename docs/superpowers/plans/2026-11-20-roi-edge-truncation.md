# ROI and Polygon Edge Truncation Fix Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A tiled run restricted by an ROI or a polygon must store every admitted object whole, and only admitted objects. Today an object that crosses the edge of the grid region (the ROI, or the polygon's bounding box) is stored cut at that edge. On any of the four edges it can then be stored with a wrong anchor, a wrong id and wrong measurements, or stored when its true anchor lies outside and it should have been dropped.

**Architecture:**
- An object-producing tiled chain gets a grid that spans the level, as an unrestricted or mask-restricted run already has. The ROI and the polygon stay in the restriction mask, which already rasterises both. Tiles outside the region are skipped, so the deferred-tile machinery from the mask fix (on-demand evaluation of skipped tiles an open object expects) carries objects across the region edge with no new merger code.
- The region (ROI ∩ polygon bounding box, at the level) moves from being the grid to being a `RunPlan` field. It still bounds `Pipeline::rect`, the raster tiles written and the pixels an accumulation pass applies to, so results other than objects do not change.
- Tiles whose owned rect misses the region are skipped without building a mask, so a small ROI on a large scene costs one rect test per extra tile.

**Tech Stack:** C++17, CMake, GoogleTest, MSVC.

**Spec:** `docs/superpowers/specs/sds/05-pipeline.md`:
- §5.5, the three-point restriction table at ~949-955 (its "Grid construction" row changes) and the "Limitation: the grid-region edge" note at ~1344-1351, which this plan removes;
- §5.6, the grid-region paragraph at ~1593-1595 and the halo paragraph at ~1662-1667 ("halo beyond the region but inside the level reads real pixels");
- UR-IMG-5 ("results match those obtained by processing the whole slide and cropping") and UR-STG-3 ("produces no objects outside those regions").

**Base:** branch `fix/roi-edge-truncation` from `feat/workspace-c` (PR #17, head bb0bfdb), because the deferred-tile machinery lives there. Open the PR against `main` once #17 has merged; until then, against `feat/workspace-c`.

## Global Constraints

- Build `./build.sh --skip-slideio`; tests `./run-tests.sh` (all must pass); one test `ctest --test-dir build -C Release -R <name>`; a disabled test: `PATH=build/slideio-install/release/bin:build/bin/Release:$PATH build/bin/Release/slideio_processor_pipeline_tests.exe --gtest_also_run_disabled_tests --gtest_filter=<name>`; docs `cmake --build build --target docs --config Release`.
- Zero compiler warnings and zero Doxygen warnings.
- No public header changes. `Pipeline::rect(level)` keeps returning the region.
- Runs with no object producer and whole-level chains behave exactly as before: same grid, same `tilesTotal`, same rasters, same reductions.
- Unrestricted and mask-only runs are unchanged (their region already equals the level).
- Results must stay identical across worker counts. `TileExecutor.ResultsIdenticalAcrossWorkerCounts`, `Seg3.IdenticalAcrossWorkerCounts`, `SeamRun.WorkerCountInvariant` and `GlobalContext.FloatAccumulationIsWorkerCountInvariant` must pass unchanged.
- Messages never carry pixel, coordinate or parameter values.
- Every source and test file starts with its `// Requirements: ...` line. `docs/traceability.md` lists every new test, one row per requirement, updated in place.
- Match the surrounding code's idiom and comment density. There is no Python; edit with the Edit tool or sed.

## Decisions

1. **Which runs get the level grid.** A run whose chain produces objects (some `targetLayers` entry valid) and has no WholeLevel operation builds its `TileGrid` over `plan.levelRect`. Every other run keeps the grid over the region. For unrestricted and mask-only runs the two are the same rect, so only ROI- and polygon-restricted object runs change.
2. **`RunPlan::region`.** A new field, `Rect64 region`, holds the region at the level (today's grid rect). `PipelineSceneSource` uses it in place of `gridRegion(plan->grid)`, so `Pipeline::rect`, the lazy `readBlock` clip and `asScene` are unchanged.
3. **Raster writes stay inside the region.** The final pass writes `tile ∩ plan.region` (the `BlockView` over that rect) and writes nothing when the intersection is empty. A tile evaluated on demand still writes nothing.
4. **Accumulation passes see what they saw before.** In an accumulation pass, `TileEvaluator::evaluate` applies over `tile ∩ plan.region` instead of the grid tile, with the halo grown from that rect as before. `mask()` is advisory (§5.1), so an operation that ignores it would otherwise count pixels of a straddling tile beyond the ROI. With the clip, its pixel set is exactly the region's, as before. Float fold order can change with the new tile boundaries; that is accepted, and worker-count invariance still holds.
5. **The final pass applies over the whole tile.** A straddling tile is evaluated over its full grid rect, so its seams to tiles outside the region are real seams. Its out-of-region pixels never reach a raster (decision 3) or an object (the anchor filter).
6. **Skipping outside the region.** `RestrictionFilter::outsideRegion(tileIndex)` is true when `ownedRectAtLevel(tileIndex)` does not intersect `plan.region`. In a pass, `TileEvaluator::evaluate` tests it before `buildTileMask` and treats a true answer exactly like a skip (counted in `tilesSkipped`, `TileOutcome{true, 0}`, deferred by the executor). It is only a shortcut: the mask is zero there anyway, because the ROI is rasterised by the outward mapping that also gives the region. The polygon's conservative raster can set the pixel just past its bounding box's outward mapping (and an anchor floors to the level pixel before the region), so at a non-integer level scale the test uses the region widened by one level pixel on every side; at an integer scale it is not widened. An on-demand evaluation still builds the mask.
7. **Progress.** An ROI- or polygon-restricted object run now reports the level grid in `tilesTotal`, and its out-of-region tiles as fast progress. This is the documented rule for mask restrictions already ("`tilesTotal` counts the grid").
8. **Per-tile memory.** The tile-bytes check uses `min(tileWidth, plan.levelRect.width)` × `min(tileHeight, plan.levelRect.height)` for a level-grid run, because a straddling tile is full size. A tiny ROI can therefore now be refused by the ceiling where it was accepted before; the refusal text is unchanged.
9. **Whole-level chains are out of scope.** A WholeLevel chain analyses the region as one image, so its objects stop at the region edge. That is a different rule (the chain never sees beyond the region) and is recorded as such in §5.5, not fixed here.

## Review Focus

1. An ROI whose right or bottom edge is the scene edge: no tile lies beyond it, and objects there are stored as in an unrestricted run (`SeamRun.RoiAtTheSceneEdgeMatchesTheUnrestrictedRun`, Task 3).
2. An object crossing the ROI's left edge with its anchor inside, and one crossing the top edge with its anchor outside: the first is stored whole, the second dropped (asserted counts in `SeamRun.RoiEdgeObjectIsNotTruncated`, Task 3).
3. An ROI at a non-integer level scale: `outsideRegion` never skips a tile whose mask has a set pixel in its owned rect (`Restriction.OutsideRegionAgreesWithTheBuiltMaskAtANonIntegerScale`, Task 2).
4. A mask-ignoring reduction in an ROI run counts exactly the region's pixels (`GlobalContext.RoiAccumulationSeesOnlyTheRegion`, Task 1).
5. An ROI combined with a mask source: objects crossing the ROI edge are whole and both restrictions still decide by anchor (`SeamRun.RoiWithAMaskSourceStoresWholeObjects`, Task 3).

---

### Task 1: The level grid for restricted object runs

**Files:**
- Modify: `src/pipeline/detail/run_plan.hpp` (new `region` field), `src/pipeline/detail/run_plan.cpp:475-500` (grid rect, tile bytes)
- Modify: `src/pipeline/detail/pipeline_scene_source.cpp:37-41,190` (use `plan->region`; drop `gridRegion` if unused)
- Modify: `src/pipeline/detail/tile_evaluator.cpp:191-192,350-356` (accumulation clip, raster write clip)
- Test: `tests/pipeline/test_restriction.cpp`, `tests/pipeline/test_global_context.cpp`, `tests/pipeline/test_pipeline_validate.cpp`

**Interfaces:**
- Produces: `RunPlan::region` (`Rect64`, level pixels; equals the grid's rect for every run that keeps the region grid). Tasks 2 and 3 use it.

- [ ] **Step 1: Write the failing tests**
  - `Restriction.PolygonObjectRunGridSpansTheLevel` replaces `Restriction.PolygonGridSpansItsBoundingBox`. Same polygon (`triangle(70, 70, 330, 200)`). With `test.maskComponents` bound to a layer, the last progress call reports `(kSide / kTile) * (kSide / kTile)` tiles, and `p.rect(0)` equals the polygon's bounding box mapped to level 0. With `test.threshold` (no objects) it still reports 15 tiles.
  - `Restriction.RoiObjectRunWritesRasterOnlyInsideTheRegion`: ROI `{70, 70, 260, 130}`, an object-producing chain with a raster sink that records each written `BlockView` rect. Every rect lies inside `p.rect(0)`, and the union of the rects equals the union written by the same ROI run with `test.threshold` (the region grid).
  - `GlobalContext.RoiAccumulationSeesOnlyTheRegion`: a test accumulating operation that ignores `mask()` and sums the pixel count of every apply's tile rect, in an ROI-restricted object run whose ROI edges are not tile-aligned. The finalized count equals the ROI's area at the level.
  - `PipelineValidate.LevelGridTileBytesUseTheFullTile`: an ROI smaller than one tile, an object chain, and a ceiling between the ROI-sized tile's need and the full tile's need. `validate()` refuses it with the existing tile-bytes message. The same spec without an object producer validates.

- [ ] **Step 2: Run them to verify they fail**

Run: `ctest --test-dir build -C Release -R "PolygonObjectRunGridSpansTheLevel|RoiObjectRunWritesRasterOnlyInsideTheRegion|RoiAccumulationSeesOnlyTheRegion|LevelGridTileBytesUseTheFullTile"`
Expected: FAIL (the grid is still the region; the validate test passes the ceiling).

- [ ] **Step 3: Implement decisions 1-5 and 8**
  - `RunPlan::region`, set from the region computed today. The grid rect is `plan.levelRect` when the chain produces objects and is not whole-level, else `region`.
  - `PipelineSceneSource` takes the level's rect from `plan->region`.
  - In `TileEvaluator::evaluate`, the rect the chain applies over is `grid.tileRectAtLevel(tileIndex)` in the final pass and its intersection with `plan_.region` in an accumulation pass. The raster write uses `tile ∩ plan_.region` and is skipped when that is empty.
  - Existing tests that pin the bounding-box grid of an object run (`Restriction.EmptyMaskTilesReadNoPixels` and any other the suite shows failing) are updated to the level grid; list each one and why in the report.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `./run-tests.sh`
Expected: all pass. `SeamRun.DISABLED_RoiEdgeObjectIsNotTruncated`, run explicitly, now passes too.

- [ ] **Step 5: Commit**

```bash
git add src/pipeline tests/pipeline docs/traceability.md
git commit -m "fix(pipeline): restricted object runs tile the whole level; region bounds rasters and accumulation"
```

### Task 2: Skip tiles outside the region without a mask

**Files:**
- Modify: `src/pipeline/detail/restriction.hpp`, `src/pipeline/detail/restriction.cpp`
- Modify: `src/pipeline/detail/tile_evaluator.cpp:195-207`
- Test: `tests/pipeline/test_restriction.cpp`

**Interfaces:**
- Consumes: `RunPlan::region` (Task 1).
- Produces: `bool RestrictionFilter::outsideRegion(int64_t tileIndex) const;`, documented as in decision 6. False for an unrestricted run.

- [ ] **Step 1: Write the failing tests**
  - `Restriction.OutsideRegionTilesBuildNoMask`: an ROI plus a counting `TileMaskSource`, in an object run. `fillMask` is called for no tile whose owned rect misses the ROI, and `tilesSkipped` is the same as without the shortcut (assert the exact count computed from the grid).
  - `Restriction.OutsideRegionAgreesWithTheBuiltMaskAtANonIntegerScale`: at level 1 of a scene with a non-integer level scale, for every tile of an ROI object run, `outsideRegion(t)` implies `skips(buildTileMask(t), t)`. Add a variant with a halo and one with a polygon, keyed and compared like the existing `SkipAgreesWithTheBuiltMaskAtANonIntegerScale` tests.

- [ ] **Step 2: Run them to verify they fail**

Run: `ctest --test-dir build -C Release -R "OutsideRegion"`
Expected: FAIL to compile (`outsideRegion` does not exist).

- [ ] **Step 3: Implement `outsideRegion` and call it in `evaluate` before `buildTileMask`, in a pass only (decision 6).**

- [ ] **Step 4: Run the tests to verify they pass**

Run: `./run-tests.sh`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add src/pipeline/detail tests/pipeline docs/traceability.md
git commit -m "perf(pipeline): skip tiles outside the restriction region without building a mask"
```

### Task 3: Reproducers, edge cases and documentation

**Files:**
- Modify: `tests/pipeline/test_seam_run.cpp:287-322` (enable the reproducer; new tests)
- Modify: `docs/superpowers/specs/sds/05-pipeline.md`, `docs/project.md:241-249`, `docs/traceability.md`

- [ ] **Step 1: Enable and extend the tests**
  - Rename `SeamRun.DISABLED_RoiEdgeObjectIsNotTruncated` to `SeamRun.RoiEdgeObjectIsNotTruncated` and drop its "known limitation" comment. Besides the existing right/bottom count, assert that the fixture has at least one object crossing the ROI's left edge with its anchor inside, and at least one crossing the top edge whose cut part has a corner inside the ROI (so the old code stored it wrongly). Run it at 1, 4 and 8 workers.
  - `SeamRun.PolygonEdgeObjectIsNotTruncated`: the same comparison for a polygon whose bounding-box edges cut blobs: expected = the unrestricted run's objects whose anchor the polygon's mask admits, compared by `sortedKeys`.
  - `SeamRun.RoiAtTheSceneEdgeMatchesTheUnrestrictedRun`: an ROI from `{256, 256}` to the scene's bottom-right corner; the stored keys equal the unrestricted run's objects anchored in it.
  - `SeamRun.RoiWithAMaskSourceStoresWholeObjects`: the ROI from the reproducer plus a `ts::DiscMaskSource`; expected = unrestricted objects whose anchor is in both, compared by keys.
  - Each test asserts `tilesEvaluatedOnDemand > 0` where the fixture has objects crossing an edge into a tile outside the region.

- [ ] **Step 2: Run them**

Run: `ctest --test-dir build -C Release -R "SeamRun"`
Expected: all pass (Tasks 1 and 2 carry the fix). A failure here is a defect in Task 1 or 2, not in the test.

- [ ] **Step 3: Update the documentation**
  - SDS 05 §5.5: the table's "Grid construction" row becomes "An object-producing tiled run's grid spans the level, others span the restriction's bounding box (5.6); tiles outside the region are skipped without a mask". Delete "Limitation: the grid-region edge" and add the whole-level rule (decision 9).
  - SDS 05 §5.6: the region paragraph at ~1593 says what the region still bounds (decisions 2-4), what `tilesTotal` reports (decision 7) and the tile-bytes rule (decision 8).
  - `docs/project.md`: move the "Must fix before release" ROI entry to "Resolved", with the same QA/regulatory note as the mask fix: ids of affected objects change, objects previously stored cut become whole, objects whose true anchor is outside are no longer stored, so counts can fall, `tilesTotal` grows for ROI and polygon object runs, and earlier ROI- or polygon-restricted results should be re-run and the change recorded in verification evidence.
  - `docs/traceability.md`: every new and renamed test, under UR-IMG-5 and UR-STG-3.

- [ ] **Step 4: Verify**

Run: `./run-tests.sh` and `cmake --build build --target docs --config Release`
Expected: all tests pass, no disabled test left for this route, zero Doxygen warnings.

- [ ] **Step 5: Commit**

```bash
git add tests/pipeline docs
git commit -m "fix(pipeline): enable the ROI-edge reproducer; polygon and edge cases; SDS 05 region rules"
```
