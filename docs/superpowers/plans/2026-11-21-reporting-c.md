# Reporting Implementation Plan C — Label Masks and Raster TIFFs

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Label masks and other maps are obtainable as multi-resolution rasters (UR-RES-7):
- `rasteriseLabels` gives a lazy SlideIO scene of a query's objects, readable at any resolution;
- `MaskWriter` writes any SlideIO scene, such as that scene or a pipeline scene, as a lossless pyramidal OME-TIFF;
- the written file reopens in SlideIO with the expected levels and dimensions.

**Architecture (user decisions, 2026-11-21):**
- **Where it lives.** The mask features need SlideIO at run time, and SlideIO's DLLs embed OpenCV. That is forbidden in the data library (design 5.1, ruling S40, enforced by `src/CMakeLists.txt`). So `rasteriseLabels` and `MaskWriter` live in the engine library, in a new engine module `masks`.
- **How files are written.** slideio-converter writes only JPEG (8-bit) or JPEG 2000, so it cannot write lossless 32-bit label masks, and it is not used. Masks are written by storage's existing lossless pyramidal TIFF writer instead.
  - It gains an OME-TIFF layout option: OME-XML in page 0, reduced levels as SubIFDs. That is the layout SlideIO's OME-TIFF driver reads.
  - It is exposed in the data library as `RasterTiffWriter`, which writes any `TileRasterSource`.
- **Data library.** It also gains a query-driven label raster source, `makeLabelMaskSource`, so a data-only consumer can rasterise and write a label mask with no SlideIO at all.

**Tech Stack:** C++17, CMake, libtiff (already in storage), SlideIO scene-source seam (`slideio::makeScene`, `ISceneSource`; engine only), GoogleTest.

**Spec:**
- `docs/superpowers/specs/sds/07-reporting.md` §7.6, to be rewritten by this plan.
- `docs/superpowers/specs/2026-09-28-module-structure-design.md` §4–5 (module graph, library split).
- `docs/superpowers/specs/sds/03-storage.md` §3.8 (raster store, pyramid layout).
- Requirements: UR-RES-7, UR-QA-3.

**Base:** branch `feat/reporting-c` from `main` (7413454 or later).

## Global Constraints

- **Commands:**
  - Build: `./build.sh --skip-slideio --warnings-as-errors`.
  - Tests: `./run-tests.sh`; all must pass.
  - One test: `ctest --test-dir build -C Release -R <name>`.
  - Docs: `cmake --build build --target docs --config Release`.
  - No-seams check: `./build.sh --skip-slideio --no-test-seams --warnings-as-errors` then `scripts/check-no-test-seams.sh build-noseams`.
- Zero compiler warnings (read the real output) and zero Doxygen warnings.
- **Library split:**
  - The data library still links no `slideio::` target and no OpenCV. The existing configure check must keep passing.
  - The new `masks` module is an engine OBJECT library. It depends on `core`, `config`, `storage`, `reporting` (data) and `slideio::slideio`, and never on `algorithm` or `workspace`.
- **Store format v2 is unchanged.**
  - The OME-TIFF layout is opt-in.
  - `RasterRegistry`'s own pyramid files stay byte-identical, and the existing storage raster tests and the v1 and v2 golden stores pass unchanged.
- **Lossless.**
  - Compression is None, LZW or Deflate only.
  - Downsampling keeps storage's rules: integer types take the top-left sample of each 2×2 block, floating types the mean. A label value is never blended.
- **No identifiers.** No slide path, store path or other identifier is written into an OME-XML or ImageDescription. A file's name in the metadata is the caller's `name()`, by default empty.
- Messages never carry pixel, coordinate or parameter values, names or paths.
- **UR-QA-3:** two writes of the same source with the same settings give byte-identical files.
- **Requirements lines and traceability:** every new source and test file starts with its `// Requirements: ...` line, and `docs/traceability.md` lists every new test.

## Decisions

1. **OME-TIFF layout in storage's writer.**
   - `detail::writePyramidTiff` gains a layout parameter, `enum class PyramidLayout { Sequential, OmeSubIfds }`, defaulting to `Sequential`, today's behaviour.
   - With `OmeSubIfds`:
     - page 0 is the full-resolution image;
     - its reduced levels are written as SubIFDs of page 0, with the same tiling, compression, planes and downsampling as today;
     - page 0's ImageDescription is minimal OME-XML: one Image with Pixels `SizeX`, `SizeY`, `SizeC` = planes, `SizeZ`=1, `SizeT`=1, `DimensionOrder="XYCZT"`, `Type` from the data type, `Interleaved="false"`, and `PhysicalSizeX`/`PhysicalSizeY` in µm when a resolution greater than 0 is given;
     - the image name is the metadata name, and it may be empty.
   - Planes are separate (`PLANARCONFIG_SEPARATE`), as today.
   - Task 1 confirms the exact attributes against SlideIO's OME-TIFF driver (`extern/slideio/src/slideio/drivers/ome-tiff`) and records them in the report.
2. **`RasterTiffWriter`** (data library, `reporting/raster_tiff_writer.hpp`; ThreadConfined; pimpl). Members:
   - `explicit RasterTiffWriter(const char* path)`;
   - `RasterTiffWriter& compression(RasterCompression)`, default `Deflate`;
   - `RasterTiffWriter& resolution(double micronsPerPixel)`: 0 means unknown, the default, and a negative or non-finite value is `InvalidArgumentError`;
   - `RasterTiffWriter& name(const char*)`, default empty;
   - `ExportResult write(TileRasterSource& source, const Rect64& regionLevel0, double scale, ProgressObserver*, const CancellationToken&)`.

   `write` behaves as follows:
   - It rasterises `regionLevel0` at `scale` (level-0 pixels per output pixel, at least 1) into a `TileSpool`, tile by tile, asking `source.fillTile` for each 512×512 output tile.
   - It writes the spool with `PyramidLayout::OmeSubIfds` to `<path>.partial`, then renames it onto `path`.
   - The data type and planes are `source.planeType()` and `planeCount()`.
   - **Progress and cancellation:** progress is reported per tile, with stage name `"raster export"`. Cancellation is checked before each tile; on cancel the partial file is removed and `wasCancelled()` is true.
   - **Result:** `objectsWritten()` is the number of tiles; `bytesWritten()` is the final file size, read before the rename.
   - **Refusals:** an empty region or a scale below 1 is `InvalidArgumentError`, before any file is created.
3. **`makeLabelMaskSource`** (data library, `reporting/label_mask.hpp`): `TileRasterSourcePtr makeLabelMaskSource(Store& store, const ObjectQuery& query);`
   - The query must have a layer; otherwise `InvalidArgumentError`.
   - The source has one `Int32` plane.
   - **Pixel value:** where an object the query admits covers the pixel (conservative rasterisation, as storage's layer sources), the value is:
     - the query's `label()` filter's `LabelId`, when it has one (SDS 07 §7.6: "encodes membership of that label");
     - otherwise the first label of the object's effective set, or 0 when it has none.
   - Where objects overlap, the smallest `ObjectId` wins, as storage's label source does. Uncovered pixels are 0.
   - For each tile it queries the store with the user's query plus `intersecting` the tile's level-0 extent grown by one pixel. Reuse storage's or core's rasterisation helpers; do not copy them.
   - The store is borrowed and must outlive the source.
4. **The `masks` engine module** (`include/slideio/processor/masks/`, `src/masks/`, `tests/masks/`).
   - **`rasteriseLabels`:** `std::shared_ptr<slideio::Scene> rasteriseLabels(Store& store, const ObjectQuery& query, const Rect64& regionLevel0);`
     - It builds a scene with `slideio::makeScene` over an `ISceneSource` backed by `makeLabelMaskSource`.
     - The scene has one `Int32` channel. Its size is the region's size in level-0 pixels, and its resolution is 0 (unknown).
     - It is lazy: no pixel is computed until read. A block read at any output size rasterises at that size directly.
     - SDS 07's `level` parameter is dropped. A store has no level scales of its own, and the scene is readable at any resolution.
   - **`MaskWriter`:**
     - **Members:** `explicit MaskWriter(const char* path)`, `MaskWriter& compression(RasterCompression)` (default `Deflate`), `MaskWriter& resolution(double micronsPerPixel)` (default: the scene's own, when it has one), `MaskWriter& name(const char*)`, and `ExportResult write(std::shared_ptr<slideio::Scene> scene, ProgressObserver*, const CancellationToken&)`.
     - **How it writes:** it adapts the scene to a `TileRasterSource` that reads blocks with `readResampledBlock`, then writes it through `RasterTiffWriter` at scale 1 over the whole scene.
     - **Refusals:** every channel must share one data type that `RasterFormat` supports; otherwise `InvalidArgumentError`, before any file. A null scene is also `InvalidArgumentError`.
     - **Dropped SDS 07 options:** `tileSize`, which is fixed at 512 by storage's writer; `pyramidLevels`, which is automatic, halving until both sides are at most 512; and `MaskCompression::Jpeg`, because label masks must be lossless. `MaskCompression` is replaced by `RasterCompression`.
   - **Dependencies:** the module depends on `slideio::slideio` and the data library only. The `masks` headers may name `slideio::Scene`, as `pipeline.hpp` does.
5. **Docs.**
   - SDS 07 §7.6 is rewritten for decisions 2–4, including why neither the converter nor the data library is used.
   - The module-structure design gains the `masks` row and its edges (§4, §5.1).
   - SDS 03 §3.8 gains the opt-in OME layout.
   - SDS 08 gains the new classes.
   - `docs/project.md` gains a Reporting plan C entry. It records the user decisions, plus follow-ups: physical resolution for `rasteriseLabels`' scene, and a SlideIO OME-TIFF compatibility check on each SlideIO upgrade.
   - `docs/traceability.md` gains every new test.

## Review Focus

1. **SlideIO reopen:** a written label mask opens with SlideIO's OME-TIFF driver, with the expected level count and level sizes, and reading level 0 returns the exact label values (`Masks.WrittenLabelMaskReopensInSlideIO`, Task 4).
2. **Labels are never blended:** a level-1 read of a written mask contains only label values present at level 0 (`Masks.ReducedLevelsKeepLabelValues`, Task 4).
3. **Store files unchanged:** a store raster published after this change is byte-identical to one published before it (`StorageRaster.SequentialLayoutUnchanged`, Task 1).
4. **Cancel and failure** leave no file at `path`, and an existing file survives (`RasterTiffWriter.CancelledWriteLeavesNoFile`, `RasterTiffWriter.CancelledWriteKeepsAnExistingFile`, Task 2).
5. **Overlapping objects:** the smallest `ObjectId` wins; a query with a `label()` filter writes that label's id; one without writes the effective first label (`LabelMaskSource.ValuesFollowTheQuery`, Task 3).

---

### Task 1: OME-TIFF layout in storage's pyramid writer

**Files:**
- Modify: `src/storage/detail/pyramid_tiff_writer.{hpp,cpp}`, SDS 03 §3.8.
- Test: the existing pyramid writer test file, `tests/storage/test_pyramid_tiff_writer.cpp`, and a new engine-side test file, `tests/masks/test_ome_layout.cpp`.

**Interfaces:**
- Produces: `enum class PyramidLayout { Sequential, OmeSubIfds }`, a `PyramidLayout` parameter on `writePyramidTiff` (default `Sequential`), and a `double micronsPerPixel` field in `PyramidMetadata` (default 0).

- [ ] **Step 1: Write the failing tests**
  - `StorageRaster.SequentialLayoutUnchanged`: write a small `Int32` and a `Float32` spool with the default layout. Each file is byte-identical to a golden produced by the current code: capture the expected bytes in the test before the change, as a hash or a small embedded size-plus-hash.
  - `PyramidTiffWriter.OmeLayoutUsesSubIfds`:
    - an `OmeSubIfds` write of a 1500×900 `Int32` spool gives a file with 1 top-level IFD whose `TIFFTAG_SUBIFD` lists 2 reduced levels (750×450, 375×225), read back with libtiff;
    - page 0's ImageDescription contains `SizeX="1500"`, `SizeY="900"` and `Type="int32"`, and no path.
  - `OmeLayout.OpensInSlideIOWithAllLevels` (engine test target, `tests/masks`): `slideio::openSlide(path, "OMETIFF")` on that file gives one scene of 1500×900 with 3 zoom levels, one `Int32` channel, and level 0 values equal to the spool's.
- [ ] **Step 2: Run, see them fail.** Step 3: implement decision 1. If the OME-TIFF driver needs other attributes, add them, and record each one and the driver line that needs it in the report.
- [ ] **Step 4:** `./run-tests.sh` passes, including every existing storage raster test and both golden stores.
- [ ] **Step 5: Commit**

```bash
git add src tests docs
git commit -m "feat(storage): opt-in OME-TIFF pyramid layout that SlideIO reads with all levels"
```

### Task 2: `RasterTiffWriter`

**Files:**
- Create: `include/slideio/processor/reporting/raster_tiff_writer.hpp`, `src/reporting/raster_tiff_writer.cpp`, `tests/reporting/test_raster_tiff_writer.cpp`.
- Modify: `include/slideio/processor/reporting/reporting.hpp`, `src/reporting/CMakeLists.txt`, `tests/reporting/CMakeLists.txt`.

**Interfaces:**
- Consumes: `writePyramidTiff(…, PyramidLayout::OmeSubIfds)` and `TileSpool` (storage detail, data library); `ExportResult`.
- Produces: `RasterTiffWriter` (decision 2).

- [ ] **Step 1: Write the failing tests**, using a deterministic test `TileRasterSource` whose value is a function of the pixel's level-0 coordinates:
  - `RasterTiffWriter.WritesTheRegionAtTheScale`: region `{100, 200, 1500, 900}` at scale 1, then at scale 2. Read back with libtiff: page 0 is 1500×900, then 750×450, and sampled values equal the source's.
  - `RasterTiffWriter.CompressionsRoundTrip`: None, LZW and Deflate each give identical samples on read-back.
  - `RasterTiffWriter.ResolutionWritesPhysicalSize`: `resolution(0.25)` puts `PhysicalSizeX="0.25"` in the OME-XML; the default writes none.
  - `RasterTiffWriter.CancelledWriteLeavesNoFile` and `RasterTiffWriter.CancelledWriteKeepsAnExistingFile`.
  - `RasterTiffWriter.ReportsProgressPerTile`.
  - `RasterTiffWriter.RefusesBadArguments`: an empty region, scale 0.5, and a negative or NaN resolution each throw, and no file is created.
  - `RasterTiffWriter.WriteIsDeterministic`: two writes are byte-identical.
- [ ] **Step 2: Run, see them fail. Step 3: Implement. Step 4: `./run-tests.sh` and the no-seams check pass.**
- [ ] **Step 5: Commit**

```bash
git add include src tests docs/traceability.md
git commit -m "feat(reporting): RasterTiffWriter writes any tile raster source as a lossless pyramidal OME-TIFF"
```

### Task 3: Query-driven label mask source

**Files:**
- Create: `include/slideio/processor/reporting/label_mask.hpp`, `src/reporting/label_mask.cpp`, `tests/reporting/test_label_mask.cpp`.
- Modify: `include/slideio/processor/reporting/reporting.hpp`, `src/reporting/CMakeLists.txt`.

**Interfaces:**
- Produces: `TileRasterSourcePtr makeLabelMaskSource(Store&, const ObjectQuery&);` (decision 3).

- [ ] **Step 1: Write the failing tests**, on a store fixture with overlapping squares in layer `nuclei`, labels `tumour` (row), `stroma` (row) and an asserted `ki67`:
  - `LabelMaskSource.ValuesFollowTheQuery`:
    - without a label filter, pixels carry each object's first effective label, and the overlap shows the smaller `ObjectId`'s;
    - `label(ki67)` gives `ki67`'s id only where a `ki67` object covers the pixel, 0 elsewhere.
  - `LabelMaskSource.RequiresALayer`: a query without a layer throws.
  - `LabelMaskSource.QueryFiltersApply`: a query restricted to one stage omits the other stage's objects.
  - `LabelMaskSource.ScaledTilesMatchTheirLevel`: a tile at scale 2 equals the 2×2 top-left sampling of the scale-1 tile, at object edges, within storage's conservative-coverage rule. Assert exactly what that rule documents.
- [ ] **Step 2: Run, see them fail. Step 3: Implement. Step 4: `./run-tests.sh` passes.**
- [ ] **Step 5: Commit**

```bash
git add include src tests docs/traceability.md
git commit -m "feat(reporting): label mask raster source driven by an object query"
```

### Task 4: The `masks` engine module — `rasteriseLabels` and `MaskWriter`

**Files:**
- Create:
  - `include/slideio/processor/masks/{masks,label_scene,mask_writer}.hpp`;
  - `src/masks/{CMakeLists.txt,label_scene.cpp,mask_writer.cpp}`;
  - `tests/masks/{CMakeLists.txt,test_label_scene.cpp,test_mask_writer.cpp}`, joining Task 1's `test_ome_layout.cpp`.
- Modify: `src/CMakeLists.txt` (add `masks` to the engine), `tests/CMakeLists.txt`, SDS 07 §7.6, the module-structure design, SDS 08, `docs/project.md`, `docs/traceability.md`.

**Interfaces:**
- Consumes: `makeLabelMaskSource` (Task 3) and `RasterTiffWriter` (Task 2).
- Produces: `rasteriseLabels`, and `MaskWriter` (decision 4).

- [ ] **Step 1: Write the failing tests**
  - `Masks.LabelSceneIsLazyAndExact`:
    - `rasteriseLabels` over region `{0, 0, 1000, 800}` gives a 1000×800 scene with one `Int32` channel;
    - a full-resolution block read equals `makeLabelMaskSource`'s tile for the same rect;
    - creating the scene runs no store query (count queries through a probe if one exists; otherwise assert no pixel work before the first read, by timing-free means stated in the report).
  - `Masks.WrittenLabelMaskReopensInSlideIO`: `MaskWriter` writes that scene. `slideio::openSlide(path, "OMETIFF")` then gives one scene of 1000×800 with the expected zoom-level count and sizes, and level-0 values equal to the scene's.
  - `Masks.ReducedLevelsKeepLabelValues`: every value read from zoom level 1 is in the set of level-0 values.
  - `Masks.WritesAPipelineScene`: a pipeline scene with a `Float32` channel (from the existing pipeline test support) written by `MaskWriter` reopens with the same size and level-0 values.
  - `Masks.RefusesMixedChannelTypes`: a scene whose channels differ in type throws, and no file is created.
  - `Masks.ResolutionDefaultsToTheScenes`: a pipeline scene with a known resolution gives a `PhysicalSizeX` equal to it.
  - **Dependency check:** the `masks` OBJECT library links `slideio::slideio` and the data library only. The existing data-library configure check still passes, and `masks` is added to the engine's OpenCV guard, if it has one.
- [ ] **Step 2: Run, see them fail. Step 3: Implement decisions 4 and 5. Step 4: `./run-tests.sh`, the docs target and the no-seams check pass.**
- [ ] **Step 5: Commit**

```bash
git add include src tests docs
git commit -m "feat(masks): lazy label-mask scene and lossless MaskWriter for any SlideIO scene"
```
