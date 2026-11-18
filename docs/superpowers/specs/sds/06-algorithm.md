# SDS 06 — `algorithm`

**Module:** `algorithm` · **Library:** `slideio-processor-engine` · **Depends on:** `core`, `config`, `pipeline`

The catalog. A leaf module — nothing depends on it — compiled against public
headers only, so UR-EXT-2 holds for every entry rather than for one
demonstration filter.

Conventions in [00-conventions.md](00-conventions.md).

---

## 6.1 Registration — `algorithm/registry.hpp`

```cpp
/// Registers every built-in operation with pipeline::OperationCatalog.
/// Idempotent. Called automatically at library load; exposed so a static
/// build can force it.
void registerBuiltinOperations();

/// Registers one category only, for a build that ships a subset.
void registerPixelOperations();
void registerSegmentationOperations();
void registerMeasurementOperations();
```

**Thread-safety:** ThreadSafe. **Requirements:** UR-EXT-5, UR-IMG-12.

This is the module's entire coupling to the rest of the library. Operations
self-register by name; `workspace` resolves stages against the catalog and
never links this module. That is what makes the catalog a leaf, and what let
its contents be settled last (6.10) without touching anything above it.

**Registration is per name and never replaces.** Each built-in is registered
under one module-wide mutex, and a name that `OperationCatalog::isRegistered`
already holds is left as it is, whoever registered it: a caller who registered
its own factory under a built-in's name first keeps it, and calling any of the
four functions again registers nothing new. The mutex serialises this module's
registrations only, so another caller may register the same name between the
check and the registration; `registerFactory` then throws
`InvalidArgumentError`, which is swallowed when the name is now registered and
rethrown otherwise. The functions throw only for a factory the catalog refuses
for another reason, and `std::bad_alloc`.

**Automatic at load.** A file-scope static object calls
`registerBuiltinOperations()` when the engine library is loaded, inside a
`try` that discards any exception, because an exception escaping a static
initialiser terminates the process; calling `registerBuiltinOperations()`
again surfaces the error. It is not guarded by a once-flag, because a callable
that throws can leave `std::call_once` hung on some standard libraries, and
idempotence per name makes a flag unnecessary.

`registerSegmentationOperations()` and `registerMeasurementOperations()` exist
so a build can ship a subset, but register nothing until the catalog has
segmentation and measurement operations; their documentation says so.

**Build.** The module is the OBJECT library `slideio-processor-algorithm`,
linked into `slideio-processor-engine` (`05-pipeline.md` 5.11). Its include
path is `include/`, SlideIO's headers and `src/algorithm/` as the root of its
own `detail/` headers, and deliberately not `src/`, so it cannot reach
`pipeline`'s private headers and is compiled against the public API only, as
a third-party operation would be (UR-EXT-2). It links `opencv_core` and
`opencv_imgproc` privately: OpenCV is confined to the engine library, not to
`pipeline`, because the catalog's median, morphology and Canny, its
derivative coefficients, and labelling and watershed (6.4, 6.7) need it. No
OpenCV type, header or macro appears in an `algorithm/` header, which the
build's OpenCV-free header check compiles as it does `pipeline/`'s. Using OpenCV does not mean trusting it: each operation keeps
its own correctness test (6.9).

## 6.2 Naming and parameter conventions

Every operation is named `category.operation` and constructed either by name
through the catalog or by a typed factory function here. Both paths produce the
same `OperationPtr`; the typed factories exist so C++ callers get compile-time
parameter checking, while the name path is what a stage configuration records
and replays (UR-STG-4).

```cpp
// Equivalent:
auto op1 = makeGaussianBlur(2.0, 0);
OperationParameters p; p.setDouble("sigma", 2.0).setInt("kernelSize", 0);
auto op2 = OperationCatalog::instance().create("pixel.gaussian_blur", p);
```

"Equivalent" is load-bearing rather than approximate: every factory here fills
the same parameter bag the catalog would receive and exposes it through
`Operation::parameters()` (5.1), so `PipelineSpec::then(op1)` and
`PipelineSpec::operation("pixel.gaussian_blur", p)` append an identical entry
and record identically (5.4). A factory that computed a value it did not put in
the bag would break that, and the round-trip conformance test of 5.1 is what
catches it.

**Parameter bags.** An operation's `parameters()` is always its **complete**
bag: every key of the table below is present, with the default filled in for any
key the caller omitted, so a typed factory and `OperationCatalog::create` with a
partial bag give bags that compare equal. An **unknown key, or a value of the
wrong type, throws `InvalidArgumentError`** naming the operation and the key,
never the value, so a misspelt recorded configuration fails rather than
replaying as defaults. An enumeration is stored as its enumerator spelling
(`"Above"`, `"Gaussian"`, `"TowardZero"`), and a `DataType` as
`dataTypeName()` (`"Float32"`); an unknown spelling is a value out of range and
throws the same way. A `double` key also accepts an integer whose magnitude is
at most 2⁵³, so exactly representable as a double: a configuration written by
hand with `"value": 128` reads back from JSON as an integer and still creates
the operation. The operation stores the value as a `double`, so its bag equals
the one created with `128.0` and round-trips unchanged; a larger integer is a
value of the wrong type. A typed factory refuses an argument out of range, or an
enumeration value that is not an enumerator, with `InvalidArgumentError` naming
the operation and the parameter.

| Operation | Key | Type | Default by name | Range |
|---|---|---|---|---|
| `pixel.colour_convert` | `from` | string | `"Rgb"` | a `ColourSpace` spelling, differing from `to` |
| | `to` | string | `"Grey"` | a `ColourSpace` spelling |
| `pixel.stain_deconvolution` | `vectors` | double array | hematoxylin and eosin | 6 or 9 components, stain by stain, as given; each vector valid as `StainVectors::setVector` requires |
| | `stainName0` … `stainName<count - 1>` | string | `"Hematoxylin"`, `"Eosin"` when `vectors` is absent too, else `""` | any |
| `pixel.threshold_fixed` | `value` | double | 128.0 | finite |
| | `direction` | string | `"Above"` | `"Above"`, `"Below"` |
| `pixel.threshold_otsu` | `direction` | string | `"Above"` | `"Above"`, `"Below"` |
| | `histogramMinimum`, `histogramMaximum` | double | absent | finite, minimum less than maximum, given together |
| | `binCount` | int | 256 when a range is given | 2 to 1048576; only with a range |
| `pixel.threshold_adaptive` | `method` | string | `"Mean"` | `"Mean"`, `"Gaussian"` |
| | `blockSize` | int | 11 | odd, 3 to 1025 |
| | `offset` | double | 2.0 | finite |
| | `direction` | string | `"Above"` | `"Above"`, `"Below"` |
| `pixel.convert_type` | `target` | string | `"Float32"` | a `dataTypeName()` spelling other than `Unknown` |
| | `rounding` | string | `"Nearest"` | a `RoundingMode` spelling |
| | `overflow` | string | `"Saturate"` | `"Saturate"`, `"Error"` |
| `pixel.gaussian_blur` | `sigma` | double | 1.0 | finite, greater than 0; with `kernelSize` 0, small enough that the derived size 2·⌈3·`sigma`⌉ + 1 is at most 1025 |
| | `kernelSize` | int | 0 | odd, 1 to 1025, or 0 to derive it from `sigma` |
| `pixel.median_blur` | `kernelSize` | int | 3 | odd, 3 to 255 |
| `pixel.sobel` | `dx` | int | 1 | 0, 1 or 2; `dx` + `dy` at least 1 |
| | `dy` | int | 0 | 0, 1 or 2 |
| | `kernelSize` | int | 3 | 1, 3, 5 or 7 |
| `pixel.scharr` | `dx`, `dy` | int | 1, 0 | (1, 0) or (0, 1) |
| `pixel.laplacian` | `kernelSize` | int | 1 | 1, 3, 5 or 7 |
| `pixel.canny` | `lowThreshold` | double | 50.0 | finite, at least 0, at most `highThreshold` |
| | `highThreshold` | double | 150.0 | finite, at least 0 |
| | `apertureSize` | int | 3 | 3, 5 or 7 |
| `pixel.bilateral_filter` | `diameter` | int | 5 | odd, 3 to 255 |
| | `sigmaColour` | double | 25.0 | finite, greater than 0 |
| | `sigmaSpace` | double | 5.0 | finite, greater than 0 |
| `pixel.morph_erode`, `pixel.morph_dilate`, `pixel.morph_open`, `pixel.morph_close` | `shape` | string | `"Rectangle"` | a `StructuringShape` spelling, or `"Custom"` for an element built by `StructuringElement::custom` |
| | `elementWidth`, `elementHeight` | int | 3, 3 | odd, 1 to 255 |
| | `elementMask` | double array | absent | present exactly when `shape` is `"Custom"`: `elementWidth` · `elementHeight` values, row by row from the top, each 0 or 1, at least one 1 |
| | `iterations` | int | 1 | 1 to 64 |

`pixel.threshold_otsu`'s range is the one exception to completeness: the three
range keys are present only when the operation was created with a range, since
"no range" has no value to fill in (6.5). Its bag still round-trips, which is
what the conformance test checks. A morphology operation's `elementMask` is
the same kind of key: a drawn shape is fully described by its name and size,
so the mask is recorded only for `"Custom"`, and a bag that gives it with
another shape, or omits it with `"Custom"`, is refused naming `elementMask`.

A range that couples two keys is reported against the key the caller set.
`pixel.gaussian_blur` with `kernelSize` 0 and a `sigma` whose derived size
would exceed 1025 is refused naming `sigma`, not `kernelSize`: the limit
belongs to the derived size, and `sigma` is what produced it. A Canny
`lowThreshold` above `highThreshold` is refused naming `lowThreshold`, and a
Sobel or Scharr pair of orders that selects no derivative names `dx`.
`StructuringElement`'s constructor and `custom` refuse a bad size or mask with
`InvalidArgumentError` naming the parameter (`width`, `height`, `mask`) and
`StructuringElement`, since no operation exists yet; a morphology factory
given a moved-from element refuses it naming the parameter `element`, rather
than reading an element that holds nothing.

Every factory's operation declares its channel arity, supported types, halo,
correctness reference and complexity note (UR-ALG-3, UR-ALG-4, UR-ALG-6,
UR-ALG-8). Algorithms of the same kind take comparable parameters in a
comparable order, and report errors, progress and cancellation through the same
mechanisms (UR-ALG-7) — which is an API review against a documented convention
applied to the whole catalog, not per algorithm.

## 6.3 Colour and stains — `algorithm/pixel/colour.hpp`

```cpp
enum class ColourSpace : int32_t { Rgb, Grey, Hsv, Lab };

/// pixel.colour_convert — Joint, requires uniform types, halo none.
OperationPtr makeColourConvert(ColourSpace from, ColourSpace to);

class StainVectors {
public:
    StainVectors();
    StainVectors(const StainVectors&);
    StainVectors(StainVectors&&) noexcept;
    StainVectors& operator=(const StainVectors&);
    StainVectors& operator=(StainVectors&&) noexcept;
    ~StainVectors();
    static StainVectors hematoxylinEosin() noexcept;
    static StainVectors hematoxylinDab() noexcept;

    int32_t count() const noexcept;
    void    setVector(int32_t index, double r, double g, double b);
    void    getVector(int32_t index, double& r, double& g, double& b) const;
    const char* stainName(int32_t index) const;
    void    setStainName(int32_t index, const char*);
};

/// pixel.stain_deconvolution — Joint, requires uniform types, halo none.
OperationPtr makeStainDeconvolution(const StainVectors&);
```

**Requirements:** UR-PIX-1, UR-PIX-2, UR-IMG-7, UR-ALG-3.

Both are `Joint` — they consume the selected channels together — and both
declare `requiresUniformTypes() == true`, so a selection mixing `Byte` and
`Float32` fails with an error naming the operation and both channels rather
than silently misinterpreting them (UR-ALG-3).

**RGB is inferred from the channels, and checked when the operation runs.**
Input that is to be read as RGB — `makeColourConvert` with `from == Rgb`, and
every `pixel.stain_deconvolution` — must be exactly three planes, each `Byte`,
whose `planeSourceChannel()` values are 0, 1 and 2 in that order. Fewer or more
planes throw `ChannelArityError`; another type or another channel order throws
`InvalidArgumentError`. The message names the operation, states the
requirement ("needs channels 0, 1, 2 as RGB, Byte") and lists the channels
found with their types, never a pixel value. Within a run the failure is
reported as every tile failure is: a `TileError` naming the operation, with the
message as its `cause()`.

An earlier draft had the factory throw when the scene's metadata did not state
that the selected channels were RGB. That cannot be built: an operation never
sees the scene — the factory has no scene, and `apply()` sees only planes — and
SlideIO's scene carries no "channels are RGB" flag to consult. Three `Byte`
channels in the order 0, 1, 2 is how SlideIO presents an RGB brightfield
scene, so the check is made on that shape, at the one point where the
planes and their source channels are known. A selection that reorders,
subsets or extends the channels is refused rather than interpreted, which is
what UR-IMG-7 asks of an operation that gives channels a meaning.

**A `UInt16` scene is refused before any tile runs.** `pixel.colour_convert`
declares `Byte` and `Float32` and `pixel.stain_deconvolution` declares `Byte`,
so the pipeline's validation rejects a `UInt16` selection with
`UnsupportedDataTypeError` naming the operation, which is earlier than the RGB
check and says the same thing (UR-IMG-11).

**Colour conversion.** Every pair converts through RGB, in double precision,
and writes `Float32`: one plane for `Grey`, else three. The units are fixed:

| Space | Planes | Units |
|---|---|---|
| `Rgb` | 3 | 0 to 255. As input: `Byte`, channels 0, 1, 2 as above |
| `Grey` | 1 | 0 to 255. From RGB, ITU-R BT.601 luma 0.299 R + 0.587 G + 0.114 B; to RGB, the value replicated. As input: `Byte` or `Float32` |
| `Hsv` | 3 | Hexcone model: H in degrees [0, 360), 0 when S is 0; S and V in [0, 1]. As input: `Float32` |
| `Lab` | 3 | CIE 1976 L\*a\*b\*, D65 white (0.95047, 1, 1.08883), L\* in [0, 100]. As input: `Float32` |

Lab goes through linear sRGB per IEC 61966-2-1, with its linearisation, and the
sRGB-to-XYZ matrix derived from that standard's primaries and the D65 white
point to seven digits (0.4124564, 0.3575761, 0.1804375, …), whose rows sum to the
white point. The four-digit matrix often quoted misses the published Lab
values by about 8 × 10⁻³, which the seven-digit form does not. An input of the
wrong plane count or type for its source space throws `ChannelArityError` or
`UnsupportedDataTypeError` naming the operation. The output is never clamped:
an out-of-gamut Lab or HSV input gives RGB outside 0 to 255. A pixel with a NaN
in any input component is NaN in every output component, whatever the pair.

Because RGB output is `Float32` and RGB input must be `Byte`, an `X → Rgb`
conversion cannot feed an `Rgb → Y` conversion; a chain wanting `X → Y` uses
that pair directly, which converts through RGB internally and loses nothing.
Writing RGB back as `Byte` would round and clamp, which UR-IMG-11 forbids an
operation to do silently; `pixel.convert_type` does it when asked.

**Stain vectors.** `StainVectors` is pimpl'd, with the full copy and move set
of C-2. It holds **at most three stains**. `setVector(index, …)` takes an index
from 0 to `count()`, where `count()` appends a stain; any other index, a fourth
stain, a negative, NaN or infinite component, or three zero components throws
`InvalidArgumentError` and leaves the object unchanged. Vectors are stored as
given, so `getVector` returns what `setVector` received, and are normalised to
unit length where they are used. `getVector`, `stainName` and `setStainName`
with an index out of range throw `InvalidArgumentError`, as does `setStainName`
with a null name. A stain's name is `""` until set, and the pointer `stainName`
returns is valid until the next non-const call. The built-in pairs are those of
Ruifrok and Johnston (2001): hematoxylin (0.650, 0.704, 0.286) with eosin
(0.072, 0.990, 0.105), and hematoxylin with DAB (0.268, 0.570, 0.776), named
`"Hematoxylin"`, `"Eosin"` and `"DAB"`.

**Stain deconvolution** is colour deconvolution after Ruifrok and Johnston.
Per pixel, each channel's optical density is OD = −log₁₀(max(I, 1) / 255), so a
sample of 0 reads as 1 and white is no stain. The unit stain vectors are the
rows of a 3 × 3 matrix M; with two stains the third row is the unit cross
product of the first two, whose amount is computed but not written. The stain
amounts are OD · M⁻¹ in double precision, written as one `Float32` plane per
stain in stain order. The factory throws `InvalidArgumentError` for fewer than
two stains, or when M's determinant is below 10⁻⁶ in magnitude, that is when
the vectors are linearly dependent.

Deconvolution output is `Float32` per stain, and it reaches a consumer
losslessly. Open question 3 asked whether that fits SlideIO's `Scene`
channel-type model and is now closed: the model always carried floats, but was
unreachable from outside the tree, so SlideIO gains a public extension seam —
`ISceneSource`, `SceneMetadata`, `makeScene` — which `slideio-processor`
implements. `SceneMetadata` carries a `DataType` per channel, so a scene mixing
`Byte` input with `Float32` stain planes declares the mix truthfully.

What a caller must expect is one read per type group rather than one read for
all channels: `cv::Mat` holds a single depth for every channel it carries, so
no block can span `Byte` and `Float32`. That is a property of every SlideIO
scene rather than of a computed one, nothing is lost by it, and `BlockView` is
planar with a `DataType` per plane for exactly this reason (`01-core.md` 1.8).

## 6.4 Filters and morphology — `algorithm/pixel/filter.hpp`

```cpp
enum class BorderMode : int32_t { Reflect, Replicate, Constant };

/// All PerChannel. Halo derives from kernel size.
OperationPtr makeGaussianBlur(double sigma, int32_t kernelSize);
OperationPtr makeMedianBlur(int32_t kernelSize);
OperationPtr makeSobel(int32_t dx, int32_t dy, int32_t kernelSize);
OperationPtr makeScharr(int32_t dx, int32_t dy);
OperationPtr makeLaplacian(int32_t kernelSize);
OperationPtr makeCanny(double lowThreshold, double highThreshold,
                       int32_t apertureSize);
OperationPtr makeBilateralFilter(int32_t diameter,
                                 double sigmaColour, double sigmaSpace);

enum class StructuringShape : int32_t { Rectangle, Ellipse, Cross };

class StructuringElement {
public:
    StructuringElement(StructuringShape, int32_t width, int32_t height);
    StructuringElement(const StructuringElement&);
    StructuringElement(StructuringElement&&) noexcept;
    StructuringElement& operator=(const StructuringElement&);
    StructuringElement& operator=(StructuringElement&&) noexcept;
    ~StructuringElement();
    static StructuringElement custom(const uint8_t* mask,
                                     int32_t width, int32_t height);
    int32_t width() const noexcept;
    int32_t height() const noexcept;
};

/// All PerChannel. Halo derives from the structuring element.
OperationPtr makeErode(const StructuringElement&, int32_t iterations);
OperationPtr makeDilate(const StructuringElement&, int32_t iterations);
OperationPtr makeOpen(const StructuringElement&, int32_t iterations);
OperationPtr makeClose(const StructuringElement&, int32_t iterations);
```

**Requirements:** UR-PIX-3, UR-PIX-4, UR-EXT-3, UR-QA-2, UR-IMG-11, UR-ALG-6,
UR-ALG-8.

Every one of these is a neighbourhood operation whose halo derives from its
parameters, and the framework supplies exactly the declared border context so
results are correct at tile seams; pixels beyond the level read as zero. Each
must produce whole-slide output matching a single-tile reference computation
on an image small enough to process in one tile (UR-QA-2) — the acceptance
criterion for both UR-PIX-3 and UR-PIX-4.

**Who computes what.** "Matching" is read as bitwise, and that decides where
OpenCV may be used. An operation that forms new values from sums — Gaussian,
Sobel, Scharr, Laplacian and bilateral — runs on the module's own kernels for
every type, because OpenCV's filters vectorise and may group a sum differently
depending on where a pixel falls in the buffer, so a pixel's value could depend
on its column within the tile. An operation that only selects an existing
value can use OpenCV wherever OpenCV's selection is exact: the median of `Byte`
(`cv::medianBlur`), erode, dilate, open and close of `Byte`, `UInt16` and
`Int16` (`cv::erode`, `cv::dilate`, `cv::morphologyEx`). Canny of `Byte` also
goes to OpenCV (`cv::Canny`): its gradients of `Byte` input are exact integers
and its output is a decision per pixel, so no sum is grouped differently from
one column to another. The median of every other type is the module's own exact order
statistic, so it agrees with OpenCV wherever both apply. OpenCV is called on a
`cv::Mat` header over the plane's own memory and row stride, made by the
private `detail::planeAsMat`, with no copy; it filters the whole halo'd plane,
and only the tile, whose windows lie wholly inside that plane, is copied out,
so OpenCV's own border rule never reaches an output pixel. OpenCV's thread
count is left as the caller configured it, and no result depends on it.

**Float morphology is the module's own.** Erode, dilate, open and close of
`Float32` and `Float64` do not go to OpenCV, although OpenCV accepts those
types: its vectorised minimum and maximum treat a NaN differently from its
scalar code for the last columns of a row, so the same window gave different
results in different columns, and the tiled output depended on where the
tiles fell. The module's own pass visits the element's set pixels in one fixed
order, row by row from the top and left to right; a NaN anywhere in the window
makes the result NaN, and between −0 and +0, which compare equal, the first met
is kept. On NaN-free input it equals OpenCV's result.

**Output types.** No operation here rounds or saturates silently (UR-IMG-11).
Gaussian, bilateral, Sobel, Scharr and Laplacian compute in `double` and write
`Float32` for every input type except `Float64`, which writes `Float64`; an
integer image smoothed or differentiated has fractional and negative values,
and writing them back into the input type would lose them. A `Float32` result
is the double rounded to nearest. Median, erode, dilate, open and close write
the input type: they select existing samples and create no value. Canny writes
`Byte`, 255 on an edge and 0 elsewhere.

**Supported types.** The summing operations accept every type except `Int64`
and `UInt64`, whose samples do not all fit a `double` exactly. The median
accepts all eleven types; it compares integers exactly, puts NaN above every
number, and copies the selected sample's bytes, so a `Float16` or a NaN keeps
its bit pattern. Morphology accepts `Byte`, `UInt16`, `Int16`, `Float32` and
`Float64`, the types `cv::erode` and `cv::dilate` accept. Canny accepts `Byte`.
Another type is refused before any tile runs, with
`UnsupportedDataTypeError` naming the operation.

**Fixed-order evaluation.** The Gaussian, Sobel, Scharr and Laplacian, and
`pixel.threshold_adaptive` (6.5), share one private separable evaluator. For
each output pixel it forms each window row's weighted sum left to right with
the horizontal kernel, then weights the column of those row sums top to bottom
with the vertical kernel, in `double`. Row sums may be shared between the
pixels of a tile, but each is a function of its own row's samples alone, so
every value is exactly what the per-pixel order gives and does not depend on
the tiling. The Laplacian is two such passes, d²/dx² and d²/dy², added per
pixel with the x term first. The bilateral filter sums over its disc in its own
fixed order, row by row and left to right. The algorithm library and its tests
are compiled with `-ffp-contract=off` on GCC and Clang, and MSVC's default
`/fp:precise` does not contract, so a multiply and an add are never fused into
one rounding in one place and kept apart in another.

**Kernels, sizes and halos.** Every halo is the same on all four sides except
morphology's, which follows the element's two axes.

- **Gaussian.** Weights exp(−d²/2σ²) for d from −`kernelSize`/2 to
  `kernelSize`/2, normalised to sum 1, the same kernel along both axes.
  `kernelSize` 0 derives 2·⌈3·`sigma`⌉ + 1, at most 1025, and size 1 leaves
  every sample unchanged apart from the conversion to the output type. Halo
  `kernelSize`/2 of the size used.
- **Sobel.** The separable, unnormalised coefficients OpenCV's `Sobel` uses,
  taken from `cv::getDerivKernels(dx, dy, kernelSize, false, CV_64F)` when the
  operation is built. Size 1 differentiates with a 3-tap kernel along the
  derivative's axis and does not smooth across it. Halo max(1, `kernelSize`/2).
- **Scharr.** The 3 × 3 coefficients [3 10 3] × [−1 0 1], from
  `cv::getDerivKernels` with `FILTER_SCHARR`. Halo 1.
- **Laplacian.** Size 1 is the 3 × 3 kernel [[0, 1, 0], [1, −4, 1], [0, 1, 0]],
  the sum of the second differences [1, −2, 1] in x and in y; a larger size is
  d²/dx² + d²/dy² from the Sobel second-derivative coefficients of that size.
  Halo max(1, `kernelSize`/2).
- **Median.** The middle value of the sorted `kernelSize` × `kernelSize`
  window. The window is gathered in the same row-major order for every pixel,
  so the choice among equal keys (signed zeros, NaNs) does not depend on the
  tiling. Halo `kernelSize`/2.
- **Bilateral.** The weighted mean over the disc of radius `diameter`/2, a
  sample at distance d with value v weighing
  exp(−d²/2σs²)·exp(−(v − c)²/2σc²), c the centre sample, on the sample values
  as read. A centre that is not finite (NaN, +∞ or −∞) is written unchanged,
  and a neighbour that is not finite is skipped, contributing to neither sum:
  otherwise one NaN or infinity would turn every finite pixel within the
  radius into NaN, which no caller asking for edge-preserving smoothing wants.
  Halo `diameter`/2.
- **Morphology.** r = `elementWidth`/2 horizontally and `elementHeight`/2
  vertically. Erode and dilate take the minimum or maximum over the element's
  set pixels `iterations` times, halo `iterations`·r per axis. Open erodes
  `iterations` times then dilates `iterations` times, and close does the
  reverse, as OpenCV's `morphologyEx` does, halo 2·`iterations`·r per axis.
  Because pixels beyond the level read as zero, an erosion reaching the
  level's edge pulls it to zero.
- **Canny.** Sobel gradients of aperture `apertureSize`, the L1 magnitude
  (`L2gradient = false`), non-maximum suppression and hysteresis, as
  `cv::Canny` computes them. Halo `apertureSize`/2 + 1 + 32 (34, 35 or 36): the
  gradient and the suppression need `apertureSize`/2 + 1, and the 32 is the
  reach declared for hysteresis, below.

**The structuring element.** `StructuringElement` is pimpl'd with the full copy
and move set of C-2. Width and height are odd, from 1 to 255, and the anchor is
the centre: the set pixel in column i and row j covers the sample at offset
(i − width/2, j − height/2). The three shapes are drawn as
`cv::getStructuringElement` draws `MORPH_RECT`, `MORPH_ELLIPSE` and
`MORPH_CROSS`. `custom(mask, width, height)` copies width · height bytes, row by
row from the top, a non-zero byte being a set pixel; the mask must be non-null
and have at least one set pixel. A moved-from element's `width()` and
`height()` return 0, and a factory refuses it (6.2). The operation copies the
element, and its bag records it as `shape`, `elementWidth`, `elementHeight`
and, for a custom element only, `elementMask`, so the name path rebuilds the
same element. `iterations` runs from 1 to 64.

**`BorderMode` is declared and not yet taken.** No factory in this version
takes a `BorderMode`: the framework supplies each operation's border context,
and pixels beyond the level read as zero, as with `Constant` and a value of 0.
The enumeration is declared as above so a later version can expose the choice
without changing the header's other declarations, and its documentation says
so.

**`makeCanny` is the exception to exactness, and it is declared rather than
discovered.** Canny's final stage is edge tracking by hysteresis: a weak edge
is kept when it is connected, along a chain of any length, to a strong one.
Connectivity of unbounded reach is not a neighbourhood property, so no fixed
halo makes it exact — a weak chain whose strong seed lies further away than
the halo is dropped where single-tile processing would have kept it.

The halo is therefore a **stated bound on the approximation**, not a guarantee
of equality. The gradient and suppression of every tile pixel lie wholly
inside the halo'd plane; the further 32 pixels are the reach hysteresis is
allowed. The operation reports that bound through its complexity note
(UR-ALG-8), which reads exactly: *"O(n) in the number of pixels; equals
single-tile Canny wherever every weak edge chain reaches a strong edge within
32 pixels, and may drop weak edges beyond that reach"*. Gradients, suppression
and the strong edges are the same at every tile size, and a tile sees fewer
strong seeds than the whole level, never more, so tiling can only drop weak
edge pixels, never add one. UR-PIX-3's acceptance criterion — "whole-slide output
matches a single-tile reference" — is stated without tolerance and cannot be
met unconditionally by this one operation; that conflict stays recorded as
open question 15 in `requirements.md` and needs a requirements amendment
rather than a silent reinterpretation here.

Everything else in 6.4 and 6.5 is genuinely local. Gaussian, median, Sobel,
Scharr, Laplacian, bilateral and all four morphology operations depend on a
bounded neighbourhood, so the declared halo makes them exact, and Otsu is
global by construction rather than by accident (6.5).

## 6.5 Thresholding — `algorithm/pixel/threshold.hpp`

```cpp
enum class ThresholdDirection : int32_t { Above, Below };
enum class AdaptiveMethod     : int32_t { Mean, Gaussian };

/// pixel.threshold_fixed — PerChannel, halo none.
OperationPtr makeFixedThreshold(double value, ThresholdDirection);

/// pixel.threshold_otsu — PerChannel, halo none, global context.
/// Accumulates a 64-bit histogram over the whole slide or ROI, then applies.
OperationPtr makeOtsuThreshold(ThresholdDirection);

/// pixel.threshold_adaptive — PerChannel, halo = blockSize/2.
OperationPtr makeAdaptiveThreshold(AdaptiveMethod, int32_t blockSize,
                                   double offset, ThresholdDirection);
```

**Requirements:** UR-PIX-5, UR-EXE-1, UR-QA-2, UR-QA-3, UR-IMG-11.

`makeOtsuThreshold` is the module's only global-context operation and the
reason `pipeline::Reduction` exists. It returns `needsGlobalContext() == true`,
accumulates a `Histogram64` per tile, combines order-independently because the
bins are integers, and computes the threshold once before the apply pass.

Its acceptance criterion is that the streamed whole-slide value equals the
value computed from a full in-memory histogram of the same slide — which also
makes it the natural regression test for UR-QA-3's worker-count invariance,
since a float accumulator here would break it non-deterministically.

**Every threshold writes `Byte`: 255 for foreground, 0 for background.**
`Above` makes a sample foreground when it is **strictly greater** than the
threshold, `Below` when it is **strictly less**; a sample equal to the
threshold is background either way. A NaN sample is always background.

**Integer samples are compared exactly.** No integer sample is converted to
`double` for the comparison, so an `Int64` sample of 2⁵³ + 1 is above a
threshold of 2⁵³, although both are the same `double`. A fractional threshold
on an integer type falls between two integers (100 is below 100.5, 101 above
it). A threshold outside the type's range is not clamped into it — clamping
would make `0 > −5` false for `Byte` — but puts every sample on one side: for
`Byte`, `Above` −5 makes every sample foreground and `Below` −5 every sample
background, and 300 the reverse.

**Fixed.** `pixel.threshold_fixed` accepts every type and compares each sample
with `value`.

**Otsu, its bins.** Without a histogram range, `Byte`, `Int8`, `UInt16` and
`Int16` get one bin per value of the type, and `binCount` must not be given.
Any other type needs `histogramMinimum` and `histogramMaximum` (with `binCount`,
256 by default); without them the run fails, reported as a `TileError` whose
cause is an `InvalidArgumentError` naming the operation, the type and the two
missing keys. With a range the bins divide [minimum, maximum] evenly, as
`Histogram64` does, a sample equal to the maximum falling in the last bin; a
sample below the range counts as bin −1 and one above it as bin `binCount`.
**A ranged Otsu is reachable only by name** — `OperationCatalog::create` or
`PipelineSpec::operation` with the range keys — because the typed factory
takes only a direction, as above, and its bag then holds `direction` alone.

**Otsu, the two passes.** The first pass accumulates one 64-bit histogram per
source channel from the samples of each tile rect that the run's restriction
admits (`05-pipeline.md` 5.4, 5.5), and tiles combine by adding their
integer bins, so the result depends on neither worker count nor tile order.
Each channel's threshold bin k\* is then the smallest k in [0, binCount − 2]
maximising
w₀ · w₁ · (m₀ − m₁)² over splits whose two classes are both non-empty, the class
means taken over bin indices; when no split has two non-empty classes, k\* is
the highest non-empty bin. The second pass makes a bin above k\* foreground for
`Above`, and a bin at or below it foreground for `Below`. A channel that counted
nothing — every sample NaN or outside the restriction — is all background.

**Otsu, distinct channels.** Each source channel has one histogram, so the
planes of a tile must come from distinct source channels. A `Joint` operation
that writes several planes from one source channel gives them all that channel
— `pixel.colour_convert` from `Grey` to `Hsv` does — and Otsu after it
would otherwise pool H, S and V into one histogram. The first pass refuses
such a tile with `InvalidArgumentError` naming the operation and stating that
the source channels must be distinct, and so does the second when one call
holds such planes; a run reports it as a `TileError` naming the operation,
with that message as its `cause()`.

**Adaptive.** `pixel.threshold_adaptive` compares each sample with
T = W − `offset`, W the average of the `blockSize` × `blockSize` window centred
on it: for `Mean` the window sum over `blockSize`²; for `Gaussian` the
separable weights exp(−d² / 2σ²), d from −`blockSize`/2 to `blockSize`/2,
σ = 0.3 · ((`blockSize` − 1)/2 − 1) + 0.8, normalised to sum 1. Its halo is
`blockSize`/2 on every side, and window pixels beyond the level read as zero.
A NaN anywhere in the window makes the pixel background, since the window
average is then NaN. **Each pixel is computed from its own window alone**, in
double, in one fixed order — each window row's weighted sum left to right, then
the row sums top to bottom — with no running sum carried from a neighbour, so
the result is bitwise independent of the tiling (UR-QA-2). It accepts every
type except `Int64` and `UInt64`, whose window sums are not exact in double;
every other type's samples are exact in double, so its integer comparisons stay exact.

## 6.6 Type conversion — `algorithm/pixel/convert.hpp`

```cpp
enum class RoundingMode   : int32_t { Nearest, TowardZero, Floor, Ceil };
enum class OverflowMode   : int32_t { Saturate, Error };

/// pixel.convert_type — PerChannel, halo none.
OperationPtr makeTypeConvert(DataType target, RoundingMode, OverflowMode);
```

**Requirements:** UR-IMG-13, UR-IMG-10, UR-IMG-11.

Conversion behaviour is explicit and documented rather than implicit, and
`OverflowMode::Error` raises rather than saturating. No operation in the catalog
silently narrows, saturates or loses precision — UR-IMG-11 forbids it, and this
is the one place a caller opts into it deliberately.

Tested at type boundaries — minimum, maximum, overflow — against the documented
behaviour (UR-IMG-13).

`pixel.convert_type` accepts every type and writes `target`. Each sample is
converted by exactly one rule, and integer samples never pass through `double`:

| Source → target | Rule |
|---|---|
| the same type | copied bit for bit |
| integer → integer | exact when in range; out of range, it overflows |
| integer → float | the nearest representable value, ties to even, rounded in integer arithmetic, so `Int64` 2⁵³ + 1 gives 2⁵³ and a `UInt64` with its top bit set rounds correctly to `Float32` and `Float64` on every compiler. A `Float16` result of magnitude 65520 or more, which would round to infinity, overflows; 65519 gives 65504 |
| float → integer | rounded by the `RoundingMode` (`Nearest` is ties to even), then out of range it overflows; NaN and ±infinity overflow |
| float → wider or equal float | exact |
| float → narrower float | the nearest representable value, ties to even; a finite value whose rounded result would be infinite overflows; NaN and ±infinity are kept |

**Overflow.** Under `OverflowMode::Saturate` an overflowing sample becomes the
target's minimum or maximum — for a float target, the largest finite value with
the sample's sign — and NaN converted to an integer type becomes 0. Under
`OverflowMode::Error` the tile fails with `InvalidArgumentError`
`"pixel.convert_type: <n> samples of <source type> do not fit <target type>"`,
counting every overflowing sample of the tile and naming no value. When planes
of several types overflow, the source types are listed in plane order, each
once (`"… 4 samples of Int16, Int32 do not fit Int8"`). The
`RoundingMode` affects only conversions to an integer type.

`Float16` is decoded exactly by the module's own binary16 routine and encoded
with rounding to nearest, ties to even, so the eleven types convert among
themselves without passing through a lossy intermediate (UR-IMG-10).

## 6.7 Segmentation — `algorithm/segment/segment.hpp`

```cpp
enum class Connectivity : int32_t { FourConnected, EightConnected };

/// segment.tissue_detect — Joint, halo small, writes a semantic layer.
class TissueDetectParameters {
public:
    TissueDetectParameters();
    TissueDetectParameters& saturationThreshold(double);
    TissueDetectParameters& minimumRegionAreaPx(int64_t);
    TissueDetectParameters& closingRadiusPx(int32_t);
    TissueDetectParameters& targetLayer(const char*); // "" = the stage's only layer
};
OperationPtr makeTissueDetect(const TissueDetectParameters&);

/// segment.connected_components — PerChannel, halo 1, writes objects.
class ConnectedComponentsParameters {
public:
    ConnectedComponentsParameters();
    ConnectedComponentsParameters& connectivity(Connectivity);
    ConnectedComponentsParameters& minimumAreaPx(int64_t);
    ConnectedComponentsParameters& maximumAreaPx(int64_t);   // 0 = unbounded
    ConnectedComponentsParameters& extractHoles(bool);
    ConnectedComponentsParameters& targetLayer(const char*);
};
OperationPtr makeConnectedComponents(const ConnectedComponentsParameters&);

/// segment.contours — extracts polygon contours including interior holes.
class ContourParameters {
public:
    ContourParameters();
    ContourParameters& includeHoles(bool);
    ContourParameters& targetLayer(const char*);
};
OperationPtr makeContourExtraction(const ContourParameters&);

enum class WatershedMarkers : int32_t {
    LocalMaxima,       // detected in the input plane
    AuxiliaryRaster    // one basin per distinct value of a bound raster (5.1),
                       // normally LayerRasterContent::ObjectAnchor
};

/// segment.watershed — PerChannel, halo = maximumMarkerSpacingPx, writes objects.
class WatershedParameters {
public:
    WatershedParameters();
    WatershedParameters& markers(WatershedMarkers);
    WatershedParameters& auxiliaryName(const char*);       // for AuxiliaryRaster
    WatershedParameters& minimumMarkerDistancePx(int32_t); // suppression radius

    /// The bound that makes this operation tileable, and a hard one: no basin
    /// boundary may depend on a marker further away than this. Derives the
    /// halo. Exceeding it is an error, never a silent approximation.
    WatershedParameters& maximumMarkerSpacingPx(int32_t);

    WatershedParameters& minimumAreaPx(int64_t);
    WatershedParameters& targetLayer(const char*);
};
OperationPtr makeWatershed(const WatershedParameters&);
```

**Requirements:** UR-SEG-1, UR-SEG-2, UR-SEG-3, UR-SEG-4, UR-SEG-7, UR-SEG-8,
UR-STG-4, UR-STG-10.

**The target layer is named, never passed as a `LayerId`.** An earlier draft
took `LayerId target` as a constructor argument on each of these factories,
which broke the name path: a stage constructs its operations through
`OperationCatalog::create(name, OperationParameters)` (5.3), the bag holds only
int, double, bool, string and double-array, and there is nowhere for a
`LayerId` to go. Every object-producing operation in the catalog was therefore
unconstructible by the only route a stage uses.

Naming the layer instead fixes that and buys two further things. A `LayerId` is
an opaque handle valid in one store, so a recorded configuration containing one
could not be replayed against a different workspace — a layer *name* round-trips
through `configurationJson` and stays meaningful (UR-STG-4, UR-RES-11). And the
typed factories here and the name path now produce byte-identical parameter
bags, because `targetLayer(const char*)` is simply `setString("targetLayer", …)`;
the equivalence claimed in 6.2 holds for object-producing operations too, which
it previously did not.

Resolution and validation belong to the framework, not to the operation. The
framework resolves the name against the layer registry before the run starts and
exposes the result as `OperationContext::targetLayer()` (5.1), so an operation
author writes `ctx.targetLayer()` and never touches a registry. An empty name
means *the single layer the stage declared* — the common case, and the reason a
one-layer stage need not repeat itself. A stage declaring several layers must
name one per object-producing operation; omitting it there is an error, not a
guess (4.4).

Every object-producing operation emits boundary-touching regions through
`OperationContext::emitSeamFragment` rather than as finished objects, so
`SeamMerger` joins them into single objects with geometry and measurements
computed as if the slide had been processed as one image (UR-SEG-3). An
operation author never writes merging logic.

**The analysis level is set on the run, not here.** Tissue detection does not
need full resolution, and UR-IMG-4 requires reading from the coarsest adequate
level — but that is `PipelineSpec::analysisLevel` (5.4), defaulting to -1 for
automatic selection. An earlier draft put `analysisLevel` on
`TissueDetectParameters`, which gave the decision two owners: the tile grid is
laid out at one level for the whole run (5.6), so an operation asking for a
different one had nowhere to be honoured. A chain that needs two levels is two
stages.

`minimumAreaPx` and `maximumAreaPx` filter **during extraction**, so discarded
objects are never written to the store. UR-SEG-7's acceptance criterion is that
a filtered run on a slide with many small artefacts yields a materially smaller
store, and that the filtered objects appear in no query.

Contours are polygon rings in level-0 slide coordinates, with holes as separate
rings (UR-SEG-4).

### Watershed, and why it is tileable after all

`segment.watershed` is **marker-controlled**, and that is the whole of the
reason it can live in a tiled catalog. Unseeded watershed — flooding from every
local minimum — assigns a basin by competition that can propagate the length of
a connected component, so segmenting a confluent sheet would need the whole
sheet, which no tile has and `SeamMerger` cannot supply because it runs
downstream of the operation that emits. That variant is not in v1 and is not
merely deferred; it does not fit this execution model.

Marker-controlled flooding is different in the way that matters. A basin grows
until it meets a neighbouring basin, so the line between two markers is decided
by the pixels between them. **The reach is bounded by marker spacing, not by
component size.** Nuclei in H&E sit roughly 28 px apart at 0.25 µm/px, so a
clump of them is segmented inside an ordinary halo however large the sheet they
belong to — the component may span fifty tiles and every boundary within it is
still a local decision.

`maximumMarkerSpacingPx` turns that from an observation into a contract, and
it is UR-SEG-8's declared reach. It derives the halo, and the operation
**throws** when a pixel of a segmented region lies further than that from any
marker in its halo'd context, because such a pixel's basin could be decided by
evidence the tile cannot see — which is the rejection UR-SEG-8 requires rather
than permits. Failing
is what keeps UR-QA-2 exact: for every input that runs, each boundary is
determined entirely within the supplied context, so the result is identical
across tile sizes by construction rather than by hoping clumps stay small.
Degrading quietly instead — segmenting what fits, passing through what does not
— would make the output depend on tile position, which 5.6 identifies as the
worst failure available because the count comes out nearly right.

### Coarse-to-fine, as a supported pattern

`WatershedMarkers::AuxiliaryRaster` is the fine half of a two-stage
multi-resolution pass, and it is why `TileRasterSource` exists (`01-core.md`
1.9). A coarse stage runs at `analysisLevel` 3 or 4, where the whole slide is
10⁸ pixels or fewer and a whole-slide watershed is an ordinary in-memory
computation; it writes regions. A fine stage then runs at level 0 or 1 with
`readsLayerAsRaster` (`04-workspace.md` 4.2) binding those regions as pixels,
and segments detail the coarse level could not resolve — structure standing
above the coarse surface.

The two halves succeed for different reasons, which is worth keeping straight.
The coarse pass is correct because the whole slide fits; the fine pass is
correct because marker-controlled flooding is local. Neither inherits the
other's argument.

**What actually crosses between them is objects, and the representation goes
round a circle.** The coarse watershed floods pixels and produces a label
image; it traces basin contours to emit them as polygons, because UR-SEG-4
requires level-0 contours and `ObjectWriter` is the only way out of a stage;
the store holds polygons; and `makeLayerRasterSource` rasterises them back into
a plane at the fine level. Raster to polygon to raster, two conversions that
cancel.

That is the price of stages communicating only through a store that holds
objects and not pixels, and it is a price worth paying: the coarse result is
durable, so re-tuning the refinement re-runs only the fine stage; it is
queryable and exportable, so a basin can be inspected in QuPath; and the
rasterisation is per tile, so no whole-slide raster is ever built. A retained
in-memory buffer would have none of those properties, and persisting the label
image instead would need a raster layer in a store deliberately built around
objects.

**The conversion is lossy in one direction that a refinement must expect.** A
contour traced at level 4 has vertices on level-4 pixel corners, so mapped down
it lands on multiples of 16 and arrives at the fine stage as a **staircase with
16-pixel steps**. Which matters depends on the use:

- A refinement that **redraws** the boundary — flooding from its own markers,
  using the coarse basin only for membership and identity — is unaffected. The
  answer it needs is which basin a pixel belongs to, and that is right to
  within half a coarse pixel.
- A refinement that treats the coarse boundary as a **geometric reference**,
  constraining displacement to a band around it, reproduces the staircase
  faithfully. The band must exceed the step size, or the output carries the
  coarse grid as visible structure.

This is the argument for `ObjectAnchor` over any encoding of distance to the
coarse boundary: identity survives downsampling exactly, and geometry does
not.

What coarse-to-fine does **not** do is revise topology. A boundary the coarse
pass did not draw cannot be refined into existence, and two regions it merged
stay merged — the fine pass subdivides within coarse regions and moves their
edges, it does not re-partition them. An object whose separating feature is
gone at every level where the slide fits, and which is also larger than any
practical halo, is outside both arguments; a confluent sheet treated as one
region is the honest example, and it remains the one case needing a post-merge
pass the design does not have.

The pattern generalises past this operation. Any operation wanting coarse
context — background estimation for adaptive thresholding, tissue-aware
filtering, scale-space feature detection — reads it the same way, which is the
better reason to have built the binding than the one algorithm that prompted
it.

## 6.8 Measurements — `algorithm/measure/measure.hpp`

```cpp
enum class MeasurementKind : int32_t {
    Area, Perimeter, Centroid, BoundingBox,
    IntensityMean, IntensityStdDev, IntensityMin, IntensityMax
};

class MeasurementSelection {
public:
    MeasurementSelection();
    static MeasurementSelection standard() noexcept;   // all of the above
    static MeasurementSelection geometryOnly() noexcept;

    MeasurementSelection& add(MeasurementKind);
    MeasurementSelection& addAll() noexcept;
    bool    contains(MeasurementKind) const noexcept;
    int32_t count() const noexcept;
};

/// measure.object_measurements — computed per object as it is emitted.
OperationPtr makeObjectMeasurements(const MeasurementSelection&,
                                    const ChannelSelection& intensityChannels);

/// Calibration is applied automatically from the scene's resolution metadata.
/// Reports whether it was available.
bool sceneIsCalibrated(slideio::ScenePtr) noexcept;
double scenePixelSizeMicrons(slideio::ScenePtr) noexcept;   // 0 if unavailable
```

**Requirements:** UR-SEG-5, UR-SEG-6, UR-IMG-2.

Measurements live in `algorithm` rather than in their own module: they are
computed per object during extraction, share the catalog's declaration
machinery, and have no consumer that does not also use the catalog.

Every accumulator is 64-bit. An intensity sum over a whole-slide tissue region
exceeds 2³¹ trivially, and UR-IMG-2's static check covers this path.

**Across a tile seam the operation emits partials, not measurements.** For a
region touching a boundary it calls the four-argument
`emitSeamFragment(fragment, layer, partials, partialCount)` with one
`IntensityPartial` per channel (`05-pipeline.md` 5.1, 5.7) instead of finishing
the measurement, because the rest of the object is in another tile. The
two-argument form carries no partials and suits an operation that measures no
intensity. `SeamMerger` merges the fragments into one polygon, combines the
partials per channel in canonical fragment order (ascending fragment anchor,
ties by tile index), and measures the result with `applyStandardMeasurements`
(`05-pipeline.md` 5.1). An object finished inside one tile is measured by the
same function, from its polygon and partials built the same way, so the two
paths share one formula rather than two that must agree. Area, perimeter,
centroid and intensity statistics are then identical to single-tile
processing — bitwise for Byte intensities, to a relative 10⁻¹² for Float32,
whose sums regroup with the tile size (`05-pipeline.md` 5.7) — which is what
UR-SEG-3's acceptance criterion measures across three tile sizes, and the
reason this operation cannot simply average or sum what each tile computed.
`applyStandardMeasurements` sets the pixel-unit measurements; the calibrated
units below are added here.

Measurements with a physical dimension are available in both pixels and
calibrated units. A scene lacking resolution metadata yields pixel units with
`Measurement::isCalibrated() == false`, which is the documented indication
UR-SEG-6 requires rather than a silent fallback.

## 6.9 Correctness references

UR-ALG-6 requires each algorithm to carry a documented correctness reference
and a test asserting agreement within a stated tolerance. This matters more
here than in most catalogs: the library takes no numerical compatibility with
OpenCV — own names, own parameters, own numerics — so nothing external supplies
that proof, and UR-QA-2 only shows that tiling does not change a result, not
that the result is right.

| Operation | Reference kind |
|---|---|
| Colour conversion | Analytic, round-trip within documented tolerance |
| Stain deconvolution | Synthetic image composed from known vectors; recovers component intensities |
| Gaussian, Sobel, Scharr, Laplacian | Analytic response to synthetic input |
| Median, bilateral | Published definition, small-image reference |
| Canny | Published definition; OpenCV's `Canny` on one tile; tiling within the declared hysteresis reach |
| Morphology | Analytic on synthetic binary shapes |
| Otsu | Full in-memory histogram of the same slide |
| Connected components | Synthetic objects of known count and shape |
| Measurements | Analytic values for synthetic objects of known dimensions |
| Tissue detect | Labelled test corpus, area within documented tolerance |
| Watershed | Synthetic touching discs of known centres and radii; recovered count and boundary within tolerance |

The operations that exist state their reference through
`OperationCatalog::correctnessReference` and are tested to these tolerances:

| Operation | Reference | Tolerance the tests assert |
|---|---|---|
| `pixel.colour_convert` | Analytic: IEC 61966-2-1 sRGB, CIE 1976 L\*a\*b\* with D65 white, hexcone HSV, ITU-R BT.601 luma | Known values within 10⁻³ (pure red is L\*a\*b\* 53.2408, 80.0925, 67.2032; white is 100, 0, 0; HSV of pure red is 0, 1, 1) and BT.601 grey within 10⁻⁴; RGB → HSV → RGB and RGB → Lab → RGB round trips over a 512 × 512 grid of `Byte` colours within 10⁻³ on the 0 to 255 scale; tiled output bitwise equal to one tile |
| `pixel.stain_deconvolution` | Ruifrok and Johnston, *Anal Quant Cytol Histol* 23:291–299 (2001); synthetic images composed from known vectors | Amounts 0 to 1 recovered within 0.01 for H&E and H-DAB wherever every channel's intensity is at least 64 (darker pixels are limited by the input's 8-bit quantisation); white within 10⁻⁶ of zero; a three-stain pixel within 0.01 |
| `pixel.threshold_fixed` | Analytic: foreground exactly when strictly above (or below) the value; NaN background | Exact, on every type, including `Int64` around 2⁵³; tiled output bitwise equal to one tile |
| `pixel.threshold_otsu` | Otsu, *IEEE Trans SMC* 9:62–66 (1979); a full in-memory histogram of the same slide | Exact: the same threshold bin and output at 1, 4 and 8 workers and tile sizes 64, 100 and 257, and under a restriction the bins of only the admitted pixels |
| `pixel.threshold_adaptive` | Analytic: window mean (or Gaussian-weighted mean) minus the offset, against a direct per-pixel computation | Exact: a `Mean` step edge analytically, `Gaussian` on random `Float32` against the direct computation; tiled output bitwise equal to one tile for both methods on `Float32` and `UInt16` |
| `pixel.convert_type` | Analytic: the documented rounding and overflow rules at each type's minimum, maximum and overflow | Exact bit patterns, including ties to even and the `Float16` limit |
| `pixel.gaussian_blur` | Analytic: a normalised Gaussian kernel's response to constant, linear and impulse images | A constant image unchanged exactly and a linear ramp reproduced within 10⁻⁹, away from the level's edge; an impulse gives the outer product of the 1-D kernel within 10⁻¹⁵; random `Float64` equal bit for bit to a direct per-pixel computation in the stated order; size 1 the identity; tiled output bitwise equal to one tile on `Float32` and `UInt16` |
| `pixel.sobel`, `pixel.scharr`, `pixel.laplacian` | Analytic: responses to polynomial images; the coefficients of OpenCV's `getDerivKernels` (Scharr [3 10 3] × [−1 0 1]) | Exact: Sobel 8 on x, y and x², Sobel of size 1 2 on x, Scharr 32 on x, Laplacian of size 1 4 on x² + y² and of size 3 8 on x²; the impulse response equals the coefficients' outer product, which equals OpenCV's `Sobel`, `Scharr` and `Laplacian` exactly, for every order and size; tiled output bitwise equal to one tile |
| `pixel.median_blur` | Published definition: the middle value of the sorted window, against a direct sort | Exact: equal to the direct sort on random `Byte`, `UInt16`, `Float32` and full-range `Int64`; `Byte` through OpenCV equal to `UInt16` through the module's own selection; a constant sample kept bit for bit in all eleven types; NaN above every number; tiled output bitwise equal to one tile on `Byte` and `Float32` |
| `pixel.bilateral_filter` | Tomasi and Manduchi, *ICCV* 1998; a direct per-pixel computation | Exact: `Float64` equal bit for bit to the direct computation and `Float32` to its rounding, also with NaN neighbours left out of both sums; an infinite centre kept and its finite neighbours exact; a 0/200 step edge kept within 10⁻⁶ and the neighbours of a NaN within 10⁻¹² of a constant; tiled output bitwise equal to one tile on `Byte` and on `Float32` with NaNs, and a column shift giving the same bits on `Float32` and `Float64` |
| `pixel.morph_erode`, `pixel.morph_dilate`, `pixel.morph_open`, `pixel.morph_close` | Analytic: erosion and dilation of synthetic binary shapes by known elements | Exact: squares eroded, dilated, opened and closed to their analytic shapes in all five types; on NaN-free input equal to OpenCV's `morphologyEx` on the zero-padded image in all five types; a NaN in the window gives NaN; the level's edge erodes inward; tiled output bitwise equal to one tile on `Float32` and `UInt16`, and on `Float32` with NaNs and signed zeros, and a column shift giving the same bits on `Float32` and `Float64` |
| `pixel.canny` | Canny, *IEEE TPAMI* 8:679–698 (1986); OpenCV's `Canny` on one tile | Exact on one tile against `cv::Canny` of the zero-padded level, at apertures 3, 5 and 7; tiled output equal to one tile where every edge is strong; on a weak chain longer than the reach, tiling only drops pixels, each further than 32 pixels from every strong one; the complexity note states the bound verbatim |

Exact agreement is asserted wherever the computation is defined in a fixed
order, and a tolerance only where the reference is a published constant, the
input is quantised, or the expected value is an analytic one computed by
another route.

## 6.10 Scope of v1 — the catalog, closed

**The v1 catalog is twenty-two operations.** This resolves open question 7 in
`requirements.md`, which the requirements call the choice that decides the size
of v1 more than any other.

| Name | Section | Arity | Halo |
|---|---|---|---|
| `pixel.colour_convert` | 6.3 | Joint, uniform types | none |
| `pixel.stain_deconvolution` | 6.3 | Joint, uniform types | none |
| `pixel.gaussian_blur` | 6.4 | PerChannel | `kernelSize`/2, of the derived size when 0 |
| `pixel.median_blur` | 6.4 | PerChannel | `kernelSize`/2 |
| `pixel.sobel` | 6.4 | PerChannel | max(1, `kernelSize`/2) |
| `pixel.scharr` | 6.4 | PerChannel | 1 |
| `pixel.laplacian` | 6.4 | PerChannel | max(1, `kernelSize`/2) |
| `pixel.canny` | 6.4 | PerChannel | `apertureSize`/2 + 1 + 32, **bounded approximation** |
| `pixel.bilateral_filter` | 6.4 | PerChannel | `diameter`/2 |
| `pixel.morph_erode` | 6.4 | PerChannel | `iterations`·(`elementWidth`/2) left and right, `iterations`·(`elementHeight`/2) top and bottom |
| `pixel.morph_dilate` | 6.4 | PerChannel | as `pixel.morph_erode` |
| `pixel.morph_open` | 6.4 | PerChannel | twice `pixel.morph_erode`'s |
| `pixel.morph_close` | 6.4 | PerChannel | twice `pixel.morph_erode`'s |
| `pixel.threshold_fixed` | 6.5 | PerChannel | none |
| `pixel.threshold_otsu` | 6.5 | PerChannel, global context | none |
| `pixel.threshold_adaptive` | 6.5 | PerChannel | blockSize/2 |
| `pixel.convert_type` | 6.6 | PerChannel | none |
| `segment.tissue_detect` | 6.7 | Joint | small |
| `segment.connected_components` | 6.7 | PerChannel | 1 |
| `segment.watershed` | 6.7 | PerChannel | max marker spacing |
| `segment.contours` | 6.7 | PerChannel | 1 |
| `measure.object_measurements` | 6.8 | PerChannel over intensity channels | none |

Ten of those names are assigned here for the first time — the filter and
morphology entries of 6.4, which the earlier drafts gave typed factories but no
catalog name. The names are durable contract rather than documentation: a stage
records the name in `configurationJson` and replays it (UR-STG-4, 6.2), so
`pixel.morph_open` is what a recorded configuration will contain for the rest
of the major version. `morph_` prefixes all four morphology entries because
`pixel.open` and `pixel.close` read as file operations in a configuration file
and sort away from their siblings.

**The list is the requirements, exhausted — not a selection from them.** Every
Must and Should in `requirements.md` §7 and §8 maps to exactly one entry or to
a framework mechanism, and nothing here exists without such a mandate:

| Requirement | Satisfied by |
|---|---|
| UR-PIX-1 colour conversion (RGB, grey, HSV, Lab) | `pixel.colour_convert` |
| UR-PIX-2 stain deconvolution, H&E and H-DAB built in | `pixel.stain_deconvolution`, `StainVectors` |
| UR-PIX-3 Gaussian, median, Sobel, Scharr, Laplacian, Canny, bilateral | the seven filter entries |
| UR-PIX-4 erode, dilate, open, close, configurable element | the four `morph_` entries |
| UR-PIX-5 fixed, Otsu, adaptive thresholding | the three `threshold_` entries |
| UR-PIX-6 composition | `pipeline`, not an operation |
| UR-IMG-13 explicit type conversion | `pixel.convert_type` |
| UR-SEG-1 tissue versus background | `segment.tissue_detect` |
| UR-SEG-2 connected components | `segment.connected_components` |
| UR-SEG-3 seam merging | `SeamMerger`, not an operation |
| UR-SEG-4 contours with holes | `segment.contours` |
| UR-SEG-5 per-object measurements | `measure.object_measurements` |
| UR-SEG-6 calibrated units | `sceneIsCalibrated`, `scenePixelSizeMicrons` |
| UR-SEG-7 filtering during extraction | `minimumAreaPx` / `maximumAreaPx` on 6.7 |
| UR-SEG-8 separation of touching objects | `segment.watershed` |
| UR-SEG-9 scale-controlled region segmentation | **nothing in v1** — see below |

That the two columns exhausted each other was the actual answer to question 7.
The question asked what the catalog contains; the requirements had already
said, in fifteen Musts and Shoulds, and the interfaces of 6.3 to 6.8 had
already implemented every one.

**One row no longer holds, and it is the last one.** UR-SEG-9 was added to
`requirements.md` in version 0.11, after this section was written and after
question 7 was closed. The catalog is unchanged at twenty-two operations, so
the mapping is now sixteen requirements against twenty-two entries with one
requirement unanswered. The closure of question 7 survives as a *decision*
rather than as a derivation from the requirements, which is a weaker thing and
is recorded here as such.

**One of those fifteen exists because this table was built.** An earlier draft
of it had `segment.watershed` in the right-hand column with nothing on the
left: UR-SEG-2 requires connected-component labelling, which merges whatever
touches, and no requirement asked for the separation that makes a nuclear count
a count of nuclei. The operation was justified by the product hole and by
nothing in the requirements, which is the traceability gap the annotation store
had. It was closed by adding UR-SEG-8 (requirements 0.6) rather than by
accepting a hole in the mapping — and the gap was visible only because the
mapping was built in both directions.

### In v1: watershed, marker-controlled only

An earlier draft of this section excluded watershed, on the argument that
flooding a basin needs the whole connected component and a component spanning a
seam is not whole until `SeamMerger` has joined it. That argument is sound for
**unseeded** watershed and wrong for the **marker-controlled** variant, which is
the one worth shipping: basin reach is bounded by marker spacing rather than by
component size, so a clump of nuclei is segmented inside an ordinary halo no
matter how large the sheet containing it. 6.7 carries the full reasoning and
the `maximumMarkerSpacingPx` bound that makes it a contract rather than a hope.

Unseeded watershed remains out of v1, and not as a deferral — it does not fit
this execution model and would need a post-merge pass over written objects,
which is the multi-pass stage capability the design does not have
(`04-workspace.md` 4.2: a stage is one `PipelineSpec`).

What that leaves genuinely unsegmentable in v1 is narrow: a region whose
separating feature is gone at every level coarse enough for the slide to fit in
memory, *and* which exceeds any practical halo. A confluent sheet treated as a
single region is the honest example. It is rare enough to defer and specific
enough to document.

### Not in v1: scale-controlled region segmentation (UR-SEG-9)

The only Must or Should in `requirements.md` §7 or §8 with no catalog entry.
Unlike superpixels below, it is not unclaimed territory — a requirement names
it, so this is a gap rather than an omission.

It is out of v1 for sequencing, not for feasibility.
`superpowers/specs/2026-10-11-multiresolution-segmentation.md` assessed
partition-seeded Baatz & Schäpe region merging and found it needs **no new
execution model**: the fusion factor depends only on additive per-segment
aggregates, so the growth phase touches no pixels and the existing
global-context contract of 05 §5.1 carries it at level 0, within the memory
ceiling, inside UR-EXE-8's budget. What it does need first is in §8 and §11 of
that note — the global-context contract declares no accumulator memory cost,
the `Σ²` accumulator type is undecided for the wider input types and bears on
UR-QA-3, and the relationship between seed scale and scale parameter is
unresolved and is the main thing blocking an API.

Three ways out, and the choice belongs to whoever sets v1's scope: ship
`segment.multiresolution` and make the catalog twenty-three; downgrade UR-SEG-9
to a *May*; or leave it a *Should* against a later release and say so in the
release notes. Leaving it unstated is the only wrong answer, because a Should
with nothing behind it reads as an oversight rather than a plan.

### Not in v1: superpixels

Omitted, with a weaker argument and a cleaner conscience. Nothing in the
requirements mandates superpixel segmentation — no UR names it — so leaving it
out breaks no commitment. Primitive layers remain fully supported by the store
and the workspace (UR-STG-12, UR-RES-16); v1 simply ships no algorithm that
populates one, and an external tool importing primitives through GeoJSON
(`03-storage.md` 3.11) reaches them anyway.

Unlike watershed it has no architectural obstacle — superpixel seeding is grid
local and its boundaries are arbitrary by nature, so seam artefacts degrade
quality rather than correctness. It is out because nothing asked for it, which
is the right reason to leave something out of a first release.

### Built-in stain vectors: H&E and H-DAB only

UR-PIX-2 requires "at least H&E and H-DAB", and v1 ships exactly those two.
Each additional built-in vector is a set of published coefficients the library
would be asserting as correct, with a citation to maintain and a correctness
test to carry (UR-ALG-6), and `StainVectors::setVector` already covers every
other stain at no cost to the caller. The floor is the ceiling until someone
brings a specific stain and a reference for it.

### Why closing it now costs nothing later

The catalog is a leaf behind a registry (6.1): adding an entry in v1.1 changes
no other module and no stored artifact. What closing it buys is the thing a
catalog-shaped product most needs and most easily loses — a v1 whose contents
are a decision with reasons, rather than whatever the interfaces happened to
accumulate.
