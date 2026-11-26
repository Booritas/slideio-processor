# Fluorescence tissue detection — design

**Date:** 2026-11-26 · **Status:** draft, for approval · **Requirement:** UR-SEG-1

## Intent

`segment.tissue_detect` finds tissue only on brightfield RGB: a pixel is tissue
when its colour is saturated enough. It refuses any scene that does not have
exactly three channels. Fluorescence and multiplex slides, such as the 5-channel
8-bit PerkinElmer scan `LuCa-7color_Scan1.qptiff` (DAPI, FITC, CY3, Texas Red,
CY5), have dark glass and bright tissue. There, tissue shows up as brightness,
not colour, so the operation needs a mode for it.

**Goal:** add a fluorescence mode that finds tissue on a scene of one or more
channels, with a cut-off picked automatically per slide and the same closing,
labelling and area filter as today.

**Non-goals:**
- Detecting the slide type automatically. The pipeline names the mode.
- Combining channels in other ways (mean, sum, weights). The pipeline's
  channel selection already chooses which channels take part.
- A separate `pixel.channel_max` operation, or a triangle method on the
  threshold operations. Either can come later.
- Greyscale brightfield (dark tissue on light glass).

**Anti-goals:**
- Changing what an existing brightfield pipeline records or produces. A
  workspace built before this change must not turn stale.
- A cut-off that depends on the tile size, worker count or tile order.

## Approaches

These were considered in the conversation of 2026-11-26.
1. **A fluorescence mode in `segment.tissue_detect`** (picked by the user). One
   operation with sensible defaults, following the brightfield rule's shape.
2. **Building blocks:** a new `pixel.channel_max`, a triangle method on the
   threshold operation, and the user chaining combine → threshold → close →
   connected components. More flexible, but four stages for every
   fluorescence pipeline.
3. **Detecting the slide type automatically.** Rejected: it guesses wrong on
   dark-field RGB and on brightfield stored as separate channels.

## Evidence from the example slide

These measurements were made at the three coarsest levels (780, 1560 and 3120
pixels wide):
- Outside the scanned area the pixels are 0. Glass inside it is 1 to 4 in the
  per-pixel channel maximum. Tissue starts at about 5.
- The triangle cut-off on the histogram of the channel maximum is 6, 7 and 7.
  On DAPI alone it is the same. A cut-off of about 5 to 7, followed by a closing
  of radius 2, gives a clean tissue outline with the alveolar spaces left open.
- Otsu on the same histogram picks 22, which cuts holes through the tissue. So
  the automatic cut-off is triangle, not Otsu.

## Design

### Parameters

The existing keys stay unchanged. New keys:

| Key | Type | Default | Range |
|---|---|---|---|
| `mode` | string | `"Brightfield"` | `"Brightfield"` or `"Fluorescence"` |
| `thresholdMethod` | string | `"Triangle"` | `"Triangle"` or `"Fixed"`; fluorescence only |
| `intensityThreshold` | double | none | finite; required with `"Fixed"`, refused otherwise |

- `saturationThreshold` belongs to brightfield. Giving it in fluorescence mode
  is refused, and so are the fluorescence keys in brightfield mode.
- `minimumRegionAreaPx`, `closingRadiusPx` and `targetLayer` apply in both modes.
- **The bag records the new keys only in fluorescence mode.** A brightfield
  operation records exactly the keys it records today, so existing pipeline
  files and workspaces compare equal and do not turn stale. (My call.)
- The typed API gains `TissueDetectParameters::mode(TissueMode)`,
  `thresholdMethod(TissueThresholdMethod)` and `intensityThreshold(double)`.
  The enums are spelled as above.

### Brightfield mode

Brightfield mode is unchanged: Joint over exactly three planes of one type,
Byte or UInt16. There is no global pass.

### Fluorescence mode

- **Input:** Joint over one or more planes of one type: Byte or UInt16 with
  either method, or Float32 with `"Fixed"` only. (My call: a triangle on Float32
  would need a histogram range, as `threshold_otsu` does, and no slide in front
  of us needs it.) Triangle on Float32 is refused with UnsupportedDataTypeError,
  and the message names `"Fixed"` as the way out.
- **The signal:** the per-pixel maximum over the input planes. The pipeline
  spec's channel selection chooses the planes, for example DAPI only.
- **Tissue:** a pixel whose maximum is strictly above the cut-off.
- **Fixed:** the cut-off is `intensityThreshold`, in the input's units.
- **Triangle:** the cut-off comes from a whole-slide pass, using the
  global-context route `threshold_otsu` uses:
  - `needsGlobalContext()` is true only in this case.
  - `accumulate()` counts the channel maximum of the tile rect (not the
    halo), only at the pixels the run's mask admits. Byte uses 256 bins and
    UInt16 65536, with exact 64-bit counts.
  - `combine()` adds the bins. `finalizeAccumulation()` computes the cut-off.
  - The result does not depend on the tile size, worker count or tile order.
  - `accumulatedStateBytes` and `workingSetBytesPerTile` account for the
    histogram, as Otsu does.
- **The triangle algorithm** (Zack, Rogers and Latt, 1977) runs on bin indices:
  - The peak is the highest bin. The tail is the last non-empty bin on the
    farther side of the peak.
  - The cut-off is the bin between them that is farthest from the straight line
    joining them.
  - When the tail lies below the peak (light background), the histogram is
    mirrored, as OpenCV does.
  - Ties take the lowest bin.
  - Byte results must equal `cv::threshold` with `THRESH_TRIANGLE` on the same
    pixels, which is the acceptance oracle. That includes OpenCV's one-bin
    adjustment, or a documented difference if matching it is wrong.
- **Degenerate histograms:**
  - An empty histogram, where the mask admits nothing, finds no tissue.
  - A single non-empty bin gives that bin as the cut-off, which also finds no
    tissue.
- **After the cut-off:** the closing, the 4-connected labelling, the area
  filter, the measurements (`area` and `perimeter`) and the output plane (Byte,
  255 on closed tissue) are those of brightfield mode, with the same code.
- **Memory:** `kBytesPerPixel` grows only if the per-pixel maximum needs a
  buffer. The mask is computed straight into the existing byte buffer, so I
  expect no growth.

### Errors

- A brightfield scene without three channels keeps today's message. It gains
  the hint: "for fluorescence, set mode to Fluorescence".
- A fluorescence scene with zero planes is refused with ChannelArityError.
- Parameter errors name the key, as the other operations' errors do.

### Documents and tests

- **Documents:**
  - `segment.hpp`: the parameter table and the mode documentation.
  - SDS `06-algorithm.md`: the tissue_detect section, the parameter table and
    the working-set table.
  - `requirements.md`: a change note (0.15). UR-SEG-1's text already covers
    any slide, so its wording does not change.
  - `traceability.md` and the catalog's validation text.
- **Tests** (`tests/algorithm`), on a synthetic multi-channel dark-background
  scene with bright blobs and noise:
  - Fixed and Triangle find the blobs' area within 1%.
  - Triangle equals OpenCV's on Byte histograms, both random and shaped.
  - Identical objects at tile sizes 64, 100 and 257, and with 1 and 4 workers.
  - The mask restriction is honoured in the histogram.
  - Byte, UInt16 and Float32 (Fixed) are accepted, and Triangle on Float32 is
    refused.
  - Brightfield bags are unchanged byte for byte.
  - The parameter refusals.
  - The working-set meter test gains the fluorescence path.
- **Not in the repository:** the example slide's check, as with the labelled
  corpus of SDS 06 §6.9. That check: about 41–44% tissue at levels 3 to 5,
  with a cut-off of 6 or 7.

### Python bindings (a separate PR, after the library merges)

`examples/tissue_detection.py`:
- gains `--mode {brightfield,fluorescence}` (default: fluorescence when the
  scene does not have 3 channels) and `--intensity-threshold`;
- drops its RGB-only refusal;
- draws the overlay from the channel maximum for fluorescence.

The README line is updated.

## Left to the builder

- Internal structure: one class with a mode switch, or two classes sharing the
  closing and emission helpers.
- Exact error wording beyond the hint above.
- Whether OpenCV's one-bin triangle adjustment is matched, or the difference is
  documented. The test fixes whichever is chosen.
