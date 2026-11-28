# Watershed: dropping foreground no marker reaches — design

**Date:** 2026-11-28 · **Status:** draft, for approval · **Requirement:** UR-SEG-8 (amended)

## Intent

`segment.watershed` throws when a foreground pixel of a tile lies further than
`maximumMarkerSpacingPx` from the marker its basin reached it from, or is reached
by no marker (`watershed.cpp`, the reach check after `flood.run()`). The throw is
deliberate. It is UR-SEG-8's "rejected, never segmented approximately", and it is
what keeps the result the same at every tile size.

In practice one stray object fails a whole stage. The fluorescence nucleus
example in slideio-processor-python failed on `LuCa-7color_Scan1.qptiff` at 823
of 3332 tiles. The cause was one dim DAPI strand on the tissue edge, 6 × 43 px
(3 × 21 µm), against a spacing of 31 px. The tiles already done were lost, and
the only remedies were in the chain: `pixel.remove_large_objects` (2026-11-26)
or a larger spacing. Both need the caller to know beforehand how large the
largest object will be, on every slide the pipeline meets.

**Goal:** an opt-in watershed mode in which foreground no marker reaches within
the spacing becomes background instead of failing the run. The mode is reported
in the run's results, and its output is the same at every tile size.

**Non-goals:**
- Changing the default. `"Fail"` stays the default and behaves exactly as today.
  Recorded pipelines, stage definitions and workspaces made before this change
  are untouched.
- Replacing `pixel.remove_large_objects`. That stays the way to keep large
  non-nucleus areas out of the result. Drop is a safety net, so one object
  nobody expected does not cost a long run.
- Dropping whole objects. An object can span many tiles, and a tile that does
  not see the out-of-reach part has already emitted its own. That would need a
  pass after the tiled one, which this execution model does not have (SDS 06).
- Skipping failed tiles. Which area is lost would then depend on the tiling,
  which breaks UR-QA-3.

**Anti-goal:** a Drop result that depends on the tile size, worker count or tile
order. A count that comes out *nearly* right is the failure UR-SEG-3 and UR-SEG-8
exist to prevent. **If Drop cannot be shown to give the same result at every tile
size, it does not ship** (see "Tile invariance: the gate").

## Approaches

1. **A watershed parameter, `unreachedForeground` = `"Fail"` | `"Drop"`
   (chosen).** Where the reach check throws today, Drop clears the pixel instead
   and counts it. It is opt-in, every existing behaviour stays the default, and
   the change is local to one operation plus one new run counter.
2. **Build `remove_large_objects` into watershed.** Rejected: it saves one chain
   step and changes nothing the chain cannot already do. It also still needs the
   caller to size the largest object in advance, which is the problem.
3. **Tolerate failed tiles in the runner.** Rejected: the lost area depends on
   tile size (UR-QA-3), and the failure stays silent in the object count.

### What changed since the 2026-11-26 spec rejected this

`2026-11-26-remove-large-objects-design.md` rejected "a watershed option to
drop foreground no marker reaches" for two reasons. Both still stand, and this
design answers them rather than dismissing them:

- *"It needs a new argument that competing basins still agree across tiles,
  probably a wider halo."* True. That argument is the main risk here, and it is a
  gate before any production code: see "Tile invariance: the gate".
- *"It keeps fake fragments."* True. A marker inside a large area still floods
  the part within reach, and that part is emitted as an object. That is why Drop
  is not the cleanup tool and `remove_large_objects` stays the recommended chain
  step. Drop trades a failed run for some fragments and a non-zero count that
  tells the caller to look.

## Behaviour

- **Parameter.** `unreachedForeground`, a string: `"Fail"` (default) or
  `"Drop"`. C++: `enum class WatershedUnreached : int32_t { Fail, Drop }` and
  `WatershedParameters& unreachedForeground(WatershedUnreached)`, beside the
  other setters in `segment.hpp`. Any other spelling is rejected as a bad value,
  like `markers`.
- **Recorded only when `"Drop"`.** This follows the `minimumMarkerValue`
  precedent: a Fail operation's parameter bag is byte for byte what it is today,
  so no stage definition, recorded configuration or workspace fingerprint
  changes.
- **Fail** is unchanged, messages included.
- **Drop.** Each tile pixel the reach check would refuse is treated as
  background. That covers both branches of `failReach`: reached from a seed
  further than the spacing, and reached by no seed. The pixel takes no basin,
  joins no object, writes 0 to the output label plane, and contributes nothing
  to any object's measurements. Every other pixel is decided exactly as in Fail
  mode.
- **Identical to Fail where Fail is robust.** On any input that Fail runs without
  throwing **at every tile size**, Drop drops nothing and returns the same
  objects, ids, measurements and output plane, and reports a count of 0. This is
  a hard requirement. It is what makes switching an existing pipeline to Drop
  safe.
  - An input Fail runs at one tile size but refuses at another is outside it.
    There, Fail's own "success" depends on the tiling, so it cannot be the
    yardstick, and Drop is held only to tile invariance. *Decided by the owner,
    2026-11-28.*
  - The gate's harness checks Fail's pass/fail across tile sizes as well. Any
    such input it finds is logged as a Fail-mode UR-QA-3 finding, separate from
    this change; it does not block Drop.
- **A basin cut by dropped pixels.** Clearing pixels can split a basin's part
  within a tile into several 4-connected pieces. They are emitted as any basin
  whose pieces are disjoint within a tile is emitted today. Basins can already
  reach a tile through the halo in two places, so the labelling (`labelRegions`
  joins by `keyOf`) and the seam merge already handle it. The builder confirms
  this with a test rather than assuming it.
- **No halo change** at first. Halo stays `2 · maximumMarkerSpacingPx + 1`,
  unless the gate shows a wider one is needed (below).

## Reporting: `droppedPixels`

A new run counter, beside the truncated-objects counter and plumbed the same
way:

`OperationContext::countDroppedPixels(int64_t)` → `TileEvaluator` counters →
`Pipeline::droppedPixelCount()` → `StageRunner::droppedPixels()` →
`SlideRunResult::stageDroppedPixels(int32_t)`.

This change ends there, in C++. The Python half is a second PR in
slideio-processor-python after this one merges (see "Follow-ups"). *My call:*
Python cannot see the counter without a re-pin, so one PR across both
repositories is not possible.

- The unit is **level pixels at the analysis level**, counted once per tile pixel
  (tile rects are disjoint, so once per level pixel per pass). Unlike
  `truncatedObjects`, this figure **does not depend on tile size**, provided the
  drop set does not. The tests compare it across tile sizes too.
- Same pass semantics as `countTruncatedObjects`: validated and dropped in an
  accumulation pass, counted into a discarded total for a lazy read, summed over
  the final pass, left as counted when a run fails.
- *My call:* a new counter rather than reusing `countTruncatedObjects`. Those
  are objects counted once per tile view, these are pixels counted once, and
  summing the two would mean nothing.
- The builder checks whether the framework can evaluate one tile twice in one
  pass (retries, on-demand tiles). If it can, the counter must not double-count,
  and the builder states how that is guaranteed.

## Tile invariance: the gate

Today's invariance argument covers only inputs that pass the reach check. SDS
06 says so: tie-breaking between basins is "tested, not proven", backed by "an
exact model of the flood" and about 28,000 random inputs. Drop brings the
refused inputs into scope, so that evidence would not carry over even if it
were at hand. It is not: the model and harness are in neither repository, and
the commit that added the claim (4488076) changed only documentation. The
owner confirms it is lost (2026-11-28), so the gate starts by writing a new
one.

**A likely counter-example to the plain rule above.** A large blob whose peak F
is more than `2s + 1` from a tile touches a nucleus whose marker N lies within
`s` of tile pixel P, near the blob's edge:
- In a context that does not contain F, the blob's visible part may have no
  marker. On a distance transform its maximum sits at the context edge, where
  `detectMaxima` declines to decide. So N floods it, P is labelled N within
  reach, and P is **kept**.
- In a larger context that contains F, F's front can reach P first from higher
  values. P is then labelled F beyond reach and is **dropped**.

Under Fail this is harmless: the second tiling fails, and the first fails at
some other tile of the blob. Under Drop it is exactly the anti-goal. It is a
hypothesis from reading `Flood::run`, not yet a reproduced case.

**The gate, before any production code:**
1. **Write a reference model and a random-input harness**, as a test target in
   `tests/algorithm/`, kept in the repository this time.
   - The model is an independent, deliberately simple implementation of the
     flood as SDS 06 "Watershed, exactly" defines it, run on the whole image in
     one context: no tiles, no halo. It covers Fail and every Drop rule below.
   - The harness runs the real `segment.watershed` through the tiled framework
     and compares it with the model on each input.
   - Inputs: random tie-heavy rasters (few distinct values, plateaus, the
     distance-transform shapes the nuclei chain produces), plus generators for
     the shapes that trigger drops: a strand longer than the spacing, an
     unmarked blob, and the blob-touching-a-nucleus case above.
2. **The pass rule** (*my call*; the owner reviews it at the checkpoint):
   - Zero mismatches in the drop set, the objects (ids, pixels, measurements)
     and the dropped count across at least 30,000 random inputs plus every
     generated shape.
   - Spacings 1 to 8, plus 31.
   - Both `markers` modes; with and without `minimumMarkerValue`.
   - Tile sizes from 1 pixel up to twice the halo, including sizes that do not
     divide the image; worker counts 1 and 4.
   - Fail's pass/fail is recorded per tile size alongside (see "Identical to
     Fail where Fail is robust").
3. **Checkpoint: stop for the owner's sign-off** before any production code.
   Report:
   - which rule passed, or the counter-examples if none did;
   - the inputs and parameters actually covered;
   - any Fail-mode findings;
   - if step 4b was needed, its memory and time cost at `s = 91` against the
     current halo.

   The plan ends its first phase here. *Decided by the owner, 2026-11-28.*
4. **If the plain rule fails**, try in order and re-run the harness each time,
   all before the checkpoint:
   a. **Bounded claims.** In Drop mode a seed may not claim a pixel further than
      `s` from itself, so fronts stop at the spacing. This changes the flood
      only in Drop mode. It must still keep "identical to Fail where Fail is
      robust", which the harness checks explicitly, because a bounded front
      can change how halo pixels are claimed.
   b. **A wider halo in Drop mode,** so every seed whose front can reach a pixel
      on any path to P is in view. Estimate: about `4s + 1` (paths within `2s`
      of P, seeds within `s` of those, plus the marker window). That is roughly
      four times the halo'd area per tile, and the per-pixel memory figure
      (`kBytesPerPixel`) has to be re-measured with it.
5. **If neither passes,** the checkpoint report says so and Drop does not ship.
   That outcome is acceptable. Shipping a tile-dependent count is not.

Once the harness exists, SDS 06's "28,000 inputs" sentence is replaced by a
reference to it and to the figures it produced for Fail mode.

## Requirement and design-document changes

- **UR-SEG-8**, in `docs/requirements.md`, with a "Changes in" entry at the next
  version. Keep "rejected with a diagnostic naming the operation and the
  distance" as the default. Add: *the caller may instead declare that
  foreground beyond the reach is to be left out of every object. The result
  then remains invariant to tile size, and the run reports how much was left
  out.* Extend the acceptance criteria to match: three tile sizes give identical
  objects and dropped counts on inputs that trigger drops.
- **SDS 06**, "Watershed, and why it is tileable after all": a paragraph on Drop,
  why it does not reopen the silent-degradation hazard (it is opt-in and
  counted), and the invariance evidence the gate produced. Add the parameter row
  to the parameter table.
- **SDS 08** class diagram: the new setter and enum.
- `segment.hpp` doc comment on `maximumMarkerSpacingPx`. It currently says
  "Exceeding it is an error, never a silent approximation"; add "unless
  `unreachedForeground` is `Drop`, which leaves it out and counts it".

## Tests

- **Strand longer than the spacing.** Fail throws exactly as today. Drop keeps
  the part within `s` of the marker as one object, clears the rest, and
  `droppedPixels` equals the analytic count.
- **Object with no marker.** Drop clears all of it, and the count equals its
  area.
- **Fail equivalence.** Every existing watershed and nuclei-chain test input
  (`test_watershed.cpp`, `test_nuclei_chain.cpp`, `test_working_set_segment.cpp`)
  is run again in Drop mode, with bit-identical output and a count of 0.
- **Tile invariance.** Drop-triggering inputs at three tile sizes and two worker
  counts give identical objects, ids, measurements and counts, plus the gate's
  random harness.
- **Split basin.** A basin cut in two by dropped pixels within one tile, and one
  cut across a seam, emit as they would at a tile size where the cut does not
  happen.
- **Parameters.** Fail records no `unreachedForeground` key. Drop round-trips.
  A bad spelling is rejected. A pipeline file written before the change loads
  and runs unchanged.
- **Reporting.** `SlideRunResult::stageDroppedPixels` and
  `Pipeline::droppedPixelCount` report the dropped count, 0 in Fail mode, with
  the same pass semantics as the truncated counter.

## Follow-ups (a second PR, in slideio-processor-python)

- Re-pin slideio-processor at the merged commit.
- Grow the native result tuples in `src/stages.cpp` and `src/slide_run.cpp` by
  one field, and add `dropped_pixels` to the stage result
  (`slideio_processor/workspace.py`) and the run result (`run.py`).
- Have `examples/nucleus_segmentation.py` print the count when non-zero.
  Neither the Python CLI nor the examples print `truncated_objects` today
  beyond that example, so there is no CLI pattern to follow. Whether the CLI
  summary prints either count is that PR's call.
- Nothing else needs exposing: operation parameters already pass through
  `PipelineSpec.add(...)` by name.
- Whether the nucleus example opts into Drop is a separate decision. It already
  avoids the failure with `remove_large_objects` and its clump spacing.

## Left to the builder

- The reference model's structure and the harness's input generators, within
  the pass rule above.
- Counter names, if `droppedPixels` collides with something.
- How the double-evaluation case for the counter is handled, if it exists.
- How the halo cost at step 4b is measured, if that step is reached.

## Calls made in this design, for review

- The parameter and spellings: `unreachedForeground`, `"Fail"` / `"Drop"`.
- Recorded only when Drop, after `minimumMarkerValue`.
- A new pixel counter rather than reusing truncated objects.
- The order of fall-backs in the gate (bounded claims before a wider halo), and
  "do not ship" as the last resort.
- The gate's pass rule: 30,000 inputs, spacings 1 to 8 plus 31, tile sizes up
  to twice the halo, zero mismatches.
- Two PRs: C++ here, Python reporting after a re-pin.

Decided by the owner on 2026-11-28: write a new reference model (the old one is
lost); hold Drop to Fail-equivalence only where Fail passes at every tile size;
stop at a checkpoint for sign-off before production code.
