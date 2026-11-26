# Removing large objects from a mask — design

**Date:** 2026-11-26 · **Status:** draft, for approval · **Requirement:** UR-PIX-9 (new)

## Intent

`segment.watershed` fails the whole run when any foreground lies further than
`maximumMarkerSpacingPx` from a marker. That refusal is what keeps its result
identical at every tile size. In practice a nucleus chain fails on the first
stained area too large to be a nucleus: a keratin band, a fold, dark
cytoplasm under a cut-off that is too low. On `CMU-1-Small-Region.svs` the H&E
nucleus example fails on every region tried. At a suitable cut-off (0.9) it
runs, but the keratin band comes out as dozens of fake nuclei.

A stained area wider or taller than the largest nucleus is not a nucleus.
Removing it from the mask before the distance transform stops the failure and
drops the fake fragments, and watershed's guarantee stays as it is.

**Goal:** a pixel operation, `pixel.remove_large_objects`, that removes every
foreground component whose bounding box is wider or taller than a set size, with
a result identical at every tile size.

**Non-goals:**
- Changing `segment.watershed`. Its reach check and messages stay as they are.
- An area bound or a lower size bound. Small specks are already removed by the
  opening and by watershed's `minimumAreaPx`.
- Fixing a cut-off that is far too low. At 0.3 on CMU-1 the epidermis forms one
  region with its nuclei, and removing it removes them too. The threshold stays
  the caller's to set.

**Anti-goal:** a result that depends on the tiling, or on which tile sees an
object first.

## Approaches

1. **New `pixel.remove_large_objects` (chosen).** Mirrors `pixel.fill_holes`,
   which already decides "is this component small enough?" within a halo the
   size of the bound. Watershed is unchanged and the operation is reusable, for
   example to clean tissue masks.
2. **A watershed option to drop foreground no marker reaches.** Rejected. It
   needs a new argument that competing basins still agree across tiles, probably
   a wider halo, and it keeps fake fragments: a marker inside a keratin band
   still floods the part of the band within reach.

## Behaviour

- PerChannel, every input type, output `Byte`. A pixel is foreground when its
  sample is neither 0 nor NaN, the rule `fill_holes` and `segment.watershed`
  use.
- An object is an 8-connected foreground component, the usual pair to
  `fill_holes`' 4-connected background.
- A pixel's output is 255 when it is foreground and its component's bounding box
  is at most `maximumObjectSizePx` wide and at most `maximumObjectSizePx` high;
  otherwise 0.
- Halo `maximumObjectSizePx` on every side. A component that touches the halo'd
  context's border reaches at least that far from a tile pixel, so it is wider or
  higher than the bound and is removed. Every other component lies whole inside
  the context and is measured exactly. So the result does not depend on the
  tiling, by the argument `fill_holes` already makes.
- Pixels beyond the level are zero. A component cut by the level edge is
  measured by its pixels inside the level.
- Parameter `maximumObjectSizePx`: an integer from 1 to 4096, the same range as
  `maximumHoleSizePx`; 32 when created by name without it. Typed factory
  `makeRemoveLargeObjects(int32_t maximumObjectSizePx)` next to
  `makeFillHoles` in `filter.hpp`. By name, an unknown key, a value of the wrong
  type or out of range throws InvalidArgumentError naming the key, never the
  value.
- Working-set declaration and memory follow `fill_holes`.

## Watershed documentation

The "settings that never fail the reach" paragraph gains a third route: with
`pixel.remove_large_objects` at a size no larger than `maximumMarkerSpacingPx`
after the opening, every remaining object is within the spacing, so the reach
holds even when the cut-off lets large stained areas through.

## Tests (the builder's call where not listed)

- Synthetic masks: objects at, one below and one above the bound in width and
  in height are kept, kept and removed; a diagonal 8-connected chain counts as
  one object; NaN is background; every input type gives the same result.
- An object crossing a tile seam, and one larger than the tile: identical
  results at three tile sizes (UR-QA-3 style), and equal to a brute-force
  whole-image reference.
- By-name creation, parameter round trip, refusals.
- A chain test: threshold → open → remove_large_objects → distance_transform →
  watershed on a mask with one large blob that fails today now runs and keeps
  the small objects.

## Documentation and traceability

`filter.hpp` docs, SDS 06 (pixel operations) and SDS 08 (class diagram),
`docs/requirements.md` (0.16, UR-PIX-9 (Should): the removal of objects larger
than a caller-declared size from a binary mask; acceptance: removes synthetic
objects above the bound, keeps those within it, identical at three tile sizes),
`docs/traceability.md`.

## Bindings, after the library merges

`examples/nucleus_segmentation.py` adds the step after `pixel.morph_open` in both
chains, sized to the watershed spacing (the largest nucleus). The docstring's
"a stained area larger than --max-nucleus-size-um fails the run" becomes "is
removed". Pin bump in the same PR.
