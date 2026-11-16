# `pipeline` Module Implementation Plan A: the engine library and the pipeline's values

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Create `libslideio-processor-engine` and give it the parts of the `pipeline` module that describe work without running it: the OpenCV `Backend`, the `Operation` contract and its `OperationContext`, `OperationParameters`, the `OperationCatalog` with its support matrix, `PipelineSpec` with a real JSON round trip, `LevelSelector`/`TileGrid`, and `Reduction`/`Histogram64`.

**Architecture:**
- **This is plan A of three.** B adds `Pipeline` and execution (validation, block reads, the worker pool and tile executor, sinks, restrictions, auxiliary rasters, global context). C adds `SeamMerger`, the UR-SEG-3 suite and `Pipeline::asScene()`. A has no dependency on B or C; B and C consume A's interfaces exactly as its Interfaces blocks state.
- **A second shared library, per design 5.1.** `slideio-processor-engine` holds `pipeline` (later also `algorithm` and `workspace`). It links `slideio-processor-data` and SlideIO publicly, and OpenCV privately. The data library stays free of both, and the existing guard keeps proving it.
- **Values first.** Everything in this plan is a value or a pure function of a scene's metadata, so it is tested without running a pipeline. Scenes in tests are synthetic: a test `ISceneSource` described by a `SceneMetadata` and built with `slideio::makeScene` (SlideIO PR #86).
- **The whole-level operations proposal is adopted (user decision, binding).** `Operation` gains `tileRequirement()` and `workingSetBytesPerLevelPixel()` here; plan B enforces them in `Pipeline::validate()`. Task 8 amends SDS 05.

**Tech Stack:** C++17, CMake ≥ 3.21 (`ENVIRONMENT_MODIFICATION` needs 3.22; this machine has 4.4), Conan 2, GoogleTest, RapidJSON, OpenCV 4.14 (Conan, static, `core` and `imgproc` only), SlideIO 2.10+ from the `extern/slideio` submodule installed by `build.sh`.

**Spec:** `docs/superpowers/specs/sds/05-pipeline.md` (SDS 05) §5.1–§5.4, §5.6, §5.8, §5.10, read under `docs/superpowers/specs/sds/00-conventions.md` (SDS 00). The library split: `docs/superpowers/specs/2026-09-28-module-structure-design.md` §5.1–§5.2 and §6.5. The adopted proposal: `docs/superpowers/specs/2026-10-11-whole-level-operations.md` §3. Core types: `include/slideio/processor/core/` and SDS 01. `BackendCapabilities`: `include/slideio/processor/config/backend_capabilities.hpp`.

## Global Constraints

- C++17 in `namespace slideio::processor`; private helpers in `slideio::processor::detail`, under `src/pipeline/detail/`.
- Public headers under `include/slideio/processor/pipeline/`, exported with `SLIDEIO_PROCESSOR_ENGINE_EXPORTS` (P1).
- ABI rules C-2: pimpl for every non-trivial class, every definition out of line, no STL in signatures except `std::shared_ptr`, every `enum class` is `: int32_t`. `Halo` is a documented POD (C-2 exception).
- Strings C-4 (UTF-8 in; fill-caller-buffer out returns the length excluding the terminator, `nullptr` queries it). Collections C-5. Errors C-6: only `ProcessorError` subclasses and `std::bad_alloc` cross the boundary; OpenCV, RapidJSON and standard-library exceptions are translated, `what()` preserved as `cause()`.
- **No OpenCV type, header or macro in any public header** (SDS 05 §5.10, UR-BLD-4). `cv::Mat` lives only in `src/pipeline/`.
- **Signatures are SDS-exact.** Copy SDS 05 character for character. The only additions are those the rulings name.
- Doxygen on every public declaration with `@throws`, `@par Thread-safety` and `@par Requirements`, as the storage headers do. `cmake --build build --target docs --config Release` must exit 0 with zero warnings; check the exit code.
- A `// Requirements: …` line at the top of every test file.
- **Build on this machine:** `./build.sh` (installs SlideIO if needed), then `./run-tests.sh`; `-R <regex>` runs one test; `./build.sh --skip-slideio` while iterating. Conan is `C:\Users\Stanislav\anaconda3\envs\conan2\Scripts\conan.exe`.
- **Commits:** one per task, on branch `feat/pipeline-a`, conventional prefix (`build:`, `feat(pipeline):`, `test(pipeline):`, `docs(pipeline):`). Org policy: AI-generated code is human-reviewed before it reaches `main`.
- Never log pixel values, coordinates of objects or parameter values that could carry clinical data; log operation names, type names and counts.

## Rulings on spec gaps

Numbered P1 onward, so they never collide with storage's S-numbers. Each binds this plan and the next two. Task 8 folds them into SDS 05 and SDS 08.

| # | Gap | Ruling |
|---|---|---|
| P1 | The engine library and its export macro. | `slideio-processor-engine` is a SHARED library assembled from object libraries, as `slideio-processor-data` is: `src/pipeline/` builds `slideio-processor-pipeline` (OBJECT). New header `include/slideio/processor/pipeline/export.hpp` defines `SLIDEIO_PROCESSOR_ENGINE_EXPORTS`: `__declspec(dllexport)` when `SLIDEIO_PROCESSOR_ENGINE_API` is defined, else `dllimport`, on Windows; `visibility("default")` elsewhere. The engine target defines `SLIDEIO_PROCESSOR_ENGINE_API` PRIVATE. Same `-Wl,--exclude-libs,ALL` on Linux as the data library, so the static OpenCV inside never interposes. |
| P2 | How OpenCV and SlideIO are linked. | Engine links `slideio-processor-data` PUBLIC, `slideio::slideio` PUBLIC (public headers name `slideio::ScenePtr`), and `opencv_core` and `opencv_imgproc` PRIVATE. SlideIO's DLLs carry their own static OpenCV; the two copies never meet because SlideIO's public API is buffer-based (`void*` plus size), so no `cv::` object crosses. The existing data-library guard in `src/CMakeLists.txt` stays unchanged and green. A new check, **`pipeline_headers_are_opencv_free`**, compiles every header under `include/slideio/processor/pipeline/` in a target that has the SlideIO and data include paths but **no OpenCV include path**; it fails to build if any header reaches OpenCV. |
| P3 | Running tests that load SlideIO. | `tests/pipeline/` builds `slideio_processor_pipeline_tests`. On Windows each discovered test gets `ENVIRONMENT_MODIFICATION PATH=path_list_prepend:<SlideIO bin dir>`, where the bin dir is taken from the imported `slideio::slideio` target's `IMPORTED_LOCATION_RELEASE` directory. On Linux and macOS the imported target's RPATH suffices; if CI shows otherwise, set `BUILD_RPATH` to the same directory. `gtest_discover_tests` runs with `DISCOVERY_MODE PRE_TEST`, so discovery itself never needs the DLLs at build time. |
| P4 | `Backend` (SDS 05 §5.10). | `name()` is `"OpenCV"`. `version()` is `cv::getVersionString()`, cached in a static `std::string` at first call. Depth mapping: `Byte→CV_8U`, `Int8→CV_8S`, `UInt16→CV_16U`, `Int16→CV_16S`, `Int32→CV_32S`, `Float16→CV_16F`, `Float32→CV_32F`, `Float64→CV_64F`. `UInt32`, `UInt64` and `Int64` are supported only when `CV_VERSION_MAJOR >= 5` (`CV_32U`, `CV_64U`, `CV_64S`), behind `#if`. `supportedTypes()` is exactly the mapped set, and **must equal `BackendCapabilities::instance().supportedTypes()`**, which `config` derives without linking OpenCV; a test asserts it. `toBackendDepth` of an unsupported type throws `UnsupportedDataTypeError` naming the type and `"OpenCV"`. `fromBackendDepth` of an unknown depth returns `DataType::Unknown`. |
| P5 | Whole-level operations (adopted, user decision). | In `operation.hpp`: `enum class TileRequirement : int32_t { Halo, WholeLevel };`, and on `Operation`: `virtual TileRequirement tileRequirement() const;` (default `Halo`) and `virtual int64_t workingSetBytesPerLevelPixel() const;` (default 0). `OperationFactory` gains the same two, non-pure, same defaults, so the catalog can answer before constructing. `OperationCatalog` gains `TileRequirement tileRequirement(const char* name) const;` and `int64_t workingSetBytesPerLevelPixel(const char* name) const;`. Enforcement is plan B's (P13). **Two further declarations the executor needs and SDS 05 lacks**, added on `Operation` (and mirrored on `OperationFactory` and as catalog queries `producesObjects(name)` / `outputPlaneCount(name, n)`): `virtual bool producesObjects() const;` (default `false`; true means the operation emits objects and therefore has a target layer that `validate()` resolves, SDS 05 §5.5) and `virtual int32_t outputPlaneCount(int32_t inputPlaneCount) const;` (default `inputPlaneCount`; consulted only for `Joint` operations, since a `PerChannel` operation is applied one plane at a time and yields one plane per input plane). The `targetLayer` parameter key is `"targetLayer"` (string). |
| P6 | `OperationContext` construction. | The class is exactly SDS 05 §5.1, pimpl'd, not copyable. Only the framework builds one: `detail::OperationContextState` (all fields of the context as plain members, pointers borrowed) and `detail::makeOperationContext(OperationContextState) -> std::unique_ptr<OperationContext>`, both in `src/pipeline/detail/operation_context_state.hpp`. `auxiliary(name)` looks up a `std::vector<std::pair<std::string, const BlockView*>>` in the state; null name or no match returns null. **SDS 05 §5.1's `emitSeamFragment(const Polygon&, LayerId)` cannot carry the intensity partials that §5.7's `addFragment` and SDS 06 §6.8 say a fragment brings**, so `OperationContext` gains the overload `void emitSeamFragment(const Polygon&, LayerId, const IntensityPartial* partials, int32_t partialCount);` (the two-argument form passes none), and the `IntensityPartial` POD moves from `seam_merger.hpp` to `operation.hpp` unchanged. Both forms call a `std::function<void(const Polygon&, LayerId, const IntensityPartial*, int32_t)>` in the state, and throw `InternalError("emitSeamFragment: this run has no seam merger")` when it is empty (plan C supplies it). `partialCount < 0`, or `partials == nullptr` with `partialCount > 0`, throws `InvalidArgumentError`. `toLevel0` uses P9's single mapping with the state's `levelScale`. |
| P6a | One formula for object measurements. | UR-SEG-3 needs an object finished inside one tile and the same object merged from fragments (plan C) to carry identical measurements, and `algorithm` compiles against public headers only, so the formula is public. New header `include/slideio/processor/pipeline/standard_measurements.hpp`: `SLIDEIO_PROCESSOR_ENGINE_EXPORTS void applyStandardMeasurements(const Polygon& level0Polygon, const IntensityPartial* partials, int32_t partialCount, MeasurementSet& out);` It sets `area` = `Polygon::area()` and `perimeter` = `Polygon::perimeter()` (units `MeasurementUnit::SquarePixels` and `MeasurementUnit::Pixels`, level-0 pixels), and for each partial with `count > 0`, `intensity.<channel>.mean` = sum/count, `intensity.<channel>.stddev` = `sqrt(max(0, sumOfSquares/count − mean²))` (population), `intensity.<channel>.min`, `intensity.<channel>.max` (unit `MeasurementUnit::Intensity`). Calibrated units are `algorithm`'s business (SDS 06 §6.8). Partials of the same channel must already be combined; a repeated channel throws `InvalidArgumentError`. `detail::combinePartials(a, b)` (count and sums added, min/max taken) lives beside it for plan C. |
| P7 | `OperationParameters` semantics. | Keys are UTF-8, non-null and non-empty, else `InvalidArgumentError` naming the setter. Stored ordered by key (bytewise), so `keyAt(i)` is ascending and two bags with the same entries are equal regardless of the order they were set in. Setting an existing key **replaces** it, type included. `setDouble` and `setDoubleArray` reject non-finite values with `InvalidArgumentError` naming the key (JSON cannot hold them, and a recorded stage must replay). `setDoubleArray` accepts `count == 0` (an empty array) and rejects `values == nullptr` with `count > 0`. Getters never coerce: `getInt` on a double-typed key returns false. `getString` returns `fallback` when the key is absent or not a string; its pointer is valid until the next non-const call. `getDoubleArray` follows C-5 (returns the stored count; fills up to `capacity`; `nullptr` queries); an absent or differently typed key returns 0. `keyAt` out of range throws `InvalidArgumentError`. Assignment operator and move members are added (C-2 full copy/move set). **JSON:** one object, keys in stored order; int → JSON integer; double → JSON number always written with a fraction or exponent (RapidJSON `Writer::Double`, shortest round-trip), so the type survives a round trip; bool → `true/false`; string → string; double array → array of such numbers. The reader is `detail::parametersFromJson(const rapidjson::Value&, const char* where)`, used by `PipelineSpec::fromJson`; a JSON integer reads as int, a number with a fraction or exponent as double, an array as a double array (every element must be a number). `detail::parametersEqual(a, b)`: same keys, same types, same values (`==` on doubles, element-wise on arrays). |
| P8 | `OperationCatalog` semantics. | `instance()` is a function-local static. `registerFactory` throws `InvalidArgumentError` for a null factory, a null or empty `name()`, a null or empty `correctnessReference()` (UR-ALG-6 makes it mandatory), a null `complexityNote()`, or a name already registered (the message names it). Names are kept in ascending bytewise order: `nameAt(i)` is deterministic regardless of registration order. Every by-name query and `create` throws `InvalidArgumentError` naming an unknown or null name. `create` also throws `InternalError` if the factory returns null, or if the created operation's `name()`, `arity()`, `requiresUniformTypes()`, `supportedTypes()`, `halo()` or `tileRequirement()` differs from the factory's (the message names the field). **Support matrix:** CSV, `\n` line endings, UTF-8, header `operation,Byte,Int8,UInt16,Int16,UInt32,Int32,UInt64,Int64,Float16,Float32,Float64`, one row per operation in `nameAt` order, each cell `yes` or `no`. A cell is `yes` exactly when `detail::operationAccepts(factory supportedTypes, type)` is true; that function is `supportedTypes.contains(type) && Backend::instance().supports(type)`, and plan B's `validate()` calls the same function. The return value is the bytes written. The path is UTF-8 through `std::filesystem::u8path`; open or write failure throws `IoError` naming it. |
| P9 | The single level mapping. | `detail::levelToLevel0(int64_t v, double scale) -> int64_t` is `llround(v * scale)`, and it is the only implementation (SDS 05 §5.6). `LevelSelector::mapToLevel0`, `OperationContext::toLevel0` and `TileGrid::tileRect` all call it. **A rectangle maps by its edges:** `x0 = map(x)`, `x1 = map(x + width)`, result width `x1 − x0`; likewise y. So two tiles that share a level edge share the level-0 edge exactly. `mapToLevel` (level-0 to level, outward): `x0 = floor(x / s)`, `x1 = ceil((x + width) / s)`, computed in `long double` and checked to fit `int64_t`. |
| P10 | `LevelSelector` semantics. | Built from a non-null `slideio::ScenePtr` (null → `InvalidArgumentError`). Level count is `scene->getNumZoomLevels()`, or 1 when that is 0, in which case level 0 is the scene rect. `scaleOfLevel(L) = double(levelWidth(0)) / double(levelWidth(L))`, from `LevelInfo::getSize()`; one factor per level, the one `RasterMask` carries. **Two scale vocabularies, documented in the header:** `scaleOfLevel` returns the level-to-level-0 factor (≥ 1, e.g. 4.0001), while `levelForScale(scale)` takes a resolution fraction (0 < scale ≤ 1, e.g. 1/16) and returns the coarsest level `L` with `1 / scaleOfLevel(L) >= scale * (1 − 1e-9)`, or 0 when none coarser qualifies. `levelForSize(w, h)` returns the coarsest level whose width ≥ `w` and height ≥ `h`, or 0. Out-of-range level → `InvalidArgumentError`; non-positive or non-finite scale or sizes → `InvalidArgumentError`. |
| P11 | `TileGrid` semantics. | `tileWidth`, `tileHeight` ≥ 1 and `level` in range, else `InvalidArgumentError`. An empty region gives `tileCount() == 0`. Tiles are row-major from the region's top-left: `tileIndex(c, r) = r * columns() + c`. The last column and row are clipped to the region. `tileRectAtLevelWithHalo` is the tile rect grown by the halo and **not** clipped: halo beyond the level reads as zeros (plan B's block read), and halo beyond the region but inside the level reads real pixels, which is what UR-IMG-5's "matches whole slide then crop" needs. `tileRect` and `tileRectWithHalo` map through P9. `isEdgeTile` is true for the first or last column or row. Out-of-range index or column/row → `InvalidArgumentError`. Tile counts, rects and indices are `int64_t` throughout (UR-IMG-1). |
| P12 | `defaultTileSize`. | When `config.tileSizeFromSlide()` and the level's `LevelInfo::getTileSize()` has both dimensions > 0, that size; otherwise `config.tileWidth()` × `config.tileHeight()`. Level out of range → `InvalidArgumentError`. |
| P14 | `Reduction` and `Histogram64`. | Both pimpl'd and exported. `Histogram64(binCount, minValue, maxValue)` requires `binCount ≥ 1`, finite bounds and `minValue < maxValue`, else `InvalidArgumentError`. `accumulate(view, tileIndex)` reads every plane of the view, each sample as `double` by its plane's `DataType` (Float16 through a new `detail::halfToDouble` in `src/pipeline/detail/half.hpp/.cpp`; `core` has no public one and the engine must not reach into `storage`'s detail), and bins `v` into `floor((v − min) / (max − min) * binCount)`; `v == max` goes in the last bin; `v` outside `[min, max]` and NaN are ignored and not counted. `tileIndex` is unused by integer reductions. `accumulate` is ThreadSafe (bins are `std::atomic<int64_t>`, relaxed). `combine(other)` requires `other` to be a `Histogram64` with the same three parameters, else `InvalidArgumentError`; it adds bins. `bin(i)` out of range → `InvalidArgumentError`. `totalCount()` is the sum of the bins. Canonical-order combination for order-dependent reductions is plan B's (P15). |

## Review Focus

1. **A pyramid whose level 1 is 2000×1500 under a 8001×6001 level 0** (factor 4.0005): adjacent tiles' level-0 rects must share edges exactly, and the union of all tile rects must have no gap and no overlap. Pinned in Task 6.
2. **A recorded spec replayed in another process:** `fromJson(toJson(spec))` must `equals` the original for every parameter type, including a double like `0.1`, an integer near `INT64_MAX`, an empty array, a UTF-8 key and a polygon with a hole. Pinned in Task 5.
3. **A third-party operation built only from public headers** must compile, register and round-trip through the catalog (UR-EXT-2). Pinned in Task 3 with a test translation unit whose include path has no `src/`.
4. **An unsupported type on the installed OpenCV** (`UInt32` on OpenCV 4) must show `no` in the support matrix and fail `toBackendDepth` with both names, and the matrix must agree with `BackendCapabilities`. Pinned in Tasks 1 and 4.
5. **A histogram fed from 8 threads with a mixed Byte/Float32 view:** every bin must equal the single-threaded result. Pinned in Task 7.

---

### Task 1: The engine library, `Backend`, and the test harness

**Files:**
- Create: `include/slideio/processor/pipeline/export.hpp`, `include/slideio/processor/pipeline/backend.hpp`, `src/pipeline/CMakeLists.txt`, `src/pipeline/backend.cpp`, `tests/pipeline/CMakeLists.txt`, `tests/pipeline/test_backend.cpp`, `tests/pipeline/header_check/` (one `.cpp` per public header, P2).
- Modify: `src/CMakeLists.txt` (add `pipeline`, the engine library), `tests/CMakeLists.txt`.

**Interfaces:**
- Produces: target `slideio-processor-engine`; macro `SLIDEIO_PROCESSOR_ENGINE_EXPORTS`; `Backend` exactly as SDS 05 §5.10 with P4's behaviour; test target `slideio_processor_pipeline_tests` and the check target `pipeline_headers_are_opencv_free` (built by `ALL`, so the normal build fails when a header leaks OpenCV).

- [ ] **Step 1: Write the failing test** `tests/pipeline/test_backend.cpp` (`// Requirements: UR-BLD-3, UR-BLD-4, UR-IMG-11, UR-IMG-12`).

```cpp
TEST(Backend, NameAndVersion) {
    EXPECT_STREQ(Backend::instance().name(), "OpenCV");
    EXPECT_STREQ(Backend::instance().version(), BackendCapabilities::instance().backendVersion());
}
TEST(Backend, AgreesWithBackendCapabilities) {
    EXPECT_TRUE(Backend::instance().supportedTypes() == BackendCapabilities::instance().supportedTypes());
}
TEST(Backend, DepthRoundTripForEverySupportedType) {
    // for each t in supportedTypes(): fromBackendDepth(toBackendDepth(t)) == t
}
TEST(Backend, UnsupportedTypeNamesTypeAndBackend) {
    // OpenCV 4 only (skip when major >= 5): toBackendDepth(DataType::UInt32) throws
    // UnsupportedDataTypeError whose what() contains "UInt32" and "OpenCV"
}
TEST(Backend, UnknownDepthIsUnknown) { EXPECT_EQ(Backend::instance().fromBackendDepth(9999), DataType::Unknown); }
```

- [ ] **Step 2: Run it and see it fail to build** (`./build.sh --skip-slideio`).
- [ ] **Step 3: Implement** P1–P4: the library, the export header, `Backend`, the header-check target, the test target with P3's PATH handling. Verify the header check fires by temporarily adding `#include <opencv2/core.hpp>` to `backend.hpp`: the build must fail; then revert.
- [ ] **Step 4: Run** `./build.sh --skip-slideio` then `./run-tests.sh`. Expect all PASS, including the existing 422 tests, and the data-library guard still green.
- [ ] **Step 5: Commit.** `build: add the engine library with the OpenCV backend adapter`

### Task 2: `OperationParameters`

**Files:**
- Create: `include/slideio/processor/pipeline/operation_parameters.hpp`, `src/pipeline/operation_parameters.cpp`, `src/pipeline/detail/parameters_json.hpp/.cpp`, `tests/pipeline/test_operation_parameters.cpp`.

**Interfaces:**
- Produces: `OperationParameters` exactly as SDS 05 §5.2, plus `operator=` and the move pair (P7); `detail::parametersToJson(const OperationParameters&, rapidjson::Writer<rapidjson::StringBuffer>&)`, `detail::parametersFromJson(const rapidjson::Value&, const char* where) -> OperationParameters` (throws `InvalidArgumentError` naming `where` and the key), `detail::parametersEqual(const OperationParameters&, const OperationParameters&) -> bool`.

- [ ] **Step 1: Write the failing test** (`// Requirements: UR-STG-4, UR-RES-11`).

```cpp
TEST(OperationParameters, TypedGettersNeverCoerce) {
    OperationParameters p; p.setInt("n", 3).setDouble("s", 1.5);
    int64_t i = 0; double d = 0;
    EXPECT_TRUE(p.getInt("n", i)); EXPECT_EQ(i, 3);
    EXPECT_FALSE(p.getInt("s", i)); EXPECT_FALSE(p.getDouble("n", d));
}
TEST(OperationParameters, KeysAreOrderedAndReplaceInPlace) {
    // set "b", "a", "c", then setString("a","x"): count()==3, keyAt(0..2)=="a","b","c", getString("a")=="x"
}
TEST(OperationParameters, RejectsNonFiniteAndBadKeys) {
    // setDouble("x", NAN), setDouble("x", INFINITY), setDoubleArray("x", {1, NAN}, 2), setInt(nullptr, 1),
    // setInt("", 1), setDoubleArray("x", nullptr, 2): each throws InvalidArgumentError
}
TEST(OperationParameters, DoubleArrayFollowsC5) {
    // set {1,2,3}: getDoubleArray(k, nullptr, 0)==3; with capacity 2 fills 2 and returns 3; empty array returns 0
}
TEST(OperationParameters, JsonRoundTripPreservesTypes) {
    // ints 0, -1, INT64_MAX, INT64_MIN; doubles 0.1, 1.0, -0.0, 1e300; bool; UTF-8 string "µm ✓";
    // empty array; key "ключ". toJson -> parse with RapidJSON -> detail::parametersFromJson ->
    // detail::parametersEqual(original, back) is true; getDouble("one") on 1.0 still a double
}
TEST(OperationParameters, ToJsonFollowsC4) {
    // toJson(nullptr,0) returns n; buffer of n+1 holds the full text; buffer of 5 holds 4 chars + NUL
}
```

- [ ] **Step 2: Run it; see it fail to build.**
- [ ] **Step 3: Implement** per P7. Storage: `std::map<std::string, Value>` where `Value` is a `std::variant` of the five types, inside the pimpl.
- [ ] **Step 4: Run** the suite; all PASS.
- [ ] **Step 5: Commit.** `feat(pipeline): OperationParameters with a typed JSON form`

### Task 3: The operation contract: `Halo`, `ChannelArity`, `TileRequirement`, `OperationContext`, `Operation`

**Files:**
- Create: `include/slideio/processor/pipeline/operation.hpp`, `include/slideio/processor/pipeline/standard_measurements.hpp`, `src/pipeline/standard_measurements.cpp`, `src/pipeline/operation.cpp`, `src/pipeline/operation_context.cpp`, `src/pipeline/detail/operation_context_state.hpp`, `src/pipeline/detail/level_mapping.hpp/.cpp` (P9's function), `tests/pipeline/test_operation.cpp`, `tests/pipeline/public_only/third_party_operation.cpp` (+ `.hpp`), `tests/pipeline/test_support/` (shared test helpers: `synthetic_scene.hpp/.cpp`, `test_operations.hpp/.cpp`).

**Interfaces:**
- Consumes: `OperationParameters` (Task 2).
- Produces:
  - Everything in SDS 05 §5.1, plus P5's `TileRequirement`, `tileRequirement()`, `workingSetBytesPerLevelPixel()`.
  - `Halo::uniform(n)` requires `n ≥ 0` (`InvalidArgumentError` otherwise; documented `@throws`, so not `noexcept` — **ruling P16**: SDS says `noexcept`; the SDS is amended to drop it, because a negative halo must be refused, not silently clamped). `Halo::none()` is all zeros. `isEmpty()` is all four zero.
  - `detail::OperationContextState` with members: `const BlockView* input; BlockView* output; ObjectSink* objectSink; EdgeSink* edgeSink; AnnotationSink* annotationSink; std::vector<std::pair<std::string, const BlockView*>> auxiliaries; LayerId targetLayer; int32_t level; Rect64 tileRectAtLevel, haloRectAtLevel, tileRect, haloRect; double levelScale; const ChannelSelection* channels; const CancellationToken* cancellation; const RasterMask* mask; std::function<void(const Polygon&, LayerId, const IntensityPartial*, int32_t)> seamFragment;` and `detail::makeOperationContext(OperationContextState) -> std::unique_ptr<OperationContext>`.
  - `detail::levelToLevel0(int64_t, double) -> int64_t` and `detail::rectToLevel0(const Rect64&, double) -> Rect64` (edge mapping, P9).
  - Test support: `testing_support::SyntheticSource` (an `slideio::ISceneSource`) built from a list of level sizes, native tile sizes and channel `slideio::DataType`s, with pixel value `f(channel, levelX, levelY)` supplied as a `std::function<double(int, int64_t, int64_t)>`; `makeSyntheticScene(...) -> slideio::ScenePtr` via `slideio::makeScene`. It implements `readLevelBlock` (returns true) and `readBlock`, writes interleaved samples as SlideIO's buffer layout requires, background zero outside the level, and counts reads in an atomic (`readCount()`), which plan B uses to prove "no pixels read".
  - Test support, all public-header-only and each with a factory: `testing_support::ThresholdOperation` (PerChannel, Byte and Float32, halo 0, output Byte: 255 where the sample ≥ parameter `level`, else 0); `testing_support::BoxMeanOperation` (PerChannel, halo `r` from parameter `radius`, output Float32, the mean over the (2r+1)² window); `testing_support::ComponentsOperation` (Joint, Byte only, one input plane, halo 0, `producesObjects() == true`, raster output a copy of the input): emits one `ObjectRecord` per 4-connected non-zero component as a polygon of pixel-edge rings in level-0 via `toLevel0`, each ring with collinear vertices removed and starting at its topmost-then-leftmost vertex (`testing_support::normaliseRing`, which plan C's merger output is compared through), measured with `applyStandardMeasurements` from one `IntensityPartial` of the component's own sample values (channel = the plane's source channel); when its bool parameter `seams` is true, components touching the tile edge go to `emitSeamFragment` with that partial instead (default false, so it runs before plan C's merger exists). Plan B uses it for emission and restrictions; plan C for seams. It is written once here, with a plain flood fill (no OpenCV), so that plan C's tile-size invariance is a test of the framework, not of a library call.

- [ ] **Step 1: Write the failing tests** (`// Requirements: UR-EXT-1, UR-EXT-2, UR-EXT-3, UR-ALG-3, UR-ALG-4, UR-IMG-11`).

```cpp
TEST(Halo, UniformNoneAndEmpty) { /* uniform(3) all 3; none().isEmpty(); uniform(-1) throws InvalidArgumentError */ }
TEST(Operation, DefaultsForGlobalContextAndLayout) {
    // ThresholdOperation: needsGlobalContext()==false, tileRequirement()==TileRequirement::Halo,
    // workingSetBytesPerLevelPixel()==0, producesObjects()==false, outputPlaneCount(3)==3;
    // accumulate/combine/finalizeAccumulation callable and do nothing
}
TEST(OperationContext, AccessorsReflectState) {
    // build a state with level 2, levelScale 4.0005, tileRectAtLevel {10,20,30,40}, a mask, an aux "coarse":
    // every accessor returns what was set; auxiliary("coarse") non-null; auxiliary("x") and auxiliary(nullptr) null
}
TEST(OperationContext, ToLevel0IsTheSingleMapping) {
    // toLevel0(Point64{7, 9}) == {llround(7*4.0005), llround(9*4.0005)};
    // polygon mapped vertex by vertex equals detail::levelToLevel0 per vertex
}
TEST(OperationContext, SeamFragmentWithoutMergerIsInternalError) { /* empty seamFragment -> InternalError */ }
TEST(OperationContext, SeamFragmentReachesCallback) { /* both overloads: callback receives polygon, layer and the partials (none for the short form); negative count throws */ }
TEST(StandardMeasurements, GeometryAndIntensity) {
    // unit square scaled 10: area 100, perimeter 40; partial {count 4, sum 10, sumSq 30, min 1, max 4, channel 2}:
    // intensity.2.mean 2.5, stddev sqrt(30/4 - 6.25), min 1, max 4; a second partial for channel 2 throws
}
TEST(PublicOnly, ThirdPartyOperationBuildsFromPublicHeaders) {
    // third_party_operation.cpp is compiled in its own OBJECT library whose include path is only
    // include/ and SlideIO's; this test constructs it and checks its declarations (UR-EXT-2)
}
```

- [ ] **Step 2: Run it; see it fail to build.**
- [ ] **Step 3: Implement** the header per SDS 05 §5.1 and P5, P6, P9, P16; the test support; the public-only OBJECT library in `tests/pipeline/CMakeLists.txt`.
- [ ] **Step 4: Run** the suite; all PASS.
- [ ] **Step 5: Commit.** `feat(pipeline): the operation contract and its context`

### Task 4: `OperationFactory`, `OperationCatalog` and the support matrix

**Files:**
- Create: `include/slideio/processor/pipeline/operation_catalog.hpp`, `src/pipeline/operation_catalog.cpp`, `src/pipeline/detail/type_contract.hpp/.cpp` (`operationAccepts`), `tests/pipeline/test_operation_catalog.cpp`, `tests/pipeline/test_support/operation_conformance.hpp/.cpp`.

**Interfaces:**
- Consumes: Tasks 1–3.
- Produces: SDS 05 §5.3 exactly, plus P5's two factory virtuals and two catalog queries; `detail::operationAccepts(DataTypeSet, DataType) -> bool` (P8); test helper `testing_support::expectConformant(const char* name)`, which creates the operation from a default `OperationParameters`, checks every declaration against the catalog's, and round-trips `create(name(), parameters())` comparing declarations and `detail::parametersEqual` — the conformance test SDS 05 §5.1 calls for, reusable by `algorithm`.
- Test isolation: the catalog is a process singleton, so tests register under names prefixed `test.` and unique per test (`test.<TestName>.threshold`).

- [ ] **Step 1: Write the failing tests** (`// Requirements: UR-IMG-12, UR-ALG-4, UR-ALG-6, UR-ALG-8, UR-EXT-5`).

```cpp
TEST(OperationCatalog, RegisterQueryCreate) { /* register ThresholdFactory as test.X.threshold; isRegistered; arity, halo, supportedTypes, correctnessReference, complexityNote, tileRequirement, workingSet match the factory; create returns non-null */ }
TEST(OperationCatalog, RejectsBadFactories) { /* null, empty name, empty correctnessReference, null complexityNote, duplicate name: each InvalidArgumentError; message names the duplicate */ }
TEST(OperationCatalog, UnknownNameIsNamed) { /* create("test.nope", p) throws InvalidArgumentError containing "test.nope" */ }
TEST(OperationCatalog, NamesAreSorted) { /* register "test.Y.b" then "test.Y.a": the index of a < index of b */ }
TEST(OperationCatalog, CreatedOperationMustMatchItsFactory) { /* a factory whose op reports a different halo -> InternalError naming "halo" */ }
TEST(OperationCatalog, SupportMatrixMatchesTheRuntimeContract) {
    // writeSupportMatrix(tmp): parse the CSV; header exactly as P8; for every row and type,
    // cell == "yes" iff detail::operationAccepts(catalog.supportedTypes(row), type)
    // on OpenCV 4 the UInt32 column is "no" for every operation
}
TEST(OperationCatalog, ConformanceHelperPassesForTestOperations) { /* expectConformant for both test operations */ }
TEST(OperationCatalog, ThreadSafeQueries) { /* 8 threads query and create while one registers new names; no crash, all creates succeed */ }
```

- [ ] **Step 2: Run it; see it fail to build.**
- [ ] **Step 3: Implement** per SDS 05 §5.3, P5 and P8 (a `std::shared_mutex` guards the map).
- [ ] **Step 4: Run** the suite; all PASS.
- [ ] **Step 5: Commit.** `feat(pipeline): the operation catalog and its generated support matrix`

### Task 5: `PipelineSpec`

**Files:**
- Create: `include/slideio/processor/pipeline/pipeline_spec.hpp`, `src/pipeline/pipeline_spec.cpp`, `tests/pipeline/test_pipeline_spec.cpp`.

**Interfaces:**
- Consumes: `OperationParameters` and its detail JSON functions (Task 2), `Operation::parameters()` (Task 3).
- Produces: SDS 05 §5.4 exactly, plus the move pair (C-2), and **ruling P17**:
  - `restrictTo` with an empty rect, and `restrictToPolygon` with an empty polygon, throw `InvalidArgumentError`. `analysisLevel` below −1 throws. `operation` with a null or empty name throws; an unknown name does not (SDS: it fails at `validate()`). `then(nullptr)` throws. `operationName`/`operationParameters` out of range throw.
  - `roi()` and `polygon()` without a restriction return an empty rect and an empty polygon.
  - **JSON schema v1:** `{"specVersion":1,"channels":"all"|[c,…],"analysisLevel":L,"roi":[x,y,w,h] (present only when set),"polygon":[{"hole":bool,"points":[[x,y],…]},…] (present only when set),"operations":[{"name":"…","parameters":{…}},…]}`. Coordinates are JSON integers. `fromJson` throws `InvalidArgumentError` naming the offending member for a null text, a parse error (with RapidJSON's offset), a missing or wrong-typed member, or `specVersion` other than 1.
  - `equals` compares channels (all vs explicit list, element-wise), level, ROI presence and value, polygon presence and ring-by-ring point equality including hole flags, and operations by name and `detail::parametersEqual`.

- [ ] **Step 1: Write the failing tests** (`// Requirements: UR-STG-4, UR-RES-11, UR-EXE-1, UR-IMG-5`).

```cpp
TEST(PipelineSpec, NamedAndConstructedOperationsBuildTheSameSpec) {
    // spec a: operation("test.S.threshold", params); spec b: then(catalog.create("test.S.threshold", params)); a.equals(b)
}
TEST(PipelineSpec, RestrictionsReplaceRatherThanAccumulate) { /* restrictTo twice: roi() is the second; same for polygon */ }
TEST(PipelineSpec, JsonRoundTripIsExact) {
    // channels {2,0}, level 3, roi, polygon with an outer ring and a hole, two operations with every
    // parameter type: fromJson(toJson()).equals(original); and toJson of the copy equals toJson of the original
}
TEST(PipelineSpec, EqualsIgnoresParameterSetOrder) { /* params set in different orders compare equal */ }
TEST(PipelineSpec, EqualsSeesEveryField) { /* changing any one field (level, a channel, roi, a hole flag, a parameter value) breaks equals */ }
TEST(PipelineSpec, FromJsonNamesTheBadMember) {
    // "{" -> InvalidArgumentError with an offset; missing "operations"; "analysisLevel":"x"; "specVersion":2
}
TEST(PipelineSpec, Validation) { /* empty roi, empty polygon, level -2, then(nullptr), operation("") each throw */ }
```

- [ ] **Step 2: Run it; see it fail to build.**
- [ ] **Step 3: Implement** per SDS 05 §5.4 and P17.
- [ ] **Step 4: Run** the suite; all PASS.
- [ ] **Step 5: Commit.** `feat(pipeline): PipelineSpec with a replayable JSON record`

### Task 6: `LevelSelector`, `TileGrid`, `defaultTileSize`

**Files:**
- Create: `include/slideio/processor/pipeline/tile_grid.hpp`, `src/pipeline/level_selector.cpp`, `src/pipeline/tile_grid.cpp`, `tests/pipeline/test_tile_grid.cpp`.

**Interfaces:**
- Consumes: P9's mapping (Task 3), the synthetic scene (Task 3).
- Produces: SDS 05 §5.6 exactly with P9–P12. Plan B uses `LevelSelector(scene)`, `TileGrid(regionAtLevel, level, w, h, selector)`, `tileRectAtLevelWithHalo`, `tileRect`, `defaultTileSize`.

- [ ] **Step 1: Write the failing tests** (`// Requirements: UR-IMG-4, UR-EXE-6, UR-EXT-3, UR-SEG-3, UR-QA-2, UR-QA-3, UR-IMG-5, UR-IMG-1`).

```cpp
TEST(LevelSelector, ScaleOfANonDyadicPyramid) {
    // levels 8001x6001, 2000x1500, 500x375: scaleOfLevel(1) == 8001.0/2000, scaleOfLevel(0) == 1
}
TEST(LevelSelector, LevelForScaleNeverDecodesLevelZeroForACoarseRequest) {
    // dyadic 4096/1024/256: levelForScale(1.0/16) == 2; levelForScale(0.25) == 1; levelForScale(0.3) == 0; levelForScale(1) == 0
}
TEST(LevelSelector, LevelForSize) { /* levelForSize(256,256)==2; (300,10)==1; (5000,1)==0 */ }
TEST(LevelSelector, MapToLevelRoundsOutward) {
    // scale 4.0005: mapToLevel({1,1,4,4}, 1) covers level-0 pixels 1..4 -> x0 = 0, x1 = ceil(5/4.0005) = 2
}
TEST(LevelSelector, SceneWithoutLevelsHasOne) { /* metadata with no LevelInfo: one level, scaleOfLevel(0)==1 */ }
TEST(TileGrid, AdjacentTilesShareLevel0EdgesExactly) {
    // the 4.0005 pyramid, level 1, region whole level, tiles 300x200: for every tile, right edge of tile(c,r)
    // == left edge of tile(c+1,r) in level-0, likewise vertically; sum of tile areas == area of mapped region
}
TEST(TileGrid, EdgeTilesAreClipped) { /* region 1000x700, tiles 512: columns 2, rows 2, tileRectAtLevel(3) == {512,512,488,188} */ }
TEST(TileGrid, HaloIsNotClipped) { /* tile 0 with Halo::uniform(5): rect {-5,-5,522,522} at level */ }
TEST(TileGrid, Int64Scale) { /* region 4e6 x 3e6 at tiles 512: tileCount() == 7813 * 5860, last index maps without overflow */ }
TEST(TileGrid, Validation) { /* tile size 0, bad level, out-of-range index each InvalidArgumentError; empty region -> 0 tiles */ }
TEST(DefaultTileSize, NativeOrConfigured) { /* native 256x256 level with tileSizeFromSlide -> 256; flag off -> config size; native 0 -> config size */ }
```

- [ ] **Step 2: Run it; see it fail to build.**
- [ ] **Step 3: Implement** per SDS 05 §5.6 and P9–P12.
- [ ] **Step 4: Run** the suite; all PASS.
- [ ] **Step 5: Commit.** `feat(pipeline): level selection and a grid laid out at the analysis level`

### Task 7: `Reduction` and `Histogram64`

**Files:**
- Create: `include/slideio/processor/pipeline/reduction.hpp`, `src/pipeline/reduction.cpp`, `tests/pipeline/test_reduction.cpp`.

**Interfaces:**
- Produces: SDS 05 §5.8 exactly with P14. Plan B's global-context pass and `algorithm`'s streamed Otsu consume `Histogram64`.

- [ ] **Step 1: Write the failing tests** (`// Requirements: UR-PIX-5, UR-QA-3, UR-IMG-2`).

```cpp
TEST(Histogram64, BinsAndEdges) {
    // Histogram64 h(4, 0, 4); a 1x6 Float32 view {0, 0.99, 1, 3.5, 4, 5}: bins {2,1,0,2}; totalCount()==5 (5 ignored)
}
TEST(Histogram64, IgnoresNaN) { /* NaN not counted */ }
TEST(Histogram64, ReadsEveryPlaneByItsType) { /* view with a Byte plane and a Float32 plane: both binned */ }
TEST(Histogram64, ConcurrentAccumulateEqualsSequential) {
    // 8 threads each accumulate 64 distinct tiles of a mixed view; bins equal a single-threaded run
}
TEST(Histogram64, CombineAddsAndChecksShape) { /* combine equal-shaped adds; different binCount/min/max throws InvalidArgumentError */ }
TEST(Histogram64, IsOrderIndependent) { EXPECT_TRUE(Histogram64(2, 0, 1).isOrderIndependent()); }
TEST(Histogram64, Validation) { /* binCount 0, min >= max, NaN bound, bin(-1), bin(binCount) each throw */ }
```

- [ ] **Step 2: Run it; see it fail to build.**
- [ ] **Step 3: Implement** per P14.
- [ ] **Step 4: Run** the suite; all PASS.
- [ ] **Step 5: Commit.** `feat(pipeline): reductions with an order-independent histogram`

### Task 8: SDS fold, class diagram, traceability

**Files:**
- Modify: `docs/superpowers/specs/sds/05-pipeline.md`, `docs/superpowers/specs/sds/08-class-diagrams.md` (D.6), `docs/traceability.md` (a `pipeline` section, same format as the others), `docs/superpowers/specs/2026-10-11-whole-level-operations.md` (status: adopted, with the plan reference), `docs/project.md` (link this plan).

- [ ] **Step 1:** Fold P1–P17 into SDS 05 at the sections they amend, as the storage plans did (no "ruling" vocabulary in the SDS; state the behaviour). In particular: §5.1 gains `TileRequirement` and the two virtuals, and `Halo::uniform` loses `noexcept`; §5.3 gains the two queries and the factory defaults; §5.10 the depth table; a new short §5.11 records the engine library, its export macro and the OpenCV/SlideIO link rules.
- [ ] **Step 2:** Update SDS 08 D.6 with the new members and `TileRequirement`.
- [ ] **Step 3:** Add the `pipeline` rows to `docs/traceability.md` from each test file's `// Requirements:` line and its `TEST` names.
- [ ] **Step 4:** Run the docs target; it must exit 0 with zero warnings. Run the full build and tests once more.
- [ ] **Step 5: Commit.** `docs(pipeline): fold plan A rulings into SDS 05`
