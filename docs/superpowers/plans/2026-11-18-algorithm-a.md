# `algorithm` Module Implementation Plan A: the catalog's scaffold and the point operations

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the `algorithm` module to `libslideio-processor-engine` with its registry and the six operations that need no neighbourhood library: `pixel.threshold_fixed`, `pixel.convert_type`, `pixel.colour_convert`, `pixel.stain_deconvolution`, `pixel.threshold_otsu` and `pixel.threshold_adaptive`.

**Architecture:**
- **This is plan A of three.** B adds the filters and morphology of SDS 06 §6.4 (eleven operations, Canny's bounded approximation included). C adds segmentation (§6.7), measurements and calibration (§6.8). A has no dependency on B or C. B and C reuse A's registry, factory helper, sample codec and test harness exactly as A's Interfaces blocks state.
- **The catalog is a leaf.** `src/algorithm/` builds the OBJECT library `slideio-processor-algorithm`, which the engine links. It includes only public headers (`include/`) and its own `src/algorithm/`, never `src/pipeline/`, so every built-in is implementable outside the library (UR-EXT-2). Operations self-register by name. Nothing above this module links it.
- **Every operation is a value plus a per-tile function.** A typed factory and the catalog name path build the same operation from the same complete parameter bag (§6.2). Each operation is tested two ways:
  - directly, by building an `OperationContext` the way the pipeline's own tests do, which reaches types the installed OpenCV refuses;
  - through a real `Pipeline` over a synthetic scene, at several tile sizes and worker counts.

**Tech Stack:** C++17, CMake ≥ 3.21, Conan 2, GoogleTest, OpenCV 4.14 (static, private to the engine). Plan A's operations use no OpenCV call; the link is set up here for plans B and C.

**Spec:** `docs/superpowers/specs/sds/06-algorithm.md` (SDS 06) §6.1–§6.3, §6.5, §6.6, §6.9, §6.10, read under SDS 00. The operation contract is SDS 05 §5.1, §5.3 and §5.8, as built (`include/slideio/processor/pipeline/`). The requirements are UR-PIX-1, UR-PIX-2, UR-PIX-5, UR-IMG-7, UR-IMG-10 to UR-IMG-13, UR-ALG-3 to UR-ALG-8, UR-EXT-2, UR-EXT-5 and UR-QA-2/3 in `docs/requirements.md`.

## Global Constraints

- C++17 in `namespace slideio::processor`; private helpers in `slideio::processor::detail`, under `src/algorithm/detail/`.
- Public headers under `include/slideio/processor/algorithm/` (`registry.hpp`, `pixel/colour.hpp`, `pixel/threshold.hpp`, `pixel/convert.hpp`), exported with `SLIDEIO_PROCESSOR_ENGINE_EXPORTS` from `pipeline/export.hpp`.
- ABI rules C-2: pimpl for every non-trivial class, every definition out of line, no STL in signatures except `std::shared_ptr`, every `enum class` is `: int32_t`.
- Strings C-4, collections C-5, errors C-6: only `ProcessorError` subclasses and `std::bad_alloc` leave a public function. Within `apply()`, any other exception is a defect.
- **No OpenCV type, header or macro in any public header.** The header check of `tests/pipeline/CMakeLists.txt` is extended to `algorithm/`.
- **Signatures are SDS-exact.** Copy SDS 06 character for character. The only additions are those the rulings name.
- Doxygen on every public declaration with `@throws`, `@par Thread-safety` and `@par Requirements`, as the pipeline headers do. `cmake --build build --target docs --config Release` must exit 0 with zero warnings; check the exit code.
- A `// Requirements: …` line at the top of every source and test file.
- **No pixel value, sample value or parameter value in any error message or log line.** Name the operation, the type, the channel and counts only.
- Public docs and code comments do not cite plan vocabulary (ruling numbers, "plan A", "Task 3"). They state the rule itself.
- **Build on this machine:** `./build.sh --skip-slideio`, then `./run-tests.sh`; `-R <regex>` runs one test. The build must have 0 warnings.
- **Commits:** one per task, on branch `feat/algorithm-a`, conventional prefix (`build:`, `feat(algorithm):`, `test(algorithm):`, `docs(algorithm):`). Org policy: AI-generated code is human-reviewed before it reaches `main`.

## Rulings on spec gaps

Numbered G1 onward, so they never collide with storage's S- or pipeline's P-numbers. They bind this plan and the next two. Task 7 folds them into SDS 06, SDS 08 and the module structure design.

| # | Gap | Ruling |
|---|---|---|
| G1 | The design confines OpenCV to `pipeline`, but plans B and C need neighbourhood kernels, labelling and watershed. **User decision.** | OpenCV is confined to the **engine library**, not to one module. `slideio-processor-algorithm` links `opencv_core` and `opencv_imgproc` PRIVATE. Its include path is `include/`, SlideIO's, and `src/algorithm/` (as the root for `#include "detail/…"`), with **no `src/`**, so it cannot reach `pipeline`'s detail. OpenCV never appears in a public header. Every operation keeps its own correctness test (UR-ALG-6), so OpenCV's numerics are checked, not trusted. The design's "OpenCV is confined to `pipeline`" and "the only module linking OpenCV" are amended in Task 7. |
| G2 | UR-IMG-7: `makeColourConvert(Rgb, …)` must refuse non-RGB channels, but an operation never sees the scene and SlideIO has no "channels are RGB" flag. **User decision.** | **RGB is inferred from the channels:** the input holds exactly three planes, each `Byte`, whose `planeSourceChannel()` values are 0, 1, 2 in that order. The check runs in `apply()`, not in the factory, which has no scene. A failure throws `ChannelArityError` (wrong plane count) or `InvalidArgumentError` (wrong type or channel order). The run reports it as the pipeline reports every tile failure: a `TileError` naming the operation, with the message as its `cause()`. The message names the operation, the requirement ("needs channels 0, 1, 2 as RGB, Byte") and the channels found. `pixel.stain_deconvolution` makes the same check. SDS 06 §6.3's "throws `InvalidArgumentError` when the scene's metadata does not state…" is amended to this. |
| G3 | The parameter bag of each factory: keys, enum spellings, defaults. | **`parameters()` is always the complete bag:** every key below is present, with defaults filled in for any key the caller omitted. So a typed factory and `OperationCatalog::create` with a partial bag compare equal after normalising. **Unknown keys and wrong value types throw `InvalidArgumentError`** naming the operation and the key, never the value, so a misspelt recorded configuration cannot replay as defaults. **Enums are stored as their enumerator spelling**, e.g. `"Above"`, `"Gaussian"`, `"TowardZero"`. **A `DataType` is stored as `dataTypeName()`**, e.g. `"Float32"`. The keys are listed in each task. |
| G4 | Registration (§6.1): idempotent, automatic at load, thread-safe. | `src/algorithm/registry.cpp` keeps one mutex and registers per name. A name `OperationCatalog::isRegistered` already holds is left as it is, whoever registered it. If `registerFactory` throws `InvalidArgumentError` because another thread registered that name in between, the error is swallowed when the name is now registered. A file-scope static object calls `registerBuiltinOperations()` at library load inside `try { … } catch (...) {}`, since an exception there would terminate the process. **No `std::call_once`**, because a throwing callable can hang it on some standard libraries. `registerSegmentationOperations()` and `registerMeasurementOperations()` register nothing until their operations exist; their docs say so without naming plans. |
| G5 | Factories. | `detail::BuiltinFactory` (`src/algorithm/detail/builtin_factory.hpp/.cpp`) implements `OperationFactory` from a maker function, a correctness reference and a complexity note. It holds a prototype built from an empty bag and answers every declaration from it, as the pipeline test helper `PrototypeFactory` does. Each operation's reference and complexity text is given in its task, verbatim. |
| G6 | Reading and writing samples of every `DataType` without loss (UR-IMG-10/11). | `src/algorithm/detail/sample_codec.hpp/.cpp`. `readSample(plane, x)` returns a `Sample`: integer types as `int64_t` or `uint64_t` (`UInt64`), never through `double`; float types as `double`. `Float16` is decoded exactly by an own binary16 routine, since `core` has no public one and the engine's half helper is `pipeline` detail. Writing to `Float16` rounds to nearest, ties to even. `dataTypeFromName(const char*)` parses `dataTypeName` spellings and throws `InvalidArgumentError` otherwise. |
| G7 | `StainVectors` is a non-trivial class with only a default constructor in SDS 06. | It is pimpl'd with the full C-2 copy/move set and a destructor, as P7 did for `OperationParameters`. **At most three stains.** `setVector(index, …)` takes `index` from 0 to `count()`, where `count()` appends, else `InvalidArgumentError`. A vector with a negative or non-finite component, or all three components zero, throws `InvalidArgumentError`; vectors are stored as given and normalised at use. `getVector`, `stainName` and `setStainName` with an out-of-range index throw `InvalidArgumentError`, and so does `setStainName` with a null name. `stainName()` is `""` until set; its pointer is valid until the next non-const call. |
| G8 | Thresholds' output. | Every `pixel.threshold_*` writes `Byte`: 255 for foreground, 0 for background. `Above` makes a sample foreground when it is **strictly greater** than the threshold, `Below` when it is **strictly less**. A NaN sample is always background. For integer inputs the comparison is **exact**: no sample is converted to `double`, so `Int64` 2⁵³ + 1 against threshold 2⁵³ is foreground for `Above`. |

## Review Focus

1. **64-bit integers near 2⁵³** (`Int64`, `UInt64`) through `pixel.threshold_fixed` and `pixel.convert_type`: results must be exact, with no detour through `double`. Pinned by direct-apply tests in Tasks 1 and 2.
2. **Streamed Otsu across workers and tiles:** at {1, 4, 8} workers and tile sizes {64, 100, 257}, the streamed Otsu must equal one computed from the full in-memory histogram. This includes a polygon-restricted run, which counts only the pixels the restriction admits. Pinned in Task 5.
3. **Adaptive thresholding on `Float32` input:** the output must be bit-identical across tile sizes {64, 100, 257} and equal to one tile covering the level, including the pixels within `blockSize/2` of the level's edge. Pinned in Task 6.
4. **A selection that only looks like RGB:** channels {2, 1, 0}, four channels, or three `UInt16` channels must fail with a `TileError` naming `pixel.colour_convert`, never produce output. Pinned in Task 3.
5. **NaN, ±∞, out-of-range floats and `Float16` subnormals** through `pixel.convert_type` under both overflow modes: each must give the documented result, and no error message may carry a sample value. Pinned in Task 2.

---

### Task 1: The module, the registry, the test harness, and `pixel.threshold_fixed`

**Files:**
- Create: `include/slideio/processor/algorithm/registry.hpp`, `include/slideio/processor/algorithm/pixel/threshold.hpp` (the fixed threshold only; Tasks 5 and 6 add to it), `src/algorithm/CMakeLists.txt`, `src/algorithm/registry.cpp`, `src/algorithm/detail/builtin_factory.hpp/.cpp`, `src/algorithm/detail/sample_codec.hpp/.cpp`, `src/algorithm/detail/bag.hpp/.cpp`, `src/algorithm/pixel/threshold_fixed.cpp`, `tests/algorithm/CMakeLists.txt`, `tests/algorithm/test_registry.cpp`, `tests/algorithm/test_threshold_fixed.cpp`, `tests/algorithm/support/harness.hpp/.cpp`.
- Modify: `src/CMakeLists.txt` (add `algorithm`; the engine links `slideio-processor-algorithm` PRIVATE), `tests/CMakeLists.txt`, `tests/pipeline/CMakeLists.txt` (the OpenCV-free header check gains one source per algorithm header, `header_check/check_algorithm_*.cpp`).

**Interfaces:**
- Produces:
  - **SDS 06 §6.1 exactly.**
  - **`detail::BuiltinFactory`:** `BuiltinFactory(OperationPtr (*maker)(const OperationParameters&), const char* correctnessReference, const char* complexityNote)`.
  - **`detail::Sample` and its codec:** `struct Sample { enum class Kind : int32_t { Signed, Unsigned, Float } kind; int64_t s; uint64_t u; double f; };`, with `Sample readSample(const void* row, int64_t x, DataType)` and `DataType dataTypeFromName(const char*)`. Task 2 adds the writing side.
  - **`detail::BagReader`** (`detail/bag.hpp`): built from `(const char* operationName, const OperationParameters&)`. It has `double getDouble(const char* key, double fallback)`, `int64_t getInt(…)`, `bool getBool(…)`, `const char* getString(…)` and a template `E getEnum(const char* key, E fallback, const char* const* spellings, int32_t count)`. Each getter marks its key as read. `void finish()` throws `InvalidArgumentError` for any key never read, i.e. an unknown key. A present key of the wrong type throws as well. Each operation builds its complete bag (G3) after reading.
  - **The threshold header:** `enum class ThresholdDirection : int32_t { Above, Below };` and `OperationPtr makeFixedThreshold(double value, ThresholdDirection);`.
  - **Test harness** (`tests/algorithm/support/harness.hpp`, namespace `slideio::processor::testing_support`):
    - `struct Image { int64_t width, height; std::vector<DataType> types; std::vector<std::vector<uint8_t>> planes; double at(int32_t plane, int64_t x, int64_t y) const; };`
    - `Image applyDirect(Operation&, const std::vector<DataType>& inputTypes, const std::vector<int32_t>& sourceChannels, int64_t width, int64_t height, const std::function<double(int32_t plane, int64_t x, int64_t y)>& pixel);` builds a context through `detail::OperationContextAccess`, as `tests/pipeline` does, and applies one tile with no halo.
    - `Image applyDirectRaw(Operation&, …)`: the same, but filled from raw sample bytes, for 64-bit and `Float16` inputs.
    - `Image runEager(std::shared_ptr<slideio::Scene>, const PipelineSpec&, int32_t tileSize, int32_t workers);` runs `Pipeline::run` with a `RasterSink` that assembles the level.

- [ ] **Step 1: Write the failing tests.**

`test_registry.cpp` (`// Requirements: UR-EXT-5, UR-IMG-12, UR-ALG-4, UR-ALG-6, UR-ALG-8, UR-STG-4`):
```cpp
TEST(Registry, RegisteredAtLoad) { EXPECT_TRUE(OperationCatalog::instance().isRegistered("pixel.threshold_fixed")); }
TEST(Registry, IdempotentAndThreadSafe) {
    // 8 threads x 100 calls of registerBuiltinOperations(), registerPixelOperations(),
    // registerSegmentationOperations(), registerMeasurementOperations(): no throw, count() unchanged.
}
TEST(Registry, EveryBuiltinIsConformant) {
    // for every catalog name with prefix "pixel.", "segment." or "measure.":
    //   testing_support::expectConformant(name);
    //   correctnessReference(name) and complexityNote(name) are non-empty
}
TEST(Registry, SupportMatrixRowsMatchRuntime) {
    // writeSupportMatrix to a temp file; for each built-in row and each type t,
    // the cell is "yes" exactly when a one-operation Pipeline over a 1-channel synthetic
    // scene of type t validates (types SlideIO cannot synthesise are checked against
    // supportedTypes(name) and Backend::instance().supports(t) instead).
}
```
`test_threshold_fixed.cpp` (`// Requirements: UR-PIX-5, UR-IMG-11, UR-ALG-6, UR-QA-2`):
```cpp
TEST(ThresholdFixed, FactoryEqualsNamePath) {
    OperationParameters p; p.setDouble("value", 100.0).setString("direction", "Below");
    EXPECT_TRUE(parametersEqual(makeFixedThreshold(100.0, ThresholdDirection::Below)->parameters(),
                                OperationCatalog::instance().create("pixel.threshold_fixed", p)->parameters()));
}
TEST(ThresholdFixed, DefaultsFillTheBag) { /* create with an empty bag: value 128.0, direction "Above" */ }
TEST(ThresholdFixed, RejectsUnknownKeyAndBadValues) {
    // key "valeu" -> InvalidArgumentError naming "valeu"; direction "Up" -> InvalidArgumentError;
    // what() never contains the offending value text
}
TEST(ThresholdFixed, StrictComparisonEveryType) {
    // applyDirect / applyDirectRaw for each of the 11 types with samples {t-1, t, t+1} around the
    // threshold t: Above gives {0, 0, 255}, Below gives {255, 0, 0}; NaN is 0 under both
}
TEST(ThresholdFixed, Int64Near2Pow53IsExact) {
    // Int64 samples 2^53 and 2^53 + 1 against value 9007199254740992.0: Above gives 0 and 255.
    // UInt64 18446744073709551615 against 1.8446744073709552e19: Above gives 0
}
TEST(ThresholdFixed, TiledEqualsSingleTile) { /* runEager at tile 64, 100, 257 and 4096 on a 300x260 Byte scene: equal */ }
```

- [ ] **Step 2: Run them to verify they fail.** Run `./build.sh --skip-slideio`. Expected: the build fails, because `registry.hpp` does not exist.

- [ ] **Step 3: Implement.**
  - The CMake follows G1. The algorithm test target mirrors `tests/pipeline/CMakeLists.txt`:
    - links `slideio-processor-engine`, `slideio_processor_pipeline_test_support` and `GTest::gtest_main`;
    - compiles `src/pipeline/detail/operation_context_state.cpp` in directly;
    - has `src/` and `tests/` on its include path (tests may reach pipeline detail; the library may not);
    - uses `TEST_PREFIX "algorithm."` and the same Windows `PATH` handling.
  - `pixel.threshold_fixed`:
    - PerChannel, halo none, `requiresUniformTypes` false, `producesObjects` false;
    - supported types are all eleven; `outputType` is `Byte`;
    - keys `value` (double, finite; default 128.0) and `direction` (default `"Above"`);
    - integer comparison is exact per G8. An integer sample `v` is above `t` exactly when `v > floor(t)`; it is below `t` exactly when `v < ceil(t)`. Both bounds are clamped into the type's range before comparing.
  - Reference text: `"Analytic: a sample is foreground exactly when it is strictly above (or below) the value; NaN is background"`. Complexity text: `"O(n) in the number of samples"`.

- [ ] **Step 4: Run.** Run `./build.sh --skip-slideio && ./run-tests.sh -R "algorithm\."`. Expected: all pass, 0 warnings. Then run `./run-tests.sh` in full. Expected: no regression, including `pipeline.` (the header check builds).

- [ ] **Step 5: Commit.** `feat(algorithm): the algorithm module, its registry, and the fixed threshold`

---

### Task 2: `pixel.convert_type`

**Files:**
- Create: `include/slideio/processor/algorithm/pixel/convert.hpp`, `src/algorithm/pixel/convert_type.cpp`, `tests/algorithm/test_convert_type.cpp`.
- Modify: `src/algorithm/detail/sample_codec.hpp/.cpp` (the writing side), `src/algorithm/registry.cpp`.

**Interfaces:**
- Consumes: Task 1's codec, `BagReader`, `BuiltinFactory` and harness.
- Produces:
  - **SDS 06 §6.6 exactly:** `RoundingMode`, `OverflowMode`, `makeTypeConvert(DataType target, RoundingMode, OverflowMode)`.
  - **`detail::writeSample(void* row, int64_t x, DataType, const Sample&, RoundingMode, OverflowMode, bool& overflowed)`:** it never throws. A value that is out of range, NaN-to-integer or infinite-to-integer sets `overflowed` and writes the saturated value per the rules below.

The conversion rules, the operation's documented behaviour (UR-IMG-13):

| From → to | Rule |
|---|---|
| integer → integer | Exact when in range. Out of range: `Saturate` clamps to the target's min or max; `Error` fails. |
| integer → float | Nearest representable, ties to even. Beyond `Float16`'s ±65504: `Saturate` gives ±65504, `Error` fails. |
| float → integer | Round by `RoundingMode`; `Nearest` is ties to even. Then range: `Saturate` clamps, `Error` fails. NaN: `Saturate` gives 0, `Error` fails. ±∞: `Saturate` gives min or max, `Error` fails. |
| float → wider or equal float | Exact. |
| float → narrower float | Nearest, ties to even. A finite value beyond the target's maximum: `Saturate` gives ± the largest finite value, `Error` fails. NaN and ±∞ are kept. |

`RoundingMode` affects only conversions to integer types. Under `Error`, the tile fails with `InvalidArgumentError("pixel.convert_type: <n> samples of <source type> do not fit <target type>")`, which carries a count and never a value.

- [ ] **Step 1: Write the failing tests** (`// Requirements: UR-IMG-13, UR-IMG-10, UR-IMG-11, UR-ALG-6`):
  - `ConvertType.FactoryEqualsNamePath`: keys `target` (default `"Float32"`), `rounding` (default `"Nearest"`), `overflow` (default `"Saturate"`). An unknown `target` spelling throws.
  - `ConvertType.IdentityIsBitExactEveryType`: `UInt32`, `Int64` and `UInt64` include min, max and 2⁵³ + 1 (the UR-IMG-10 acceptance case). `Float16` includes a subnormal, −0, +∞ and NaN.
  - `ConvertType.IntegerBoundaries`:
    - `Int16` → `Byte`: −1, 0, 255, 256 give 0, 0, 255, 255 under `Saturate`; under `Error` the tile throws, and `what()` contains `"2 samples"` and not `"256"`.
    - `UInt64` max → `Int64`: saturates to `INT64_MAX`.
    - `Int64` min → `UInt32`: saturates to 0.
  - `ConvertType.RoundingModes`: `Float32` 2.5, −2.5, 1.5 and −0.5 → `Int32` give:
    - `Nearest`: 2, −2, 2, 0 (−0.5 rounds to −0, written as 0);
    - `TowardZero`: 2, −2, 1, 0;
    - `Floor`: 2, −3, 1, −1;
    - `Ceil`: 3, −2, 2, 0.
  - `ConvertType.NonFiniteToInteger`: NaN, +∞ and −∞ → `Byte` give 0, 255, 0 under `Saturate`; each one alone fails under `Error`.
  - `ConvertType.FloatNarrowing`:
    - `Float64` 1e300 → `Float32`: `FLT_MAX` under `Saturate`, fails under `Error`.
    - `Float32` 65520.0 → `Float16`: 65504 under `Saturate`, fails under `Error`.
    - `Float64` 1 + 2⁻⁵³ → `Float32`: 1.0.
    - NaN and ∞ pass through.
  - `ConvertType.Int64ToFloat64RoundsToNearest`: 2⁵³ + 1 gives 2⁵³.
  - `ConvertType.ThroughAPipeline`: `runEager` on a `UInt16` scene → `Float32` is exact at tile sizes 64 and 257.
- [ ] **Step 2: Run them to verify they fail.** Run `./run-tests.sh -R "algorithm\.ConvertType"`. Expected: build failure on `convert.hpp`.
- [ ] **Step 3: Implement.** PerChannel, halo none, all eleven types supported, `outputType` = `target`. Reference text: `"Analytic: the documented rounding and overflow rules, checked at each type's minimum, maximum and overflow"`. Complexity text: `"O(n) in the number of samples"`.
- [ ] **Step 4: Run.** Run `./run-tests.sh -R "algorithm\."`. Expected: all pass.
- [ ] **Step 5: Commit.** `feat(algorithm): explicit type conversion`

---

### Task 3: `pixel.colour_convert`

**Files:**
- Create: `include/slideio/processor/algorithm/pixel/colour.hpp` (`ColourSpace` and `makeColourConvert`; Task 4 adds the stain part), `src/algorithm/pixel/colour_convert.cpp`, `src/algorithm/detail/colour_math.hpp/.cpp`, `src/algorithm/detail/rgb_input.hpp/.cpp`, `tests/algorithm/test_colour_convert.cpp`.
- Modify: `src/algorithm/registry.cpp`.

**Interfaces:**
- Produces:
  - **SDS 06 §6.3** `ColourSpace` and `makeColourConvert(ColourSpace from, ColourSpace to)`.
  - **`detail::requireRgbInput(const BlockView& input, const char* operationName)`:** the G2 check. Task 4 calls it too.

The operation's conventions:
- **Kind and declarations:**
  - Joint, with `requiresUniformTypes` true;
  - halo none;
  - supported types `{Byte, Float32}`;
  - `outputType` is `Float32`;
  - `outputPlaneCount` is 1 when `to == Grey`, else 3.
- **Keys:** `from` and `to`. Defaults are `"Rgb"` and `"Grey"`. `from == to` throws `InvalidArgumentError`.
- **Input:**
  - `from == Rgb` requires the G2 RGB input, which is `Byte`.
  - `from == Grey` requires one plane, `Byte` or `Float32`.
  - `Hsv` and `Lab` require three `Float32` planes, as this operation writes them.
  - Otherwise it throws `ChannelArityError` or `UnsupportedDataTypeError` naming the operation.
- **Units:**
  - RGB and grey are on the 0–255 scale.
  - HSV has H in degrees [0, 360), with 0 when S = 0, and S, V in [0, 1].
  - Lab is CIE L\*a\*b\*: sRGB per IEC 61966-2-1 with its linearisation, D65 white (0.95047, 1, 1.08883), L\* in [0, 100].
  - Grey from RGB is 0.299 R + 0.587 G + 0.114 B. RGB from grey replicates the value.
  - Every pair converts through RGB. Output is never clamped: an out-of-gamut Lab gives RGB outside 0–255, as UR-IMG-11 requires rather than saturating.
- **Reference text:** `"Analytic: IEC 61966-2-1 sRGB, CIE 1976 L*a*b* with D65 white, hexcone HSV, ITU-R BT.601 luma; round trips within 1e-3 on the 0-255 scale"`.
- **Complexity text:** `"O(n) in the number of pixels"`.

- [ ] **Step 1: Write the failing tests** (`// Requirements: UR-PIX-1, UR-IMG-7, UR-ALG-3, UR-ALG-6`):
  - `ColourConvert.FactoryEqualsNamePath`; `ColourConvert.SameSpaceRejected`.
  - `ColourConvert.KnownValues`:
    - RGB (255, 0, 0) gives HSV (0, 1, 1) and Lab (53.2408, 80.0925, 67.2032), within 1e-3.
    - RGB (255, 255, 255) gives Lab (100, 0, 0), within 1e-3.
    - RGB (10, 20, 30) gives grey 18.15, within 1e-4.
  - `ColourConvert.RoundTrips`: over all 2¹⁸ RGB triples on a 64-step grid, Rgb → Hsv → Rgb and Rgb → Lab → Rgb come back within 1e-3. Each half runs through `applyDirect`, the second on the first's `Float32` output.
  - `ColourConvert.RgbIsInferredFromChannels`: through a `Pipeline`, each of these fails with a `TileError` whose `operationName` is `"pixel.colour_convert"`:
    - selection {2, 1, 0} on a 3-channel `Byte` scene;
    - a 4-channel `Byte` scene with `all()` selected;
    - a 3-channel `UInt16` scene.

    Selection {0, 1, 2} on a 3-channel `Byte` scene succeeds.
  - `ColourConvert.TiledEqualsSingleTile`: `runEager` at tile sizes 64 and 257 equals a single tile, bit for bit.
- [ ] **Step 2: Run them to verify they fail.**
- [ ] **Step 3: Implement.** Compute in `double`; write `Float32`.
- [ ] **Step 4: Run.** Run `./run-tests.sh -R "algorithm\."`. Expected: all pass.
- [ ] **Step 5: Commit.** `feat(algorithm): colour space conversion`

---

### Task 4: `StainVectors` and `pixel.stain_deconvolution`

**Files:**
- Modify: `include/slideio/processor/algorithm/pixel/colour.hpp` (`StainVectors`, `makeStainDeconvolution`), `src/algorithm/registry.cpp`.
- Create: `src/algorithm/pixel/stain_vectors.cpp`, `src/algorithm/pixel/stain_deconvolution.cpp`, `tests/algorithm/test_stain_deconvolution.cpp`.

**Interfaces:**
- Produces: SDS 06 §6.3's `StainVectors` per G7, and `makeStainDeconvolution(const StainVectors&)`.

The operation's conventions:
- **Built-in vectors** (Ruifrok & Johnston, *Anal Quant Cytol Histol* 23:291–299, 2001):
  - Hematoxylin: (0.650, 0.704, 0.286);
  - Eosin: (0.072, 0.990, 0.105);
  - DAB: (0.268, 0.570, 0.776).

  `hematoxylinEosin()` is {Hematoxylin, Eosin}. `hematoxylinDab()` is {Hematoxylin, DAB}. Names are exactly those words.
- **Keys:**
  - `vectors`: a double array of 3 × count, as given;
  - `stainName0` to `stainName<count−1>`: strings.

  The default is H&E. Building throws `InvalidArgumentError` when count is not 2 or 3, or when the unit vectors are linearly dependent (|det| < 1e-6 after completion).
- **Kind and declarations:**
  - Joint, with `requiresUniformTypes` true;
  - halo none;
  - supported types `{Byte}`, with the G2 RGB input;
  - `outputType` is `Float32`;
  - `outputPlaneCount` is the stain count.
- **Computation:**
  1. Optical density per channel: OD = −log10(max(I, 1) / 255).
  2. Normalise each vector to unit length. With two stains, the third row of the matrix M is the unit cross product of the first two.
  3. Concentrations are OD · M⁻¹. One plane is written per stain, in order; the completed third component is not written.
- **Reference text:** `"Ruifrok AC, Johnston DA. Quantification of histochemical staining by color deconvolution. Anal Quant Cytol Histol 2001;23:291-299; synthetic images composed from known vectors"`.
- **Complexity text:** `"O(n) in the number of pixels"`.

- [ ] **Step 1: Write the failing tests** (`// Requirements: UR-PIX-2, UR-IMG-7, UR-ALG-6`):
  - `StainVectors.Builtins`: values and names as above.
  - `StainVectors.SetGetAndLimits`: G7's errors, including a fourth stain; copy, move and assignment.
  - `StainDeconvolution.FactoryEqualsNamePath`; `StainDeconvolution.RejectsDependentVectors` (two equal vectors).
  - `StainDeconvolution.RecoversComposedIntensities`: for concentrations c₁, c₂ ∈ {0, 0.1, …, 1.0}, build pixels I = round(255 · 10^(−(c₁·v₁ + c₂·v₂))) per channel. Every recovered c is within 0.01 of the true value wherever every channel's I ≥ 64. This holds for both built-ins. The tolerance is stated in the operation's docs.
  - `StainDeconvolution.RequiresRgb`: the same three failing selections as Task 3.
- [ ] **Step 2: Run them to verify they fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run.** Run `./run-tests.sh -R "algorithm\."`. Expected: all pass.
- [ ] **Step 5: Commit.** `feat(algorithm): stain colour deconvolution`

---

### Task 5: `pixel.threshold_otsu`

**Files:**
- Modify: `include/slideio/processor/algorithm/pixel/threshold.hpp` (`makeOtsuThreshold`), `src/algorithm/registry.cpp`.
- Create: `src/algorithm/pixel/threshold_otsu.cpp`, `tests/algorithm/test_threshold_otsu.cpp`.

**Interfaces:**
- Consumes:
  - `Histogram64` (SDS 05 §5.8);
  - `Operation::accumulate`, `combine` and `finalizeAccumulation`;
  - the PerChannel accumulation contract: one call per plane, told apart by `planeSourceChannel()`.
- Produces: `OperationPtr makeOtsuThreshold(ThresholdDirection);`.

The operation's conventions:
- **Kind and declarations:**
  - PerChannel, halo none;
  - `needsGlobalContext()` true;
  - supported types are all eleven;
  - `outputType` is `Byte`.
- **Keys:**
  - `direction`, default `"Above"`;
  - optional `histogramMinimum` and `histogramMaximum`, finite doubles with minimum < maximum;
  - `binCount`, an int from 2 to 1048576, default 256.

  `binCount` is meaningful only with the range. When the range is absent, the complete bag omits all three, the documented exception to G3's complete bag.
- **Bins:**
  - With the range, bins are `Histogram64(binCount, min, max)`.
  - Without it, `Byte`, `Int8`, `UInt16` and `Int16` get one bin per value over the type's full range.
  - Any other type without a range fails the run in `accumulate` with `InvalidArgumentError` naming the operation and the type, and saying that `histogramMinimum`/`histogramMaximum` are needed.
- **Accumulation:**
  - One histogram per source channel.
  - Only the samples of `tileRectAtLevel()` that `mask()` admits are counted (a null mask admits all).
  - `combine` adds bins channel by channel.
- **Finalisation per channel:**
  - k\* is the smallest k in [0, binCount − 2] maximising ω₀ω₁(μ₀ − μ₁)², with both classes non-empty. The class means are taken over bin indices, computed in `double` from the 64-bit counts in ascending bin order.
  - When no k gives two non-empty classes, k\* is the highest non-empty bin.
  - A channel that counted nothing makes every sample background.
- **Apply:**
  - A sample's bin is computed exactly as `Histogram64` computes it.
  - Below the range counts as bin −1; above it counts as bin `binCount`.
  - `Above` makes bin > k\* foreground; `Below` makes bin ≤ k\* foreground. NaN is background.
- **Reference text:** `"Otsu N. A threshold selection method from gray-level histograms. IEEE Trans SMC 1979;9:62-66; equals the threshold of a full in-memory histogram of the same slide"`.
- **Complexity text:** `"O(n) in the number of samples, plus O(bins) once per channel; two passes over the slide"`.

- [ ] **Step 1: Write the failing tests** (`// Requirements: UR-PIX-5, UR-QA-3, UR-EXE-1, UR-ALG-6`):
  - `ThresholdOtsu.FactoryEqualsNamePath`, with and without a range.
  - `ThresholdOtsu.BimodalAnalytic`: a `Byte` scene with two equal populations at 60 and 180 puts k\* between them. The output is foreground exactly on the 180 pixels for `Above`.
  - `ThresholdOtsu.StreamedEqualsInMemory`: on a 640 × 480 `Byte` scene with a textured trimodal function, the test builds the full histogram in memory and computes k\* with its own straightforward Otsu. The run at {1, 4, 8} workers × tiles {64, 100, 257} gives output equal to thresholding at that k\*, pixel for pixel.
  - `ThresholdOtsu.RestrictedCountsOnlyAdmittedPixels`: the same as the previous test with `restrictToPolygon` set to a triangle. The in-memory reference counts only the pixels set in a `RasterMask` over the level, filled by `rasteriseInto(mask, triangle)`. That is the same conservative rasteriser the restriction uses, so the two agree on edge pixels by construction.
  - `ThresholdOtsu.FloatNeedsARange`: a `Float32` scene without a range fails with `TileError`, whose cause names the operation and `"histogramMinimum"`. With a range [0, 1] and 256 bins, it equals the in-memory reference.
  - `ThresholdOtsu.ConstantImage`: an all-100 `Byte` scene gives all 0 for `Above` and all 255 for `Below`.
- [ ] **Step 2: Run them to verify they fail.**
- [ ] **Step 3: Implement.** The accumulator state is a channel → `Histogram64` map plus the computed k\* per channel. `accumulate` reads samples through the codec. For one-bin-per-value types, a sample's bin is computed as an integer, without `double`.
- [ ] **Step 4: Run.** Run `./run-tests.sh -R "algorithm\."` with `--gtest_repeat=3 --gtest_shuffle`. Expected: all pass.
- [ ] **Step 5: Commit.** `feat(algorithm): streamed Otsu threshold`

---

### Task 6: `pixel.threshold_adaptive`

**Files:**
- Modify: `include/slideio/processor/algorithm/pixel/threshold.hpp` (`AdaptiveMethod`, `makeAdaptiveThreshold`), `src/algorithm/registry.cpp`.
- Create: `src/algorithm/pixel/threshold_adaptive.cpp`, `tests/algorithm/test_threshold_adaptive.cpp`.

**Interfaces:**
- Produces: `OperationPtr makeAdaptiveThreshold(AdaptiveMethod, int32_t blockSize, double offset, ThresholdDirection);`.

The operation's conventions:
- **Kind and declarations:**
  - PerChannel;
  - halo `Halo::uniform(blockSize / 2)`;
  - supported types are all except `UInt64` and `Int64`, whose window sums do not fit exactly;
  - `outputType` is `Byte`.
- **Keys:**
  - `method`: default `"Mean"`;
  - `blockSize`: an odd int from 3 to 1025, default 11;
  - `offset`: a finite double, default 2.0;
  - `direction`: default `"Above"`.
- **The local threshold:** T(x, y) = W(x, y) − `offset`, where W is the window average over `blockSize` × `blockSize` centred on the pixel.
  - `Mean` gives every pixel equal weight.
  - `Gaussian` uses separable weights exp(−d² / 2σ²), with σ = 0.3 · ((blockSize − 1) / 2 − 1) + 0.8, normalised to sum 1.
- **Exact tiling (UR-QA-2):** each output pixel is computed from its own window alone, in a fixed order: for each window row top to bottom, the weighted row sum left to right, then the column of those sums top to bottom, in `double`. No sliding or integral sums are carried from neighbouring pixels. So a pixel's result does not depend on its tile, and pixels beyond the level read as the framework supplies them (zero) under tiling and in one tile alike.
- **Comparison:** `Above` is v > T, `Below` is v < T, with v read through the codec.
- **Reference text:** `"Analytic: the threshold at each pixel is its window mean (or Gaussian-weighted mean) minus the offset, compared with a direct per-pixel computation"`.
- **Complexity text:** `"O(n * blockSize) in the number of samples"`.

- [ ] **Step 1: Write the failing tests** (`// Requirements: UR-PIX-5, UR-QA-2, UR-ALG-6`):
  - `ThresholdAdaptive.FactoryEqualsNamePath`; `ThresholdAdaptive.RejectsEvenOrSmallBlock`, with block sizes 2, 4 and 1027.
  - `ThresholdAdaptive.StepEdgeAnalytic`, on a `Byte` image of 0 left of x = 50 and 200 from there, with `Mean`, block 5 and offset 0:
    - x = 50 is foreground for `Above` (200 > 120);
    - x = 49 is background (0 < 80);
    - x = 10 is background (0 > 0 is false).
  - `ThresholdAdaptive.MatchesDirectComputation`: on a random `Float32` 97 × 83 image, every pixel equals the test's own naive double loop with zero padding, for `Gaussian` with block 7.
  - `ThresholdAdaptive.TiledEqualsSingleTile`: `Float32` and `UInt16` scenes, 300 × 260, block 15. Tiles {64, 100, 257} equal tile 4096, bit for bit, including the outer 7 pixels.
- [ ] **Step 2: Run them to verify they fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run.** Run `./run-tests.sh -R "algorithm\."`. Expected: all pass.
- [ ] **Step 5: Commit.** `feat(algorithm): adaptive threshold`

---

### Task 7: SDS fold, design amendment, class diagram, traceability

**Files:**
- Modify:
  - `docs/superpowers/specs/sds/06-algorithm.md`:
    - §6.1 registration per G4;
    - §6.3 per G2 and G7;
    - a new "Parameter bags" paragraph in §6.2 with G3 and every key of Tasks 1–6;
    - §6.5 thresholds per G8 and Task 5's bins;
    - §6.6 with Task 2's rule table;
    - §6.9 with the tolerances the tests assert.
  - `docs/superpowers/specs/2026-09-28-module-structure-design.md`: "OpenCV is confined to `pipeline`" and "the only module linking OpenCV" become "confined to the engine library", per G1, with its reason.
  - `docs/superpowers/specs/sds/05-pipeline.md` §5.11: `algorithm` has joined the engine.
  - `docs/superpowers/specs/sds/08-class-diagrams.md`: the algorithm diagram shows the classes and factories that now exist.
  - `docs/traceability.md`: rows for UR-PIX-1, UR-PIX-2, UR-PIX-5, UR-IMG-7, UR-IMG-13, UR-ALG-6 and UR-EXT-5, pointing to the tests.
  - `docs/project.md`: the algorithm plan A entry becomes "Implemented on branch `feat/algorithm-a`".
  - `README.md`: the Libraries section lists `algorithm` in the engine.

- [ ] **Step 1: Write the edits.** No ruling numbers or plan vocabulary may appear in the SDS or the headers.
- [ ] **Step 2: Verify.** Run `cmake --build build --target docs --config Release`. Expected: exit 0, 0 warnings. Then run `./run-tests.sh`. Expected: all pass.
- [ ] **Step 3: Commit.** `docs(algorithm): the catalog's scaffold and point operations in the design`
