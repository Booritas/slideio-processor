# Analysis resolution — the input `analysisLevel(-1)` never had

**Status:** design note. §2 is a defect report; §3–§5 are a proposal; §6
corrects a document already written.
**Date:** 2026-10-13
**Raised by:** "How is the zoom level for the operation defined?" — which has
no complete answer today.
**Touches:** SDS [05](sds/05-pipeline.md) §5.4, §5.5, §5.6 ·
[04](sds/04-workspace.md) §4.4, §4.5 · [03](sds/03-storage.md) §3.12 ·
`requirements.md` UR-IMG-4, UR-SEG-6, UR-STG-4, UR-STG-8 ·
[multiresolution segmentation design](2026-10-12-multiresolution-segmentation-design.md) §5

---

## 1. How the level is chosen today

The level is a property of the **run**, not of an operation. `PipelineSpec`
carries it, the tile grid is laid out at it, and a chain cannot mix levels
(05 §5.4, §5.6). 06 §6.7 records the rejected alternative — an earlier draft
put `analysisLevel` on `TissueDetectParameters`, which gave the decision two
owners — and a chain needing two levels is two stages, through
`CoarseToFineSpec` (04 §4.2).

None of that is in question. What follows is about the default.

## 2. The defect

```cpp
/// The pyramid level to read from; -1 (the default) lets `LevelSelector`
/// choose the coarsest adequate one (UR-IMG-4).
PipelineSpec& analysisLevel(int32_t);
```

Adequate **for what**? `LevelSelector` offers exactly two ways to answer:

```cpp
int32_t levelForScale(double scale) const;
int32_t levelForSize(int64_t targetWidth, int64_t targetHeight) const;
```

Both need a target, and **nothing supplies one.** `PipelineSpec` carries
channels, two restrictions, the level and the operation chain. `Configuration`
carries tiling and budget. Neither has a scale, a resolution, a target size or
a microns-per-pixel field; the only `micronsPerPixel` in the SDS is
`MaskWriter::resolution` in reporting, which is an output setting for a
different subsystem.

So the mechanism for automatic level selection is fully specified and its input
does not exist. `analysisLevel(-1)` — **the default** — has no defined answer.

### It is a requirement-level gap, not a design oversight

UR-IMG-4 is a Must, and it is written as though the input already exists:

> reading from the coarsest level that satisfies **the requested output
> resolution** rather than decoding level 0 and downsampling.
> *Acceptance:* for **a request at 1/16 resolution** on a slide with a matching
> level, instrumentation shows no level-0 decode.

There is no way to make a request at 1/16 resolution. **UR-IMG-4's acceptance
test cannot be written against the current API**, which makes this the second
requirement in this project whose acceptance criterion outruns its design — the
first being UR-ALG-6 against multiresolution segmentation
([2026-10-12](2026-10-12-multiresolution-segmentation-design.md) §2).

### What it costs downstream

**Every `...Px` parameter in the catalog is level-relative.** §5.6 is explicit:
halo is declared, supplied and applied in level pixels and the framework never
rescales it — "a 5-pixel radius means five of the pixels being processed, at
any level". So `minimumMarkerDistancePx(20)` is 5 µm at level 0 and 20 µm at
level 2 on a 0.25 µm/px slide. With the level undefined, the physical meaning
of every spatial parameter in a chain is undefined with it.

That reaches both things specified this week. The multiresolution seed scale
resolves against `levelPixels`, so it cannot be computed before the level is;
and `pixel.distance_transform`'s `maximumDistancePx` derives a halo whose
physical reach moves with the level.

## 3. Proposal — say what resolution you want

Three ways to state one thing, on `PipelineSpec`:

```cpp
enum class LevelRequest : int32_t {
    ExplicitLevel,       // analysisLevel(n)
    Scale,               // analysisScale(s)
    ResolutionMicrons,   // analysisResolutionMicrons(r)
};

/// An explicit pyramid level. Unchanged.
PipelineSpec& analysisLevel(int32_t);

/// A fraction of level 0: 1.0 is full resolution, 0.0625 is UR-IMG-4's
/// 1/16. Resolved with LevelSelector::levelForScale. Works on an
/// uncalibrated slide, because it asks nothing of the metadata.
PipelineSpec& analysisScale(double);

/// A physical target in microns per pixel, resolved against the scene's
/// reported resolution. Requires a calibrated scene (UR-SEG-6); on one
/// without resolution metadata, validate() throws rather than guessing.
PipelineSpec& analysisResolutionMicrons(double);

LevelRequest levelRequest() const noexcept;
```

**Last setter wins**, which is §5.4's existing convention for single-valued
spec fields ("calling either setter twice **replaces** that restriction"), and
is cheaper than an error path for three setters that mean the same thing. The
request is one discriminated field, so `toJson` and `equals` gain one value,
not three.

**`analysisScale` exists because UR-IMG-4's acceptance is written in scale
terms** and because it is the only form that works on an uncalibrated slide.
**`analysisResolutionMicrons` exists because it is the one a user actually
thinks in** — "segment at 1 µm/px" is a statement about tissue, "segment at
scale 0.25" is a statement about a file. Both reduce to one `LevelSelector`
call that already exists.

**The default, when nothing is set, is level 0.** Not "the coarsest adequate",
because with no stated need nothing is adequate or inadequate — it is the only
choice that cannot silently discard resolution the caller wanted. It is also
the only choice that keeps every `PipelineSpec` in the current SDS valid. The
cost is speed, and the caller buys it back by saying what they need, which is
the behaviour UR-IMG-4 is asking for in the first place.

**It does not make px parameters physical.** Converting the catalog's `...Px`
parameters to microns is a separate and much larger change, and this note does
not propose it. What this buys is that the level is *decidable*, so the
conversion factor is at least knowable.

## 4. Resolution happens in `validate()`, before the grid exists

`Pipeline::validate()` resolves the request to a concrete level, and must do so
**first**, because the tile grid is laid out at that level (§5.6) and because
anything sized against the level depends on it. The ordering is strict:

1. resolve `LevelRequest` → level
2. lay out `TileGrid` at that level
3. resolve anything level-dependent — the multiresolution seed scale, any
   future budget-driven parameter
4. the existing declared-type, arity, halo and budget checks

Rejections: `analysisResolutionMicrons` on an uncalibrated scene; a requested
resolution finer than level 0 provides; a requested level outside the pyramid.
All `InvalidArgumentError` from `validate()`, before a pixel is read.

The whole-level note's [§3.3 rule 3](2026-10-11-whole-level-operations.md)
proposed extending the same call to pick "the coarsest level satisfying the
requested resolution *and* the finest that fits the budget". That rule
inherited the same unsourced phrase and becomes writable only once this exists.

## 5. Declared configuration and resolved configuration are different things

This is the part that is not merely additive.

A resolved value has to be recorded — otherwise a run cannot be reproduced or
explained. The obvious move is to write it back into the spec. **That move is
wrong, and it is already written into one document.**

`PipelineSpec::equals` defines staleness: §4.5's `Reconfigured` reason compares
a stage's declared configuration against its stored one. If `validate()` mutates
the spec, then the stored configuration after a run no longer equals the
configuration the caller declares on the next `declare()`, so the stage is
marked `Reconfigured` and stale **every time, forever**. It is the
non-terminating staleness of a dependency cycle, in miniature, arrived at from
the other direction.

So:

| | Holds | Defines | Written by |
|---|---|---|---|
| **Declared configuration** | what the caller asked for, `analysisLevel(-1)` and all | staleness, via `equals` | the caller |
| **Resolved configuration** | what actually ran — level 2, seed scale 44 px | reproduction and explanation | `validate()` |

The declared form stays exactly as it is. The resolved form is a second JSON
column on the stage record (03 §3.12), written once per run, never compared.
Replay reads the resolved form; staleness compares the declared form.

**04 §4.4 has a related bug.** It records

```cpp
layers.recordAnalysisLevel(def.writtenLayerAt(i), p.analysisLevel());
```

which under the default records **-1** — the request, not the level. It should
record the resolved level, and with §3 in place there is one to record.

## 6. Correction to the multiresolution segmentation design

[2026-10-12](2026-10-12-multiresolution-segmentation-design.md) §5 says, of the
automatically derived seed scale:

> `validate()` writes the resolved seed scale back into the parameter bag, and
> the bag is what a stage records through `Operation::parameters()` and
> `configurationJson`.

**That is the mutation §5 of this note rules out.** The parameter bag is part of
the spec, the spec defines staleness, and an MRS stage under automatic seed
scale would therefore be marked stale after every run.

The conclusion the passage drew is still right — the resolved seed scale must be
recorded, and recording it is what makes automatic mode reproducible. Only the
destination changes: the **resolved configuration** of §5 above, not the
parameter bag. The MRS document needs that paragraph rewritten, and it is the
only change this note forces on it.

Worth noting what did *not* go wrong. The analogy that produced the error —
"this is the same shape as `analysisLevel(-1)` resolving through
`LevelSelector`" — was correct. Both resolve a request into a fact at validate
time, and both need the fact recorded. What the analogy did not carry is that
the existing case never wrote anything back, so it never met the staleness
problem. Copying a pattern from a mechanism that was itself unfinished
reproduced its unfinishedness.

## 7. What this does not do

- **It does not let one chain span levels.** Still two stages, still
  `CoarseToFineSpec`.
- **It does not let an operation request a level.** 06 §6.7 removed that
  deliberately and this note does not reopen it; the request stays on the run,
  where the single tile grid can honour it.
- **It does not make the catalog's spatial parameters physical.** §3.
- **It does not settle the whole-level note's budget rule.** It supplies the
  input that rule was missing and leaves the rule itself open.

## 8. Open

- **Whether `analysisResolutionMicrons` should fall back or throw on an
  uncalibrated scene.** Throwing is proposed, on the three-times-now principle
  that this design rejects rather than approximates. But UR-SEG-6's own
  acceptance takes the other arm for measurements — "a scene lacking resolution
  metadata yields pixel units and a documented indication" — so the two are
  inconsistent in spirit and someone should decide whether that matters.
- **Whether the resolved configuration belongs on the stage record or in the
  event journal.** A column is proposed because it is per-run state with a
  natural owner. The journal (03 §3.9) is the alternative and is already the
  place runs explain themselves.
- **Whether UR-IMG-4 needs amending.** It presumes a requested output
  resolution and the design now supplies one, so arguably not — but its
  acceptance criterion has been unwritable since it was drafted, and that is
  worth a line in the requirements rather than silent repair.
