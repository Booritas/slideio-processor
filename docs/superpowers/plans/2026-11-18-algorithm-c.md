# `algorithm` Module Implementation Plan C: segmentation and measurements

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the object-producing operations of SDS 06 §6.7–6.8 to the catalog, `segment.connected_components`, `segment.contours`, `segment.watershed` and `segment.tissue_detect`, with per-object measurements in pixel and calibrated units, identical across tile sizes (UR-SEG-3).

**Architecture:**
- **This is plan C of three.** Plans A (PR #12) and B (PR #13) built the module and its 17 pixel operations; the operation working set and its follow-up (PR #14) made every operation declare its memory. C depends on all three.
- **Three framework extensions come first (Tasks 1–3), all in `pipeline`.** They close gaps between SDS 06 and the framework as built, decided by the user on 2026-11-18:
  1. **A finishing step.** Every object, whether finished in one tile or merged from seam fragments, passes through its producer's `Operation::finishObject` before it reaches the sink. That one place adds the selected measurements and calibrated units and applies area filters and hole rules, so merged and unmerged objects are treated by one rule.
  2. **The pixel size.** `OperationContext::pixelSizeMicrons()` comes from the scene's resolution, so calibration is automatic and recorded configurations stay slide-independent.
  3. **Corner joins.** The merger joins fragments that touch diagonally across a seam, for layers whose producer declares 8-connected objects.
- **Segmenters measure.** `segment.connected_components` and `segment.watershed` are **Joint**: plane 0 is what is segmented, planes 1.. are the intensity channels measured. The measurement selection is their parameter. `measure.object_measurements` leaves the catalog, which closes at **twenty-one** operations; SDS 06 is amended in Task 9.
- **One tracer, one finisher.** All four operations label pixels inside the tile rect, trace each region's pixel-edge rings with one shared tracer, emit regions touching the tile rect's edge as seam fragments with partials, and emit the rest as finished objects. The framework normalises rings, so operations do not.

**Tech Stack:** C++17, CMake, Conan 2, GoogleTest, OpenCV 4.14 (static, private to the engine; `core` and `imgproc`, sequential per PR #14).

**Spec:** SDS 06 §6.7, §6.8, §6.9 and §6.10, as amended by plans A and B, read under SDS 00; SDS 05 §5.1, §5.5, §5.7 as built; requirements UR-SEG-1 to UR-SEG-8, UR-ALG-6, UR-QA-2, UR-EXE-2 in `docs/requirements.md`. The rulings below amend SDS 05 and 06; Task 9 writes them in.

## Global Constraints

Plan B's Global Constraints hold unchanged (C++17, `slideio::processor` and `detail`, export macros, C-2/C-4/C-5/C-6, no OpenCV in public headers, SDS-exact signatures as amended here, Doxygen with `@throws`/`@par Thread-safety`/`@par Requirements` and a 0-warning `docs` target, `// Requirements:` lines, no sample, parameter or coordinate values in messages, no plan vocabulary in code or public docs, `./build.sh --skip-slideio` with 0 warnings, `./run-tests.sh` green). Additionally:
- Branch `feat/algorithm-c` from `main` after PR #14 merges; one commit per task, prefixes `feat(pipeline):`, `feat(algorithm):`, `test(...)`, `docs(...)`. Org policy: AI-generated code is human-reviewed before it reaches `main`.
- Every new operation declares `workingSetBytesPerLevelPixel()` / `workingSetBytesPerTile()` and is checked by the working-set executable's allocation meter, as plan B's operations are (`tests/algorithm/support/working_set_check`).
- Signatures spell `std::shared_ptr<slideio::Scene>`, never `slideio::ScenePtr` (a SlideIO macro, SDS 05 §5.5).
- Plan A/B helpers are reused, not copied: `detail::BagReader`, `detail::BuiltinFactory`, `detail/builtin_operations.hpp`, `registry.cpp`, the test harness.
- New public headers `include/slideio/processor/algorithm/segment/segment.hpp` and `.../measure/measure.hpp` each get a header check in `tests/pipeline/CMakeLists.txt`, or configure fails.

## Rulings on spec gaps

Numbered G17 onward, continuing plan B's. Task 9 folds them into SDS 05 and 06.

| # | Gap | Ruling |
|---|---|---|
| G17 | Merged objects never see their producer: no filters, selected measurements or calibration (user decision 1). | `virtual bool Operation::finishObject(ObjectRecord& record, const ObjectFinishContext& context) const` — default returns true and changes nothing. Called once per object after ring normalisation and `applyStandardMeasurements`, before the restriction's anchor test is applied to merged objects and after it for tile objects (either order is equivalent: finishing never moves the anchor). Returning false discards the object; it is counted in the run's `droppedByFilter` counter. It may add or remove measurements and remove hole rings; it must not change the outer ring (the id is its anchor). `ObjectFinishContext` is a small public struct: `double pixelSizeMicrons` (0 when unknown) and `int32_t level`. Reentrant: called concurrently from merging and tile threads. |
| G18 | Which operation finishes a merged object. | **Each bound layer has at most one producer in a run.** `validate()` refuses a chain in which two operations producing objects resolve to the same layer ("operations X and Y both target layer \"L\""; names only). `detail::FinishingSink` wraps the run sink; it routes by `record.layer()` to the producer's `finishObject`. The tile filter and the merger both emit into it. |
| G19 | Ring normalisation is in `pipeline/detail` and must match point for point between paths. | The **framework** normalises every emitted record's rings (`detail::normaliseRings`, as merged objects already are) in the tile filter before finishing. Operations emit rings in any rotation, with the outer ring positive (clockwise on screen, y down) and holes negative — the tracer's natural orientation. |
| G20 | An object-producing operation not last in its chain sees a `tileRect()` grown by later halos, so its fragments miss the seams. | `validate()` refuses a chain in which an operation producing objects is followed by any operation with a non-zero halo ("operation X produces objects and must not be followed by a halo"). |
| G21 | Calibration (user decision 3). | `double OperationContext::pixelSizeMicrons() const noexcept` — the level-0 pixel size in µm, the mean of SlideIO's x and y resolution × 10⁶ when both are positive and agree within 10⁻⁶ relative, else 0. Filled by the executor and the lazy path from the run's scene. `sceneIsCalibrated(std::shared_ptr<slideio::Scene>)` and `scenePixelSizeMicrons(...)` in `measure.hpp` apply the same rule and are `noexcept` (a null scene gives false / 0). |
| G22 | 8-connected objects across seams (user decision 4). | `virtual bool Operation::objectsAreEightConnected() const` (default false), constant for the life of the operation. The executor tells the merger per layer. On such a layer two fragments in diagonally adjacent tiles **join when each has a ring vertex on the shared tile corner and the corner pixels on both sides are foreground of each** — expressed geometrically: fragment A in tile (c, r) and B in tile (c+1, r+1) (or the anti-diagonal) both have a vertex at the common corner point and their rings meet only there. At a vertex where one component's outline meets itself diagonally, the merger's linker takes the **sharpest left** turn on 8-connected layers (one outer ring touching itself at a point, which `Polygon::addRing` accepts) and the sharpest right turn otherwise. The tracer applies the same rule, so single-tile and merged objects agree. |
| G23 | `targetLayer("")` and the complete-bag rule (SDS 06 §6.2) clash with `run_plan.cpp`'s lookup. | An empty target layer **is left out of the bag**, the one completeness exception besides Otsu's range: the typed setter with `""` removes the key, and `BagReader` treats an absent `targetLayer` as empty. |
| G24 | Raster output is mandatory, but SDS 06 gives none for segmenters. | Every segmenter writes one `Int32` plane: the region's 1-based index within the tile for region pixels, 0 elsewhere — a debugging view, not an identity. `segment.tissue_detect` writes `Byte` 255/0. Documented as tile-local. |
| G25 | Arity and intensity (user decision 2). | `segment.connected_components`, `segment.watershed`: **Joint**, no uniform-type requirement. Plane 0 is segmented; planes 1.. are measured, each with `IntensityPartial::channel` = that plane's `sourceChannel`. `segment.contours`: PerChannel (a label raster, nothing measured). `segment.tissue_detect`: Joint, three uniform `Byte` or `UInt16` planes (RGB), nothing measured. NaN intensity samples are skipped (not counted); an infinite one is counted. |
| G26 | Foreground. | A pixel of plane 0 is foreground when it is non-zero and not NaN. For `segment.contours`, regions are maximal connected sets of **equal** non-zero, non-NaN value; for the others, of foreground. |
| G27 | Measurement selection and names. | `MeasurementSelection` and `MeasurementKind` as SDS 06 §6.8. Names (level-0 pixel units): `area` (SquarePixels), `perimeter` (Pixels) — both from `applyStandardMeasurements`; `centroid.x`, `centroid.y` (Pixels, `Polygon::centroid()` area-weighted, but **unrounded**: computed in the finisher by the shoelace formula in `double`); `boundingBox.x`, `.y`, `.width`, `.height` (Pixels); `intensity.<c>.mean|stddev|min|max` (Intensity). When `pixelSizeMicrons() > 0`, each geometric one also gets `<name>.um` (Micrometres) or `area.um2` (SquareMicrometres) = pixel value × s or × s². Unselected names are removed. Bag key `measurements`: comma-separated kind names (`"Area,Perimeter"`), default `standard()`. `makeObjectMeasurements` is removed; `ChannelSelection` is the run's (`PipelineSpec::channels`). |
| G28 | Area filters (UR-SEG-7) and holes on merged objects. | `minimumAreaPx` / `maximumAreaPx` (0 = unbounded) compare the final object's `area` in level-0 square pixels **in the finisher**, so a filter applies identically however the object was assembled. `extractHoles(false)` / `includeHoles(false)` remove hole rings in the finisher and recompute `area` and `perimeter` from the outer ring; intensity statistics stay over the object's own pixels. Areas are in level-0 pixels, so a filter means the same at every analysis level. |
| G29 | Connected components. | `connectivity` default `EightConnected` (UR-SEG-8's nuclei touch at corners); `minimumAreaPx` 0, `maximumAreaPx` 0, `extractHoles` true. Halo 1. Labelling: two-pass union–find on the tile rect at level coordinates. |
| G30 | Contours vs connected components. | `segment.contours` traces a **label raster** (G26): each region of equal value becomes an object with its rings; 4-connected regions (`objectsAreEightConnected()` false). `includeHoles` default true. Halo 1. Its input types: all integer types up to 32 bits and `Float32` (exact values). |
| G31 | Watershed. | Marker-controlled flooding over plane 0 restricted to foreground (G26), **from markers downhill**: a pixel joins the basin of its highest already-labelled 4-neighbour, processing pixels in descending value; ties broken by **level coordinates** (y, then x) so the order is position-independent. Markers: `LocalMaxima` — a foreground pixel strictly greater than every pixel within `minimumMarkerDistancePx` (Chebyshev), plateaus resolved to the topmost-leftmost pixel in level coordinates; `AuxiliaryRaster` — one marker region per distinct value ≠ -1 of `ctx.auxiliary(auxiliaryName)` (Int64; a missing binding throws `InvalidArgumentError` naming the operation and binding). Defaults: `LocalMaxima`, `minimumMarkerDistancePx` 5, `maximumMarkerSpacingPx` 32, `minimumAreaPx` 0. **Halo = 2 · maximumMarkerSpacingPx + 1**: a tile pixel's basin depends on markers within the spacing, and those markers' detection on a further `minimumMarkerDistancePx` ≤ spacing. **The reach check:** after flooding the halo'd context, any foreground pixel of the tile rect whose Chebyshev distance to its basin's marker exceeds `maximumMarkerSpacingPx`, or which is unreached, throws `InvalidArgumentError` with message `"segment.watershed: a region lies further than maximumMarkerSpacingPx from every marker"`. UR-SEG-8 asks the diagnostic to name the operation and the distance; it names the distance by its parameter, not its value, because messages carry no parameter values (Global Constraints). The executor's usual wrapping of an `apply()` failure applies; tests match the inner message. Constraint `minimumMarkerDistancePx ≤ maximumMarkerSpacingPx`. 8-connected objects false (basins split 4-connected). |
| G32 | Tissue detection. | Joint over RGB `Byte`/`UInt16`. A pixel is tissue when HSV saturation `(max − min) / max` > `saturationThreshold` (default 0.07; `max == 0` is background), then a morphological **closing** with a disc of radius `closingRadiusPx` (default 2, 0 = none) in level pixels, then regions traced 4-connected as objects in `targetLayer`; `minimumRegionAreaPx` (default 0) filters in the finisher on level-0 area. Halo `2 · closingRadiusPx + 1`. Geometry only; no measurements beyond area and perimeter. Correctness reference: analytic on synthetic tissue/background images (the labelled corpus of SDS 06 §6.9 is outside the repository; recorded as an open item). |
| G33 | Memory of the merger for tissue (one region may span the level). | Unchanged: the merger's fragments are reserved per SDS 05 §5.7. Documented in `segment.hpp`: tissue detection should run at a coarse `analysisLevel`. |

## Review Focus

1. **An object spanning a 2×2 block of tiles with a hole cut by both seams**: expect one object, one hole, identical to one tile at three tile sizes — Task 5 test `ObjectAcrossFourTilesWithSplitHole`.
2. **8-connected chain of pixels crossing a tile corner diagonally**: expect one object, equal to one tile — Task 3 test `DiagonalAcrossCornerJoins` and Task 5 `DiagonalChainAcrossCornerIsOneObject`.
3. **Object whose anchor the restriction excludes, and one filtered by area after merging**: expect neither in the sink and the counters to say why — Task 1 test `FinisherDiscardsMergedObject`, Task 5 `AreaFilterAppliesToMergedObjects`.
4. **NaN intensity samples inside an object**: expect statistics over the finite samples, equal across tile sizes — Task 5 `NaNIntensitySamplesAreSkipped`.
5. **Watershed clump wider than the declared spacing**: expect the reach error at every tile size, never a partial segmentation — Task 7 `SpacingExceededThrowsAtEveryTileSize`.

---

### Task 1: The finishing step and chain checks

**Files:**
- Modify: `include/slideio/processor/pipeline/operation.hpp` (add `ObjectFinishContext`, `finishObject`, `objectsAreEightConnected`), `src/pipeline/operation.cpp`
- Create: `src/pipeline/detail/finishing_sink.hpp/.cpp`
- Modify: `src/pipeline/detail/tile_evaluator.cpp` (normalise, then finish, in `TileObjectFilter`), `src/pipeline/executor.cpp` (wrap the run sink), `src/pipeline/detail/run_plan.cpp` (G18, G20)
- Test: `tests/pipeline/test_object_finishing.cpp`

**Interfaces:**
- Produces: `struct ObjectFinishContext { double pixelSizeMicrons; int32_t level; };` `virtual bool Operation::finishObject(ObjectRecord&, const ObjectFinishContext&) const;` `virtual bool Operation::objectsAreEightConnected() const;` `detail::FinishingSink(ObjectSink& target, std::vector<std::pair<LayerId, const Operation*>> producers, double pixelSizeMicrons, int32_t level)`.

- [ ] **Step 1: Write the failing tests** using `test.components` extended with a `finishMinArea` double parameter (a test-op finisher discarding area below it) and a `finishTag` bool (adds measurement `"finished"` = 1):
  - `EveryTileObjectIsFinished`, `EveryMergedObjectIsFinished`: every record in the `CollectingObjectSink` has `"finished"`, at tile sizes 64, 100, 257.
  - `FinisherDiscardsMergedObject`: a blob spanning two tiles with area 40 and `finishMinArea` 50 → not in the sink; a 60-area blob kept; result equal at three tile sizes.
  - `TileObjectRingsAreNormalised`: an operation emitting a ring rotated to start mid-edge is stored starting at its topmost-leftmost vertex.
  - `TwoProducersOnOneLayerRefused`, `ObjectProducerFollowedByHaloRefused` (validate throws `InvalidArgumentError`, message names the operations), `ObjectProducerFollowedByPointOpAccepted`.
- [ ] **Step 2: Run** `./run-tests.sh` filtered to `ObjectFinishing*`; expect compile failure.
- [ ] **Step 3: Implement** per G17–G20. Discards increment a new `droppedByFilter` counter alongside `droppedOutsideTile`; extend whatever reports those counters.
- [ ] **Step 4: Run** the new tests and the full pipeline suite; expect PASS.
- [ ] **Step 5: Commit** `feat(pipeline): objects pass through their producer's finishing step`

### Task 2: The pixel size in the operation context

**Files:**
- Modify: `include/slideio/processor/pipeline/operation.hpp`, `src/pipeline/operation_context.cpp`, the executor and lazy-path code that build contexts, `tests/algorithm/support/harness.{hpp,cpp}` (an optional `pixelSizeMicrons` argument)
- Test: `tests/pipeline/test_pixel_size.cpp`

**Interfaces:**
- Produces: `double OperationContext::pixelSizeMicrons() const noexcept;` and `detail::levelZeroPixelSizeMicrons(const slideio::Scene&) noexcept -> double` (G21's rule), used by Task 8's free functions.

- [ ] **Step 1: Failing tests** with a test scene of known resolution (extend the synthetic scene with a resolution setter if it has none): `PixelSizeFromResolution` (0.25e-6 m → 0.25), `PixelSizeZeroWhenUnknown` ((0,0) → 0), `PixelSizeZeroWhenAnisotropic` ((0.25e-6, 0.5e-6) → 0), `PixelSizeIsLevelZeroAtEveryLevel` (analysis level 2 → still 0.25), eager and lazy paths; `ObjectFinishContext::pixelSizeMicrons` equals it.
- [ ] **Step 2: Run**; expect FAIL.
- [ ] **Step 3: Implement** per G21.
- [ ] **Step 4: Run**; expect PASS.
- [ ] **Step 5: Commit** `feat(pipeline): operations see the scene's pixel size`

### Task 3: Corner joins in the seam merger

**Files:**
- Modify: `src/pipeline/detail/fragment_merge.{hpp,cpp}` (diagonal contacts; left-turn linking on 8-connected layers), `src/pipeline/seam_merger.cpp` (per-layer connectivity), `src/pipeline/executor.cpp` (tell the merger from `objectsAreEightConnected()`), `tests/pipeline/test_support/test_operations.{hpp,cpp}` (`test.components` gets `eightConnected`; its tracer takes left turns when set)
- Test: `tests/pipeline/test_fragment_merge.cpp`, `tests/pipeline/test_seg3_tiling_invariance.cpp`

**Interfaces:**
- Produces: `void detail::SeamMergerAccess::setEightConnected(SeamMerger&, LayerId)`; behaviour per G22.

- [ ] **Step 1: Failing tests:** `DiagonalAcrossCornerJoins` (two one-pixel fragments in diagonal tiles meeting at the corner → one object with one ring of 8 vertices touching itself once, on an 8-connected layer); `DiagonalAcrossCornerStaysSplitOn4Connected`; `PinchMergesToOneOuterRing` (a component pinching at a vertex no longer throws on 8-connected layers); in `test_seg3_tiling_invariance.cpp`, `EightConnectedBlobsInvariantAcrossTileSizes` over `cornerTouchingPairs` at tile sizes 64, 100, 257, equal to one tile.
- [ ] **Step 2: Run**; expect FAIL.
- [ ] **Step 3: Implement** per G22. A corner contact is recorded per fragment for each tile-corner vertex it has; two contacts join when they are at the same corner point, in diagonal tiles, and the layer is 8-connected. Closing waits for the diagonal tile too.
- [ ] **Step 4: Run** the merger, spill and SEG-3 suites; expect PASS (existing 4-connected results unchanged).
- [ ] **Step 5: Commit** `feat(pipeline): the merger joins 8-connected objects at tile corners`

### Task 4: Shared tracing, region emission and measurement finishing

**Files:**
- Create: `src/algorithm/detail/region_trace.{hpp,cpp}`, `src/algorithm/detail/region_emit.{hpp,cpp}`, `src/algorithm/detail/measurement_finish.{hpp,cpp}`, `include/slideio/processor/algorithm/measure/measure.hpp`, `src/algorithm/measure/measure.cpp`
- Modify: `src/algorithm/CMakeLists.txt`, `tests/pipeline/CMakeLists.txt` (header check), `tests/algorithm/support/harness.{hpp,cpp}`
- Test: `tests/algorithm/test_region_trace.cpp`, `tests/algorithm/test_measurement_finish.cpp`

**Interfaces:**
- Produces:
  - `std::vector<detail::TracedRegion> detail::traceRegions(const int32_t* labels, int64_t width, int64_t height, int64_t stride, bool eightConnected)` — `TracedRegion { int32_t label; std::vector<std::vector<Point64>> rings; std::vector<bool> isHole; bool touchesEdge; }`; rings in level pixel-corner coordinates relative to the label buffer, outer positive, holes negative, left turns at pinches when `eightConnected`.
  - `void detail::emitRegions(OperationContext&, const int32_t* labels, int64_t stride, const std::vector<detail::TracedRegion>&, const BlockView& input, int32_t firstIntensityPlane)` — offsets to the tile rect, maps through `ctx.toLevel0`, computes per-region partials over planes ≥ `firstIntensityPlane` (G25), emits finished records (`applyStandardMeasurements` then `objectSink()->emit`) or `emitSeamFragment` with partials.
  - `class detail::MeasurementPlan` built from `MeasurementSelection`, area bounds and a holes flag, with `bool finish(ObjectRecord&, const ObjectFinishContext&) const` per G27–G28, used by every segmenter's `finishObject`.
  - Public `measure.hpp` per SDS 06 §6.8 as amended: `MeasurementKind`, `MeasurementSelection` (pimpl'd, C-2 set), `sceneIsCalibrated`, `scenePixelSizeMicrons` — **no** `makeObjectMeasurements`.
  - Harness: `applyDirectObjects(Operation&, ..., double pixelSizeMicrons) -> DirectObjects { std::vector<ObjectRecord> objects; std::vector<Polygon> fragments; }` (a sink, a target layer and a fragment collector on the context), and `runEagerObjects(scene, spec, tileSize, workers) -> std::vector<ObjectRecord>` binding layer `"objects"`, sorted by id.
- [ ] **Step 1: Failing tests:** tracer — a 3×3 square (one ring, area 9, perimeter 12), a ring with a hole (two rings, hole flag), a diagonal pair 4- vs 8-connected (two objects vs one ring with a pinch), edge-touching flags; finisher — `area.um2` = area × s² at s = 0.5, no `.um` names at s = 0, unselected intensity names removed, `centroid.x/y` of a 2×4 rectangle at (10, 20) = 11, 22 exactly, `boundingBox.*` exact, minimum/maximum area discard, `extractHoles(false)` removes the hole and area becomes the outer ring's; `sceneIsCalibrated(nullptr)` false.
- [ ] **Step 2: Run**; expect FAIL.
- [ ] **Step 3: Implement.** Partial sums in `double` in row-major order within the tile (SDS 06 §6.8's 64-bit rule; count is `int64_t`).
- [ ] **Step 4: Run**; expect PASS; `docs` target 0 warnings.
- [ ] **Step 5: Commit** `feat(algorithm): region tracing, emission and measurement finishing`

### Task 5: `segment.connected_components`

**Files:**
- Create: `include/slideio/processor/algorithm/segment/segment.hpp` (`Connectivity`, `ConnectedComponentsParameters` with `measurements(const MeasurementSelection&)` added; the other three parameter classes are declared by their tasks), `src/algorithm/segment/connected_components.cpp`
- Modify: `src/algorithm/detail/builtin_operations.hpp`, `src/algorithm/registry.cpp` (`registerSegmentationOperations`), `tests/pipeline/CMakeLists.txt` (header check), `tests/algorithm/test_registry.cpp` (`pipelineValidates` binds a layer for operations that produce objects)
- Test: `tests/algorithm/test_connected_components.cpp`, working-set check entry

**Interfaces:**
- Consumes: Tasks 1–4.
- Produces: the catalog entry `segment.connected_components`; bag keys `connectivity` (`"FourConnected"`/`"EightConnected"`), `minimumAreaPx`, `maximumAreaPx`, `extractHoles`, `measurements`, `targetLayer` (G23).

- [ ] **Step 1: Failing tests** (`// Requirements: UR-SEG-2, UR-SEG-3, UR-SEG-4, UR-SEG-5, UR-SEG-7`):
  - `KnownObjectsCountAndShape`: 5 synthetic rectangles and one ring with a hole → 5 + 1 objects, exact areas, holes as separate rings (UR-SEG-4).
  - `MeasurementsAnalytic`: a 10×6 rectangle of intensity 7 on plane 1 → area 60, perimeter 32, centroid exact, mean 7, stddev 0, min = max = 7, `.um` names at pixel size 0.5.
  - `TilingInvariantAtThreeSizes` (UR-SEG-3): `makeBlobScene` with `MaskAndFloatValues`, tile sizes 64, 100, 257 and one tile: equal count, ids, geometry, `area`, `perimeter`, `centroid.*`; intensity bitwise for a `Byte` plane, within 10⁻¹² relative for `Float32`.
  - `ObjectAcrossFourTilesWithSplitHole`, `DiagonalChainAcrossCornerIsOneObject`, `AreaFilterAppliesToMergedObjects`, `NaNIntensitySamplesAreSkipped` (Review Focus 1, 2, 3, 4).
  - `FilteredRunStoresFewerObjects` (UR-SEG-7): many 1-pixel artefacts with `minimumAreaPx` 4 → none of them in the sink.
  - `RejectsBadParameters`: negative areas, `maximumAreaPx` < `minimumAreaPx` (when non-zero), unknown measurement name → `InvalidArgumentError`.
- [ ] **Step 2: Run**; expect FAIL.
- [ ] **Step 3: Implement** per G24–G29. Working set: `Int32` labels plus union–find parent per pixel of the tile rect, declared per level pixel.
- [ ] **Step 4: Run** the suite and the working-set executable; expect PASS.
- [ ] **Step 5: Commit** `feat(algorithm): connected components with measurements`

### Task 6: `segment.contours`

**Files:** `src/algorithm/segment/contours.cpp`; `segment.hpp` (`ContourParameters`); registry; `tests/algorithm/test_contours.cpp`; working-set entry.

**Interfaces:** the catalog entry `segment.contours`; bag keys `includeHoles`, `targetLayer`.

- [ ] **Step 1: Failing tests:** `LabelRegionsBecomeObjects` (a label raster with values 1, 2, 3 touching each other → three objects whose rings share edges), `HoleAsSeparateRing` (UR-SEG-4), `IncludeHolesFalseDropsHoles`, `TilingInvariantAtThreeSizes`, `Float32LabelsExact`, `UnsupportedTypesRejected` (Int64, Float64).
- [ ] **Step 2: Run**; expect FAIL.
- [ ] **Step 3: Implement** per G26, G30. Measurements: area and perimeter only.
- [ ] **Step 4: Run**; expect PASS.
- [ ] **Step 5: Commit** `feat(algorithm): contours of a label raster`

### Task 7: `segment.watershed`

**Files:** `src/algorithm/segment/watershed.cpp`; `segment.hpp` (`WatershedMarkers`, `WatershedParameters` with `measurements(...)`); registry; `tests/algorithm/test_watershed.cpp`; working-set entry.

**Interfaces:** the catalog entry `segment.watershed`; bag keys `markers` (`"LocalMaxima"`/`"AuxiliaryRaster"`), `auxiliaryName`, `minimumMarkerDistancePx`, `maximumMarkerSpacingPx`, `minimumAreaPx`, `measurements`, `targetLayer`.

- [ ] **Step 1: Failing tests** (`// Requirements: UR-SEG-8, UR-SEG-3, UR-ALG-6`):
  - `TouchingDiscsSeparated`: two discs of radius 10 with centres 16 apart, plane 0 the distance transform of their union (computed in the test) → 2 objects; each area within 5% of πr² minus the shared lens's half; centroids within 1 px of the analytic ones.
  - `TilingInvariantAtThreeSizes`: a grid of 30 touching disc pairs at tile sizes 64, 100, 257 and one tile → identical results.
  - `SpacingExceededThrowsAtEveryTileSize` (Review Focus 5): one disc of radius 60 with `maximumMarkerSpacingPx` 32 → the reach error at every tile size and on one tile; message contains `segment.watershed` and `maximumMarkerSpacingPx`.
  - `AuxiliaryMarkers`: an `Int64` anchor raster with two marker regions → two basins; `MissingAuxiliaryThrows`.
  - `PlateauMarkerIsTopmostLeftmost`, `HaloFollowsSpacing` (`halo()` = 2·s+1), `RejectsDistanceAboveSpacing`.
- [ ] **Step 2: Run**; expect FAIL.
- [ ] **Step 3: Implement** per G31. Priority queue keyed (value desc, y, x) in level coordinates. Working set: `Int32` labels, a queue entry per pixel, the marker-detection buffer.
- [ ] **Step 4: Run**; expect PASS.
- [ ] **Step 5: Commit** `feat(algorithm): marker-controlled watershed with an enforced reach`

### Task 8: `segment.tissue_detect`

**Files:** `src/algorithm/segment/tissue_detect.cpp`; `segment.hpp` (`TissueDetectParameters`); registry; `tests/algorithm/test_tissue_detect.cpp`; working-set entry.

**Interfaces:** the catalog entry `segment.tissue_detect`; bag keys `saturationThreshold`, `minimumRegionAreaPx`, `closingRadiusPx`, `targetLayer`.

- [ ] **Step 1: Failing tests** (`// Requirements: UR-SEG-1, UR-SEG-3`): `TissueAreaOnSyntheticSlide` (pink tissue ellipses on white with grey noise → tissue area within 1% of the analytic ellipse area at level 0); `ClosingBridgesGaps` (a 2-px gap closed at radius 2, not at 0); `SmallRegionsFiltered` at level-0 area; `TilingInvariantAtThreeSizes`; `LevelTwoAreasInLevelZeroPixels` (analysis level 2 → areas ×16 within the staircase's tolerance); `UInt16Accepted`, `NonRgbRejected`.
- [ ] **Step 2: Run**; expect FAIL.
- [ ] **Step 3: Implement** per G32. The closing is the module's own disc dilation-then-erosion on a `Byte` mask (exact).
- [ ] **Step 4: Run**; expect PASS.
- [ ] **Step 5: Commit** `feat(algorithm): tissue detection`

### Task 9: Documentation, catalog closure and traceability

**Files:** `docs/superpowers/specs/sds/05-pipeline.md` (G17–G22: §5.1 declarations, §5.5 validation, §5.7 corner joins and finishing), `06-algorithm.md` (§6.2 parameter rows for `segment.*`, §6.7 and §6.8 as amended, §6.9 references and tolerances for the four operations, §6.10 twenty-one operations and the dropped entry with its reason), `08-class-diagrams.md`, `docs/traceability.md`, `docs/project.md` (plan C entry), `docs/requirements.md` (record G32's corpus as an open item only if the requirements' open-questions table is the place; otherwise SDS 06 §6.9).

- [ ] **Step 1:** Write the amendments; every G-ruling appears once, in SDS words, no plan vocabulary.
- [ ] **Step 2:** `cmake --build build --target docs --config Release` → 0 warnings; `./build.sh --skip-slideio` and `./run-tests.sh` green; `tests/algorithm/test_registry.cpp` lists 21 operations.
- [ ] **Step 3: Commit** `docs: segmentation, measurements and the finished catalog`
