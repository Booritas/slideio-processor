# GCC and Clang Warning Cleanup Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The project's own sources build with zero warnings on every CI compiler (MSVC, GCC 11, GCC 14, Apple Clang), and CI fails on any new warning.

**Architecture:**
- **The warnings.** The CI log of run 44907238158 shows 13 warnings in `src/` and `tests/`. MSVC shows none. Each is fixed at its source, with no pragma or suppression, except where the code deliberately does what the warning is about; there the code is reshaped so the intent stays visible.
- **The option.** `SLIDEIO_PROCESSOR_WARNINGS_AS_ERRORS` (default OFF) adds `-Werror` (GCC/Clang) or `/WX` (MSVC) to our own targets. It is reached through `build.sh --warnings-as-errors`, which every CI job passes.
- **Verification.** GCC and Clang are not available on the development machine (Windows, MSVC). CI on a pushed branch is the verification for those compilers. The controller asks the user before pushing.

**Tech Stack:** C++17, CMake, GCC 11 and 14, Apple Clang, MSVC, GitHub Actions.

**Spec:**
- UR-BLD-1 and UR-BLD-2 (build and pass on Windows, Linux and macOS; C++17 with CMake).
- The project's "zero compiler warnings" rule used in every plan.
- CI log of run 44907238158; the warning list is in decision 1.

**Base:** branch `build/gcc-clang-warnings` from `main` (1613764).

## Global Constraints

- **Commands:**
  - Build: `./build.sh --skip-slideio`.
  - Tests: `./run-tests.sh`; all must pass.
  - Docs: `cmake --build build --target docs --config Release`.
- **No behaviour change.** No production logic changes beyond what a warning fix needs. Every test keeps what it asserts.
- **No warning suppressions:** no `#pragma`, no `-Wno-…`, no attribute-based silencing. The one exception is GCC's `__extension__` keyword on the `__int128` typedef (decision 2), which is the documented way to say a non-ISO type is used on purpose.
- **Werror scope.** The option only adds `-Werror` / `/WX` to our targets, the `src/` and `tests/` targets. It must not reach SlideIO (built separately) or third-party code.
- Messages carry no pixel, coordinate or parameter values.

## Decisions

1. **The warnings to fix**, as file:line on `main` (1613764):
   - `src/storage/detail/polygon_predicates.cpp:39`: `__int128` is not ISO C++ (`-Wpedantic`; GCC 11 and 14).
   - `src/config/detail/stream_log_sink.cpp:47`: `snprintf` output may be truncated (`-Wformat-truncation`; GCC 11 and 14).
   - `src/pipeline/pipeline_spec.cpp:229, 233, 267, 275`: possibly dangling reference to a temporary (`-Wdangling-reference`; GCC 14).
   - `tests/core/test_geometry.cpp:228`: possibly dangling reference (GCC 14).
   - `tests/core/test_raster_mask.cpp:358` and `tests/storage/test_cross_process.cpp:232`: ambiguous `else` (`-Wdangling-else`; GCC).
   - `tests/algorithm/test_measurement_finish.cpp:191`: loop variable binds to a temporary (`-Wrange-loop-construct`; GCC).
   - `tests/algorithm/test_measurement_finish.cpp:310`: self-assignment (`-Wself-assign-overloaded`; Clang).
   - `tests/pipeline/test_lazy_scene.cpp:103` and `tests/pipeline/test_tile_evaluator.cpp:84`: unused private field `height_` (`-Wunused-private-field`; Clang).
2. **The fixes:**
   - **`polygon_predicates.cpp`:** in the `__SIZEOF_INT128__` branch, declare `__extension__ typedef unsigned __int128 Uint128;` and use `Uint128`. The product stays exact, and the MSVC `_umul128` branch and the schoolbook fallback are untouched.
   - **`stream_log_sink.cpp`:** size the buffer for the worst case of every `int` field, and say so in a comment. Every `%d` field is printed in full whatever its value, so `char buffer[96]` fits seven `int` fields of 11 characters each, plus the separators, `Z` and the terminator. Keep `snprintf`.
   - **`pipeline_spec.cpp`:** GCC 14 flags a returned reference when an argument is a temporary. Find the temporary in `member(...)`'s call (for example a `std::string` built from a literal) and remove it: give the helper `const char*` parameters, or otherwise ensure no argument is a temporary. Do not copy the `rapidjson::Value`.
   - **`test_geometry.cpp:228`:** move the braced list of `std::pair`s into a named local array before the loop.
   - **Dangling `else`:** add braces in both test files.
   - **`test_measurement_finish.cpp:191`:** iterate as `const char* name`, converting where a `std::string` is needed.
   - **`test_measurement_finish.cpp:310`:** keep the self-assignment test, through an alias: `MeasurementSelection& same = assigned; assigned = same;`, with a comment that it is deliberate.
   - **Unused `height_`:** remove the field and its initialiser in both fakes, unless a natural use exists. If the constructor takes the height only for that field, keep the parameter, unnamed or `(void)`-free: give it no name.
3. **The option.** In the top-level `CMakeLists.txt`:
   - `option(SLIDEIO_PROCESSOR_WARNINGS_AS_ERRORS "Treat compiler warnings in this project's sources as errors" OFF)`.
   - When ON, add `-Werror` (non-MSVC) or `/WX` (MSVC) to the existing `add_compile_options` block, which already covers only this project's targets.
   - Confirm SlideIO is not built in this CMake tree (it is installed separately and found with `find_package`). Imported targets' include directories count as system headers, so their warnings stay out.
   - If a dependency header still warns under the option, report it rather than suppressing it.
4. **`build.sh --warnings-as-errors`** passes `-DSLIDEIO_PROCESSOR_WARNINGS_AS_ERRORS=ON`, and combines with `--no-test-seams`. Its usage line: "Fail the build on any compiler warning in this project's sources."
5. **CI.** Every build invocation in `.github/workflows/build-validation.yml` passes `--warnings-as-errors`: the matrix `Build` step, the no-seams step, and the manylinux job's `./build.sh`.
6. **Docs.** README build section: the flag, and that CI uses it. SDS 00 conventions: one line next to C-15, or the existing warnings convention if there is one (grep `warning`), stating that CI builds with warnings as errors on all four compilers.

## Review Focus

1. Each fix keeps the test's assertion and the production behaviour: the full suite passes on Windows (Task 1).
2. `__extension__` compiles cleanly on Clang and GCC with `-Wpedantic`, and the 128-bit product is still exact. The existing polygon predicate tests with products past 2^64 run on Linux and macOS in CI (Task 1, then Task 2's CI run).
3. The option does not reach SlideIO or dependency code: a Windows build with `--warnings-as-errors` succeeds locally (Task 2).
4. The first CI run with `--warnings-as-errors` passes on all five jobs. If a compiler still warns, the job fails and names the line (Task 2).
5. Nothing else in `src/` or `tests/` warns on those compilers. CI with `-Werror` proves it (Task 2).

---

### Task 1: Fix the 13 warnings

**Files:**
- Modify: the files in decision 1.

- [ ] **Step 1: Apply decision 2's fixes.**

- [ ] **Step 2: Verify on Windows**

Run: `./build.sh --skip-slideio` then `./run-tests.sh`
Expected: zero MSVC warnings, all 1181 tests pass.

- [ ] **Step 3: Check for WSL or Docker.** If `wsl -l -q` lists a distribution with GCC, or `docker` runs, build with GCC there and record the warning count for `src/` and `tests/` (expected 0). Otherwise record that GCC and Clang are verified by Task 2's CI run.

- [ ] **Step 4: Commit**

```bash
git add src tests
git commit -m "build: fix the GCC and Clang warnings in src and tests"
```

### Task 2: Warnings as errors in CI

**Files:**
- Modify: `CMakeLists.txt`, `build.sh`, `.github/workflows/build-validation.yml`, `README.md`, `docs/superpowers/specs/sds/00-conventions.md`.

**Interfaces:**
- Produces: the CMake option `SLIDEIO_PROCESSOR_WARNINGS_AS_ERRORS` and `build.sh --warnings-as-errors`.

- [ ] **Step 1: Implement decisions 3 and 4.**

- [ ] **Step 2: Verify on Windows**

Run: `./build.sh --skip-slideio --warnings-as-errors` then `./run-tests.sh`, and `./build.sh --skip-slideio --no-test-seams --warnings-as-errors`.
Expected: both builds succeed; all tests pass.

- [ ] **Step 3: Implement decisions 5 and 6.** Validate the workflow YAML parses.

- [ ] **Step 4: Commit**

```bash
git add CMakeLists.txt build.sh .github/workflows/build-validation.yml README.md docs
git commit -m "build: SLIDEIO_PROCESSOR_WARNINGS_AS_ERRORS; CI fails on any warning"
```

- [ ] **Step 5: CI proof (controller).** After the final review, and with the user's OK, push the branch. Run the workflow on it with `gh workflow run build-validation.yml --ref build/gcc-clang-warnings`, if the workflow has `workflow_dispatch`; otherwise open a PR. Confirm all five jobs pass before merge. If a job fails on a remaining warning, the fix goes back to Task 1's implementer as a fix round.
