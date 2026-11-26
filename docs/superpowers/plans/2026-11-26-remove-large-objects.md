# pixel.remove_large_objects Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A pixel operation that removes every 8-connected foreground component wider or taller than `maximumObjectSizePx`, identical at every tile size, so a nucleus chain no longer fails on stained areas too large to be a nucleus.

**Architecture:** A sibling of `pixel.fill_holes` (`src/algorithm/pixel/fill_holes.cpp`): same halo (the size bound), same foreground rule, same Byte state + one-component BFS queue over the halo'd context, but labelling 8-connected *foreground* and clearing components that touch the context border or exceed the bound. Registered by name; typed factory in `filter.hpp`.

**Tech Stack:** C++17, GoogleTest, CMake (`./run-tests.sh`).

**Spec:** `docs/superpowers/specs/2026-11-26-remove-large-objects-design.md`

## Global Constraints

- Operation name `pixel.remove_large_objects`; parameter `maximumObjectSizePx`, integer 1 to 4096, 32 when created by name without it.
- Typed factory `OperationPtr makeRemoveLargeObjects(int32_t maximumObjectSizePx);` in `include/slideio/processor/algorithm/pixel/filter.hpp`, next to `makeFillHoles`.
- PerChannel, every input type, output `Byte`: 255 for a kept foreground pixel, 0 otherwise. Foreground = sample neither 0 nor NaN.
- Kept iff the component's bounding box is ≤ `maximumObjectSizePx` wide **and** ≤ `maximumObjectSizePx` high (inclusive bound).
- Halo `Halo::uniform(maximumObjectSizePx)`. A component touching the halo'd context's border is removed. Pixels beyond the level are zero in the context (as for fill_holes), so a component cut by the level edge is measured by its in-level pixels.
- By name: unknown key, wrong type or out-of-range value → InvalidArgumentError naming the key, never the value. Typed factory out of range → InvalidArgumentError naming the operation and parameter.
- `segment.watershed` code is not changed; only its documentation gains a paragraph.
- Never run two test suites at once. GCC/Clang `-Werror` hygiene (no unused, no sign-compare). Keep each file's existing line endings.
- Requirement id UR-PIX-9 (Should); requirements document version 0.16.

## Review Focus

- A component larger than the tile that crosses several seams: every tile must remove its part (touches the halo border) — covered by `TiledEqualsSingleTile` with a blob larger than the smallest tile.
- A component exactly `maximumObjectSizePx` wide whose pixels reach the halo border from a tile-edge pixel: the bound must be such that touching the border implies width > bound — `TiledEqualsSingleTile` also places objects of exactly the bound (32×32 and 32×1) straddling seams at each tile size.
- A component touching the **level** edge but small: must be kept (beyond-level pixels are zero, not border) — `SmallObjectAtLevelEdgeKept`.
- A context too large to label in int32: refused with the same message shape as fill_holes — `RefusesAContextTooLargeToLabel`.
- Non-Byte inputs (UInt16, Float32 with NaN): same result as the Byte mask — `NaNIsBackground`, `OutputIsByte255`.

---

### Task 1: The operation, its registration and unit tests

**Files:**
- Create: `src/algorithm/pixel/remove_large_objects.cpp`
- Create: `tests/algorithm/test_remove_large_objects.cpp`
- Modify: `include/slideio/processor/algorithm/pixel/filter.hpp` (declaration + doc, after `makeFillHoles`)
- Modify: `src/algorithm/detail/builtin_operations.hpp` (`OperationFactoryPtr makeRemoveLargeObjectsFactory();` after fill_holes)
- Modify: `src/algorithm/registry.cpp:78` (register after `pixel.fill_holes`)
- Modify: `src/algorithm/CMakeLists.txt`, `tests/algorithm/CMakeLists.txt`
- Modify: `tests/algorithm/test_registry.cpp:91` (the expected name list; adjust any count)
- Modify: `tests/algorithm/test_working_set_filter.cpp` (new `WorkingSetFilter.RemoveLargeObjectsDeclarationBoundsTheMeasuredPeak`, modelled on `FillHolesDeclarationBoundsTheMeasuredPeak`)

**Interfaces:**
- Produces: `OperationPtr makeRemoveLargeObjects(int32_t maximumObjectSizePx)`; name `pixel.remove_large_objects`; `parameters()` holds exactly `{maximumObjectSizePx}`.

- [ ] **Step 1: Write the failing tests** in `test_remove_large_objects.cpp`, modelled on `test_fill_holes.cpp` (reuse its helpers' style for building masks, running one tile and tiled runs):
  - `RemoveLargeObjects.FactoryEqualsNamePath` — typed `makeRemoveLargeObjects(7)` and by-name `{maximumObjectSizePx: 7}` give equal parameters and equal output; by name without the key gives 32.
  - `RemoveLargeObjects.RejectsBadParameters` — 0, 4097, negative, wrong type, unknown key: InvalidArgumentError whose message names the key and not the value.
  - `RemoveLargeObjects.BoundIsInclusive` — at size 5: a 5×5 square kept; 6×5 and 5×6 removed; a 1×5 bar kept, a 1×6 bar removed.
  - `RemoveLargeObjects.DiagonalChainIsOneObject` — pixels (0,0),(1,1)…(5,5) at size 5: removed as one 6×6 component; the same pixels with one gap kept as two.
  - `RemoveLargeObjects.SmallObjectAtLevelEdgeKept` — a 3×3 square in the level's corner at size 5 is kept; a 10×3 bar along the top edge is removed.
  - `RemoveLargeObjects.NaNIsBackground` — Float32 mask where NaN separates two 4×4 squares that would otherwise form an 8×4 object: kept at size 5.
  - `RemoveLargeObjects.OutputIsByte255` — UInt16 and Float32 inputs give Byte 255/0 equal to the Byte mask's result.
  - `RemoveLargeObjects.MatchesBruteForce` — random masks (60×50, 55% foreground and 30% foreground, fixed seeds) at sizes 2, 4, 8: equal to a test-local whole-image reference (8-connected BFS, bbox ≤ size).
  - `RemoveLargeObjects.TiledEqualsSingleTile` — a 300×260 mask with small objects across seams plus one blob of 120×90: tiled output at tile sizes 64, 100 and 257 bitwise equal to one tile, Byte and Float32, at size 32.
  - `RemoveLargeObjects.RefusesAContextTooLargeToLabel` — as `FillHoles.RefusesAContextTooLargeToLabel`.

- [ ] **Step 2: Run to verify they fail** — build `slideio_processor_algorithm_tests`; it fails to compile/link (no `makeRemoveLargeObjects`). Record that as RED.

- [ ] **Step 3: Implement** `remove_large_objects.cpp` by following `fill_holes.cpp`'s structure (parameter validation, `fromParameters`, `BuiltinFactory` with its complexity note, the int32 context check, per-plane loop, working-set declarations of 5 bytes per level pixel and two allocator blocks per tile). Differences: BFS over `kForeground` with 8 neighbours; a component is cleared when `minX == 0 || minY == 0 || maxX + 1 == inWidth || maxY + 1 == inHeight` or its box exceeds the size in either dimension; output 255 only for kept foreground. Then the declaration/doc in `filter.hpp` (behaviour, the halo argument, parameters, throws, thread-safety, `@par Requirements UR-PIX-9, UR-QA-3, UR-ALG-6`), the factory declaration, registration, CMake entries, the registry name list and the working-set test.

- [ ] **Step 4: Run** `--gtest_filter=RemoveLargeObjects.*:Registry*:WorkingSetFilter.*` → all pass; then the whole algorithm test executable → all pass.

- [ ] **Step 5: Commit** — `algorithm: pixel.remove_large_objects removes mask components wider or higher than a bound`.

### Task 2: Nucleus chain test and watershed documentation

**Files:**
- Modify: `tests/algorithm/test_nuclei_chain.cpp`
- Modify: `include/slideio/processor/algorithm/segment/segment.hpp` (the "Settings that never fail the reach on a distance transform" list in the `segment.watershed` doc)

**Interfaces:**
- Consumes: `makeRemoveLargeObjects(int32_t)` from Task 1.

- [ ] **Step 1: Write the failing test** `NucleiChain.LargeStainedAreaRemovedBeforeWatershed`: the existing chain shape of this file (threshold → fill_holes → morph_open → distance_transform → watershed with spacing S) on a scene holding the file's small nuclei plus one solid 4S×S/2 band. Assert (a) without the new step the run throws, its message containing `maximumMarkerSpacingPx` (pins today's refusal); (b) with `makeRemoveLargeObjects(S)` inserted after the opening the run succeeds, the object count equals the small-nuclei-only scene's, and no object's bounding box exceeds S; (c) with the step, results are identical at three tile sizes (use the file's existing three-size helper).

- [ ] **Step 2: Run** `--gtest_filter=NucleiChain.*` → (b)/(c) fail only if Task 1 is wrong; (a) passes. If (b) passes without the step, the band is not large enough: fix the test, not the code.

- [ ] **Step 3: Document** in `segment.hpp`: a third bullet — `pixel.remove_large_objects` after the opening, with `maximumObjectSizePx` at most `maximumMarkerSpacingPx`, leaves only objects within the spacing, so the reach holds even when the cut-off lets large stained areas through; those areas are removed, not segmented.

- [ ] **Step 4: Run** the whole algorithm executable → all pass.

- [ ] **Step 5: Commit** — `algorithm: nucleus chain removes stained areas larger than the marker spacing; watershed docs`.

### Task 3: SDS, requirements, traceability

**Files:**
- Modify: `docs/superpowers/specs/sds/06-algorithm.md` — every place `pixel.fill_holes` has a row or sentence gets a matching one: parameter table (~:138), working-set table (~:234), factory listing (~:434), a short section after the fill_holes section (~:701), the verification table (~:1683, stating exactly what Task 1's tests check), the operation count (~:1716, twenty-three → twenty-four), the operation table (~:1734), the requirement map (~:1777).
- Modify: `docs/superpowers/specs/sds/08-class-diagrams.md` — the factory, wherever `makeFillHoles` appears.
- Modify: `docs/requirements.md` — version 0.16 with a "Changes in 0.16" note; UR-PIX-9 (Should) after UR-PIX-8: "The library shall provide the removal of objects larger than a caller-declared size from a binary mask." *Acceptance:* removes synthetic objects above the bound, keeps those within it, and is identical at three tile sizes.
- Modify: `docs/traceability.md` — a UR-PIX-9 row listing the `RemoveLargeObjects.*` tests; add them to the UR-ALG-6 and UR-QA-2 rows like fill_holes'; the working-set test to UR-ALG-8 and UR-EXE-2; `NucleiChain.LargeStainedAreaRemovedBeforeWatershed` wherever the other NucleiChain tests are listed.

- [ ] **Step 1: Edit the documents** as listed; grep the repo for `fill_holes` / `FillHoles` afterwards and confirm each hit has its counterpart or doesn't need one.
- [ ] **Step 2: Verify** — `./run-tests.sh` once, alone → all pass (any docs/traceability checks included).
- [ ] **Step 3: Commit** — `docs: pixel.remove_large_objects in SDS 06 and 08, UR-PIX-9 (requirements 0.16), traceability`.

## After the library merges (bindings, separate PR)

`examples/nucleus_segmentation.py`: add `.add("pixel.remove_large_objects", maximumObjectSizePx=p["spacing"])` after `pixel.morph_open` (both modes share that part of the chain); docstring "a stained area larger than --max-nucleus-size-um … fails the run" → "is removed"; README line; pin bump; verify on CMU-1-Small-Region.svs (default threshold runs; 0.9 drops the keratin band) and on the LuCa slide.
