# Single-Transaction Declare Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `StageRegistry::declare` writes all of its rows in one storage transaction. A read-only registry refreshing during a declare then sees the store either wholly before it or wholly after it, never between, and a declare that fails changes neither the store nor the registry.

**Architecture:**
- **Today.** `declare` makes many separate commits: the stale flags, the configuration, the written layers, then each stage's dependency rows, cleared and re-added.
- **The race.** SDS 04 §4.3 (~708-717) records a refresh that lands between the configuration write and the dependency rewrite. It re-pins a stale stored row to a stage that is still unreadable, and keeps that pin afterwards, so the reader and the writer disagree.
- **The fix, part 1: storage.** Storage gains a public RAII `StageStateWriter::Batch`. While it is open, every write the calling thread makes on that store joins one `BEGIN IMMEDIATE` transaction, each write as a savepoint, until `commit()`.
- **The fix, part 2: the registry.** `declare` is restructured as compute aside, write once, adopt:
  - every row and every in-memory structure is derived before the store is touched;
  - all writes go into one batch;
  - the new in-memory state is swapped in only after the commit succeeds.

**Tech Stack:** C++17, CMake, SQLite (WAL), GoogleTest, MSVC.

**Spec:**
- `docs/superpowers/specs/sds/04-workspace.md` §4.3: `declare`, "The edges around an unreadable stage" (~698-717), and "A read-only registry follows a live writer" (~719-735).
- `docs/superpowers/specs/sds/03-storage.md` §3.10a, `StageStateWriter`.
- UR-STG-2, UR-STG-4, UR-OBS-1.

**Base:** branch `fix/single-transaction-declare` from `main` (b39c3a1).

## Global Constraints

- **Commands:**
  - Build: `./build.sh --skip-slideio`.
  - Tests: `./run-tests.sh`; all must pass.
  - One test: `ctest --test-dir build -C Release -R <name>`.
  - Docs: `cmake --build build --target docs --config Release`.
- Zero compiler warnings and zero Doxygen warnings.
- **Public header changes:**
  - Allowed: `StageStateWriter::Batch` in `include/slideio/processor/storage/stage_state_writer.hpp`.
  - Not allowed: any change to the `StageRegistry` header.
  - SDS 03 §3.10a and SDS 08's `StageStateWriter` class box show `Batch`.
- **No `std::function` in public headers;** none exists today. Engine-to-data hooks use a function pointer plus `void*`, as `hookNextWorkspaceCreateForTesting` does.
- **Existing behaviour must hold.** Every existing `StageRegistry.*`, `StageRunner.*`, `StalenessGraph.*` and storage test passes unchanged. Write behaviour outside a batch is unchanged, byte for byte.
- **Messages:** they never carry pixel, coordinate or parameter values. Stage names may appear, as they do today.
- **Requirements lines and traceability:**
  - Every new source and test file starts with its `// Requirements: ...` line.
  - `docs/traceability.md` lists every new test, updated in place in the existing rows.
- Match the surrounding code's idiom and comment density.

## Decisions

1. **`StageStateWriter::Batch` (public, RAII), exported from the data library.**
   - **Members:**
     - `explicit Batch(StageStateWriter& writer)`.
     - `~Batch()`, which rolls back unless committed and never throws.
     - `void commit()`.
     - Not copyable or movable.
   - **The constructor** takes the store's write mutex and runs `BEGIN IMMEDIATE`. It throws:
     - `IoError` on a read-only or closed store, as a write does today;
     - `InvalidArgumentError("StageStateWriter::Batch: a batch is already open on this thread")` when the calling thread already holds a batch on that store. Nesting is refused.
   - **Joining.** While the batch is open, a write by the same thread through any writer of that store joins it. A join is `SAVEPOINT`, then `RELEASE` on success; when the write throws, `ROLLBACK TO` and `RELEASE`, then the same exception as today. So a failed write undoes only itself, and the batch can still commit.
   - **Durable writes refused.** A durable write (`withDurableWriteTransaction`) on the batch's thread throws `InvalidArgumentError("<operation>: a durable write cannot join a batch")`. Its synchronous level cannot change inside a transaction.
   - **Other threads** wait on the write mutex until the batch ends, as they do for any write today.
   - **`commit()`** runs `COMMIT`. On failure it rolls back and throws `IoError`. Calling it twice is `InvalidArgumentError`.
   - **Side effects deferred to the commit.**
     - `setStatus` inside a batch defers its `notifyCommitted()` until the batch commits, and skips it on rollback.
     - `revision()` still counts each call. It may count a change a batch then rolled back, but never misses one, and its doc says so.
2. **Where the mechanics live.** The ownership and the joining go in `detail::StoreImpl`:
   - **New members:** the owning thread id, a held `std::unique_lock<std::mutex>`, and a pending-notify flag.
   - **`runWriteTransaction`** checks the owner first; on the owning thread it joins with a savepoint instead of locking.
   - **`close()`** keeps taking the write mutex, so it waits for an open batch on another thread. A batch must not be open on the thread that closes the store; `Batch`'s doc says so.
3. **`declare` runs as compute aside, write once, adopt.** Before the batch opens, compute:
   - the candidate entries, pins and unresolved positions (today's candidate);
   - the next dependency graph and staleness edges;
   - for a redeclare that changes the declaration, the set of stages to mark stale: the stage itself, plus every stage the forward answer lists under the current edges and under the candidate edges, each once;
   - the record JSON and the definition copy.

   Then, in one `Batch`, by case:
   - **New stage:** `writer.declare`, which gives the id. Re-key the candidate entry from its placeholder to that id and derive the graph and staleness edges with the real id. Then the written layers, then every stage's dependency rows (`clearDependencies` and `addDependency`, as `deriveAndPersist` writes them today).
   - **Changed redeclare:** the stale flags, `setConfiguration`, the written layers, then every stage's dependency rows.
   - **Equivalent redeclare:** today's repair writes (the written layers and every stage's rows), only on a writable store.

   Then `commit()`, then the swap. The swap must not be able to throw, so reserve capacity before the batch, as `declare` already does for `retired`. The swap replaces the definitions (the old one retired), `unreadable`, `pins`, `graph`, `stalenessEdges` and `unresolved`, and increments `edgesGeneration`.
   - **On failure:** if anything throws before the commit returns, the batch rolls back and the registry's memory is untouched. The comments about "stale first, so the stage is never silently current" and "rows of an earlier declare that failed part way are repaired" are rewritten. A declare is now all or nothing. The repair stays, because a store written before this change can still hold partial rows.
   - **Refactor:** split `deriveAndPersist` into derive (pure), persist (writes, inside the caller's batch) and adopt (swap). Every caller of `deriveAndPersist` uses the three.
4. **Test seam.** Add `SLIDEIO_PROCESSOR_ENGINE_EXPORTS void hookNextDeclareMidwayForTesting(void (*hook)(void*), void* context) noexcept;` in a new `src/workspace/detail/declare_fault.hpp`.
   - The next `declare` that writes a configuration (new or changed) calls the hook once. The call comes after the stage row or configuration is written and before any dependency row, then the hook is cleared.
   - A hook that throws makes `declare` throw that exception.
   - Under the old code the same point lies between two commits, which is what makes the race test fail before the fix.
5. **Refresh side, no change.** `restore` reads each stage's configuration before any dependency row.
   - With declare atomic, a commit landing during a restore can pair only an old configuration with new rows.
   - The configuration differs from the snapshot, so the next query restores again and reads both new.
   - SDS 04 states this. A single read snapshot for restore is not needed and is not added.
6. **Out of scope.**
   - `markStale`, `clearStale` and `StageRunner`'s writes keep their separate commits.
   - `declareCoarseToFine` keeps one transaction per declaration. Its SDS 04 wording "`storage` has no transaction across declarations" (~422, ~470) becomes "each declaration is its own transaction".

## Review Focus

1. A read-only registry that refreshes in the middle of redeclaring a stage that was unreadable ends with the writer's dependencies and run blocker (`StageRegistry.RefreshMidRedeclareKeepsReaderAndWriterInAgreement`, Task 2).
2. A redeclare that fails part way leaves the store and the registry exactly as before: configuration, stale flags, dependency rows, `definition()`, `dependencyCount` (`StageRegistry.FailedRedeclareChangesNothing`, Task 2).
3. A new-stage declare that fails part way leaves no stage row, and the same name can be declared afterwards (`StageRegistry.FailedDeclareOfANewStageLeavesNoRow`, Task 2).
4. A write from another thread during a batch waits and is then applied; it is neither lost nor interleaved (`StageStateBatch.OtherThreadWritesWaitForTheBatch`, Task 1).
5. A failing write inside a batch undoes only itself, and the batch still commits (`StageStateBatch.FailingWriteUndoesOnlyItself`, Task 1).

---

### Task 1: `StageStateWriter::Batch`

**Files:**
- Modify: `include/slideio/processor/storage/stage_state_writer.hpp`, `src/storage/stage_state_writer.cpp`, `src/storage/detail/store_impl.{hpp,cpp}` (`runWriteTransaction`, the owner, the savepoint join, the deferred notify).
- Test: `tests/storage/test_stage_state.cpp`.
- Docs: `docs/superpowers/specs/sds/03-storage.md` §3.10a, `docs/superpowers/specs/sds/08-class-diagrams.md` (the `StageStateWriter` box, ~998 and the class), `docs/traceability.md`.

**Interfaces:**
- Produces: `class StageStateWriter::Batch { public: explicit Batch(StageStateWriter&); ~Batch(); void commit(); };`, not copyable or movable.

- [ ] **Step 1: Write the failing tests.** Each opens a writable store, plus a second `Store` opened read-only on the same file where it needs to observe, as the existing read-only tests do.
  - **`StageStateBatch.CommitMakesEveryWriteVisibleAtOnce`:**
    - Declare stages A, B and C.
    - Open a batch and in it: `setConfiguration(A, "{\"v\":2}")`, `clearDependencies(B)`, `addDependency(B, C)`, `setStale(C, true)`.
    - Before `commit()`, the read-only store sees A's old configuration, B's old dependency rows and C not stale.
    - After it, the store sees all three new values.
  - **`StageStateBatch.DestructorRollsBack`:** the same writes in a batch that is destroyed without `commit()`. Nothing changes; a later write outside a batch succeeds.
  - **`StageStateBatch.FailingWriteUndoesOnlyItself`:**
    - In a batch: `setConfiguration(A, "{\"v\":2}")`, then `addDependency(A, StageId(9999))`, which throws `InvalidArgumentError` as today.
    - Then `commit()`. A has the new configuration and no dependency on 9999.
  - **`StageStateBatch.OtherThreadWritesWaitForTheBatch`:**
    - Open a batch on the main thread and write `setStale(A, true)`.
    - Start thread T, which calls `setStale(B, true)` and then records whether `committedFlag` was set.
    - The main thread sleeps 100 ms, sets `committedFlag = true`, then commits.
    - Join T. T saw the flag set, and both flags are stored.
  - **`StageStateBatch.NestedBatchIsRefused`:** a second `Batch` on the same thread throws `InvalidArgumentError`. The first still commits.
  - **`StageStateBatch.ReadOnlyStoreRefusesABatch`:** `Batch` on a read-only store throws `IoError`.
  - **`StageStateBatch.CommitTwiceIsRefused`:** a second `commit()` throws `InvalidArgumentError`.
  - **`StageStateBatch.DurableWriteInsideABatchIsRefused`:**
    - Inside a batch, the public call that commits durably throws `InvalidArgumentError`. That call is the `RasterRegistry` operation using `withDurableWriteTransaction` at `src/storage/raster_registry.cpp` ~387; find its public name.
    - The batch still commits its earlier write.
  - **`StageStateBatch.StatusNotifiesOnlyOnCommit`:** use the store's commit waiter (`CommitWaiter` or its public equivalent).
    - A `setStatus` inside a batch that is rolled back does not wake a waiter.
    - One inside a committed batch does.
    - If the waiter API cannot observe this deterministically, replace this test with a unit check of the pending-notify flag through the existing storage test access, and say so in the report.

- [ ] **Step 2: Run them to verify they fail**

Run: `ctest --test-dir build -C Release -R "StageStateBatch\."`
Expected: compile failure (`Batch` does not exist).

- [ ] **Step 3: Implement decisions 1 and 2.** Document `Batch` in the header: the thread rule, the savepoint join, the refusals, the close rule and the `revision()` note. Update SDS 03 §3.10a and SDS 08 to match.

- [ ] **Step 4: Verify**

Run: `./run-tests.sh`
Expected: all pass, every existing storage test unchanged.

- [ ] **Step 5: Commit**

```bash
git add include src tests docs
git commit -m "feat(storage): StageStateWriter::Batch groups a thread's writes into one transaction"
```

### Task 2: `declare` in one transaction

**Files:**
- Modify: `src/workspace/stage_registry.cpp` (`declare`, and `deriveAndPersist` split into derive, persist and adopt).
- Create: `src/workspace/detail/declare_fault.hpp` (decision 4).
- Test: `tests/workspace/test_stage_registry.cpp`.
- Docs:
  - SDS 04 §4.3: replace the "A redeclaration is not one transaction ... it is a follow-up." passage with decisions 3 and 5. Reword ~422 and ~470 per decision 6.
  - `docs/project.md`: in the workspace plan C entry, a resolved note for the single-transaction declare.
  - `docs/traceability.md`.

**Interfaces:**
- Consumes: `StageStateWriter::Batch` (Task 1).
- Produces: `detail::hookNextDeclareMidwayForTesting(void (*)(void*), void*) noexcept`.

- [ ] **Step 1: Write the failing tests**
  - **`StageRegistry.RefreshMidRedeclareKeepsReaderAndWriterInAgreement`:**
    - **Setup:**
      - Declare `u` writing layer L and `x` reading L, so the stored row x → u exists.
      - Corrupt both stored configurations through `store.stageStateWriter().setConfiguration(id, "not json")`, as `UnreadableRecordIsKeptByNameAndNeverRunnable` does.
      - Reopen a writer registry; it restores both as unreadable, with the pin x → u.
      - Open a read-only `Store` and registry on the same file.
    - **The redeclare:** set the hook to call the reader's `dependencyCount(x)`, which forces a refresh. Then redeclare `x` with a definition that reads no layer.
    - **After it**, for x the reader and the writer agree:
      - `dependencyCount(x)` is 0 on both;
      - `runBlocker(x)` is equal on both.
    - Under the old code the reader keeps the pin.
  - **`StageRegistry.FailedRedeclareChangesNothing`:**
    - Declare `a`, and `b` reading `a`'s layer. Mark `a` complete through `setStatus`.
    - Set a hook that throws `IoError("injected")`, then redeclare `a` with a changed parameter. `declare` throws `IoError`.
    - Afterwards:
      - `a`'s stored configuration is the old one, and neither `a` nor `b` is stale;
      - the dependency rows are unchanged;
      - `registry.definition(a)` is the old definition, and `dependencyCount(b) == 1`;
      - a reopened registry agrees.
  - **`StageRegistry.FailedDeclareOfANewStageLeavesNoRow`:**
    - A throwing hook, then declare new stage `n`: `declare` throws.
    - `count()` is unchanged and `find("n")` is false.
    - Declaring `n` again without a hook succeeds.

- [ ] **Step 2: Run them to verify they fail**

Run: `ctest --test-dir build -C Release -R "StageRegistry\.(RefreshMid|FailedRedeclare|FailedDeclareOfANew)"`
Expected:
- First, compile failure: the hook does not exist.
- After a hook-only stub, placed where decision 4 puts it but without the batch, all three fail: the reader keeps the pin; the configuration or stale flag is already written; the stage row exists.

- [ ] **Step 3: Implement decisions 3 and 4, and update the docs.**

- [ ] **Step 4: Verify**

Run: `./run-tests.sh` and `cmake --build build --target docs --config Release`
Expected: all tests pass, including every existing `StageRegistry.*`, `StageRunner.*` and `StalenessGraph.*` test unchanged; zero Doxygen warnings.

- [ ] **Step 5: Commit**

```bash
git add src tests docs
git commit -m "fix(workspace): declare writes its rows in one transaction; a failed declare changes nothing"
```
