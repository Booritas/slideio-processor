# Plan C Review Follow-up Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the six PR #15 review findings deferred to this PR (`docs/project.md`, "Plan C review follow-up").

**Architecture:** Two small cleanups (an unused halo, a duplicated pixel-size rule), two public-API additions that let the run skip record copies (an ownership-taking `ObjectSink` emit, an operation's declaration that it has no finishing step), and two seam-merger changes (edge contacts indexed by side and position, then charged to the memory budget as seam points already are). Each task amends the SDS section it changes and the traceability rows of the tests it adds.

**Tech Stack:** C++17, CMake, Conan 2, OpenCV 4.14, GoogleTest, MSVC (Windows), Doxygen.

**Spec:** `docs/project.md` ("Plan C review follow-up"), with the sections it amends: `docs/superpowers/specs/sds/01-core.md` (ObjectSink), `05-pipeline.md` (finishing, seam merger memory), `06-algorithm.md` §6.7 (segmenter halos), `08-class-diagrams.md` where the changed classes appear.

## Global Constraints

- Build `./build.sh --skip-slideio`; tests `./run-tests.sh` (all must pass); one test `ctest --test-dir build -C Release -R <name>` (the test exe cannot be run directly: DLL path). Docs `cmake --build build --target docs --config Release`.
- Zero compiler warnings and zero Doxygen warnings.
- Every source and test file starts with its `// Requirements: ...` line; a new file gets one, a changed file keeps its list current.
- `docs/traceability.md` rows list every test of the files they cover; add each new test there.
- Error messages name the operation or component only: no pixel, sample, coordinate or parameter values.
- Public API changes are documented in the header (Doxygen, `@par Thread-safety`, `@par Requirements`) and in the SDS section that declares the class.
- Match the surrounding code's comment density, naming and idiom. No Python on the machine: edit with the Edit tool or sed.
- Commit messages: conventional (`fix(...)`, `feat(...)`, `docs: ...`), one commit per task at least.

## Review Focus

1. An existing `ObjectSink` that overrides only `emit()` (the storage `ObjectWriter`, the test sinks) must receive every object through the new entry points' defaults, unchanged — Task 3 test `ObjectSinkDefaults.OwnedAndKeptReachEmit`.
2. A third-party operation that overrides `finishObject()` but not `finishesObjects()` must still be finished — Task 4 test `ObjectFinishing.FinishingIsTheDefault`.
3. On a 4-connected layer, two fragments whose seam intervals meet at one point must not join after the edge index replaces the scan — Task 5 test `SeamMerge.IndexedContactsJoinOnlyOverlaps`.
4. Contacts on one side of a seam may overlap each other (two layers, or two join keys); the index must not assume disjoint intervals — Task 5 test `SeamMerge.IndexedContactsJoinAcrossLayersAndKeys`.
5. With a budget too small for the edge contacts, the merger must never wait and must still merge identically — Task 6 test `SeamSpill.EdgeContactsAreChargedUntilTheirTilesComplete` (tight-budget half).

---

### Task 1: One pixel-size rule, in core

**Files:**
- Modify: `include/slideio/processor/core/measurement.hpp`, `src/core/measurement.cpp`
- Modify: `src/pipeline/detail/pixel_size.cpp` (keep `levelZeroPixelSizeMicrons` as a wrapper), `src/algorithm/measure/measure.cpp` (`scenePixelSizeMicrons` as a wrapper; drop the "repeated here" comment)
- Modify: `docs/superpowers/specs/sds/01-core.md` (declare the function beside the measurement types), `05-pipeline.md` §5.1 `pixelSizeMicrons()` and `06-algorithm.md` near line 1309 (both point to the one rule)
- Test: `tests/core/test_measurement.cpp`

**Interfaces:**
- Produces: `SLIDEIO_PROCESSOR_DATA_EXPORTS double pixelSizeMicronsFromResolution(double xMetres, double yMetres) noexcept;` in namespace `slideio::processor`. Core is in the data library, which must not link SlideIO, so the helper takes the two resolution values, not a scene.

- [ ] **Step 1: Write the failing test** `Measurement.PixelSizeFromResolution` in `tests/core/test_measurement.cpp`, with the cases both existing tests use:

```cpp
EXPECT_DOUBLE_EQ(pixelSizeMicronsFromResolution(0.25e-6, 0.25e-6), 0.25);
EXPECT_EQ(pixelSizeMicronsFromResolution(0.0, 0.0), 0.0);
EXPECT_EQ(pixelSizeMicronsFromResolution(0.25e-6, 0.5e-6), 0.0);   // anisotropic
EXPECT_EQ(pixelSizeMicronsFromResolution(0.25e-6, 0.0), 0.0);
EXPECT_EQ(pixelSizeMicronsFromResolution(-0.25e-6, -0.25e-6), 0.0);
EXPECT_EQ(pixelSizeMicronsFromResolution(std::nan(""), 0.25e-6), 0.0);
EXPECT_EQ(pixelSizeMicronsFromResolution(INFINITY, INFINITY), 0.0);
EXPECT_NEAR(pixelSizeMicronsFromResolution(0.25e-6, 0.25e-6 * (1.0 + 5e-7)), 0.25 * (1.0 + 2.5e-7), 1e-12);
EXPECT_EQ(pixelSizeMicronsFromResolution(0.25e-6, 0.25e-6 * (1.0 + 5e-6)), 0.0);
```

- [ ] **Step 2: Run it; expect a compile failure** (`pixelSizeMicronsFromResolution` undeclared).
- [ ] **Step 3: Implement** the function with the rule now in `pixel_size.cpp` (finite, positive, agree within `1e-6 * max`, mean × 10^6, else 0). Make `levelZeroPixelSizeMicrons` and `scenePixelSizeMicrons` read `getResolution()` inside their existing `try` and return the helper's result; they keep their null and exception handling.
- [ ] **Step 4: Run** `ctest ... -R "Measurement|PixelSize|MeasurementFinish"`; expect PASS, including the unchanged `test_pixel_size.cpp` and `test_measurement_finish.cpp` cases.
- [ ] **Step 5: Update** the SDS text and traceability; build docs.
- [ ] **Step 6: Commit** `refactor(core): one level-0 pixel-size rule`.

### Task 2: Connected components and contours declare no halo

**Files:**
- Modify: `src/algorithm/segment/connected_components.cpp:149`, `src/algorithm/segment/contours.cpp:163` (remove the `halo()` overrides; the base default is no halo)
- Modify: `include/slideio/processor/algorithm/segment/segment.hpp:231,409`, `docs/superpowers/specs/sds/06-algorithm.md:790,804` ("halo 1" → "halo none")
- Test: `tests/algorithm/test_connected_components.cpp`, `tests/algorithm/test_contours.cpp:168`

**Interfaces:** none new. `apply()` keeps computing offsets from `haloRectAtLevel()`, which now equals the tile rect, so the offsets are 0.

- [ ] **Step 1: Change the tests first:** in `test_contours.cpp:168` expect `op->halo().isEmpty()`; add `EXPECT_TRUE(op->halo().isEmpty());` to a connected-components test that already builds the op with defaults. Run them; expect FAIL.
- [ ] **Step 2: Remove both overrides** and update the doc text. Check the working-set comments in both files: the figures are per pixel of the halo'd tile, which is now the tile, so the numbers stay; reword only text that says the tile is grown.
- [ ] **Step 3: Run** `ctest ... -R "ConnectedComponents|Contours|WorkingSet|Tiling|Seam"`; expect PASS. The tiling-invariance tests (objects identical at every tile size) are the proof the halo was never read; if one fails, stop and report rather than restore the halo.
- [ ] **Step 4: Run the full suite**, then commit `fix(algorithm): connected components and contours read no halo`.

### Task 3: An ownership-taking `ObjectSink` emit

**Files:**
- Modify: `include/slideio/processor/core/flow.hpp` (ObjectSink), `src/core/flow.cpp` (defaults)
- Modify: `src/pipeline/detail/finishing_sink.{hpp,cpp}`, `src/pipeline/detail/tile_evaluator.{hpp,cpp}` (TileObjectFilter), `src/pipeline/seam_merger.cpp` (`Impl::emit`), `src/algorithm/detail/region_emit.cpp:146`
- Modify: `docs/superpowers/specs/sds/01-core.md:696` (ObjectSink block and its prose), `05-pipeline.md` (finishing-sink and tile-filter text), `08-class-diagrams.md` (ObjectSink members)
- Test: `tests/core/test_flow.cpp` (create if absent, else the file holding ObjectSink tests), `tests/pipeline/test_object_finishing.cpp`

**Interfaces:**
- Produces, on `ObjectSink` (public, data library):
  - `virtual bool emitKept(const ObjectRecord& record);` — emits one object and tells whether the sink kept it (false: it discarded it). Default: `emit(record); return true;`.
  - `virtual bool emitOwned(ObjectRecord&& record);` — as `emitKept()`, for a record the caller gives up; the sink may move from it or finish it in place. Default: `return emitKept(record);`.
  - `emit()` stays pure virtual and keeps its meaning. A derived class overriding only some of the three adds `using ObjectSink::emitKept;` etc. where name hiding needs it.
- `FinishingSink` overrides both (replacing its non-virtual `emitKept`/`emitOwned(ObjectRecord&)`); `emit()` calls `emitKept()`.
- `TileObjectFilter` overrides `emitKept()` (`emit()` calls it) and `emitOwned()`; it forwards to `target_->emitKept()` / `target_->emitOwned(std::move(...))` and counts a record only when the target returns true. The `finishing_` member and the `dynamic_cast` are removed.
- `SeamMerger::Impl::emit` and `detail::emitRegions` pass the records they build with `emitOwned(std::move(record))`.

- [ ] **Step 1: Write the failing tests.**
  - `ObjectSinkDefaults.OwnedAndKeptReachEmit`: a sink overriding only `emit`/`flush`/`emittedCount`; `emitKept(r)` and `emitOwned(ObjectRecord(r))` both return true and the sink's `emit` saw two records equal to `r` (id, layer, ring, measurements).
  - `ObjectFinishing.OwnedRecordsAreNotCopied`: a target sink overriding `emitOwned` that records whether each record arrived through it; a `TileObjectFilter` over a `FinishingSink` over that target; emit one non-normalised record (rotated ring, as in `TileFilterCountsOnlyWhatFinishingKeeps`) and one normalised one; expect both to reach the target through `emitOwned`, and the filter's and finishing sink's counts as in that test.
  - Keep `ObjectFinishing.TileFilterCountsOnlyWhatFinishingKeeps` passing unchanged.
- [ ] **Step 2: Run them;** expect compile failure, then FAIL.
- [ ] **Step 3: Implement** as in Interfaces. In `FinishingSink::emitKept` a record with a producer is copied once and the copy finished; in `emitOwned` the record itself is finished and moved to the target with `target_.emitOwned(std::move(record))`. A record on a layer with no producer goes on with `target_.emitKept(record)` / `target_.emitOwned(std::move(record))`.
- [ ] **Step 4: Run** `ctest ... -R "ObjectSink|ObjectFinishing|Seam|ConnectedComponents|Watershed|Contours|TissueDetect"`, then the full suite.
- [ ] **Step 5: Update** SDS 01/05/08, the header docs and traceability; build docs.
- [ ] **Step 6: Commit** `feat(core): ObjectSink takes ownership of an emitted record`.

### Task 4: An operation may declare it has no finishing step

**Files:**
- Modify: `include/slideio/processor/pipeline/operation.hpp` (beside `finishObject`, line ~789), `src/pipeline/operation.cpp`
- Modify: `src/pipeline/detail/finishing_sink.{hpp,cpp}`
- Modify: `src/algorithm/detail/measurement_finish.{hpp,cpp}` and the four segmenters (`connected_components.cpp`, `contours.cpp`, `watershed.cpp`, `tissue_detect.cpp`)
- Modify: `tests/pipeline/test_support/test_operations.{hpp,cpp}` (`test.components` gains bool parameter `noFinishing`, default false)
- Modify: `docs/superpowers/specs/sds/05-pipeline.md` (the `finishObject` block near line 185 and "Every object passes through its producer's `finishObject`" near line 288), `06-algorithm.md` (measurement plan), `08-class-diagrams.md`
- Test: `tests/pipeline/test_object_finishing.cpp`, `tests/algorithm/test_measurement_finish.cpp`

**Interfaces:**
- Consumes: Task 3's `ObjectSink::emitKept/emitOwned` and `FinishingSink` overrides.
- Produces:
  - `virtual bool Operation::finishesObjects() const;` — true when `finishObject()` may change or discard an object; false lets the run pass the operation's objects on without calling it, so without a copy. **True by default**, so an operation that overrides `finishObject()` alone is still finished. Documented with `@par Thread-safety` (reentrant) and `@par Requirements` UR-SEG-3, UR-SEG-5, UR-SEG-7.
  - `bool detail::MeasurementPlan::changesObjects() const noexcept;` — false only when `finish()` leaves every record unchanged and keeps it for every input; when a configuration is not certain to be a no-op, true.
  - Each segmenter's `finishesObjects()` returns `plan_.changesObjects()`.
  - `FinishingSink` reads `finishesObjects()` once per producer in its constructor and treats a producer that returns false like a layer with no producer.

- [ ] **Step 1: Write the failing tests.**
  - `ObjectFinishing.OperationWithoutFinishingIsPassedThrough`: `test.components` with `finishTag` true and `noFinishing` true, tile side 100; every record lacks the `"finished"` measurement, and the run's `droppedByFilter` is 0 even with `finishMinArea` 50.
  - `ObjectFinishing.FinishingIsTheDefault`: a minimal `Operation` subclass that overrides `finishObject` only; `finishesObjects()` returns true. Plus `EveryTileObjectIsFinished` still passes.
  - `MeasurementFinish.ChangesObjectsOnlyWhenFinishDoes`: for each plan in a small table (the defaults; an empty selection with no bounds and holes kept; a minimum area; holes removed), if `changesObjects()` is false then `finish()` keeps a record with a hole and two measurements and leaves it equal to its copy. The empty-selection-no-bounds-holes-kept case must report false; the minimum-area and holes-removed cases true.
- [ ] **Step 2: Run them;** expect FAIL.
- [ ] **Step 3: Implement** as in Interfaces.
- [ ] **Step 4: Run** `ctest ... -R "ObjectFinishing|MeasurementFinish|ConnectedComponents|Watershed|Contours|TissueDetect|Seam"`, then the full suite.
- [ ] **Step 5: Update** SDS and traceability; build docs.
- [ ] **Step 6: Commit** `feat(pipeline): an operation may declare it has no finishing step`.

### Task 5: Edge contacts indexed by side and position

**Files:**
- Modify: `src/pipeline/detail/seam_merger_access.hpp` (new `SeamEdgesWaiting`; `edges_` becomes `std::unordered_map<int64_t, detail::SeamEdgesWaiting>`), `src/pipeline/seam_merger.cpp:172-185` (the join loop)
- Modify: `docs/superpowers/specs/sds/05-pipeline.md` §5.7 (how a contact finds its partners)
- Test: `tests/pipeline/test_seam_merger.cpp`

**Interfaces:**
- Produces, in `seam_merger_access.hpp`:

```cpp
/// The edge contacts waiting between two side-adjacent tiles until both
/// complete. Side 0 holds the contacts of the lower tile index of the pair,
/// side 1 those of the higher, each keyed by the contact's begin; longest is
/// the longest interval (end - begin) on that side, which bounds the search.
/// Intervals on one side may overlap (several layers or join keys).
struct SeamEdgesWaiting {
    std::multimap<int64_t, SeamEdgeContact> bySide[2];
    int64_t longest[2] = {0, 0};
};
```

- The join for a new contact `c` from side `s` looks only at side `1 - s`: from `lower_bound(c.begin - longest[1 - s] + 1)` while the key is below `c.end`, applying the existing tests (`joinKey`, not taken, same layer, `contactsJoin`). Then it inserts `c` on side `s` and updates `longest[s]`. The `other.tile != tile` test is implied by the side and goes.

- [ ] **Step 1: Write the failing-or-guarding tests** (they pass on the current scan and must still pass after):
  - `SeamMerge.IndexedContactsJoinInAnyOrder`: a 2x1 grid of 20x20 tiles; one fragment in tile 0 touching the seam over rows 2..18, and four in tile 1 over rows 2..4, 6..8, 10..12, 14..18; added in both orders (tile 0 first, tile 1 first); one merged object each time, with the expected area.
  - `SeamMerge.IndexedContactsJoinOnlyOverlaps`: 4-connected; tile 0 fragment over rows 2..5, tile 1 fragment over rows 5..8 (intervals meet at one point); two objects.
  - `SeamMerge.IndexedContactsJoinAcrossLayersAndKeys`: two layers whose fragments overlap on the same side and seam; each joins only its own layer's partner.
- [ ] **Step 2: Run them on the current code;** expect PASS (they pin behaviour).
- [ ] **Step 3: Implement** the struct and the join; `complete()`'s `edges_.erase` and the clear in `takeRemaining()` keep working on the new map.
- [ ] **Step 4: Run** `ctest ... -R "Seam"`, then the full suite.
- [ ] **Step 5: Update** SDS 05 §5.7 and traceability.
- [ ] **Step 6: Commit** `perf(pipeline): index waiting edge contacts by side and position`.

### Task 6: Edge contacts charged to the memory budget

**Files:**
- Modify: `src/pipeline/detail/seam_merger_access.hpp` (`SeamEdgesWaiting` gains `BudgetHold held;`; new `kSeamEdgeContactBytes`; `holdPoints` → `holdWaiting(BudgetHold& held, int64_t bytes)`; `unreservedPointBytes` → `unreservedSeamBytes`)
- Modify: `src/pipeline/seam_merger.cpp` (`add()` tallies contacts per tile pair as it tallies points, and holds both after `reserveFor`)
- Modify: `docs/superpowers/specs/sds/05-pipeline.md:1789-1798` (the paragraph on seam points covers edge contacts too)
- Test: `tests/pipeline/test_seam_spill.cpp`

**Interfaces:**
- Consumes: Task 5's `SeamEdgesWaiting`.
- Produces: `constexpr int64_t kSeamEdgeContactBytes = static_cast<int64_t>(sizeof(SeamEdgeContact)) + 64;` — the contact plus one multimap node (key, links, colour), documented like `kSeamPointContactBytes`. `holdWaiting` keeps `holdPoints`' behaviour: `tryReserve`, else spill the oldest-opened component, else keep unreserved and add to `unreservedSeamBytes`; never waits.

- [ ] **Step 1: Write the failing test** `SeamSpill.EdgeContactsAreChargedUntilTheirTilesComplete`, shaped like `SeamPointsAreChargedUntilTheirTilesComplete`: a 2x1 grid of 5x5 tiles, 4-connected layer, `a` = rectangle(3, 0, 2, 5) in tile 0 and `b` = rectangle(5, 0, 2, 5) in tile 1. With an ample budget: after adding `a`, `reservedBytes() - residentBytes(a) == kSeamEdgeContactBytes`; one object of area 20; no spill; `reservedBytes() == 0` at the end. With a budget of `residentBytes(a)`: both fragments spill, one object of area 20, `reservedBytes() == 0` at the end.
- [ ] **Step 2: Update** `SeamPointsAreChargedUntilTheirTilesComplete`: `a` = rectangle(3, 3, 2, 2) has two edge contacts (x = 5 and y = 5), so its held bytes are now point bytes plus `2 * kSeamEdgeContactBytes`; assert that split exactly. Run both; expect FAIL.
- [ ] **Step 3: Implement** as in Interfaces.
- [ ] **Step 4: Run** `ctest ... -R "Seam"`, then the full suite.
- [ ] **Step 5: Update** SDS 05 and traceability; in `docs/project.md` mark "Plan C review follow-up" **Implemented** (PR number added at push).
- [ ] **Step 6: Commit** `fix(pipeline): charge waiting seam edge contacts to the memory budget`.

## Rulings

- Task 2: `halo()` is pure virtual, so the two segmenters return `Halo::none()` explicitly instead of removing the override — a pure virtual cannot be left unimplemented.
- Task 4: no `MeasurementPlan` configuration is a no-op (finish() always writes or strips `area`), so `MeasurementPlan::changesObjects()`, the segmenters' `finishesObjects()` overrides and the `MeasurementFinish.ChangesObjectsOnlyWhenFinishDoes` test were dropped; segmenters keep the default (true), and `ConnectedComponents.TypedAndNamedBagsAreEqual` asserts it — a segmenter's finishing step always changes its objects.
- Task 5: the test suite is `SeamMerger` (the file's convention), not `SeamMerge`, and its Step 1 tests guard behaviour rather than fail first — the behaviour already existed and the tests pin it.
- Task 6: two exact-budget tests (`OldestComponentsSpillFirst`, `UnwritableScratchIsIoError`) had budgets raised by the new contact charge, with their geometry kept — the charge adds bytes per waiting contact, so the old budgets no longer forced the intended spill.
- Final review: the new virtuals were moved after the existing ones — the project appends new virtuals to keep the vtable order stable.
