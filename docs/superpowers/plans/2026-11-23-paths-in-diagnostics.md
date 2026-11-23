# Paths in diagnostics — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Errors, error causes and log lines name the file or directory they are about, and third-party error text reaches them unaltered.

**Architecture:**
- **Shared helpers:** `src/core/detail/diagnostics.hpp` stops redacting and gains `withPath(message, path)`.
- **Throw and log sites:** each one that has a path in scope appends it.
- **libtiff and SQLite:** each is handed the file name.
- **Test:** the rule's test is inverted.
- **Documentation:** the SDS text that states the old rule is rewritten.

**Tech Stack:** C++17, CMake, GoogleTest, Doxygen (the `docs` target).

**Spec:** `docs/superpowers/specs/2026-11-23-paths-in-diagnostics-design.md`.

## Global Constraints

- **Format:**
  - `detail::withPath(const std::string& message, const std::string& path)` returns `message + " (" + path + ")"`.
  - The path is the UTF-8 string the caller gave. A `std::filesystem::path` goes through `u8string()`.
  - The role wording ("the store", "the raster file", "the output file", "the scratch file", and so on) stays in every message.
- `diagnosticText(e)` returns `e.what()` for every exception, including `std::filesystem::filesystem_error`.
- **Unchanged:**
  - Exported file content: no slide path is written into exports, and the slide id defaults to `""`.
  - The exception-translation rules: `bad_alloc` passes through; anything else becomes `IoError` or `InternalError`.
  - Every exception type and `ErrorCode`.
- **Build and test (Git Bash):**
  - Build with `./build.sh`, test with `./run-tests.sh`, and focus with `./run-tests.sh -R <regex>`.
  - The Doxygen `docs` target must stay warning-free: `cmake --build build --config Release --target docs`.
  - Logs go under `build/` with a `pd-` prefix.
- **Warnings:** no new compiler warnings. CI builds GCC and Clang with `-Werror`.

## Review Focus

1. **A message that names the wrong file.** For example the `.partial` path where the target is meant, or the store path in a raster error. Each new path test asserts the specific path.
2. **A path built from a temporary that dies before `throw`.** `withPath` returns `std::string`, and the `IoError` copies it. Never keep a `c_str()` of a temporary.
3. **SQLite with no connection.** `throwSqliteError(nullptr, …)` must not call `sqlite3_db_filename`.
4. **libtiff given the path as its module name.** The warning and error handlers must stay null-safe, and libtiff's text must still reach `cause()`.
5. **Exact-text tests that silently stop checking.** Update each one to the new exact text. Never loosen one to `contains(role)` alone.

---

### Task 1: Helpers, slide causes, and the inverted rule test

**Files:**
- Modify: `src/core/detail/diagnostics.hpp`, `src/workspace/workspace.cpp` (`slideCause`)
- Delete: `src/pipeline/detail/slide_cause.hpp`
- Update the `slideCauseText` callers: `src/masks/mask_writer.cpp`, `src/pipeline/detail/level_table.cpp`, `src/pipeline/detail/pipeline_scene_source.cpp`, `src/pipeline/detail/scene_reader.cpp`
- Move: `tests/workspace/test_no_paths_in_diagnostics.cpp` → `tests/workspace/test_paths_in_diagnostics.cpp`, and update `tests/workspace/CMakeLists.txt`
- Modify: `docs/traceability.md`, the rows mapping UR-STG-9 and UR-EXE-5 to the old test name

**Interfaces:**
- Produces `std::string detail::withPath(const std::string& message, const std::string& path)` in `core/detail/diagnostics.hpp`. Tasks 2–4 use it.
- `diagnosticText` keeps its signature.
- `redactSlidePath` and `slideCauseText` are removed. Remove `replaceAll` too if nothing else uses it.

- [ ] **Step 1: Write the failing tests.** In the renamed file, rename the test to `Diagnostics.PathsInErrorsAndLogs` and invert it.
  - Keep the existing fixture, which puts `kMarker` in every directory and file name, and keep its failure scenarios.
  - Assert that each recorded diagnostic contains `kMarker`, where today it asserts that none does.
  - For scenarios that Tasks 2–4 cover, mark the expectation in a table with the owning task, and skip those rows until that task lands. Remove a row's skip when its task lands.
  - Add `Diagnostics.FilesystemErrorKeepsItsPaths`: `diagnosticText` of a `std::filesystem::filesystem_error` built with path `"a/" + kMarker` contains `kMarker`.
  - Add `Diagnostics.WithPathAppendsThePath`: `withPath("cannot open the store file", "D:/x/s.db") == "cannot open the store file (D:/x/s.db)"`.
  - Add `Diagnostics.SlideCauseIsNotRedacted`: opening a missing slide whose path holds `kMarker` gives a `cause()` that is not `"<slide>"` but carries SlideIO's text, including the marker when SlideIO words it with the path.
- [ ] **Step 2: Run and confirm RED.** Run `./run-tests.sh -R Diagnostics`. The new and inverted rows fail.
- [ ] **Step 3: Implement.**
  - Change `diagnosticText`.
  - Add `withPath`.
  - Delete the redaction functions and the slide-cause header.
  - Point the four callers and workspace's `slideCause` at `diagnosticText`, and delete `slideCause`.
  - Rewrite the header comment of `diagnostics.hpp` to describe the new rule.
- [ ] **Step 4: Run.** Run `./run-tests.sh -R Diagnostics`, which passes except for the rows marked for later tasks. Then run the full suite. Any exact-text test that broke because a cause now carries a path is updated to the new exact text, which is not loosened.
- [ ] **Step 5: Commit.** `git commit -m "feat(core): diagnostics keep third-party text and gain withPath; invert the no-paths test"`

### Task 2: Storage names its files

**Files (src/storage/):**
- `detail/file_io.cpp`: `throwIo` and its `syncFile`/`removeFile` callers.
- `detail/store_impl.cpp` and `.hpp`: `throwIo`, and the open, create and exists checks.
- `detail/sqlite.cpp`: `throwSqliteError`, using `sqlite3_db_filename(db, "main")` when `db != nullptr`.
- `detail/writer_lock.cpp`: `throwHeld` and `throwLockFileError`.
- `detail/tiff_io.cpp`: `kModuleName` becomes the UTF-8 path of the file being opened, passed to `TIFFClientOpenExt`. Also `fail`, the flush error and the warning log line.
- `detail/tiff_raster_reader.cpp`.
- `detail/tile_spool.cpp`.
- `raster_registry.cpp`, including the delete-warning log line.
- `raster_writer.cpp`, including its log lines.
- `store.cpp`, the close and cleanup log lines.
- `geojson_import.cpp` logs: add the path where the file is in scope.

Also update the doc comments of the internal headers that promise no path: `pyramid_tiff_writer.hpp`, `sqlite.hpp`, `tiff_io.hpp`, `tiff_raster_reader.hpp`, `tile_spool.hpp` and `writer_lock.hpp`.

**Interfaces:** consumes `withPath`. The storage `throwIo` helpers may gain a `path` parameter, or callers can pass `withPath(message, path)`. Choose one form per helper and apply it everywhere.

- [ ] **Step 1: Failing tests.**
  - Enable the storage rows of `Diagnostics.PathsInErrorsAndLogs`.
  - Add focused checks: a missing store path appears in `Store::open`'s message; a corrupt or unopenable TIFF's path appears in `tiff_io`'s message; a held writer lock names the lock file.
  - Update these exact-text expectations to their new exact text: `tests/storage/test_tiff_io.cpp` (the "libtiff failed on the raster file" line and the three no-path checks), `tests/storage/test_store.cpp` (the "the store" contains-checks stay), and `tests/storage/test_event_journal.cpp`.
  - Keep the test in `test_store.cpp` that asserts a malformed record's content ("ACC-0042") is absent: it is about record content, not a path.
- [ ] **Step 2: RED.** Run `./run-tests.sh -R "Diagnostics|Store|Tiff|Raster|Spool|WriterLock|EventJournal|GeoJsonImport"`.
- [ ] **Step 3: Implement.** Apply `withPath` at every storage throw and log site that has a path in scope.
- [ ] **Step 4: Run.** The focused suite, then the full suite.
- [ ] **Step 5: Commit.** `git commit -m "feat(storage): storage errors and logs name their files"`

### Task 3: Workspace, run, pipeline and config name their files

**Files:**
- `src/workspace/workspace.cpp`: the slide open and scene messages, the workspace and scratch directories, `requirePath` where a path exists, and the cleanup log lines.
- `src/workspace/stage_runner.cpp`: its log lines.
- `src/run/slide_run.cpp`: "the slide cannot be found" and "the export directory cannot be made".
- `src/run/pipeline_file.cpp`: "cannot be opened" and "cannot be read".
- `src/pipeline/executor.cpp`: the scratch location.
- `src/pipeline/operation_catalog.cpp`: the file open and write.
- `src/pipeline/detail/fragment_spill.cpp` and `src/pipeline/seam_merger.cpp`: the scratch file.
- `src/config/detail/stream_log_sink.cpp`: the log file. Keep the original open failure as the cause rather than swallowing it.
- Public headers whose Doxygen says "never its path" or "carry no path": `config/logging.hpp`, `pipeline/operation_catalog.hpp`, `pipeline/seam_merger.hpp`, `run/slide_run.hpp`, `workspace/workspace.hpp`.

- [ ] **Step 1: Failing tests.**
  - Enable these rows of `Diagnostics.PathsInErrorsAndLogs`.
  - In `tests/run/test_slide_run.cpp`:
    - Rename `SlideRun.AMovedSlideGivesAnErrorWithNoPath` to `SlideRun.AMovedSlideNamesItsPath`. Its exact text becomes `"IoError: SlideRun: the slide cannot be found (<moved path>)"`.
    - Invert the `message.find(path) == npos` check to `!= npos`.
    - Keep `SlideRun.ExportsCarryNoSlidePath` unchanged, because it is about export content.
  - In `tests/workspace/test_workspace.cpp`, invert the two no-path checks.
  - In `tests/pipeline/test_seam_spill.cpp`, the expected "cannot create the scratch file" becomes that text plus ` (<path>)`.
  - Add `PipelineFile.LoadMissingNamesThePath`: `PipelineFile::load` on a missing file gives an `IoError` whose `what()` contains the path.
- [ ] **Step 2: RED.** Run `./run-tests.sh -R "Diagnostics|SlideRun|Workspace|SeamSpill|PipelineFile|Logging|OperationCatalog|Executor"`.
- [ ] **Step 3: Implement** with `withPath`.
- [ ] **Step 4: Run.** The focused suite, then the full suite.
- [ ] **Step 5: Commit.** `git commit -m "feat(workspace,run,pipeline,config): errors and logs name their files"`

### Task 4: Exports, masks and GeoJSON name their files

**Files:**
- `src/reporting/csv_writer.cpp`
- `src/reporting/geojson_exporter.cpp`: the comment that replaces the writer's messages goes, and the writer's text flows through.
- `src/core/geojson_writer.cpp` and `src/core/geojson_reader.cpp`: "of the file" becomes "of the file (<path>)" where a path exists.
- `src/reporting/raster_tiff_writer.cpp`
- `src/reporting/detail/export_driver.cpp`:
  - The replace failure names the target, keeps the `std::error_code`'s message as `cause()`, and names the `.partial` path.
  - The same applies to `raster_tiff_writer.cpp`'s replace and size reads.
- `src/masks/mask_writer.cpp`
- Public headers: `core/geojson.hpp`, `masks/mask_writer.hpp`, `reporting/csv_writer.hpp`, `reporting/geojson_exporter.hpp`, `reporting/raster_tiff_writer.hpp`, `storage/raster_store.hpp`, `storage/store.hpp`.

- [ ] **Step 1: Failing tests.**
  - Enable the remaining rows of `Diagnostics.PathsInErrorsAndLogs`.
  - Add `ExportDriver.ReplaceFailureNamesBothFilesAndTheCause`. Make the target unreplaceable, for example by making it a non-empty directory. Expect `what()` to contain the target path and `cause()` to be non-null and non-empty.
  - Add `GeoJsonReader.ErrorNamesTheFile`, which checks the file path in the reader's message.
  - In `tests/core/test_geojson_reader.cpp`, invert the no-path check.
  - Keep every export-content test (the no slide id or file name in CSV, GeoJSON, raster or pyramid TIFF content) unchanged.
- [ ] **Step 2: RED.** Run `./run-tests.sh -R "Diagnostics|Csv|GeoJson|RasterTiff|ExportDriver|MaskWriter"`.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run.** The focused suite, then the full suite. At this point `Diagnostics.PathsInErrorsAndLogs` has no skipped rows.
- [ ] **Step 5: Commit.** `git commit -m "feat(reporting,masks,core): export and GeoJSON errors name their files"`

### Task 5: Documentation

**Files:**
- `docs/superpowers/specs/sds/00-conventions.md`:
  - Replace the "No paths in diagnostics" paragraph block in "Errors" with a "Paths in diagnostics" paragraph. It covers the decision of 2026-11-23 and its reason, the `withPath` format, unaltered third-party text, logs following the same rule, the test that enforces it, and the PHI caution from the spec.
  - Update the cross-reference "(subject to the path rule below)".
- `docs/superpowers/specs/sds/02-config.md`, `03-storage.md`, `04-workspace.md`, `07-reporting.md`, `09-run.md`:
  - Reword every "never its path", "not its path" or "carry no path" sentence that is about diagnostics.
  - Leave "no path in exports" sentences unchanged.
  - `04-workspace.md`'s stale "The library does not redact the path" sentence goes.
- `docs/superpowers/specs/2026-11-22-python-bindings-design.md`: below the anti-goal about paths in errors, add the dated note "Revoked for errors and logs on 2026-11-23 (paths-in-diagnostics design); exports unchanged."
- Leave historical plans unchanged.

- [ ] **Step 1: Find every statement.** Run `grep -rn -i "never its path\|not its path\|no path\|carry no path\|by its role" docs/superpowers/specs include src`. List each hit in your report as reworded, export-content (kept), or already gone.
- [ ] **Step 2: Edit.**
- [ ] **Step 3: Verify.** The docs target builds warning-free, and the full suite passes.
- [ ] **Step 4: Commit.** `git commit -m "docs: paths appear in diagnostics (rule revoked 2026-11-23)"`
