# Test Seams Option Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A CMake option, `SLIDEIO_PROCESSOR_TEST_SEAMS`, decides whether the shared libraries contain and export their test-only seams. A build with it off ships no seam symbol and no seam code path, and CI proves it.

**Architecture:**
- **The seams today.** Seven test-only functions are exported from the release libraries:
  - from the engine library, six `detail::*ForTesting` functions, declared in four headers under `src/workspace/detail/`;
  - from the data library, `detail::trackForCloseTesting`, declared, and befriended by `Store`, in the public `include/slideio/processor/storage/store.hpp`.
- **The fix.**
  - The option defaults to ON, so development builds and `./run-tests.sh` behave as today.
  - When it is ON, the top-level CMake adds `SLIDEIO_PROCESSOR_TEST_SEAMS=1` to every target in the build tree.
  - When it is OFF, the declarations, the definitions, and the production branches that consult the seams are compiled out. `tests/` is not built.
  - `build.sh --no-test-seams` builds that configuration into its own directory.
  - A script checks a built library's export table.
  - A CI step builds with the option off on Ubuntu and runs the check.

**Tech Stack:** CMake 3.21, C++17, MSVC, GCC and Clang, GitHub Actions, bash.

**Spec:**
- The workspace plan B follow-up, "gate the test seams (now five) behind a CMake option" (memory `workspace-b-followups`, `docs/project.md`).
- The seam headers' own comments: "Test seams, not API".
- UR-BLD-* (the build requirements: `docs/superpowers/specs/sds/07-*.md`, or wherever `grep -rn "UR-BLD" docs/superpowers/specs` finds the build section).

**Base:** branch `build/test-seams-option` from `main` (1a51137).

## Global Constraints

- **Commands:**
  - Build: `./build.sh --skip-slideio`.
  - Tests: `./run-tests.sh`; all must pass.
  - Docs: `cmake --build build --target docs --config Release`.
- Zero compiler warnings and zero Doxygen warnings, with the option ON and with it OFF.
- **Option ON (default):** nothing observable changes. The same 1181 tests pass, with no test edited except as decision 4 requires.
- **Option OFF:**
  - `tests/` is not added, and GTest is not required.
  - The libraries build.
  - No exported symbol of either shared library contains `ForTesting`.
- **Public header:** the only change is `store.hpp`'s `trackForCloseTesting` declaration and friend, wrapped in `#if defined(SLIDEIO_PROCESSOR_TEST_SEAMS)`. Consumers outside the build tree never define the macro, so they never see the seam. Friend declarations do not change the class layout, so ABI is unaffected.
- **No other behaviour change:** a seam that is never armed behaves exactly like a seam compiled out.
- Messages carry no pixel, coordinate or parameter values.
- Every new source or script file starts with its `Requirements:` line (`#` comment for CMake and bash).

## Decisions

1. **The option.** In the top-level `CMakeLists.txt`:
   - `option(SLIDEIO_PROCESSOR_TEST_SEAMS "Build and export the test-only seams the tests need; turn off for release packages" ON)`.
   - When ON: `add_compile_definitions(SLIDEIO_PROCESSOR_TEST_SEAMS=1)`, `find_package(GTest REQUIRED)`, `add_subdirectory(tests)`.
   - When OFF: none of the three, and `message(STATUS "slideio-processor: test seams off; tests are not built")`.
   - `enable_testing()` stays unconditional.
   - `add_compile_definitions` is directory-scoped, so nothing reaches an installed or exported package configuration.
2. **What is gated.** Each item below is wrapped in `#if defined(SLIDEIO_PROCESSOR_TEST_SEAMS)` ... `#endif`:
   - every declaration in `src/workspace/detail/{create_fault,declare_fault,refresh_fault,run_fault}.hpp`;
   - each matching definition in `workspace.cpp`, `stage_registry.cpp` and `stage_runner.cpp`;
   - the globals they set;
   - every production branch that reads those globals, including `StageRunner`'s `injectFault` member and its uses;
   - `trackForCloseTesting`'s declaration and definition in `src/storage/detail/store_impl.{hpp,cpp}` (or wherever it is defined), and its declaration and friend in `store.hpp`.

   With the macro undefined, the code reads as if the seam had never been armed. Before starting, grep `src/` and `include/` for `SLIDEIO_PROCESSOR_(DATA|ENGINE)_EXPORTS` under any `detail` namespace:
   - gate any other export that only tests use, and list it in the report;
   - do not gate an export the other shared library uses, such as `level_mapping`'s functions or `StoreImpl` helpers.
3. **Not gated.** These internals are compiled into the test executables directly, not exported:
   - `WriterLock::acquire(path, afterOpen)`;
   - `TiffRasterReader`'s `openHook_`;
   - `TileSpool::piecesStored` and `piecesDecoded`.

   Leave them, and say so in one comment line near the option.
4. **Tests.** No test changes, unless a test file compiles in the library because of a seam header's location. Every test still sees the seams, because `add_compile_definitions` reaches the test targets.
5. **`build.sh`.**
   - **The flag.** `--no-test-seams` configures `-DSLIDEIO_PROCESSOR_TEST_SEAMS=OFF` into `build-noseams` (release) or `build-noseams-debug` (debug).
   - **Usage text:** "Build without the test-only seams, as for a release package; tests are not built."
   - **Final hint:** the line suggesting `./run-tests.sh` is replaced by one suggesting `scripts/check-no-test-seams.sh <dir>`.
   - **Default:** the default build is unchanged.
6. **`scripts/check-no-test-seams.sh <build dir>`** (bash, executable, `set -euo pipefail`).
   - It finds the two shared libraries under `<build dir>/bin` (`.so`, `.dylib` or `.dll`) and lists their exported symbols, by platform:
     - Linux: `nm -D --defined-only`;
     - macOS: `nm -gU`;
     - Windows: `dumpbin /exports`, or `llvm-nm`/`nm` when `dumpbin` is not on `PATH`. If no tool is found, exit 2 with a message.
   - It fails (exit 1) when any symbol contains `ForTesting`, naming the library and the symbol. It prints `no test seams exported` on success.
   - Exit 2 also when a library is missing.
7. **CI.** In `.github/workflows/build-validation.yml`, add to the ubuntu-22.04 matrix entry only (a matrix `include` flag, or an `if: matrix.os == 'ubuntu-22.04'` step after Test):
   - `./build.sh --skip-slideio --no-test-seams`;
   - `scripts/check-no-test-seams.sh build-noseams`.

   It reuses the Conan cache and SlideIO install from the main build.
8. **Docs.**
   - README build section: the flag and when to use it.
   - The SDS build section (decision 2's list, and why it matters for a release package).
   - `docs/project.md`: the plan B follow-up is marked resolved.
   - Memory is updated by the controller, not the implementer.

## Review Focus

1. A default build is byte-for-byte the same in behaviour: the full suite passes unchanged (Task 1).
2. An OFF build compiles with zero warnings (no unused-variable or unused-function warnings from half-gated code), and exports nothing containing `ForTesting`. This is checked locally on Windows with the script (Task 1) and on Linux in CI (Task 2).
3. Nothing in the OFF build still reads a seam global. Grep of the OFF preprocessed sources, or reading the diff: every reader is inside the same `#if` as its global (Task 1).
4. A consumer that includes `store.hpp` without the macro compiles. The OFF build compiles the library's own includes of it, which proves this (Task 1).
5. The script fails when a seam is exported. Run it once against the default (ON) build and record its failure output in the report (Task 1).

---

### Task 1: The option, the gating, `build.sh` and the check script

**Files:**
- Modify:
  - `CMakeLists.txt`;
  - `src/workspace/detail/{create_fault,declare_fault,refresh_fault,run_fault}.hpp`;
  - `src/workspace/{workspace,stage_registry,stage_runner}.cpp`;
  - `src/storage/detail/store_impl.{hpp,cpp}` (and the `.cpp` defining `trackForCloseTesting`, if another);
  - `include/slideio/processor/storage/store.hpp`;
  - `build.sh`;
  - README;
  - the SDS build section;
  - `docs/project.md`.
- Create: `scripts/check-no-test-seams.sh`.

**Interfaces:**
- Produces:
  - the CMake option `SLIDEIO_PROCESSOR_TEST_SEAMS` (default ON);
  - the macro `SLIDEIO_PROCESSOR_TEST_SEAMS`;
  - `build.sh --no-test-seams`, which writes `build-noseams`;
  - `scripts/check-no-test-seams.sh <build dir>`, with exits 0, 1 and 2 per decision 6.

- [ ] **Step 1: Write the check script** (decision 6) and run it against the current default build.

Run: `scripts/check-no-test-seams.sh build`
Expected: exit 1, listing the seven `*ForTesting` exports (engine six, data one). Record the output in the report.

- [ ] **Step 2: Implement decisions 1, 2, 3 and 5.**

- [ ] **Step 3: Verify the default build is unchanged**

Run: `./build.sh --skip-slideio` then `./run-tests.sh`
Expected: zero warnings, all 1181 tests pass.

- [ ] **Step 4: Verify the OFF build**

Run: `./build.sh --skip-slideio --no-test-seams` then `scripts/check-no-test-seams.sh build-noseams`
Expected: zero warnings, `tests/` not configured (the status message appears), the script prints `no test seams exported` and exits 0.

- [ ] **Step 5: Docs** (decision 8, except CI), then the docs target. Zero Doxygen warnings.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt src include build.sh scripts README.md docs
git commit -m "build: SLIDEIO_PROCESSOR_TEST_SEAMS option compiles the test-only seams out of release builds"
```

### Task 2: CI builds without seams and checks the exports

**Files:**
- Modify: `.github/workflows/build-validation.yml`; the SDS build section and README (one line on the CI check).

**Interfaces:**
- Consumes: `build.sh --no-test-seams`, `scripts/check-no-test-seams.sh` (Task 1).

- [ ] **Step 1: Add the step** (decision 7).

- [ ] **Step 2: Validate the workflow.** If `actionlint` is available, run `actionlint .github/workflows/build-validation.yml` and expect no errors. If it is not, check the YAML parses (`python -c "import yaml,sys; yaml.safe_load(open(sys.argv[1]))" .github/workflows/build-validation.yml`), and say in the report that the step's real run happens in CI after push.

- [ ] **Step 3: Commit**

```bash
git add .github/workflows/build-validation.yml README.md docs
git commit -m "ci: build without test seams on Ubuntu and check the exports"
```
