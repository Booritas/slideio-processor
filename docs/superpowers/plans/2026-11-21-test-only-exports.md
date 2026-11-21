# Test-Only Exports Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The six engine functions exported only so tests can call them are exported only when `SLIDEIO_PROCESSOR_TEST_SEAMS` is ON, and `scripts/check-no-test-seams.sh` fails if any of them is exported.

**Architecture:**
- **The six functions.** They are declared with `SLIDEIO_PROCESSOR_ENGINE_EXPORTS`:
  - `SeamMergerAccess::notifyTileSkipped`, `takeAwaitedDeferred` and `completeDeferred` (`src/pipeline/detail/seam_merger_access.hpp`);
  - `detail::stageRecordJson`, `stageFromRecord` and `sameDeclaration` (`src/workspace/detail/stage_record.hpp`).

  All their production callers are inside the engine library, so they need no export for production. They do not contain "Testing", so the check cannot see them.
- **The macro.** A new src-only macro, `SLIDEIO_PROCESSOR_ENGINE_TEST_EXPORTS`:
  - it expands to `SLIDEIO_PROCESSOR_ENGINE_EXPORTS` when `SLIDEIO_PROCESSOR_TEST_SEAMS` is defined;
  - otherwise it expands to nothing, so the function has hidden visibility and stays callable inside the engine.

  The six declarations use it.
- **The check.** The script fails on these six names as well as on "Testing".

**Tech Stack:** C++17, CMake, bash.

**Spec:**
- `docs/superpowers/specs/sds/00-conventions.md` C-15, "Test seams and release builds".
- The final-review follow-up of `docs/superpowers/plans/2026-11-21-test-seams-option.md`.
- UR-BLD-2.

**Base:** branch `build/test-only-exports` from `main` (256e9da).

## Global Constraints

- **Commands:**
  - Build: `./build.sh --skip-slideio`.
  - Tests: `./run-tests.sh`; all must pass.
  - No-seams build: `./build.sh --skip-slideio --no-test-seams`.
  - Check: `scripts/check-no-test-seams.sh <build dir>`.
  - Docs: `cmake --build build --target docs --config Release`.
- Zero compiler warnings and zero Doxygen warnings, with the option ON and with it OFF.
- **Option ON:** nothing changes. The six functions stay exported and all 1181 tests pass unedited.
- **Option OFF:** none of the six is exported, and none of the seven `Testing` seams. The engine still builds and links: every production caller is in the engine.
- **No public header changes.** The macro lives under `src/`.
- Every new file starts with its `// Requirements: UR-BLD-2` line (`#` for bash).

## Decisions

1. **The macro.** Create `src/pipeline/detail/engine_test_export.hpp` (`#pragma once`; Requirements line). It includes `<slideio/processor/pipeline/export.hpp>` and defines `SLIDEIO_PROCESSOR_ENGINE_TEST_EXPORTS`:
   - with `SLIDEIO_PROCESSOR_TEST_SEAMS` defined: `SLIDEIO_PROCESSOR_ENGINE_EXPORTS`;
   - otherwise: empty.

   A comment says it is for detail functions that production calls only inside the engine, and that only tests call across the library boundary. `src/workspace/detail/stage_record.hpp` includes it too, as the workspace sources already include pipeline detail headers. If the include path does not allow that, place the header where both can include it, and say where in the report.
2. **The six declarations** switch from `SLIDEIO_PROCESSOR_ENGINE_EXPORTS` to `SLIDEIO_PROCESSOR_ENGINE_TEST_EXPORTS`. Nothing else in either header changes.
   - First confirm no code outside the engine calls them. The data library and the algorithm object code linked elsewhere must not: grep `src/` for the six names, and list each caller and the target it builds into in the report.
   - If one has a production caller outside the engine, leave it exported and say so.
3. **The script.**
   - `scripts/check-no-test-seams.sh` fails on any exported symbol containing `Testing`, or any of `notifyTileSkipped`, `takeAwaitedDeferred`, `completeDeferred`, `stageRecordJson`, `stageFromRecord` or `sameDeclaration`. Use one `grep -E` pattern built from a named array near the top of the script, with a comment saying why each name is listed.
   - Exit codes and the success text are unchanged.
   - The header comment names both kinds of match.
4. **Docs.**
   - SDS 00 C-15 lists the six functions and the macro: they are test-only exports, hidden when the option is off.
   - README's "Release builds without test seams" says the check also covers them.
   - In `docs/project.md`, the test-seams "Resolved" bullet gains one sentence.

## Review Focus

1. The OFF build links. A hidden function called only inside the engine must still resolve, so the build must succeed with zero warnings (Task 1, Step 4).
2. The extended check catches the six. Run against the OFF build before the change, it fails and lists exactly the six (Task 1, Step 1).
3. The ON build and tests are unchanged: all 1181 tests pass (Task 1, Step 3).
4. No production caller outside the engine depends on one of the six (decision 2's grep, in the report).
5. A pattern name cannot false-match an unrelated exported symbol in either library. Check the ON build's full export listing for any other symbol that contains one of the six names, and record it in the report.

---

### Task 1: Hide the test-only exports and extend the check

**Files:**
- Create: `src/pipeline/detail/engine_test_export.hpp`.
- Modify: `src/pipeline/detail/seam_merger_access.hpp`, `src/workspace/detail/stage_record.hpp`, `scripts/check-no-test-seams.sh`, `docs/superpowers/specs/sds/00-conventions.md` (C-15), `README.md`, `docs/project.md`.

**Interfaces:**
- Produces: the macro `SLIDEIO_PROCESSOR_ENGINE_TEST_EXPORTS`.

- [ ] **Step 1: Extend the script first** (decision 3). Run it against the existing no-seams build, building it first if `build-noseams/` is missing.

Run: `scripts/check-no-test-seams.sh build-noseams`
Expected: exit 1, listing exactly the six names. Record the output in the report.

- [ ] **Step 2: Implement decisions 1 and 2**, with decision 2's grep first.

- [ ] **Step 3: Verify the default build**

Run: `./build.sh --skip-slideio` then `./run-tests.sh`
Expected: zero warnings, all 1181 tests pass. Then `scripts/check-no-test-seams.sh build`: exit 1, listing the seven `Testing` seams and the six. Record the output.

- [ ] **Step 4: Verify the OFF build**

Run: `./build.sh --skip-slideio --no-test-seams` then `scripts/check-no-test-seams.sh build-noseams`
Expected: zero warnings; `no test seams exported`, exit 0.

- [ ] **Step 5: Docs** (decision 4), then the docs target. Zero Doxygen warnings.

- [ ] **Step 6: Commit**

```bash
git add src scripts docs README.md
git commit -m "build: export the six test-only engine functions only with test seams on; the check covers them"
```
