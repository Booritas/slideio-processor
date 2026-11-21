# Stale-Since Record Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The store records when each stage's stale flag was set, so that `StalenessGraph`'s causes name the source stage exactly for a stage that never completed. Today such a stage only gets "redeclared or marked stale directly, or a source stage re-ran".

**Architecture:**
- **Storage, format version 2.** The `stages` table gains `stale_since_ms`:
  - it is set when the stale flag goes from clear to set;
  - it is kept while the flag stays set;
  - it is cleared with the flag.
- **A new public reader call,** `StageStateReader::staleSinceUnixMs`, returns that value.
- **Migration.** A writable open of a version 1 store upgrades it in place, in one durable transaction. A read-only open of a version 1 store reads it as it is, with `staleSinceUnixMs` 0.
- **The cause rule.** `StalenessGraph`'s reverse answer compares a `Complete` source's completion time with the stage's stale-since time when the stage never completed.

**Tech Stack:** C++17, CMake, SQLite, GoogleTest, MSVC.

**Spec:**
- `docs/superpowers/specs/sds/03-storage.md`: §3.10, the stage state reader and writer; §3.12, schema and format.
- `docs/superpowers/specs/sds/04-workspace.md` §4.5: "The reverse answer" (~1293-1325) and "Cached answers".
- `include/slideio/processor/storage/store_format.hpp`, which holds the frozen-v1 rule.
- Requirements: UR-RES-12 (format versioning), UR-STG-8, UR-STG-11.

**Base:** branch `feat/stale-since` from `main` (61fcab2).

## Global Constraints

- **Commands:**
  - Build: `./build.sh --skip-slideio`.
  - Tests: `./run-tests.sh`; all must pass.
  - One test: `ctest --test-dir build -C Release -R <name>`.
  - Docs: `cmake --build build --target docs --config Release`.
  - Disabled tests: `PATH=build/slideio-install/release/bin:build/bin/Release:$PATH build/bin/Release/<test exe> --gtest_also_run_disabled_tests --gtest_filter=<name>`.
- Zero compiler warnings and zero Doxygen warnings.
- **The v1 fixtures are never regenerated:** `tests/storage/data/store-v1.schema.sql` and `store-v1.golden.store` with its `-rasters` directory. Version 2 gets `store-v2.*` fixtures of its own, written by a generator. The v1 golden store is the input the migration is tested on.
- `storeFormatVersion()` returns 2. Messages never carry pixel, coordinate or parameter values.
- **Public API changes:**
  - Allowed: `StageStateReader::staleSinceUnixMs`, and the documentation of `storeFormatVersion`, `StageStateWriter::setStale` and the `StalenessGraph` cause accessors.
  - Not allowed: anything else in a public header.
- **Unchanged behaviour:** every existing test passes, apart from those this plan names as changed. For a stage that completed, every cause answer is unchanged.
- **Requirements lines and traceability:**
  - Every new source and test file starts with its `// Requirements: ...` line.
  - `docs/traceability.md` lists every new or renamed test, updated in place.
- Match the surrounding code's idiom and comment density.

## Decisions

1. **The column.** `stale_since_ms INTEGER NOT NULL DEFAULT 0` is the last column of `stages`.
   - The fresh-store DDL declares it with exactly the text `ALTER TABLE stages ADD COLUMN stale_since_ms INTEGER NOT NULL DEFAULT 0` appends. So a migrated store and a fresh one have identical `sqlite_master` SQL, and the v2 snapshot test proves it.
   - 0 means unknown, or not stale.
2. **`setStale` semantics.**
   - `setStale(stage, true)` sets `stale = 1`, and sets `stale_since_ms` to the current system time in milliseconds (the clock `setStatus` uses for `completed_at_ms`) only when `stale` was 0. A flag already set keeps its time.
   - `setStale(stage, false)` sets both to 0.
   - One `UPDATE`; no other write changes either column. `revision()` behaves as today.
3. **Reader.** Add `int64_t StageStateReader::staleSinceUnixMs(StageId stage) const`.
   - It returns 0 for a stage that is not stale, and 0 on a version 1 store opened read-only.
   - An unknown stage throws `InvalidArgumentError`, as `completedAtUnixMs` does.
   - SDS 03 §3.10 and SDS 08's `StageStateReader` box list it.
4. **Format check and migration** in `StoreImpl`:
   - **Accepted versions.** A stored version of 1 or 2 is accepted. Anything else is a `StoreFormatError` as today, with library version 2. Its message says this library reads version 2 and upgrades version 1.
   - **Writable open of version 1.**
     - After the version check, and before anything else uses the store, one durable write transaction runs the `ALTER TABLE` and sets `meta.format_version` to 2.
     - `Store::formatVersion()` then reports 2.
     - If the transaction fails, the store is left at version 1 and the open throws `IoError`.
   - **Read-only open of version 1** changes nothing. `formatVersion()` reports 1, and `StageStateReader` runs its queries without the column (`staleSinceUnixMs` returns 0).
   - **Writers on a version 1 store.** None can exist after a writable open, because the open migrates it.
   - **Documentation.** SDS 03 §3.12 gains a "Version 2" paragraph: the column, the migration, read-only version 1, and that older libraries refuse a migrated store. The `storeFormatVersion` doc in `store_format.hpp` says version 2 is current, version 1 is frozen and migrated, and states the fixture rule for both.
5. **Fixtures.**
   - **Shared helpers.** Move the golden writer and checker, now in `tests/storage/test_store_format_v1.cpp`, into a shared `tests/storage/store_format_golden.{hpp,cpp}`. The v1 and v2 tests both use them; nothing is copied.
   - **v2 generator.** `StoreFormatV2.DISABLED_GenerateFixtures` writes `store-v2.schema.sql` and `store-v2.golden.store` (with its `-rasters` directory). Its content is the v1 golden content, plus one stage marked stale, so its `staleSinceUnixMs` is non-zero.
   - Run the generator once and commit the fixtures.
   - **v1 generator.** `StoreFormatV1.DISABLED_GenerateFixtures` is deleted. The library can no longer write version 1, and a comment at the top of the v1 file says so.
6. **The cause rule** in `src/workspace/staleness_graph_impl.hpp`:
   - **`changedSince`** takes the stage's completion time `completed` and its stale-since time `since`. A `Complete` source U counts when:
     - `completed != 0` and `completedAtUnixMs(U) > completed`, as today; or
     - `completed == 0`, `since != 0` and `completedAtUnixMs(U) >= since`.

     `>=` because a source's re-run marks its dependents and then completes. In the same millisecond the re-run is the more likely history, and naming the source loses nothing.
   - **The self cause** has detail `the stage was redeclared or marked stale directly` when the stage completed or `since != 0`. The detail `the stage has not completed: it was redeclared or marked stale directly, or a source stage re-ran` remains only for `completed == 0 && since == 0`: a flag set before the migration.
   - **Other sources** (`Running`, `Failed`, stale) are unchanged.
   - **Cache key.** The causes cache key needs no change: `setStale` already advances `revision()`.
   - **Docs.** Update the `causeCount` and `causeReasonDetailAt` docs, and SDS 04 §4.5 "The reverse answer", which removes "the store keeps no time for when a stale flag was set".
7. **QA/regulatory note** in `docs/project.md`:
   - Store format version 2.
   - A version 1 store is upgraded in place on its first writable open by this library. Older library versions then refuse it. Read-only opens leave it at version 1.
   - Causes reported for stages that never completed change from the ambiguous detail to a named source or the plain "redeclared" detail, except for flags set before the upgrade.
   - Archive a copy of a version 1 store before opening it writable if it must stay readable by an older release.
   - The workspace plan C entry's follow-up "durable stale-flag provenance" is marked resolved.

## Review Focus

1. A version 1 store opened writable is migrated once and fully. Its schema equals a fresh version 2 store's, and its content reads back (`StoreFormatV2.MigratedV1SchemaMatchesTheV2Snapshot`, Task 1).
2. A version 1 store opened read-only is not modified, and still reads, with stale-since 0 (`StoreFormatV1.GoldenStoreOpensReadOnlyWithoutMigrating`, Task 1).
3. Marking an already-stale stage again keeps its first stale-since time (`StageState.StaleSinceIsSetOnceAndClearedWithTheFlag`, Task 1).
4. A never-completed stage marked by a source's re-run that then completed names that source (`StalenessGraph.NeverCompletedStageMarkedByASourceReRunNamesTheSource`, Task 2).
5. A stale flag that predates the migration keeps the cautious detail (`StalenessGraph.StaleSinceUnknownKeepsTheCautiousDetail`, Task 2).

---

### Task 1: Store format version 2

**Files:**
- Modify:
  - `src/storage/detail/schema.cpp` (DDL);
  - `src/storage/detail/store_impl.{hpp,cpp}` (version check, migration);
  - `src/storage/stage_state_writer.cpp` (`setStale`);
  - `src/storage/stage_state_reader.cpp` and `include/slideio/processor/storage/stage_state_reader.hpp` (`staleSinceUnixMs`);
  - `src/storage/store_format.cpp` and `include/slideio/processor/storage/store_format.hpp`.
- Create: `tests/storage/store_format_golden.{hpp,cpp}`, `tests/storage/test_store_format_v2.cpp`, and the `tests/storage/data/store-v2.*` fixtures. Register them in `tests/storage/CMakeLists.txt`.
- Modify tests: `tests/storage/test_store_format_v1.cpp`, `tests/storage/test_stage_state.cpp`, and the test asserting the format version is 1 (`StorageTypes.StoreFormatVersionIsOne` in `tests/storage/test_types.cpp`; rename it `StoreFormatVersionIsTwo`).
- Docs: SDS 03 §3.10 and §3.12, SDS 08 (`StageStateReader`), `docs/traceability.md`.

**Interfaces:**
- Produces: `int64_t StageStateReader::staleSinceUnixMs(StageId stage) const;`, `storeFormatVersion() == 2`, and the `setStale` semantics of decision 2.

- [ ] **Step 1: Write the failing tests**
  - **`StageState.StaleSinceIsSetOnceAndClearedWithTheFlag`:**
    - Declare a stage. `staleSinceUnixMs` is 0.
    - Take `before` as the current milliseconds, then `setStale(s, true)`. The value `t` satisfies `t >= before`.
    - Sleep 20 ms and `setStale(s, true)` again: still `t`.
    - `setStale(s, false)`: 0.
    - `setStale(s, true)` again after a 20 ms sleep: greater than `t`.
    - An unknown stage throws `InvalidArgumentError`.
  - **`StoreFormatV2.SchemaMatchesTheFrozenSnapshot`:** a fresh store's schema dump equals `store-v2.schema.sql`. Same mechanism as v1's test.
  - **`StoreFormatV2.MigratedV1SchemaMatchesTheV2Snapshot`:**
    - Copy the v1 golden store and open it `ReadWrite`. `formatVersion() == 2`.
    - The v1 golden content reads back, and every stage's `staleSinceUnixMs` is 0.
    - Close it. Its schema dump equals `store-v2.schema.sql`. A read-only reopen reports `formatVersion() == 2`.
  - **`StoreFormatV2.GoldenStoreStillOpens`:** the v2 golden store, `ReadOnly` and `ReadWrite`. Its content reads back, including the stale stage's non-zero `staleSinceUnixMs`.
  - **`StoreFormatV1.GoldenStoreOpensReadOnlyWithoutMigrating`:**
    - Copy the v1 golden store and open it `ReadOnly`. `formatVersion() == 1`, the content reads back, and `staleSinceUnixMs` is 0 for every stage.
    - Close it. The copy's bytes are unchanged; compare the main file with the original golden file.
  - **`StoreFormatV1.GoldenStoreSchemaMatchesTheV1Snapshot`:** the v1 golden store's schema dump equals `store-v1.schema.sql`. It replaces `StoreFormatV1.SchemaMatchesTheFrozenSnapshot`, since a fresh store is now v2.
  - Update `StoreFormatV1.GoldenStoreStillOpens` to `ReadOnly` only (the `ReadWrite` path is the migration test), and rename `StorageTypes.StoreFormatVersionIsOne`.

- [ ] **Step 2: Run them to verify they fail**

Run: `ctest --test-dir build -C Release -R "StageState\.StaleSince|StoreFormatV|StorageTypes\."`
Expected: compile failure (`staleSinceUnixMs` does not exist). After a stub, they fail on the version, the snapshot and the stale-since values.

- [ ] **Step 3: Implement decisions 1–5.**
  - Run the v2 generator once:
    `PATH=build/slideio-install/release/bin:build/bin/Release:$PATH build/bin/Release/<storage test exe> --gtest_also_run_disabled_tests --gtest_filter=StoreFormatV2.DISABLED_GenerateFixtures`
  - Commit the generated fixtures. Never run any v1 generator.

- [ ] **Step 4: Verify**

Run: `./run-tests.sh` and the docs target.
Expected: all pass; zero warnings.

- [ ] **Step 5: Commit**

```bash
git add include src tests docs
git commit -m "feat(storage): store format v2 records when a stage's stale flag was set; v1 stores migrate on writable open"
```

### Task 2: Exact causes for stages that never completed

**Files:**
- Modify: `src/workspace/staleness_graph_impl.hpp` (`computeCauses`, `changedSince`, `selfCause`), and `include/slideio/processor/workspace/staleness_graph.hpp` (docs of `causeCount` and `causeReasonDetailAt` only).
- Test: `tests/workspace/test_staleness_graph.cpp` (or the file holding the existing `StalenessGraph.*` cause tests).
- Docs: SDS 04 §4.5, `docs/project.md` (decision 7), `docs/traceability.md`.

**Interfaces:**
- Consumes: `StageStateReader::staleSinceUnixMs` (Task 1).

- [ ] **Step 1: Write the failing tests.** Build the scenarios through `StageRegistry` and `StageStateWriter::setStatus`, as the existing cause tests do.
  - **`StalenessGraph.NeverCompletedStageMarkedByASourceReRunNamesTheSource`:**
    - Declare `u` writing layer L and `s` reading L. Complete `u`.
    - Re-run `u` the way `StageRunner` marks it: dependents stale, then `Running`, then `Complete`. Use `StageRunner` if the existing tests do; otherwise use `markStale` semantics through the registry plus `setStatus`.
    - `s` never completed. `causeCount(s) == 1`, `causeAt(s, 0) == u`, and the reason is `LayerRead`.
    - The old code reports `s` itself, with the ambiguous detail.
  - **`StalenessGraph.NeverCompletedStageRedeclaredAfterTheSourceCompletedIsItsOwnCause`:**
    - Complete `u`, sleep 20 ms, then redeclare `s` with a changed parameter.
    - `causeCount(s) == 1` and `causeAt(s, 0) == s`, with reason `Reconfigured` and detail exactly `the stage was redeclared or marked stale directly`.
  - **`StalenessGraph.StaleSinceUnknownKeepsTheCautiousDetail`:**
    - Mark `s` stale, close the workspace, and set `stale_since_ms = 0` for `s` through raw SQLite on the store file. This stands in for a flag set before the migration.
    - Reopen. The detail is exactly `the stage has not completed: it was redeclared or marked stale directly, or a source stage re-ran`.
  - Existing cause tests for completed stages pass unchanged. An existing test that pins the ambiguous detail for a never-completed stage whose flag is now timed changes to the new expectation; name each one in the report.

- [ ] **Step 2: Run them to verify they fail**

Run: `ctest --test-dir build -C Release -R "StalenessGraph\."`
Expected: the first two fail (self cause with the ambiguous detail); the third passes (guard).

- [ ] **Step 3: Implement decision 6, and add the docs of decisions 6 and 7.**

- [ ] **Step 4: Verify**

Run: `./run-tests.sh` and the docs target.
Expected: all pass; zero Doxygen warnings.

- [ ] **Step 5: Commit**

```bash
git add include src tests docs
git commit -m "feat(workspace): name the source of a never-completed stage's staleness from its stale-since time"
```
