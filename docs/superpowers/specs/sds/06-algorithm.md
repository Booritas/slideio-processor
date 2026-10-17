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

**Requirements:** UR-PIX-1, UR-PIX-2, UR-ALG-3.

Both are `Joint` — they consume the selected channels together — and both
declare `requiresUniformTypes() == true`, so a selection mixing `Byte` and
`Float32` fails with an error naming the operation and both channels rather
than silently misinterpreting them (UR-ALG-3).

`makeColourConvert` with `from == Rgb` throws `InvalidArgumentError` when the
scene's metadata does not state that the selected channels are RGB. UR-IMG-7
forbids inferring a channel's meaning from its index, and its acceptance
criterion is exactly this error.

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

**Requirements:** UR-PIX-3, UR-PIX-4, UR-EXT-3, UR-QA-2.

Every one of these is a neighbourhood operation with a non-zero halo, and the
framework supplies exactly the declared border context so results are correct
at tile seams. Each must produce whole-slide output matching a single-tile
reference computation on an image small enough to process in one tile
(UR-QA-2) — the acceptance criterion for both UR-PIX-3 and UR-PIX-4.

`kernelSize == 0` on `makeGaussianBlur` derives the kernel from `sigma`.

**`makeCanny` is the exception to the sentence above, and it is declared rather
than discovered.** Canny's final stage is edge tracking by hysteresis: a weak
edge is kept when it is connected, along a chain of any length, to a strong
one. Connectivity of unbounded reach is not a neighbourhood property, so no
fixed halo makes it exact — a weak chain whose strong seed lies further away
than the halo is dropped where single-tile processing would have kept it.

The halo is therefore a **stated bound on the approximation**, not a guarantee
of equality, and `makeCanny` documents it as such: the result matches
single-tile Canny wherever every hysteresis chain is shorter than
`apertureSize`-derived halo, and the operation reports that bound through its
complexity note (UR-ALG-8). UR-PIX-3's acceptance criterion — "whole-slide
output matches a single-tile reference" — is stated without tolerance and
cannot be met unconditionally by this one operation; that conflict is recorded
as open question 15 in `requirements.md` and needs a requirements amendment
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

**Requirements:** UR-PIX-5, UR-EXE-1, UR-QA-3.

`makeOtsuThreshold` is the module's only global-context operation and the
reason `pipeline::Reduction` exists. It returns `needsGlobalContext() == true`,
accumulates a `Histogram64` per tile, combines order-independently because the
bins are integers, and computes the threshold once before the apply pass.

Its acceptance criterion is that the streamed whole-slide value equals the
value computed from a full in-memory histogram of the same slide — which also
makes it the natural regression test for UR-QA-3's worker-count invariance,
since a float accumulator here would break it non-deterministically.

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
region touching a boundary it calls `emitSeamFragment` with an
`IntensityPartial` per channel (`05-pipeline.md` 5.7) instead of finishing the
measurement, because the rest of the object is in another tile. `SeamMerger`
recomputes the geometric measurements from the merged polygon and combines the
intensity partials in canonical fragment order. Area, perimeter, centroid and
intensity statistics are then identical to single-tile processing, which is
what UR-SEG-3's acceptance criterion measures across three tile sizes — and
the reason this operation cannot simply average or sum what each tile
computed.

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
| Morphology | Analytic on synthetic binary shapes |
| Otsu | Full in-memory histogram of the same slide |
| Connected components | Synthetic objects of known count and shape |
| Measurements | Analytic values for synthetic objects of known dimensions |
| Tissue detect | Labelled test corpus, area within documented tolerance |
| Watershed | Synthetic touching discs of known centres and radii; recovered count and boundary within tolerance |

## 6.10 Scope of v1 — the catalog, closed

**The v1 catalog is twenty-two operations.** This resolves open question 7 in
`requirements.md`, which the requirements call the choice that decides the size
of v1 more than any other.

| Name | Section | Arity | Halo |
|---|---|---|---|
| `pixel.colour_convert` | 6.3 | Joint, uniform types | none |
| `pixel.stain_deconvolution` | 6.3 | Joint, uniform types | none |
| `pixel.gaussian_blur` | 6.4 | PerChannel | kernel |
| `pixel.median_blur` | 6.4 | PerChannel | kernel |
| `pixel.sobel` | 6.4 | PerChannel | kernel |
| `pixel.scharr` | 6.4 | PerChannel | fixed 3×3 |
| `pixel.laplacian` | 6.4 | PerChannel | kernel |
| `pixel.canny` | 6.4 | PerChannel | aperture, **bounded approximation** |
| `pixel.bilateral_filter` | 6.4 | PerChannel | diameter |
| `pixel.morph_erode` | 6.4 | PerChannel | structuring element |
| `pixel.morph_dilate` | 6.4 | PerChannel | structuring element |
| `pixel.morph_open` | 6.4 | PerChannel | structuring element |
| `pixel.morph_close` | 6.4 | PerChannel | structuring element |
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
