# Watershed Drop gate: checkpoint report

Date: 2026-11-28. Branch `feat/watershed-unreached`. Spec:
`2026-11-28-watershed-unreached-foreground-design.md`, "Tile invariance: the gate".
For the owner's sign-off before phase 2.

## Verdict

One rule passed: **bounded claims plus a halo of `4s + 1`, in Drop mode only.** On
30,000 random inputs and 81 generated shapes it shows 0 mismatches in every class,
0 harness errors and 0 Fail-mode findings. The plain rule failed (5,496 of 30,000
random inputs tile-dependent). Bounded claims alone also failed (167 of 30,000).
The passing rule costs a larger halo in Drop mode: about 1.81x time at `s = 91` with
512-px tiles, and a halo'd area up to about 4x at large `s`. The 288 B per halo'd
pixel budget still holds. "0 mismatches" is evidence over the coverage below, not a
proof. The evidence is strong at spacings 1-4 and weaker at 5-8. Spacing 31 adds
nothing at these image sizes.

## The rules, side by side

Random inputs are seeds 0..29999. Shapes are 81 generated inputs. All three runs
used the same harness and the same coverage (every tile-size role at 1 and 4
workers). Counts are inputs.

| | Plain (Task 4) | Bounded, halo 2s+1 (Task 5a) | Bounded, halo 4s+1 (Task 5b) |
|---|---|---|---|
| (a) tiled vs single-tile, random | **5,496** (objects and mask both: 5,496) | **167** (objects only 38, mask only 0, both 129) | **0** |
| (a) shapes | **48** of 81 (all blob-touching-nucleus, both) | 0 | 0 |
| (b) operation vs model | 0 | 0 | 0 |
| (c) Drop vs robust Fail | 0 | 0 | 0 |
| Harness errors | 0 | 0 | 0 |
| Fail-mode findings | 0 | 0 | 0 |
| Wall time, random inputs | 531 s | 492 s | 1,101 s |
| Result | fail | fail | **pass** |

Class (a) compares tiled Drop with single-tile Drop (objects and the dropped mask).
Class (b) compares single-tile Drop with the reference model, using the model rule
that matches the operation (plain, or bounded). Class (c) compares Drop with Fail on
inputs where Fail runs at every tile size. Plain wall time is from the second Task 4
run (541 s ctest total).

Mechanism of the plain failure, reproduced on shape 30 (0 degrees, spacing 3,
Byte): 10 pixels beside the nucleus are kept at tile sizes 1-16, 20 and 24 and
dropped over one tile. This is the counter-example the design predicted. Bounded
claims stop it at the claim; they left 167 inputs tile-dependent only because
halo `2s+1` did not show every front that can reach a pixel.

## Coverage achieved, against the pass rule

Final run (`task-5-wide-halo-run-2.log`). Image sizes are 16-80 px a side.

| Pass rule item | Achieved |
|---|---|
| At least 30,000 random inputs plus every shape | 30,000 random + 81 shapes (all generated shapes) |
| Pipeline runs | 600,000 random + 3,912 shapes |
| Spacings 1-8 and 31 | 3,367 / 3,387 / 3,266 / 3,339 / 3,289 / 3,430 / 3,393 / 3,278 inputs at 1-8; 3,251 at 31 |
| Both marker modes | LocalMaxima 22,674; AuxiliaryRaster 7,326 |
| With and without `minimumMarkerValue` | floor on 15,154; off 14,846 (never combined with anchors) |
| Tile sizes, 1 px up to twice the halo, non-dividing sizes included | 6 roles per input: 1, 3, 7, halo/2, halo, 2*halo+1, with halo = `4s+1`. Each role: 30,000 inputs at each worker count. Shapes also sweep tiles 1-16, 20, 24, 32, 64 |
| Worker counts 1 and 4 | both, on every input. No input differed between them |
| Sample types | Byte 15,164; Float32 14,836 |
| Generators | distance 7,639; distance/small 7,540; rectangles 3,757; rectangles/small 3,701; blocks 3,680; blocks/small 3,683 |
| Inputs where single-tile Drop drops | 25,969 (5,343,539 pixels) |
| Fail recorded per size | runs at every size on 3,807 inputs; refuses at every size on 26,193; mixed: 0 |
| Named regressions | 15, all pass: Task 4 seeds 1, 8, 13, 14, 22, 26, 188; seeds 176, 433, 695, 1496, 3235 that bounded claims at 2s+1 left tile-dependent; three blob-nucleus shapes (shape 30, 45 degrees capped at 6 spacing 3 Float32, 90 degrees spacing 6 Byte) |

### How many runs could actually disagree

A tiled run only tests invariance if some tile's halo'd context is smaller than the
level. With a halo of `4s+1` on 16-80 px images, big spacings give every tile the
whole image. Per spacing, tiled Drop runs on random inputs (both worker counts):

| Spacing | Halo | Tiled Drop runs | With a context smaller than the level | Share |
|---|---|---|---|---|
| 1 | 5 | 40,404 | 40,404 | 100% |
| 2 | 9 | 40,644 | 40,512 | 99.7% |
| 3 | 13 | 39,192 | 37,596 | 95.9% |
| 4 | 17 | 40,068 | 34,620 | 86.4% |
| 5 | 21 | 39,468 | 27,344 | 69.3% |
| 6 | 25 | 41,160 | 22,006 | 53.5% |
| 7 | 29 | 40,716 | 17,022 | 41.8% |
| 8 | 33 | 39,336 | 12,204 | 31.0% |
| 31 | 125 | 39,012 | **0** | 0% |
| All | | 360,000 | 231,708 | 64.4% |

Shapes: spacings 3-6 have every run non-trivial (480, 532, 504, 504); spacing 8 has
216 of 432; spacing 31 has 0 of 48.

Plain reading:
- **Spacing 31 is trivially invariant at these image sizes.** None of its runs had a
  tile that could not see the whole image. It is not evidence for the rule.
- The evidence comes from spacings 1-8, strongest at 1-4 (86% or more of runs
  non-trivial), then thinning to 31% at spacing 8.
- All 48 blob-nucleus shapes (spacings 3-6) are non-trivial. They are the shapes
  that broke the plain rule.
- `4s + 1` is the design's estimate, confirmed by this harness. It is not proven.
  Covering spacing 31 would need images well beyond 125 px a side.

## Cost of the wider halo

Measured in Task 5, Step 4. The working-set figure used a temporary instrument that
was not committed.

**Memory.** `kBytesPerPixel = 288` still holds. Peak measured per halo'd pixel on
the adversarial working-set inputs (spacing 2, three measured planes, tile 257):

| Mode | Halo | Peak per halo'd pixel |
|---|---|---|
| Fail | 5 | 248.6 B |
| Drop | 9 | 234.6 B |

The per-pixel figure falls because labelling and tracing scale with the tile and
only the flood scales with the halo'd area. The declared working set per tile
still grows with the halo'd area, about 4x at large `s`. Those inputs drop nothing;
a heavily dropping input re-queues pixels, but a pixel is queued at most once at a
time, so the queue bound is unchanged.

**Time.** One-off test, not committed. Input: 2048 x 2048 Float32 field of discs of
radius 6-14, `s = 91`, markerDistance 5, 512-px tiles, 4 workers, best of 3,
6,785 objects in every configuration.

| Configuration | Halo | Wall time |
|---|---|---|
| Fail | 183 | 0.383 s |
| Drop, halo forced back to 2s+1 | 183 | 0.397 s |
| Drop, as committed | 365 | 0.718 s |

- Wider halo: 0.718 s against 0.397 s at the same rule, 1.81x. The halo'd tile area
  is 2.0x here (1242^2 against 878^2).
- Bounded claims alone cost about 4% (0.397 s against 0.383 s).
- Fail mode is unchanged: its halo stays `2s+1` and its path is unchanged.

**New Drop-only limit.** `4s + 1` overflows int32 above `s = 536870911`. Drop now
refuses a larger `maximumMarkerSpacingPx`. Fail's limit (1073741823) is unchanged.
This is visible to callers, though only at absurd spacings. The SDS must say so.

## Caveats

- Built and tested on MSVC only. No GCC or Clang `-Werror` build has run on this
  branch. Linux CI must confirm before merge.
- Random inputs use 6 tile-size roles, not every size from 1 to twice the halo.
  Only the blob-nucleus shapes sweep tiles 1-16.
- Fail's own `2s+1`-scaled boundary sizes (s, 2s+1, 4s+3) no longer run in the
  harness. Fail runs at the Drop-halo sizes. Task 4 covered Fail's own sizes. A
  union of both would restore them.
- Fail runs with 4 workers only. Drop runs at 1 and 4.
- Class (c) rests on 3,807 inputs, about 13% of the random inputs, where Fail runs
  at every size. Blocks/small inputs are rarely Fail-robust.
- The model includes `harness.hpp` for the `Image` type, so it is less independent
  of the harness than its header suggests.
- Spacing above 536870911 in Drop gets only the generic limit message today, not
  Drop's own limit text. It is minor and unfixed.
- Some lines in `test_watershed_model.cpp` exceed the 100-column clang-format limit.
- The model's DropBounded reading, that an unclaimable pixel may be claimed later by
  a neighbour from a nearer seed, is the implementer's. The operation matches it on
  30,081 inputs, but the SDS wording ("its first flooded side neighbour") is
  ambiguous and needs a fix.
- Image sizes are small (16-80 px). Larger images were not run.

## Fail-mode findings

None. In every run (plain, bounded, bounded + `4s+1`), 0 inputs had Fail run at some
tile sizes and refuse at others. In the final run: 3,807 random inputs run at every
size, 26,193 refuse at every size, 0 mixed. On the 81 shapes, 81 refuse at every
size. No UR-QA-3 finding arises from this gate.

## Decision asked of the owner

Approve or reject shipping Drop as **bounded claims plus halo `4s + 1`, Drop mode
only**.

If approved, phase 2 does (Tasks 7-9 of the plan):
1. Counter plumbing: `droppedPixels` from `OperationContext` to
   `SlideRunResult::stageDroppedPixels`.
2. Fail-equivalence and split-basin tests.
3. UR-SEG-8, SDS 06 and 08, and `segment.hpp` changes, including the
   bounded-claims rule, the `4s+1` halo, its cost and the new limit.

Decisions the results raise:
- **Is 1.81x time in Drop mode at `s = 91` acceptable?** Drop is opt-in and Fail is
  unchanged, so the cost lands only on pipelines that choose Drop.
- **Is a larger-image run at `s = 31` wanted before shipping?** At current image
  sizes spacing 31 is untested. A run needs images several times 125 px a side, and
  the harness time grows with them.
- **Should Fail's own boundary sizes return to the harness?** A union of both size
  sets would restore them at some extra run time.

If not approved, Drop does not ship. Fail stays as it is.

## Sources

`task-4-report.md`, `task-4-full-run-2.log`, `task-5-report.md`,
`task-5-bounded-run.log`, `task-5-wide-halo-run-2.log`, `progress.md` (deferred
minors), in `.superpowers/sdd/2026-11-28-watershed-unreached-foreground/`.
