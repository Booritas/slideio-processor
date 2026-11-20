# Workspace Implementation Plan C

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Finish the `workspace` module:
- `StalenessGraph` (§4.5), the five propagation paths with their reasons, in both directions;
- `StageRegistry::staleness()`;
- `markStale` along every path;
- `dependsOn` that may name a stage declared later;
- `declareCoarseToFine` (§4.2);
- the follow-ups plans A and B left.

**Architecture:** `StalenessGraph` is an exported, pimpl'd, read-only view owned by `StageRegistry`. Its edges are rebuilt in memory with the dependency graph on every `declare` and restore, under the registry's lock. Nothing new is persisted. The dependency rows stay the effective dependencies (decision 7 of plan A), and the staleness flag stays the durable truth. `declareCoarseToFine` is a free function over `StageRegistry` and `StageDefinition`. This is plan C of three: plans A and B are merged (main 491e2a9).

**Tech Stack:** C++17, CMake, SQLite via `storage`, GoogleTest, MSVC.

**Spec:** `docs/superpowers/specs/sds/04-workspace.md` §4.5, §4.2 "Declaring a coarse-to-fine pair", and §4.3 (`staleness()`, `markStale`, `dependsOn`). Requirements: UR-STG-2, -8, -11, -12, UR-SEG-8, UR-IMG-4, UR-STG-10 in `docs/requirements.md`. Conventions: `00-conventions.md` (C-1 to C-13).

## Global Constraints

- Build `./build.sh --skip-slideio`; tests `./run-tests.sh` (all must pass); one test `ctest --test-dir build -C Release -R <name>` (test exes cannot run directly: DLL path). Docs `cmake --build build --target docs --config Release`.
- Zero compiler warnings and zero Doxygen warnings.
- Public headers live under `include/slideio/processor/workspace/`, in namespace `slideio::processor`.
  - Classes are exported with `SLIDEIO_PROCESSOR_ENGINE_EXPORTS`, and every non-trivial class is pimpl'd (C-2).
  - MSVC dllexport exports every declared member, so never declare a member without defining it.
  - Strings in are `const char*` UTF-8 and copied; strings out are owned by the object (C-4).
  - Collections are a count plus an indexed accessor (C-5).
- Each public member is documented with `@par Thread-safety` and `@par Requirements` as the SDS states them (C-7, C-12).
- Every source and test file starts with its `// Requirements: ...` line. `docs/traceability.md` `## workspace` lists every test, one row per requirement, updated in place.
- Error messages and reason details name stages, layers and rasters, never pixel, coordinate or parameter values, and never the slide path.
- `workspace` depends on `core`, `config`, `storage`, `pipeline` only; it never links `algorithm`.
- `StageRegistry` keeps its plan A design:
  - one mutex, with no locking public accessor called while it is held;
  - the in-memory graph is the single source for graph accessors;
  - a read-only registry refreshes before graph-backed accessors;
  - unreadable stages are kept by name with their edges pinned.
- Match the surrounding code's comment density, naming and idiom. No Python on the machine: edit with the Edit tool or sed.
- Conventional commit messages; one commit per task at least.

## Decisions this plan makes (amend the SDS in the task that implements each)

1. **The five edges.** For stages U and V (U ≠ V), there is an edge U → V with a reason when:
   - **LayerRead**: V reads (`readLayerAt`) a layer that U writes.
   - **RasterRead**: V reads (`readRasterAt`) the raster U writes (`writtenRaster`).
   - **LayerOrdering**: V writes a semantic layer that is strictly above (`LayerRegistry::isAbove(descendant, ancestor)`) a layer U writes. For example, the `tissue` writer reaches the `cells` and `nuclei` writers.
   - **DerivedFrom**: V reads a *primitive* layer that U writes, and V writes at least one semantic layer. Such a pair is reported as DerivedFrom instead of LayerRead.
     - Why: a stage can only assert derived-from edges onto objects it has read, so a reads-based relation finds every derivation a run can produce.
     - Storage has no layer-level derived-from relation, and object-level edges are not consulted.
   - **StageDependency**: V's explicit `dependsOn` names U.

   Edges come from the in-memory definitions, never from `LayerRegistry::writerStageAt`, which keeps a writer a redeclaration dropped (plan A follow-up). An unreadable stage contributes its pinned edges with reason `StageDependency`.

   The staleness graph may contain cycles. One example: a `cells` stage that reads `nuclei`, with `nuclei` below `cells`. Only the effective-dependency graph (LayerRead, RasterRead, StageDependency) must stay acyclic, as today. `isRunnable` and the persisted dependency rows are unchanged: LayerOrdering and DerivedFrom do not gate running.

2. **Forward answer.** `affectedCount/At/reasonAt/reasonDetailAt(S)` list one entry per edge U → V in the subgraph reachable from S, with V as the affected stage:
   - in breadth-first order from S, edges of one source ordered by ascending V, then by reason enum;
   - a stage reached down two paths appears twice;
   - S itself appears only when a cycle leads back to it.

   Each detail names the source stage and the artifact, e.g. `reads layer "tissue" written by stage "background"` or `writes layer "nuclei" above layer "tissue" written by stage "background"`.

3. **Reverse answer, from durable data.** For a stale stage S, the causes are its incoming edges U → S whose U is "changed since S": U is stale, or U is not `Complete`, or `completedAtUnixMs(U) > completedAtUnixMs(S)`, one entry per qualifying edge (a pair joined by two reasons appears twice), ordered by ascending U, then by reason enum. When no incoming edge qualifies, S is its own cause, with reason `Reconfigured` and detail `the stage was redeclared or marked stale directly`. A stage that is not stale has 0 causes.
   - Causes need no extra storage, and they are the same after a reopen.
   - `None` is never reported. It remains in the enum as §4.5 declares it.

4. **`markStale` follows all five paths.** It walks the staleness graph forward. A new member, `void markAffectedStale(StageId)`, marks every stage the forward answer lists, excluding S unless a cycle reaches it, in one locked pass (PR #16 finding #7). `StageRunner`'s re-run step 1 calls it instead of scanning dependencies itself. `LayerLifecycle::drop` still marks nothing (UR-STG-12).

5. **`dependsOn` may name a stage not yet declared** (plan A deferral, §4.3 "derivation is order-independent").
   - An unresolved name makes the stage not runnable. `isRunnable` is false, and `StageRunner`'s gate refuses with a `StageError` naming the missing stage.
   - It is not counted in `dependencyCount` until the named stage is declared; then the next derivation resolves it.
   - A self-dependency stays a `StageError` at `declare`.
   - A cycle closed by a later declaration is refused at that declaration, as today.
   - The restore fallback (plan A minor 6) no longer treats a dangling `dependsOn` as unreadable.
   - New accessors: `int32_t unresolvedDependencyCount(StageId) const;` and `const char* unresolvedDependencyAt(StageId, int32_t) const;`.

6. **`declareCoarseToFine`** validates everything before declaring anything:
   - the stage names differ;
   - the written layers differ;
   - when both `analysisLevel()`s are ≥ 0, the coarse one is strictly greater (coarser);
   - both halves were set.

   It throws `StageError` naming the offending field (`"names"`, `"layers"`, `"levels"`, `"coarse"`, `"fine"`). It then declares the coarse stage and then the fine one. If the fine `declare` throws (for example on a cycle), the coarse stage stays declared, because `storage` has no transaction across declarations; the helper documents this and rethrows. The binding defaults to `"coarse"` with `LayerRasterContent::ObjectAnchor`, and `restrictFineToCoarse` defaults to true. It lives in `include/slideio/processor/workspace/coarse_to_fine.hpp`.

7. **Follow-ups folded in** (memory `workspace-a-followups`, `workspace-b-followups`), each with a test:
   - A read-only refresh never swaps in a shrunken or empty state. When the stored stage count is lower than the snapshot, it keeps the snapshot. When the store is closed, `requireOpen` throws first.
   - A refresh keeps the previous pins for a stage that is still unreadable.
   - `markStale` and `clearStale` on a read-only registry throw `IoError` before resolving the stage.
   - `UnsupportedDataTypeError` and `ChannelArityError` from `validate()` carry `ErrorContext.operationName`.
   - The coarse-restriction test gets a disc in the dilation band, so a zero half-step fails it.
   - The SDS §4.4 wording "steps 2 and 3 sit inside the guarded region" is aligned with the code, which guards step 1 too.

   Not in this plan:
   - gating the test seams behind a CMake option;
   - sanitising foreign exception text (this needs Security/privacy sign-off);
   - the raster output-type check;
   - the "seam merge" progress callback.

## Review Focus

1. **A staleness graph with a cycle.** A `cells` stage reads `nuclei`, and `nuclei` is below `cells`. `markStale` must terminate, mark both, and leave the effective graph acyclic and declarable. Task 1, `StalenessGraph.CycleThroughLayerOrderingTerminates`.
2. **Causes survive a reopen.** Re-run `background`, close, reopen read-only: `causeAt(cells)` names `background` with `LayerOrdering`, from durable data alone. Task 2, `StalenessGraph.CausesAreTheSameAfterReopen`.
3. **Dropping a primitive layer marks nothing stale**, even when a semantic stage reads it (UR-STG-12). Task 2, `StalenessGraph.DroppingAPrimitiveLayerMarksNothing`.
4. **A `dependsOn` naming a stage declared later.** The stage is refused at run, naming the missing stage. Once the other is declared and completes, it runs. A later declaration that closes a cycle through it is refused. Task 3, `StageRegistry.LateDeclaredDependencyResolves`.
5. **`declareCoarseToFine` with equal explicit levels, or with the fine level automatic (-1).** Equal levels throw `StageError` naming `"levels"` and declare nothing. A fine level of -1 is accepted. Task 4, `CoarseToFine.LevelsMustBeStrictlyOrdered`.

---

### Task 1: `StalenessGraph` — edges and the forward answer

**Files:**
- Create: `include/slideio/processor/workspace/staleness_graph.hpp`, `src/workspace/staleness_graph.cpp`, `tests/workspace/test_staleness_graph.cpp`
- Modify: `include/slideio/processor/workspace/stage_registry.hpp` and `src/workspace/stage_registry.cpp` (`staleness()`, `markStale` along the graph), `src/workspace/CMakeLists.txt`, `tests/workspace/CMakeLists.txt`

**Interfaces:**
- Consumes: plan A's `StageRegistry` internals (definitions by `StageId`, the effective graph, pins, the mutex, the read-only refresh), and `LayerRegistry::kind`, `isAbove`, `name`.
- Produces:
  - `enum class StalenessReason : int32_t { None, LayerRead, RasterRead, LayerOrdering, DerivedFrom, StageDependency, Reconfigured };`
  - `StalenessGraph` exactly as §4.5 declares it, with the forward members implemented here. The reverse members are declared and implemented in Task 2, so do not declare them here.
  - `const StalenessGraph& StageRegistry::staleness() const noexcept;`
  - The graph's accessors take the registry's mutex. Each copies the strings it returns into storage owned by the graph that lives until the next `declare` or refresh, which the header documents. An unknown stage throws `InvalidArgumentError`.

- [ ] **Step 1: Write the failing tests** (layers `tissue` > `cells` > `nuclei` semantic; `superpixels` primitive; stages declared through the registry; statuses set through `StageStateWriter` where needed):
  - `StalenessGraph.LayerReadEdge`: `background` writes `tissue`, and `cells` restricts to `tissue`. `affected(background)` has `(cells, LayerRead)`, and the detail contains `"tissue"` and `"background"`.
  - `StalenessGraph.RasterReadEdge`.
  - `StalenessGraph.LayerOrderingReachesUndeclaredDependents`: `background` writes `tissue`. `nuclei` writes `nuclei` and reads nothing. `affected(background)` contains `(nuclei, LayerOrdering)` (UR-STG-11 acceptance).
  - `StalenessGraph.DerivedFromReplacesLayerReadForPrimitives`: `sp` writes `superpixels` (primitive). `cells` reads `superpixels` and writes `cells`. Exactly one entry: `(cells, DerivedFrom)`.
  - `StalenessGraph.ExplicitDependencyIsStageDependency`.
  - `StalenessGraph.TransitiveAndMultiPathEntries`: `cells` is reached by both LayerRead and LayerOrdering, so it appears twice. A stage two hops away appears, with the reason of its own edge.
  - `StalenessGraph.CycleThroughLayerOrderingTerminates` (Review Focus 1): `cells` writes `cells` and reads `nuclei`; `nucStage` writes `nuclei`. Both declare successfully. `markStale(cells)` marks both and returns. `affected(cells)` lists `(nucStage, LayerOrdering)`, and `affected(nucStage)` lists `(cells, LayerRead)`.
  - `StalenessGraph.MarkStaleFollowsLayerOrdering`: `markStale(background)` marks `nuclei`, which only LayerOrdering reaches.
  - `StalenessGraph.ReadOnlyViewFollowsLiveWriter`: an observer's `staleness()` sees a stage the writer declared after the observer opened.
- [ ] **Step 2: Run;** expect a compile failure. **Step 3: Implement** decision 1 and decision 2; `markStale` walks the graph with a visited set. **Step 4: Run** the focused tests (`-R "StalenessGraph|StageRegistry"`), then the full suite.
- [ ] **Step 5: SDS §4.3 and §4.5** (decisions 1 and 2; replace the plan A note on staleness), traceability (UR-STG-8, UR-STG-11); **commit** `feat(workspace): staleness graph with five propagation paths`.

### Task 2: Causes, one-pass marking and the runner

**Files:**
- Modify: `staleness_graph.hpp/.cpp`, `stage_registry.hpp/.cpp` (`markAffectedStale`), `src/workspace/stage_runner.cpp` (re-run step 1), the tests

**Interfaces:**
- Consumes: Task 1; `StageStateReader` (`isStale`, `status`, `completedAtUnixMs`).
- Produces:
  - `causeCount`, `causeAt`, `causeReasonAt` and `causeReasonDetailAt`, per decision 3.
  - `void StageRegistry::markAffectedStale(StageId);` per decision 4, ThreadSafe, `IoError` when read-only.
  - `StageRunner` calls it once at re-run step 1.

- [ ] **Step 1: Write the failing tests:**
  - `StalenessGraph.CausesNameTheChangedUpstream`: run `background` and `cells` (restricted to `tissue`), then re-run `background`. `causeCount(cells) == 1`, and `causeAt` is `background` with `LayerRead`.
  - `StalenessGraph.CausesAreTheSameAfterReopen` (Review Focus 2): after the above plus a `nuclei` stage reached by LayerOrdering, close the workspace and `openReadOnly`. The causes of `nuclei` and of `cells` are unchanged.
  - `StalenessGraph.ReconfiguredStageIsItsOwnCause`: redeclare `cells` with a changed pipeline. `causeAt(cells) == cells` with `Reconfigured`.
  - `StalenessGraph.CurrentStageHasNoCauses`.
  - `StalenessGraph.DroppingAPrimitiveLayerMarksNothing` (Review Focus 3).
  - `StageRegistry.MarkAffectedStaleMarksEveryPathOnce`: a diamond of four stages. Each is set stale exactly once, which you check by counting `setStale` writes through the journal or a seam. Use whatever is observable. If nothing is, assert the final flags and document why.
  - `StageRunner.RerunMarksLayerOrderingDependentsStale`: re-running `background` through `StageRunner` makes the LayerOrdering-only `nuclei` stage stale.
- [ ] **Step 2: Run;** expect FAIL. **Step 3: Implement.** **Step 4: Run** focused, then full.
- [ ] **Step 5: SDS §4.3, §4.4** (step 1 uses `markAffectedStale`), and §4.5 (decision 3); traceability; **commit** `feat(workspace): staleness causes and one-pass marking`.

### Task 3: `dependsOn` naming a stage declared later

**Files:**
- Modify: `stage_registry.hpp/.cpp`, `src/workspace/stage_runner.cpp` (gate message), `tests/workspace/test_stage_registry.cpp`, `tests/workspace/test_stage_runner.cpp`

**Interfaces:**
- Consumes: plan A's derivation, cycle check and restore; plan B's gate.
- Produces: decision 5, including `unresolvedDependencyCount` and `unresolvedDependencyAt`.

- [ ] **Step 1: Write the failing tests:**
  - `StageRegistry.LateDeclaredDependencyResolves` (Review Focus 4):
    1. Declare `b` with `dependsOn("a")`. The declaration succeeds, `isRunnable(b) == false`, `unresolvedDependencyAt(b, 0) == "a"`, and `dependencyCount(b) == 0`.
    2. Declare `a`. Now `dependencyAt(b, 0) == a`, the edge is explicit, and the unresolved count is 0.
    3. Declare `c` with `dependsOn("b")` and writing a layer that `a` reads, which closes a cycle. That declaration is refused with a `StageError` naming the stages.
  - `StageRegistry.UnresolvedDependencySurvivesReopen`.
  - `StageRegistry.SelfDependencyIsStillRefused`.
  - `StageRunner.UnresolvedDependencyIsRefusedAtTheGate`: the `StageError` names `"a"`, and the status stays `Pending`.
  - Update or replace `StageRegistry.UnknownExplicitDependencyIsStageError` and the restore-fallback test, whose behaviour this decision changes. Keep the restore fallback for its other trigger, a self-dependency in a record.
- [ ] **Step 2: Run;** expect FAIL. **Step 3: Implement.** **Step 4: Run** focused, then full.
- [ ] **Step 5: SDS §4.3** (decision 5) and traceability (UR-STG-2); **commit** `feat(workspace): dependsOn may name a stage declared later`.

### Task 4: `declareCoarseToFine`

**Files:**
- Create: `include/slideio/processor/workspace/coarse_to_fine.hpp`, `src/workspace/coarse_to_fine.cpp`, `tests/workspace/test_coarse_to_fine.cpp`

**Interfaces:**
- Consumes: `StageRegistry::declare`, `StageDefinition`, `PipelineSpec::analysisLevel()`, `LayerRasterContent`.
- Produces:
  - `CoarseToFineSpec` and `declareCoarseToFine(StageRegistry&, const CoarseToFineSpec&, StageId& coarseOut, StageId& fineOut)`, exactly as §4.2 declares them.
  - `CoarseToFineSpec` also gets a copy assignment, since a copy constructor is declared. It is pimpl'd and exported.
  - Decision 6 applies.

- [ ] **Step 1: Write the failing tests:**
  - `CoarseToFine.DeclaresTwoWiredStages`. The fine definition has:
    - `rasterBindingAt(0) == "coarse"`;
    - `rasterContentAt(0) == ObjectAnchor`;
    - `rasterLayerAt(0) == coarse layer`;
    - `layerRestriction() == coarse layer`;
    - `dependencyAt(fine, 0) == coarse`, not explicit.

    `staleness().affected(coarse)` has `(fine, LayerRead)`.
  - `CoarseToFine.BindingAndRestrictionCanBeChanged`: `binding("prob", Membership)` together with `restrictFineToCoarse(false)`.
  - `CoarseToFine.LevelsMustBeStrictlyOrdered` (Review Focus 5): coarse 2 and fine 2 throw `StageError` containing `"levels"`, and the registry count is unchanged. Coarse 2 and fine -1 are accepted. Coarse 1 and fine 3 are refused.
  - `CoarseToFine.SameNamesOrLayersAreRefused`, and `CoarseToFine.MissingHalfIsRefused`.
  - `CoarseToFine.PairRunsEndToEnd`:
    1. On the blob slide (plan B's test support), declare the coarse half at `analysisLevel(2)` writing `nests_coarse`, and the fine half at level 0 writing `nests` with a chain that reads the binding (`test.ws.auxProbe` with `binding = "coarse"`).
    2. Run both with `StageRunner`. The fine stage completes, and the probe saw the binding.
    3. Re-run the coarse stage. The fine stage is stale.
- [ ] **Step 2: Run;** expect a compile failure. **Step 3: Implement.** **Step 4: Run** focused, then full.
- [ ] **Step 5: SDS §4.2** (decision 6), traceability (UR-SEG-8, UR-IMG-4, UR-STG-10); **commit** `feat(workspace): declareCoarseToFine`.

### Task 5: Follow-ups from plans A and B, and the module docs

**Files:**
- Modify: `src/workspace/stage_registry.cpp`, `src/pipeline/detail/run_plan.cpp`, `tests/workspace/test_stage_registry.cpp`, `tests/workspace/test_stage_runner.cpp`, `tests/workspace/support/test_slide.{hpp,cpp}`, `tests/pipeline/test_pipeline_validate.cpp`
- Modify: SDS 04 §4.3 and §4.4, `docs/superpowers/specs/sds/08-class-diagrams.md` (`StalenessGraph`, `CoarseToFineSpec`, the new registry members), `docs/project.md` (a "Workspace implementation plan C" entry with status; plan B's status is merged as 491e2a9), traceability

**Interfaces:**
- Consumes: Tasks 1–4.
- Produces: decision 7.

- [ ] **Step 1: Write the failing tests:**
  - `StageRegistry.RefreshNeverShrinksToEmpty`: the writer closes while an observer holds a snapshot. The observer's graph accessors keep the snapshot or throw `IoError`, and never report 0 stages for a store that had some. Use a test seam if a racing close cannot be made deterministic.
  - `StageRegistry.RefreshKeepsPinsOfAStillUnreadableStage`.
  - `StageRegistry.ReadOnlyMarkStaleOfALaterStageIsIoError`: an observer calls `markStale` or `clearStale` on a stage the writer declared after the observer opened, and gets `IoError`.
  - `PipelineValidate.TypeAndArityErrorsNameTheOperation`.
  - `StageRunner.RestrictionToCoarseLayerIsOneSided`, extended: a disc whose centre lies inside the dilation band (outside the undilated level-2 region, within `halfStep`) must be found. Show that it fails with the half-step forced to 0, record that in the report, then restore.
- [ ] **Step 2: Run;** expect FAIL. **Step 3: Implement.** **Step 4: Run** focused, then full.
- [ ] **Step 5: Docs** (the §4.4 guarded-region wording, SDS 08, project.md, traceability), docs build; **commit** `fix(workspace): plan A and B follow-ups; workspace module docs`.
