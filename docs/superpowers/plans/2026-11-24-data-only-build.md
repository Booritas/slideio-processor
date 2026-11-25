# Data-Only Build Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A `SLIDEIO_PROCESSOR_DATA_ONLY` CMake option that builds and tests only the `slideio-processor-data` shared library, without SlideIO or OpenCV, so slideio-view can link it through `add_subdirectory`.

**Architecture:** One option in the top-level `CMakeLists.txt` gates every SlideIO/OpenCV `find_package`, the engine-side `src/` modules and their configure checks, and the engine-side test directories. The OpenCV version the data library reports comes from the conanfile pin. A tiny consumer project proves the `add_subdirectory` route; a CI job keeps both green.

**Tech Stack:** CMake ≥ 3.21, Conan 2, GoogleTest, bash, GitHub Actions.

**Spec:** `D:/Projects/slideio/slideio-view/docs/superpowers/specs/2026-11-24-processing-results-overlay-design.md` §4 (the consumer is the viewer; this plan is its prerequisite).

**Branch:** `feat/data-only-build` in slideio-processor (from `main` 52072da).

## Global Constraints

- Option name `SLIDEIO_PROCESSOR_DATA_ONLY`, default `OFF`. With it `OFF`, the build, the targets and the tests are exactly as today (the Python bindings consume the full library through `add_subdirectory`).
- With it `ON`: no `find_package(slideio …)`, no `find_package(OpenCV …)`; only `src/core`, `src/config`, `src/storage`, `src/reporting` and the `slideio-processor-data` target are configured; the configure-time guards on `slideio-processor-data` and its modules (no OpenCV, no `slideio::`) still run.
- `BackendCapabilities` in data-only mode reports name `"OpenCV"` and the version of the `requires("opencv/x.y.z")` line in `conanfile.py`, parsed at configure time, the same regex `tests/config/CMakeLists.txt` uses.
- Data-only with test seams `ON` builds and runs the tests of `tests/core`, `tests/config`, `tests/storage`, `tests/reporting`, except `tests/core/test_data_type.cpp` (it includes `<slideio/core/slideio_enums.hpp>`).
- Warnings as errors must pass in data-only mode (CI uses `--warnings-as-errors`).
- Commits are human-reviewed before push; push and merge only on the owner's instruction.

## Review Focus

1. A consumer that sets the option before `add_subdirectory` but has no OpenCV/SlideIO installed must configure: the consumer project (Task 2) has neither on its prefix path.
2. A consumer with test seams `OFF` must not need GTest: the consumer project configures with seams off.
3. The full build (option `OFF`) must be unchanged: Task 1 Step 6 runs the full suite.
4. The OpenCV version baked in data-only mode must equal the conanfile pin, not a hard-coded string: `config.BackendCapabilities.*` tests already compare against the pin and must pass in data-only mode.
5. A data-only build directory must not be confused with a full one by `run-tests.sh`: its own build directory name (Task 1 Step 3).

---

### Task 1: The option, its build script flag and its tests

**Files:**
- Modify: `CMakeLists.txt` (top level)
- Modify: `src/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`, `tests/core/CMakeLists.txt`
- Modify: `conanfile.py`
- Modify: `build.sh`, `run-tests.sh`

**Interfaces:**
- Produces: CMake cache option `SLIDEIO_PROCESSOR_DATA_ONLY`; `build.sh --data-only` writing `build-data` (`build-data-debug`); `run-tests.sh --data-only` running it; Conan option `data_only` (bool, default False) that drops the `opencv` requirement.

- [ ] **Step 1: Write the check that fails today**

Run, from a shell with Conan on PATH (no SlideIO prefix given):
```bash
conan install . -of build-data/conan -s build_type=Release -s compiler.cppstd=17 --build=missing
cmake -S . -B build-data -DCMAKE_TOOLCHAIN_FILE=build-data/conan/conan_toolchain.cmake \
      -DCMAKE_BUILD_TYPE=Release -DSLIDEIO_PROCESSOR_DATA_ONLY=ON
```
Expected: FAIL at `find_package(slideio 2.11 REQUIRED)` (and CMake warns the option is unused).

- [ ] **Step 2: Implement the option**

- Top-level `CMakeLists.txt`: declare the option next to `SLIDEIO_PROCESSOR_TEST_SEAMS`. When ON, skip `find_package(slideio …)` and `find_package(OpenCV …)`, and instead set `OpenCV_VERSION` and `OpenCV_VERSION_MAJOR` from the conanfile pin (the regex `requires\("opencv/[0-9.]+"\)` and the two `string(REGEX MATCH …)` lines of `tests/config/CMakeLists.txt`), failing the configure with a message naming `conanfile.py` when no pin is found. Print `slideio-processor: data-only build (no SlideIO, no OpenCV)`.
- `src/CMakeLists.txt`: when ON, add only `core config storage reporting`, create `slideio-processor-data` and run its link guard; skip the masks/run guards and everything from `add_library(slideio-processor-engine …)` to the end.
- `tests/CMakeLists.txt`: when ON, add only `core config storage reporting`.
- `tests/core/CMakeLists.txt`: when ON, leave `test_data_type.cpp` out of `slideio_processor_core_tests` and skip the `slideio::core` include-directories line.
- `conanfile.py`: `options = {"data_only": [True, False]}`, `default_options = {"data_only": False}`; skip `self.requires("opencv/…")` and the OpenCV option block when `data_only`. **Keep the `requires("opencv/x.y.z")` line textually present** (it is the pin the configure reads); guard it with `if not self.options.data_only:` on the line above.
- `build.sh`: `--data-only` → `data_only=ON`; build directory `build-data` (`build-data-debug`), the SlideIO step skipped as with `--skip-slideio`, `conan install … -o data_only=True`, no `CMAKE_PREFIX_PATH` to the SlideIO install, `-DSLIDEIO_PROCESSOR_DATA_ONLY=ON`. Allowed with `-c debug` on Windows (no SlideIO across an STL API). `--data-only --no-test-seams` writes `build-noseams-data`. Document the flag in the usage header.
- `run-tests.sh`: `--data-only` selects `build-data` / `build-data-debug`.

- [ ] **Step 3: Build and run the data-only tests**

Run: `./build.sh --data-only --warnings-as-errors && ./run-tests.sh --data-only`
Expected: build succeeds; ctest reports 100% passed, and its total equals the full build's count for the `core.`, `config.`, `storage.`, `reporting.` prefixes and the header checks, minus the tests of `test_data_type.cpp` (`ctest -N` lists them; compare by name).

- [ ] **Step 4: Check nothing SlideIO or OpenCV slipped in**

Run: `grep -i "slideio_DIR\|OpenCV_DIR" build-data/CMakeCache.txt`
Expected: no lines (or only `-NOTFOUND` values).

- [ ] **Step 5: Run the data-only build without test seams**

Run: `./build.sh --data-only --no-test-seams --warnings-as-errors`
Expected: succeeds, and `scripts/check-no-test-seams.sh build-noseams-data` passes (it checks the data library's exports; if it insists on the engine library, make it skip a library that is absent and say so, in this step).

- [ ] **Step 6: Run the full build unchanged**

Run: `./build.sh --warnings-as-errors && ./run-tests.sh`
Expected: 1362 tests pass, as on `main`.

- [ ] **Step 7: Commit**

```bash
git add CMakeLists.txt src/CMakeLists.txt tests/CMakeLists.txt tests/core/CMakeLists.txt conanfile.py build.sh run-tests.sh scripts/check-no-test-seams.sh
git commit -m "build: SLIDEIO_PROCESSOR_DATA_ONLY builds the data library alone"
```

### Task 2: Consumer project and CI

**Files:**
- Create: `tests/data_only_consumer/CMakeLists.txt`, `tests/data_only_consumer/main.cpp`
- Create: `scripts/check-data-only-consumer.sh`
- Modify: `.github/workflows/build-validation.yml`
- Modify: `README.md` (a "Data library only" paragraph)

**Interfaces:**
- Consumes: Task 1's option, `build.sh --data-only`'s Conan output folder.
- Produces: `scripts/check-data-only-consumer.sh <conan-output-folder>` — configures, builds and runs the consumer, exit 0 on success.

- [ ] **Step 1: Write the consumer**

`tests/data_only_consumer/CMakeLists.txt`: a standalone project (`cmake_minimum_required(VERSION 3.21)`, `project(data_only_consumer CXX)`), C++17, that sets `SLIDEIO_PROCESSOR_DATA_ONLY ON` and `SLIDEIO_PROCESSOR_TEST_SEAMS OFF` as cache variables (`set(... CACHE BOOL "" FORCE)`), `add_subdirectory(<repo root> slideio-processor)`, and builds `consumer` from `main.cpp` linking `slideio-processor-data` only. Not added to the main build.

`main.cpp` must, in a temporary directory:
```cpp
// create a store, write nothing, reopen it read-only, read provenance
auto rw = slideio::processor::Store::open(path, OpenMode::CreateNew, Configuration());
Provenance p = rw->provenance(); p.setSlideId("/slides/a.svs"); p.setSceneIndex(2); rw->setProvenance(p);
auto ro = slideio::processor::Store::open(path, OpenMode::ReadOnly, Configuration());
// exit 1 unless ro->provenance().slideId() == "/slides/a.svs" && sceneIndex() == 2
// exit 1 unless ro->objects().totalObjectCount() == 0
```
(`OpenMode` is in `include/slideio/processor/storage/types.hpp`; `Provenance::setSlideId`/`setSceneIndex` in `core/provenance.hpp`. Close both stores before exit.)

- [ ] **Step 2: Write the script and run it**

`scripts/check-data-only-consumer.sh`: takes the Conan output folder of a data-only build (`build-data/conan`), configures `tests/data_only_consumer` into `build-data-consumer` with that toolchain and **no** prefix path to SlideIO or OpenCV, builds it, runs `consumer` (with the library's directory on `PATH` on Windows, `LD_LIBRARY_PATH`/`DYLD_LIBRARY_PATH` elsewhere), and fails on a non-zero exit.

Run: `./build.sh --data-only && scripts/check-data-only-consumer.sh build-data/conan`
Expected: exit 0, and the consumer's configure output contains `data-only build (no SlideIO, no OpenCV)`.

- [ ] **Step 3: Add the CI job**

In `build-validation.yml`, a job `build-data-only` with a matrix of `windows-2022` and `ubuntu-22.04` (same setup steps as `build`: Python + conan, MSVC on Windows, Conan cache keyed separately with `data-only` in the key; no SlideIO toolchain sync, no SlideIO cache), running:
```bash
./build.sh --data-only --warnings-as-errors
./run-tests.sh --data-only
scripts/check-data-only-consumer.sh build-data/conan
```

- [ ] **Step 4: Document**

README: a short "Data library only" section — what the option builds, `./build.sh --data-only`, and the `add_subdirectory` snippet (set the two cache variables, then `target_link_libraries(<target> PRIVATE slideio-processor-data)`), and that the consumer must provide sqlite3 (R-tree enabled), rapidjson, libtiff and zlib to `find_package`.

- [ ] **Step 5: Commit**

```bash
git add tests/data_only_consumer scripts/check-data-only-consumer.sh .github/workflows/build-validation.yml README.md
git commit -m "build: data-only consumer check and CI job"
```
