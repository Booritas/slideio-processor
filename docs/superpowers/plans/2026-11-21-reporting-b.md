# Reporting Implementation Plan B — GeoJSON Export

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `GeoJsonExporter` streams a query's objects into a QuPath-compatible GeoJSON FeatureCollection, with provenance, layers, effective labels marked standard or local, the measurements a `ColumnSchema` selects, and each object's parent id. Exported files open in QuPath, and import back through `GeoJsonImporter` with geometry, label and measurements intact (UR-RES-8).

**Architecture:**
- **One emitter.** Every feature goes through `core::GeoJsonWriter`, so the library keeps one GeoJSON emitter (SDS 07 §7.5). It gains an overload that also writes `parentId`.
- **One export driver.** The streaming loop `CsvWriter` already has is moved into `src/reporting/detail/export_driver`, so both writers share it. It covers the `.partial` file and rename, progress, cancellation, and the name cache. `CsvWriter`'s behaviour is unchanged.
- **Hierarchy deferred (user decision, 2026-11-21).** Writing `parentId` makes the files ready for it, but this plan does not make re-import restore layers and parents (UR-RES-15, Should). That needs a layered import mode in `storage`, recorded as a follow-up.
- **Later-stage measurements (user decision, 2026-11-21, added after the plan was approved).** Measurements a later stage asserted on an object are exported, by CSV and GeoJSON alike, in stage-qualified columns named `<stage name>/<measurement name>`, so no value is hidden and each stays traceable to its stage. Task 3 adds them; the GeoJSON exporter becomes Task 4.

**Tech Stack:** C++17, CMake, RapidJSON (already used by core's GeoJSON codec), GoogleTest.

**Spec:**
- `docs/superpowers/specs/sds/07-reporting.md` §7.1 and §7.5.
- `docs/superpowers/specs/sds/01-core.md` (`GeoJsonWriter`) and `03-storage.md` (`GeoJsonImporter`).
- Requirements: UR-RES-8, UR-RES-11, UR-RES-15 (layer carried; round trip deferred), UR-RES-18, UR-QA-3.

**Base:** branch `feat/reporting-b` from `feat/reporting-a`, after Plan A is rebased onto `main` (7dbaa0a or later). If Plan A has merged by then, branch from `main`.

## Global Constraints

- **Commands:**
  - Build: `./build.sh --skip-slideio --warnings-as-errors`.
  - Tests: `./run-tests.sh`; all must pass.
  - One test: `ctest --test-dir build -C Release -R <name>`.
  - Docs: `cmake --build build --target docs --config Release`.
  - No-seams check: `./build.sh --skip-slideio --no-test-seams --warnings-as-errors` then `scripts/check-no-test-seams.sh build-noseams`.
- Zero compiler warnings (read the real build output) and zero Doxygen warnings.
- **Dependency rules:** `reporting` includes only `core`, `config` and `storage`. `tests/reporting` links only the data library and test support.
- **Public API:**
  - New: `GeoJsonExporter` and the `GeoJsonWriter::writeFeature` overload of decision 2.
  - No other public signature changes. The existing `writeFeature` keeps its behaviour byte for byte.
- **Messages** never carry counts, names, paths, label text or values.
- **UR-QA-3:** two exports of an unchanged store with the same query and schema are byte-identical. No export time is written.
- **Requirements lines and traceability:** every new source and test file starts with its `// Requirements: ...` line, and `docs/traceability.md` lists every new test.

## Decisions

1. **Shared export driver** (refactor, Task 1). Move from `csv_writer.cpp` into `src/reporting/detail/export_driver.{hpp,cpp}`:
   - the `.partial` path and its RAII guard, and rename-on-success;
   - the progress cadence (every 1000 objects and at the end, stage name `"export"`) and cancellation before each object;
   - the per-export name cache: layer, stage and label names, and the local flags.

   Its shape is the implementer's choice: a function taking a per-object callback, or a small class. `CsvWriter` uses it, every `reporting.*` CSV test passes unchanged, and output stays byte-identical.
2. **Parent id in the writer.** Add `void GeoJsonWriter::writeFeature(const ObjectRecord&, const char* layerName, const char* const* labelNames, const bool* labelIsWorkspaceLocal, int32_t labelCount, const ObjectId* parent);`.
   - When `parent` is not null, it writes `"parentId": "<ObjectId::toString>"` right after `"objectId"`. When it is null, the output equals the existing overload's.
   - The existing overload calls the new one with `nullptr`.
   - SDS 01 and SDS 08 are updated.
3. **`GeoJsonExporter` API.** Follows SDS 07 §7.5, with these changes recorded in SDS 07:
   - `coordinatePrecision` is dropped, because store coordinates are integers and the writer writes them exactly.
   - `quPathCompatible` is dropped, because the writer always emits the QuPath shape: `objectType: "detection"` and `classification: {"name": …}`.

   Members:
   - `explicit GeoJsonExporter(const char* path)`;
   - `~GeoJsonExporter()`;
   - `GeoJsonExporter& schema(const ColumnSchema&)`;
   - `ExportResult write(Store&, const ObjectQuery&)`;
   - `ExportResult write(Store&, const ObjectQuery&, ProgressObserver*, const CancellationToken&)`.

   It is ThreadConfined, uses pimpl, and lives in `reporting/geojson_exporter.hpp`, included by the umbrella header.
4. **What the schema means for GeoJSON.**
   - **Default:** with no `schema()` call, the schema is `ColumnSchema().allMeasurements().units(UnitPolicy::BothColumns)`, so a default export loses no measurement.
   - **Measurements:** the resolved measurement columns (`detail::resolveColumns`) decide which values a feature carries, under their column names (`area`, `area.um2`, …). The exporter builds a per-object copy of the record holding only those, in column order, each with its source unit.
   - **Labels:** `labelPolicy` decides the label list: `EffectiveSet` passes the whole effective set, and `ProducerOnly` passes the row label alone. The first passed label becomes the `classification`.
   - **Parent:** `includeParent(true)` passes the parent from `EdgeStore::parentOf`, or none for a root.
   - **Ignored:** `idFormat`, `includeLayer`, `includeStage`, `includeLabel(false)`, `includeBoundingBox` and `includeGeometryWkt`. The feature always carries `objectId`, `layer`, geometry and `labels`.
   - The exporter's Doxygen and SDS 07 §7.5 say all of this.
5. **Non-finite measurement values.** JSON has no NaN or Inf, and the writer refuses them. A non-finite value is left out of that feature's `measurements`, and the rest of the feature is written. This is documented in the header and in SDS 07.
6. **Bytes and atomicity.** The file is written to `<path>.partial` through `GeoJsonWriter`, then `end()`. `bytesWritten` is the partial file's size taken before the rename, so nothing can throw after it. Then rename onto `path`. Cancel and failure behave as in `CsvWriter`.
7. **Provenance and PHI.** `GeoJsonWriter::begin` writes the store's provenance, whose `slideId` is the slide path. The exporter's Doxygen and SDS 07 §7.5 carry the same PHI/PII note as `CsvWriter` (HIPAA, GDPR) and point to the existing redaction follow-up in `docs/project.md`.
9. **Later-stage (asserted) measurements**, Task 3. They appear in every export, in stage-qualified columns.
   - **Storage listing.** Add to `AnnotationStore`:
     - `int32_t assertedMeasurementColumnCount() const;`
     - `void assertedMeasurementColumnAt(int32_t index, StageId& stage, const char*& name) const;`

     These list the distinct `(stage, name)` pairs in `measurement_assertions`, whatever their visibility, ordered by stage id, then name bytewise. Names are interned for the store's life. Errors as `ObjectStore::measurementNameAt`. SDS 03 and SDS 08 are updated.
   - **Column naming.** An asserted measurement's column is `<stage name>/<name>`, with the stage name from `StageStateReader`.
   - **Units.** The unit policy applies per stage exactly as decision 5 of Plan A does per row: a stage's `n` and `n.um`/`n.um2` assertions are base and twin.
   - **Selection.** `ColumnSchema` gains `ColumnSchema& assertedMeasurement(const char* stageName, const char* name);`, with the same refusals as `measurement()` (null or empty, a `.um`/`.um2` name, a duplicate). It selects one stage's base name; the stage name is resolved per store, and an unknown stage gives an empty column.
   - **`allMeasurements()`** now adds, after the row columns, every asserted `(stage, base name)` pair in the listing's order, without duplicates.
   - **Resolution.** `detail::resolveColumns` gains an asserted-measurement column kind, carrying the stage id and the store name to read.
   - **Values.** Per object, each writer reads the object's visible asserted measurements once: `assertedMeasurementCount`, `assertedMeasurementAt`, `measurementAssertedBy`. A column the object lacks, or whose assertion is not visible, is an empty cell (CSV) or an absent key (GeoJSON).
   - **Name clashes.** A resolved column name equal to another column's name (for example a row measurement literally named `a/b`) is an `InvalidArgumentError` at `write`, before the file is created. It is documented.
   - **Documentation.** SDS 07 §7.2 drops the "asserted measurements are not exported" note and documents this decision. `docs/project.md` marks that follow-up resolved and notes that per-row lookups grow by the assertion reads, under the existing per-row lookup follow-up.
8. **Docs.**
   - SDS 07 §7.5 records decisions 3–7.
   - SDS 01 and SDS 08 record decision 2.
   - `docs/project.md` gains a Reporting plan B entry with an open follow-up: "a layered GeoJSON import mode in `storage` that maps features to layers by name and restores parent edges from `parentId` (UR-RES-15, user decision 2026-11-21)".
   - `docs/traceability.md` gains every new test.

## Review Focus

1. **Default export:** a default exporter writes every measurement in both units, and a re-import keeps each value (`GeoJsonExport.DefaultSchemaKeepsEveryMeasurement`, Task 4).
2. **Round trip:** export, then `GeoJsonImporter` into a fresh layer, gives the same object count, geometry, classification label and measurement values (`GeoJsonExport.RoundTripsThroughTheImporter`, Task 4).
3. **Awkward text:** a layer or label name with quotes, backslashes or non-ASCII characters is valid JSON, and the round trip keeps it (`GeoJsonExport.NamesNeedingEscapesRoundTrip`, Task 4).
4. **Cancel and failure** leave no file at `path`, and an existing file survives (`GeoJsonExport.CancelledExportLeavesNoFile`, `GeoJsonExport.CancelledExportKeepsAnExistingFile`, Task 4).
5. **Refactor safety:** after the driver extraction, every CSV test passes unchanged, and an export of the CSV fixture is byte-identical to before the refactor (`CsvExportTest.OutputUnchangedByTheSharedDriver`, Task 1).

---

### Task 1: Shared export driver

**Files:**
- Create: `src/reporting/detail/export_driver.{hpp,cpp}`.
- Modify: `src/reporting/csv_writer.cpp`, `src/reporting/CMakeLists.txt`.
- Test: `tests/reporting/test_csv_writer.cpp`.

**Interfaces:**
- Produces: the driver (decision 1), used by `CsvWriter` now and `GeoJsonExporter` in Task 4.

- [ ] **Step 1: Add `CsvExportTest.OutputUnchangedByTheSharedDriver`** before refactoring.
  - Export the richest existing CSV fixture: labels, parents, WKT, twins.
  - Compare against a golden string the test builds from the current output. Paste it in from a run before the refactor; it is not a file.
  - It passes now.
- [ ] **Step 2: Extract the driver** (decision 1) and switch `CsvWriter` to it.
- [ ] **Step 3: Verify.** `./run-tests.sh`: every test passes, including Step 1's and every existing `reporting.*` test, unchanged.
- [ ] **Step 4: Commit**

```bash
git add src tests
git commit -m "refactor(reporting): shared export driver for every writer"
```

### Task 2: Parent id in `GeoJsonWriter`

**Files:**
- Modify: `include/slideio/processor/core/geojson.hpp`, `src/core/geojson_writer.cpp`, SDS 01, SDS 08.
- Test: the existing core GeoJSON writer test file (`tests/core/test_geojson*.cpp`).

**Interfaces:**
- Produces: the `writeFeature(…, const ObjectId* parent)` overload (decision 2).

- [ ] **Step 1: Write the failing tests**
  - `GeoJsonWriter.ParentIdFollowsObjectId`: with a parent, the feature text contains `"objectId":"<id>","objectType"`… and `"parentId":"<parent>"` right after `objectId`.
  - `GeoJsonWriter.NullParentMatchesTheOldOverload`: the same record written through both overloads gives byte-identical files.
- [ ] **Step 2: Run, see them fail. Step 3: Implement. Step 4: `./run-tests.sh` passes.**
- [ ] **Step 5: Commit**

```bash
git add include src tests docs
git commit -m "feat(core): GeoJsonWriter writes an optional parent id"
```

### Task 3: Later-stage measurements in exports

**Files:**
- Modify:
  - `include/slideio/processor/storage/annotation_store.hpp` and `src/storage/annotation_store.cpp` (the listing);
  - `include/slideio/processor/reporting/column_schema.hpp`, `src/reporting/column_schema.cpp` and `src/reporting/detail/columns.{hpp,cpp}`;
  - `src/reporting/csv_writer.cpp`;
  - SDS 03, SDS 07 and SDS 08, `docs/project.md` and `docs/traceability.md`.
- Test: the existing annotation store test file, `tests/reporting/test_column_schema.cpp` and `tests/reporting/test_csv_writer.cpp`.

**Interfaces:**
- Produces:
  - `AnnotationStore::assertedMeasurementColumnCount/At`;
  - `ColumnSchema::assertedMeasurement(stageName, name)`;
  - the asserted column kind in `detail::ResolvedColumns`.

  Task 4 relies on all three.

- [ ] **Step 1: Write the failing tests**
  - `AnnotationStore.AssertedMeasurementColumnsAreListed`: two stages assert `positive_fraction`, and one also asserts `area` and `area.um2`. The listing gives the four `(stage, name)` pairs ordered by stage id, then name.
  - `ColumnSchema.AllMeasurementsAddsStageQualifiedColumns`, on a store with row `area`, `area.um2`, and stage `ki67` asserting `positive_fraction`:
    - `allMeasurements()` with `Calibrated` gives `area.um2`, `ki67/positive_fraction`;
    - with `BothColumns` gives `area`, `area.um2`, `ki67/positive_fraction`.
  - `ColumnSchema.AssertedMeasurementSelectsOneStage`: `assertedMeasurement("ki67", "positive_fraction")` alone gives exactly that column.
  - `ColumnSchema.AssertedMeasurementRefusals`: null or empty stage, null or empty name, a `.um` name, and a duplicate each throw.
  - `CsvExportTest.LaterStageMeasurementsAreExported`:
    - an object with a row `area` and assertions of `positive_fraction` by stages `ki67` and `er` gives cells for `area`, `er/positive_fraction` and `ki67/positive_fraction`, each with its own value;
    - an object without assertions gives empty cells there.
  - `CsvExportTest.InvisibleAssertionIsEmpty`: an assertion by a stage that is not `Complete` gives an empty cell.
  - `CsvExportTest.ClashingColumnNamesAreRefused`: a row measurement named `ki67/positive_fraction` together with that assertion makes `write` throw `InvalidArgumentError`, and no file is created.
- [ ] **Step 2: Run, see them fail. Step 3: Implement decision 9. Step 4: `./run-tests.sh`, the docs target and the no-seams check pass.**
- [ ] **Step 5: Commit**

```bash
git add include src tests docs
git commit -m "feat(reporting): export later-stage measurements in stage-qualified columns"
```

### Task 4: `GeoJsonExporter`

**Files:**
- Create: `include/slideio/processor/reporting/geojson_exporter.hpp`, `src/reporting/geojson_exporter.cpp`, `tests/reporting/test_geojson_exporter.cpp`.
- Modify: `include/slideio/processor/reporting/reporting.hpp`, `src/reporting/CMakeLists.txt`, `tests/reporting/CMakeLists.txt`, SDS 07, `docs/project.md`, `docs/traceability.md`.

**Interfaces:**
- Consumes: the export driver (Task 1); `writeFeature(…, parent)` (Task 2); the asserted-measurement columns (Task 3); `ColumnSchema`, `detail::resolveColumns` and `ExportResult` (Plan A).
- Produces: `GeoJsonExporter` (decision 3).

- [ ] **Step 1: Write the failing tests.**
  - **Fixture.** Use the same store fixture as the CSV tests:
    - a parent layer and a child layer with parent edges;
    - standard, local and asserted labels;
    - calibrated twins and a hole polygon.
  - **Parsing.** Parse the output with RapidJSON in the test, which is already a test dependency through core.
  - **The tests:**
    - `GeoJsonExport.WritesAFeatureCollection`:
      - the top level is a `FeatureCollection` with a `provenance` object;
      - one feature per object, in cursor order;
      - each feature has `objectId`, `objectType: "detection"`, `layer`, a `labels` array with `workspaceLocal` flags, and `classification.name` equal to the first label;
      - a hole polygon gives a Polygon with two rings.
    - `GeoJsonExport.DefaultSchemaKeepsEveryMeasurement`: with no schema, a calibrated store gives `area` and `area.um2` for each object, with the stored values.
    - `GeoJsonExport.SchemaSelectsMeasurementsAndUnits`: `measurement("area")` with `Calibrated` gives only `area.um2`; `PixelsOnly` gives only `area`.
    - `GeoJsonExport.ProducerOnlyLabels`: the `labels` array holds the row label alone.
    - `GeoJsonExport.ParentIdWhenRequested`: `includeParent(true)` gives `parentId` on children and none on roots. Without it, no feature has `parentId`.
    - `GeoJsonExport.NonFiniteMeasurementIsOmitted`: a `+Inf` measurement is missing from that feature, and the feature's other measurements are present.
    - `GeoJsonExport.RoundTripsThroughTheImporter`:
      - export a layer;
      - import into a new layer of a second store with `GeoJsonImporter`;
      - the counts are equal, and the geometry of each re-imported object equals the original's (compare by sorted ring point lists);
      - the classification label is equal;
      - the measurement values are equal by name.
    - `GeoJsonExport.NamesNeedingEscapesRoundTrip`: a local label named `a"b\c` and a layer named `Zellkerne ü` survive the round trip and parse as valid JSON.
    - `GeoJsonExport.ExportIsDeterministic`: two exports are byte-identical.
    - `GeoJsonExport.CancelledExportLeavesNoFile` and `GeoJsonExport.CancelledExportKeepsAnExistingFile`.
    - `GeoJsonExport.ReportsProgress`: progress after 1000, 2000 and at the end for 2500 objects.
    - `GeoJsonExport.BytesWrittenIsTheFileSize`.
    - `GeoJsonExport.LaterStageMeasurementsAreKeys`: an object with a `ki67` assertion of `positive_fraction` has the key `ki67/positive_fraction` in its `measurements`, with the asserted value.
- [ ] **Step 2: Run, see them fail. Step 3: Implement decisions 3–8. Step 4: `./run-tests.sh`, the docs target and the no-seams check pass.**
- [ ] **Step 5: Commit**

```bash
git add include src tests docs
git commit -m "feat(reporting): GeoJSON export, QuPath-shaped, with parents and selected measurements"
```
