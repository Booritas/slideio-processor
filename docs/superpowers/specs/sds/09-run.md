# SDS 09 — `run`

**Module:** `run` · **Library:** `slideio-processor-engine` · **Depends on:** `core`, `config`, `pipeline`, `storage`, `reporting`, `workspace`, `masks` (and so SlideIO)

The whole-slide run: a pipeline file that describes what to do to a slide, and
`SlideRun`, which does it to one slide scene. Added by the slide-run work
(Python bindings project 1; design
[`2026-11-22-python-bindings-design.md`](../2026-11-22-python-bindings-design.md)
§4.1, §4.2 and §8). It is the single-slide entry point that the planned
`slideio-process` CLI, the Python bindings and later batch processing wrap.

Conventions in [00-conventions.md](00-conventions.md).

---

## 9.1 Dependencies

`run` is an engine OBJECT library, linked into the engine. It depends on
`workspace` (the workspace, its registries, `StageRunner`), `masks`
(`rasteriseLabels`, `MaskWriter`), `pipeline` (`PipelineSpec`,
`OperationCatalog`, the level table) and the data library (`reporting`'s writers,
`storage`'s registries, `core`, `config`).

It never depends on `algorithm`. A run reaches operations by name through
`OperationCatalog`, the same way `workspace` does, so the catalog's contents —
built-ins, a host's own operations, Python operations registered by the
bindings — can change without touching `run`. `src/CMakeLists.txt` fails the
configure if `slideio-processor-run` ever links `slideio-processor-algorithm`.
Its tests may link the algorithm test support (`tests/algorithm/support`, the
`test.disks` label-image operation); the library may not.

---

## 9.2 Pipeline file — `run/pipeline_file.hpp`, `run/export_spec.hpp`

A versioned JSON document describing one slide run completely, naming no slide.

```json
{ "formatVersion": 1,
  "requires": ["mylab.models"],
  "layers":  [ { "name": "tissue" },
               { "name": "nuclei", "parent": "tissue", "kind": "semantic" } ],
  "rasters": [ { "name": "prob", "type": "Float32", "planes": 1, "compression": "Deflate" } ],
  "stages":  [ <stage record, the stageRecordVersion 1 form of SDS 04 §4.2> ],
  "exports": [ { "kind": "csv", "file": "nuclei.csv", "layer": "nuclei", "measurements": ["area"] },
               { "kind": "geojson", "file": "nuclei.geojson", "layer": "nuclei" },
               { "kind": "labelMask", "file": "nuclei-mask.ome.tif", "layer": "nuclei" },
               { "kind": "rasterTiff", "file": "prob.ome.tif", "raster": "prob" } ] }
```

- **`requires`** lists Python modules that provide operations. C++ treats it as
  opaque: a host that can import Python modules loads them before the run
  resolves operations; the C++ CLI, which cannot, fails naming the first
  operation the catalog lacks.
- **Defaults.** A layer's `parent` defaults to the pixel layer and its `kind` to
  `"semantic"` (the other kind is `"primitive"`, which has no parent); a raster's
  `compression` to `RasterDeclaration`'s default, Deflate; a CSV export without
  `measurements` exports every measurement (`ColumnSchema::standard().allMeasurements()`).
- **Stages** are kept as the records they are, unknown keys included (the
  optional `nondeterministicOperations` of SDS 04 among them). Their layer and
  raster names are resolved when the file is run, not when it is read.

`class PipelineFile` (copyable, pimpl): `fromJson(const char*)`,
`load(const char* path)`, `toJson(char*, int32_t)` sized as
`PipelineSpec::toJson` is, the builders `require` and `addExport`,
count-and-index readers for requires, layers (name, parent or null, kind),
rasters (`RasterDeclaration`), stages (name, record JSON) and exports, `equals`,
and `fromWorkspace(Workspace&)`, which takes every non-pixel layer, every raster
and every stage record of a development workspace. The raster registry cannot
report a raster's compression, so `fromWorkspace` omits it and the default
applies when the file is loaded. `fromJson(toJson(x))` gives an equal file.

`class ExportSpec` (copyable, pimpl): `kind()` (`enum class ExportKind { Csv,
GeoJson, LabelMask, RasterTiff }`), `file()`, `layer()`, `raster()`, and the
measurements by count and index; built through a named constructor per kind
(`csv`, `geoJson`, `labelMask`, `rasterTiff`) and, for CSV, `measurement()`.

**Refused by `fromJson`** with `InvalidArgumentError`: a wrong `formatVersion`;
a missing, mistyped or unknown member; an unknown export kind; duplicate layer,
raster, stage or export-file names (export files compared case-insensitively);
a layer parent that is not an earlier layer; a stage whose `pipeline` does not
parse with `PipelineSpec::fromJson`; an export `file` that is empty, `.`, `..`,
or contains `/`, `\` or `:`; and an export naming a layer or raster the file
neither declares nor has a stage write. Messages name stages, layers, rasters,
export kinds and members, never values or paths.

---

## 9.3 `SlideRun` — `run/slide_run.hpp`

```cpp
class SlideRun {                                       // ThreadConfined, pimpl
public:
    SlideRun(const PipelineFile& file, const char* slidePath, int32_t sceneIndex,
             const char* workspacePath, const char* exportDirectory,
             const Configuration& configuration);
    SlideRun& progress(ProgressObserver* observer) noexcept;
    SlideRun& cancellation(const CancellationToken& token) noexcept;
    SlideRun& slideId(const char* id);                 // default ""
    SlideRunResult run();
};
```

The slide, the scene index, the workspace directory and the export directory
are run arguments; the file names none of them. The constructor opens nothing
and refuses only null or empty paths and a negative scene index.

`run()` works in five steps, in this order.

1. **Validate operations.** For every operation of every stage, in file order,
   `OperationCatalog::isRegistered` must hold; the first that does not is an
   `InvalidArgumentError` naming the operation and its stage. Then each is built
   with `OperationCatalog::create(name, parameters)`, whose errors propagate.
   Nothing has been created yet: a bad file leaves no workspace and no export
   directory. Validation must come before declaration (step 3): an operation
   unregistered when its stage is declared counts as deterministic, so its
   stage record would lack the `nondeterministicOperations` key (SDS 04).
2. **Open or create the workspace.** The workspace exists when its directory
   holds a store file, `<workspace>/workspace.store`, which is what
   `Workspace::open` requires (Workspace's fixed layout, SDS 04 §4.1). An
   existing workspace is first opened with `Workspace::openReadOnly` to read its
   binding. When its slide is not `std::filesystem::equivalent` to `slidePath`,
   or its scene index differs, the run is refused with an `InvalidArgumentError`
   whose message, "SlideRun: the workspace belongs to another slide or scene",
   names neither path. A refused run leaves the store's contents,
   its stage records and every file other than SQLite's `-wal` and `-shm`
   sidecars unchanged, and creates no other file or directory. The sidecars are
   SQLite's housekeeping, not data: the read-only open may create or touch them,
   as any viewer's open does, but it takes no writer lock, clears no scratch
   and migrates no store. `SlideRun.RefusesAWorkspaceOfAnotherSlide` snapshots
   every file and directory of a cleanly closed workspace (no sidecars) and
   compares their bytes after each refusal, sidecars excepted, with the stage
   records. Otherwise the workspace is opened
   with `Workspace::open`. Any other path is made with `Workspace::create`.
3. **Declare.** Each layer of the file is declared through `LayerLifecycle`,
   unless the workspace has a layer of that name: it is reused when it has the
   same kind and, for a semantic layer, the same parent, and refused otherwise
   with an `InvalidArgumentError` naming it. Each raster likewise, compared by
   plane type and plane count. Then every stage is built with
   `detail::stageFromRecord` and declared with `StageRegistry::declare`. An
   identical declaration changes nothing; a changed one rewrites the record and
   marks the stage and its dependents stale, by the existing rules (SDS 04 §4.3).
4. **Run stages.** Repeatedly, the first stage in file order that is not
   `Complete`, or is stale, and is runnable runs with a `StageRunner`. When
   stages still need running but none is runnable, the first of them is refused
   with a `StageError` naming it and its `RunBlocker` (the unresolved or blocking
   dependency's name included). Workspace stages the file does not name are left
   alone. The staleness graph is acyclic, so the loop ends.
5. **Write exports.** The export directory is made when missing. Each export is
   written to `<file>.partial` in it and renamed onto `<file>` only once
   complete; the writers own that protocol (SDS 07 §7.3, §7.6), so `SlideRun`
   passes them the final path. The kinds map as follows:

   | Kind | Writer | Reads |
   |---|---|---|
   | `csv` | `CsvWriter`, schema `ColumnSchema::standard()` with the listed measurements, or `allMeasurements()` | `ObjectQuery().layer(layer)` |
   | `geojson` | `GeoJsonExporter`, default schema | `ObjectQuery().layer(layer)` |
   | `labelMask` | `rasteriseLabels` over the scene's full level-0 rect, then `MaskWriter` | the layer's objects |
   | `rasterTiff` | `RasterTiffWriter`, named after the raster | `RasterRegistry::sourceFor(raster)` over the whole scene, at the scale of `writtenAtLevel` in the scene's level table |

   The label mask and the raster carry the slide's x pixel size, scaled to the
   output, when the slide reports one.

   **No slide path in any export.** The workspace records the slide path, and a
   slide file name can carry patient identifiers (PHI under HIPAA, personal data
   under GDPR; the design's anti-goals forbid slide paths in exports). Every CSV
   and GeoJSON export is written with `slideId(id)` set to the run's
   `slideId()`, empty by default, so its provenance carries that id in place of
   the path (SDS 07 §7.3, §7.5); the rest of the provenance is unchanged. The
   label-mask and raster TIFF exports carry no identifier at all: their OME-XML
   holds only the image name (the raster's name, or empty for a mask) and the
   pixel size. `SlideRun.ExportsCarryNoSlidePath` checks every export file's
   bytes for the path as given, its absolute form and its file name. An export naming a layer or raster the
   workspace lacks, or a raster no stage has written, is an
   `InvalidArgumentError` naming it.

**Progress and cancellation.** The observer and token are forwarded to every
`StageRunner` and every writer, and the token is also checked before each
export. A cancelled run throws `CancelledError`. A writer removes its partial
files when cancelled or failing, so no `.partial` file is left; an export file
a previous run completed is left as it was, because a rename replaces it only
on completion.

**The workspace** is closed when `run()` returns or throws, so its writer lock
is released and a second `SlideRun` over it, or a `Workspace::open`, can follow.

**Messages** name the file's entities — stages, layers, rasters, operations,
export kinds — never paths, slide names or values. Errors from the workspace,
the runner and the writers propagate as they are.

**Determinism (UR-QA-3).** A second run of an unchanged file runs nothing and
rewrites byte-identical CSV and GeoJSON. Two runs of one file over one slide
into two fresh workspaces give CSV and GeoJSON that differ only in the
provenance's store creation time (`created_at_unix_ms` / `createdAtUnixMs`),
which SDS 07 §7.1 rule 2 carries in every export; `SlideRun.ExportsAreDeterministic`
blanks that one value and compares the rest byte for byte.

---

## 9.4 `SlideRunResult` — `run/slide_run_result.hpp`

Copyable, pimpl, count-and-index accessors:

- per stage, in file order: `stageName`, `stageRan` (false when it was
  `Complete` and current), `stageObjectsWritten` and `stageTruncatedObjects`
  (`StageRunner::objectsWritten()` and `truncatedObjects()` of its run; 0 when
  it did not run);
- per export, in file order: `exportFile` (the plain name of the file) and
  `exportBytes` (`ExportResult::bytesWritten()`, the file's size).

A run that throws returns no result.

---

## 9.5 What batch processing adds later

The first version runs one slide in one process (design D5). Batch processing
is a set of additions around `SlideRun`, none of which changes the pipeline
file (design §8):

- **Batch store mode:** a storage open option using SQLite's rollback journal
  and an exclusive lock for the whole run, for cluster file systems where WAL
  is unreliable.
- **Status file per slide:** queued, running (stage and progress), done, failed
  (error type and message), replaced atomically; a clear exit code. `run()`'s
  five steps are where it would be updated.
- **Batch manifest:** for every slide, the files written and the outcome, from
  `SlideRunResult`.
- **`run_batch(pipeline, slides, parallel=N)`** on a workstation, and a
  `status` command reading the status files on a cluster.
- **Slide identifiers:** exports and the manifest name slides by an ID the
  researcher supplies, never by file name. Each slide's id reaches its exports
  through `SlideRun::slideId()` (§9.3).
