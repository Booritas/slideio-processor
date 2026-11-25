# Distance transform, hole filling and watershed marker floor — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `pixel.distance_transform`, `pixel.fill_holes` and `segment.watershed`'s optional `minimumMarkerValue`, so nucleus segmentation runs from the built-in catalog alone.

**Architecture:** Two new PerChannel pixel operations, each in its own source file under `src/algorithm/pixel/`. Each has a typed factory declared in `include/slideio/processor/algorithm/pixel/filter.hpp`, and a `BuiltinFactory` registered by name in `registry.cpp`. Both are exact under tiling because the declared bound is their halo. The watershed change is one optional, absent-by-default key. The documents move in the same tasks: requirements 0.14, SDS 06 §6.2/§6.4/§6.7/§6.9/§6.10 and the working-set table, and the traceability matrix.

**Tech Stack:** C++17, OpenCV 4 (imgproc), GoogleTest, CMake/Conan, built with `./build.sh` and tested with `./run-tests.sh` (Git Bash).

**Spec:** `docs/superpowers/specs/2026-11-25-distance-transform-and-holes-design.md`

## Global Constraints

- A pixel is **foreground when its sample is neither 0 nor NaN** (the rule `segment.watershed` uses, `watershed.cpp:120`).
- `pixel.distance_transform`: output `Float32`; `maximumDistancePx` int, default 32, range 1 to 4096; halo `maximumDistancePx`; output = min(Euclidean distance to the nearest background pixel centre, `maximumDistancePx`), background 0.
- `pixel.fill_holes`: output `Byte` 255/0; `maximumHoleSizePx` int, default 32, range 1 to 4096; `maximumHoleAreaPx` int, **absent by default**, at least 1 when given; halo `maximumHoleSizePx`. A hole is a 4-connected background component that does not touch the context border, whose bounding box is at most `maximumHoleSizePx` on each side, and whose area is at most `maximumHoleAreaPx` when that is given.
- `segment.watershed` `minimumMarkerValue`: double, **absent by default**, finite. With `LocalMaxima`, a pixel whose value is below it is not a marker. Given with `AuxiliaryRaster`, it is refused with `InvalidArgumentError` naming `minimumMarkerValue`.
- Parameter bags follow SDS 06 §6.2: complete bag, unknown key or wrong type throws `InvalidArgumentError` naming operation and key, **never the value**. An absent-by-default key stays absent from the bag.
- Tiled output is **bitwise equal** to the single-tile output at three tile sizes (UR-QA-2, UR-SEG-3).
- Every operation declares a working set bounded by its measured peak (`tests/algorithm/test_working_set_*.cpp`). OpenCV calls go through `detail::useSequentialOpenCv(kName)` and `detail::translateCv(kName)`.
- Each test file starts with a `// Requirements: …` line, and every new test name is entered in `docs/traceability.md`.
- Requirements become version 0.14, dated 2026-11-25: UR-PIX-7 (Should, distance transform) and UR-PIX-8 (Should, hole filling), under §7.

## Review Focus

1. **A mask touching the level edge.** Distance counts the outside of the level as background. A background region open to the edge is never filled. Pinned by `DistanceTransform.LevelEdgeIsBackground` and `FillHoles.RegionOpenToLevelEdgeStaysOpen`.
2. **A hole exactly at the bound and one pixel over.** It must be filled at size *S* and not at *S*+1, at every tile size. Pinned by `FillHoles.BoundIsInclusive` and `FillHoles.TiledEqualsSingleTile`.
3. **A recorded watershed configuration from before this change.** It must create the same bag, without `minimumMarkerValue`. Pinned by `Watershed.MarkerFloorAbsentByDefault`.
4. **NaN and negative samples in a Float32 mask.** NaN is background; a negative value is foreground. Pinned by `DistanceTransform.NaNIsBackgroundNegativeIsForeground` and `FillHoles.NaNIsBackground`.
5. **A tile with no background in its context** (inside a large nucleus or tissue). The output is `maximumDistancePx` everywhere, with no crash on OpenCV's empty-background case. Pinned by `DistanceTransform.AllForegroundGivesTheMaximum`.

---

### Task 1: `pixel.distance_transform`

**Files:**
- Create: `src/algorithm/pixel/distance_transform.cpp`
- Modify: `include/slideio/processor/algorithm/pixel/filter.hpp` (declare and document the factory), `src/algorithm/CMakeLists.txt`, `src/algorithm/detail/builtin_operations.hpp`, `src/algorithm/registry.cpp` (`registerPixelOperations`)
- Test: `tests/algorithm/test_distance_transform.cpp` (new, add to `tests/algorithm/CMakeLists.txt`), `tests/algorithm/test_registry.cpp` (`RegisteredAtLoad` list), `tests/algorithm/test_working_set_filter.cpp`
- Docs: `docs/requirements.md`, `docs/superpowers/specs/sds/06-algorithm.md`, `docs/traceability.md`

**Interfaces:**
- Produces: `SLIDEIO_PROCESSOR_ENGINE_EXPORTS OperationPtr makeDistanceTransform(int32_t maximumDistancePx);` (throws `InvalidArgumentError` "pixel.distance_transform: parameter 'maximumDistancePx' must be from 1 to 4096"); `OperationFactoryPtr detail::makeDistanceTransformFactory();`; catalog name `"pixel.distance_transform"`.

- [ ] **Step 1: Write the failing tests** in `test_distance_transform.cpp` (header `// Requirements: UR-PIX-7, UR-QA-2, UR-ALG-6`). Model the harness use on `test_median_blur.cpp`: `ts::applyDirect` for one tile, and `ts::makeSyntheticScene` plus `ts::runEager` for tiling. The brute force is `min(H, min over background (bx,by) of sqrt((x-bx)²+(y-by)²))`, with the outside of the image as background. The tests:
  - `FactoryEqualsNamePath`: `makeDistanceTransform(7)` and the catalog with `maximumDistancePx` 7 give equal bags. The default bag holds `maximumDistancePx` == 32.
  - `RejectsBadMaximum`: 0, 4097 and a Double value throw `InvalidArgumentError`, by both the factory and name paths. The message names `maximumDistancePx` and not the value.
  - `MatchesBruteForce`: random 0/1 `Byte` masks (seeded `std::mt19937`, 60×50, 70% foreground) with maximums 3, 8 and 32. The output is a `Float32` equal to the brute force cast to float (`EXPECT_EQ`, exact).
  - `DiscCentreIsItsRadius`: a filled disc of radius 10 centred in 41×41. The centre value equals the brute force, and 0 lies outside the disc.
  - `LevelEdgeIsBackground`: an all-foreground 20×12 image with maximum 32. The value at (x,y) is `min(x+1, y+1, 20-x, 12-y)`.
  - `NaNIsBackgroundNegativeIsForeground`: a `Float32` mask of -1 with one NaN at the centre. The centre is 0, its 4-neighbours are 1, and diagonals are √2 as float.
  - `AllForegroundGivesTheMaximum`: a 300×260 all-255 scene with maximum 8 at tile 64. Every pixel more than 8 from the level edge is 8.
  - `OutputTypeIsFloat32` for `Byte`, `UInt16`, `Int32` and `Float64` input.
  - `HaloIsTheMaximum`: `halo()` is uniform 5 for `makeDistanceTransform(5)`.
  - `TiledEqualsSingleTile`: a 300×260 pattern scene whose foreground is blobs (`((x/9 + y/7) % 3) != 0` minus a sparse speckle) on `Byte` and `Float32`, with maximum 12. Tiles 64, 100 and 257 give planes bitwise equal to tile 4096.

  In `test_registry.cpp`, add `"pixel.distance_transform"` to `RegisteredAtLoad`. In `test_working_set_filter.cpp`, add `DistanceTransformDeclarationBoundsTheMeasuredPeak` (default and maximum 64 on `Byte` and `Float32`, with OpenCV's share bounded as the median's helper does) and `OpenCvIsSequentialOnceDistanceTransformApplies`.

- [ ] **Step 2: Build and run to verify they fail**

Run: `./build.sh && ./run-tests.sh -R 'DistanceTransform|Registry'`
Expected: the build fails because `makeDistanceTransform` is undeclared. That counts as the failing state.

- [ ] **Step 3: Implement** `distance_transform.cpp` after the shape of `median_blur.cpp`:
  - `PerChannel`, `DataTypeSet::all()`, `requiresUniformTypes()` false, `outputType` `Float32`, halo `Halo::uniform(maximumDistancePx)`.
  - Per plane, build a `Byte` mask of the whole halo'd input (foreground = neither 0 nor NaN, read via `detail::readSample`).
  - Run `cv::distanceTransform(mask, dist, cv::DIST_L2, cv::DIST_MASK_PRECISE, CV_32F)`, then write `std::min(dist, float(H))` for the tile's rect into the output.
  - If the mask has no zero pixel, write `H` directly. Do not call OpenCV in that case: with no background it returns huge values, so clamping still works but the fast path is also clearer.
  - Declare `workingSetBytesPerLevelPixel` from the mask plus the destination plus OpenCV's precise-mode buffers. Derive the figure from `imgproc/src/distransform.cpp` (`trueDistTrans`) and comment the source the way the median does.
  - The factory note is "Published definition: exact Euclidean distance (Felzenszwalb and Huttenlocher); checked against brute force". Complexity is "O(n) in the number of samples of the halo'd tile".

  Register it in `registry.cpp` after `pixel.morph_close`. Document it in `filter.hpp` in the style of `makeMedianBlur`, stating the foreground rule, the clamp, the halo and that outside the level is background.

- [ ] **Step 4: Run tests to verify they pass**

Run: `./build.sh && ./run-tests.sh -R 'DistanceTransform|Registry|WorkingSetFilter'`
Expected: all pass.

- [ ] **Step 5: Documents**
  - **requirements.md:**
    - Version 0.14, date 2026-11-25.
    - A **Changes in 0.14** paragraph: UR-PIX-7 added, a distance transform for `segment.watershed`'s input (SDS 06 §6.4, §6.10). Say that nothing else was renumbered or changed.
    - UR-PIX-7 (Should) after UR-PIX-6, with the acceptance from the spec's Traceability section.
  - **06-algorithm.md:**
    - §6.4: the factory in the code block, and a short subsection *Distance transform* with the exactness argument from the spec §1.
    - §6.2: a parameter table row.
    - Working-set table: a row.
    - §6.9: a reference-kind row ("Brute-force distance on random masks; exact").
    - §6.10: a catalog row (`pixel.distance_transform` | 6.4 | PerChannel | `maximumDistancePx`) and a requirement row (UR-PIX-7 → `pixel.distance_transform`). Change "twenty-one" to "twenty-two" in the catalog prose, and record the addition beside the existing note on the catalog's size.
    - §6.7 *Flood a surface that peaks inside each object*: replace "v1's catalog has no distance transform, so it comes from the caller's own operation or an earlier stage" with a pointer to `pixel.distance_transform`.
  - **traceability.md:** UR-PIX-7, UR-QA-2 and UR-ALG-6 rows for the new tests; the new working-set and registry test names; the module paragraph's operation count and list.

- [ ] **Step 6: Commit**

```bash
git add src/algorithm include tests/algorithm docs/requirements.md docs/superpowers/specs/sds/06-algorithm.md docs/traceability.md
git commit -m "algorithm: pixel.distance_transform (UR-PIX-7)"
```

### Task 2: `segment.watershed` `minimumMarkerValue`

**Files:**
- Modify: `src/algorithm/segment/watershed.cpp` (values struct, bag reading, factory checks, marker detection), `include/slideio/processor/algorithm/segment/segment.hpp` (`WatershedParameters::minimumMarkerValue(double)`, parameter table near line 483 and the marker docs near 463/699)
- Test: `tests/algorithm/test_watershed.cpp`
- Docs: `docs/superpowers/specs/sds/06-algorithm.md` (§6.2 row, §6.7 *Markers*), `docs/traceability.md`

**Interfaces:**
- Produces: `WatershedParameters& WatershedParameters::minimumMarkerValue(double value);` (non-finite throws `InvalidArgumentError` naming `minimumMarkerValue`); bag key `"minimumMarkerValue"` (double), present only when set.

- [ ] **Step 1: Write the failing tests** in `test_watershed.cpp`:
  - `MarkerFloorAbsentByDefault`: `makeWatershed(WatershedParameters())->parameters()` has no `minimumMarkerValue` key, and a by-name bag without it creates an equal bag.
  - `MarkerFloorRoundTrips`: the typed `.minimumMarkerValue(2.5)` and the by-name `setDouble("minimumMarkerValue", 2.5)` give equal bags; the integer `3` by name reads back as 3.0.
  - `MarkerFloorRejected`: NaN and infinity throw by both paths. `AuxiliaryRaster` with the floor throws `InvalidArgumentError` whose message contains `minimumMarkerValue` and no digits of the value.
  - `MarkerFloorDropsShallowSeed`: the disc pair of `TouchingDiscsSeparated` plus a third disc of radius 3 far from them. Without a floor there are 3 objects. With `minimumMarkerValue(4.0)` the small disc has no marker and fails the reach check with the reach message, because its pixels lie beyond any seed. Use `maximumMarkerSpacingPx` 12 so the throw is certain. Assert that the 3-object result exists without the floor and that `InvalidArgumentError` is thrown with it.
  - `MarkerFloorKeepsTiling`: `TilingInvariantAtThreeSizes`'s input with a floor below every true centre value gives the same objects as without the floor, at all three tile sizes.

- [ ] **Step 2: Run to verify failure**

Run: `./build.sh`
Expected: compile error on `minimumMarkerValue`.

- [ ] **Step 3: Implement.**
  - Add `std::optional<double> minimumMarkerValue` to the values struct. Read it by name only when the key is present, as `targetLayer` is (`bag.getDouble` on a present key). Write it to `parameters_` only when set.
  - In the factory checks, refuse non-finite values, and refuse the floor with `AuxiliaryRaster` ("parameter 'minimumMarkerValue' applies only to LocalMaxima markers").
  - In the `LocalMaxima` marker search, skip a candidate whose value is below the floor. It stays foreground and floods.
  - The halo and working set are unchanged.

- [ ] **Step 4: Run tests**

Run: `./build.sh && ./run-tests.sh -R 'Watershed|Registry|WorkingSetSegment'`
Expected: all pass, the existing watershed tests included.

- [ ] **Step 5: Documents.** In §6.2, add a watershed row with "absent" as the default. In §6.7 *Markers*, add a sentence on the floor and its interaction with the reach check. In traceability, add the new tests under UR-SEG-8.

- [ ] **Step 6: Commit**

```bash
git add src/algorithm include tests/algorithm docs/superpowers/specs/sds/06-algorithm.md docs/traceability.md
git commit -m "algorithm: segment.watershed minimumMarkerValue"
```

### Task 3: `pixel.fill_holes`

**Files:**
- Create: `src/algorithm/pixel/fill_holes.cpp`
- Modify: `include/slideio/processor/algorithm/pixel/filter.hpp`, `src/algorithm/CMakeLists.txt`, `src/algorithm/detail/builtin_operations.hpp`, `src/algorithm/registry.cpp`
- Test: `tests/algorithm/test_fill_holes.cpp` (new, add to CMake), `tests/algorithm/test_registry.cpp`, `tests/algorithm/test_working_set_filter.cpp`
- Docs: `docs/requirements.md`, `docs/superpowers/specs/sds/06-algorithm.md`, `docs/traceability.md`

**Interfaces:**
- Produces:
  - `struct FillHolesParameters` is not needed. Use `SLIDEIO_PROCESSOR_ENGINE_EXPORTS OperationPtr makeFillHoles(int32_t maximumHoleSizePx, int64_t maximumHoleAreaPx = 0);`. Here 0 means no area limit, so the key stays absent; a negative value throws. By name, a present `maximumHoleAreaPx` must be at least 1.
  - `OperationFactoryPtr detail::makeFillHolesFactory();`
  - Catalog name `"pixel.fill_holes"`.

- [ ] **Step 1: Write the failing tests** in `test_fill_holes.cpp` (header `// Requirements: UR-PIX-8, UR-QA-2, UR-ALG-6`). The reference is a brute-force flood fill over the whole image: 4-connected background components that do not touch the image border, with bounding box ≤ S on each side and area ≤ A. The tests:
  - `FactoryEqualsNamePath`: the default bag has `maximumHoleSizePx` 32 and no `maximumHoleAreaPx`. `makeFillHoles(9, 20)` equals the name path with both keys.
  - `RejectsBadParameters`: size 0 or 4097, area 0 by name, a negative area by the factory, and wrong types. Messages name the key and not the value.
  - `BoundIsInclusive`: square ring masks with inner holes of 5×5 and 6×6, with S = 5. The first is filled and the second is not.
  - `AreaLimit`: a 4×4 hole with S = 10. It is filled with area 16 and not with area 15.
  - `RegionOpenToLevelEdgeStaysOpen`: a U shape whose opening touches the image edge stays unfilled even with a small S.
  - `DiagonalGapIsAHole`: a foreground ring closed only diagonally encloses a 4-connected hole, which is filled. This is the 8/4 connectivity pair.
  - `NestedRings`: a hole containing an island containing a hole. Both holes within S are filled, and the output has no 0 inside the outer ring.
  - `NaNIsBackground`: a `Float32` ring of 1.0 around a NaN centre fills the centre.
  - `OutputIsByte255`: foreground of any value in `UInt16` and `Float32` gives 255.
  - `MatchesBruteForce`: random masks (seeded, 60×50, 55% foreground) with S in {2, 4, 8} and area absent or 6. The output equals the reference exactly.
  - `TiledEqualsSingleTile`: a 300×260 scene of ring blobs with holes of sizes 3 to 14 placed across tile seams, plus random speckle, on `Byte` and `Float32`, with S = 10. Tiles 64, 100 and 257 are bitwise equal to tile 4096.

  Add the registry entry and working-set tests (`FillHolesDeclarationBoundsTheMeasuredPeak`, and the sequential-OpenCV test if OpenCV is used), as in Task 1.

- [ ] **Step 2: Run to verify failure**

Run: `./build.sh`
Expected: compile error on `makeFillHoles`.

- [ ] **Step 3: Implement** `fill_holes.cpp`:
  - `PerChannel`, all types, output `Byte`, halo `Halo::uniform(maximumHoleSizePx)`.
  - Per plane, build a `Byte` background mask of the halo'd context.
  - Label it with `cv::connectedComponentsWithStats(…, 4, CV_32S)` under `useSequentialOpenCv`. The module's own labelling is also acceptable if it simplifies the working-set accounting; record the choice in the report.
  - A label is a hole when its bounding box does not touch any context border (x > 0, y > 0, x + w < width, y + h < height), w ≤ S and h ≤ S, and the area is ≤ A when A is set.
  - Write 255 for foreground or hole pixels of the tile rect, 0 otherwise.
  - Declare the working set from the masks, labels and stats with a commented source.
  - The factory note is "Published definition: background components enclosed within the declared size; checked against a brute-force flood fill". Complexity is "O(n) in the number of samples of the halo'd tile".

- [ ] **Step 4: Run tests**

Run: `./build.sh && ./run-tests.sh -R 'FillHoles|Registry|WorkingSetFilter'`
Expected: all pass.

- [ ] **Step 5: Documents**
  - **requirements.md:** add UR-PIX-8 (Should) after UR-PIX-7 and extend the 0.14 changes paragraph to name it.
  - **06-algorithm.md:**
    - §6.4: the factory and a *Hole filling* subsection with the exactness argument from the spec §2.
    - §6.2: rows for both keys, the area key "absent".
    - Working-set table, §6.9 and §6.10: rows as in Task 1.
    - The catalog count goes from "twenty-two" to "twenty-three".
  - **traceability.md:** rows as in Task 1.

- [ ] **Step 6: Commit**

```bash
git add src/algorithm include tests/algorithm docs/requirements.md docs/superpowers/specs/sds/06-algorithm.md docs/traceability.md
git commit -m "algorithm: pixel.fill_holes (UR-PIX-8)"
```

### Task 4: Nuclei chain end-to-end through the pipeline

**Files:**
- Test: `tests/algorithm/test_nuclei_chain.cpp` (new, add to CMake; header `// Requirements: UR-SEG-8, UR-SEG-3, UR-PIX-7, UR-PIX-8`)
- Docs: `docs/traceability.md`

**Interfaces:**
- Consumes: `makeFillHoles`, `makeDistanceTransform`, `WatershedParameters::minimumMarkerValue` from Tasks 1–3; `ts::runEagerObjects` from `tests/algorithm/support/harness.hpp`.

- [ ] **Step 1: Write the test.** `NucleiChain.TouchingRingNucleiSeparatedAtThreeTileSizes`:
  - The input is a synthetic `Byte` scene, 320×280: 12 discs of radius 9 in pairs whose centres lie 14 apart. Each disc has a pale 3×3 centre of value 0, and the discs have value 200.
  - The chain is `threshold_fixed(100)` → `fill_holes(8)` → `distance_transform(12)` → `segment.watershed` with `minimumMarkerDistancePx` 5, `maximumMarkerSpacingPx` 16 and `minimumMarkerValue` 3.0.
  - Assert 12 objects, each with area within 10% of π·81 and centroid within 1.5 px of its disc centre.
  - Assert the identical object set (geometry and area) at tiles 64, 100 and 257 against tile 4096.
- [ ] **Step 2: Run**

Run: `./build.sh && ./run-tests.sh -R NucleiChain`
Expected: PASS. A failure here points to an earlier task's operation; fix it there, not in the test's numbers.

- [ ] **Step 3: Full suite**

Run: `./run-tests.sh`
Expected: all pass.

- [ ] **Step 4: Commit**

```bash
git add tests/algorithm docs/traceability.md
git commit -m "tests: nuclei chain from built-in operations at three tile sizes"
```
