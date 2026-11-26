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
so a build can ship a subset. The first registers the four `segment.*`
operations of 6.7. The second registers nothing: the segmentation operations
measure their own objects (6.8), so the catalog has no measurement operation,
and the function stays so that a later one has a place to go without a change
to the API; its documentation says so.

**Build.** The module is the OBJECT library `slideio-processor-algorithm`,
linked into `slideio-processor-engine` (`05-pipeline.md` 5.11). Its include
path is `include/`, SlideIO's headers and `src/algorithm/` as the root of its
own `detail/` headers, and deliberately not `src/`, so it cannot reach
`pipeline`'s private headers and is compiled against the public API only, as
a third-party operation would be (UR-EXT-2). It links `opencv_core` and
`opencv_imgproc` privately: OpenCV is confined to the engine library, not to
`pipeline`, because the catalog's median, morphology and Canny and its
derivative coefficients (6.4) need it. The segmentation operations of 6.7 do
not: their labelling, tracing, flooding and closing are the module's own,
because each must give one answer however the level is tiled. No
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
| `pixel.gaussian_blur` | `sigma` | double | 1.0 | finite, at least 10⁻⁶; with `kernelSize` 0, small enough that the derived size 2·⌈3·`sigma`⌉ + 1 is at most 1025 |
| | `kernelSize` | int | 0 | odd, 1 to 1025, or 0 to derive it from `sigma` |
| `pixel.median_blur` | `kernelSize` | int | 3 | odd, 3 to 255 |
| `pixel.distance_transform` | `maximumDistancePx` | int | 32 | 1 to 4096 |
| `pixel.fill_holes` | `maximumHoleSizePx` | int | 32 | 1 to 4096 |
| | `maximumHoleAreaPx` | int | absent (no area limit) | at least 1 when given |
| `pixel.sobel` | `dx` | int | 1 | 0, 1 or 2; `dx` + `dy` at least 1 |
| | `dy` | int | 0 | 0, 1 or 2 |
| | `kernelSize` | int | 3 | 1, 3, 5 or 7 |
| `pixel.scharr` | `dx`, `dy` | int | 1, 0 | (1, 0) or (0, 1) |
| `pixel.laplacian` | `kernelSize` | int | 1 | 1, 3, 5 or 7 |
| `pixel.canny` | `lowThreshold` | double | 50.0 | finite, at least 0, at most `highThreshold` |
| | `highThreshold` | double | 150.0 | finite, at least 0 |
| | `apertureSize` | int | 3 | 3, 5 or 7 |
| `pixel.bilateral_filter` | `diameter` | int | 5 | odd, 3 to 255 |
| | `sigmaColour` | double | 25.0 | finite, at least 10⁻⁶ |
| | `sigmaSpace` | double | 5.0 | finite, at least 10⁻⁶ |
| `pixel.morph_erode`, `pixel.morph_dilate`, `pixel.morph_open`, `pixel.morph_close` | `shape` | string | `"Rectangle"` | a `StructuringShape` spelling, or `"Custom"` for an element built by `StructuringElement::custom` |
| | `elementWidth`, `elementHeight` | int | 3, 3 | odd, 1 to 255 |
| | `elementMask` | double array | absent | present exactly when `shape` is `"Custom"`: `elementWidth` · `elementHeight` values, row by row from the top, each 0 or 1, at least one 1 |
| | `iterations` | int | 1 | 1 to 64 |
| `segment.connected_components` | `connectivity` | string | `"EightConnected"` | a `Connectivity` spelling |
| | `minimumAreaPx` | int | 0 | at least 0; square level-0 pixels |
| | `maximumAreaPx` | int | 0 (unbounded) | at least 0; when not 0, at least `minimumAreaPx` |
| | `extractHoles` | bool | true | |
| | `measurements` | string | every `MeasurementKind` | `MeasurementKind` spellings separated by commas, without spaces (`"Area,Perimeter"`); empty selects none |
| | `targetLayer` | string | absent: valid only when exactly one layer is bound | the name of a bound layer |
| `segment.contours` | `includeHoles` | bool | true | |
| | `targetLayer` | string | absent: valid only when exactly one layer is bound | the name of a bound layer |
| `segment.watershed` | `markers` | string | `"LocalMaxima"` | a `WatershedMarkers` spelling |
| | `auxiliaryName` | string | `""` | with `"AuxiliaryRaster"`, not empty: the name of a bound raster |
| | `minimumMarkerDistancePx` | int | 5 | 1 to `maximumMarkerSpacingPx` |
| | `maximumMarkerSpacingPx` | int | 32 | 1 to 1073741823, so the halo 2 · spacing + 1 fits an `int32_t` |
| | `minimumAreaPx` | int | 0 | at least 0; square level-0 pixels |
| | `measurements` | string | every `MeasurementKind` | as `segment.connected_components` |
| | `targetLayer` | string | absent: valid only when exactly one layer is bound | the name of a bound layer |
| | `minimumMarkerValue` | double | absent: every local maximum is a marker | finite; only with `"LocalMaxima"`; absent rather than a default so that existing recorded configurations are unchanged |
| `segment.tissue_detect` | `mode` | string | `"Brightfield"` | `"Brightfield"` or `"Fluorescence"`; recorded only in fluorescence mode, so a brightfield operation's bag is what it was before the mode existed |
| | `saturationThreshold` | double | 0.07 | 0 to 1; brightfield only, refused in fluorescence mode |
| | `thresholdMethod` | string | `"Triangle"` in fluorescence mode | `"Triangle"` or `"Fixed"`; fluorescence only, refused in brightfield mode |
| | `intensityThreshold` | double | absent | finite; required with `"Fixed"`, refused with `"Triangle"` and in brightfield mode |
| | `ignoreZeros` | bool | true with `"Triangle"` | recorded with fluorescence and `"Triangle"` only, always, so a Triangle bag is complete; refused with `"Fixed"` and in brightfield mode |
| | `minimumRegionAreaPx` | int | 0 | at least 0; square level-0 pixels |
| | `closingRadiusPx` | int | 2 | 0 to 127; 0 for no closing |
| | `targetLayer` | string | absent: valid only when exactly one layer is bound | the name of a bound layer |

`pixel.threshold_otsu`'s range is the one exception to completeness: the three
range keys are present only when the operation was created with a range, since
"no range" has no value to fill in (6.5). Its bag still round-trips, which is
what the conformance test checks. An object producer's `targetLayer` is the
other: the typed setter given `""`, the default, leaves the key out of the bag,
and an absent key means the run's only bound layer (6.7, `05-pipeline.md` 5.5),
so a one-layer stage records no layer name at all. A `measurements` text naming
an unknown kind is a value out of range and is refused naming the key; the bag
records the selected kinds in enumerator order, so a selection round-trips
whatever order it was given in. A morphology operation's `elementMask` is the
same kind of key: a drawn shape is fully described by its name and size, so the
mask is recorded only for `"Custom"`, and a bag that gives it with another
shape, or omits it with `"Custom"`, is refused naming `elementMask`.

A range that couples two keys is reported against the key the caller set.
`pixel.gaussian_blur` with `kernelSize` 0 and a `sigma` whose derived size
would exceed 1025 is refused naming `sigma`, not `kernelSize`: the limit
belongs to the derived size, and `sigma` is what produced it. A Canny
`lowThreshold` above `highThreshold` is refused naming `lowThreshold`, and a
Sobel or Scharr pair of orders that selects no derivative names `dx`.
`StructuringElement`'s constructor and `custom` refuse a bad shape, size or
mask with `InvalidArgumentError` naming the parameter (`shape`, `width`,
`height`, `mask`) and
`StructuringElement`, since no operation exists yet; a morphology factory
given a moved-from element refuses it naming the parameter `element`, rather
than reading an element that holds nothing.

Every factory's operation declares its channel arity, supported types, halo,
correctness reference and complexity note (UR-ALG-3, UR-ALG-4, UR-ALG-6,
UR-ALG-8). Algorithms of the same kind take comparable parameters in a
comparable order, and report errors, progress and cancellation through the same
mechanisms (UR-ALG-7) — which is an API review against a documented convention
applied to the whole catalog, not per algorithm.

**Working sets.** Every built-in also declares its working set (5.1): the
memory one `apply()` or `accumulate()` call allocates beyond the input and
output blocks the framework supplies, as bytes per pixel of the rect the call
is given (`workingSetBytesPerLevelPixel()`) and bytes per call
(`workingSetBytesPerTile()`). The pipeline reserves the larger of its
operations' working sets with each tile (5.5), so these figures are what keep
a run inside its memory ceiling. Both are computed from the instance's
parameters, so the factory's figures are those of the default parameters and
the instance is authoritative; the parameter tables above need no change. The
declarations are upper bounds derived from the code, each with a comment
beside it saying which buffers it counts:

| Operations | Per pixel | Per call |
|---|---|---|
| `pixel.colour_convert`, `pixel.stain_deconvolution`, `pixel.convert_type`, `pixel.threshold_fixed` | 0: they write straight into the output | 0: row pointers, tables and bounds live on the stack |
| `pixel.threshold_adaptive` | the window sums and the separable box filter's row and ring of row sums | the allocator's overhead on those buffers |
| `pixel.threshold_otsu` | `accumulate()` packs the tile's admitted samples into one row, at most 8 bytes per pixel; `apply()` allocates nothing | one channel's histogram and its local copy (16 bytes per bin; without a range, the largest per-value histogram of 65,536 bins) and its bookkeeping, whatever the number of channels |
| `pixel.gaussian_blur`, `pixel.sobel`, `pixel.scharr`, `pixel.laplacian` | the tile's values as doubles (and one more pass's for a derivative that sums two) and the separable filter's row of samples and ring of row sums, one pass at a time | the allocator's overhead on those buffers |
| `pixel.median_blur` | one sort key per pixel on the module's own path; the backend's destination and bordered copy on the `Byte` path | the larger of the window's copy (grows with the kernel's area) and the backend's per-column histograms on the `Byte` path |
| `pixel.distance_transform` | the module's own two passes, all metered: the column distances of the tile's rows as `uint16`, at most 2 bytes per pixel of the halo'd rect, and two rows of the rect's width (the running column distances, `uint16`, and the envelope's columns, `int64`), at most 10 / (2 · `maximumDistancePx` + 1) bytes per pixel; no backend buffer | the three vectors' allocator blocks |
| `pixel.fill_holes` | the module's own labelling, all metered: per pixel of the halo'd rect the `Byte` state mask (1) and the queue of one component's pixel indices as `int32` (4), 5 bytes; no backend buffer | the two vectors' allocator blocks |
| `pixel.bilateral_filter` | the plane as doubles and the tile's values | the allocator's overhead on those buffers |
| `pixel.canny` | the edge and suppression maps, the derivatives of every row, the one stack of edge pixels and the backend's magnitude and Sobel rows, all from the backend's single stripe of rows | the kernel-sized parts of the magnitude rows and of Sobel's filter, the kernels and bookkeeping, whatever the tile |
| `pixel.morph_erode`, `pixel.morph_dilate`, `pixel.morph_open`, `pixel.morph_close` | the larger of the float path's two copies of the plane and the integer path's ring buffer, rows and the backend's destination | the larger of the float path's element offsets and the integer path's filter engine (grows with the element's size) |
| `segment.connected_components` | 192: the union-find parents, the traced rings, the region table and the objects emitted inside the call (the tile filter's and the finisher's copies included), one intensity partial per object and measured plane for at most three measured planes | 16384: one object's record and its finished copy with every measurement, and the small arrays of a tile with few objects |
| `segment.contours` | 264: as connected components, without partials, but regions of different values touch side by side, so the traced arrays and a join key per region can reach one per pixel | 4096 |
| `segment.watershed` | 288 per pixel of the halo'd tile: plane 0 as doubles, each pixel's seed and the flood's queue, then the labelling, tracing and emission of contours with three measured planes | 16384 |
| `segment.tissue_detect` | 200 per pixel of the halo'd tile: the closing's two distance maps, then one label per tile pixel and the tracing and emission of the regions; fluorescence adds nothing, the mask being computed straight into the same buffer | 4096; with Triangle, 4096 + 2 · 65536 · 8 + 2 · 64 (the histogram's 65536 bins of 8 bytes and the allocator's overhead on them, counted twice: `accumulate()` keeps no local copy, so the second is a deliberate, conservative margin, which also covers `finalizeAccumulation()`'s reduced copy of a stepped `UInt16` histogram, at most 32768 bins). A Triangle instance's accumulated state, whatever the input type, is 65536 · 8 + 64 bytes of histogram, the instance, and 4096 + 128 · 4 + 64 bytes of bookkeeping; brightfield and Fixed instances declare 0 accumulated state and need no global context |

The catalog's parameter-free figures for `segment.tissue_detect` (`accumulatedStateBytes`, `workingSetBytesPerTile`, the supported types and the plane count) describe the default, brightfield operation; a run uses the figures of its own instance.

**The backend's share is bounded from its source.** OpenCV allocates part of
its memory through its own allocator, which no test in this module can count.
An operation that calls OpenCV — the median's `Byte` path, Canny, the integer
morphology path — declares OpenCV's internal buffers analytically, from the
OpenCV 4.14 source, and cites the file and function beside the declaration.
OpenCV runs on the calling thread (6.4), so the figures are those of its
sequential paths: one stripe of rows in `cv::Canny`, and no per-thread tiles
in the filter engine behind Sobel and morphology. No figure depends on the
host's core count. The figures are read once, before a run.

**The module's own allocations are measured.** A separate test executable,
which compiles the pipeline and algorithm code in rather than linking the
engine library, replaces the global `operator new` and `operator delete` with a
counting pair that keeps per-thread live and peak bytes while a meter is
active, counting the meter's own thread only. For every built-in, at
representative and upper-limit parameters and on the types it supports, a test
applies one call under the meter and asserts that the measured peak is at most
the halo'd rect's area × the per-pixel figure + the per-call figure. For the
OpenCV paths the analytic OpenCV share is subtracted from the declaration
before the comparison, so the test proves the module's own memory, and the
OpenCV share stays documented rather than measured. OpenCV allocates on the
calling thread alone, so the meter sees everything it takes through
`operator new`. A further test bounds Canny's declaration from above as well:
at a 1024-pixel tile with the default parameters, on an input dense with
edges, it is at most three times the measured peak plus the OpenCV share.
A segmentation operation's objects are filtered, normalised and finished
inside its `apply()` call (`05-pipeline.md` 5.1, 5.5), so the copies the tile
filter and the finisher make count toward its figures, and the meter, which
for these operations also compiles in the core library, sees them. Its tests
apply each segmenter to adversarial inputs — checkerboards, a hole at every
other pixel, every pixel its own label or basin — as well as representative
ones. Further
tests refuse a run whose working set does not fit the ceiling (a bilateral
filter on a large tile, Otsu with a very large bin count) and bound Otsu over
many channels.

The working sets are per call. What a global-context operation's instance holds
between calls is its accumulated state, declared separately with
`accumulatedStateBytes(planeCount)` (5.1). Otsu's is, per plane, one histogram
of 8 bytes per bin (the largest per-value histogram of 65,536 bins when there is
no range), the allocator's overhead on its bin array and a fixed bookkeeping
share for the channel's map node, its `Histogram64` and that histogram's state;
plus the instance's own object, its parameter bag and its control block. A tile
of `planeCount` planes has at most that many channels, and combining adds a
channel only when it is new, so the figure bounds a per-tile instance after
`accumulate()` and the run's instance after `combine()` alike. The pipeline
reserves it for the run's instance and for the per-tile instances the fold may
hold, and refuses a run whose tile, states and one more instance do not fit the
ceiling (5.5, 5.8, 5.9). A test measures the instances' live bytes under the
meter against the declaration, for a `Byte`, a `UInt16` and a ranged `Float32`
input.

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
OperationPtr makeDistanceTransform(int32_t maximumDistancePx);
OperationPtr makeFillHoles(int32_t maximumHoleSizePx,
                           int64_t maximumHoleAreaPx = 0); // 0: no area limit
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
so OpenCV's own border rule never reaches an output pixel.

**OpenCV runs single-threaded.** The engine links its own OpenCV privately
and makes it sequential (`cv::setNumThreads(0)`, once per process, through the
private `detail::useSequentialOpenCv`) at the start of each `apply()` of an
operation that calls it: Canny, the four morphology operations and the median.
Never when an operation is built: the built-in factories build a default
operation while the library loads (6.1), when OpenCV's own statics may not
exist yet. The pipeline's tile
workers (5.5) are then the only parallelism: OpenCV's threads would
oversubscribe the cores those workers already occupy, make Canny's and
morphology's working sets depend on the host's core count, and allocate where
the memory accounting cannot see. A lazy read of a single tile is therefore not
parallelised inside OpenCV either. No result depends on the thread count: a
lazy read after an eager run on the same engine gives the same bits.

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

**Non-finite samples in the sums.** In the Gaussian, Sobel, Scharr and
Laplacian a NaN in a pixel's window makes it NaN; an infinity makes it +∞ or
−∞, or NaN where it meets a zero coefficient or an infinity of the opposite
sign. They are plain weighted sums, and nothing is skipped. The bilateral
filter, whose rule differs, is described below.

**Kernels, sizes and halos.** Every halo is the same on all four sides except
morphology's, which follows the element's two axes.

- **Gaussian.** Weights exp(−d²/2σ²) for d from −`kernelSize`/2 to
  `kernelSize`/2, normalised to sum 1, the same kernel along both axes.
  `kernelSize` 0 derives 2·⌈3·`sigma`⌉ + 1, at most 1025, and size 1 leaves
  every sample unchanged apart from the conversion to the output type. Halo
  `kernelSize`/2 of the size used. `sigma` is at least 10⁻⁶: below that
  2σ² can underflow to 0 and every weight become 0/0, while at it every
  off-centre weight is already 0, so nothing smaller is needed.
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
  `sigmaColour` and `sigmaSpace` are at least 10⁻⁶, for the Gaussian's
  reason. Halo `diameter`/2.
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
  reach declared for hysteresis, below. OpenCV converts each threshold to
  `int`, which is undefined beyond `int`'s range, so a threshold above 10⁷ is
  passed to it as 10⁷: that, and 10⁷/16 at aperture 7, already exceeds every
  L1 magnitude of two 16-bit gradients, so the result is unchanged. The
  parameters keep the value given.

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
32 pixels, and may drop or keep weak edges beyond that reach"*. Tiling can
change a weak chain beyond that reach either way. A tile may lack the chain's
strong seed and drop it. And OpenCV computes its Sobel gradients with
`BORDER_REPLICATE` inside the halo'd plane, so in the outer
`apertureSize`/2 + 1 rows and columns of the halo a pixel can get a gradient
the whole level would not give it, become a strong seed the level does not
have, and keep a weak chain one tile covering the level drops. The tile
pixels themselves, whose gradients lie wholly inside the plane, are never
seeds of that kind. UR-PIX-3's acceptance criterion — "whole-slide output
matches a single-tile reference" — is stated without tolerance and cannot be
met unconditionally by this one operation; that conflict stays recorded as
open question 15 in `requirements.md` and needs a requirements amendment
rather than a silent reinterpretation here.

Everything else in 6.4 and 6.5 is genuinely local. Gaussian, median, Sobel,
Scharr, Laplacian, bilateral, the distance transform, hole filling and all four morphology operations depend on a
bounded neighbourhood, so the declared halo makes them exact, and Otsu is
global by construction rather than by accident (6.5).


### Distance transform

`pixel.distance_transform` is the input `segment.watershed` needs (6.7): a
`Float32` surface that peaks at the centre of each object. A pixel is foreground
when its sample is neither 0 nor NaN, the rule the watershed uses; the output is
0 on background and, on foreground, the Euclidean distance to the centre of the
nearest background pixel, clamped at `maximumDistancePx`. Pixels beyond the level
are zero (05 §5.5) and so are background, in a tiled run and a whole-level run
alike.

**Why it is exact at any tile size.** The halo is the clamp, *H* =
`maximumDistancePx`. A tile pixel whose true distance *d* is at most *H* has its
nearest background pixel within Chebyshev distance *H*, so inside the halo'd
context, and the context gives *d*. If *d* > *H*, every background pixel in the
context is further than *H* and the output is *H*; a context with no background
at all gives *H* as well. So the output is min(*d*, *H*) at every tile size.

**How it is computed.** The module's own two-pass squared distance transform of
Felzenszwalb and Huttenlocher, in integers throughout. Pass 1 gives, per column
of the halo'd context and for each of the tile's rows, the distance to the
nearest background pixel of that column, capped at *H* + 1 (a column whose
background is further than *H* cannot give a distance of at most *H*, so the cap
changes no output). Pass 2 takes, per tile row, the lower envelope of the
parabolas (*x* − *p*)² + *g*(*p*)² over the columns with *g*(*p*) ≤ *H*; it compares
the parabolas' breakpoints by cross-multiplication in `int64`, with no division
and no floating point. Columns more than 2 *H* + 1 apart share no pixel within
*H*, so each run of columns without such a gap is an envelope of its own; that
bounds the gap between two members of an envelope by 4 *H* + 1 and keeps every
product below about 3 · 10¹², far inside `int64`. The output is the square root,
in double, of the least integer squared distance, rounded to float, or *H* when
that squared distance is at least *H*², compared as integers. Nothing depends on a
pixel's position in its context, so tiled output is bitwise equal.
OpenCV's `distanceTransform` (`DIST_MASK_PRECISE`) was the first
implementation; the final review of the branch found that OpenCV 4.14.0 computes
the breakpoints in `float`, which picks the wrong parabola for long distances in
contexts thousands of pixels wide (an error of one to three in the squared
distance, depending on the context column, so on the tiling, from a maximum of
about 1024 up), and the module took the computation over.

### Hole filling

`pixel.fill_holes` closes the holes of a binary mask, the usual cleaning step
before the distance transform and the watershed. A pixel is foreground when its
sample is neither 0 nor NaN; the output is `Byte`, 255 for foreground and for
filled holes and 0 elsewhere, the convention of the thresholds. A **hole** is a
4-connected component of background (the complement of an 8-connected
foreground, the usual pair) that does not reach the level edge, whose bounding
box is at most `maximumHoleSizePx` on each side, and whose area is at most
`maximumHoleAreaPx` when that is given. A diagonal gap in a ring therefore does
not let the inside out.

**Why it is exact at any tile size.** The halo is the size bound, *S* =
`maximumHoleSizePx`. In the halo'd context, a background component that does not
touch the context's border is a whole component of the level, because it cannot
connect beyond the context without crossing that border. A component that does
touch the border is either larger than the bound or not decided here. For a tile
pixel in a hole of bounding box at most *S*, every pixel of the hole lies within
Chebyshev distance *S* - 1, so the hole lies inside the context clear of its
border and is found whole. A tile pixel in a larger background component sees it
touching the border or with a bounding box above *S*, and is not filled in
either case. Background beyond the level is zero (05 §5.5), so a region open to
the level edge joins the strip outside the level and touches the context's
border, which matches the whole-level pass. The rule is therefore exact.

**Implementation.** The module's own labelling, not OpenCV's: a breadth-first
search of each 4-connected background component of the halo'd context, over a
`Byte` state mask with one queue of `int32` indices for the component in hand,
recording its bounding box and area. A component that is enclosed and within the
bounds is marked as a hole, and the tile's rect is written from the mask. Every
buffer comes from `operator new`, so the working-set check measures all of it
(the table of 6.2): 5 bytes per pixel of the halo'd rect. The result is a set of
whole components and does not depend on the order of the search.
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

`makeOtsuThreshold` is a global-context operation, the first and the
reason `pipeline::Reduction` exists; the other is `segment.tissue_detect` in
fluorescence mode with the Triangle method (6.7), which takes the same route.
It returns `needsGlobalContext() == true`,
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

/// segment.connected_components — Joint, halo none, every type, one Int32 plane,
/// writes objects. Plane 0 is segmented; planes 1 to 3 are measured.
class ConnectedComponentsParameters {
public:
    ConnectedComponentsParameters();
    ConnectedComponentsParameters& connectivity(Connectivity);       // EightConnected
    ConnectedComponentsParameters& minimumAreaPx(int64_t);           // 0
    ConnectedComponentsParameters& maximumAreaPx(int64_t);           // 0 = unbounded
    ConnectedComponentsParameters& extractHoles(bool);               // true
    ConnectedComponentsParameters& measurements(const MeasurementSelection&); // standard()
    ConnectedComponentsParameters& targetLayer(const char*);         // "" = the only bound layer
};
OperationPtr makeConnectedComponents(const ConnectedComponentsParameters&);

/// segment.contours — PerChannel, halo none, integers up to 32 bits and Float32,
/// one Int32 plane per input plane, writes objects. Traces a label raster.
class ContourParameters {
public:
    ContourParameters();
    ContourParameters& includeHoles(bool);                           // true
    ContourParameters& targetLayer(const char*);
};
OperationPtr makeContourExtraction(const ContourParameters&);

enum class WatershedMarkers : int32_t {
    LocalMaxima,       // detected in plane 0
    AuxiliaryRaster    // one basin per distinct value other than -1 of a bound
                       // Int64 raster (5.1), normally LayerRasterContent::ObjectAnchor
};

/// segment.watershed — Joint, halo 2 * maximumMarkerSpacingPx + 1, every
/// type, one Int32 plane, writes objects. Plane 0 is flooded; planes 1 to 3
/// are measured.
class WatershedParameters {
public:
    WatershedParameters();
    WatershedParameters& markers(WatershedMarkers);                  // LocalMaxima
    WatershedParameters& auxiliaryName(const char*);                 // for AuxiliaryRaster
    WatershedParameters& minimumMarkerDistancePx(int32_t);           // 5
    WatershedParameters& minimumMarkerValue(double);                 // absent

    /// The bound that makes this operation tileable, and a hard one: every
    /// foreground pixel must lie within this distance of the marker pixel its
    /// basin reached it from. Derives the halo. Exceeding it is an error,
    /// never a silent approximation.
    WatershedParameters& maximumMarkerSpacingPx(int32_t);            // 32

    WatershedParameters& minimumAreaPx(int64_t);                     // 0
    WatershedParameters& measurements(const MeasurementSelection&);  // standard()
    WatershedParameters& targetLayer(const char*);
};
OperationPtr makeWatershed(const WatershedParameters&);

enum class TissueMode : int32_t { Brightfield, Fluorescence };
enum class TissueThresholdMethod : int32_t { Triangle, Fixed };

/// segment.tissue_detect — Joint over planes of one type, Byte, UInt16 or
/// Float32; brightfield: exactly three (RGB); fluorescence: one or more.
/// Halo 2 * closingRadiusPx + 1, one Byte plane, writes objects.
class TissueDetectParameters {
public:
    TissueDetectParameters();
    TissueDetectParameters& mode(TissueMode);                        // Brightfield
    TissueDetectParameters& thresholdMethod(TissueThresholdMethod);  // Triangle, fluorescence only
    TissueDetectParameters& intensityThreshold(double);              // required with Fixed
    TissueDetectParameters& saturationThreshold(double);             // 0.07, brightfield only
    TissueDetectParameters& minimumRegionAreaPx(int64_t);            // 0
    TissueDetectParameters& closingRadiusPx(int32_t);                // 2, 0 = none
    TissueDetectParameters& targetLayer(const char*);
};
OperationPtr makeTissueDetect(const TissueDetectParameters&);
```

Each parameters class is pimpl'd with the full copy and move set; a
moved-from object may only be assigned to or destroyed, and a setter or
factory given one throws `InvalidArgumentError`.

**Requirements:** UR-SEG-1, UR-SEG-2, UR-SEG-3, UR-SEG-4, UR-SEG-5, UR-SEG-6,
UR-SEG-7, UR-SEG-8, UR-STG-4, UR-STG-10.

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
one-layer stage need not repeat itself. The typed setter given `""` leaves the
key out of the bag rather than storing an empty string, so the empty name and
the absent key are one case (6.2). A stage declaring several layers must
name one per object-producing operation; omitting it there is an error, not a
guess (4.4). Two operations of one run may not target the same layer
(`05-pipeline.md` 5.5).

### One tracer, one finisher

The four operations share their structure, and so the guarantees below hold
for all of them alike.

**Foreground.** A pixel of plane 0 is foreground when its sample is neither 0
nor NaN; -0 is background, as 0 is, and an infinite sample is foreground. For
`segment.contours` a region is a maximal 4-connected set of pixels of one
**equal** foreground value, compared exactly (a `Float32` label and the next
`Float32` are two labels); for the others, a maximal connected set of
foreground pixels.

**Labelling and tracing.** Each operation labels the regions of its tile rect
at level coordinates — a two-pass union–find, regions numbered 1 to K in the
order their first pixel is met row by row — and one shared tracer follows each
region's boundary along pixel edges into rings: the outer ring positive
(clockwise on screen, y down), holes negative. Where a region's outline meets
itself diagonally at a point, the tracer takes the sharpest left turn for an
8-connected region, so that one outer ring passes twice through the point,
and the sharpest right turn otherwise — the rule the seam merger applies to
merged objects (`05-pipeline.md` 5.7), so an object found in one tile and the
same object merged agree point for point. The framework normalises the rings
of every emitted object (`05-pipeline.md` 5.5), so an operation need not; the
shared tracer already emits the normal form, which the framework recognises
and passes on without copying the polygon, a saving the working sets of 6.2
count on.

**Emission.** A region touching the tile rect's edge is emitted with
`emitSeamFragment` (`05-pipeline.md` 5.1) together with its intensity partials
and its join key; the rest are emitted as finished objects with
`applyStandardMeasurements` applied. Rings are mapped to level-0 coordinates
through `toLevel0` (UR-SEG-4). An operation author never writes merging logic:
`SeamMerger` joins the fragments into single objects with geometry and
measurements computed as if the slide had been processed as one image
(UR-SEG-3). The join keys, which agree across tiles, keep apart regions that
touch: `segment.connected_components` and `segment.tissue_detect` use 0,
since their regions are separated by background (or, for 8-connected
components, joined anyway); `segment.contours` uses the region's sample value,
its bit pattern as an `int64_t` for `Float32`; `segment.watershed` uses its
basin's identity (below).

**Finishing.** Every object, whole or merged, passes through its operation's
`finishObject` (`05-pipeline.md` 5.1), which applies one
`detail::MeasurementPlan`: without holes kept it removes the hole rings and
recomputes `area` and `perimeter` from the outer ring, intensity statistics
staying over the object's own pixels; it discards an object whose final `area`
lies outside the area bounds; it adds the centroid and bounding box when
selected and the calibrated units when the pixel size is known (6.8); and it
removes every measurement of a kind not selected. An operation that measures
nothing beyond the geometry (`segment.contours`, `segment.tissue_detect`) keeps
`area` and `perimeter` and their calibrated forms. A segmenter therefore
always finishes, and keeps the default `finishesObjects()` of true
(`05-pipeline.md` 5.1): whatever its parameters, its measurement plan writes or
strips the standard measurements — `area` is set when selected and removed
when not, so even an empty selection with no area bounds and holes kept
changes the record.

**Channels and measured intensity.** `segment.connected_components` and
`segment.watershed` are **Joint** with no uniform-type requirement: plane 0 is
segmented, and planes 1 onward are measured, each partial's channel being that
plane's source channel. They measure **at most three planes** besides plane 0,
because their working set is declared for three (6.2); validating a run that
gives them more fails with `InvalidArgumentError` "measures at most three
channels besides the one it segments", from `outputPlaneCount`
(`05-pipeline.md` 5.5). A NaN intensity sample is skipped, not counted; an
infinite one is counted. The measured samples are the **scene's**, as read,
not the segmenter's input planes: for each input plane from 1 on, the
segmenter measures the plane of `OperationContext::sourceInput()`
(`05-pipeline.md` 5.1) with the same source channel. A PerChannel pixel
operation ahead of it transforms planes 1 onward as well as plane 0, so in
blur → threshold →
`segment.connected_components` plane 0 is the thresholded mask while the
measured samples are the unblurred channels; `intensity.<c>.*` therefore
always describes channel c as it is in the scene, and a channel carried by
several input planes (after a Joint operation that writes more planes than it
reads, such as grey to RGB) is measured once. Keeping the samples as read
for the whole chain costs no reservation: the tile already reserves every
step's buffers at once (`05-pipeline.md` 5.5).
Derived planes, such as stain densities or HSV, are therefore never measured
in the chain that derives them: to measure them, derive them in an inner
pipeline and run the segmenter over its `Pipeline::asScene()` (`05-pipeline.md` 5.5), whose
samples they then are.
`segment.contours` is **PerChannel** and measures nothing; it runs on exactly
one channel, the label raster: per channel over several, every region would
be stored once per channel under one id, so validation refuses a run giving
it more than one plane (`05-pipeline.md` 5.5).
`segment.tissue_detect` is **Joint** over planes of one type, `Byte`, `UInt16`
or `Float32`. In brightfield mode it takes exactly three planes; in
fluorescence mode one or more. Another count throws `ChannelArityError`, the
error of the catalog's other RGB operations; the brightfield message points to
fluorescence mode.

**The raster output.** Every operation must write a raster, and for the
segmenters it is a view for inspection: one `Int32` plane holding each region
pixel's 1-based index among the regions of its tile, and 0 elsewhere — local
to the tile, not an object's identity. `segment.tissue_detect` writes one
`Byte` plane, 255 on closed tissue and 0 elsewhere.

**Nothing may grow the tile after them.** Each operation finds its seams on
its own tile rect, so a run in which an operation with a halo follows one of
these is refused (`05-pipeline.md` 5.5).

### The four operations

**`segment.connected_components`** labels the foreground of plane 0 under
`connectivity`, `EightConnected` by default because nuclei touch at corners
(UR-SEG-8); `objectsAreEightConnected()` follows it, so the merger joins
diagonal contacts across seams too. Halo 1. Holes are kept by default
(`extractHoles`).

**`segment.contours`** turns a label raster into objects: every region of one
value becomes an object with its rings, and regions of different values may
touch, sharing the edges between them. Its regions are 4-connected
(`objectsAreEightConnected()` is false): two regions of one value touching
only at a corner are two objects. Halo 1. It accepts every integer type up to
32 bits and `Float32`, whose values it compares exactly. Holes are kept by
default (`includeHoles`).

**`segment.watershed`** separates touching objects (UR-SEG-8); the next
subsection describes it.

**`segment.tissue_detect`** marks a pixel as tissue when its HSV saturation,
(max − min) / max over its three samples in any order, exceeds
`saturationThreshold`; a pixel whose samples are all 0, as pixels beyond the
level are, is background. The mask is then closed — dilated, then eroded —
with a Euclidean disc of radius `closingRadiusPx` in pixels of the analysis
level (the pixels within that distance of its centre; 0 for no closing). The
closing is the module's own and exact under tiling: the halo,
2 · `closingRadiusPx` + 1, holds every pixel a tile pixel's closed value
depends on. Each maximal 4-connected set of closed tissue pixels is an object,
traced as above; `minimumRegionAreaPx` filters in the finisher on level-0
area. One tissue region may span the level, and the merger holds its
fragments until it is complete (`05-pipeline.md` 5.7), so tissue detection
should run at a coarse `analysisLevel`; its documentation says so.

**Fluorescence mode.** The brightfield rule above is the default and is
unchanged; in brightfield mode a `Float32` pixel follows the same saturation
rule, and one whose maximum is not above 0, or is NaN, is background. With
`mode` set to `"Fluorescence"` the operation takes one or more planes of one
type (the pipeline's channel selection chooses them, for example DAPI alone),
and tissue shows as brightness, not colour:

- **The rule.** A pixel's signal is the maximum over its planes, NaN samples
  ignored; the pixel is tissue when the signal is strictly above the cut-off.
  Pixels beyond the level are background whatever the cut-off, so a negative
  cut-off cannot turn them into tissue, and a pixel whose samples are all NaN
  is background. The closing, the labelling, the area filter, the measurements
  and the output plane are those of brightfield mode, by the same code.
- **The cut-off.** With `thresholdMethod` `"Fixed"` it is
  `intensityThreshold`, in the input's units; `Byte`, `UInt16` and `Float32`
  are accepted. With `"Triangle"`, the default, it comes from the whole slide,
  by the route `pixel.threshold_otsu` takes: `needsGlobalContext()` is true
  only in this case, `accumulate()` counts the signal of the tile rect (not
  the halo) at the pixels the run's mask admits into 256 bins (`Byte`) or
  65536 (`UInt16`) of exact 64-bit counts, leaving out a signal of exactly 0
  when `ignoreZeros` is true (the default), `combine()` adds the bins and
  `finalizeAccumulation()` takes the cut-off from the sum. The cut-off does
  not depend on the tile size, the worker count or the tile order.
  `Float32` with `"Triangle"` throws `UnsupportedDataTypeError` naming
  `"Fixed"` as the way out, from `outputType()` when validation plans the run,
  before any tile is read (`supportedTypes()` does not depend on the mode, so
  it cannot refuse it; `accumulate()` refuses it again): a triangle on
  `Float32` would need a histogram range, and no slide needs it.
- **The triangle** is that of Zack, Rogers and Latt (*J Histochem Cytochem*
  25:741–753, 1977), as OpenCV 4.14's `getThreshVal_Triangle_8u` implements
  it, generalised to 256 or 65536 bins of 64-bit counts and matched step for
  step: the bounds widened by one bin, the histogram flipped when the peak lies
  nearer the left bound, the bin farthest from the line joining the bound to
  the peak (the first maximum kept, not the lowest bin among ties), OpenCV's
  final decrement, and the flip undone. On `Byte` it equals `cv::threshold`
  with `THRESH_TRIANGLE` on the same pixels, which the test takes as its
  oracle.
- **UInt16 in steps.** `UInt16` data is often a narrower range scaled up,
  every value a multiple of a step g (257 for 8-bit data stretched to 16
  bits); the empty bins between the populated ones would put the cut-off right
  next to the glass's peak. So for `UInt16` the bins are first reduced by g,
  the greatest common divisor of the populated values (0 fits every step):
  value v is counted as v / g over 65535 / g + 1 bins, the triangle gives t on
  them, and the cut-off is t · g. For a multiple of g, v > t · g exactly when
  v / g > t, so 8-bit data scaled by 257 gives the cut-off of the 8-bit data
  times 257 and the same objects. When t is at or above the largest reduced
  value, every counted value would be at or below t · g: two values, 0 and
  1000, reduce to bins 0 and 1, whose triangle gives 2, and 2000 would leave
  the signal out. Then the reduction is dropped and the triangle is taken on
  the unreduced 65536 bins, which gives a cut-off between the two values.
  Dense data (g = 1) is unchanged, and `Byte` is never reduced, so it still
  equals OpenCV.
- **The empty histogram.** Only a histogram with nothing counted, the mask
  admitting no pixel or, with `ignoreZeros`, no pixel above 0, has no cut-off,
  and then nothing is tissue.
- **The cut-off's floor.** The cut-off never falls below the smallest counted
  value minus 1. The triangle can fall below it: OpenCV gives -1 for a
  histogram that rises straight to its peak from bin 0 and for a uniform 1,
  and a single `UInt16` value v reduces to 1, whose -1 scales back to −v. A
  cut-off that low would make the pixels nothing counted tissue, such as the
  zeros outside a polygon or mask restriction; raised to the floor, every
  counted pixel keeps its side of the cut-off (still OpenCV's answer on the
  counted pixels) and only uncounted values below the smallest counted one
  change. So a single counted value v > 0 gives the cut-off v − 1 (all counted
  pixels are tissue), and a single bin at 0 finds nothing.
- **Exact zeros: `ignoreZeros`.** Pixels outside the scanned area are exactly
  0. Counted, they can be the histogram's peak even as a fifth of the slide
  (the example slide at level 2, below): the line from the peak at 0 then
  makes the sparsest low glass bin the farthest, the cut-off falls into the
  glass, and glass is found as tissue. So with `ignoreZeros` true, the
  default, a pixel whose signal is exactly 0 is not counted. It stays
  background: the cut-off is never below the smallest counted value minus 1,
  which is at least 0. A histogram of nothing but zeros is then empty and
  finds nothing, as before. The `UInt16` step reduction and the floor apply to
  whatever was counted, unchanged. `ignoreZeros` false counts the zeros like
  any value; it is for a slide whose glass is genuinely 0, where the zeros are
  the background the cut-off must lie above.
- **The bag.** The operation records `mode`, `thresholdMethod` and
  `intensityThreshold` only in fluorescence mode, and `ignoreZeros` only with
  fluorescence and `"Triangle"`, always with its value, so a Triangle bag is
  complete. A brightfield operation records exactly the keys it recorded
  before the mode existed, so existing pipeline files and workspaces compare
  equal and do not turn stale. `saturationThreshold` is refused in
  fluorescence mode, the fluorescence keys in brightfield mode, and
  `ignoreZeros` with `"Fixed"`, whether given typed or by name.

**Evidence from the example slide.** The design rests on measurements of one
5-channel, 8-bit PerkinElmer scan (DAPI, FITC, CY3, Texas Red, CY5) at its
three coarsest levels, 780, 1560 and 3120 pixels wide:

- Outside the scanned area the pixels are 0. Glass inside it is 1 to 4 in the
  per-pixel channel maximum. Tissue starts at about 5.
- The triangle cut-off on the histogram of the channel maximum is 6, 7 and 7;
  on DAPI alone it is the same. A cut-off of about 5 to 7, followed by a
  closing of radius 2, gives a clean tissue outline with the alveolar spaces
  left open.
- Otsu on the same histogram picks 22, which cuts holes through the tissue, so
  the automatic cut-off is the triangle, not Otsu.
- The unscanned area, exactly 0, is about 21% of the pixels and the
  histogram's peak. Counted, it breaks the triangle at level 2 (6240 pixels
  wide): the cut-off collapses to 2 and the glass becomes tissue. Left out
  (`ignoreZeros`), the cut-off is 7 to 8 at every level:

| Level | Width | Cut-off, zeros counted | Cut-off, zeros left out |
|---|---|---|---|
| 2 | 6240 | 2 (glass found as tissue: 68% of the pixels) | 7 to 8 |
| 3 | 3120 | 7 | 7 to 8 |
| 4 | 1560 | 7 | 7 to 8 |
| 5 | 780 | 6 | 7 to 8 |

The slide is not in the repository, so its check (about 41–44% tissue at levels
2 to 5 with zeros left out, the cut-off 7 or 8) is outside the test suite, as
the labelled corpus of 6.9 is. `TriangleZeroBandWithBroadGlass` reproduces the
level-2 failure on a synthetic slide: a fifth exact zeros, glass broad over 1
to 4 but sparse at 1.

**The analysis level is set on the run, not here.** Tissue detection does not
need full resolution, and UR-IMG-4 requires reading from the coarsest adequate
level — but that is `PipelineSpec::analysisLevel` (5.4), defaulting to -1 for
automatic selection. An earlier draft put `analysisLevel` on
`TissueDetectParameters`, which gave the decision two owners: the tile grid is
laid out at one level for the whole run (5.6), so an operation asking for a
different one had nowhere to be honoured. A chain that needs two levels is two
stages.

`minimumAreaPx`, `maximumAreaPx` and `minimumRegionAreaPx` filter **during
extraction**, in the finisher, so discarded objects are never written to the
store. UR-SEG-7's acceptance criterion is that a filtered run on a slide with
many small artefacts yields a materially smaller store, and that the filtered
objects appear in no query. Because the finisher sees the final object, a
bound applies identically however the object was assembled — whole in one
tile or merged from fragments, each of which may be below the bound on its
own. The bounds compare `area` in square **level-0** pixels, after any holes
were removed, so a bound means the same at every analysis level.

Contours are polygon rings in level-0 slide coordinates, with holes as separate
rings (UR-SEG-4).

### Model label images — `algorithm/segment/label_image.hpp`

```cpp
/// Abstract; no catalog name of its own. A subclass (a segmentation model,
/// in C++ or, later, Python) declares name(), halo() and parameters() and:
class LabelImageOperation : public Operation {
public:
    /// One Int32 plane over the halo rect, zero-filled, owned by the base;
    /// 0 is background, other values need be distinct only within the call.
    virtual void predictLabels(const OperationContext& context, BlockView& labels) = 0;
    // Defaults: Joint, every type, not uniform, one Int32 output plane,
    // producesObjects() true, area and perimeter (calibrated forms too).
    void apply(OperationContext& context) final;
};
```

A network such as Cellpose or StarDist predicts, per tile, a label image whose
values mean something only inside that tile. `segment.contours` cannot take it:
it joins seam fragments by value, and here one nucleus has value 7 on one side of
a seam and 12 on the other, while 7 names another nucleus elsewhere in the same
tile. `LabelImageOperation` delivers each object whole from the one tile that
owns its anchor, over a halo the subclass declares at least as large as its
largest object (`05-pipeline.md`, "Anchor ownership instead of seam joining").
Its `apply()`:

1. calls `predictLabels()` over the halo rect, then clears the labels beyond the
   analysis level (`OperationContext::levelRectAtLevel()`);
2. labels each maximal 4-connected set of one non-zero value as one region
   (`detail::labelRegions`), so two disjoint regions of one value are two
   objects and touching regions of different values share an edge;
3. traces the regions, holes included (`detail::traceRegions`), from the halo
   rect's origin;
4. takes each region's anchor, the first vertex of its first outer ring;
5. keeps the region only when the anchor lies in the half-open tile rect;
6. drops a kept region that touches a side of the halo rect lying strictly
   inside the level, counting it with `countTruncatedObjects()`; a side on the
   level's edge does not count. The count is per region dropped in this tile's
   view: another tile may see a part of the same object whose own anchor it
   owns and drop and count that too, so a truncated object counts at least
   once and the total depends on the tile size (SDS 05, 5.5);
7. emits every other kept region whole through the object sink
   (`detail::emitWholeRegions`, which shares `emitRegions`' body), never as a
   seam fragment, with `area` and `perimeter` and, on a calibrated scene,
   `area.um2` and `perimeter.um`;
8. writes the labels over the tile rect into its output plane.

Its working set is 272 bytes per pixel of the halo rect: the predicted and
region label planes, the union-find parents, and the tracing and emission
figures `segment.contours` declares; plus 4096 bytes and the allocator's
overhead on 16 buffers per call. A halo too large for the memory ceiling is
refused by validation before any tile runs. With a halo covering its objects,
its objects are the same for every tile size and worker count (UR-QA-3);
`test.disks`, the test subclass, checks that at tile sizes 512 and 1024 with 1
and 4 workers.

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
**throws** when a pixel of a segmented region lies further than that from the
marker its basin reached it from, or is reached by none, because such a
pixel's basin could be decided by
evidence the tile cannot see — which is the rejection UR-SEG-8 requires rather
than permits. Failing is what keeps UR-QA-2 within reach: for every input
that runs, each pixel's seed lies within the supplied context, so that much of
the result is bounded by construction rather than by hoping clumps stay small.
Where values tie at the meeting of basins, which basin wins can depend on how
the flood reached the tie, and that case is tested, not proven (below).
Degrading quietly instead — segmenting what fits, passing through what does not
— would make the output depend on tile position, which 5.6 identifies as the
worst failure available because the count comes out nearly right.

### Watershed, exactly

**Flooding.** Plane 0 is flooded, foreground only, **from the markers
downhill**, as a frontier flood rather than a global sort: the frontier holds
the unflooded foreground pixels beside a flooded one, the frontier pixel of
highest value is flooded next, ties broken by level coordinates (row, then
column), and it joins the basin of its first flooded side neighbour — highest
value, ties again by level coordinates. A pixel enters the frontier only when
a neighbour has been flooded, so a lower pixel next to a basin may be flooded
before a higher one no basin has reached yet. Within a halo'd context the
order is that of level coordinates, so it never depends on where the tile
lies. That the result is then the same at every tile size is what the tests
assert, exactly (6.9), not a theorem: the reach check bounds each pixel's
distance to its seed, but not the length of the flood paths that decide ties
between basins, and there is no general proof that a halo of
2 · `maximumMarkerSpacingPx` + 1 makes every input tile-invariant. As evidence,
not proof, an exact model of the flood found no counter-example in about
28,000 tie-heavy random inputs with tiles far smaller than the halo. Samples
are compared as doubles, exactly for every type but 64-bit integers beyond 2⁵³
in magnitude.

**Markers.** With `LocalMaxima`, a marker is a foreground pixel that comes
before every other foreground pixel within `minimumMarkerDistancePx`
(Chebyshev) — greater, or equal and above it, or to its left on the same row
— so a plateau gives its topmost, then leftmost, pixel. A pixel whose window
reaches beyond the halo'd context is not decided there, which the halo makes
irrelevant to the tile's own pixels. With `AuxiliaryRaster`, every foreground
pixel whose value in the bound `Int64` raster is not -1 is a marker, and one
basin is one value. A tile whose run binds no raster under `auxiliaryName`, or
one that is not a single `Int64` plane, throws `InvalidArgumentError` naming
the parameter `auxiliaryName`, never the binding's name, since messages carry
no parameter values. `minimumMarkerDistancePx` must not exceed
`maximumMarkerSpacingPx`.

With `minimumMarkerValue` set, a `LocalMaxima` candidate below it is simply
not a marker; it still floods and counts as foreground. The halo and the
working set are unchanged. A region it leaves with no marker within the
spacing fails the reach check. The floor is refused with `AuxiliaryRaster` by an
`InvalidArgumentError` naming `minimumMarkerValue`; a non-finite value is
refused by the typed setter, and `OperationParameters` cannot hold one.

**Settings that keep the reach on a distance transform.** One object with no
marker fails the whole run, so a debris speck below the floor would stop a
slide. Flooding `pixel.distance_transform` of a mask, two settings rule every
such failure out, and the operation's documentation states them:

- *Open the mask first* with `pixel.morph_open` and an `Ellipse` element of
  side 2k + 1, k ≥ `minimumMarkerValue` and k ≥ `minimumMarkerDistancePx` + 1.
  Every object the opening keeps contains a whole element, and every pixel
  outside OpenCV's drawn ellipse of radius k lies further than k from its
  centre, so the object's peak exceeds k and the floor keeps it. A pixel p of
  one object and a pixel q of another, of values v and w, satisfy
  v + w ≤ |p − q| + 2, or a 4-connected path along the segment between them
  would join the objects. For q to outrank p, the first object's peak, it
  needs w ≥ v > k within Chebyshev d = `minimumMarkerDistancePx`, so at most
  √2·d away, which needs 2k < √2·d + 2: false for k ≥ d + 1. Specks, lines and spurs narrower than
  the element are removed. `NucleiChain.OpeningClearsDebrisBelowTheMarkerFloor`
  runs the chain over debris with and without the opening.
- *Set the spacing to the largest object*: at least the largest width or height
  of an object. An elongated object of even width has a flat ridge, which
  gives one marker, and that marker has to reach the whole object.

Dropping an object with no marker instead of failing was considered and not
taken: a tile cannot tell an object with no marker from one whose marker lies
beyond its halo, so the result would depend on the tiling.

**Halo, 2 · `maximumMarkerSpacingPx` + 1.** A tile pixel's basin depends on the
markers within the spacing, and their detection on pixels a further
`minimumMarkerDistancePx` away, at most the spacing; the one more keeps the
pixels just beyond in view. The working set is declared per pixel of that
halo'd tile, which is large beside the tile when the spacing is: a spacing of
32 grows a 256-pixel tile to 386 pixels a side.

**The reach check.** After flooding the halo'd context, and before anything
is emitted, every foreground pixel of the tile rect must have been flooded
from a marker pixel within `maximumMarkerSpacingPx` (Chebyshev). The distance
is measured to **the seed pixel its flood path started from** — each flooded
pixel carries its seed's position — not to its basin's marker region as a
whole: an auxiliary marker region may be large, and a distance to its corner
would refuse inputs that flood correctly. A pixel no marker reached, or
reached from a seed further away, fails the tile with `InvalidArgumentError`,
and the run with it; nothing is ever segmented partially in its place. The
message names the first such pixel of the tile, in row order, in level
coordinates, and what to change. A pixel reached from too far gives
"segment.watershed: the foreground at level pixel (x, y) lies further than
maximumMarkerSpacingPx from its marker; set maximumMarkerSpacingPx to at least
the largest width or height of an object". A pixel no marker reached gives
"segment.watershed: no marker within maximumMarkerSpacingPx reaches the
foreground at level pixel (x, y): its object has no marker, because its peak is
below minimumMarkerValue or a higher object lies within
minimumMarkerDistancePx, or it is wider or higher than maximumMarkerSpacingPx;
on a distance transform, open the mask first with an Ellipse element of radius
at least minimumMarkerValue and minimumMarkerDistancePx + 1, and set
maximumMarkerSpacingPx to at least the largest width or height of an object",
with the floor's clauses only when it is set. The tile cannot tell those causes
apart: its object's marker may lie beyond the halo. UR-SEG-8 asks the
diagnostic to name the operation and the distance: it names the distance by
its parameter, not its value, because no message carries a parameter value; a
pixel position is not a parameter. The executor wraps the failure of a tile as
it wraps any `apply()` failure (5.9); the inner message is the one above.

**Objects.** Within the tile rect, a basin's pixels are labelled into
4-connected parts — a basin restricted to the tile may split — and each part
is an object, traced as 6.7 describes; basins touching each other share the
edges between them. `objectsAreEightConnected()` is false. A basin's join key
is its identity, the same in every tile: the marker's pixel packed from level
coordinates as `(y << 31) | x`, or with `AuxiliaryRaster` the raster's value.
Holes are kept; `minimumAreaPx` filters in the finisher.

**Flood a surface that peaks inside each object.** On a plateau the markers
are only the topmost-leftmost pixels within each window, and the flood
reaches the rest of a flat region from them, so a flat or binary input — a
thresholded mask flooded as it is — gives few markers far apart and usually
fails the reach check rather than separating anything. The input to flood is a
distance transform of the mask, whose maxima sit at the objects' centres (the
test of 6.9 floods exactly that), or a smoothed intensity. The distance
transform is `pixel.distance_transform` (6.4), which clamps at its parameter
`maximumDistancePx`, so that should be at least the objects' radius for a peak to survive.

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

class MeasurementSelection {                     // pimpl'd; empty until a kind is added
public:
    MeasurementSelection();                      // empty; does not allocate
    static MeasurementSelection standard();      // all of the above
    static MeasurementSelection geometryOnly();  // Area, Perimeter, Centroid, BoundingBox

    MeasurementSelection& add(MeasurementKind);
    MeasurementSelection& addAll();
    bool    contains(MeasurementKind) const noexcept;
    int32_t count() const noexcept;
};

/// Calibration is applied automatically from the scene's resolution metadata.
/// Reports whether it is available, by the rule of OperationContext::pixelSizeMicrons.
bool sceneIsCalibrated(std::shared_ptr<slideio::Scene>) noexcept;
double scenePixelSizeMicrons(std::shared_ptr<slideio::Scene>) noexcept;  // 0 if unavailable
```

**Requirements:** UR-SEG-5, UR-SEG-6, UR-IMG-2.

Measurements live in `algorithm` rather than in their own module: they are
computed per object during extraction, share the catalog's declaration
machinery, and have no consumer that does not also use the catalog.

**The segmenters measure; there is no measurement operation.** A selection is
a parameter of the operations that produce objects with intensity —
`segment.connected_components` and `segment.watershed`, bag key
`measurements` (6.2), `standard()` by default — and their objects are measured
as they are finished (6.7). An earlier draft had a separate
`measure.object_measurements` operation over the run's intensity channels. It
could not have worked as a chain step: the objects it would measure are
emitted by an earlier operation and merged by the framework after that
operation's tiles close, so no later operation ever sees them, and a merged
object never passes through a later operation's `apply`. The intensity
channels are the run's own (`PipelineSpec::channels`), from plane 1 of the
segmenter's input onward, so the selection needs no `ChannelSelection` of its
own. They are measured on the scene's samples of those channels, as read,
whatever earlier steps of the chain did to the planes (6.7).

`standard()`, `geometryOnly()` and `addAll()` are not `noexcept`: the
selection is pimpl'd under C-2 and an empty one holds nothing, so filling one
allocates and may throw `std::bad_alloc`. `add` refuses a value that is not an
enumerator with `InvalidArgumentError`. A moved-from selection is empty.

**Names and units.** Every geometric measurement is in level-0 pixels,
whatever the analysis level:

| Kind | Names | Unit |
|---|---|---|
| `Area` | `area` | `SquarePixels` |
| `Perimeter` | `perimeter`, the length of every ring | `Pixels` |
| `Centroid` | `centroid.x`, `centroid.y` | `Pixels` |
| `BoundingBox` | `boundingBox.x`, `.y`, `.width`, `.height` | `Pixels` |
| `IntensityMean`, `IntensityStdDev`, `IntensityMin`, `IntensityMax` | `intensity.<c>.mean`, `.stddev` (population), `.min`, `.max`, for each measured channel c | `Intensity` |

`area` and `perimeter` come from `applyStandardMeasurements`, as do the
intensity statistics. The centroid is area-weighted, holes weighing
negatively, as `Polygon::centroid()` is, but **unrounded**: the finisher
computes it by the shoelace formula in `double`. When the run's pixel size s
is positive, each selected geometric measurement also gets a calibrated twin,
`<name>.um` in `Micrometres` (the pixel value × s) or `area.um2` in
`SquareMicrometres` (× s²). A kind not selected leaves no name behind, its
calibrated twin included.

Every accumulator is 64-bit. An intensity sum over a whole-slide tissue region
exceeds 2³¹ trivially, and UR-IMG-2's static check covers this path.

**Across a tile seam the operation emits partials, not measurements.** For a
region touching a boundary it calls `emitSeamFragment` with one
`IntensityPartial` per measured channel (`05-pipeline.md` 5.1, 5.7) instead of
finishing the measurement, because the rest of the object is in another tile.
`SeamMerger` merges the fragments into one polygon, combines the partials per
channel in canonical fragment order (ascending fragment anchor, ties by tile
index), and measures the result with `applyStandardMeasurements`
(`05-pipeline.md` 5.1). An object finished inside one tile is measured by the
same function, from its polygon and partials built the same way, and both then
pass through the operation's `finishObject`, so the two paths share one
formula rather than two that must agree. Area, perimeter, centroid, bounding
box and intensity statistics are then identical to single-tile processing —
bitwise for integer intensities whose sums stay below 2⁵³, to a relative
10⁻¹² for `Float32`, whose sums regroup with the tile size (`05-pipeline.md`
5.7) — which is what UR-SEG-3's acceptance criterion measures across three
tile sizes, and the reason the operation cannot simply average or sum what
each tile computed.

Measurements with a physical dimension are available in both pixels and
calibrated units. The pixel size comes from the scene through
`OperationContext::pixelSizeMicrons()` (`05-pipeline.md` 5.1): the mean of
SlideIO's x and y resolution when both are finite, positive and agree within
one part in a million. A scene lacking resolution metadata, or with anisotropic
pixels, yields pixel units only — no `.um` names, and no measurement with
`Measurement::isCalibrated()` true — and `sceneIsCalibrated()` returns false
for it: that is the documented indication UR-SEG-6 requires rather than a
silent fallback. `sceneIsCalibrated` and `scenePixelSizeMicrons` apply the same
rule as the run (`pixelSizeMicronsFromResolution`, `01-core.md` 1.5), so a
caller can tell before running what the objects will carry; a null scene, or
one whose resolution cannot be read, gives false and 0.

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
| Contours | Synthetic label rasters of known regions, touching and nested |
| Measurements | Analytic values for synthetic objects of known dimensions |
| Tissue detect | Labelled test corpus, area within documented tolerance; in v1 analytic on synthetic tissue and glass (below) |
| Watershed | Synthetic touching discs of known centres and radii; recovered count, area and centroid within tolerance |

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
| `pixel.distance_transform` | Published definition: exact Euclidean distance (Felzenszwalb and Huttenlocher, *Theory of Computing* 8:415–428 (2012)); brute force over all background pixels | Exact: equal to the brute-force distance, clamped at the maximum, on random 0/1 `Byte` masks at maximums 3, 8 and 32, the level's edge counting as background; the centre of a disc equal to the brute force; a NaN sample background and a negative one foreground; output `Float32` for every input type; at maximums 300 and 1000 on sparse masks; over the 4096-pixel row of a context 8192 pixels wide, at maximum 2048 the final review's case (background 545 and 568 rows away at columns 4481 and 6081, where a `float` breakpoint gives √949889 for √949888) and sparse background at maximums 2048 and 4096; tiled output bitwise equal to one tile on `Byte` and `Float32`, at maximum 600 on a sparse mask, and at maximum 1100 on a sparse 3000 by 2600 level whose distances run to the maximum, at tiles 1000 and 1536, the single tile equal to the brute force on a lattice |
| `pixel.fill_holes` | Published definition: background components enclosed within the declared size; a brute-force flood fill | Exact: equal to the flood fill, over the whole image, on random masks (60 by 50, 55% foreground) at sizes 2, 4 and 8 with the area absent and 6; a hole of 5 by 5 filled and one of 6 by 6 not at size 5; an area of 16 filled and of 15 not; a U open to the level edge unfilled; a ring closed only diagonally filled; nested rings; NaN background; output `Byte` 255 for `UInt16` and `Float32`; tiled output bitwise equal to one tile at tiles 64, 100 and 257 on `Byte` and `Float32`, with holes across the seams |
| `pixel.bilateral_filter` | Tomasi and Manduchi, *ICCV* 1998; a direct per-pixel computation | Exact: `Float64` equal bit for bit to the direct computation and `Float32` to its rounding, also with NaN neighbours left out of both sums; an infinite centre kept and its finite neighbours exact; a 0/200 step edge kept within 10⁻⁶ and the neighbours of a NaN within 10⁻¹² of a constant; tiled output bitwise equal to one tile on `Byte` and on `Float32` with NaNs, and a column shift giving the same bits on `Float32` and `Float64` |
| `pixel.morph_erode`, `pixel.morph_dilate`, `pixel.morph_open`, `pixel.morph_close` | Analytic: erosion and dilation of synthetic binary shapes by known elements | Exact: squares eroded, dilated, opened and closed to their analytic shapes in all five types; on NaN-free input equal to OpenCV's `morphologyEx` on the zero-padded image in all five types; a NaN in the window gives NaN; the level's edge erodes inward; tiled output bitwise equal to one tile on `Float32` and `UInt16`, and on `Float32` with NaNs and signed zeros, and a column shift giving the same bits on `Float32` and `Float64` |
| `pixel.canny` | Canny, *IEEE TPAMI* 8:679–698 (1986); OpenCV's `Canny` on one tile | Exact on one tile against `cv::Canny` of the zero-padded level, at apertures 3, 5 and 7; tiled output equal to one tile where every edge is strong; on a weak chain longer than the reach, the pixels tiling changes there are dropped ones, each further than 32 pixels from every strong one; thresholds beyond every reachable magnitude, up to 10¹², give no edges; the complexity note states the bound verbatim |
| `segment.connected_components` | Analytic: known rectangles, rings with holes and diagonal chains | Exact counts, rings, areas, perimeters, centroids, bounding boxes, calibrated twins and intensity statistics; diagonal neighbours one object 8-connected and two 4-connected; NaN and 0 background in every type; NaN intensity samples skipped, the mean, minimum and maximum exact and the standard deviation within 10⁻⁹ relative of a direct computation; objects identical to one tile at tile sizes 64, 100 and 257, both connectivities — geometry and integer intensities exact, `Float32` intensities within 10⁻¹² relative; an object across four tiles with a hole cut by both seams, and an 8-connected chain across a tile corner, each one object equal to one tile; an area bound applied to merged objects; a filtered run storing fewer objects |
| `segment.contours` | Analytic: label rasters of touching regions, holes filled by other labels or by background, `Float32` labels one ulp apart | Exact counts, shared rings, areas and perimeters; `Float32` labels compared exactly, -0 and NaN background; fragments keyed by their value; objects identical to one tile at tile sizes 64, 100 and 257 with regions of different values touching across seams, and at 25, 50 and 64 on a checkerboard of two adjacent `Float32` labels |
| `segment.watershed` | Analytic: two touching discs of radius 10, centres 16 apart, whose distance transform is flooded | Two objects, each with an area within 5% of the disc less half the shared lens and a centroid within 1 pixel of the analytic one, their areas summing exactly to the union; plateau markers at their topmost-leftmost pixel; anchor-raster markers; objects identical to one tile, exactly, at tile sizes 64, 100 and 257 over thirty pairs of touching discs; a region beyond the spacing fails with the reach error at every tile size, never a partial segmentation |
| `segment.tissue_detect` | Analytic: pink ellipses with one-pixel holes on grey-noise glass with dust | Detected area within 1% of the ellipses' analytic area at level 0, the holes closed; a 2-pixel gap bridged at radius 2 and left open at 0; at analysis level 2 areas in level-0 pixels (16 per level-2 pixel) within the staircase bound 16 · 4 · (a + b + 1) of the analytic area; calibrated area and perimeter exact; objects identical to one tile, exactly, at tile sizes 64, 100 and 257 and closing radii 0, 2 and 5, across bars and gaps on seams at every phase; `UInt16` samples (the `Byte` slide × 257) giving the same objects as `Byte`; fluorescence: two blobs bright in different channels over 0 to 4 glass noise and an exact-zero band give the blobs' area within 1% with a fixed and a triangle cut-off, the triangle equal to OpenCV's on `Byte` (random and shaped histograms), identical objects at tile sizes 64, 100 and 257 and 1 or 4 workers, only admitted pixels counted, the empty histogram finding nothing, `UInt16` with just two values (0 and 1000) found as `Byte` with 0 and 200 is and as `"Fixed"` at 500, a single counted value under a polygon restriction giving exactly the restricted rectangle in `Byte` and `UInt16`, `Float32` accepted with `"Fixed"` and refused with `"Triangle"` by validation, before any tile runs |
| Measurement finishing | Analytic: rectangles and rectangles with holes, finished directly | Exact: calibrated twins at 0.5 µm per pixel and none without a pixel size; centroid unrounded and holes weighing negatively; unselected names removed; area bounds inclusive and applied after holes are dropped; `sceneIsCalibrated` and `scenePixelSizeMicrons` following the run's rule, the mean of two agreeing resolutions within 10⁻¹² |

Exact agreement is asserted wherever the computation is defined in a fixed
order, and a tolerance only where the reference is a published constant, the
input is quantised, or the expected value is an analytic one computed by
another route.

**Open: the tissue corpus.** UR-SEG-1's acceptance asks for detected tissue
area within a documented tolerance of ground truth on a labelled test corpus,
and the reference table above names that corpus. No such corpus is in the
repository, so `segment.tissue_detect`'s test is analytic on synthetic
tissue and glass, which proves the operation computes what it is specified to
compute but not that a saturation threshold of 0.07 separates tissue from
glass on real slides. Its correctness reference says so. Agreeing on a corpus,
its location and the tolerance, and running the operation against it, is an
open item; until it is done UR-SEG-1 is covered only analytically.

## 6.10 Scope of v1 — the catalog, closed

**The v1 catalog is twenty-three operations.** This resolves open question 7 in
`requirements.md`, which the requirements call the choice that decides the size
of v1 more than any other. It was closed at twenty-two; the twenty-second,
`measure.object_measurements`, left it when the segmentation operations took
over measuring (below), which made it twenty-one. Requirements 0.14 then added
`pixel.distance_transform` (UR-PIX-7), for the watershed's input, and
`pixel.fill_holes` (UR-PIX-8), for cleaning masks, which makes it twenty-three.

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
| `pixel.distance_transform` | 6.4 | PerChannel | `maximumDistancePx` |
| `pixel.fill_holes` | 6.4 | PerChannel | `maximumHoleSizePx` |
| `pixel.threshold_fixed` | 6.5 | PerChannel | none |
| `pixel.threshold_otsu` | 6.5 | PerChannel, global context | none |
| `pixel.threshold_adaptive` | 6.5 | PerChannel | blockSize/2 |
| `pixel.convert_type` | 6.6 | PerChannel | none |
| `segment.tissue_detect` | 6.7 | Joint, planes of one type: three (RGB) in brightfield mode, one or more in fluorescence mode; global context with the triangle method | 2 · `closingRadiusPx` + 1 |
| `segment.connected_components` | 6.7 | Joint: plane 0 segmented, up to three planes measured | 1 |
| `segment.watershed` | 6.7 | Joint: plane 0 flooded, up to three planes measured | 2 · `maximumMarkerSpacingPx` + 1 |
| `segment.contours` | 6.7 | PerChannel | 1 |

**`measure.object_measurements` left the catalog.** It was to measure, per
object as it was emitted, the objects an earlier operation produced. A chain
step cannot do that: steps pass rasters to each other, not objects, and an
object cut by seams is merged by the framework only after its producer's tiles
close, so no later operation ever sees it — and a measurement that only some
objects received would break UR-SEG-3. Measuring belongs where every object,
whole or merged, passes once: its producer's finishing step (`05-pipeline.md`
5.1). So the segmenters that see intensity, `segment.connected_components` and
`segment.watershed`, take a `MeasurementSelection` and measure their own
input planes besides the one they segment (6.7), and the requirement it answered now maps to them (6.8).

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
| UR-PIX-7 distance transform for the watershed's input | `pixel.distance_transform` |
| UR-PIX-8 filling of holes in a binary mask | `pixel.fill_holes` |
| UR-IMG-13 explicit type conversion | `pixel.convert_type` |
| UR-SEG-1 tissue versus background | `segment.tissue_detect` |
| UR-SEG-2 connected components | `segment.connected_components` |
| UR-SEG-3 seam merging | `SeamMerger`, not an operation |
| UR-SEG-4 contours with holes | `segment.contours` |
| UR-SEG-5 per-object measurements | the `measurements` selection of `segment.connected_components` and `segment.watershed`, finished per object (6.8) |
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
question 7 was closed. The catalog did not grow for it — it was twenty-one
operations, after `measure.object_measurements` left, until UR-PIX-7 and
UR-PIX-8 added two more — so the mapping is now
eighteen requirements against twenty-three entries with one requirement
unanswered. The closure of question 7 survives as a *decision*
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
