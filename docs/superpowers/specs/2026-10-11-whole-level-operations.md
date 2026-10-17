# Whole-level operations — making an unstated condition a declared contract

**Status:** design note. Nothing amended; §3 is a proposal.
**Date:** 2026-10-11
**Raised by:** SDS 06 §6.7's coarse watershed, which already depends on this
capability without saying so, and §6.10's unseeded watershed, which would.
**Touches:** SDS [05](sds/05-pipeline.md) §5.1, §5.4, §5.5, §5.6 ·
[06](sds/06-algorithm.md) §6.7 · [04](sds/04-workspace.md) §4.2 ·
[08](sds/08-class-diagrams.md) D.6 · `requirements.md` UR-SEG-3, UR-EXE-2,
UR-IMG-4

---

> **Amended 2026-10-11.** This note was first raised by the
> multiresolution-segmentation assessment, which appeared to need a whole-level
> contract. It does not: a partition-seeded MRS accumulates over tiles and
> never holds the level, so it fits the existing global-context contract
> unchanged. See [multiresolution
> segmentation](2026-10-11-multiresolution-segmentation.md) §2. **The note
> stands on the watershed cases alone** — §6.7's coarse stage is specified as
> correct on a condition its caller can silently violate, whatever else is
> built.

---

## 1. The gap

`Operation` declares `Halo halo()`. A halo says *"I need this many pixels
beyond my tile."* There is no way for an operation to say **"I need all of
them."**

Tile size is a property of the *configuration*, not of the operation:
`Configuration::tileSize` and `tileSizeFromSlide` (02 §2.1), resolved by
`defaultTileSize` (05 §5.6). It is chosen without reference to what any
operation in the chain requires.

Meanwhile 06 §6.7 already rests a correctness argument on the whole level
arriving in one piece:

> A coarse stage runs at `analysisLevel` 3 or 4, where the whole slide is 10⁸
> pixels or fewer and a whole-slide watershed is an ordinary in-memory
> computation […] The coarse pass is correct because the whole slide fits.

Nothing in the contract delivers that. `tileSizeFromSlide` defaults to **true**,
so the tile size comes from the slide's own tiling — for a pyramidal TIFF,
typically 256 or 512 px at *every* level, including level 3. The coarse stage
as specified therefore receives 512 px tiles and floods each one independently.

**The failure is silent.** Basins come out plausible and wrong, and the object
count is roughly right — which 05 §5.6 itself identifies as the worst failure
available, because nothing looks broken.

This is the same shape as the annotation write-order dependency found in the
2026-10-10 measurements: the system is correct because of how it happens to be
configured, and nothing enforces the configuration.

## 2. Why "just set a large tile size" is not the answer

- **It puts the operation's correctness condition outside the operation.** A
  caller composing a pipeline cannot see that one of its operations has a
  layout precondition, and UR-EXT-2 requires an external author to be able to
  write a built-in-equivalent operation without privileged knowledge.
- **It collides with UR-SEG-3.** Its acceptance criterion is results identical
  across at least three tile sizes. Choosing one specific tile size to make an
  algorithm correct is precisely the thing that criterion exists to detect.
- **Nothing checks it.** There is no validation that the configured size
  actually covers the level, so the condition fails open.
- **It does not interact with the budget.** Whether the level fits in
  `MemoryBudget::ceilingBytes()` is the real question, and tile size does not
  express it.

## 3. Proposal

Three additions. Two virtuals with defaults and one validation rule.

### 3.1 `Operation::tileRequirement()`

```cpp
enum class TileRequirement : int32_t {
    Halo,       ///< the default: this operation needs its tile plus halo()
    WholeLevel  ///< this operation needs the analysis level in one block
};

/// What layout this operation requires. `Halo` for everything tiled, which
/// is every operation in the v1 catalog. `WholeLevel` for an operation whose
/// result is not decomposable — flooding that propagates the length of a
/// connected component, a region-adjacency graph, a global relaxation.
virtual TileRequirement tileRequirement() const;   // default Halo
```

**An enum rather than a `bool`** so a third case — whole row, whole connected
region — can arrive later without another virtual, and because it reads
better at the call site than `requiresWholeImage()`.

**Not a sentinel inside `Halo`.** `Halo` is a POD of four `int32_t` edge widths
used arithmetically throughout `TileGrid::tileRectAtLevelWithHalo` and
`tileRectWithHalo`. A magic value would need special-casing on every one of
those paths, and `Halo::uniform(INT32_MAX)` overflows the rect arithmetic
outright. The sentinel stays out of the arithmetic type.

### 3.2 `Operation::workingSetBytesPerLevelPixel()`

```cpp
/// Bytes of operation-private state per level pixel, excluding the input and
/// output blocks, which the framework already accounts for. Lets the
/// framework decide whether a WholeLevel operation fits the budget at a given
/// level, and choose a level when `analysisLevel` is -1.
virtual int64_t workingSetBytesPerLevelPixel() const;   // default 0
```

This is the member that turns *"it fits at level 4"* from folklore into
something checkable, and it is what separates two whole-level algorithms that
differ by an order of magnitude:

| Operation | Private state per level pixel | Reference slide fits at |
|---|---|---|
| Unseeded watershed | label image plus a boundary priority queue; order 10 B | level 3 marginally, level 4 comfortably |
| Multiresolution segmentation | segment node, RAG adjacency, per-band Σ and Σ², perimeter, bbox; order 100 B | level 5 |

These two figures are estimates made to motivate the mechanism, not
measurements — the entire point of the member is that each operation declares
its own rather than a reader guessing. What is robust is the **ratio**: a
graph with per-segment statistics is an order of magnitude heavier per pixel
than a label image, which costs it one to two pyramid levels.

**Applied to §6.7, that arithmetic is already uncomfortable.** At level 3 the
reference slide is 1.25×10⁸ level pixels. At 12 B that is 1.5 GB, plus ~375 MB
for a 3-channel input block: ~1.88 GB against UR-EXE-2's 2 GiB default. It
fits, but it leaves roughly 270 MB for object accumulation and store buffers,
which 01 §1.11 says the *same* ceiling must cover. So §6.7's "level 3 or 4" is
comfortable only at level 4, and whether level 3 survives depends on an
estimate nobody has written down. A declared working set turns that into a
validation failure with numbers in it, rather than an out-of-memory death an
hour into a run.

### 3.3 Enforcement in `Pipeline::validate()`

`validate()` already checks declared types, arity and halo against the scene.
It gains three rules:

1. **Layout.** If any operation in the chain reports `WholeLevel`, the run is
   laid out as a **single tile** covering the analysis level's region —
   `TileGrid` with `tileCount() == 1` — regardless of `tileSize` and
   `tileSizeFromSlide`. The configuration no longer participates.

2. **Budget.** Compute

   ```
   levelPixels × (inputBytesPerPixel + outputBytesPerPixel
                  + Σ workingSetBytesPerLevelPixel over the chain)
   ```

   and compare against `MemoryBudget::ceilingBytes()`. On exceedance throw
   `InvalidArgumentError` naming the operation, the level, the requirement and
   the ceiling — **before a pixel is read**. This is §6.7's
   `maximumMarkerSpacingPx` pattern, where UR-SEG-8 requires rejection rather
   than merely permitting it.

3. **Level selection.** With `analysisLevel(-1)`, use
   `LevelSelector::levelForSize` to pick the coarsest level satisfying the
   requested resolution *and* the finest that fits the budget, failing when
   the two do not overlap. The hook already exists; only the budget term is
   new.

## 4. What this does to UR-SEG-3

UR-SEG-3's acceptance is *results identical across at least three tile sizes*.
A `WholeLevel` operation satisfies it **vacuously and provably**: tile size no
longer participates in its layout, so there is nothing for three tile sizes to
perturb. That is a stronger guarantee than the halo case, where invariance is
achieved by careful seam handling and demonstrated by test.

The requirement text needs no change. **The test suite does.** The tile-size
sweep must be skipped for whole-level operations, or asserted to be a no-op —
not reported as a pass, which would turn it into a test that cannot fail.

## 5. What this does not solve

- **It does not make unseeded watershed tileable.** It remains a whole-level
  algorithm; what changes is that the level at which it is correct becomes
  checkable instead of assumed. (Multiresolution segmentation was the other
  motivating case and no longer is — seeded from a partition it accumulates
  over tiles and needs no layout contract at all.)
- **It does not provide a multi-pass stage.** A whole-level operation still
  runs once, and the capability named in 06 §6.10 is still absent.
- **It does not bound total memory for a chain of several whole-level
  operations.** The sum in §3.3 assumes their working sets are concurrent,
  which is conservative: it may reject a chain that would in fact fit because
  the operations run in sequence.
- **It does not let one chain span levels.** `analysisLevel` is a property of
  the run, not of an operation (05 §5.4), so a chain mixing a whole-level
  coarse operation with a fine-grained one still cannot exist. Coarse-to-fine
  (04 §4.2, `CoarseToFineSpec`) remains the way to span levels and is
  unaffected by this note.

## 6. Cost and sequencing

Two virtuals with defaults, so every operation in the v1 catalog compiles
unchanged and behaves identically — `TileRequirement::Halo`, zero working set.
The enforcement is new code in one place, `Pipeline::validate()`. D.6.1 of the
class diagrams gains two members.

§3.1 and §3.3 must land together: a declaration without enforcement is worse
than neither, because it reads as a guarantee. §3.2 can follow, but rule 2 of
§3.3 is inert without it.

**This should land before any whole-level algorithm ships, the coarse
watershed of §6.7 included** — that stage is currently specified as correct on
a condition its caller can silently violate, and §3.2's arithmetic suggests the
coarser of the two levels it nominates is the only safe one.

---

## 7. Open

- **The working-set figures in §3.2 are estimates.** They motivate the
  mechanism; they do not settle any operation's level. Each operation's real
  figure arrives with its implementation.
- **Whether `WholeLevel` should force a single tile or merely require one.**
  Forcing it (§3.3 rule 1) overrides an explicit `tileSize` silently. The
  alternative is to throw when the configured size does not cover the level,
  which is noisier but never surprises a caller who set the size deliberately.
  Forcing is proposed because the configuration default — `tileSizeFromSlide`
  — is not a deliberate choice about this operation; but it is a judgement
  call, not a derivation.
- **No requirement covers whole-level operations.** UR-SEG-3 constrains tile
  invariance and UR-EXE-2 the memory ceiling, but nothing states that an
  algorithm may demand a layout. If this proposal is adopted it probably wants
  a requirement of its own, in the way UR-SEG-8 was added for watershed's
  declared reach.
