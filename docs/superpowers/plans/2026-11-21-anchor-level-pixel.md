# Anchor Level Pixel Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** At every level, a polygon or mask-source restriction tests an object's anchor at the level pixel the object was found in. Today the test can land one pixel to the left or above.

**Architecture:**
- **The two conventions.**
  - The library maps a level pixel's corner to level 0 with `llround(L × scale)`, through `detail::levelToLevel0`. Tile ownership, `OperationContext` and object geometry all use this mapping.
  - `RasterMask::testLevel0` maps a level-0 point back with `floor(x / scale)`.
- **Where they disagree.** At a non-integer scale, if `llround` rounds `P × scale` down, then `floor(llround(P × scale) / scale)` is `P − 1`. So an object found at level pixel P is tested at the pixel before it.
  - Example at scale 2.4: level column 126 starts at level-0 302 (302.4 rounded), and 302 / 2.4 floors to 125.
  - A mask source that sets only column 126 therefore refuses the objects found in column 126. This is the workspace restrict-to-layer path.
- **The fix.** `testLevel0` tests the level pixel `L` that is the largest with `levelToLevel0(L, scale) ≤ x` (likewise y). That is the same inverse `LevelSelector::mapToLevel` already uses for a rectangle's near edge.
  - To make that possible, the single mapping moves from `src/pipeline/detail/` to `src/core/detail/`, so that core and pipeline share it.
  - `RestrictionFilter::levelPixelOf` uses the same function.
  - With one convention everywhere, the floor-only widening code becomes dead, and Task 2 removes it:
    - the extra column and row in `ownedRectAtLevel`;
    - the widened zero halo in `buildTileMask`;
    - the ROI union in the constructor.

**Tech Stack:** C++17, CMake, GoogleTest, MSVC.

**Spec:**
- `docs/superpowers/specs/sds/01-core.md` §1.17, `RasterMask::testLevel0`.
- `docs/superpowers/specs/sds/05-pipeline.md` §5.5 "The test is the anchor" (~971).
- UR-STG-3: "produces no objects outside those regions".
- UR-IMG-5: "results match those obtained by processing the whole slide and cropping".
- The open follow-up in `docs/project.md` (~297) asks for this decision.

**Base:** branch `fix/anchor-level-pixel` from `main` (e45ce9b).

## Global Constraints

- **Commands:**
  - Build: `./build.sh --skip-slideio`.
  - Tests: `./run-tests.sh`; all must pass.
  - One test: `ctest --test-dir build -C Release -R <name>`.
  - Docs: `cmake --build build --target docs --config Release`.
- Zero compiler warnings and zero Doxygen warnings.
- **Public headers:**
  - The only public header change is the documentation of `RasterMask::testLevel0` in `include/slideio/processor/core/raster_mask.hpp`.
  - No signature changes; SDS 08 is untouched.
- **What stays the same:**
  - At level 0, and at every integer scale, every result is unchanged: there `llround(L × s) = L × s`, so the new inverse equals `floor`.
  - Unrestricted runs are unchanged. ROI admission is unchanged (already exact in level 0), but the advisory ROI mask operations see loses the left/top column/row that e45ce9b added, which can change mask-honouring global-context operations such as `threshold.otsu`.
  - `RasterMask::test`, `rasteriseInto` and `dilateMask` are unchanged.
- **Determinism:**
  - Results must stay identical across worker counts.
  - `SeamRun.RestrictedRunIsWorkerCountInvariant` and `TileExecutor.ResultsIdenticalAcrossWorkerCounts` must pass unchanged.
- **Messages and traceability:**
  - Messages never carry pixel, coordinate or parameter values.
  - Every source and test file that has a `// Requirements: ...` line keeps it. The new core file `level_mapping.{hpp,cpp}` keeps the line its pipeline original has.
  - `docs/traceability.md` lists every new test, updated in place in the existing rows.
- Match the surrounding code's idiom and comment density.

## Decisions

1. **The level pixel of a level-0 point is the mapping's inverse.**
   - For a point `x`, it is the largest `L` with `levelToLevel0(L, scale) ≤ x`; y is computed the same way.
   - For `scale ≥ 1` the mapping is strictly increasing, so for every P, `L(levelToLevel0(P)) = P`.
   - For `scale < 1` (`RasterMask` accepts down to 1e-6) it stays well defined.
   - Integer arithmetic decides the answer; a double division only seeds it:
     - Seed `floor((x + 0.5) / scale)`, which is a few steps from the answer at practical magnitudes, and bounded at the 2^62 guard.
     - Then step down while `levelToLevel0(L) > x`, and up while `levelToLevel0(L + 1) ≤ x`.
     - Return false when the seed's magnitude reaches 2^62, or when a product `|L × scale|` reaches 2^63.
2. **One mapping, in core.**
   - Move `src/pipeline/detail/level_mapping.{hpp,cpp}` to `src/core/detail/level_mapping.{hpp,cpp}`. The namespace `slideio::processor::detail` and the functions stay unchanged.
   - Add `bool levelOrdinateAtOrBelow(int64_t level0, double scale, int64_t& out) noexcept` (decision 1).
   - Update every include, in src and tests. Move the `.cpp` from the pipeline CMake list to the core one.
   - `LevelSelector`'s `lastAtOrBelow` calls `levelOrdinateAtOrBelow` and keeps its throwing contract (`mappedRectDoesNotFit()` on false). `firstAtOrAbove` stays where it is.
3. **`RasterMask::testLevel0` uses it.**
   - It tests `{levelOrdinateAtOrBelow(x), levelOrdinateAtOrBelow(y)}`, and returns false when either does not fit.
   - The header doc and SDS 01 §1.17 say "the level pixel whose mapped corners hold the point", in place of `floor(x / levelScale)`.
   - The doc keeps the anchor rationale: an object found at level pixel P has its anchor at `levelToLevel0(P)`, so it is tested at P.
   - The old `floorDiv` helper becomes unused; delete it.
4. **`RestrictionFilter::levelPixelOf` uses it too**, so `tileOf` and the cached-mask lookup find the pixel `testLevel0` tests. If the result does not fit, it throws `InvalidArgumentError("RestrictionFilter: an anchor lies outside the level's coordinate range")`; `admits` already may throw.
5. **What becomes dead, and goes (Task 2).** Every anchor a tile owns, `levelToLevel0` of its level rect, now has its level pixel inside that rect. So:
   - `ownedRectAtLevel` is removed; its callers use `grid.tileRectAtLevel`.
   - `buildTileMask` builds over `tileRectAtLevelWithHalo(tile, chainHalo)` exactly.
   - The ROI part is `mapToLevel(roi)` alone.
     - For an anchor `x` in `[x0, x1)`, its level pixel is at least `lastAtOrBelow(x0)` and below `firstAtOrAbove(x1)`, so the old union adds nothing.
     - At a non-integer scale this takes back the left/top column or row the previous fix added to the mask operations see.
   - `outsideRegion` keeps its one-pixel widening at a non-integer scale. The comment is rewritten to give the reason that remains:
     - `rasteriseInto` sets a pixel whose real square `[P × s, (P + 1) × s)` touches the polygon.
     - That pixel can lie one outside `mapToLevel(boundingBox)`, on any side.
6. **Out of scope.** The conservative rasterisers (`rasteriseInto`, and the layer mask source built on it) still map vertices by `x / scale`, real squares. They stay as they are.
   - A pixel's real square and its `llround` span differ by less than half a level-0 pixel on each side.
   - The pixel-centre property (Review Focus 3) holds under the new test.

## Review Focus

1. A mask source that sets one level column at scale 2.4 admits exactly the anchors of the objects found in that column. That includes a column whose corner `llround` rounds down (`Restriction.MaskSourceTestsTheAnchorsOwnLevelPixel`, `Restriction.MaskSourceRunKeepsTheObjectsFoundInItsPixels`, Task 1).
2. A negative coordinate at a half-integer product (`llround` rounds halves away from zero) maps to the right pixel (`RasterMask.TestLevel0FindsThePixelWhoseMappedCornersHoldThePoint`, Task 1).
3. A polygon still admits every anchor that lies on the mapping's lattice and whose pixel centre it contains (`Restriction.PolygonAdmitsEveryAnchorWhosePixelCentreItContains`, Task 1).
4. A huge point at a tiny scale returns false at once, with no long loop (`RasterMask.TestLevel0OutOfRangeIsFalse`, Task 1).
5. After the widening goes, no tile whose own anchors the restriction admits is skipped. The existing `Restriction.SkipAgreesWithTheBuiltMask*` and `Restriction.OutsideRegionAgreesWithTheBuiltMask*` tests still pass, and the tile mask is exactly the halo rect (`Restriction.TileMaskIsTheHaloRect`, Task 2).

---

### Task 1: One level-pixel convention

**Files:**
- Move: `src/pipeline/detail/level_mapping.{hpp,cpp}` → `src/core/detail/level_mapping.{hpp,cpp}`. Modify `src/core/CMakeLists.txt` and `src/pipeline/CMakeLists.txt`, and every `#include <pipeline/detail/level_mapping.hpp>` in `src/` and `tests/` (grep for it).
- Modify: `src/core/raster_mask.cpp` (`testLevel0`, delete `floorDiv`), and `include/slideio/processor/core/raster_mask.hpp` (doc of `testLevel0` only).
- Modify: `src/pipeline/level_selector.cpp` (`lastAtOrBelow`).
- Modify: `src/pipeline/detail/restriction.{hpp,cpp}` (`levelPixelOf`, and its comments).
- Test: `tests/core/test_raster_mask.cpp`, `tests/pipeline/test_restriction.cpp`, `tests/pipeline/test_seam_run.cpp`.
- Docs: `docs/superpowers/specs/sds/01-core.md` §1.17, around the `testLevel0` declaration (~1529), and the cross-reference at ~232 if its wording depends on floor. Also `docs/traceability.md`.

**Interfaces:**
- Produces: `bool detail::levelOrdinateAtOrBelow(int64_t level0, double scale, int64_t& out) noexcept;` in `<core/detail/level_mapping.hpp>`, with `levelToLevel0` and `rectToLevel0` moved there unchanged.

- [ ] **Step 1: Write the failing tests**
  - **`RasterMask.TestLevel0FindsThePixelWhoseMappedCornersHoldThePoint`:**
    - Scale 2.4, region `{120, 0, 10, 1}`, only level pixel 126 set. `testLevel0` of `{302, 0}`, `{303, 0}` and `{304, 0}` is true; of `{301, 0}` and `{305, 0}` it is false. The old rule gives false at 302.
    - Scale 2.5, region `{-3, 0, 3, 1}`, only pixel −1 set. `testLevel0({-3, 0})` is true (`llround(−2.5) = −3`) and `testLevel0({-4, 0})` is false. The old rule gives false at −3.
    - The integer-scale cases already in `ConstructionAndCoordinates` and `AccessorsNullCopyMove` stay unchanged and must still pass.
  - **`RasterMask.TestLevel0InvertsTheLevelMapping`:**
    - For scale in {1.0, 1.5, 2.0, 2.4, 2.5, 3.7, 4.0} and P from −300 to 300, use a one-row mask over `[−301, 302)` with only pixel P set.
    - `testLevel0({llround(P × s), 0})` is true, and `testLevel0({llround(P × s) − 1, 0})` is false.
    - The same holds in y on a one-column mask.
  - **`RasterMask.TestLevel0OutOfRangeIsFalse`:** at scale 1e-6, region `{0, 0, 1, 1}` set, `testLevel0({INT64_MAX, 0})` and `testLevel0({INT64_MIN, 0})` are false.
  - **`Restriction.MaskSourceTestsTheAnchorsOwnLevelPixel`:**
    - Build the plan and filter as `checkAdmits` does, at level 1 of a 600 → 250 synthetic scene (scale 2.4), with `LineMaskSource(false, c)` and no ROI or polygon.
    - For every c from 120 to 135, with `a = llround(c × 2.4)`:
      - `admits({a, 300})` is true;
      - `admits({a − 1, 300})` and `admits({llround((c + 1) × 2.4), 300})` are false.
    - Repeat with `LineMaskSource(true, c)`, swapping x and y.
    - Move `LineMaskSource` above its first use if needed.
  - **`Restriction.MaskSourceRunKeepsTheObjectsFoundInItsPixels`:**
    - Run `"test.components"` at level 1 of a 600 → 250 scene of `dashes`, unrestricted, then with `LineMaskSource(false, 123)`, at 1 and 4 workers.
    - Level column 123 starts at level-0 295 (295.2 rounded down); 295 / 2.4 floors to 122.
    - The stored ids equal the unrestricted ids whose `anchor().x == 295`. Assert that set is non-empty.
  - **`Restriction.PolygonAdmitsEveryAnchorWhosePixelCentreItContains`:**
    - Level 1, scale 2.4, polygon quad `{70, 70}, {380, 90}, {400, 380}, {90, 410}`.
    - For every level pixel P of the grid, take the anchor `a = {llround(P.x × 2.4), llround(P.y × 2.4)}`. When `quad.contains(a)`, `admits(a)` is true.
    - Assert at least 100 such anchors were checked.
  - **Replace every `floor(… / scale)` oracle with the level-pixel oracle,** `selector.mapToLevel(Rect64{a.x, a.y, 1, 1}, level)`'s top-left. Add one helper per test file, and update the comments that describe floor. The places:
    - `checkAdmits` (~554);
    - `expectSkipAgreesWithTheMask` (~972): pixel 127 now holds level-0 305 and 306, in tile 1. Drop the `EXPECT_GE(along, 307)` "owned by tile 2" check, and assert the anchors lie in [305, 307);
    - `RoiAndMaskSourceBothDecide` (~1230): pixel 127 is no longer in the ROI mask. Switch the ROI to `{308, 308, 99, 99}` and `leftOfRoi` to `{307, 356}`, which is in pixel 128, set by `mapToLevel(roi)`, so only `inRoi` refuses it;
    - `expectRoiRunStoresExactlyItsAnchors` (~1281);
    - `expectRoiAtLevelOneKeepsItsAnchors` in `test_seam_run.cpp` (~405).

    If a fixture's `ASSERT_GT(…, 0)` precondition no longer holds, change its values, keep the assertion, and record the new values in the report.

- [ ] **Step 2: Run them to verify they fail**

Run: `ctest --test-dir build -C Release -R "RasterMask\.TestLevel0|Restriction\.(MaskSource|Polygon)"`
Expected:
- `FindsThePixel…`, `Inverts…`, `MaskSourceTestsTheAnchorsOwnLevelPixel` and `MaskSourceRunKeepsTheObjectsFoundInItsPixels` fail.
- `OutOfRange` and `PolygonAdmits…` may already pass; they are guards. Say so in the report.

- [ ] **Step 3: Implement decisions 1–4.**

- [ ] **Step 4: Verify**

Run: `./run-tests.sh`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add src include tests docs
git commit -m "fix(core): test a level-0 point at the level pixel the library's mapping puts it in"
```

### Task 2: Remove the floor-only widening; document

**Files:**
- Modify: `src/pipeline/detail/restriction.{hpp,cpp}`:
  - the constructor's ROI union (~23-36);
  - `ownedRectAtLevel` (remove it);
  - `buildTileMask`, `skips` and `outsideRegion`;
  - the class doc's ROI paragraph and the `buildTileMask`/`outsideRegion` docs.
- Test: `tests/pipeline/test_restriction.cpp`.
- Docs:
  - `docs/superpowers/specs/sds/05-pipeline.md` §5.5: the ROI-mask sentence "at a non-integer scale the mask grows by at most one level pixel on its left and top edges" (~985), plus every other floor/widening sentence there (grep `floor` and `widen`).
  - `docs/project.md` (~297-305), and `docs/traceability.md`.

**Interfaces:**
- Consumes: `detail::levelOrdinateAtOrBelow`, and `levelPixelOf` as Task 1 leaves it.

- [ ] **Step 1: Write the tests**
  - **`Restriction.TileMaskIsTheHaloRect`:** at level 1 (scale 2.4) of a mask-source run, with halo 0 and with halo 2, `buildTileMask(t).regionAtLevel() == plan.grid.tileRectAtLevelWithHalo(t, halo)` for every tile. It fails before the change at halo 0.
  - **`Restriction.RoiMaskIsTheLevelMappingOfTheRoi`:** for ROIs `{307, 307, 100, 100}` and `{308, 308, 99, 99}` at scale 2.4, the union of the tile masks' set pixels is exactly `mapToLevel(roi, 1)`. This pins decision 5; it passes before and after.
  - Update the comment of `NonIntegerScaleKeepsObjectsOnTileEdges` (~873), which describes floor.

- [ ] **Step 2: Run them**

Run: `ctest --test-dir build -C Release -R "Restriction\.(TileMaskIsTheHaloRect|RoiMaskIsTheLevelMapping)"`
Expected: `TileMaskIsTheHaloRect` fails at halo 0.

- [ ] **Step 3: Implement decision 5, and update the documentation**
  - **SDS 05 §5.5:**
    - A polygon or a mask source is tested at the level pixel the object was found in, the inverse of the level mapping.
    - The ROI mask is `mapToLevel(roi)` and holds every ROI anchor's pixel at every scale.
    - Remove the "grows by at most one level pixel" sentence.
  - **`docs/project.md`:**
    - Move the follow-up to resolved.
    - Add a QA/regulatory note:
      - At levels whose scale is not an integer, runs restricted by a polygon or a mask source (workspace restrict-to-layer) can keep or drop different objects along the restriction's edges.
      - The mask an ROI-restricted operation sees loses the extra left/top column or row added on 2026-11-21.
      - `tilesSkipped` can change.
      - Level 0 and integer scales are unchanged.
      - Re-run affected stages and record the change in the verification evidence. Stored results are not marked stale automatically.
  - **`docs/traceability.md`:** every new test from Tasks 1 and 2, under UR-IMG-5 and UR-STG-3. Add the core tests under UR-QA-2 and UR-QA-3 too, as the `RasterMask.*` row does.

- [ ] **Step 4: Verify**

Run: `./run-tests.sh` and `cmake --build build --target docs --config Release`
Expected: all tests pass, zero Doxygen warnings.

- [ ] **Step 5: Commit**

```bash
git add src tests docs
git commit -m "refactor(pipeline): drop the floor-only mask widening; SDS 05 anchor pixel rule"
```
