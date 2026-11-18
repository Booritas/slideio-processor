# Accumulation State and Sequential OpenCV Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the two gaps the operation working-set plan left open, as the user decided on 2026-11-18 before algorithm plan C:
- **A.** A global-context run counts the state its operation instances keep between calls, and the ordered fold can no longer run ahead without limit.
- **B.** The engine runs its private OpenCV single-threaded, so the tile workers are the only parallelism and the OpenCV declarations no longer grow with the host's cores.

**Architecture:**
- **A, the declaration.** A new virtual `Operation::accumulatedStateBytes(int32_t planeCount)` bounds what one instance holds between calls, after any number of `accumulate()` and `combine()` calls. `buildRunPlan` records it for every global-context operation. A run whose tile, run-instance state and one tile instance's state cannot fit the ceiling together is refused in `validate()`.
- **A, the executor.** It reserves the run instances' state for the whole run. Each accumulation pass then admits at most `cap` live tile instances, counting both those in flight and those the fold holds. `cap` is `2 × workers`, lowered so that `cap` instances' state fits beside one tile. The pass reserves `cap × state` up front. Slots are taken **before** a tile index is handed out, so the lowest tile not yet combined always belongs to a running worker, and the fold cannot deadlock.
- **B.** `detail::useSequentialOpenCv()` calls `cv::setNumThreads(0)` once, from the constructor of every operation that runs an OpenCV function. Canny and morphology then lose their per-thread terms and re-derive their bounds for OpenCV's sequential path.

**Tech Stack:** C++17, CMake, Conan 2, GoogleTest, OpenCV 4.14 (static, private to the engine).

**Spec:** SDS 05:
- §5.1 (`Operation` declarations) and §5.3 (`OperationCatalog`);
- §5.5 (`validate()`) and §5.6 (tile memory, including the "What the working-set figures do not cover" paragraph this plan retires);
- §5.8 (`OrderedFold`, global-context passes) and §5.9 (executor).

Also SDS 06 for the algorithm operations, and requirement UR-EXE-2 (memory budget). The design decisions are recorded in the plan `2026-11-18-operation-working-set.md` and its final review.

## Global Constraints

- The pipeline's and algorithm plans' Global Constraints hold:
  - C++17, `slideio::processor` and `detail` namespaces, the export macros;
  - no OpenCV in public headers;
  - Doxygen with `@throws` / `@par Thread-safety` / `@par Requirements`, and a 0-warning `docs` target;
  - `// Requirements:` lines;
  - no sample, pixel or parameter values in messages (byte counts and ceilings may appear, as the existing memory messages do), and no plan vocabulary in code or public docs;
  - `./build.sh --skip-slideio` with 0 warnings, and `./run-tests.sh` green.
- Branch `feat/working-set` (PR #14). One commit per task, with prefixes `feat(pipeline):`, `feat(algorithm):`, `docs:`. AI-generated code is human-reviewed before it reaches `main`.
- **ABI:** the new virtuals go **after** the existing ones on `Operation` and `OperationFactory`. This is acceptable before the first release.
- Metered tests live in `slideio_processor_working_set_tests` (prefix `algorithm.ws.`). The main test executables cannot see the engine DLL's allocations, or its OpenCV, on Windows.

## Rulings on spec gaps

| # | Gap | Ruling |
|---|---|---|
| S1 | What `accumulatedStateBytes` covers. | It bounds the bytes one instance holds between calls: after `accumulate()` on any number of tiles with `planeCount` planes, and after `combine()` with any number of instances of the same parameters. It excludes what the working-set figures already cover during a call. The default is 0 and a negative figure counts as 0. `OperationFactory` mirrors it for default parameters, and `OperationCatalog::accumulatedStateBytes(const char* name, int32_t planeCount)` answers it; the instance is authoritative. |
| S2 | Which plane count is passed. | The plane count of the operation's input step in the plan, `RunPlan::planeTypes[g].size()` for the operation at position g. |
| S3 | What `validate()` refuses. | It refuses when `tileBytes + Σ_g state_g + max_g state_g > ceiling` (saturating), where `state_g` is the declaration for global-context operation g. The `InvalidArgumentError` names that operation with the largest state, the bytes and the ceiling. With no global-context operation, the term is 0. |
| S4 | The bound on held instances. | For pass g: `available = ceiling − tileBytes − Σ state`, `cap_g = state_g == 0 ? 2 × workers : min(2 × workers, available / state_g)`. S3 guarantees `cap_g ≥ 1`. The pass reserves `cap_g × state_g` (reserveOrWait) before handing out tiles and releases it when the pass ends. The run reserves `Σ state` (reserveOrWait) before the first pass and releases it when `run()` returns, on every path. |
| S5 | When a slot is released. | When the fold combines the tile's instance, or when the tile is skipped. On failure, slots are dropped with the instances. A worker waiting for a slot wakes and leaves on the pass's stop flag (a failure elsewhere) or on cancellation (`CancelledError`, as `reserveOrWait` reports it). |
| S6 | Where OpenCV is made sequential. | `detail::useSequentialOpenCv()` in `src/algorithm/detail/cv_plane.{hpp,cpp}` runs `cv::setNumThreads(0)` under `std::call_once`. It is called from the constructor of every operation whose `apply()` calls an OpenCV function, never from a static initialiser (OpenCV's own statics may not exist yet). The OpenCV build in use (no TBB, no OpenMP) keeps a process-wide thread count, and OpenCV is private to the engine, so nothing else can change it. |

## Review Focus

1. **A tile fails while other workers wait for a slot:** the run ends with that tile's `TileError` and does not hang. Pinned in Task 2.
2. **Cancellation while a worker waits for a slot:** the run throws `CancelledError` promptly. Pinned in Task 2.
3. **Every tile of an accumulation pass skipped by the restriction:** slots are released on skip, and the pass completes with one worker and with many. Pinned in Task 2.
4. **Two global-context operations in one chain:** the run reserves both states, and each pass bounds its own instances. Pinned in Task 1 (validation) and Task 2 (reserved bytes).
5. **A lazy read after an eager run on the same engine:** OpenCV stays single-threaded, and Canny's output is unchanged, bit for bit, from before this plan. Pinned in Task 4 by the existing Canny tiling-invariance and reference tests, which must pass unchanged.

---

### Task 1: The state declaration and its validation

**Files:**
- Modify:
  - `include/slideio/processor/pipeline/operation.hpp` and `operation_catalog.hpp`;
  - `src/pipeline/operation.cpp` and `operation_catalog.cpp`;
  - `src/pipeline/detail/run_plan.hpp` and `run_plan.cpp`.
- Test: `tests/pipeline/test_working_set.cpp` (and its test-support operation, where `WorkingSetOperation` lives).

**Interfaces:**
- Produces:
  - `virtual int64_t Operation::accumulatedStateBytes(int32_t planeCount) const;` (default 0);
  - `virtual int64_t OperationFactory::accumulatedStateBytes(int32_t planeCount) const;` (default 0);
  - `int64_t OperationCatalog::accumulatedStateBytes(const char* name, int32_t planeCount) const;`, which throws as `workingSetBytesPerTile(name)` does;
  - `RunPlan::accumulatedStateBytes`, a `std::vector<int64_t>` parallel to `globalContextPositions`, each entry clamped at 0.

- [ ] **Step 1: Write the failing tests** in `test_working_set.cpp`. They use a test operation, `needsGlobalContext()` true, whose state declaration is settable:
  - `WorkingSet.StateDefaultsToZero`: a default `Operation`, a factory and the catalog all return 0.
  - `WorkingSet.StateRecordedPerGlobalOperation`: in a chain [state 1000, plain, state 3000], `plan.accumulatedStateBytes == {1000, 3000}`, and the plane count passed is the step's plane count. The test operation records the argument; assert it equals the selected channel count.
  - `WorkingSet.NegativeStateCountsAsZero`: a declaration of −5 gives `{0}`.
  - `WorkingSet.StateRefusedOverTheCeiling`: set the ceiling to `tileBytes + 2 × S − 1`, where `tileBytes` is from the same chain validated with state 0. `validate()` throws `InvalidArgumentError` whose message contains the operation's name. With a ceiling of `tileBytes + 2 × S`, it validates.
  - `WorkingSet.TwoStatesSumAndMax`: states 1000 and 3000. A ceiling of `tileBytes + 4000 + 3000` passes, and one byte less is refused.
- [ ] **Step 2: Run them; they fail to compile** (no `accumulatedStateBytes`). Command: `./build.sh --skip-slideio`.
- [ ] **Step 3: Implement S1–S3.**
  - The `Operation` Doxygen states S1. It also says an instance's state beyond its declaration is not covered by the memory ceiling, and that a global-context operation whose state grows with the tiles it has seen cannot declare an honest bound and must keep its state bounded.
  - In `buildRunPlan`, the S3 check runs after the existing `tileBytes` check, with the same saturating helpers.
- [ ] **Step 4: Run** `./build.sh --skip-slideio && ./run-tests.sh`. Expected: 0 warnings, all tests pass.
- [ ] **Step 5: Commit:** `feat(pipeline): global-context operations declare their accumulated state`.

### Task 2: The executor reserves the state and bounds the fold

**Files:**
- Create: `src/pipeline/detail/instance_slots.hpp`.
- Modify: `src/pipeline/executor.cpp` (the `runPass` lambda and the accumulation-pass loop).
- Test: `tests/pipeline/test_global_context.cpp`.

**Interfaces:**
- Consumes: `RunPlan::accumulatedStateBytes` (Task 1).
- Produces: `detail::InstanceSlots`, a counting semaphore:
  - `explicit InstanceSlots(int64_t capacity)` (capacity ≥ 1, else `InvalidArgumentError`);
  - `bool acquire(const CancellationToken& token, const std::atomic<bool>& stop)` returns false when `stop` is set, and throws `CancelledError` when cancelled;
  - `void release() noexcept`;
  - `int64_t inUse() const`.

  Waiting is a condition-variable wait with a short timeout, so that it re-checks `stop` and the token. That is the same polling idea `MemoryBudget::reserveOrWait` uses for cancellation; reuse its interval.

- [ ] **Step 1: Write the failing tests** in `test_global_context.cpp`. They use a global-context test operation that declares state S and counts its live instances that have accumulated at least one tile (a static atomic counter plus a high-water mark):
  - `GlobalContext.FoldBacklogBounded`:
    - Setup: 4 workers, a 1×64 tile grid, ceiling large. Tile 0's `accumulate()` waits (at most 2 s) until the instance high-water mark stops rising for 100 ms, then returns.
    - Assert the high-water mark is `≤ 2 × 4 + 1` (the run's instance included), and the result equals a serial run's.
  - `GlobalContext.FoldBacklogFollowsTheCeiling`: with the ceiling at `tileBytes + S + 3 × S`, the high-water mark of tile instances is `≤ 3`. The run instance takes the first S, so `available / S == 3`.
  - `GlobalContext.StateReservedDuringTheRun`: a test operation reads `budget.reservedBytes()` inside `accumulate()` and inside `apply()`.
    - During the accumulation pass, it is at least `tileBytes + S + cap × S`.
    - During the final pass, it is at least `tileBytes + S`.
    - After `run()` returns, it is 0.
  - `GlobalContext.FailureWhileWaitingForASlot`: tile 0's `accumulate()` blocks until at least `cap` other tiles have submitted, then throws. `run()` throws `TileError` naming tile 0 within 5 s.
  - `GlobalContext.CancellationWhileWaitingForASlot`: same shape, but the token is cancelled instead. `run()` throws `CancelledError` within 5 s.
  - `GlobalContext.AllTilesSkippedReleaseSlots`: a restriction that admits no tile, with 1 worker and with 4 workers. `run()` completes, and `finalizeAccumulation()` sees an empty state.
- [ ] **Step 2: Run them; they fail.** The backlog test exceeds the bound, and the reservation test sees no state bytes.
- [ ] **Step 3: Implement S4 and S5.**
  - `runPass` gains an optional `InstanceSlots*`. When one is set, a worker calls `acquire` before `fetch_add` on `next`, and breaks out when it returns false.
  - The fold's combine lambda combines, destroys the tile instance (`reset()`), then releases its slot, so a released slot never overlaps a live instance. The skip path releases one immediately.
  - When a tile fails after acquiring, the slot is released on that path as well, so that no waiter is stranded.
  - Use an RAII holder in the style of `Reservation` for the run-level and pass-level byte reservations.
- [ ] **Step 4: Run** `./build.sh --skip-slideio && GTEST_REPEAT=20 ./run-tests.sh -R GlobalContext`, then the full `./run-tests.sh`. Expected: 0 warnings, and every run passes.
- [ ] **Step 5: Commit:** `feat(pipeline): accumulation passes reserve instance state and bound the fold`.

### Task 3: Otsu declares its state

**Files:**
- Modify: `src/algorithm/pixel/threshold_otsu.cpp`.
- Test: `tests/algorithm/test_working_set_pixel.cpp`.

**Interfaces:**
- Consumes: `Operation::accumulatedStateBytes(int32_t)` (Task 1).

- [ ] **Step 1: Write the failing test** `WorkingSetPixel.OtsuStateBoundsTheMeasuredState`.
  - For Byte at 4 planes, UInt16 at 3 planes, and Float32 at 2 planes with a range of 1,048,576 bins:
    - start an `AllocationMeter`;
    - create a run instance;
    - for 3 tiles, create a tile instance, `accumulate()` a 64×64 tile with all planes into it, and `combine` it into the run instance, then destroy the tile instance;
    - assert `meter.liveBytes() ≤ runInstance.accumulatedStateBytes(planes)`, measured while the run instance is alive.
  - Also assert that one tile instance's live bytes after its `accumulate()` are within the same declaration.
  - The run instance is created inside the meter, so its own object size counts; the declaration covers it.
- [ ] **Step 2: Run** `./run-tests.sh -R algorithm.ws.`. It fails, because the declaration is 0.
- [ ] **Step 3: Implement** `accumulatedStateBytes(planeCount)`: `planeCount × (bins × 8 + kBookkeepingBytes + detail::kAllocatorBlockOverhead)` plus the instance's own map and bookkeeping. `bins` is computed as in `workingSetBytesPerTile()`. A comment derives each term, matching the file's existing declaration comments.
- [ ] **Step 4: Run** `./run-tests.sh`. Expected: all tests pass.
- [ ] **Step 5: Docs.**
  - SDS 05 §5.1/§5.3: the declaration.
  - SDS 05 §5.6: replace the paragraph "What the working-set figures do not cover…" with the S3/S4 behaviour.
  - SDS 05 §5.8: replace "How many results are held at once is bounded by how far completion runs ahead…" with the slot bound.
  - SDS 05 §5.9: the run-level and pass-level reservations.
  - SDS 06: Otsu's state.
  - SDS 08: the class diagram.
  - `traceability.md`.
  - `cmake --build build --target docs --config Release` exits 0 with 0 warnings.
- [ ] **Step 6: Commit:** `feat(algorithm): Otsu declares its accumulated state` (code, tests and docs together).

### Task 4: The engine's OpenCV runs single-threaded

**Files:**
- Modify:
  - `src/algorithm/detail/cv_plane.hpp` and `cv_plane.cpp`;
  - `src/algorithm/pixel/canny.cpp` and `morphology.cpp`;
  - every other `src/algorithm/pixel/*.cpp` whose `apply()` calls an OpenCV function (today: `median_blur.cpp`; check `derivatives.cpp` and `structuring_element.cpp` and include them if they call one).
- Test: `tests/algorithm/test_working_set_filter.cpp`.

**Interfaces:**
- Produces: `void detail::useSequentialOpenCv();`, which is idempotent and thread-safe, and throws nothing an operation should let escape. A `cv::Exception` goes through `translateCv`.

- [ ] **Step 1: Write the failing tests** in `test_working_set_filter.cpp` (the ws executable runs the engine's OpenCV in-process):
  - `WorkingSetFilter.OpenCvIsSequentialOnceAnOperationExists`: after `OperationCatalog::instance().create("pixel.canny", …)`, `cv::getNumThreads() == 1`. The same holds after creating a morphology and a median operation.
  - `WorkingSetFilter.CannyDeclarationIsTight`:
    - at a 1024×1024 tile with the default parameters, take the declared bytes, `haloArea × perPixel + perTile`;
    - the declaration must be at most 3 × the measured `measureApplyPeak` plus the documented analytic OpenCV share for that tile;
    - the share is taken from the bound in `canny.cpp`, evaluated for one thread.
  - **If the sequential-path derivation cannot reach that, report DONE_WITH_CONCERNS with the derivation. Do not loosen the factor.**
- [ ] **Step 2: Run** `./run-tests.sh -R algorithm.ws.`. Both tests fail.
- [ ] **Step 3: Implement S6.**
  - Remove `threads_` and its `cv::getNumThreads()` calls from Canny and morphology.
  - Re-derive both operations' per-pixel and per-tile declarations from the OpenCV 4.14 source for the sequential path: with one thread, `cv::Canny` runs one stripe, and `TiledFilterInvoker`'s per-thread tile is not taken. Cite file and function in the comments, as today.
  - The existing `…DeclarationBoundsTheMeasuredPeak` tests must still pass unchanged.
- [ ] **Step 4: Run** `./build.sh --skip-slideio && ./run-tests.sh`. Expected: 0 warnings, and all tests pass, including every Canny and morphology tiling-invariance and reference test unchanged.
- [ ] **Step 5: Docs.**
  - SDS 06: a backend note that the engine runs its private OpenCV single-threaded, so the tile workers are the only parallelism; a lazy single-tile read is not parallelised inside OpenCV.
  - SDS 06: the revised Canny and morphology working sets.
  - `project.md`: extend the "Operation working set" entry to name this plan.
  - The docs target exits 0 with 0 warnings.
- [ ] **Step 6: Commit:** `feat(algorithm): the engine runs its OpenCV single-threaded`.
