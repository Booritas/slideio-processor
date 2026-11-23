# Python bindings — design

Status: for approval, 2026-11-22.
Requirements: UR-API-1 to UR-API-5 (`docs/requirements.md` §13), UR-EXT-5,
UR-RES-11, UR-QA-3, §9a (observation).

## 1. Intent

Make slideio-processor usable from Python, so that:

1. **A pharma researcher who writes Python** can develop a pipeline on a few
   slides, including stages that run their own models (PyTorch and similar),
   and extract results as files: CSV, GeoJSON, label masks.
2. **A pathologist** can drive processing through MCP tools built on the
   bindings, and review the results in `slideio-view`.

Later, the same pipeline runs as a batch over many slides, on a cluster or a
workstation, with reliable coarse progress and exported files as the main
product.

### Goals for the first version

- Single-slide processing on a workstation, from Python and from a command line.
- Custom Python operations inside the pipeline, tile by tile, producing a
  per-tile label image (turned into objects) or a per-tile raster.
- A pipeline file (stages and exports) that describes a run completely and is
  the artefact later batch processing reuses unchanged.
- Results readable from Python: queries, cursors, geometry and measurements as
  NumPy arrays; rasters as NumPy views without a copy.
- Exports to CSV, GeoJSON and raster or label-mask TIFF.
- Live review: `slideio-view` watches the workspace while the run writes it.

### Non-goals for the first version (designed for, built later)

- Batch processing: `run_batch`, the batch store mode, per-slide status files,
  the batch manifest, cluster use. Section 8 says how the first version leaves
  room for them.
- Parquet export. CSV is enough to start.
- Per-object classification by a model (an image patch per object in, a label
  out).
- A cross-slide results reader.
- The MCP server itself. It is a separate project built on the bindings; this
  design only has to make it possible (section 7).

### Anti-goals

- A second copy of the SlideIO C++ library in the process (D2).
- Two definitions of the pipeline file, one in C++ and one in Python (D4).
- Slide paths, slide metadata or pixel values in errors, status or exports.
  Slide names and metadata can carry patient identifiers. The library's
  existing message rules hold for everything the bindings add. Under HIPAA and
  GDPR, the MCP layer will need its own review of what it sends to a model.
  Revoked for errors and logs on 2026-11-23 (paths-in-diagnostics design); exports unchanged.

## 2. Decisions

### D1. One Python package wraps both native libraries

The C++ library builds as `slideio-processor-data` (core, config, storage,
reporting; no OpenCV) and `slideio-processor-engine` (pipeline, algorithm,
workspace, masks; SlideIO and OpenCV). The Python package wraps both in one
wheel.

*Why:* SlideIO's own libraries, and so the `slideio` wheel, already carry OpenCV,
so a reader-only Python package saves little for anyone who also opens the
slide. One wheel means one version and one release. The Python API does not
expose the native split, so a reader-only wheel can be added later without
changing it.

### D2. One copy of the SlideIO C++ library per process, owned by `slideio`

The processor wheel ships no SlideIO libraries. It declares an exact dependency
on the `slideio` package (`slideio==X.Y.Z`) and loads the SlideIO shared
libraries that `slideio` installs in `slideio/core/libs`:

- Linux and macOS: a relative run path from the processor's native libraries
  into `slideio/core/libs`; `auditwheel repair` and `delocate` exclude
  `libslideio*`.
- Windows: the package imports `slideio` first and adds `slideio/core/libs` to
  the DLL search path before loading its own module.
- The native module is built against the same SlideIO release, with the same
  compilers and CI images as the `slideio` wheels.
- On import, the package compares the SlideIO version it was built against with
  the version `slideio` reports and raises `ImportError` naming both if they
  differ.

*Why:* two copies cannot coexist. On Windows the loader reuses whichever
`slideio.dll` was loaded first, by name, so a mismatched copy fails as a crash.
On Linux and macOS the copies stay separate, and a scene opened by one cannot be
used by the other.

*Cost:* every SlideIO release needs a matching processor wheel, and the
processor's `extern/slideio` pin must be a SlideIO release published on PyPI.

*OpenCV is not shared and needs no handling (call made during design):* the
engine links OpenCV statically and privately, with its symbols hidden
(`src/CMakeLists.txt`), and SlideIO's libraries embed their own static OpenCV
the same way. Neither wheel ships an OpenCV shared library, so the two copies
never meet.

### D3. `slideio-python` exposes a scene handle: `Scene._native_handle()`

A small addition to `slideio-python` (`src/pybind.cpp`, on the `Scene` class):

```cpp
.def("_native_handle", [](std::shared_ptr<PyScene> self) {
    auto* scene = new std::shared_ptr<slideio::Scene>(extractScene(self));
    return py::capsule(scene, "slideio.Scene",
        [](void* p) { delete static_cast<std::shared_ptr<slideio::Scene>*>(p); });
})
```

It returns a `PyCapsule` named `"slideio.Scene"` holding a heap-allocated
`std::shared_ptr<slideio::Scene>`, built with the existing `extractScene`. The
processor's binding checks the capsule's name, copies the shared pointer out,
and keeps a reference to the Python `Scene` object for as long as it uses the
scene, since `PyScene` also holds the `Slide`.

*Why not call `extractScene` directly:* it is an internal function of the
`slideiopybind` module. Extension modules export no C++ symbols, and its
argument, `std::shared_ptr<PyScene>`, would need pybind11 to share the `PyScene`
type across modules, which works only with the same pybind11 version, compiler
and flags. The capsule is an explicit, testable contract between the two
packages.

*Why it is safe:* the capsule carries a pointer into the one shared copy of the
SlideIO library (D2).

*Contract:* the method name and capsule name are a public contract between the
two packages, though the leading underscore keeps the method out of the
documented user API. Changing either is a breaking change for both packages.

### D4. The pipeline file and the slide run live in C++; Python is a thin layer

`slideio-processor` gains the pipeline file and `SlideRun` (section 4). The
planned `slideio-process` CLI (module design §5.3) wraps them. The Python
package binds them, registers Python operations, and adds a Python CLI and,
later, the batch helper.

*Approaches considered:*

- **A. Everything above the raw API in Python.** Quickest to change, but the
  C++ CLI would need its own copy of the format and the runner, and the two
  would drift. C++ users would get no batch support.
- **B. Pipeline file and runner in C++, Python thin.** Chosen. One format and
  one runner for C++, the CLI and Python. A batch file can be checked without
  Python, matching the store's rule that a record is readable without the code
  that produced it (UR-RES-11). More C++ work before the bindings are useful.
- **C. A high-level-only Python API.** Dropped: a researcher developing a
  pipeline needs stage definitions, operation parameters and queries, and
  UR-API-3 and UR-API-4 require zero-copy blocks and cursors anyway.

### D5. Batch runs: a single-slide entry point, with a helper on top

The unit of work is one slide in one process. A batch is many such runs: a
`run_batch(pipeline, slides, parallel=N)` helper on a workstation, or an
external scheduler calling the single-slide CLI on a cluster. The first version
delivers only the single-slide run; section 8 lists what batch adds.

*Why one process per slide:* the GIL, each slide's memory budget and failures
stay separate. GPU memory is the practical limit on `N`: each process holds its
own CUDA context and model copy.

### D6. Model output: per-tile label images and per-tile rasters first

A Python operation returns either a raster for the tile (probabilities,
classes, any planes) or a label image that the framework turns into objects.
Per-object classification comes later.

## 3. Work breakdown

Three projects, planned and built in this order:

1. **slideio-processor (C++):** the pipeline file, `SlideRun`, the label-image
   ownership mode, and the operation nondeterminism flag (section 4).
2. **slideio-python:** `Scene._native_handle()` (D3), released with the SlideIO
   version the processor pins.
3. **The bindings repository**, `slideio-processor-python` (section 5, D7).

## 4. Changes to slideio-processor

### 4.1 Pipeline file

A versioned JSON document describing one slide run completely:

- `formatVersion`;
- `requires`: Python modules that provide operations, as import paths, for
  example `["mylab.models"]`. C++ treats the list as opaque: a host that can
  import Python modules loads them before resolving operations. The C++ CLI,
  which cannot, fails with an error naming the first operation the catalog
  lacks;
- `layers` and `rasters` to declare, by name;
- `stages`: each in the existing stage-record form (`04-workspace.md`, the
  `stageRecordVersion` JSON: layers by name, `dependsOn`, the embedded
  `PipelineSpec::toJson`);
- `exports`: each names its kind (`csv`, `geojson`, `rasterTiff`, `labelMask`),
  what it exports (a layer, a query, a raster) and its options, and a relative
  output file name.

It round-trips: a pipeline file can be produced from a development workspace's
declared stages plus an export list, and loading it validates every name and
parameter before anything runs.

### 4.2 SlideRun

Runs one pipeline file against one slide scene:

1. creates or opens the workspace for the scene;
2. declares the file's layers, rasters and stages; an identical existing
   declaration is a no-op, and a different one makes the stage stale, by the
   existing staleness rules;
3. runs every stage that is not complete and current, in dependency order;
4. writes each export atomically (to a temporary name, then renamed) into an
   output directory.

It takes a progress observer and a cancellation token, and returns a result:
for each stage, whether it ran or was skipped as current, the objects written,
and the label-image halo-overflow count (4.3); for each export, the file
written and its size.

The pipeline file names no slide: the slide, the scene index (default 0), the
workspace directory and the export directory are run arguments. A workspace
already bound to a different slide or scene is refused, by the existing
provenance check, rather than reused. Batch-only hooks (status file, manifest entry) are left out of the
first version, but the run is structured as those steps so they can be added
around it (section 8).

### 4.3 Label-image ownership mode

For operations whose output is a per-tile label image with tile-local values
(the usual model output, where one object can carry different values in
neighbouring tiles and unrelated objects can share a value):

- the operation declares a halo at least as wide as the largest object;
- the framework traces each label in the halo-extended image and keeps only
  objects whose anchor lies inside the tile; objects are therefore complete,
  and no cross-tile joining is needed;
- the anchor is the one object ids already derive from: the object's
  topmost-then-leftmost boundary vertex (`05-pipeline.md`, `ObjectId::fromAnchor`).
  An object is kept when its anchor lies in the tile's half-open rectangle, so
  exactly one tile keeps it and results do not depend on tile size or worker
  count;
- objects are traced as `segment.contours` traces them: each maximal
  4-connected set of pixels of one non-zero value is one object, with holes;
  touching objects of different values share edges. Each gets the same
  geometry measurements as `segment.contours` (`area`, `perimeter`, and their
  calibrated forms), and the stage's measuring and finishing apply as for any
  object-producing operation;
- the halo-extended tile counts against the memory budget through the
  operation's declared per-tile working set, by the existing checks; a halo
  too large for the budget is refused before the run;
- an object that touches the outer edge of the halo means the halo was too
  small; the framework counts such objects, reports the count in the run
  result, and never stores a cut-off object.

This lives in the C++ library, not the bindings: a C++ user with their own
model has the same need, and it stays deterministic and testable without
Python. It is a separate mode from `segment.contours`, which joins seam
fragments by equal value and so needs globally consistent labels.

### 4.4 Operation nondeterminism flag

`OperationFactory` gains a flag saying the operation's results may vary from run
to run (GPU models usually do). The stage record stores it, and the UR-QA-3
byte-identity checks exempt such stages, naming them. The flag does not affect
staleness.

### 4.5 Store mode for live viewing

No change: the store's WAL mode already lets `slideio-view` read while the run
writes, on the same machine (§9a).

## 5. The Python package

### 5.1 Structure

Following `slideio-python`: a pybind11 module, a raw import layer, and a
Pythonic wrapper re-exported from the package root. Platforms and Python
versions follow `slideio-python`'s wheels (Windows x64, manylinux_2_28 x86_64,
macOS arm64 and x86_64). Dependencies: `slideio==X.Y.Z` (D2), `numpy`; `pandas`
as an optional extra for DataFrame conversions.

### 5.2 Binding surface for the first version

- **core:** ids, geometry, `BlockView` as a NumPy view without a copy
  (UR-API-3), the error hierarchy mapped to a matching Python exception
  hierarchy (UR-API-5).
- **config:** configuration and logging.
- **storage and workspace:** create and open workspaces (from a `slideio.Scene`
  via D3, or from a path), layers, stage definitions, the stage registry,
  `StageRunner` with progress and cancellation.
- **queries and results:** object queries; cursors as Python iterators that do
  not materialise the result set (UR-API-4); geometry and measurements as NumPy
  arrays per object or per batch; DataFrame helpers when `pandas` is present.
- **pipeline and algorithm:** `PipelineSpec`, operation parameters, the
  operation catalog and its built-in operations by name.
- **reporting and masks:** CSV, GeoJSON, raster TIFF and label-mask export.
- **runs:** the pipeline file and `SlideRun` (section 4), and a
  `slideio-processor run --pipeline <file> --slide <path> [--scene <index>]
  --workspace <dir> --out <dir>` command.

### 5.3 Python operations

A researcher subclasses a base class, declares the operation's contract as
class attributes, and implements one method:

```python
class NucleiSegmenter(slideio_processor.Operation):
    name = "mylab.nuclei"
    version = "1.2"                            # required; part of the declaration
    output = slideio_processor.Output.LABELS   # or Output.RASTER
    halo = 64                                  # level pixels
    deterministic = False
    max_concurrency = 1                        # calls at once; None = unlimited
    parameters = {"weights": ""}               # name -> default; the type follows the default

    def __init__(self, parameters):
        self.model = load_model(parameters["weights"])   # once, shared by workers

    def apply(self, tile, context):            # tile: NumPy view, halo included
        return self.model.predict(tile)        # label image or raster, same extent
```

Registering the class registers a C++ stand-in factory in the operation
catalog. It forwards each tile to Python and returns the array to the
framework, which crops the halo and either writes the raster or applies the
label-image ownership mode (4.3).

- **Registration:** registering happens at import time through
  `OperationCatalog::registerFactory`, which is thread-safe and accepts
  user-defined operations at run time. The pipeline file's `requires` list is
  imported before its operations are resolved.
- **Output:** `Output.RASTER` operations also declare `output_dtype` (default
  `float32`) and `output_planes` (default 1); the result goes to the stage's
  `writesRaster`. `Output.LABELS` operations return an `int32` label image;
  their objects go to the layer named by the standard `targetLayer`
  parameter, as for built-in segmenters. The returned array must have the
  input's extent, halo included, and the declared type and plane count;
  anything else fails the run with an error naming the operation.
- **Parameters:** `parameters` declares every accepted key with its default.
  An unknown key or a value of the wrong type is refused when the pipeline is
  loaded, as for built-in operations, with the key named and never the value.
- **Staleness:** `version` is stored with the operation's parameters in the
  stage declaration. Changing it, or any parameter such as the weights path,
  makes the declaration differ, so the stage is stale by the existing rules
  and re-runs. Changing code without bumping `version` does not; that is the
  researcher's responsibility, and the documentation says so. No store format
  change is needed.
- **Threads and the GIL:** a run releases the GIL on the calling thread. Worker
  threads take the GIL for each call into Python. Libraries such as PyTorch
  release it while they compute. `max_concurrency` limits simultaneous calls,
  for a GPU that should see one call at a time.
- **One instance:** the operation object is created once per run and shared by
  all workers, so a model is loaded once.
- **Errors:** an exception in `apply` cancels the run and surfaces as the
  library's error type for a failed operation, with the Python exception
  chained as its cause. The message names the operation, never slide data.
- **Ctrl-C** during a run sets the run's cancellation token.
- **Provenance:** the stage record notes the operation's module, its
  `__version__` if it has one, and the nondeterminism flag (UR-RES-11).

## 6. Live review in slideio-view

The workspace stays in WAL mode, so `slideio-view` on the same workstation sees
layers and objects appear while the run writes them, and opens the finished
workspace afterwards.

## 7. What the MCP layer needs from the bindings

- Runs that do not block: `SlideRun` and `StageRunner` usable from a background
  thread with the GIL released, with progress callbacks and a cancellation
  handle.
- Typed exceptions whose messages contain no slide metadata or pixel
  values.
- Result summaries (counts, measurement statistics) computable without
  exporting files. In the first version these are small Python helpers over
  queries and cursors; no C++ is needed for them.

## 8. Room for batch processing (not built in the first version)

- **Batch store mode:** a storage open option using SQLite's rollback journal
  and an exclusive lock for the whole run, for cluster file systems where WAL
  is unreliable and nobody watches live. Writing to node-local scratch and
  copying at the end stays optional.
- **Status file per slide:** queued, running (stage and progress), done,
  failed (error type and message), replaced atomically; a clear exit code.
- **Batch manifest:** for every slide, the files written and the outcome.
- **`run_batch(pipeline, slides, parallel=N)`** on a workstation; a `status`
  command that reads the status files on a cluster.
- **Slide identifiers:** exports and the manifest name slides by an ID the
  researcher supplies, never by file name, and copy no slide metadata.

Each is an addition around `SlideRun`; none changes the pipeline file.

## 9. Repository and release prerequisites

- **D7. The bindings live in a new repository, `slideio-processor-python`,**
  with `extern/slideio-processor` as a submodule (which carries
  `extern/slideio` in turn). CI checks that the nested SlideIO pin is the
  commit of the `slideio` release the package depends on. *Why not the
  `slideio-python` repository:* it would match SlideIO versions by
  construction, but would tie every processor release to a SlideIO release.
- **SlideIO on PyPI.** The processor pins SlideIO 2.11.0 (`edda013a`), and
  `slideio` 2.11.0 is published, so the first wheel can depend on
  `slideio==2.11.0`.

## 10. Left to the builder

- Python module and class names beyond those above, and the wrapper layout.
- The exact JSON spelling of export entries.
- How cursors batch rows internally, within the memory ceiling.
- The test layout in the bindings repository.
