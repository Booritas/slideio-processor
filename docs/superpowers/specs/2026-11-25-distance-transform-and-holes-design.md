# Distance transform, hole filling and a marker floor for watershed — design

| | |
|---|---|
| **Date** | 2026-11-25 |
| **Status** | Draft, for approval |
| **Branch** | `feat/distance-transform-and-holes` |

## Intent

Nucleus segmentation on H&E should run from the built-in catalog alone. Today
it cannot. `segment.watershed` is meant to flood "a distance transform of the
mask" (SDS 06 §6.7, *Flood a surface that peaks inside each object*), but v1's
catalog has no distance transform. A thresholded mask flooded as it is gives one
`LocalMaxima` seed per plateau and fails the reach check on any large stained
area. The example `slideio-processor-python/examples/nucleus_ops.py` works
around this with an OpenCV Python operation. These three additions let
`examples/nucleus_segmentation.py` drop that operation:

```
pixel.stain_deconvolution → pixel.gaussian_blur → pixel.threshold_fixed
→ pixel.fill_holes → pixel.morph_open → pixel.distance_transform
→ segment.watershed (LocalMaxima, minimumMarkerValue)
```

**Goals:** exact results at every tile size (UR-QA-2, UR-SEG-3), the same
conventions as the rest of the catalog (6.2 parameter bags, 6.9 correctness
references, working-set declarations), and no change to any stored artifact or
recorded configuration.

**Non-goals:** unbounded hole filling or distance (neither is tileable), other
distance metrics, and a geodesic or h-maxima marker mode.

**Anti-goal:** a result that depends on tile size. Wherever the true answer
would need evidence beyond a declared bound, the operation is defined by that
bound, exactly, rather than approximated.

## Approaches considered

1. **Three catalog entries (chosen).** `pixel.distance_transform`,
   `pixel.fill_holes`, and an optional `minimumMarkerValue` on
   `segment.watershed`. Each is small, independently tested and useful outside
   nuclei.
2. **A distance mode on `segment.watershed`** (flood the distance map of its own
   mask). Fewer stages, but it fuses two responsibilities, hides the surface
   being flooded from inspection, and still needs hole filling elsewhere.
3. **A dedicated `segment.nuclei`.** Most convenient, but it bakes a stain model
   and a set of thresholds into the library. That is the kind of opinionated
   algorithm the catalog leaves to callers (6.10).

## Traceability

The catalog is closed (SDS 06 §6.10): every entry traces to a requirement. Two
requirements are added, as `requirements.md` 0.14:

- **UR-PIX-7 (Should)** — Euclidean distance transform of a binary mask, up to
  a caller-declared maximum distance. *Acceptance:* equals the brute-force
  distance on synthetic masks, clamped at the maximum; identical at three tile
  sizes.
- **UR-PIX-8 (Should)** — Filling of holes in a binary mask, up to a
  caller-declared hole size. *Acceptance:* fills synthetic holes up to the
  bound, leaves larger holes and regions open to the level edge unfilled, and
  is identical at three tile sizes.

The marker floor serves UR-SEG-8 and needs no new requirement. §6.10 becomes
twenty-three operations. The traceability matrix gains the new tests.

## 1. `pixel.distance_transform`

- **Input:** PerChannel, any type. A pixel is **foreground when its sample is
  neither 0 nor NaN**, the rule `segment.watershed` already uses.
- **Output:** `Float32`, the Euclidean distance from each foreground pixel to the
  centre of the nearest background pixel, clamped at `maximumDistancePx`.
  Background is 0. Pixels outside the level are zero (05 §5.5) and so count as
  background: objects at the level edge get distance to that edge, in tiled and
  whole-level runs alike.
- **Parameter:** `maximumDistancePx`, int, default 32, range 1 to 4096.
- **Halo:** `maximumDistancePx`.
- **Why exact.** A tile pixel whose true distance *d* is at most the halo *H*
  has its nearest background within Chebyshev distance *H*, so inside the
  halo'd context, and the context gives *d*. If *d* > *H*, every background
  pixel in the context is further than *H*, and the output is *H*. So the output
  is min(*d*, *H*) at every tile size. A context with no background at all
  also gives *H*.
- **Implementation:** OpenCV `cv::distanceTransform(DIST_L2, DIST_MASK_PRECISE)`
  (Felzenszwalb–Huttenlocher, exact) on a `Byte` mask of the halo'd context,
  then clamp and copy the tile. The value is √(integer), computed the same way
  in every context, so tiled output is bitwise equal. Runs under
  `useSequentialOpenCv` as the other OpenCV operations do.
  *Amended at the final review (2026-11-25):* OpenCV 4.14.0 computes the
  parabola breakpoints in `float32`, so for long distances in contexts
  thousands of pixels wide it can pick the wrong parabola, off by 1 to 3 in the
  squared distance and depending on the tiling. The module now runs its own
  Felzenszwalb–Huttenlocher passes with the breakpoints compared exactly in
  `int64`, and OpenCV is not called (SDS 06 §6.4).
- **Reference (6.9):** brute force over all background pixels on small random
  and synthetic masks (disc: centre value = radius to within one pixel's
  discretisation, exactly the brute force). Exact.
- **Working set:** the `Byte` mask and the backend's `Float32` destination and
  its integer workspace, per pixel of the halo'd context. The figure is bounded
  from OpenCV's source, as for the other OpenCV paths. *(Amended at the final
  review: now the module's own buffers, all metered.)*

## 2. `pixel.fill_holes`

- **Input:** PerChannel, any type, foreground as above.
- **Output:** `Byte`, 255 for foreground and for filled holes, 0 elsewhere, the
  same convention as the thresholds.
- **Definition:** a **hole** is a 4-connected component of background (the
  complement of an 8-connected foreground, the usual pair) that does not reach
  outside the level, whose bounding box is at most `maximumHoleSizePx` on each
  side, and whose area is at most `maximumHoleAreaPx` when that is given. Holes
  are filled.
- **Parameters:** `maximumHoleSizePx`, int, default 32, range 1 to 4096.
  `maximumHoleAreaPx`, int, **absent by default** (no area limit), at least 1
  when given.
- **Halo:** `maximumHoleSizePx`.
- **Why exact.** In the halo'd context, a background component that does not
  touch the context's border is a whole component of the level, because it
  cannot connect beyond the context without crossing that border. A component
  that does touch the border is either larger than the bound or not decided
  here. For a tile pixel in a hole of bounding box at most *S* = halo, every
  pixel of the hole lies within Chebyshev *S* − 1, so the hole lies inside the
  context clear of its border and is found whole. A tile pixel in a larger
  background component sees it touching the border or with a bounding box
  above *S*, and is not filled in either case. Background beyond the level is
  zero, so a region open to the level edge joins the strip outside the level
  and touches the context's border, which matches the whole-level pass. The
  rule is therefore exact.
- **Implementation:** the module's own 4-connected labelling of the inverted
  context with a bounding box and area per label, or `cv::connectedComponentsWithStats`
  on `Byte`. The plan picks whichever the working-set accounting makes simpler.
- **Reference:** synthetic masks with holes of known size at, just under and
  just over the bound, a hole touching the level edge, nested rings, and a
  brute-force flood-fill on random masks. Exact.

## 3. `segment.watershed`: `minimumMarkerValue`

- **Parameter:** `minimumMarkerValue`, double, **absent by default**, finite.
  With `LocalMaxima`, a pixel whose value is below it is not a marker. It
  still floods and still counts as foreground. With `AuxiliaryRaster` the
  parameter is refused, as a value out of range, because the markers come from
  the raster.
- **Why absent by default:** the 6.2 rule fills every key into the bag. A key
  with a default would change the recorded configuration of every existing
  watershed stage and mark them stale. An absent key leaves those bags as they
  are, as `pixel.threshold_otsu`'s `histogramMinimum` already does.
- **Purpose:** on a distance map, it drops seeds on ragged edges and thin
  bridges (the `dist > 0.3 · r` test of `nucleus_ops.py`).
- **Tiling:** unchanged. The marker test stays local to the
  `minimumMarkerDistancePx` window, and the reach check still applies. A floor
  that leaves a region with no marker fails that check, as any too-sparse
  marker set does. That is the declared behaviour.
  *Amended at PR review (2026-11-25):* one debris speck below the floor would
  fail a whole slide, so the documentation now states settings under which no
  object lacks a marker: open the mask first with an `Ellipse` of radius
  k ≥ max(`minimumMarkerValue`, `minimumMarkerDistancePx` + 1), and set the
  spacing to at least the largest object (SDS 06 §6.7). The reach failure names
  the failing pixel and these settings. Dropping unmarked objects was rejected:
  a tile cannot tell them from objects whose marker lies beyond its halo.
- **Reference:** the 6.9 touching-discs test with a floor that removes an
  edge seed, plus a case where the floor removes every seed and the reach check
  fires.

## Bindings and example

`slideio-processor-python` registers no per-operation code; the catalog is read
at runtime. Its catalog tests that count operations (21) are updated, and the
`segment.watershed` docstring gains the parameter. The nucleus example is then
rewritten to the chain above, its Python operation dropped. That is a separate
commit in the bindings repo, after the library branch merges.

## Left to the builder

The labelling route for `fill_holes`; exact working-set figures (derived from
code, checked by `tests/algorithm/test_working_set_*`); the order of the three
tasks (1, then 3, then 2, as requested: distance transform, marker floor, hole
filling).
