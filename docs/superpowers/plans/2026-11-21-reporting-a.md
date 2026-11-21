# Reporting Implementation Plan A — Foundation, CSV, Materialise

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The `reporting` module exists in the data library and can do three things:
- export a query's objects as a CSV file that pandas loads, with provenance, layers and every label marked;
- materialise a filtered, capped query into memory;
- serve GeoJSON (plan B) and masks (plan C) with a shared column schema. Parquet is deferred: the user chose to decide its dependency later.

**Architecture:**
- **Where it lives.** A new OBJECT library, `slideio-processor-reporting`, linked into `slideio-processor-data`. Public headers are under `include/slideio/processor/reporting/`.
- **What it depends on.** It uses only `core`, `config` and `storage`. The test target links only the data library, which proves no engine dependency.
- **How writers work.**
  - Each writer streams `store.objects().query(q)`, so nothing is held but the current row.
  - Each row's layer name, stage name, effective labels and parent come from the store's registries.
  - Output goes to `<path>.partial` and is renamed into place on success, so a cancelled or failed export never leaves a file that looks complete.

**Tech Stack:** C++17, CMake, SQLite (through `storage`), GoogleTest, MSVC/GCC/Clang.

**Spec:** `docs/superpowers/specs/sds/07-reporting.md` §7.1–7.3 and §7.7, and `docs/superpowers/specs/sds/03-storage.md` (object store, annotations, vocabulary, edges).

Requirements:
- UR-RES-6: bounded materialisation;
- UR-RES-9: CSV;
- UR-RES-11: provenance;
- UR-RES-15 and UR-RES-18: layers and labels marked;
- UR-SEG-6: calibrated or not, indicated;
- UR-QA-3: byte-identical exports.

**Base:** branch `feat/reporting-a` from `main`, taken after `build/gcc-clang-warnings` is merged.

## Global Constraints

- **Commands:**
  - Build: `./build.sh --skip-slideio --warnings-as-errors`.
  - Tests: `./run-tests.sh`; all must pass.
  - One test: `ctest --test-dir build -C Release -R <name>`.
  - Docs: `cmake --build build --target docs --config Release`.
- Zero compiler warnings (CI builds with warnings as errors) and zero Doxygen warnings.
- **Dependency rules:**
  - `reporting` includes only `core`, `config` and `storage` headers: never `pipeline`, `algorithm`, `workspace` or OpenCV.
  - `tests/reporting` links only `slideio-processor-data` and test support.
- Every public declaration carries `SLIDEIO_PROCESSOR_DATA_EXPORTS` and Doxygen with `@throws`, `@par Thread-safety` and `@par Requirements`, as the other modules' headers do.
- Messages never carry pixel, coordinate or parameter values, counts, label names or paths.
- **UR-QA-3:** two exports of an unchanged store with the same query and schema are byte-identical. No export time, random value or memory address appears in output. Objects come out in the cursor's order.
- Every source and test file starts with its `// Requirements: ...` line. `docs/traceability.md` lists every new test, one row per requirement, updated in place.

## Decisions

1. **Module wiring.**
   - Create `src/reporting/CMakeLists.txt`, an OBJECT library added to `slideio-processor-data` the way `storage` is.
   - Add `include/slideio/processor/reporting/reporting.hpp`, an umbrella header that includes every reporting header, as `storage/storage.hpp` does.
   - Add `tests/reporting/CMakeLists.txt`, with a target `slideio_processor_reporting_tests` linking `slideio-processor-data`, `GTest::gtest_main` and the existing test support, and discovered with the `reporting.` prefix.
   - Reuse `tests/storage/store_fixture.hpp` to build stores.
2. **`ExportResult`** lives in its own header, `reporting/export_result.hpp`, because every writer returns it. SDS 07 places it in `csv_writer.hpp`; SDS 07 is updated.
   - Public constructor: `ExportResult(int64_t objectsWritten, int64_t bytesWritten, bool wasCancelled) noexcept`.
   - Accessors as in SDS 07 §7.3.
3. **Store measurement names.** `allMeasurements()` needs the names a store holds, and storage has no call for that. Add:
   - `int32_t ObjectStore::measurementNameCount() const;`
   - `const char* ObjectStore::measurementNameAt(int32_t index) const;`

   These list the interned names (table `measurement_names`) in ascending byte order. Returned pointers live as long as the store, interned like `StageStateReader`'s strings. `IoError` is thrown on a closed store, and `InvalidArgumentError` for an index out of range. SDS 03 and SDS 08 are updated.
4. **Column names, exact and in this order.** Every column is optional except the identifier.
   - **Identifier:** `IdFormat::TwoColumns` gives `layer_id`, `anchor_id`, with the decimal `LayerId` value and the anchor as the decimal of `ObjectId`'s anchor. `PackedString` gives `object_id`, which is `ObjectId::toString`.
   - **`includeLayer`:** `layer`, the layer name.
   - **`includeStage`:** `stage`, the stage name, or empty when the object has no stage (imported objects).
   - **`includeLabel`:**
     - `EffectiveSet` gives `labels` and `labels_local`;
     - `ProducerOnly` gives `label` and `label_local`, from the row label only.

     Names are joined with `;` in the effective set's order. `labels_local` is the parallel list of `1` (workspace-local, `!vocabulary.isRecognised`) or `0` (standard). Inside a name, `\` becomes `\\` and `;` becomes `\;`. An object with no label gets empty cells.
   - **`includeBoundingBox`:** `bbox_x`, `bbox_y`, `bbox_width`, `bbox_height`, in level-0 pixels, half-open, from `ObjectRecord::boundingBox()`.
   - **`includeGeometryWkt`:** `geometry_wkt`, in level-0 integer coordinates.
     - With one outer ring, `POLYGON((outer),(hole),…)`.
     - With several outer rings, `MULTIPOLYGON(((outer),(hole),…),…)`. Each hole goes to the first outer ring, in ring order, that contains its first vertex (point-in-ring test on that ring alone).
     - Rings are closed by repeating the first point.
   - **`includeParent`:** TwoColumns gives `parent_layer_id`, `parent_anchor_id`; Packed gives `parent_id`. Both come from `EdgeStore::parentOf`, and are empty when there is no parent.
   - **Measurements**, after the fixed columns. See decision 5.
5. **Measurements and units (UR-SEG-6).** Storage keeps each measurement in pixels under its base name, plus, when the scene was calibrated, a twin under the base name followed by `.um` or `.um2`.
   - **Base names.**
     - `measurement(name)` adds base names in call order; adding a name twice is `InvalidArgumentError`.
     - `allMeasurements()` adds, at write time, every store measurement name that does not end in `.um` or `.um2`, in ascending byte order, after any explicit names, without duplicates.
   - **Columns per base name `n`:**
     - **`PixelsOnly`:** column `n`, with the pixel value.
     - **`Calibrated`** (default):
       - one column, named after the twin (`n.um` or `n.um2`) when the store holds that twin name, with the twin's value;
       - otherwise named `n`, with the pixel value.

       The header therefore says which unit the column holds.
     - **`BothColumns`:** column `n` and, when the store holds a twin name, the twin column too.
   - **Values.** A value a row lacks is an empty cell. Values are written with `%.17g`, so they round-trip.
   - **Resolution.** `columnCount()` and `columnNameAt()` report the fixed columns plus the explicit names, as `PixelsOnly` would. The full list, with twins and `allMeasurements` expansion, is resolved per store by `detail::resolveColumns(const ColumnSchema&, const Store&)` in `src/reporting/detail/columns.{hpp,cpp}`. Every writer uses it.
6. **CSV format.**
   - RFC 4180: a field containing the delimiter, `"`, CR or LF is quoted, with `"` doubled. Lines end in `\n`. The file is UTF-8 without a BOM.
   - The header line comes first, after any provenance lines.
   - `delimiter(c)` refuses `"`, CR, LF and `#` with `InvalidArgumentError`.
7. **Provenance header** (`provenanceHeader(true)`, default):
   ```
   # slideio-processor export
   # slide_id: <slideId>
   # scene_index: <n>
   # library_version: <v>
   # created_at_unix_ms: <n>
   # stage: <name> <configuration>
   ```
   - The values come from the store's provenance.
   - There is one `stage` line per stage, in `StageStateReader` order. The configuration is the stored JSON, with CR and LF written as `\r` and `\n`, so each stays on one line.
   - Nothing else is written: no export time.
8. **Writing, progress, cancellation.**
   - `write(store, q, observer, token)`:
     - gets `total = store.objects().count(q)`;
     - streams `store.objects().query(q)`;
     - calls `observer->onProgress("export", fraction, completed, total)` after every 1000 objects and once at the end, when an observer is given;
     - checks `token.isCancelled()` before each object.
   - **On cancel:** the partial file is removed and the result has `wasCancelled() == true`, with the objects counted up to then.
   - **On success:** the partial file is renamed onto `path`, replacing any existing file. `bytesWritten` is the final file size.
   - **On any exception:** the partial file is removed and the exception propagates.
   - The two-argument `write` calls the four-argument one with no observer and a token never cancelled.
   - `CsvWriter` is ThreadConfined.
9. **`materialise`** (§7.7), in `reporting/materialise.hpp`.
   - **Refusals.** `InvalidArgumentError`, before reading anything, when:
     - `maxObjects < 1`;
     - `q.isUnfiltered()`;
     - `store.objects().count(q) > maxObjects`.
   - **Otherwise,** it reads every record into an `ObjectArray`.
   - **`ObjectArray::at`** throws `InvalidArgumentError` out of range.
   - There is no overload without `maxObjects`.
10. **Docs.**
    - SDS 07 §7.2–7.3 and §7.7 gain decisions 2 and 4–9.
    - §7.4 says Parquet is deferred (user decision 2026-11-21).
    - SDS 03 and SDS 08 gain decision 3.
    - `docs/project.md` gains a Reporting plan A entry (UR-RES-10 Parquet deferred).
    - `docs/traceability.md` gains every new test.

## Review Focus

1. **Characters that need escaping:**
   - a label name containing `;`, `\`, `"`, a comma or a newline round-trips through `labels` and CSV quoting;
   - a stage configuration with a newline stays on one provenance line.

   Tests: `CsvWriter.LabelNamesWithDelimitersRoundTrip`, `CsvWriter.ProvenanceStaysOneLinePerStage` (Task 4).
2. **Uncalibrated store:** `Calibrated` writes the pixel columns under their base names, and `BothColumns` adds no twin (`CsvWriter.UncalibratedStoreKeepsPixelColumns`, Task 4).
3. **Cancellation and failure leave no file at `path`,** and an existing file at `path` survives a cancelled export unchanged (`CsvWriter.CancelledExportLeavesNoFile`, `CsvWriter.CancelledExportKeepsAnExistingFile`, Task 4).
4. **Two exports of the same store are byte-identical** (`CsvWriter.ExportIsDeterministic`, Task 4).
5. **Materialise refuses:** an unfiltered query, a cap of 0, and a count one above the cap, each before reading (`Materialise.RefusesUnfilteredQuery`, `Materialise.RefusesNonPositiveCap`, `Materialise.RefusesCountAboveCap`, Task 1).

---

### Task 1: Module wiring, `ExportResult`, `materialise`

**Files:**
- Create:
  - `src/reporting/CMakeLists.txt`, `src/reporting/export_result.cpp`, `src/reporting/materialise.cpp`;
  - `include/slideio/processor/reporting/{reporting,export_result,materialise}.hpp`;
  - `tests/reporting/CMakeLists.txt`, `tests/reporting/test_materialise.cpp`.
- Modify: `src/CMakeLists.txt` (link the object library into the data library), `tests/CMakeLists.txt`, `docs/traceability.md`.

**Interfaces:**
- Produces:
  - `class ExportResult` (decision 2);
  - `class ObjectArray { int64_t count() const noexcept; const ObjectRecord& at(int64_t) const; }` and `using ObjectArrayPtr = std::shared_ptr<ObjectArray>;`;
  - `ObjectArrayPtr materialise(Store&, const ObjectQuery&, int64_t maxObjects);`
  - the `slideio-processor-reporting` OBJECT library and the `reporting.` test prefix.

- [ ] **Step 1: Write the failing tests** in `tests/reporting/test_materialise.cpp`, on a store with 5 objects in layer `nuclei`:
  - `Materialise.ReadsAFilteredQuery`: `ObjectQuery().layer(nuclei)` with cap 5 gives `count() == 5`, and `at(i).id()` matches the cursor order.
  - `Materialise.RefusesUnfilteredQuery`: `ObjectQuery()` throws `InvalidArgumentError`.
  - `Materialise.RefusesNonPositiveCap`: caps 0 and -1 throw.
  - `Materialise.RefusesCountAboveCap`: the layer query with cap 4 throws.
  - `Materialise.AtOutOfRangeThrows`: `at(-1)` and `at(5)` throw.
  - `ExportResult.ReportsItsValues`: the constructor's values come back from the accessors.

- [ ] **Step 2: Run them to verify they fail**

Run: `ctest --test-dir build -C Release -R "reporting\."`
Expected: configure or compile failure (no reporting module).

- [ ] **Step 3: Implement decisions 1, 2 and 9.**

- [ ] **Step 4: Verify**

Run: `./build.sh --skip-slideio --warnings-as-errors` then `./run-tests.sh`
Expected: all pass, zero warnings.

- [ ] **Step 5: Commit**

```bash
git add src include tests docs/traceability.md
git commit -m "feat(reporting): module, ExportResult and bounded materialise"
```

### Task 2: Store measurement names

**Files:**
- Modify: `include/slideio/processor/storage/object_store.hpp`, `src/storage/object_store.cpp`, SDS 03 (object store section), SDS 08 (`ObjectStore`).
- Test: `tests/storage/test_object_store.cpp`, or the file holding `ObjectStore` tests.

**Interfaces:**
- Produces: `int32_t ObjectStore::measurementNameCount() const;` and `const char* ObjectStore::measurementNameAt(int32_t index) const;`

- [ ] **Step 1: Write the failing tests**
  - `ObjectStore.MeasurementNamesAreListedInByteOrder`: objects written with measurements `perimeter`, `area`, `area.um2` give count 3 and `area`, `area.um2`, `perimeter` in that order.
  - `ObjectStore.MeasurementNameIndexOutOfRangeThrows`.
  - `ObjectStore.MeasurementNamesOnAClosedStoreThrowIoError`.

- [ ] **Step 2: Run, see them fail. Step 3: Implement decision 3. Step 4: `./run-tests.sh` passes.**

- [ ] **Step 5: Commit**

```bash
git add include src tests docs
git commit -m "feat(storage): list the store's measurement names"
```

### Task 3: `ColumnSchema` and column resolution

**Files:**
- Create: `include/slideio/processor/reporting/column_schema.hpp`, `src/reporting/column_schema.cpp`, `src/reporting/detail/columns.{hpp,cpp}`, `tests/reporting/test_column_schema.cpp`.

**Interfaces:**
- Consumes: `ObjectStore::measurementNameCount/At` (Task 2).
- Produces:
  - the SDS 07 §7.2 `ColumnSchema`, `IdFormat`, `LabelPolicy` and `UnitPolicy`;
  - `detail::ResolvedColumns detail::resolveColumns(const ColumnSchema&, const Store&)`, which returns the ordered column names and, for each measurement column, the store name its value comes from.

- [ ] **Step 1: Write the failing tests** (decisions 4 and 5):
  - `ColumnSchema.StandardHeader`: `ColumnSchema::standard().toHeaderLine` gives exactly `layer_id,anchor_id,layer,stage,labels,labels_local,bbox_x,bbox_y,bbox_width,bbox_height`.
  - `ColumnSchema.PackedIdAndOptionalColumns`: `PackedString`, `includeGeometryWkt(true)` and `includeParent(true)` give `object_id,…,geometry_wkt,parent_id`, in decision 4's order.
  - `ColumnSchema.ProducerOnlyLabelColumns`: `label` and `label_local` replace `labels` and `labels_local`.
  - `ColumnSchema.DuplicateMeasurementThrows`.
  - `ColumnSchema.HeaderBufferTooSmall`: `toHeaderLine` with a short buffer returns the needed size and writes a terminated prefix, following the library's existing `toJson`/`toString` buffer convention.
  - `ColumnSchema.UnitPoliciesResolveTwins`, on a store holding `area`, `area.um2` and `perimeter` (no twin):
    - `Calibrated` + `allMeasurements` gives `area.um2`, `perimeter`;
    - `PixelsOnly` gives `area`, `perimeter`;
    - `BothColumns` gives `area`, `area.um2`, `perimeter`.
  - `ColumnSchema.ExplicitNamesComeFirst`: `measurement("perimeter")` then `allMeasurements()` gives `perimeter` before `area…`, with no duplicate.

- [ ] **Step 2: Run, see them fail. Step 3: Implement. Step 4: `./run-tests.sh` passes.**

- [ ] **Step 5: Commit**

```bash
git add include src tests docs/traceability.md
git commit -m "feat(reporting): column schema and per-store column resolution"
```

### Task 4: `CsvWriter`

**Files:**
- Create: `include/slideio/processor/reporting/csv_writer.hpp`, `src/reporting/csv_writer.cpp`, `src/reporting/detail/{csv_format,wkt}.{hpp,cpp}`, `tests/reporting/test_csv_writer.cpp`.
- Modify: SDS 07, SDS 03, SDS 08, `docs/project.md`, `docs/traceability.md` (decision 10).

**Interfaces:**
- Consumes: `ColumnSchema` and `detail::resolveColumns` (Task 3); `ExportResult` (Task 1).
- Produces: the SDS 07 §7.3 `CsvWriter`.

- [ ] **Step 1: Write the failing tests.** Build stores with the store fixture: a parent layer and a child layer with parent edges, standard and workspace-local labels, an asserted second label, calibrated twins, and a hole polygon.
  - `CsvWriter.WritesHeaderAndRows`: the provenance lines exactly as decision 7, then the header, then one row per object in cursor order with the expected cells.
  - `CsvWriter.EffectiveLabelsAndLocalFlags`: an object with a row label plus an asserted local label gives `labels` = `tumour;myLocal`, `labels_local` = `0;1`, in the store's effective order.
  - `CsvWriter.LabelNamesWithDelimitersRoundTrip`: a local label named `a;b\c"d,e` is escaped per decision 4 and quoted per decision 6. Parsing the line back with a minimal RFC 4180 reader in the test recovers it.
  - `CsvWriter.ParentColumns`: a child gives its parent's ids; a root gives empty cells.
  - `CsvWriter.GeometryWkt`: a square with a hole gives `POLYGON((…),(…))`; two outer rings give `MULTIPOLYGON`.
  - `CsvWriter.UncalibratedStoreKeepsPixelColumns`: no twins, `Calibrated` and `BothColumns` give base-name columns only.
  - `CsvWriter.ProvenanceStaysOneLinePerStage`: a stage configuration containing a newline is written with `\n` on one line.
  - `CsvWriter.ExportIsDeterministic`: two exports are byte-identical.
  - `CsvWriter.CancelledExportLeavesNoFile`: a token cancelled before the first object gives `wasCancelled()`, and no file at `path` or `path.partial`.
  - `CsvWriter.CancelledExportKeepsAnExistingFile`: a pre-existing file at `path` is unchanged after a cancelled export.
  - `CsvWriter.ReportsProgress`: 2500 objects give progress calls at 1000 and 2000 and a final one at 2500, with `total == 2500`.
  - `CsvWriter.RefusesBadDelimiter`: `"`, `\n` and `#` throw.
  - `CsvWriter.CustomDelimiter`: `;` as the delimiter quotes a `labels` cell that holds `;`.

- [ ] **Step 2: Run, see them fail. Step 3: Implement decisions 4–8 and 10. Step 4: `./run-tests.sh` and the docs target pass.**

- [ ] **Step 5: Commit**

```bash
git add include src tests docs
git commit -m "feat(reporting): CSV export with provenance, labels, parents and units"
```
