# `algorithm` Module Implementation Plan B: filters and morphology

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the eleven neighbourhood operations of SDS 06 §6.4 to the catalog: `pixel.gaussian_blur`, `pixel.median_blur`, `pixel.sobel`, `pixel.scharr`, `pixel.laplacian`, `pixel.canny`, `pixel.bilateral_filter`, and `pixel.morph_erode`, `pixel.morph_dilate`, `pixel.morph_open`, `pixel.morph_close` with `StructuringElement`.

**Architecture:**
- **This is plan B of three.** Plan A (merged as PR #12) built the module, its registry, `BuiltinFactory`, `BagReader`, the sample codec, the test harness and six point operations. C adds segmentation and measurements. B depends only on A.
- **Every filter must be exact under tiling (UR-QA-2), so each output pixel is a function of its own neighbourhood alone.** Two routes meet that:
  - **The module's own kernels, used wherever arithmetic is floating-point.** A pixel's sum is taken in a fixed order that does not depend on where the pixel sits in its tile, as `pixel.threshold_adaptive` already does. OpenCV's floating-point filters are not used: their SIMD main loop and scalar tail can round differently, which would make a pixel's last bit depend on its column within the tile. Its float bilateral filter also builds its lookup table from the tile's own value range.
  - **OpenCV, used where the result is exact by nature.** These are order statistics (median on `Byte`, erosion and dilation) and Canny on `Byte`, whose gradients are integers.

  A tiled-versus-one-tile bitwise test in every task enforces the rule.
- **The halo carries the context.** The framework supplies each tile grown by the declared halo, and pixels beyond the level read as zero both under tiling and in one tile. No operation special-cases edges.

**Tech Stack:** C++17, CMake, Conan 2, GoogleTest, OpenCV 4.14 (static, private to the engine; `core` and `imgproc`).

**Spec:** SDS 06 §6.2, §6.4, §6.9 and §6.10, as amended by plan A, read under SDS 00. The operation contract is SDS 05 §5.1 as built. The requirements are UR-PIX-3, UR-PIX-4, UR-EXT-3, UR-QA-2, UR-QA-3, UR-IMG-11, UR-ALG-3 to UR-ALG-8 and open question 15 (Canny) in `docs/requirements.md`.

## Global Constraints

Plan A's Global Constraints hold unchanged: C++17, `slideio::processor` and `detail`, `SLIDEIO_PROCESSOR_ENGINE_EXPORTS`, C-2/C-4/C-5/C-6, no OpenCV in public headers, SDS-exact signatures, Doxygen with `@throws`/`@par Thread-safety`/`@par Requirements` and a 0-warning `docs` target, `// Requirements:` lines, no sample or parameter values in messages, no plan vocabulary in code or public docs, `./build.sh --skip-slideio` with 0 warnings. Additionally:
- Branch `feat/algorithm-b`, one commit per task, prefixes `feat(algorithm):`, `test(algorithm):`, `docs(algorithm):`. Org policy: AI-generated code is human-reviewed before it reaches `main`.
- Plan A's helpers are reused, not copied: `detail::BagReader` (strict keys; whole numbers accepted for double keys), `detail::BuiltinFactory`, `detail/builtin_operations.hpp`, the sample codec, `registry.cpp`, and the test harness (`applyDirect` pads by `op.halo()`; `runEager` is exact at level 0).
- The public header is `include/slideio/processor/algorithm/pixel/filter.hpp`, exactly SDS 06 §6.4, including `BorderMode`. The header check must list it, or configure fails.

## Rulings on spec gaps

Numbered G9 onward, continuing plan A's. Task 7 folds them into SDS 06.

| # | Gap | Ruling |
|---|---|---|
| G9 | Which computations go to OpenCV. | **Own fixed-order kernels:** Gaussian, Sobel, Scharr, Laplacian and bilateral, for every type. **OpenCV:** median on `Byte` (`cv::medianBlur`); erode and dilate (`cv::erode`, `cv::dilate`) on the types OpenCV morphs; Canny on `Byte` (`cv::Canny`). Median on the other types is the module's own, an exact order statistic, so it agrees with OpenCV wherever both apply. OpenCV is called on a `cv::Mat` header over the plane's own memory, through `detail::planeAsMat` in `src/algorithm/detail/cv_plane.hpp/.cpp`. No copy is made unless OpenCV needs a continuous buffer. OpenCV's own thread count is left as configured; results do not depend on it. |
| G10 | Output types: UR-IMG-11 forbids silent loss, but SDS 06 does not say what a smoothed or differentiated integer image becomes. | **Gaussian, bilateral, Sobel, Scharr and Laplacian** write `Float32` for every input type except `Float64`, which writes `Float64`. They compute in `double`, so no rounding back into the input type ever happens. **Median, erode, dilate, open and close** write the input type: they select existing values and create no new ones. **Canny** writes `Byte`, 255 on an edge and 0 elsewhere. |
| G11 | Supported types. | **Own kernels:** all types except `Int64` and `UInt64`, whose samples do not all fit a `double`. **Median:** all eleven; `Byte` goes to OpenCV, the rest to the module's own exact selection. **Morphology:** `{Byte, UInt16, Int16, Float32, Float64}`, the types `cv::erode` and `cv::dilate` accept. **Canny:** `{Byte}`. |
| G12 | Kernel sizes, halos and coefficients. | <ul><li>**Gaussian:** `sigma` finite and > 0. `kernelSize` is odd from 1 to 1025, or 0, which derives `2·ceil(3·sigma) + 1`. Weights are exp(−d²/2σ²), normalised to sum 1. Halo `kernelSize/2`.</li><li>**Sobel:** `kernelSize` ∈ {1, 3, 5, 7}; `dx`, `dy` ∈ {0, 1, 2} with `dx + dy ≥ 1`. Coefficients come from `cv::getDerivKernels(dx, dy, kernelSize, false, CV_64F)`, unnormalised as OpenCV's `Sobel`. Halo `max(1, kernelSize/2)`.</li><li>**Scharr:** (`dx`, `dy`) is (1, 0) or (0, 1); coefficients come from `cv::getDerivKernels(dx, dy, FILTER_SCHARR, false, CV_64F)`. Halo 1.</li><li>**Laplacian:** `kernelSize` ∈ {1, 3, 5, 7}. Size 1 is the 3×3 kernel [[0,1,0],[1,−4,1],[0,1,0]], computed as the sum of the second differences [1,−2,1] in x and in y. Larger sizes are d²/dx² + d²/dy² from `getDerivKernels(2,0,k)` and `getDerivKernels(0,2,k)`. Halo `max(1, kernelSize/2)`.</li><li>**Median:** odd `kernelSize` from 3 to 255; halo `kernelSize/2`.</li><li>**Bilateral:** odd `diameter` from 3 to 255; `sigmaColour` and `sigmaSpace` finite and > 0. The window is the disc of radius `diameter/2`. Weights are exp(−d²/2σs²)·exp(−(v − c)²/2σc²) on the sample values as read. Halo `diameter/2`. A NaN centre gives NaN; NaN neighbours are skipped.</li><li>**Canny:** `apertureSize` ∈ {3, 5, 7}; `lowThreshold` and `highThreshold` finite, 0 ≤ low ≤ high. Uses `L2gradient = false`. Halo is `apertureSize/2 + 1 + 32`: the gradient and non-maximum suppression need `apertureSize/2 + 1`, and the 32 is the declared hysteresis reach.</li></ul> |
| G13 | Exact fixed-order evaluation. | `detail::SeparableFilter` in `src/algorithm/detail/separable.hpp/.cpp` takes a horizontal and a vertical kernel of doubles and evaluates one plane. For each output pixel it forms each window row's weighted sum left to right, then the column of those sums top to bottom, in `double`, as `pixel.threshold_adaptive` does. **`pixel.threshold_adaptive` is refactored onto it in Task 1**, and its existing bitwise tests must still pass unchanged. The Laplacian is two separable passes added per pixel in a fixed order: the x term first. |
| G14 | Morphology's structuring element and iterations. | `StructuringElement` is pimpl'd with the full C-2 copy/move set. **Odd `width` and `height` from 1 to 255**, else `InvalidArgumentError`. The anchor is the centre. Shapes are drawn as `cv::getStructuringElement` draws `MORPH_RECT`, `MORPH_ELLIPSE` and `MORPH_CROSS`. `custom(mask, w, h)` copies `w·h` bytes, where non-zero means set; it needs at least one set pixel and a non-null mask. `iterations` runs from 1 to 64. **Halo:** `iterations · r` for erode and dilate, `2 · iterations · r` for open and close, per axis (r = width/2 or height/2). Open is erode n times, then dilate n times; close is the reverse. OpenCV's `morphologyEx` does exactly this. **Bag keys:** `shape` (`"Rectangle"`, `"Ellipse"`, `"Cross"`, `"Custom"`), `elementWidth`, `elementHeight`, `elementMask` (a double array of 0/1, present only for `Custom`) and `iterations`. Defaults are a 3×3 `Rectangle` and 1 iteration. |
| G15 | `BorderMode` is declared in §6.4 but no factory takes it. | It is declared exactly as the SDS has it. Its documentation says no v1 factory takes it, because the framework supplies border context: pixels beyond the level read as zero. A later version may expose it. |
| G16 | Canny's declared approximation (§6.4, open question 15). | The halo is the stated bound, and the complexity note says so verbatim: `"O(n) in the number of pixels; equals single-tile Canny wherever every weak edge chain reaches a strong edge within 32 pixels, and may drop weak edges beyond that reach"`. The requirement conflict stays recorded as open question 15. This plan amends no requirement. |

## Review Focus

1. **`Float32` Gaussian and Sobel at tile sizes {64, 100, 257}:** bit-identical to one tile, including the pixels within the halo of the level edge. Pinned in Tasks 1 and 2.
2. **Morphology near the level edge:** the zero read beyond the level makes erosion eat inward from the edge, identically under tiling and in one tile. Open and close need twice the halo of a single erosion. Pinned in Task 4.
3. **Median on `Byte` (OpenCV) and on `UInt16` (own) over the same values:** identical results. Pinned in Task 3.
4. **Canny's approximation is real and bounded:** short edges are identical under tiling, and a weak chain longer than 32 pixels from its strong seed across a seam is the documented exception. Pinned in Task 6.
5. **A NaN in a bilateral window:** documented behaviour, never a crash or a tile-dependent result. Pinned in Task 5.

---

### Task 1: The separable engine, `filter.hpp`, and `pixel.gaussian_blur`

**Files:**
- Create: `include/slideio/processor/algorithm/pixel/filter.hpp` (all of §6.4's declarations; later tasks implement them), `src/algorithm/detail/separable.hpp/.cpp`, `src/algorithm/pixel/gaussian_blur.cpp`, `tests/algorithm/test_gaussian_blur.cpp`, `tests/pipeline/header_check/check_algorithm_pixel_filter.cpp`.
- Modify: `src/algorithm/pixel/threshold_adaptive.cpp` (onto `SeparableFilter`, G13), `src/algorithm/CMakeLists.txt`, `src/algorithm/registry.cpp`, `src/algorithm/detail/builtin_operations.hpp`, `tests/algorithm/CMakeLists.txt`, `tests/pipeline/CMakeLists.txt`.

**Interfaces:**
- Produces:
  - `detail::SeparableFilter`: `SeparableFilter(std::vector<double> horizontal, std::vector<double> vertical);` (odd lengths) and `void apply(const BlockView& input, int32_t plane, const Rect64& haloRectAtLevel, const Rect64& tileRectAtLevel, double* out, int64_t outStride) const;`. It reads samples through the codec and writes `double`s.
  - `detail::writeFloatResult(BlockView& output, int32_t plane, const double* values, int64_t stride)`, which writes `Float32` or `Float64` per G10.
  - `filter.hpp` with every §6.4 declaration. The factories not yet implemented are left undefined until their task, so nothing links them before then.

- [ ] **Step 1: Write the failing tests** (`// Requirements: UR-PIX-3, UR-QA-2, UR-IMG-11, UR-ALG-6`):
  - `GaussianBlur.FactoryEqualsNamePath`: keys `sigma` (default 1.0) and `kernelSize` (default 0). `makeGaussianBlur(2.0, 0)` equals the name path with `sigma` 2.0 and `kernelSize` 0, and its halo is 6 (kernel 13).
  - `GaussianBlur.RejectsBadParameters`: `sigma` 0, −1 and NaN; `kernelSize` 2, −1 and 1027. Messages name the key, never the value.
  - `GaussianBlur.AnalyticResponses`:
    - a constant 100 image gives exactly 100 away from edges;
    - a linear ramp f = 3x gives 3x within 1e-9 away from edges;
    - an impulse of 1 at the centre reproduces the 2-D normalised kernel within 1e-15.
  - `GaussianBlur.OutputTypes`: `Byte`, `Int16` and `Float32` give `Float32`; `Float64` gives `Float64`. `Int64` is refused at validation.
  - `GaussianBlur.TiledEqualsSingleTile`: `Float32` and `UInt16` scenes of 300×260, `sigma` 2.5. Tiles {64, 100, 257} equal tile 4096 bitwise.
  - `ThresholdAdaptive.*`: the existing tests stay unchanged and green after the refactor.
- [ ] **Step 2: Run them to verify they fail.** Expected: the build fails on `filter.hpp`.
- [ ] **Step 3: Implement.** Reference text: `"Analytic: a normalised Gaussian kernel's response to constant, linear and impulse images"`. Complexity text: `"O(n * kernelSize) in the number of samples"`.
- [ ] **Step 4: Run.** Run `./run-tests.sh -R "algorithm\."`. Expected: all pass, 0 warnings.
- [ ] **Step 5: Commit.** `feat(algorithm): separable filters and the Gaussian blur`

---

### Task 2: `pixel.sobel`, `pixel.scharr`, `pixel.laplacian`

**Files:**
- Create: `src/algorithm/pixel/derivatives.cpp`, `tests/algorithm/test_derivatives.cpp`.
- Modify: `src/algorithm/registry.cpp`, `src/algorithm/detail/builtin_operations.hpp`.

**Interfaces:**
- Consumes: Task 1's `SeparableFilter` and `writeFloatResult`.
- Produces: `makeSobel`, `makeScharr`, `makeLaplacian` per §6.4 and G12. Bag keys: `dx`, `dy`, `kernelSize` for Sobel (defaults 1, 0, 3); `dx`, `dy` for Scharr (defaults 1, 0); `kernelSize` for Laplacian (default 1).

- [ ] **Step 1: Write the failing tests** (`// Requirements: UR-PIX-3, UR-QA-2, UR-ALG-6`):
  - `Derivatives.FactoriesEqualNamePaths`; `Derivatives.RejectsBadOrders`: Sobel (0, 0), (3, 0), `kernelSize` 4; Scharr (1, 1); Laplacian `kernelSize` 9.
  - `Derivatives.AnalyticResponses`, away from edges:
    - Sobel dx=1, size 3 on f = x gives 8;
    - Sobel dy=1, size 3 on f = y gives 8;
    - Sobel dx=2, size 3 on f = x² gives 8;
    - Scharr dx=1 on f = x gives 32;
    - Laplacian size 1 on f = x² + y² gives 4;
    - Laplacian size 3 on f = x² gives 8. That is OpenCV's 3×3 kernel [[2,0,2],[0,−8,0],[2,0,2]].

    Each is exact.
  - `Derivatives.CoefficientsMatchOpenCV`: for every valid (dx, dy, size), an impulse response equals the outer product of `cv::getDerivKernels`. The test target may link `opencv_imgproc` for this test only.
  - `Derivatives.TiledEqualsSingleTile`: `Float32` 300×260, Sobel (1, 1, 5), Scharr (0, 1), Laplacian 7. Tiles {64, 100, 257} equal tile 4096 bitwise.
- [ ] **Step 2: Run them to verify they fail.**
- [ ] **Step 3: Implement.** Reference texts:
  - `"Analytic: responses to polynomial images; coefficients of OpenCV getDerivKernels"` for Sobel and Laplacian;
  - `"Analytic: response to a linear ramp; Scharr coefficients [3 10 3] x [-1 0 1]"` for Scharr.

  Complexity text: `"O(n * kernelSize) in the number of samples"`.
- [ ] **Step 4: Run.** Run `./run-tests.sh -R "algorithm\."`. Expected: all pass.
- [ ] **Step 5: Commit.** `feat(algorithm): Sobel, Scharr and Laplacian derivatives`

---

### Task 3: OpenCV plane views and `pixel.median_blur`

**Files:**
- Create: `src/algorithm/detail/cv_plane.hpp/.cpp`, `src/algorithm/pixel/median_blur.cpp`, `tests/algorithm/test_median_blur.cpp`.
- Modify: `src/algorithm/registry.cpp`, `src/algorithm/detail/builtin_operations.hpp`.

**Interfaces:**
- Produces:
  - `cv::Mat detail::planeAsMat(const BlockView&, int32_t plane)`: a header over the plane's memory, with the type from `Backend`'s depth mapping, rebuilt locally. Algorithm may not include pipeline detail. It throws `UnsupportedDataTypeError` for a type OpenCV cannot hold.
  - `void detail::translateCv(const char* operationName)`: called from a `catch (const cv::Exception&)`. It throws `InternalError` with the OpenCV message as `cause()` (C-6).
  - `makeMedianBlur(int32_t kernelSize)`, with bag key `kernelSize` (default 3).

- [ ] **Step 1: Write the failing tests** (`// Requirements: UR-PIX-3, UR-QA-2, UR-IMG-11, UR-ALG-6`):
  - `MedianBlur.FactoryEqualsNamePath`; `MedianBlur.RejectsBadKernel`, with 2, 1 and 257.
  - `MedianBlur.MatchesDefinition`: on a random 61×47 image, every pixel equals the test's own sort-based median of its zero-padded window. This holds for `Byte` with sizes 3, 5 and 7 (OpenCV), and for `UInt16`, `Float32` and `Int64` with sizes 3 and 7 (own).
  - `MedianBlur.ByteAndUInt16Agree`: the same values 0–255 stored as `Byte` and as `UInt16` give equal medians at size 9.
  - `MedianBlur.OutputTypeIsInputType`.
  - `MedianBlur.TiledEqualsSingleTile`: `Byte` and `Float32` 300×260 at size 7, tiles {64, 100, 257} against 4096.
- [ ] **Step 2: Run them to verify they fail.**
- [ ] **Step 3: Implement.** The own path selects with `std::nth_element` over a per-pixel window copy. A NaN sample sorts as greater than every number, and this is documented. Reference text: `"Published definition: the middle value of the sorted window; checked against a direct sort-based reference"`. Complexity text: `"O(n * kernelSize^2) in the number of samples (O(n * kernelSize) for Byte)"`.
- [ ] **Step 4: Run.** Run `./run-tests.sh -R "algorithm\."`. Expected: all pass.
- [ ] **Step 5: Commit.** `feat(algorithm): median blur`

---

### Task 4: `StructuringElement` and morphology

**Files:**
- Create: `src/algorithm/pixel/structuring_element.cpp`, `src/algorithm/pixel/morphology.cpp`, `tests/algorithm/test_morphology.cpp`.
- Modify: `src/algorithm/registry.cpp`, `src/algorithm/detail/builtin_operations.hpp`.

**Interfaces:**
- Consumes: Task 3's `planeAsMat` and `translateCv`.
- Produces: `StructuringElement`, `makeErode`, `makeDilate`, `makeOpen`, `makeClose` per §6.4 and G14.

- [ ] **Step 1: Write the failing tests** (`// Requirements: UR-PIX-4, UR-QA-2, UR-ALG-6`):
  - `StructuringElement.ShapesAndLimits`:
    - `Ellipse` 5×5 sets exactly the pixels `cv::getStructuringElement(MORPH_ELLIPSE)` sets;
    - even sizes, 0 and 257 throw;
    - an all-zero custom mask throws;
    - copy and move work.
  - `Morphology.FactoriesEqualNamePaths`, including `Custom` with `elementMask`; `Morphology.HaloDoublesForOpenClose`: a 5×3 element at 2 iterations gives a halo of 4 left and right and 2 top and bottom for erode, and 8 and 4 for open.
  - `Morphology.AnalyticShapes`, on a binary `Byte` image of a 21×21 square at 255:
    - erode with a 3×3 rectangle gives the 19×19 square;
    - dilate gives 23×23;
    - open with 5×5 removes a 3×3 speck and keeps the square;
    - close with 5×5 fills a 2-pixel gap.
  - `Morphology.EdgeErodesInward`: an all-255 image eroded once by 3×3 gives 0 on the outermost ring of the level, because the zero beyond the level is read. Tiled runs agree.
  - `Morphology.TiledEqualsSingleTile`: `Float32` and `UInt16` random images, every operation, 2 iterations, `Ellipse` 7×5. Tiles {64, 100, 257} equal 4096.
- [ ] **Step 2: Run them to verify they fail.**
- [ ] **Step 3: Implement** with `cv::erode`, `cv::dilate` and `cv::morphologyEx` on the halo'd plane, anchor centre, `BORDER_CONSTANT` with OpenCV's default morphology border value. The halo guarantees the tile never sees that border. The tile rect is copied out of the result. Reference text: `"Analytic: erosion and dilation of synthetic binary shapes by known elements"`. Complexity text: `"O(n * elementWidth * elementHeight * iterations) in the number of samples"`.
- [ ] **Step 4: Run.** Run `./run-tests.sh -R "algorithm\."`. Expected: all pass.
- [ ] **Step 5: Commit.** `feat(algorithm): morphology with structuring elements`

---

### Task 5: `pixel.bilateral_filter`

**Files:**
- Create: `src/algorithm/pixel/bilateral_filter.cpp`, `tests/algorithm/test_bilateral_filter.cpp`.
- Modify: `src/algorithm/registry.cpp`, `src/algorithm/detail/builtin_operations.hpp`.

**Interfaces:**
- Produces: `makeBilateralFilter(int32_t diameter, double sigmaColour, double sigmaSpace)`, with bag keys `diameter`, `sigmaColour`, `sigmaSpace` (defaults 5, 25.0, 5.0).

- [ ] **Step 1: Write the failing tests** (`// Requirements: UR-PIX-3, UR-QA-2, UR-ALG-6`):
  - `BilateralFilter.FactoryEqualsNamePath`; `BilateralFilter.RejectsBadParameters`.
  - `BilateralFilter.MatchesDefinition`: on a random `Float32` 41×37 image, every pixel equals the test's own direct double loop over the disc. The weights must be summed in the same order (row by row, left to right), so the comparison is exact.
  - `BilateralFilter.PreservesAStepEdge`: a 0/200 step with `sigmaColour` 10 keeps both sides within 1e-6 of 0 and 200 at the edge.
  - `BilateralFilter.NanCentreAndNeighbour`: a NaN centre gives NaN; a NaN neighbour is skipped and does not spread.
  - `BilateralFilter.TiledEqualsSingleTile`: `Byte` and `Float32` 300×260, diameter 9. Tiles {64, 100, 257} equal 4096.
- [ ] **Step 2: Run them to verify they fail.**
- [ ] **Step 3: Implement.** Precompute the spatial weights per operation. The colour weight is computed per pair. Write per G10. Reference text: `"Tomasi C, Manduchi R. Bilateral filtering for gray and color images. ICCV 1998; checked against a direct per-pixel computation"`. Complexity text: `"O(n * diameter^2) in the number of samples"`.
- [ ] **Step 4: Run.** Run `./run-tests.sh -R "algorithm\."`. Expected: all pass.
- [ ] **Step 5: Commit.** `feat(algorithm): bilateral filter`

---

### Task 6: `pixel.canny`

**Files:**
- Create: `src/algorithm/pixel/canny.cpp`, `tests/algorithm/test_canny.cpp`.
- Modify: `src/algorithm/registry.cpp`, `src/algorithm/detail/builtin_operations.hpp`.

**Interfaces:**
- Consumes: Task 3's `planeAsMat` and `translateCv`.
- Produces: `makeCanny(double lowThreshold, double highThreshold, int32_t apertureSize)`, with bag keys `lowThreshold`, `highThreshold`, `apertureSize` (defaults 50.0, 150.0, 3).

- [ ] **Step 1: Write the failing tests** (`// Requirements: UR-PIX-3, UR-QA-2, UR-ALG-6, UR-ALG-8`):
  - `Canny.FactoryEqualsNamePath`; `Canny.RejectsBadParameters`: low > high, a negative value, NaN, aperture 4 and 9. `Canny.HaloIsTheDeclaredBound`: aperture 3 gives halo 34, 5 gives 35, and 7 gives 36.
  - `Canny.MatchesOpenCVOnOneTile`: on a 200×160 image of filled circles and rectangles, the one-tile result equals `cv::Canny` on the zero-padded image, cropped.
  - `Canny.ShortEdgesTileExactly`: circles of radius ≤ 10 well apart, with every edge strong. Tiles {64, 100} equal tile 4096.
  - `Canny.LongWeakChainIsTheDeclaredApproximation`: a straight line whose gradient sits between low and high for 120 pixels, with a strong seed only at its left end. At tile 64 the result differs from one tile, and only on pixels more than 32 from the seed. This pins the documented approximation.
  - `Canny.ComplexityNoteStatesTheBound`: the catalog's complexity note equals G16's text verbatim.
- [ ] **Step 2: Run them to verify they fail.**
- [ ] **Step 3: Implement.** Call `cv::Canny(src, dst, low, high, apertureSize, false)` on the halo'd plane and copy the tile rect out. Reference text: `"Canny J. A computational approach to edge detection. IEEE TPAMI 1986;8:679-698; equals OpenCV Canny on one tile"`. Complexity text: G16's.
- [ ] **Step 4: Run.** Run `./run-tests.sh -R "algorithm\."`. Expected: all pass.
- [ ] **Step 5: Commit.** `feat(algorithm): Canny edges with a declared hysteresis bound`

---

### Task 7: SDS fold, traceability

**Files:**
- Modify:
  - `docs/superpowers/specs/sds/06-algorithm.md`:
    - §6.2 parameter-bag table: every key of Tasks 1–6;
    - §6.4: G9 to G16, stated as rules, with their reasons;
    - §6.9: references and tolerances as tested;
    - §6.10: the catalog table's halo column made exact.
  - `docs/superpowers/specs/sds/08-class-diagrams.md`: `StructuringElement` and the filter factories.
  - `docs/traceability.md`: UR-PIX-3, UR-PIX-4 and UR-QA-2 rows.
  - `docs/project.md`: the algorithm plan B entry becomes "Implemented on branch `feat/algorithm-b`".

- [ ] **Step 1: Write the edits.** No ruling numbers or plan vocabulary.
- [ ] **Step 2: Verify.** Run `cmake --build build --target docs --config Release`. Expected: exit 0, 0 warnings. Then run `./run-tests.sh`. Expected: all pass.
- [ ] **Step 3: Commit.** `docs(algorithm): filters and morphology in the design`
