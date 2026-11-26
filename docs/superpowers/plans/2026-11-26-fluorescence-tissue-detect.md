# Fluorescence Tissue Detection Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give `segment.tissue_detect` a `"Fluorescence"` mode that marks tissue where the per-pixel channel maximum is above a fixed or whole-slide triangle cut-off, leaving brightfield untouched.

**Architecture:** One operation class with a mode switch in `src/algorithm/segment/tissue_detect.cpp`. The per-pixel mask step is the only thing that differs between modes: saturation or channel maximum. The closing, labelling and emission code is shared as it stands. The triangle cut-off uses the global-context route `pixel.threshold_otsu` uses (`accumulate` / `combine` / `finalizeAccumulation`). The triangle itself is a small free function in a detail header, so it can be tested against OpenCV directly.

**Tech Stack:** C++17, GoogleTest, OpenCV (tests only, as the reference triangle), CMake. Python bindings example (separate repo) in Task 5.

**Spec:** `docs/superpowers/specs/2026-11-26-fluorescence-tissue-detect-design.md`

## Global Constraints

- Brightfield is the default mode. A brightfield operation's parameter bag records exactly what it records today: `saturationThreshold`, `minimumRegionAreaPx`, `closingRadiusPx`, and `targetLayer` only when set. Nothing new.
- New bag keys:
  - `mode`: `"Brightfield"` or `"Fluorescence"`.
  - `thresholdMethod`: `"Triangle"` (default) or `"Fixed"`.
  - `intensityThreshold`: double, finite; required with `"Fixed"` and refused otherwise.
- The bag records `mode` and `thresholdMethod` only in fluorescence mode, and `intensityThreshold` only with `"Fixed"`.
- `saturationThreshold` given in fluorescence mode is refused. `thresholdMethod` or `intensityThreshold` given in brightfield mode is refused. Both apply to the typed API and to the bag.
- Fluorescence input: Joint, one or more planes of one type.
  - Byte or UInt16 with either method.
  - Float32 with `"Fixed"` only. Triangle on Float32 throws `UnsupportedDataTypeError`, and its message contains `"Fixed"`.
- Tissue: the per-pixel maximum over the input planes is strictly greater than the cut-off.
- Triangle cut-off:
  - Counted from the tile rect (not the halo), only at pixels the run's mask admits.
  - 256 bins for Byte and 65536 for UInt16, with exact `int64_t` counts.
  - On Byte it must equal `cv::threshold(..., THRESH_BINARY | THRESH_TRIANGLE)` on the same pixels, OpenCV's bound adjustments and flip included.
  - An empty histogram finds no tissue.
- The results must not depend on tile size (64, 100, 257 vs one tile) or worker count (1 vs 4).
- A brightfield scene without three channels keeps its message and gains the hint `"; for fluorescence, set mode to Fluorescence"`.
- Errors name the operation (`segment.tissue_detect`) and the key, as the existing ones do.
- Never run two test suites at once on Windows (one `./run-tests.sh` or ctest at a time).
- Commits go on branch `feat/fluorescence-tissue-detect`. Do not push.

## Review Focus

1. **Existing pipelines turning stale.** A brightfield bag must be byte-identical to today's. Pinned in Task 1 (`BrightfieldBagUnchanged`).
2. **Plenty of exact-zero background outside the scan area** (the example slide). The triangle must still land between the glass noise and the tissue, not at 0. Pinned in Task 3 (`TriangleIgnoresZeroPlateau`): the scene carries a band of exact zeros.
3. **A restriction that admits nothing, or all-background slides.** No crash and no objects. Pinned in Task 3 (`EmptyHistogramFindsNothing`).
4. **A channel selection of one channel** (DAPI only). The single-plane path works. Pinned in Task 2 (`SingleChannelSelection`).
5. **Typed API and named bag disagreeing** in fluorescence mode. Pinned in Task 1 (`FluorescenceTypedAndNamedBagsAreEqual`).

---

### Task 1: Parameters, modes and validation

**Files:**
- Modify: `include/slideio/processor/algorithm/segment/segment.hpp`. This covers the TissueDetectParameters doc table, the new enums and setters, and makeTissueDetect's `@throws`.
- Modify: `src/algorithm/segment/tissue_detect.cpp`. This covers `TissueDetectValues`, the setters, `makeTissueDetect` validation, `tissueDetectFromParameters` and the parameters_ recording.
- Test: `tests/algorithm/test_tissue_detect.cpp`

**Interfaces:**
- Produces (public, in `segment.hpp`):
  - `enum class TissueMode : int32_t { Brightfield, Fluorescence };`
  - `enum class TissueThresholdMethod : int32_t { Triangle, Fixed };`
  - `TissueDetectParameters& mode(TissueMode)`
  - `TissueDetectParameters& thresholdMethod(TissueThresholdMethod)`
  - `TissueDetectParameters& intensityThreshold(double)`
- Produces (detail): `TissueDetectValues` gains:
  - `TissueMode mode = Brightfield`
  - `std::optional<double> saturationThreshold` (unset → 0.07 in brightfield; set → refused in fluorescence)
  - `std::optional<TissueThresholdMethod> thresholdMethod`
  - `std::optional<double> intensityThreshold`

  Tasks 2 and 3 read these.

- [ ] **Step 1: Write the failing tests** in `test_tissue_detect.cpp`:
  - `BrightfieldBagUnchanged`:
    - `makeTissueDetect(TissueDetectParameters())` and `.mode(TissueMode::Brightfield)` both give `jsonOf(...) == R"({"saturationThreshold":0.07,"minimumRegionAreaPx":0,"closingRadiusPx":2})"`.
    - Before asserting, check the exact current JSON spelling by printing today's output once, and paste that literal.
  - `FluorescenceTypedAndNamedBagsAreEqual`:
    - Triangle: typed `.mode(Fluorescence).closingRadiusPx(3)` equals the catalog's `create` from the bag `{mode:"Fluorescence", closingRadiusPx:3}`.
    - The recorded keys are exactly `mode`, `thresholdMethod` ("Triangle"), `minimumRegionAreaPx` and `closingRadiusPx`: count 4, no `saturationThreshold`.
    - Fixed: `.thresholdMethod(Fixed).intensityThreshold(5.5)` records `intensityThreshold` 5.5; count 5.
  - `RejectsBadFluorescenceParameters`, using the existing `expectInvalid` pattern for typed and by-name, each naming the key:
    - fluorescence + `saturationThreshold(0.1)` → `"saturationThreshold"`;
    - brightfield + `thresholdMethod(Fixed)` → `"thresholdMethod"`;
    - brightfield + `intensityThreshold(3)` → `"intensityThreshold"`;
    - fluorescence + Fixed without intensity → `"intensityThreshold"`;
    - fluorescence + Triangle + `intensityThreshold(3)` → `"intensityThreshold"`;
    - Fixed + NaN, and by name `+inf` is already refused by OperationParameters. Typed NaN → `"intensityThreshold"`;
    - by name `mode:"Dark"` → `"mode"`;
    - by name `thresholdMethod:"Otsu"` → `"thresholdMethod"`.

- [ ] **Step 2: Run and see them fail (compile error on the missing enums)**

  Run: `cmake --build build --config Release --target slideio_processor_algorithm_tests` then `build/bin/Release/slideio_processor_algorithm_tests --gtest_filter=TissueDetect.*`
  Expected: build fails on `TissueMode`.

- [ ] **Step 3: Implement.**
  - Parse the enums with the existing `detail::BagReader` string helpers, spelled exactly as the enumerators.
  - Validate in `makeTissueDetect`, so both paths share the messages.
  - Record keys in the constructor in this order: brightfield as today; fluorescence `mode`, `thresholdMethod`, [`intensityThreshold`], `minimumRegionAreaPx`, `closingRadiusPx`, [`targetLayer`].
  - Doc-comment every new public member in the header's existing style: `@throws`, Thread-safety, Requirements UR-SEG-1.
  - Extend the parameter table.
  - Apply does not change in this task. Fluorescence `apply` may throw `InternalError("not yet")` until Task 2. Do not ship that past Task 2.

- [ ] **Step 4: Run the TissueDetect tests**: all pass, including the existing `TypedAndNamedBagsAreEqual`.

- [ ] **Step 5: Commit** `algorithm: tissue_detect mode, threshold method and intensity parameters`

---

### Task 2: Fluorescence mode with a fixed cut-off

**Files:**
- Modify: `src/algorithm/segment/tissue_detect.cpp`. This covers `outputPlaneCount`, `checkPlaneCount`, `supportedTypes`, `apply`, and a new mask function `maximumMask<T>(in, mask, cutoff)`.
- Test: `tests/algorithm/test_tissue_detect.cpp`

**Interfaces:**
- Consumes: the Task 1 values.
- Produces: a private member `double cutoff_` that the mask uses. Fixed sets it in the constructor. Task 3 sets it in `finalizeAccumulation`.
- Produces: a private `bool cutoffKnown_`. It is true for Fixed and set by Task 3's finalize. When it is false, apply finds no tissue.

- [ ] **Step 1: Write the failing tests**, on a new helper `fluorescenceScene(slideio::DataType type, double gain)`:
  - The level is 600 × 400, with 3 channels.
  - Background: each channel is `hashOf(x, y + 1000 * c) % 5` (0..4).
  - Columns `x < 40` are exactly 0 in every channel: outside the scan.
  - Ellipse A, centre (180, 200), radii 120 × 90, has channel 0 = `30 + hashOf(x, y) % 40` and the other channels at background.
  - Ellipse B, centre (440, 200), radii 100 × 120, is bright only in channel 2, with the same values.
  - Every value is multiplied by `gain`: 1 for Byte and Float32, 256 for UInt16.
  - Analytic area: `π(120·90 + 100·120)`.

  Tests:
  - `FluorescenceFixedFindsBothBlobs`:
    - Byte, Fixed 4.5, radius 2, `ts::runEagerObjects(scene, spec, 1000, 4)`.
    - 2 objects, and `areaOf` within 1% of the analytic area.
    - This shows the maximum is taken across channels, since B is bright only in channel 2.
  - `FluorescenceFloat32Fixed`: the same on `DT_Float32` gives the same objects as Byte.
  - `FluorescenceUInt16Fixed`: gain 256, Fixed `4.5 * 256`, the same area.
  - `FluorescenceTilingInvariant`: Fixed 4.5, radii {0, 2, 5}. `expectSameObjects` of the reference (one tile) vs tiles {64, 100, 257}, and vs workers 1 at tile 100.
  - `SingleChannelSelection`:
    - The spec's channels are `{2}`.
    - Only ellipse B is found: 1 object, area within 1% of π·100·120.
  - `FluorescenceZeroPlanesRefused`: `outputPlaneCount(0)` throws `ChannelArityError`.
  - Update `NonRgbRejected`: the brightfield message now ends with `"; for fluorescence, set mode to Fluorescence"`. Assert that the substring `"set mode to Fluorescence"` is present.

- [ ] **Step 2: Run them**: they fail, with "not yet" or a missing message substring.

- [ ] **Step 3: Implement.**
  - In fluorescence mode:
    - `checkPlaneCount` requires ≥ 1.
    - `supportedTypes` is Byte, UInt16 and Float32. Brightfield stays Byte and UInt16. `supportedTypes()` is per instance, so it follows `mode`.
    - `requiresUniformTypes` stays true.
  - `maximumMask<T>` writes `1` when the maximum over all planes `> cutoff_`.
  - The type dispatch in `close()` gains Float32 for fluorescence.
  - Leave `kBytesPerPixel` (200) unchanged: the mask is built into the same byte buffer. The working-set meter test in Task 3 checks this.

- [ ] **Step 4: Run** `--gtest_filter=TissueDetect.*`: all pass.

- [ ] **Step 5: Commit** `algorithm: tissue_detect fluorescence mode with a fixed cut-off`

---

### Task 3: Triangle cut-off over the whole slide

**Files:**
- Create: `src/algorithm/segment/detail/triangle_threshold.hpp`. A header-only `inline` function, so no CMake source list change. If the project puts detail functions in `.cpp`, add `triangle_threshold.cpp` to `src/algorithm/CMakeLists.txt` instead.
- Modify: `src/algorithm/segment/tissue_detect.cpp`. This covers `needsGlobalContext`, `accumulate`, `combine`, `finalizeAccumulation`, `accumulatedStateBytes` and `workingSetBytesPerTile`.
- Test: `tests/algorithm/test_tissue_detect.cpp` and `tests/algorithm/test_working_set_segment.cpp`

**Interfaces:**
- Produces: `namespace slideio::processor::detail { int64_t triangleThresholdBin(const std::vector<int64_t>& bins); }`.
  - It returns the cut-off bin t; tissue is `bin > t`.
  - It returns -1 for an all-zero histogram, which means no cut-off.
  - The algorithm is OpenCV's `getThreshVal_Triangle_8u` (imgproc/src/thresh.cpp), generalised to `bins.size()` bins over `int64_t` counts and `double` distances. It keeps OpenCV's steps:
    - left and right bounds widened by one;
    - flip when the peak is nearer the left bound;
    - the line from bound to peak;
    - the maximum distance, keeping the first;
    - OpenCV's final `thresh` adjustment;
    - unflip.
  - Read OpenCV's source in the build's OpenCV checkout. Find it with `git -C <repo> ls-files | grep thresh.cpp` or search `build/_deps` and `extern`, and port it line for line.
- Consumes: `cutoff_` and `cutoffKnown_` from Task 2.

- [ ] **Step 1: Write the failing tests.**
  - `TriangleMatchesOpenCv`, for each of these 8-bit images:
    - random uniform;
    - a dark peak with a long bright tail;
    - a bright peak with a dark tail (the flip);
    - two values only;
    - one value;
    - the background-plus-zeros histogram of `fluorescenceScene`'s channel maximum.

    For each: build the 256-bin histogram, then `EXPECT_EQ(triangleThresholdBin(bins), static_cast<int64_t>(cv::threshold(img, dst, 0, 255, THRESH_BINARY | THRESH_TRIANGLE)))`. For one value, check what OpenCV returns and expect that.
  - `TriangleAllZeroIsMinusOne`: a 256-bin histogram of zeros gives -1.
  - `TriangleIgnoresZeroPlateau`:
    - `fluorescenceScene` Byte, Triangle, radius 2.
    - 2 objects with area within 1% of analytic.
    - Plus a sanity check that `triangleThresholdBin` on the scene's maximum histogram is in [4, 29].
  - `TriangleUInt16`: gain 256, Triangle, the same 2 objects and area.
  - `TriangleTilingAndWorkersInvariant`: Triangle, tiles {64, 100, 257} vs one tile, and workers 1 vs 4 at tile 100. The objects are identical.
  - `TriangleRestrictedCountsOnlyAdmittedPixels`:
    - Restrict to a polygon covering ellipse A plus background (x 40..330).
    - Assert that the run equals one with `Fixed` set to the triangle bin of the admitted pixels' histogram, computed in the test.
    - Assert that this bin differs from the whole-slide one. If it does not, enlarge the background share inside the polygon until it does: the test must discriminate.
  - `EmptyHistogramFindsNothing`: restrict to a polygon inside the x < 40 zero band. Triangle finds 0 objects and doesn't throw.
  - `TriangleFloat32Refused`: Float32 + Triangle throws `UnsupportedDataTypeError` (it may surface wrapped in the run's TileError; assert on the cause's message) whose message contains `"Fixed"`.
  - In `test_working_set_segment.cpp`, `TissueDetectFluorescenceBounded`: the existing `expectApplyBounded` runs on a fluorescence Fixed operation with 1, 3 and 5 Byte planes and 3 UInt16 planes. Add a check that `workingSetBytesPerTile()` of a Triangle operation is ≥ `kBytesPerTile + 2 * 65536 * 8`.

- [ ] **Step 2: Run them**: compile failure on the missing `triangle_threshold.hpp`.

- [ ] **Step 3: Implement.**
  - `needsGlobalContext()` is true only for fluorescence + Triangle.
  - `accumulate`:
    - Checks that the type is Byte or UInt16, else throws `UnsupportedDataTypeError` with `"Triangle needs Byte or UInt16 input; for Float32 set thresholdMethod to Fixed"`.
    - Lazily sizes a `std::vector<int64_t>` of 256 or 65536 bins.
    - Counts the channel maximum over `tileRectAtLevel()` within the halo'd input, at pixels where `context.mask()` is null or admits the level pixel. Use the same mask test `threshold_otsu.cpp`'s `accumulatePlane` uses; read it.
  - `combine`:
    - `dynamic_cast`s, else fails with "cannot combine with an operation of another kind".
    - Adds the bins; differing sizes → fail.
  - `finalizeAccumulation`:
    - `t = triangleThresholdBin(bins)`.
    - When `t < 0`, `cutoffKnown_` stays false.
    - Otherwise `cutoff_ = t` and `cutoffKnown_ = true`.
  - `accumulatedStateBytes(planeCount)`: `65536 * 8` plus `sizeof(*this)` and an allocator-block overhead, following Otsu's formula (`detail::kAllocatorBlockOverhead`).
  - `workingSetBytesPerTile()` for Triangle: `kBytesPerTile + 2 * 65536 * 8`, as Otsu counts a local copy. Brightfield and Fixed are unchanged.
  - `apply` with `!cutoffKnown_` writes an all-zero output plane and emits nothing.

- [ ] **Step 4: Run** `--gtest_filter=TissueDetect.*:WorkingSetSegment.*`, then the whole algorithm test executable. All pass.

- [ ] **Step 5: Commit** `algorithm: tissue_detect triangle cut-off from a whole-slide histogram`

---

### Task 4: Documents, traceability and the full suite

**Files:**
- Modify: `include/slideio/processor/algorithm/segment/segment.hpp`. The file comment (line ~8, "tissue detection on RGB") becomes "tissue detection on brightfield RGB and on fluorescence". The `makeTissueDetect` description (~line 980) gains a fluorescence paragraph.
- Modify: `docs/superpowers/specs/sds/06-algorithm.md`:
  - the parameter table (~171);
  - the working-set table (~237): the Triangle per-tile bytes;
  - the typed API block (~925);
  - the arity paragraph (~1060);
  - the `segment.tissue_detect` section (~1093): a "Fluorescence mode" subsection with the rule, the triangle (Zack, Rogers & Latt 1977; matches OpenCV's), the empty-histogram rule, the bag-recording rule and the example-slide evidence from the spec.
- Modify: `docs/superpowers/specs/sds/08-class-diagrams.md` (~1982): the new setters and enums.
- Modify: `docs/requirements.md`:
  - The version becomes 0.15 (draft).
  - Add a "Changes in 0.15" note above 0.14: "`segment.tissue_detect` gains a fluorescence mode (channel maximum over a fixed or whole-slide triangle cut-off); UR-SEG-1's wording is unchanged".
- Modify: `docs/traceability.md`: add the new test names to the UR-SEG-1, UR-SEG-3 (tiling invariance tests) and UR-SEG-7 rows, following the format already in the rows.
- Modify: `src/algorithm/segment/tissue_detect.cpp`: the factory's validation text. Append "; fluorescence: two blobs bright in different channels over 0..4 glass noise and an exact-zero band give the blobs' area within 1% with a fixed and a triangle cut-off, the triangle equal to OpenCV's on Byte, identical objects at tile sizes 64, 100 and 257 and 1 or 4 workers". Append to the complexity text: "; fluorescence triangle: one O(n) counting pass over the tile rects and O(b) in the histogram's bins".

- [ ] **Step 1: Make the edits.** Grep `segment.tissue_detect` and `TissueDetectParameters` across `docs/` and `include/` for any spot missed. Leave the old plans in `docs/superpowers/plans/` unchanged.
- [ ] **Step 2: Build everything and run the full suite once**: `./run-tests.sh` (nothing else running). Expected: everything passes, with 0 failures; report the count.
- [ ] **Step 3: Commit** `docs: tissue_detect fluorescence mode in SDS 06 and 08, requirements 0.15, traceability`

---

### Task 5: Python example and the real slide (bindings repo, own branch)

The library branch must be built and installed first:
1. `cmake --install build --config Release --prefix build/slideio-install/release`. Check the existing install step in the bindings memory; the install dir is `D:/Projects/slideio/slideio-processor/build/slideio-install/release`.
2. Build the bindings: `SLIDEIO_INSTALL_DIR=...` `C:/Users/Stanislav/anaconda3/envs/slideio/python.exe build_deps.py`, then `python -m pip wheel . --no-deps -w dist`, then `pip install --no-deps --force-reinstall --no-cache-dir <wheel>`.

**Files (in `D:/Projects/slideio/slideio-processor-python`, branch `feat/fluorescence-tissue-example` from main):**
- Modify: `examples/tissue_detection.py`
- Modify: `README.md` (the `tissue_detection.py` bullet under "## Examples")

- [ ] **Step 1: Update the example.**
  - Add `--mode {brightfield,fluorescence}`. The default is fluorescence when `scene.num_channels != 3`, else brightfield.
  - Add `--intensity-threshold FLOAT`. When given it means `thresholdMethod="Fixed"`; it is refused with `--mode brightfield`.
  - Remove the "needs an RGB scene" exit.
  - `tissue_stage` passes `mode="Fluorescence"` (and the fixed keys) only in fluorescence mode, so brightfield stages are unchanged.
  - The overlay thumbnail for fluorescence is the channel maximum, stretched to its 99.5th percentile, as grey.
  - Update the module docstring.
- [ ] **Step 2: Run it on the example slide.**

  `python examples/tissue_detection.py "d:\Projects\slideio\images\images\pke\openmicroscopy\PKI_scans\LuCa-7color_Scan1.qptiff" --workspace <scratchpad>/ws --out <scratchpad>/out --overlay`

  Expected:
  - fluorescence chosen automatically;
  - the tissue area is 40–45% of the scene at the chosen level;
  - the overlay outlines the tissue with the alveolar spaces open.

  Look at the overlay PNG. Also rerun the brightfield path on `JP2K-33003-1.svs` (as in the earlier example runs) and confirm its region count matches a run from bindings main.
- [ ] **Step 3: Run the bindings tests** from the scratchpad `pytest-run` directory, outside the checkout. Expected: 230 passed, 1 skipped, or more if tests were added.
- [ ] **Step 4: Commit** `examples: tissue detection on fluorescence slides`. Do not push. Pushing and PRs wait for the user, and the bindings PR follows the library merge.
