# Workspace Implementation Plan B

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `StageRunner` (§4.4). It runs one declared stage of a workspace to completion. Before running, it gates on dependencies and checks the stage's layer bindings. During the run, it wires the stage's restriction, rasters and writers into a `Pipeline`. Afterwards it flushes in the stated order and records the result. On a re-run it replaces the stage's earlier output, and on failure or cancellation it discards the stage's output.

**Architecture:** A new exported, pimpl'd class in the existing `slideio-processor-workspace` OBJECT library. It uses only the public APIs of `pipeline` (`Pipeline`, `OperationCatalog`, `LevelSelector`), `storage` (the writers, the discards, `StageStateWriter`, `LayerRegistry`) and plan A (`Workspace`, `StageRegistry`, `StageDefinition`). This is plan B of three. Plan A (merged in 48a2955) built the workspace, the definitions and the registry. Plan C builds `StalenessGraph` (§4.5) and `declareCoarseToFine`.

**Tech Stack:** C++17, CMake, SlideIO, SQLite via `storage`, libtiff (tests only), GoogleTest, MSVC.

**Spec:** `docs/superpowers/specs/sds/04-workspace.md` §4.4 (with §4.3 for gating and §4.7 for the typical sequence). Requirements: UR-STG-3, -5, -6, -7, -10, UR-EXE-1, -4, -5 in `docs/requirements.md`. Conventions: `00-conventions.md` (C-1 to C-13).

## Global Constraints

- Build `./build.sh --skip-slideio`; tests `./run-tests.sh` (all must pass); one test `ctest --test-dir build -C Release -R <name>` (test exes cannot run directly: DLL path). Docs `cmake --build build --target docs --config Release`.
- Zero compiler warnings and zero Doxygen warnings.
- Public headers under `include/slideio/processor/workspace/`, namespace `slideio::processor`, classes exported with `SLIDEIO_PROCESSOR_ENGINE_EXPORTS`, every non-trivial class pimpl'd (C-2). MSVC dllexport exports every declared member, so never declare a member without defining it. Strings in are `const char*` UTF-8 and copied; strings out are owned by the object (C-4).
- Each public member is documented in the header with `@par Thread-safety` and `@par Requirements` as the SDS states them (C-7, C-12).
- Every source and test file starts with its `// Requirements: ...` line; `docs/traceability.md` `## workspace` lists every test, one row per requirement, updated in place.
- Error and log messages name the stage, operation, layer, raster or path involved, never pixel, sample, coordinate or parameter values. A tile appears only as the `ErrorContext` tile index.
- `workspace` depends on `core`, `config`, `storage`, `pipeline` only; it never links `algorithm`. Tests may use `algorithm` operations through `OperationCatalog` by name, since they are in the engine DLL.
- Slide paths can embed patient identifiers (SDS 04 §4.1): the runner logs and throws with stage names, never the slide path.
- Match the surrounding code's comment density, naming and idiom. No Python on the machine: edit with the Edit tool or sed.
- Conventional commit messages; one commit per task at least.

## Decisions this plan makes (amend SDS §4.4 in the task that implements each)

1. **Gating.** `run()` first throws `IoError` when the workspace is read-only. It then throws `StageError` naming the stage in three cases:
   - the stage's record is unreadable (`StageRegistry::isUnreadable`, added in Task 1);
   - a dependency is not `Complete`, or is stale (the message names the first such dependency, in `dependencyAt` order);
   - another `StageRunner` of the same workspace is inside `run()`.
   A stale stage whose dependencies are current is runnable: re-running it is how it becomes current.
2. **`isRerun()`** is true when the stage's status at the start of `run()` is not `Pending`. That covers `Complete`, `Failed`, and `Running`; `Running` is left behind by a process that died mid-run, because the workspace's single writer lock means no live process can own it. Before `run()` it answers for the status at construction.
3. **Re-run order.**
   1. Discard the stage's prior output with `discardStage`, `discardEdgesOfStage`, `discardAnnotationsOfStage` and `discardRasterOfStage`.
   2. Call `StageRegistry::markStale` on every stage that depends directly on this one, because its inputs are gone or about to be replaced whatever this run's outcome.
   3. Only then set the stage `Running`.
4. **Success** records the following, in this order after the flushes:
   1. the analysis level of each written layer;
   2. `setStatus(Complete)`;
   3. `StageRegistry::clearStale(id)`.
5. **Failure.** The following exceptions are raised from validation, the tile run or the flushes:
   - `CancelledError` is re-thrown as `CancelledError` carrying the stage name.
   - `std::bad_alloc` is re-thrown unchanged.
   - Every other `ProcessorError` or `std::exception` becomes a `StageError` with `stageName` = the stage, `cause` = the original `what()`, and the original's operation name and tile copied when it has them.

   Before any of these leaves `run()`, the runner does the following, best effort:
   1. abandons the raster writer;
   2. runs the four discards;
   3. sets the stage `Failed`.

   If any of these steps itself throws, the runner logs an Error naming the stage and the original exception still propagates. The analysis level is never recorded on failure.
6. **Layer binding checks** run before `Pipeline::validate()` and throw `StageError` naming the stage, the operation and the layer. They cover every operation for which `OperationCatalog::instance().producesObjects(name)` is true:
   - its `targetLayer`, when present and non-empty, must name one of the stage's written layers;
   - when absent or empty, the stage must declare exactly one written layer.
   Raster-only operations are not consulted.
7. **Annotation-target checks (§4.4) are deferred.** No operation in the catalog names an annotation target layer today, and `storage` refuses an undeclared layer at emit (§4.4: "the invariant itself lives in storage"). §4.4 gains a note saying the runner check arrives with the first operation that takes such a parameter.
8. **Counters.** `objectsWritten()` and `edgesAsserted()` are the `emittedCount()` and `assertedCount()` of this run's writers, taken after the flushes. Both are 0 before `run()` and after a failed run.
9. **Resumable unit (UR-STG-7).** The runner keeps the tile as its unit of work by passing everything through `Pipeline::runInto`. No per-tile state is written in v1. §4.4 already says this; the task adds no schema.

## Review Focus

1. **A process killed mid-run.** The stage is left `Running`. The next `run()` must treat it as a re-run: it discards whatever the dead run flushed and finishes with the same object count as a clean run. Task 4 `StageRunner.RunningLeftByDeadProcessIsRerun`.
2. **Running the same stage twice** yields the same object count, the same identifiers and the same edge count, not twice as many (UR-STG-6 acceptance). Task 4 `StageRunner.RerunReplacesNotDuplicates`.
3. **A tile fault** gives a `StageError` naming the stage and the tile. The stage ends `Failed`, no object, edge, annotation or raster of it is visible, and the preceding stage is intact (UR-STG-5 acceptance). Task 4 `StageRunner.TileFaultFailsStageAndLeavesPriorStateIntact`.
4. **A stage restricted to a layer written at a coarse level** produces no object outside the dilated mask, and misses none inside the undilated region (§4.4's one-sided property). Task 2 `StageRunner.RestrictionToCoarseLayerIsOneSided`.
5. **Re-running a dependency** makes its dependent stale, and the dependent is refused (`StageError` naming the dependency) until the dependency completes again. After that the dependent runs and becomes current. Task 4 `StageRunner.RerunOfDependencyStalesAndThenReleasesDependent`.

---

### Task 1: `StageRunner` — gating and the success path

**Files:**
- Create: `include/slideio/processor/workspace/stage_runner.hpp`, `src/workspace/stage_runner.cpp`, `src/workspace/detail/run_slot.hpp`, `tests/workspace/test_stage_runner.cpp`
- Modify: `include/slideio/processor/workspace/stage_registry.hpp` and `src/workspace/stage_registry.cpp` (`isUnreadable`), `src/workspace/workspace.cpp` (the run slot), `src/workspace/CMakeLists.txt`, `tests/workspace/CMakeLists.txt`, `tests/workspace/support/test_slide.{hpp,cpp}`

**Interfaces:**
- Consumes:
  - From plan A: `Workspace` (`store()`, `scene()`, `scratchConfiguration()`, `memoryBudget()`, `layers()`, `stages()`, `isReadOnly()`), `StageRegistry` (`definition`, `status`, `isRunnable`, `dependencyCount`, `dependencyAt`, `isStale`, `name`, `clearStale`) and `StageDefinition` accessors.
  - From `storage`: `ObjectStore::writerForStage`, `EdgeStore::writerForStage`, `AnnotationStore::writerForStage`, `StageStateWriter::setStatus`.
  - From `pipeline`: `Pipeline` (`memoryBudget`, `edgeSink`, `annotationSink`, `stageName`, `bindLayer`, `validate`, `runInto`, `analysisLevel`).
- Produces:
  - `StageRunner` as §4.4 declares it: `StageRunner(Workspace&, StageId)`, `~StageRunner()`, `progress(ProgressObserver*)`, `cancellation(const CancellationToken&)`, `run()`, `isRerun()`, `objectsWritten()`, `edgesAsserted()`. It is non-copyable. The constructor throws `InvalidArgumentError` for a stage the registry does not know.
  - `bool StageRegistry::isUnreadable(StageId) const;`: true for a stage kept by name only at restore (plan A). Unknown stage → `InvalidArgumentError`. It refreshes in a read-only registry the same way the other graph-backed accessors do.
  - In `src/workspace/detail/run_slot.hpp` (namespace `slideio::processor::detail`), a RAII `class RunSlot { public: explicit RunSlot(Workspace&); ~RunSlot(); }`. It throws `StageError` naming the stage passed to it when the workspace's slot is taken. The flag lives in `Workspace::Impl` as a `std::atomic<bool>`, reached through a detail accessor declared in `run_slot.hpp`.
- Test support: `std::filesystem::path writeBlobSlide(const std::filesystem::path& file, int32_t width, int32_t height);`. It writes the same TIFF form as `writeTestSlide`, white with nine dark discs of radius 20 on a 3×3 grid (centres at 1/6, 3/6 and 5/6 of each side). A chain of `pixel.colour_convert` to grey, `pixel.threshold_fixed` (inverted) and `segment.connected_components` then finds exactly 9 objects. Check the operations' parameter names in their `src/algorithm/` sources.

- [ ] **Step 1: Write the failing tests** (a 600×600 blob slide; layers declared through `layerLifecycle()`):
  - `StageRunner.FirstRunWritesObjectsAndCompletes`: the stage writes `nuclei`. After `run()`:
    - `status == Complete` and `completedAtUnixMs > 0`;
    - `isRerun() == false`;
    - `objectsWritten() == 9`;
    - an `ObjectStore` query of `nuclei` returns 9;
    - `isStale == false`.
  - `StageRunner.AnalysisLevelRecordedOnSuccess`: `layers().coarsestAnalysisLevel(nuclei) == p.analysisLevel()` (level 0 for this slide).
  - `StageRunner.ProgressNamesTheStage`: a recording `ProgressObserver` sees at least one call, and every `stageName` equals the stage's name.
  - `StageRunner.BlockedByIncompleteDependency`: background writes `tissue`, and nuclei restricted to `tissue` is run first. It throws `StageError`; the message contains `"nuclei"` and `"background"`; nuclei stays `Pending` with no objects.
  - `StageRunner.BlockedByStaleDependency`: run background, `markStale(background)`, then run nuclei: `StageError` naming background.
  - `StageRunner.UnreadableStageIsRefused`: corrupt the record the way plan A's `UnreadableRecordIsKeptByNameAndNeverRunnable` does, reopen, run: `StageError` naming the stage.
  - `StageRunner.ReadOnlyWorkspaceCannotRun`: `IoError`, and the writer's status is unchanged.
  - `StageRunner.OneRunnerPerWorkspace`: a progress observer on the first runner calls a second runner's `run()` from inside its first callback. That inner call throws `StageError` naming the second stage, and the outer run completes.
  - `StageRunner.UnknownStageIsInvalidArgument`.
  - `StageRegistry.IsUnreadableReportsRestoredNameOnlyStages`: in `test_stage_registry.cpp`.
- [ ] **Step 2: Run;** expect compile failure.
- [ ] **Step 3: Implement.**
  - The success path, in the order §4.4's listing gives, without the restriction and the rasters, which Tasks 2 and 3 add.
  - Flushes: objects, then edges, then annotations.
  - Then decision 4's success recording, and decision 8's counters.
  - Failure handling is Task 4. For now an exception propagates after `setStatus(Failed)`.
- [ ] **Step 4: Run** focused (`-R "StageRunner|StageRegistry"`), then full.
- [ ] **Step 5: SDS §4.4** (decisions 1, 4 and 8, and `isUnreadable` in §4.3), traceability (UR-STG-10, UR-EXE-1, UR-EXE-4); **commit** `feat(workspace): stage runner gating and success path`.

### Task 2: Layer binding and the restriction

**Files:**
- Modify: `src/workspace/stage_runner.cpp`, `tests/workspace/test_stage_runner.cpp`, `tests/workspace/support/test_slide.{hpp,cpp}`

**Interfaces:**
- Consumes: Task 1; `OperationCatalog::instance().producesObjects(const char*)`; `PipelineSpec::operationCount()`, `operationName(i)`, `operationParameters(i).getString("targetLayer", "")`; `makeLayerMaskSource(ObjectSource&, LayerId, double)`; `LevelSelector(scene).scaleOfLevel(level)`; `LayerRegistry::coarsestAnalysisLevel`; `Pipeline::maskSource`.
- Produces: decision 6's checks, and the mask from §4.4's listing:
  - `halfStep = LevelSelector(scene).scaleOfLevel(coarsestAnalysisLevel(src)) / 2.0`;
  - `makeLayerMaskSource(store->objects(), src, halfStep)`;
  - the mask source outlives the run.

- [ ] **Step 1: Write the failing tests:**
  - `StageRunner.TargetLayerNotDeclaredIsStageError`: the stage writes `nuclei`, and its connected-components operation has `targetLayer` `"cells"`. The `StageError` message contains the stage, `segment.connected_components` and `cells`. No objects; the status is `Failed`; `validate()` is never reached, so the message is the runner's, not the pipeline's.
  - `StageRunner.MissingTargetWithTwoLayersIsStageError`: the stage writes `cells` and `nuclei`, and the operation has no `targetLayer`.
  - `StageRunner.MissingTargetWithNoLayerIsStageError`.
  - `StageRunner.TwoLayersEachNamedRun`: two connected-components operations, each naming its layer, as in §4.7. Both layers get 9 objects. Thresholds are as in Task 1; the second runs on a copy if the chain needs it.
  - `StageRunner.RasterOnlyStageMayDeclareNoLayer`: a chain of `pixel.gaussian_blur` only, no layer. It completes with 0 objects.
  - `StageRunner.RestrictionKeepsObjectsInsideTheLayer`: background is `segment.tissue_detect`, or a fixed-threshold connected-components that covers the left half. That needs a new helper, `writeHalfBlobSlide`: discs only on the left half, plus a grey rectangle covering the left half. Nuclei restricted to that layer get only the left-half discs.
  - `StageRunner.RestrictionToCoarseLayerIsOneSided` (Review Focus 4):
    - run background at a coarse level (a level-2 analysis via the operation's level parameter, or a pyramid slide from `writeTestSlide` with levels if the helper supports it; otherwise add a `levels` argument);
    - run nuclei at level 0;
    - assert that no nuclei object's bounds lie outside the background objects' bounds dilated by `halfStep`, and that every disc whose centre is inside the undilated background is found.
- [ ] **Step 2: Run;** expect FAIL. **Step 3: Implement.** **Step 4: Run** focused, then full.
- [ ] **Step 5: SDS §4.4** (decision 6 note), traceability (UR-STG-3, UR-STG-10); **commit** `feat(workspace): stage runner layer binding and restriction`.

### Task 3: Rasters — written, read and layer-backed

**Files:**
- Modify: `src/workspace/stage_runner.cpp`, `tests/workspace/test_stage_runner.cpp`

**Interfaces:**
- Consumes:
  - `RasterRegistry` (`declare`, `writerForStage(id, raster)`, `sourceFor(raster)`, the published state);
  - `makeLayerRasterSource(ObjectSource&, LayerId, LayerRasterContent)`;
  - `Pipeline::rasterSink`, `Pipeline::auxiliaryRaster(name, source)`;
  - `StageDefinition` raster accessors.
- Produces: §4.4's raster wiring.
  - Layer-backed and stored raster sources are bound under their binding names.
  - The written raster's writer is flushed last, after annotations.
  - The sources live until `runInto` returns.

- [ ] **Step 1: Write the failing tests:**
  - `StageRunner.WrittenRasterIsPublishedOnSuccess`: a stage `writesRaster(prob)` with a `pixel.gaussian_blur` chain. After `run()` the raster is published; a `sourceFor(prob)` read of one tile succeeds.
  - `StageRunner.StoredRasterIsReadByBinding`: a second stage `readsRaster("prob", prob)`. It runs a chain with a test operation, `"test.ws.auxProbe"`, that declares the auxiliary input `"prob"` and records that it read a tile from it.
    - Define the operation in a new `tests/workspace/support/aux_probe.{hpp,cpp}`, in the way `tests/pipeline/test_bindings.cpp` defines its `ProbeOp` with an auxiliary input.
    - Register it through `tests/pipeline/test_support/test_operations.hpp`'s `PrototypeFactory`, and compile `test_operations.cpp` into the workspace tests.
    - The run completes, the probe saw the binding, and the second stage depends on the first.
  - `StageRunner.LayerAsRasterIsBound`: `readsLayerAsRaster("coarse", nuclei, LayerRasterContent::Membership)` with `"test.ws.auxProbe"` declaring `"coarse"`. The probe saw the binding, and the run completes.
  - `StageRunner.FlushOrderObjectsEdgesAnnotationsRaster`: the event journal after a run that writes objects and a raster shows `ObjectsAdded` before `RasterWritten`.
- [ ] **Step 2: Run;** expect FAIL. **Step 3: Implement.** **Step 4: Run** focused, then full.
- [ ] **Step 5: Traceability; commit** `feat(workspace): stage runner rasters`.

### Task 4: Failure, cancellation and re-run

**Files:**
- Modify: `src/workspace/stage_runner.cpp`, `tests/workspace/test_stage_runner.cpp`, `tests/workspace/CMakeLists.txt`
- Modify: SDS 04 §4.4 (decisions 2, 3, 5, 7 and 9), `docs/superpowers/specs/sds/08-class-diagrams.md` (`StageRunner`), `docs/project.md` (a "Workspace implementation plan B" entry with status), traceability

**Interfaces:**
- Consumes: Tasks 1–3; `ObjectStore::discardStage`, `EdgeStore::discardEdgesOfStage`, `AnnotationStore::discardAnnotationsOfStage`, `RasterRegistry::discardRasterOfStage`; `StageRegistry::markStale`; the event journal (`StageDiscarded`); `ErrorContext`.
  - Tests compile `tests/pipeline/test_support/executor_support.cpp` into the workspace test executable to use `"test.probe"` and its `failX`, `failY` and `failKind` parameters.
- Produces: decisions 2, 3 and 5.

- [ ] **Step 1: Write the failing tests:**
  - `StageRunner.TileFaultFailsStageAndLeavesPriorStateIntact` (Review Focus 3):
    - background completes;
    - nuclei's chain includes `test.probe` (`copy` 1) failing at a pixel inside the second tile row, with tile size from the configuration.
    - It throws `StageError`, with `stageName() == "nuclei"`, `hasTile()`, and the cause holding `"deliberate failure"`.
    - Afterwards: nuclei is `Failed`; a `nuclei` query returns 0; edges and annotations of nuclei are 0; a `StageDiscarded` event for nuclei exists; background's objects and status are unchanged; `objectsWritten() == 0`; the analysis level is not recorded.
  - `StageRunner.IoErrorBecomesStageError`: `failKind` 1 gives a `StageError` with the cause.
  - `StageRunner.CancellationFailsStageAndRethrowsCancelled`: cancel from the progress observer after the first tile. It throws `CancelledError`, `stageName()` is `"nuclei"`, the status is `Failed`, and there are 0 objects.
  - `StageRunner.RerunReplacesNotDuplicates` (Review Focus 2): run twice. The second run has `isRerun() == true`. Count, the sorted object identifiers and the edge count all equal those of the first run.
  - `StageRunner.FailedStageReruns`: fail once with the probe armed, then disarm and run again. `isRerun() == true`, `Complete`, 9 objects.
  - `StageRunner.RunningLeftByDeadProcessIsRerun` (Review Focus 1):
    - emulate the dead process: `StageStateWriter::setStatus(id, Running)` plus objects flushed through `objects().writerForStage(id)`, then close and reopen;
    - `run()` has `isRerun() == true` and ends with exactly 9 objects.
  - `StageRunner.RerunOfDependencyStalesAndThenReleasesDependent` (Review Focus 5):
    1. Run background, then nuclei.
    2. Re-run background: nuclei is stale.
    3. `markStale(background)`, then run nuclei: `StageError` naming background.
    4. Run background again, then nuclei: it completes, and `isStale(nuclei) == false`.
  - `StageRunner.RerunOfCellsLeavesAttachedNucleiUntouched` (UR-STG-6 acceptance):
    - nuclei run first;
    - a cells stage reads nuclei and writes `cells`, with the blob chain. No catalog operation asserts parent edges, so after its first `run()` the test asserts one parent edge per nucleus through `store()->edges().writerForStage(cellsId)` and flushes;
    - after a second `run()` of cells: nuclei objects are unchanged (count and identifiers), the cells stage's edges are 0 (the runner's discard removed them), and there are 9 cells.
- [ ] **Step 2: Run;** expect FAIL. **Step 3: Implement** decisions 2, 3 and 5. **Step 4: Run** focused, then full.
- [ ] **Step 5: Docs** (SDS §4.4, SDS 08, project.md, traceability for UR-STG-5, -6, -7, UR-EXE-4, -5), docs build; **commit** `feat(workspace): stage runner failure, cancellation and re-run`.
