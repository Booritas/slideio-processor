# Watershed `unreachedForeground` Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An opt-in `segment.watershed` mode, `unreachedForeground = "Drop"`, in which
foreground no marker reaches within `maximumMarkerSpacingPx` becomes background instead of
failing the run. It is counted, and gives the same result at every tile size.

**Architecture:** Phase 1 builds the Drop candidate (the parameter and the drop rule in
`watershed.cpp`), an independent whole-image reference model of the flood, and a
random-input harness that compares tiled runs with single-tile runs and with the model.
It then **stops at the owner's checkpoint**. Phase 2, only after sign-off, adds the
`droppedPixels` run counter through the run layers, the remaining tests, and the
requirement and SDS changes.

**Tech Stack:** C++17, GoogleTest, CMake. Build with `./build.sh`, test with
`./run-tests.sh -R <regex>` (ctest names are prefixed, e.g. `algorithm.Watershed.`).

**Spec:** `docs/superpowers/specs/2026-11-28-watershed-unreached-foreground-design.md`
(commit be89a40). Read it first; this plan argues from it.

## Global Constraints

- All work on branch `feat/watershed-unreached` in slideio-processor. **Nothing is merged to
  `main` before the owner signs off at the Task 6 checkpoint.** Phase 1 code is a candidate
  under test, not shipped code; this is how the spec's "before any production code" is
  read.
- Parameter `unreachedForeground`, a string, `"Fail"` (default) or `"Drop"`. C++:
  `enum class WatershedUnreached : int32_t { Fail, Drop };` and
  `WatershedParameters& unreachedForeground(WatershedUnreached);` in `segment.hpp`.
- The key is recorded in the parameter bag **only when `"Drop"`** (the
  `minimumMarkerValue` precedent). A Fail bag stays byte for byte what it is today, so
  `defaults->parameters().count()` stays 6.
- Fail mode is unchanged, its messages included.
- Drop clears every tile pixel the reach check would refuse, in both `failReach` branches
  (seed beyond the spacing, no seed). A cleared pixel takes no basin, joins no object,
  writes 0 to the label plane and adds nothing to measurements.
- Identical to Fail where Fail is robust: on an input Fail runs at **every** tile size,
  Drop drops nothing and returns identical objects, ids, measurements and label plane.
- Counter (phase 2): `droppedPixels`, in level pixels at the analysis level, counted once
  per tile pixel. Same pass semantics as `countTruncatedObjects`.
- Gate pass rule: zero mismatches in drop set, objects (ids, rings, measurements) and
  dropped count over ≥ 30,000 random inputs plus every generated shape. Spacings 1–8 and
  31; both `markers` modes; with and without `minimumMarkerValue`; tile sizes from 1 to
  2 × halo, including non-dividing sizes; worker counts 1 and 4.
- Requirements document goes from version 0.16 to **0.17** (phase 2). UR-SEG-8 is amended,
  not renumbered.
- Hygiene: GCC/Clang `-Werror` (no unused, no sign-compare); keep each file's existing line
  endings; never run two test suites at once.

## Review Focus

- **Drop with `markers = "AuxiliaryRaster"`.** Anchor seeds can sit anywhere, so the reach
  check fires differently than with local maxima. Expected: the same clearing rule, and
  invariant across tilings. Pinned by `Watershed.DropWithAuxiliaryMarkers` (Task 2) and
  the harness's anchor mode (Task 4).
- **`minimumAreaPx` after dropping.** An object shrunk below the floor by dropped pixels is
  filtered as any small object is, the same at every tile size, and its kept pixels are
  *not* counted as dropped. Pinned by `Watershed.DropThenAreaFloor` (Task 2) and by the
  counter test in Task 7.
- **A basin cut into disjoint pieces by dropped pixels, across a seam.** Expected: emitted
  as at a tile size where no seam cuts it. Pinned by `Watershed.DropSplitsBasinAcrossSeam`
  (Task 8) and the harness's strand generator (Task 4).
- **Tile sizes that don't divide the level, and tiles of 1 pixel.** Level-edge tiles
  have truncated contexts. Expected: identical results. Pinned by the harness tile-size
  set (Task 4).
- **The counter under a restricted run with on-demand tiles.** A skipped tile evaluated
  on demand must not be counted twice. Pinned by
  `Executor.DroppedPixelsCountedOncePerPixelWithOnDemandTiles` (Task 7).

---

## Phase 1 — candidate, model, gate

### Task 1: The `unreachedForeground` parameter

**Files:**
- Modify: `include/slideio/processor/algorithm/segment/segment.hpp` (enum beside
  `WatershedMarkers`; setter beside `minimumMarkerValue`)
- Modify: `src/algorithm/segment/watershed.cpp` (`detail::WatershedValues` field, setter,
  constructor's bag recording, `int32Parameter`-style string parsing in the by-name
  factory near `p.maximumMarkerSpacingPx(int32Parameter(...))`)
- Test: `tests/algorithm/test_watershed.cpp`

**Interfaces:**
- Produces: `enum class WatershedUnreached : int32_t { Fail, Drop };`,
  `WatershedParameters& WatershedParameters::unreachedForeground(WatershedUnreached)`,
  `detail::WatershedValues::unreached` (default `Fail`), and the spellings
  `kUnreachedSpellings = {"Fail", "Drop"}` used for both parsing and recording.

- [ ] **Step 1: Write the failing tests** in `test_watershed.cpp`:
  - `Watershed.UnreachedAbsentByDefault`: `makeWatershed(WatershedParameters())` and
    `create(kName, OperationParameters())` both have `getString("unreachedForeground",
    "absent")` equal to `"absent"` and `parameters().count() == 6`. A typed
    `.unreachedForeground(WatershedUnreached::Fail)` is also absent with count 6.
  - `Watershed.UnreachedDropRoundTrips`: typed `.unreachedForeground(Drop)` and a named bag
    `setString("unreachedForeground", "Drop")` give equal `jsonOf(...)`, count 7, value
    `"Drop"`. A named `"Fail"` gives the default bag.
  - `Watershed.UnreachedRejected`: `setString("unreachedForeground", "drop")`, `"Skip"` and
    `setInt("unreachedForeground", 1)` each throw `InvalidArgumentError` whose message
    contains `unreachedForeground` and not the bad value.
- [ ] **Step 2: Run them.** `./build.sh && ./run-tests.sh -R "algorithm\.Watershed\.Unreached"`.
  Expected: build fails on the undeclared enum/setter.
- [ ] **Step 3: Implement** the enum, setter (moved-from check as for the other setters),
  `WatershedValues::unreached`, parsing via the same bad-value path `markers` uses, and
  recording with `setString("unreachedForeground", "Drop")` only when `Drop`.
- [ ] **Step 4: Run** `./run-tests.sh -R "algorithm\.Watershed\."`. Expected: all pass,
  `TypedAndNamedBagsAreEqual` and `MarkerFloorAbsentByDefault` unchanged.
- [ ] **Step 5: Commit** `algorithm: watershed unreachedForeground parameter (Fail | Drop), recorded only when Drop`.

### Task 2: Drop clears out-of-reach pixels (the plain rule)

**Files:**
- Modify: `src/algorithm/segment/watershed.cpp` (the reach-check loop after `flood.run()`)
- Test: `tests/algorithm/test_watershed.cpp`

**Interfaces:**
- Consumes: `WatershedUnreached`, `WatershedValues::unreached` (Task 1).
- Produces: in Drop mode, `seeds[i]` of every refused tile pixel is set to `kUnreached`
  before labelling. Nothing downstream changes, because `labelRegions` already treats
  `seedAt < 0` as background. Tasks 4 and 5 rely on this being the only Drop difference.

- [ ] **Step 1: Write the failing tests** (single tile via `ts::applyDirectObjects` /
  `ts::applyDirect`, the strip pixel function of `StripLongerThanSpacingThrows`, parameters
  `minimumMarkerDistancePx(2).maximumMarkerSpacingPx(8).unreachedForeground(Drop)`):
  - `Watershed.DropStripKeepsReach`: one object, area 18 (x 0..8, y 4..5). The label plane
    is non-zero exactly at x ≤ 8 on rows 4–5, and 0 at (9, 4) and (149, 5).
  - `Watershed.DropUnmarkedObject`: the strip with `.minimumMarkerValue(1000.0)` (above
    every sample, so no marker) gives zero objects and an all-zero label plane.
  - `Watershed.DropDiscKeepsChebyshevSquare`: the radius-60 disc of
    `SpacingExceededThrowsAtEveryTileSize` at spacing 32 gives one object. Every kept pixel
    satisfies `max(|x-100|, |y-100|) <= 32`, and every disc pixel satisfying it is kept.
  - `Watershed.DropWithAuxiliaryMarkers`: the strip with one anchor at (0, 4) via
    `runWithAnchors` at tile size 512 gives the same single 18-pixel object.
  - `Watershed.DropThenAreaFloor`: the strip with `.minimumAreaPx(19)` gives zero objects.
- [ ] **Step 2: Run** `./run-tests.sh -R "algorithm\.Watershed\.Drop"`. Expected: FAIL with
  the reach error.
- [ ] **Step 3: Implement.** In the reach loop, on a refused pixel call `failReach` when
  `Fail`; when `Drop`, set `seeds[i] = kUnreached`. Keep the loop before anything is
  emitted.
- [ ] **Step 4: Run** `./run-tests.sh -R "algorithm\.Watershed\."`. Expected: all pass,
  including every existing Fail-mode test.
- [ ] **Step 5: Commit** `algorithm: watershed Drop clears foreground beyond the reach`.

### Task 3: Whole-image reference model of the flood

**Files:**
- Create: `tests/algorithm/support/flood_model.hpp`, `tests/algorithm/support/flood_model.cpp`
- Modify: `tests/algorithm/CMakeLists.txt` (add `support/flood_model.cpp` to the library
  that already builds `support/harness.cpp`)
- Test: `tests/algorithm/test_flood_model.cpp` (added to `slideio_processor_algorithm_tests`)

**Interfaces:**
- Produces:
  ```cpp
  namespace slideio::processor::testing_support {
  enum class ModelRule { Fail, DropPlain, DropBounded };
  struct ModelParameters {
      int64_t markerDistance = 5;            // minimumMarkerDistancePx
      int64_t spacing = 32;                  // maximumMarkerSpacingPx
      std::optional<double> floor;           // minimumMarkerValue
      std::vector<int64_t> anchors;          // empty: LocalMaxima; else per-pixel value, -1 = none
      ModelRule rule = ModelRule::Fail;
  };
  struct ModelResult {
      bool refused = false;                  // Fail: some foreground beyond reach
      std::vector<int64_t> key;              // per pixel: basin key, or kModelBackground / kModelDropped
  };
  constexpr int64_t kModelBackground = -1;
  constexpr int64_t kModelDropped = -2;
  ModelResult modelWatershed(const std::vector<double>& values, int64_t width, int64_t height,
                             const ModelParameters& p);
  }
  ```
  The key is `(y << 31) | x` of the marker pixel, or the anchor value: the same identity
  `keyOf` uses in `watershed.cpp`.

- [ ] **Step 1: Write the failing tests** in `test_flood_model.cpp`:
  - `FloodModel.MatchesSingleTileOnTouchingDiscs`: on the two-disc input of
    `TouchingDiscsSeparated`, the model's key map and the real op's single-tile label plane
    (`ts::applyDirect`) induce the same partition of foreground. That is, a bijection
    between labels and keys, and background agrees.
  - `FloodModel.StripRules`: on the strip at spacing 8: `Fail` sets `refused`; `DropPlain`
    keeps x ≤ 8 and marks the rest `kModelDropped`; with no marker, every foreground pixel
    is `kModelDropped`.
- [ ] **Step 2: Run** `./build.sh && ./run-tests.sh -R "algorithm\.FloodModel\."`. Expected:
  link failure.
- [ ] **Step 3: Implement `modelWatershed`** deliberately simply, from SDS 06 "Watershed,
  exactly" (line ~1395), on the whole level as one context:
  - **Markers:** a foreground pixel that precedes every foreground pixel within Chebyshev
    `markerDistance` (values descending, then index ascending), and is not below
    `floor`. At the level edge, follow the SDS rule. Where SDS is silent, do what
    `watershed.cpp` does for a context equal to the level, and say so in a comment.
  - **Flood:** repeatedly take the highest-precedence unflooded foreground pixel with a
    flooded 4-neighbour. It joins the seed of its highest-precedence flooded neighbour. A
    plain O(n²) loop is fine at harness sizes; no priority queue, no code shared with
    `watershed.cpp`.
  - **`DropBounded`:** a neighbour's seed is a candidate only if that seed lies within
    Chebyshev `spacing` of the pixel. A pixel with no candidate stays unflooded and does
    not propagate.
  - **Reach:** under `Fail`, any foreground pixel unflooded or beyond `spacing` from its
    seed sets `refused`. Under both Drop rules, such pixels become `kModelDropped`.
- [ ] **Step 4: Run** `./run-tests.sh -R "algorithm\.FloodModel\."`. Expected: PASS.
- [ ] **Step 5: Commit** `tests: whole-image reference model of the watershed flood`.

### Task 4: Random-input invariance harness (the gate)

**Files:**
- Create: `tests/algorithm/test_watershed_model.cpp`, built as its own executable
  `slideio_processor_watershed_model_tests` in `tests/algorithm/CMakeLists.txt`, ctest
  prefix `algorithm.model.`, with ctest label `slow` and a timeout of 3600. It is kept
  out of the default suite's time budget but committed.
- Test: the file itself.

**Interfaces:**
- Consumes: `modelWatershed` (Task 3); Drop (Tasks 1–2); `ts::runEager`,
  `ts::runEagerObjects`, `ts::applyDirect`, `expectSameObjects`, `runWithAnchors`. Move
  the last two from `test_watershed.cpp` into `support/harness.{hpp,cpp}` so both files
  share them.
- Produces: a summary printed to stdout and recorded with `RecordProperty`: inputs run per
  rule, mismatches by kind, and Fail-mode findings. Task 6's report is built from it.

- [ ] **Step 1: Write the harness tests:**
  - `WatershedModel.RandomInputsDropPlain` runs 30,000 inputs from seeds `0..29999`
    (`std::mt19937_64`). Each input:
    - is 16–48 px a side, Float32 or Byte;
    - is built from few distinct values with plateaus. Half the inputs are distance
      transforms of random discs and strands, capped like `pixel.distance_transform`'s
      `maximumDistancePx`;
    - has spacing drawn from {1..8, 31} and `markerDistance` from 1..spacing;
    - has a floor present in half of the inputs;
    - uses anchors (AuxiliaryRaster) in a quarter of the inputs.

    For each input:
    1. Run the model (`DropPlain`) and the real op single-tile (`applyDirect`, Drop). The
       partitions must match (`FloodModel` comparison), and so must the dropped sets:
       `kModelDropped` against label 0 on foreground.
    2. Run real Drop tiled at sizes {1, 3, 7, halo/2, halo, 2·halo + 1}, where halo is
       2·spacing + 1, with 1 and 4 workers. Objects must equal the single-tile objects
       (`expectSameObjects`), and the dropped mask from `runEager(..., bindObjects=true)`
       (label 0 on foreground) must equal the single-tile one.
    3. Run real Fail at the same tile sizes and record pass/throw per size:
       - if it passes at every size, Drop must have dropped nothing and match Fail's
         objects exactly;
       - if it passes at some sizes and throws at others, record a Fail-mode finding (the
         seed and the sizes). This does not fail the test.
  - `WatershedModel.GeneratedShapes`: the same three comparisons on fixed generators:
    - a strand longer than the spacing, at angles 0°, 45°, 90° and placed across seams;
    - an unmarked blob;
    - the blob-touching-a-nucleus case from the spec, with a blob peak more than 2s + 1
      from the nucleus and tiles chosen so some contexts hold the peak and some don't;
    - a 64-px disc at spacing 31.
- [ ] **Step 2: Build and run** `./build.sh && ./run-tests.sh -R "algorithm\.model\."`.
- [ ] **Step 3: Read the result.** Zero mismatches in both tests means the plain rule
  passes; go to Task 6 and skip Task 5. Any mismatch: save the failing seeds, go to Task 5.
- [ ] **Step 4: Commit** the harness and, if it passed, the summary numbers in the commit
  message: `tests: watershed invariance harness (plain Drop: <N> inputs, 0 mismatches)`.

### Task 5 (only if Task 4 found mismatches): fall-backs

**Files:**
- Modify: `src/algorithm/segment/watershed.cpp` (`Flood::run` gains a Drop-only bound;
  `halo()` for 5b)
- Modify: `tests/algorithm/test_watershed_model.cpp` (model rule `DropBounded`; halo sizes)

**Interfaces:**
- Consumes: Task 4's failing seeds, `ModelRule::DropBounded`.
- Produces: whichever rule passes becomes Drop's only behaviour. No public switch between
  rules is added.

- [ ] **Step 1 (5a, bounded claims):** in Drop mode, when `Flood::run` picks `first`, skip
  flooded neighbours whose seed lies further than Chebyshev `spacing_` from the pixel. A
  pixel with no candidate stays `kUnreached` and is not enqueued from. Fail mode keeps
  today's code path exactly.
- [ ] **Step 2:** switch the harness's model rule to `DropBounded` and add the failing seeds
  from Task 4 as named regression inputs. Run `./run-tests.sh -R "algorithm\.model\."`.
  Also run `./run-tests.sh -R "algorithm\.Watershed\."`: Task 2's tests must still pass.
  Zero mismatches → Task 6.
- [ ] **Step 3 (5b, wider halo, only if 5a still mismatches):** `halo()` returns
  `Halo::uniform(4 * spacing_ + 1)` when Drop, and `2 * spacing_ + 1` otherwise. Harness
  tile sizes scale to the new halo. Re-run the harness.
- [ ] **Step 4 (5b only): measure the cost.**
  - Working set: re-run `./run-tests.sh -R "algorithm\.ws\."` for watershed's per-pixel
    figure and record whether `kBytesPerPixel` (288) still holds at the larger halo.
  - Time: a one-off timing gtest (not committed) on a 2048 × 2048 nuclei-chain-like
    distance transform at `s = 91`, 512-px tiles, comparing halo 183 against 365. Record
    the wall time of each.
- [ ] **Step 5: Commit** whichever rule passed, with the harness result in the message. If
  neither passed, commit the harness with the failing seeds marked `DISABLED_`-prefixed
  regression cases, and go to Task 6 with a "no rule passed" report.

### Task 6: Checkpoint report — STOP for owner sign-off

**Files:**
- Create: `docs/superpowers/specs/2026-11-28-watershed-unreached-foreground-gate.md`

- [ ] **Step 1: Write the report:**
  - which rule passed (plain, bounded, or bounded with the wider halo), or the
    counter-examples (seeds, parameters, a picture of the mismatching pixels as ASCII)
    if none did;
  - the inputs and parameters actually covered, against the pass rule in Global
    Constraints;
  - every Fail-mode finding (the input seed, the tile sizes that pass and that throw);
  - for 5b, the working-set and timing figures from Task 5 Step 4.
- [ ] **Step 2: Commit** `docs: watershed Drop gate report` and push the branch
  `feat/watershed-unreached` to origin. Don't open a merge.
- [ ] **Step 3: STOP.** Hand the report to the owner. Phase 2 starts only on their explicit
  sign-off. If the report says no rule passed, the work ends here.

---

## Phase 2 — after sign-off

### Task 7: The `droppedPixels` run counter

**Files:**
- Modify: `include/slideio/processor/pipeline/operation.hpp` (`countDroppedPixels` beside
  `countTruncatedObjects`, same doc structure)
- Modify: `src/pipeline/operation_context.cpp`, `src/pipeline/detail/operation_context_state.hpp`
  (`std::atomic<int64_t>* droppedCounter = nullptr;`)
- Modify: `src/pipeline/detail/tile_evaluator.hpp` (`RunCounters::droppedPixels` and its
  `reset()`), `src/pipeline/detail/tile_evaluator.cpp` (set it where `truncatedCounter` is
  set, in the final pass; the lazy read's own discarded total too)
- Modify: `include/slideio/processor/pipeline/pipeline.hpp`, `src/pipeline/pipeline.cpp`
  (`int64_t droppedPixelCount() const noexcept;`)
- Modify: `include/slideio/processor/workspace/stage_runner.hpp`,
  `src/workspace/stage_runner.cpp` (`int64_t droppedPixels() const noexcept;`, captured
  where `truncated` is)
- Modify: `include/slideio/processor/run/slide_run_result.hpp`,
  `src/run/slide_run_result.cpp`, `src/run/slide_run.cpp` (`addStage(name, ran,
  objectsWritten, truncatedObjects, droppedPixels)`;
  `int64_t stageDroppedPixels(int32_t index) const;`)
- Modify: `src/algorithm/segment/watershed.cpp` (count the cleared tile pixels per tile,
  then call `context.countDroppedPixels(n)` once)
- Modify: `tests/algorithm/support/harness.{hpp,cpp}` (`struct EagerRun { std::vector<ObjectRecord>
  objects; int64_t droppedPixels; int64_t truncatedObjects; };` and
  `EagerRun runEagerCounted(scene, spec, tileSize, workers)`)
- Test: `tests/algorithm/test_watershed.cpp`, `tests/pipeline/test_executor.cpp`,
  `tests/workspace/test_stage_runner.cpp`, `tests/run/test_slide_run.cpp`

**Interfaces:**
- Consumes: Drop (Tasks 1–2, plus 5 if used).
- Produces: `OperationContext::countDroppedPixels(int64_t)`,
  `Pipeline::droppedPixelCount()`, `StageRunner::droppedPixels()`,
  `SlideRunResult::stageDroppedPixels(int32_t)`. The follow-up Python PR consumes the
  last.

- [ ] **Step 1: Write the failing tests:**
  - `Watershed.DropCountsPixels`: the strip (Task 2) through `runEagerCounted` at tile sizes
    16, 64, 512 gives `droppedPixels == 2 * 141` (x 9..149, two rows) every time. Fail mode
    on an input that passes gives 0.
  - `Watershed.DropThenAreaFloorCountsOnlyDropped`: Task 2's area-floor case reports 282,
    not 300.
  - `Executor.DroppedPixelsCountedOncePerPixelWithOnDemandTiles`: a restricted run in which
    an object reaches into a skipped tile (so `tilesEvaluatedOnDemand > 0`) reports the
    same `droppedPixelCount()` as the unrestricted run over the same pixels. If the
    framework can evaluate a tile twice in one pass, fix the double count where the
    counter is incremented, and say how in the commit message.
  - `OperationContext.CountDroppedPixelsRejectsNegative`: the shape of the existing
    negative-count test for truncated objects.
  - `StageRunner.ReportsDroppedPixels` and `SlideRun.ReportsDroppedPixelsPerStage`: a
    one-stage Drop pipeline on the strip scene reports 282 at both layers. Fail stages
    report 0.
- [ ] **Step 2: Run** `./build.sh`. Expected: compile failures on the undeclared members.
- [ ] **Step 3: Implement** the plumbing by following `truncatedObjects` site for site; the
  grep list is in the spec's "Reporting" section.
- [ ] **Step 4: Run** `./run-tests.sh` (the whole suite, once). Expected: all pass.
- [ ] **Step 5: Commit** `run: droppedPixels counter from OperationContext to SlideRunResult; watershed Drop counts`.

### Task 8: Fail-equivalence and split-basin tests

**Files:**
- Modify: `tests/algorithm/test_watershed.cpp`, `tests/algorithm/test_nuclei_chain.cpp`

**Interfaces:**
- Consumes: `runEagerCounted` (Task 7).

- [ ] **Step 1: Write the tests:**
  - `Watershed.DropEqualsFailWhereFailRuns`: every input of `TouchingDiscsSeparated`,
    `TilingInvariantAtThreeSizes` (grid and both sheets), `AuxiliaryMarkers` and
    `MarkerFloorKeepsTiling` is run in Drop at the tile sizes those tests use. Each must
    give `expectSameObjects` against its Fail result and `droppedPixels == 0`.
  - `NucleiChain.DropEqualsFail`: the nuclei-chain test's pipeline with
    `unreachedForeground(Drop)` must give identical objects and count 0.
  - `Watershed.DropSplitsBasinAcrossSeam`: a marker at the left end of an L-shaped strip,
    whose corner lies beyond the spacing while both arms come back within it. The basin's
    kept pixels form two disjoint pieces with one key. Tile sizes chosen so a seam runs
    between the pieces must give the same objects as tile size 512.
- [ ] **Step 2: Run** `./run-tests.sh -R "algorithm\.(Watershed|NucleiChain)\."`. Expected:
  PASS. A failure here is a Drop bug; fix it in `watershed.cpp`, not in the test.
- [ ] **Step 3: Commit** `tests: watershed Drop equals Fail where Fail runs; split basin across seams`.

### Task 9: Requirements and design documents

**Files:**
- Modify: `docs/requirements.md` (header version 0.17, date; "Changes in 0.17"; UR-SEG-8 text
  and acceptance)
- Modify: `docs/superpowers/specs/sds/06-algorithm.md` (parameter table row after line 164's
  `segment.watershed` rows; a Drop paragraph in "Watershed, and why it is tileable after
  all"; replace the "about 28,000 tie-heavy random inputs" sentence in "Watershed,
  exactly" with a reference to `test_watershed_model.cpp` and its figures; the
  working-set row at line ~242 only if 5b changed the halo)
- Modify: `docs/superpowers/specs/sds/08-class-diagrams.md` (`WatershedUnreached` beside
  `WatershedMarkers`; the setter on `WatershedParameters`)
- Modify: `include/slideio/processor/algorithm/segment/segment.hpp` (the
  `maximumMarkerSpacingPx` doc comment, exact copy from the spec: "unless
  `unreachedForeground` is `Drop`, which leaves it out and counts it")

- [ ] **Step 1: Write the UR-SEG-8 amendment** with the spec's sentence verbatim: "the
  caller may instead declare that foreground beyond the reach is to be left out of every
  object; the result then remains invariant to tile size, and the run reports how much
  was left out". Add the acceptance criterion: on Drop-triggering inputs, three tile
  sizes give identical objects and dropped counts.
- [ ] **Step 2: Write the SDS 06 and 08 changes** listed above. Use the gate report's
  figures and the rule that passed.
- [ ] **Step 3: Build** `./build.sh` (Doxygen comment change only) and check the SDS
  section numbers referenced in the Changes entry exist.
- [ ] **Step 4: Commit** `docs: UR-SEG-8 amended for watershed Drop (0.17); SDS 06/08`.
- [ ] **Step 5: Finish the branch** via superpowers:finishing-a-development-branch. Merging
  to `main` needs the owner's go-ahead.

## Follow-up (separate PR, slideio-processor-python, not in this plan)

Re-pin slideio-processor at the merged commit; add `dropped_pixels` to the stage and run
results (`src/stages.cpp`, `src/slide_run.cpp`, `workspace.py`, `run.py`); print it in
`examples/nucleus_segmentation.py` when non-zero.
