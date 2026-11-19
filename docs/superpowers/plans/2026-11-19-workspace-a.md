# Workspace Implementation Plan A

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The `workspace` module's foundation: a workspace directory that binds one slide scene to a store, the layer lifecycle, stage definitions with their durable record, and the stage registry that derives dependencies, rejects cycles, tracks staleness along those dependencies and restores itself when a workspace is reopened.

**Architecture:** A new OBJECT library `slideio-processor-workspace`, linked into the engine DLL like `pipeline` and `algorithm`. Everything durable goes through `storage`: the store file and its raster directory live inside the workspace directory, the slide binding is the store's `Provenance`, and each stage's record is its `StageStateWriter` configuration, from which the registry rebuilds its definitions at `open`. This is plan A of three: plan B is `StageRunner` (§4.4), plan C is `StalenessGraph` with all five propagation paths and its reasons (§4.5) plus `declareCoarseToFine` (§4.2).

**Tech Stack:** C++17, CMake, Conan 2, SlideIO (GDAL driver for test slides), SQLite via `storage`, rapidjson, libtiff (tests only), GoogleTest, MSVC.

**Spec:** `docs/superpowers/specs/sds/04-workspace.md` §4.1, §4.2 (not the coarse-to-fine helper), §4.3, §4.6, §4.7; requirements UR-STG-1, -2, -4, -8, -9, -10, -12, UR-EXE-2, UR-OBS-1 in `docs/requirements.md`. Conventions in `00-conventions.md` (C-1 to C-13).

## Global Constraints

- Build `./build.sh --skip-slideio`; tests `./run-tests.sh` (all must pass); one test `ctest --test-dir build -C Release -R <name>` (test exes cannot run directly: DLL path). Docs `cmake --build build --target docs --config Release`.
- Zero compiler warnings and zero Doxygen warnings.
- Public headers under `include/slideio/processor/workspace/`, namespace `slideio::processor`, classes exported with `SLIDEIO_PROCESSOR_ENGINE_EXPORTS`, every non-trivial class pimpl'd (C-2); strings in are `const char*` UTF-8 and copied, strings out are owned by the object (C-4); collections are count plus indexed accessor (C-5).
- Each public member is documented in the header with `@par Thread-safety` and `@par Requirements` as the SDS states them (C-7, C-12).
- Every source and test file starts with its `// Requirements: ...` line; `docs/traceability.md` gains a `## workspace` section listing every test.
- Error messages name the stage, layer, raster or path involved, never pixel, sample, coordinate or parameter values.
- `workspace` depends on `core`, `config`, `storage`, `pipeline` only; it never links `algorithm` (§4.2: operations are resolved by name through `OperationCatalog`).
- Match the surrounding code's comment density, naming and idiom. No Python on the machine: edit with the Edit tool or sed.
- Conventional commit messages; one commit per task at least.

## Decisions this plan makes (amend the SDS in the task that implements each)

1. **Directory layout.** A workspace is a directory: the store at `<workspace>/workspace.store` (its sidecars and `workspace.store-rasters/` beside it, so inside the directory) and scratch at `<workspace>/scratch/`. `create` makes the directory; it throws `IoError` when the path exists and is not an empty directory.
2. **Slide binding** is the store's `Provenance`: `slideId` holds the slide path as given (UTF-8), `sceneIndex` the scene. `open` reopens the slide from it with `slideio::openSlide(path, "")`; a slide that no longer opens is an `IoError` naming the path.
3. **`StageDefinition::configurationJson` moves to the registry.** A definition holds `LayerId`s and `RasterId`s, which name nothing without a store, while the record (§4.2) must carry names. The record is produced by the registry at `declare` and read back with `StageRegistry::configuration(StageId)`, which already exists; `StageDefinition::configurationJson` is removed from §4.2.
4. **The record** (`stageRecordVersion` 1) carries every declaration by name, so a definition is rebuilt from it alone:
   `{"stageRecordVersion":1,"stage":"nuclei","readsLayers":["tissue"],"writesLayers":["nuclei"],"annotatesLayers":[],"restrictToLayer":"tissue","readsLayersAsRaster":[{"binding":"coarse","layer":"nests_coarse","content":"ObjectAnchor"}],"writesRaster":null,"readsRasters":[{"binding":"prob","raster":"probability"}],"dependsOn":[],"pipeline":{…PipelineSpec::toJson…}}`
   `readsLayers` lists only explicit `readsLayer` calls; reads implied by `annotatesLayer`, `restrictToLayer` and `readsLayersAsRaster` are derived again on load. Derived dependencies are not in the record (§4.2).
5. **Memory budget.** `Workspace` makes one budget, `makeMemoryBudget(configuration.memoryCeilingBytes())`, for every pipeline it runs. `storage` takes no budget today, so the SDS sentence about handing it to storage is amended to say so.
6. **Staleness in plan A** propagates along the effective dependencies only — the `LayerRead`, `RasterRead` and explicit `dependsOn` edges. Plan C adds the layer-ordering and derived-from paths and `StalenessGraph`; `StageRegistry::staleness()` arrives with it. The §4.3 text gains a note saying so until then.
7. **Persisted dependencies.** On every `declare` the registry rewrites every stage's dependency rows (`clearDependencies` + `addDependency`) and records the declaring stage's written layers (`addWrittenLayer`), so `StageStateReader` and `LayerRegistry::writerStageAt` see the derived graph without linking this module.

## Review Focus

1. A workspace path or slide path with non-ASCII characters works on Windows (paths built with `std::filesystem::u8path`) — Task 1 test `Workspace.NonAsciiPathsRoundTrip`.
2. A second `open` while a writer holds the workspace fails with `IoError`, while `openReadOnly` alongside the writer succeeds and sees its stages — Task 1 `Workspace.SecondWriterRefusedReaderAllowed`, Task 5 `StageRegistry.ReadOnlyWorkspaceSeesStagesButCannotDeclare`.
3. A reader declared before its writer gets the dependency once the writer is declared, and `isRunnable` waits for it — Task 4 `StageRegistry.ReaderDeclaredBeforeWriter`.
4. Redeclaring a stage whose pipeline was built through `then()` with one built through `operation()` with the same parameters is not a reconfiguration and marks nothing stale — Task 5 `StageRegistry.EquivalentRedeclarationMarksNothingStale`.
5. A workspace reopened in a new process has the same stages, dependencies and stale flags, and redeclaring an unchanged stage there marks nothing stale — Task 5 `StageRegistry.ReopenedWorkspaceRestoresDefinitions`.

---

### Task 1: The module and `Workspace`

**Files:**
- Create: `include/slideio/processor/workspace/workspace.hpp`, `src/workspace/workspace.cpp`, `src/workspace/CMakeLists.txt`
- Modify: `src/CMakeLists.txt` (`add_subdirectory(workspace)`; `slideio-processor-workspace` in the engine's PRIVATE list; the comment saying "later also workspace")
- Create: `tests/workspace/CMakeLists.txt` (copy the engine-test pattern of `tests/pipeline/CMakeLists.txt`, including the PATH launcher properties, prefix `workspace.`, link `TIFF::TIFF`), `tests/workspace/support/test_slide.{hpp,cpp}`, `tests/workspace/test_workspace.cpp`; `tests/CMakeLists.txt` (`add_subdirectory(workspace)`)
- Modify: `docs/superpowers/specs/sds/04-workspace.md` §4.1 (decisions 1, 2, 5), `docs/traceability.md` (new `## workspace` section)

**Interfaces:**
- Produces (§4.1, minus `stages()` and `layerLifecycle()`, which Tasks 2 and 5 add):
  `static WorkspacePtr create(const char* workspacePath, const char* slidePath, int32_t sceneIndex, const Configuration&);` `static WorkspacePtr open(const char*, const Configuration&);` `static WorkspacePtr openReadOnly(const char*, const Configuration&);` `path()`, `slidePath()`, `sceneIndex()`, `isReadOnly()`, `StorePtr store() noexcept`, `std::shared_ptr<slideio::Scene> scene() noexcept`, `configuration()`, `scratchConfiguration()`, `MemoryBudgetPtr memoryBudget() noexcept`, `LayerRegistry& layers() noexcept`, `void close()`.
- Test support (namespace `slideio::processor::testing_support`): `std::filesystem::path writeTestSlide(const std::filesystem::path& file, int32_t width, int32_t height);` writes an 8-bit, 3-sample, tiled TIFF with libtiff; and `class TempDirectory` (unique path under the temp dir, `remove_all` on destruction; `path()`, `utf8()`).

- [ ] **Step 1: Check the slide route.** Write `writeTestSlide` and a first test `Workspace.TestSlideOpensThroughSlideIO` that opens it with `slideio::openSlide(path, "")` and checks one scene of the written size. If SlideIO cannot open it, try driver `"GDAL"` explicitly; if neither opens it, stop and report BLOCKED with the error.
- [ ] **Step 2: Write the failing tests** in `test_workspace.cpp`:
  - `Workspace.CreateBindsSlideAndScene`: after `create`, `path()`, `slidePath()`, `sceneIndex()` are as given, `isReadOnly()` false, `scene()` has the slide's size, `store()->provenance().slideId()` equals the slide path, and `<ws>/workspace.store` exists.
  - `Workspace.CreateRefusesNonEmptyDirectory` (`IoError`) and `Workspace.CreateAcceptsEmptyDirectory`.
  - `Workspace.OpenRestoresBinding`: create, close, `open`; same slide path, scene index and scene size; layers declared before close are found.
  - `Workspace.OpenWithMissingSlideIsIoError`: delete the slide file between close and `open`; the message names the slide path.
  - `Workspace.SecondWriterRefusedReaderAllowed`: with one `open` live, a second `open` throws `IoError`; `openReadOnly` succeeds and `isReadOnly()` is true.
  - `Workspace.ScratchIsInsideAndClearedAtOpen`: `scratchConfiguration().scratchDirectory()` is `<ws>/scratch`, every other setting `equals` `configuration()`'s; a file left in it before `open` is gone after.
  - `Workspace.OpenSweepsUnreferencedRasterFiles`: a file named like a library raster file placed in the rasters directory is removed by `open` (use the naming rule `RasterRegistry::removeUnreferencedFiles` documents).
  - `Workspace.MemoryBudgetIsOneAndSized`: `memoryBudget()` returns the same pointer twice, with ceiling `configuration().memoryCeilingBytes()`.
  - `Workspace.NonAsciiPathsRoundTrip`: workspace and slide under a directory named `"wörk_空間"`; create, close, open.
  - `Workspace.CloseIsIdempotent`.
- [ ] **Step 3: Run them;** expect compile failure.
- [ ] **Step 4: Implement.** `create`: make the directory, open the slide (scene index checked against `getNumScenes()`, `InvalidArgumentError` when out of range), `Store::open(storePath, OpenMode::CreateNew, configuration)`, set provenance, create `scratch/`. `open`/`openReadOnly`: `Store::open(..., ReadWrite | ReadOnly)`, slide from provenance; `open` (not `openReadOnly`) clears `scratch/` and calls `rasters().removeUnreferencedFiles()`, logging the bytes reclaimed at info level with `logMessage`. The budget is created once per `Workspace`.
- [ ] **Step 5: Run** `ctest ... -R workspace.`; then the full suite.
- [ ] **Step 6: Docs** (SDS §4.1 decisions 1, 2, 5; traceability), docs build, **commit** `feat(workspace): the workspace directory binding a slide scene to a store`.

### Task 2: `LayerLifecycle`

**Files:**
- Create: `include/slideio/processor/workspace/layer_lifecycle.hpp`, `src/workspace/layer_lifecycle.cpp`, `tests/workspace/test_layer_lifecycle.cpp`
- Modify: `workspace.hpp/.cpp` (`LayerLifecycle& layerLifecycle() noexcept;`)

**Interfaces:**
- Consumes: Task 1's `Workspace`.
- Produces (§4.6): `LayerId declareSemantic(const char* name, LayerId parent); LayerId declarePrimitive(const char* name); void drop(LayerId primitiveLayer); bool isDropped(LayerId) const noexcept; bool wasEverPopulated(LayerId) const noexcept; int64_t droppedObjectCount(LayerId) const noexcept;` — each delegates to the store's `LayerRegistry` (`declareSemantic`, `declarePrimitive`, `dropPrimitive`, `isDropped`, `wasEverPopulated`, `droppedObjectCount`). "Records the declaration in the workspace" is the store's `LayerDeclared` event; nothing else is kept.

- [ ] **Step 1: Write the failing tests:** `LayerLifecycle.DeclaresThroughTheStore` (a semantic chain pixel → tissue → cells is visible through `ws->layers()`, `isAbove(cells, tissue)`); `LayerLifecycle.DropSemanticIsInvalidArgument`; `LayerLifecycle.DropKeepsDerivedLayerComplete` (write objects to a primitive `superpixels` and a semantic `regions` through `ObjectStore::writerForStage` of a stage declared with `StageStateWriter::declare`, drop `superpixels`: `isDropped` true, `wasEverPopulated` true, `droppedObjectCount` equals what was written, `regions` still returns all its objects, and no stage `isStale`); `LayerLifecycle.NeverPopulatedIsNotDropped`; `LayerLifecycle.ReadOnlyWorkspaceCannotDeclare` (`IoError`).
- [ ] **Step 2: Run;** expect compile failure. **Step 3: Implement.** **Step 4: Run** focused, then full.
- [ ] **Step 5: Traceability; commit** `feat(workspace): layer lifecycle`.

### Task 3: `StageDefinition` and its record

**Files:**
- Create: `include/slideio/processor/workspace/stage_definition.hpp`, `src/workspace/stage_definition.cpp`, `src/workspace/detail/stage_record.{hpp,cpp}`, `tests/workspace/test_stage_definition.cpp`
- Modify: SDS 04 §4.2 (decisions 3 and 4: remove `configurationJson`, give the record's full shape), `src/workspace/CMakeLists.txt` (rapidjson PRIVATE)

**Interfaces:**
- Produces: `StageDefinition` exactly as §4.2 declares it, except `configurationJson` (decision 3); also copy assignment `StageDefinition& operator=(const StageDefinition&)`. `readLayerAt` lists the union of explicit reads, annotated layers, the restriction and the raster-read layers, each once, in first-declaration order. `writesRaster` called twice throws `InvalidArgumentError` (§4.2: at most one).
- Produces (detail, namespace `slideio::processor::detail`):
  - `std::string stageRecordJson(const StageDefinition&, const LayerRegistry&, const RasterRegistry&);` — decision 4's shape; names from the registries; `InvalidArgumentError` naming the stage when an id is unknown to them.
  - `StageDefinition stageFromRecord(const char* json, const LayerRegistry&, const RasterRegistry&);` — the inverse; `InvalidArgumentError` on malformed text, a `stageRecordVersion` other than 1, or a name the registries do not hold (naming it).
  - `bool sameDeclaration(const StageDefinition& a, const StageDefinition& b);` — name, `PipelineSpec::equals`, and exact equality of every declaration list and optional (explicit reads, writes, annotates, restriction, layer rasters with binding and content, written raster, raster reads with binding, explicit dependencies), order-insensitive within each list.

- [ ] **Step 1: Write the failing tests:**
  - `StageDefinition.AccessorsReflectDeclarations` (each builder member reflected by its accessors; `annotatesLayer`, `restrictToLayer` and `readsLayerAsRaster` each appear once in `readLayerAt` and not in `writtenLayerAt`).
  - `StageDefinition.SecondWrittenRasterRefused`.
  - `StageDefinition.CopyIsIndependent` (changing the copy leaves the original).
  - `StageRecord.RoundTripsEveryField` (a definition using every builder member → `stageRecordJson` → `stageFromRecord` → `sameDeclaration` true; the JSON parsed with rapidjson has `"stage"`, `"writesLayers"` by name and a `"pipeline"` object that `PipelineSpec::fromJson` accepts).
  - `StageRecord.ImpliedReadsAreNotRecorded` (`restrictToLayer(tissue)` alone yields `"readsLayers":[]`).
  - `StageRecord.UnknownNameOnLoadIsInvalidArgument` (message names the layer).
  - `StageRecord.WrongVersionIsInvalidArgument`.
  - `StageRecord.SameDeclarationIgnoresListOrderAndJsonFormatting` (two definitions reading `a, b` and `b, a` compare equal; differing restriction compares unequal).
  Layers and rasters come from a store opened with `Store::open(..., CreateNew, ...)` in a `TempDirectory`.
- [ ] **Step 2: Run;** expect compile failure. **Step 3: Implement** (rapidjson, as `PipelineSpec::toJson` does). **Step 4: Run** focused, then full.
- [ ] **Step 5: SDS §4.2 amendment, traceability; commit** `feat(workspace): stage definitions and their durable record`.

### Task 4: `StageRegistry` — declaring and deriving

**Files:**
- Create: `include/slideio/processor/workspace/stage_registry.hpp`, `src/workspace/stage_registry.cpp`, `tests/workspace/test_stage_registry.cpp`
- Modify: `workspace.hpp/.cpp` (`StageRegistry& stages() noexcept;`)

**Interfaces:**
- Consumes: Task 3's detail functions; `StageStateWriter` (`declare`, `setConfiguration`, `addWrittenLayer`, `clearDependencies`, `addDependency`) and `StageStateReader`.
- Produces (§4.3 minus `staleness()`, decision 6): `StageId declare(const StageDefinition&); count(); at(); find(); name(); status(); isStale(); configuration(); completedAtUnixMs(); dependencyCount(StageId); dependencyAt(StageId, int32_t); isExplicitDependency(StageId, int32_t); isRunnable(StageId);`. `markStale` and `clearStale` are added in Task 5, not declared here: the class is exported, and MSVC exports every declared member, so a declared-but-undefined one fails to link. Also `const StageDefinition& definition(StageId) const;` (for plan B's runner; the reference stays valid until the next `declare`). Read accessors delegate to `StageStateReader`; ThreadSafe under one mutex.
- Effective dependencies of S: every stage writing a layer S reads (`readLayerAt`), every stage whose `writtenRaster()` is a raster S reads, and S's explicit `dependsOn` (resolved by name; an unknown name throws `StageError` naming both stages). Ordered by ascending `StageId`, each once; `isExplicitDependency` is true when it came from `dependsOn` (even if also derived).

- [ ] **Step 1: Write the failing tests:**
  - `StageRegistry.DeclarePersistsRecordAndWrittenLayers` (`configuration(id)` equals `stageRecordJson`; `store()->layers().writerStageAt(nuclei, 0) == id`; status `Pending`).
  - `StageRegistry.DependencyDerivedFromReads` (§4.7's background → nuclei: nuclei depends on background, not explicit; `StageStateReader::dependencyAt` agrees).
  - `StageRegistry.ReaderDeclaredBeforeWriter` (declare nuclei first: no dependency; declare background: nuclei now depends on it).
  - `StageRegistry.RasterReadDerivesDependency`.
  - `StageRegistry.ExplicitDependencyFlagged`, `StageRegistry.UnknownExplicitDependencyIsStageError`.
  - `StageRegistry.ReadOfUndeclaredLayerIsInvalidArgument` (a `LayerId` the store never issued).
  - `StageRegistry.CycleIsStageErrorNamingStages` (A reads x writes y, B reads y writes x; the second `declare` throws, the message contains both names, and the registry is unchanged: count 1, A has no dependency).
  - `StageRegistry.LayerWrittenByNoStageIsNotAnError`.
  - `StageRegistry.RunnableWhenDependenciesCompleteAndCurrent` (set statuses through `StageStateWriter::setStatus`/`setStale`: runnable only when every dependency is `Complete` and not stale; a stage with no dependencies is runnable).
  - `StageRegistry.TwoStagesWritingOneLayerBothCount` (a reader of that layer depends on both).
- [ ] **Step 2: Run;** expect compile failure. **Step 3: Implement.** Until Task 5, declaring an existing name throws `InvalidArgumentError` (what `StageStateWriter::declare` throws for a duplicate). A failed `declare` writes nothing: validate and derive the new graph in memory first (cycle check by DFS over the candidate graph), then persist. **Step 4: Run** focused, then full.
- [ ] **Step 5: Traceability; commit** `feat(workspace): stage registry with derived dependencies`.

### Task 5: `StageRegistry` — redeclaring, staleness and restoring

**Files:**
- Modify: `src/workspace/stage_registry.cpp`, `src/workspace/workspace.cpp` (restore on `open`/`openReadOnly`), `tests/workspace/test_stage_registry.cpp`
- Modify: SDS 04 §4.3 (decisions 6 and 7), `docs/superpowers/specs/sds/08-class-diagrams.md` (workspace classes as built), `docs/project.md` (a "Workspace implementation plan A" entry: plan A of three, what B and C hold, status), traceability

**Interfaces:**
- Consumes: Task 4.
- Produces: `void markStale(StageId);` — sets the flag on the stage and on every stage reachable from it along reversed effective dependencies (its dependents, transitively), through `StageStateWriter::setStale`. `void clearStale(StageId);` — that stage only. `declare` of an existing name: when `sameDeclaration` holds, nothing changes and the same `StageId` returns; otherwise the record is rewritten (`setConfiguration`), the dependencies re-derived, and `markStale(id)` called. Restoring: the registry's constructor reads every stage row and rebuilds definitions with `stageFromRecord`; a row whose record does not parse is kept by name only and is never runnable (log a warning naming the stage).

- [ ] **Step 1: Write the failing tests:**
  - `StageRegistry.MarkStaleIsTransitive` (background → nuclei → cells: marking background marks both; `StageStateReader::isStale` agrees; an unrelated stage stays current).
  - `StageRegistry.ClearStaleIsNotTransitive`.
  - `StageRegistry.ReconfiguredStageAndDependentsGoStale` (redeclare nuclei with a different pipeline parameter: nuclei and cells stale, background not; `configuration(id)` is the new record; same `StageId`).
  - `StageRegistry.EquivalentRedeclarationMarksNothingStale` (first declaration built with `PipelineSpec::then(OperationCatalog::instance().create(name, params))`, second with `operation(name, params)`; nothing stale).
  - `StageRegistry.RedeclarationThatWouldCycleIsRefused` (registry unchanged).
  - `StageRegistry.ReopenedWorkspaceRestoresDefinitions` (declare three stages, mark one stale, close; `open`: same names, ids, dependencies, stale flags, `sameDeclaration(definition(id), original)`; redeclaring an unchanged stage marks nothing stale).
  - `StageRegistry.ReadOnlyWorkspaceSeesStagesButCannotDeclare` (`openReadOnly` alongside a live writer: stages and dependencies visible; `declare` and `markStale` throw `IoError`).
- [ ] **Step 2: Run;** expect FAIL. **Step 3: Implement.** **Step 4: Run** focused, then full.
- [ ] **Step 5: Docs** (SDS §4.3, SDS 08, project.md, traceability), docs build; **commit** `feat(workspace): redeclaration, staleness along dependencies, restore at open`.
