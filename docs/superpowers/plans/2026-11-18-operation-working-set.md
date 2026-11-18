# Operation Working Set Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every operation declares the working memory one tile's computation needs beyond the framework's buffers. The pipeline counts that memory in the per-tile reservation, so the memory ceiling also bounds operations' own scratch. This was the user's decision (2026-11-18), taken before algorithm plan C.

**Architecture:**
- **Today `tileNeed` counts only framework buffers.** In `run_plan.cpp` it covers every step's planes, the reader's interleave buffer, the auxiliaries and the restriction masks, over the tile grown by the chain's halo. `workingSetBytesPerLevelPixel()` exists, but is consulted only for `WholeLevel` chains.
- **The fix:** give that declaration the same meaning for `Halo` operations, and add one declaration for memory that does not scale with pixels.
- **Operations run one at a time on a tile,** and a PerChannel operation runs plane by plane. So a tile's peak is the framework buffers plus the **largest** single operation's working set, not their sum.
- **The executor and the lazy scene need no change:** both reserve `RunPlan::tileBytes`, and this plan changes only how `tileBytes` is computed.

**Tech Stack:** C++17, CMake, Conan 2, GoogleTest, OpenCV 4.14 (private to the engine).

**Spec:** SDS 05 §5.1 (the `Operation` and `OperationFactory` declarations), §5.3 (`OperationCatalog`), §5.5 (`validate()`, whole-level chains) and §5.6 (tile memory). SDS 06 for the algorithm operations. The requirements are UR-EXE-2 (memory budget) and UR-EXT-3/4.

## Global Constraints

- The pipeline's and plan A's Global Constraints hold:
  - C++17;
  - `slideio::processor` and `detail`;
  - the export macros;
  - C-2, C-4, C-5, C-6;
  - no OpenCV in public headers;
  - Doxygen with `@throws` / `@par Thread-safety` / `@par Requirements`, and a 0-warning `docs` target;
  - `// Requirements:` lines;
  - no sample or parameter values in messages, and no plan vocabulary in code or public docs;
  - `./build.sh --skip-slideio` with 0 warnings and `./run-tests.sh` green.
- Branch `feat/working-set`, one commit per task, prefixes `feat(pipeline):`, `feat(algorithm):`, `test(algorithm):`, `docs:`. AI-generated code is human-reviewed before it reaches `main`.
- **ABI:** adding a virtual to `Operation` and `OperationFactory` changes their vtables. That is acceptable before the first release (no published binaries). The new virtuals go **after** the existing ones.

## Rulings on spec gaps

Numbered W1 onward.

| # | Gap | Ruling |
|---|---|---|
| W1 | What `workingSetBytesPerLevelPixel()` means for a `Halo` operation. | **One meaning for both requirements:** the operation's own working memory per pixel of the rect it is applied to, for one `apply()` call (one plane for a PerChannel operation). It excludes the input and output blocks the framework counts. For a `Halo` operation the rect is the tile grown by the halo; for a `WholeLevel` operation it is the level. The same figure covers `accumulate()` in a global-context pass. |
| W2 | Memory that does not scale with pixels: a median's window copy, a histogram's bins, a kernel's coefficients. | **New declaration** `virtual int64_t workingSetBytesPerTile() const;` on `Operation`, default 0. It is the operation's working memory per `apply()` or `accumulate()` call that does not scale with the tile's pixels. `OperationFactory` mirrors it for default parameters (default 0), and `OperationCatalog::workingSetBytesPerTile(const char* name)` answers it. Both declarations are parameter-dependent: the instance is authoritative, and the catalog does not compare them on `create()`, as with `halo()`. |
| W3 | How `tileNeed` counts them. | `tileNeed(w, h) = haloArea × tilePixelBytes + max over operations of (haloArea × perPixel + perTile)`. `haloArea` is the chain-halo'd tile area, which bounds every operation's own input rect. Negative declarations count as 0, and the arithmetic saturates as now. **Whole-level chains** keep their formula: the Σ of per-pixel declarations, a deliberately conservative bound. They also add the max of `perTile`. The refusal messages name the operation with the largest working set when it decides the outcome. |
| W4 | OpenCV allocates through its own allocator, which a test cannot count. | An operation calling OpenCV declares OpenCV's internal buffers analytically. The bound comes from the OpenCV 4.14 source, cited by file and function in a comment beside the declaration. It is tested only as far as the module's own allocations are concerned (W5). |
| W5 | How a declaration is proved. | The algorithm test executable replaces global `operator new` / `operator delete` with a counting pair, `tests/algorithm/support/allocation_meter.hpp/.cpp`. It keeps thread-local live and peak byte counts, and allocations made while no meter is active are not counted. `AllocationMeter meter; op.apply(ctx); meter.peakBytes()` measures one call. A generic test asserts that the measured peak is at most `haloArea × perPixel + perTile` for every built-in, at representative and upper-limit parameters, on the types it supports. For OpenCV paths, the analytic OpenCV share is subtracted from the declaration before the comparison. The comparison is therefore about the module's own memory, and the OpenCV share is documented, not measured. |

## Review Focus

1. **A chain of a cheap operation and an expensive one:** `tileBytes` must grow by the expensive one's working set only, not the sum. Pinned in Task 1.
2. **A bilateral filter at tile size 4096, diameter 255, under a 512 MB ceiling:** `validate()` refuses before any pixel is read, and the message names the operation. Pinned in Task 4.
3. **Otsu at 1,048,576 bins:** its per-tile working set covers the histograms, and a run under a ceiling that only just fits the buffers is refused. Pinned in Task 3.
4. **The lazy scene and the eager executor reserve the same `tileBytes`:** pinned in Task 1.
5. **The meter itself:** an operation that allocates 1 MB in `apply()` reads at least 1 MB, and allocations on other threads are not counted. Pinned in Task 2.

---

### Task 1: The declarations and the reservation

**Files:**
- Modify:
  - `include/slideio/processor/pipeline/operation.hpp` and `operation_catalog.hpp` (W1 doc, W2 declarations);
  - `src/pipeline/operation.cpp` and `operation_catalog.cpp`;
  - `src/pipeline/detail/run_plan.cpp` (W3);
  - `tests/pipeline/test_support/operation_conformance.cpp` (compare `workingSetBytesPerTile()` on round trip);
  - `tests/pipeline/test_support/test_operations.hpp/.cpp` (`PrototypeFactory` mirrors it; one probe operation `test.workingSet` with parameters `perPixel` and `perTile`).
- Test: `tests/pipeline/test_working_set.cpp`.

**Interfaces:**
- Produces:
  - `virtual int64_t Operation::workingSetBytesPerTile() const;`
  - `virtual int64_t OperationFactory::workingSetBytesPerTile() const;`
  - `int64_t OperationCatalog::workingSetBytesPerTile(const char* name) const;`
  - the W3 formula in `RunPlan::tileBytes`.

- [ ] **Step 1: Write the failing tests** (`// Requirements: UR-EXE-2, UR-EXT-3, UR-EXT-4`):
  - `WorkingSet.DefaultsAreZero`: a test operation without overrides declares 0 and 0, and `tileBytes` is unchanged from the formula before this plan.
  - `WorkingSet.PerPixelScalesWithTheHaloedTile`: a chain of `test.boxMean` (radius 3) then `test.workingSet` (perPixel 10). `tileBytes` grows by exactly (64+6)² × 10 at tile 64.
  - `WorkingSet.LargestOperationNotTheSum`: two `test.workingSet` steps (10 and 30 per pixel) add only the 30 term.
  - `WorkingSet.PerTileAdds`: perTile 1,000,000 adds exactly 1,000,000.
  - `WorkingSet.RefusedOverTheCeiling`: a ceiling that fits the buffers but not the working set fails `validate()` with `InvalidArgumentError` naming `test.workingSet`.
  - `WorkingSet.WholeLevelAddsPerTile`: a whole-level chain adds the max `perTile` to its need.
  - `WorkingSet.LazyAndEagerReserveTheSame`: under a tight budget, a lazy read and an eager run both reserve `tileBytes`. Check through `MemoryBudget::reservedBytes()` at a probe, as the existing executor tests do.
  - `WorkingSet.CatalogAnswers`: `OperationCatalog::workingSetBytesPerTile("test.workingSet")` returns the default-parameter value.
- [ ] **Step 2: Run them to verify they fail.**
- [ ] **Step 3: Implement** W1 to W3. The doc comments state W1 and W2, and the existing `workingSetBytesPerLevelPixel()` docs lose "whole-level only".
- [ ] **Step 4: Run.** Run `./run-tests.sh`. Expected: everything passes. Existing `tileBytes` expectations elsewhere change only where a test operation now declares something.
- [ ] **Step 5: Commit.** `feat(pipeline): operations declare their working set and the tile reservation counts it`

---

### Task 2: The allocation meter

**Files:**
- Create: `tests/algorithm/support/allocation_meter.hpp/.cpp`, `tests/algorithm/test_allocation_meter.cpp`.
- Modify: `tests/algorithm/CMakeLists.txt`.

**Interfaces:**
- Produces:
  - `class AllocationMeter { public: AllocationMeter(); ~AllocationMeter(); int64_t peakBytes() const; };`. A meter is RAII and thread-local; nesting is not allowed and is asserted.
  - `int64_t testing_support::measureApplyPeak(Operation&, <harness arguments of applyDirect>)`. It builds the context outside the meter and measures only `apply()`.

- [ ] **Step 1: Write the failing tests** (`// Requirements: UR-EXE-2`):
  - `AllocationMeter.CountsOnlyItsThreadAndScope`: a 1 MB vector inside the scope reads at least 1 MB; the same allocation outside reads 0; one on another thread reads 0.
  - `AllocationMeter.PeakNotTotal`: allocating and freeing 1 MB ten times reads about 1 MB, not 10 MB.
  - `AllocationMeter.SizedAndAlignedDeletes`: aligned `new`/`delete` (C++17 overloads) are counted and balanced.
- [ ] **Step 2: Run them to verify they fail.**
- [ ] **Step 3: Implement.** Replace all the global `operator new`/`delete` forms (plain, nothrow, aligned, sized). The size is kept in a header before the block, so `delete` knows what to subtract. The replacement is used only by this test executable.
- [ ] **Step 4: Run.** Run `./run-tests.sh -R "algorithm\."`. Expected: all pass, the existing algorithm tests included.
- [ ] **Step 5: Commit.** `test(algorithm): an allocation meter for operation working sets`

---

### Task 3: Declarations for the point operations

**Files:**
- Modify: `src/algorithm/pixel/{threshold_fixed,convert_type,colour_convert,stain_deconvolution,threshold_otsu,threshold_adaptive}.cpp`.
- Create: `tests/algorithm/test_working_set_pixel.cpp`.

**Interfaces:**
- Consumes: Task 1's declarations and Task 2's `measureApplyPeak`.

- [ ] **Step 1: Write the failing test** (`// Requirements: UR-EXE-2, UR-ALG-8`):
  - `WorkingSetPixel.DeclarationsBoundTheMeasuredPeak`: for each of the six operations, at default and upper-limit parameters, on every supported type and tile sizes 64 and 257, the measured `apply()` peak is at most `haloArea × perPixel + perTile`. Otsu is measured in `accumulate()` as well, at `binCount` 1,048,576 with a range.
  - `WorkingSetPixel.OtsuRefusedWhenItsBinsDoNotFit`: `validate()` refuses under a ceiling the buffers fit but the histograms do not.
- [ ] **Step 2: Run it to verify it fails** wherever an operation allocates more than its default 0 declaration.
- [ ] **Step 3: Declare.** Each operation returns its measured need, rounded up to a stated formula. One comment per declaration names what it covers, for example "the plane as doubles, 8 bytes per halo'd pixel". Take no margin beyond the formula.
- [ ] **Step 4: Run.** Run `./run-tests.sh -R "algorithm\."`. Expected: all pass.
- [ ] **Step 5: Commit.** `feat(algorithm): point operations declare their working set`

---

### Task 4: Declarations for the filters and morphology

**Files:**
- Modify: `src/algorithm/pixel/{gaussian_blur,derivatives,median_blur,morphology,bilateral_filter,canny}.cpp`, and `src/algorithm/detail/separable.*` (a helper stating the engine's working set, for its users).
- Create: `tests/algorithm/test_working_set_filter.cpp`.

- [ ] **Step 1: Write the failing tests** (`// Requirements: UR-EXE-2, UR-ALG-8`):
  - `WorkingSetFilter.DeclarationsBoundTheMeasuredPeak`: as in Task 3, for all eleven operations. Parameters cover the defaults and the upper limits: Gaussian 1025, median 255 on `UInt16` (own path), bilateral 255, morphology 255×255 at 1 iteration. Tiles are 64 and 257. For OpenCV paths (median on `Byte`, integer morphology, Canny), the cited OpenCV share is subtracted before the comparison (W4).
  - `WorkingSetFilter.BilateralLargeTileRefusedUnderTheCeiling`: tile 4096, diameter 255, a 512 MB ceiling: `validate()` refuses, naming `pixel.bilateral_filter`.
- [ ] **Step 2: Run them to verify they fail.**
- [ ] **Step 3: Declare.** Use per-pixel figures over the halo'd area and per-call figures for windows, kernels and rings, each with its one-line comment. The OpenCV shares are bounds cited from OpenCV 4.14:
  - `cv::Canny`: `dx`/`dy` 16S, the magnitude ring and the map;
  - `cv::medianBlur` 8U: the histogram path;
  - `cv::erode`/`dilate`/`morphologyEx`: the row buffers and the destination.

  Each cites file and function.
- [ ] **Step 4: Run.** Run `./run-tests.sh -R "algorithm\."`. Expected: all pass.
- [ ] **Step 5: Commit.** `feat(algorithm): filters and morphology declare their working set`

---

### Task 5: SDS fold

**Files:**
- Modify:
  - `docs/superpowers/specs/sds/05-pipeline.md`:
    - §5.1, §5.3: W1 and W2;
    - §5.5 whole-level chains: add the per-tile term;
    - §5.6 or wherever tile memory is stated: the W3 formula.
  - `docs/superpowers/specs/sds/06-algorithm.md` §6.2: every operation declares its working set; the parameter tables need no change.
  - `docs/superpowers/specs/sds/08-class-diagrams.md`: the new virtual.
  - `docs/traceability.md`: UR-EXE-2 rows.
  - `docs/project.md`: this plan's entry, "Implemented on branch `feat/working-set`".

- [ ] **Step 1: Write the edits.** No ruling numbers or plan vocabulary.
- [ ] **Step 2: Verify.** Run `cmake --build build --target docs --config Release`. Expected: exit 0, 0 warnings. Then run `./run-tests.sh`. Expected: all pass.
- [ ] **Step 3: Commit.** `docs: operations' working set in the design`
