# Exact ROI Admission Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** At every analysis level, an ROI-restricted run keeps exactly the objects whose level-0 anchor lies in the ROI rectangle, half-open (`x ≤ ax < x + width`, likewise in y).

**Architecture:**
- Today the ROI is decided at level resolution, with two mismatched rules. The mask sets the level pixels of `LevelSelector::mapToLevel(roi)`, whose pixel edges come from `llround(L × scale)`. The anchor is then tested at the level pixel `floor(anchor / scale)`.
- The two disagree at a non-integer scale. At scale 2.4, the corner of level pixel 128 is level-0 x 307, but 307 floors to 127. So an ROI starting at 307 refuses an anchor at 307, and one ending at 307 admits it.
- At any level above 0 with an ROI edge inside a level pixel, an anchor up to one level pixel outside the ROI is admitted, at integer scales too. For example, at scale 2 an ROI starting at 307 admits an anchor at 306. The follow-up note in `docs/project.md` claims integer scales are exact; that is wrong.
- The fix tests the ROI exactly in level-0 coordinates, in both places an anchor is admitted: `RestrictionFilter::admits` and `TileObjectFilter::admits`. The polygon and the mask source keep their level-mask test.
- The mask's ROI part still decides skipping and is still handed to operations. It becomes the union of `mapToLevel(roi)` and the level pixels that hold an ROI anchor under `floor`, so it never clears a pixel where an admissible anchor is tested.

**Tech Stack:** C++17, CMake, GoogleTest, MSVC.

**Spec:** `docs/superpowers/specs/sds/05-pipeline.md` §5.5: the three-point restriction table, "The test is the anchor" (~971) and "the authoritative one" (object emission). UR-STG-3 ("produces no objects outside those regions") and UR-IMG-5 ("results match those obtained by processing the whole slide and cropping").

**Base:** branch `fix/roi-exact-admission` from `main` (8e0b2f3).

## Global Constraints

- Build `./build.sh --skip-slideio`; tests `./run-tests.sh` (all must pass); one test `ctest --test-dir build -C Release -R <name>`; docs `cmake --build build --target docs --config Release`.
- Zero compiler warnings and zero Doxygen warnings.
- No public header changes. Everything here is in `src/pipeline/detail/`.
- Unrestricted runs, and runs restricted only by a polygon or a mask source, are unchanged.
- At level 0 an ROI run is unchanged: there the mask's rule is already exact.
- Results must stay identical across worker counts. `SeamRun.RestrictedRunIsWorkerCountInvariant` and `TileExecutor.ResultsIdenticalAcrossWorkerCounts` must pass unchanged.
- Messages never carry pixel, coordinate or parameter values.
- Every source and test file starts with its `// Requirements: ...` line. `docs/traceability.md` lists every new test, one row per requirement, updated in place.
- Match the surrounding code's idiom and comment density.

## Decisions

1. **ROI admission is a level-0 test.** `RestrictionFilter::inRoi(Point64 anchorLevel0)` is true when the run has no ROI, or when `roi.x ≤ a.x < roi.x + roi.width` and the same holds in y, with `roi` taken from `PipelineSpec::roi()`. `admits()` returns false when `inRoi` is false, before any mask test, including the uncached one-pixel path. `TileObjectFilter` takes a `const RestrictionFilter*` (null for an unrestricted run) and refuses an anchor `inRoi` rejects. It counts that refusal in `droppedByRestriction`, as it counts a mask refusal.
2. **The mask's ROI part covers both rules.** `RestrictionFilter`'s level rect for the ROI (today `roiAtLevel_ = mapToLevel(roi)`) becomes the bounding union of:
   - `mapToLevel(roi)`;
   - the pixels holding an ROI anchor, `[levelPixelOf(roi.x), levelPixelOf(roi.x + roi.width − 1)]` in x, and the same in y.

   Each is a contiguous interval and they overlap, so the union is one rect. At an integer scale the second is contained in the first, so the mask does not change. At a non-integer scale it grows by at most one level pixel on its left and top edges.
3. **What that changes for operations.** `Operation::mask()` is advisory (§5.1). An operation that honours it, such as the Otsu threshold restricted to an ROI, can see one more edge column or row at a non-integer scale, never fewer. `readBlock`'s "outside the restriction is zero" moves with it. This is accepted and documented. The mask stays what skipping and operations see; only emission is exact.
4. **Skipping stays sound.** `skips()` and `outsideRegion()` are unchanged:
   - Every admissible anchor is tested at a pixel the widened mask sets (decision 2), so `skips()` keeps every tile holding one.
   - `outsideRegion()` already widens `plan.region` by one level pixel on every side at a non-integer scale. That covers the added pixels: `floor(x0/s) ≥ llround-based start − 1`, and `floor((x1−1)/s) ≤ llround-based end`.
5. **Out of scope, recorded:** the polygon and the mask source are still tested at `floor(anchor / scale)`. At a non-integer scale, roughly half the time that pixel is the one before the level pixel whose corner the anchor is, because the level↔level-0 mapping uses `llround(L × scale)` for pixel corners. So a mask-source restriction (the workspace restrict-to-layer path) can test an object's anchor one level pixel to its left or above. This is a core convention question (`RasterMask::testLevel0`, a public header), not an ROI one. Task 2 records it as an open follow-up in `docs/project.md`.

## Review Focus

1. An anchor exactly on the ROI's first column or row at a non-integer scale is admitted (`Restriction.RoiAdmitsExactlyItsLevel0Anchors`, Task 1).
2. An anchor one column before the ROI, inside the same level pixel, at an integer scale above 1, is refused (same test, scale-2 cases).
3. A tile whose only admissible anchors lie in the added edge pixel is not skipped (`Restriction.RoiMaskSetsEveryPixelHoldingAnRoiAnchor`, Task 1).
4. An ROI combined with a mask source: both still decide, with the ROI exact (`Restriction.RoiAndMaskSourceBothDecide`, Task 1).
5. A level-1 run's stored objects equal the unrestricted run's objects anchored in the ROI, at 1 and 4 workers (`SeamRun.RoiAtLevelOneKeepsExactlyItsAnchors`, Task 2).

---

### Task 1: Exact ROI admission

**Files:**
- Modify: `src/pipeline/detail/restriction.hpp`, `src/pipeline/detail/restriction.cpp:12-24` (constructor), `:162-213` (`admits`)
- Modify: `src/pipeline/detail/tile_evaluator.hpp:56`, `src/pipeline/detail/tile_evaluator.cpp:34-73`
- Test: `tests/pipeline/test_restriction.cpp`

**Interfaces:**
- Produces: `bool RestrictionFilter::inRoi(Point64 anchorLevel0) const noexcept;` (public). `TileObjectFilter(ObjectSink* target, const Rect64& tileRect, const RasterMask* mask, const RestrictionFilter* restriction, RunCounters& counters);`.

- [ ] **Step 1: Write the failing tests.** Build the plan and the filter as `checkAdmits` does (`detail::PipelineAccess`).
  - `Restriction.RoiAdmitsExactlyItsLevel0Anchors`:
    - On the 600 → 250 pyramid (scale 2.4) at level 1, ROI `{307, 307, 100, 100}`: `admits({307, 307})` and `admits({406, 406})` are true; `admits({306, 307})`, `admits({307, 306})`, `admits({407, 406})` and `admits({406, 407})` are false.
    - Same pyramid, ROI `{308, 308, 99, 99}`, whose start lies inside level pixel 128: `admits({307, 308})` is false and `admits({308, 308})` is true.
    - On a 512 → 256 pyramid (scale 2) at level 1, ROI `{307, 307, 100, 100}`: `admits({306, 307})` is false and `admits({307, 307})` is true.
    - `inRoi` agrees with each case.
  - `Restriction.RoiMaskSetsEveryPixelHoldingAnRoiAnchor`: at level 1 of the 600 → 250 pyramid, for both ROIs above, a mask built over the whole level by the filter (through `buildTileMask` of every tile) has `testLevel0(a)` true for every level-0 anchor `a` on the ROI's first and last columns and rows.
  - `Restriction.RoiAndMaskSourceBothDecide`: ROI `{307, 307, 100, 100}` plus `ts::DiscMaskSource` at level 1 (scale 2.4). An anchor inside the ROI but outside the disc is refused. An anchor inside the disc but at x 306 is refused. An anchor in both is admitted.
  - Update `checkAdmits`'s oracle to the new rule: `admits` equals the tile mask's answer AND `inRoi`. Say so in a comment.

- [ ] **Step 2: Run them to verify they fail**

Run: `ctest --test-dir build -C Release -R "Restriction\."`
Expected: the new tests fail to compile (`inRoi` does not exist). After a stub that returns true they fail on the edge anchors.

- [ ] **Step 3: Implement decisions 1 and 2.** Keep the ROI in level 0 on the filter (`roi_`, from `spec.roi()`). Compute the widened level rect in the constructor with `levelPixelOf`. Pass `&restriction_` (or null when unrestricted) to `TileObjectFilter` in `TileEvaluator::evaluate`. Update the class doc's ROI paragraph (restriction.hpp:28-29) to say the mask sets both pixel sets and admission tests the ROI in level 0.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `./run-tests.sh`
Expected: all pass, with the existing level-0 ROI tests unchanged.

- [ ] **Step 5: Commit**

```bash
git add src/pipeline/detail tests/pipeline docs/traceability.md
git commit -m "fix(pipeline): admit ROI anchors exactly in level 0; ROI mask covers every anchor pixel"
```

### Task 2: End-to-end runs and documentation

**Files:**
- Modify: `tests/pipeline/test_seam_run.cpp`
- Modify: `docs/superpowers/specs/sds/05-pipeline.md` (§5.5 "The test is the anchor" and the restriction table), `docs/project.md:271-279`, `docs/traceability.md`

**Interfaces:**
- Consumes: `RestrictionFilter::inRoi` (Task 1).

- [ ] **Step 1: Write the tests**
  - `SeamRun.RoiAtLevelOneKeepsExactlyItsAnchors`:
    - Use a two-level synthetic scene of blobs (600 → 250, scale 2.4) and the object chain of the existing `SeamRun` tests, at level 1.
    - Run it unrestricted, then with ROI `{308, 230, 211, 189}`, whose edges lie inside level pixels. Run the restricted run at 1 and 4 workers.
    - The stored `sortedKeys` equal the unrestricted objects whose anchor `inRoi` admits.
    - Assert at least one unrestricted object whose anchor is in the ROI's edge level pixel but outside the ROI, so the test can fail. Pick other edge values that cut blobs if these do not, and record them in the report.
  - `SeamRun.RoiAtLevelOneOfAnIntegerScaleKeepsExactlyItsAnchors`: the same check on a 512 → 256 pyramid (scale 2).

- [ ] **Step 2: Run them.** Confirm both fail on `main` (temporarily `git checkout main -- src/`, build, run, then `git checkout HEAD -- src/` and rebuild).

Run: `ctest --test-dir build -C Release -R "SeamRun\.RoiAtLevelOne"`
Expected: FAIL on `main`'s src, PASS on the branch.

- [ ] **Step 3: Update the documentation**
  - SDS 05 §5.5 "The test is the anchor": an ROI is tested exactly, in level 0, at the anchor; a polygon or a mask source is tested on the level mask at the anchor's level pixel. Add the advisory-mask note from decision 3.
  - `docs/project.md`: move the ROI admission follow-up to resolved, and correct its claim that integer scales were exact. Add the QA/regulatory note: at levels above 0, objects anchored just outside an ROI edge are no longer stored, and ones on its first column or row no longer dropped, so counts at an ROI's edges can change; re-run ROI-restricted stages at levels above 0 and record the change in verification evidence. Add a new open follow-up for decision 5, the floor-versus-`llround` level pixel of an anchor tested against a polygon or a mask source.
  - `docs/traceability.md`: every new test, under UR-IMG-5 and UR-STG-3.

- [ ] **Step 4: Verify**

Run: `./run-tests.sh` and `cmake --build build --target docs --config Release`
Expected: all tests pass, zero Doxygen warnings.

- [ ] **Step 5: Commit**

```bash
git add tests/pipeline docs
git commit -m "test(pipeline): ROI runs at level 1 keep exactly their anchors; SDS 05 ROI admission rule"
```
