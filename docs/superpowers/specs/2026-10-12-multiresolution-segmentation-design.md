# Shipping `segment.multiresolution` — design

**Status:** design, approved for planning. Supersedes the proposal half of the
[feasibility note](2026-10-11-multiresolution-segmentation.md).
**Date:** 2026-10-12
**Decision:** scale-controlled region segmentation **enters the v1 catalog**,
which becomes twenty-three operations. UR-SEG-9 gains the entry it lacked and
SDS 06 §6.10's requirement mapping is a derivation again.
**Touches:** SDS [05](sds/05-pipeline.md) §5.1, §5.5, §5.7, §5.8 ·
[06](sds/06-algorithm.md) §6.7, §6.9, §6.10 · [08](sds/08-class-diagrams.md)
D.6.1 · `requirements.md` UR-SEG-9, UR-EXE-2, UR-QA-3, UR-ALG-6, UR-IMG-11 ·
[whole-level operations](2026-10-11-whole-level-operations.md) §3.2

---

## 1. What is being shipped, and the four decisions that shape it

Partition-seeded Baatz & Schäpe region merging, as variant B of the
feasibility note: tiled, running through the existing `needsGlobalContext`
four-phase contract, at level 0, inside UR-EXE-2's ceiling. **No new execution
model.** The note established that; this document decides the parameters the
note left open and corrects two things it got wrong.

| | Decision | Why |
|---|---|---|
| **Partition source** | either internal or external, internal by default | the external path makes UR-ALG-6 satisfiable; it costs one enum and one string |
| **Seed scale vs scale parameter** | both declared, checked before the run *and* after growth | `segment.watershed`'s existing pattern, applied unchanged |
| **Accumulator type** | integer inputs only; float callers convert explicitly | the quantisation belongs in `pixel.convert_type`, which already owns it |
| **Memory** | declared per level pixel, enforced in `validate()`, seed scale derivable from the budget | UR-EXE-2 requires independence from slide size |

Each is argued where it lands below. The one place a reader should look first
is §5, because it is the decision that stops the operation violating a Must.

## 2. Interface — `algorithm/segment/segment.hpp`

```cpp
enum class PartitionSource : int32_t {
    InternalWatershed,   // the default: marker-controlled watershed, per tile
    AuxiliaryRaster      // a bound primitive layer, normally
                         // LayerRasterContent::ObjectAnchor
};

/// segment.multiresolution — Joint, uniform types, halo = maximumMarkerSpacingPx,
/// global context, writes objects.
class MultiresolutionParameters {
public:
    MultiresolutionParameters();

    // Granularity — Baatz & Schäpe
    MultiresolutionParameters& scale(double);
    MultiresolutionParameters& colourWeight(double);        // w_color, 0..1
    MultiresolutionParameters& compactnessWeight(double);   // w_cmpct, 0..1
    MultiresolutionParameters& bandWeights(const double*, int32_t);

    // The partition
    MultiresolutionParameters& partitionSource(PartitionSource);
    MultiresolutionParameters& auxiliaryName(const char*);  // AuxiliaryRaster only

    /// Characteristic linear size of one region of the initial partition,
    /// and the number that drives memory. On InternalWatershed it IS the
    /// marker suppression radius and so sets basin size; on AuxiliaryRaster
    /// it declares a property of the supplied partition, verified during
    /// accumulation. 0 means derive it from the memory ceiling (§5), which
    /// is legal only on InternalWatershed. Whatever it resolves to is
    /// recorded in the run's resolved configuration, never written back
    /// into this bag — see 2026-10-13-analysis-resolution.md §5.
    MultiresolutionParameters& seedScalePx(int32_t);

    /// The bound that makes internal seeding tileable, inherited from
    /// segment.watershed under its own name, meaning and rejection rule.
    /// Derives the halo. A marker is a point; a seed is a region.
    MultiresolutionParameters& maximumMarkerSpacingPx(int32_t);

    /// The finest output the caller intends to resolve. Rejected before the
    /// run when it is below the resolved seed scale.
    MultiresolutionParameters& minimumRegionSizePx(int32_t);

    MultiresolutionParameters& targetLayer(const char*);
};
OperationPtr makeMultiresolutionSegmentation(const MultiresolutionParameters&);
```

**Declarations.** `arity()` is `Joint` and `requiresUniformTypes()` is `true`,
because `h_color` sums over bands and one accumulator type must serve all of
them. `supportedTypes()` is `{Byte, Int8, UInt16, Int16}` — §4. `halo()` is
`maximumMarkerSpacingPx`. `needsGlobalContext()` is `true`.

**The name.** `segment.multiresolution` is the literature's name, so a reader
who has the Baatz & Schäpe paper in hand finds it. The cost is that
"multiresolution" can be misread as pyramid levels, which this is not: the
operation runs at a single `analysisLevel` like every other, and coarse-to-fine
remains a separate mechanism (06 §6.7). `segment.region_merge` was the
alternative and describes the mechanism rather than the citation. Recognisability
won; the ambiguity is documented here and in 6.7 rather than left to be
discovered.

### Marker and seed are different things, and the names say so

An earlier draft of this interface called the reach bound
`maximumSeedSpacingPx`, which was wrong. In `segment.watershed` a **marker** is
a point you flood from; here a **seed** is a whole region of the initial
partition. The bound constrains how far a basin boundary may be decided from —
a property of markers — so it keeps watershed's name, meaning and error
unchanged, which is what UR-ALG-7's convention review is for.

The two paths then use `seedScalePx` differently, and the difference is worth
stating because one is a control and the other is a claim.

| | `seedScalePx` is | Verified by |
|---|---|---|
| `InternalWatershed` | **the marker suppression radius**, which is what sets basin size — so MRS does not expose `minimumMarkerDistancePx` separately; this is that parameter, named for what it means here | construction: the seeder is given it |
| `AuxiliaryRaster` | **a declaration about a partition MRS did not compute**, used to budget memory before reading a pixel | measurement during `accumulate` (§6) |

Collapsing them into one name on the internal path is deliberate. Exposing both
`seedScalePx` and `minimumMarkerDistancePx` would be two knobs for one quantity,
and a caller setting them inconsistently would get a memory budget computed from
one and a partition produced by the other — a silent overrun rather than an
error.

**A declaration MRS cannot check up front has to be checked late.** On
`AuxiliaryRaster` the supplied partition may be finer than declared, in which
case the budget of §5 is simply wrong and the run would exceed the ceiling.
`accumulate` therefore counts the seeds it actually finds and throws when the
realised density materially exceeds the declared one. For the same reason
`seedScalePx(0)` is rejected on this path: there is nothing to derive it from
without reading the partition, and `validate()` runs before any pixel is read.

### Why the partition may come from outside

The feasibility note proposed internal seeding only, on the ground that an
external partition costs a prior stage and a measured 10.8 minutes writing ten
million objects that are discarded immediately
([2026-10-10](2026-10-10-storage-measurements.md)). That reasoning holds for
*production* use and is why internal is the default.

It does not hold for **UR-ALG-6**, which requires a documented correctness
reference and a test asserting agreement within a stated tolerance. §9 of the
note established that the only defensible reference is the published definition
*applied to the supplied partition* — and a partition computed internally from
level-0 gradients cannot be supplied. Without `AuxiliaryRaster` the requirement
is unsatisfiable by anything stronger than visual inspection.

So the external path exists primarily to make the operation testable, and
secondarily because it composes: any partition a user can import through GeoJSON
(03 §3.11), or produce with `segment.watershed`, becomes a legal seeding.
The mechanism is `readsLayerAsRaster` with `LayerRasterContent::ObjectAnchor`
(04 §4.2), already built for `WatershedMarkers::AuxiliaryRaster`, so the cost
is one enum, one string parameter, and no new machinery.

## 3. Execution — the four phases

| Phase | Work | Resident |
|---|---|---|
| `accumulate(ctx)` | seed the tile; accumulate node attributes and shared-boundary lengths | one tile + halo, plus this worker's partial graph |
| `combine(other)` | graph union: node attributes add, edge attributes add, bboxes unite | the merged partial |
| `finalizeAccumulation()` | mutual-best-fitting merge steps to a fixed point | the graph, **no pixels** |
| `apply(ctx)` | **re-seed the tile**, map seeds to final regions, emit | one tile + halo, plus the seed-to-region map |

The growth phase touches no pixels, which is the whole argument for tileability
and is unchanged from §2 of the feasibility note: every term of the fusion
factor is a per-segment aggregate and every aggregate is additive under a merge.

Seeds are identified by decision 8's anchor — topmost-then-leftmost pixel — so a
seed straddling a tile boundary is computed identically by both tiles with no
communication. Double counting is prevented by the two ownership rules of the
note's §4.3: node attributes count only pixels inside the owning tile's rect,
never its halo; an adjacent pixel pair is counted by the tile holding the pixel
on the canonical side.

### `apply` re-seeds, and that doubles the pixel cost

A merged region's geometry is the union of its seeds' geometry, and **seed
geometry is never retained**: §5's budget holds a bbox per seed, not a polygon,
and five million polygons do not fit. `apply` therefore re-runs the seeder on
its tile, maps each seed to its final region through the union-find built in
`finalizeAccumulation`, and emits the union.

This is correct for the reason §4.3 already relies on — seeding is
deterministic, so the second pass reproduces the first exactly — but it means
the feasibility note's §7 cost table was **wrong**. `apply` is not "negligible";
it is a second full streaming pass with a watershed in it.

| Phase | Corrected cost |
|---|---|
| `accumulate` | one streaming pass at the analysis level, seeder included, fully parallel |
| `finalizeAccumulation` | ~10 s; single-threaded by design, node count falls geometrically |
| `apply` | a second streaming pass, seeder included, fully parallel |
| seed store traffic | zero |

Two passes rather than one. Against UR-EXE-8's thirty minutes, of which the
2026-10-10 measurements account for ~13.4 minutes of writes, this is still
comfortable — but it is twice what the note claimed and the planning should
budget it.

The alternative, retaining per-tile label images or seed polygons across
phases, costs either 8×10⁹ pixels of state or five million polygons. Neither
fits. Re-seeding is not a compromise; it is the only affordable option, and
determinism is what makes it exact rather than approximate.

### Seam load inverts §5.7's assumption

`segment.watershed` sends a minority of its objects through `SeamMerger`:
nuclei are tens of pixels across against tiles of hundreds, so most never touch
a seam. MRS at level 0 produces 10⁴–10⁵ regions averaging roughly 300 × 300 px
against 512 px tiles, so **nearly every output region touches a seam** and goes
through §5.7.

This is not a correctness problem — §5.7 bounds fragment residency and spills —
but it is the opposite of the load the section was sized against, and 05 §5.7
should say so rather than let it be found in a profile.

## 4. Types and determinism — UR-QA-3

**§5.8 already states the rule this needs, for a different class.**
`Reduction::isOrderIndependent()` exists precisely so float accumulators are
combined in canonical tile order and UR-QA-3 cannot fail intermittently across
worker counts. `Operation::combine(const Operation&)` has no such declaration,
because the global-context group has only ever served whole-slide Otsu, whose
accumulator is a histogram.

§5.8's remedy does not transplant. Canonical tile order for a *graph*
accumulator means holding one region-adjacency graph per tile until the fold,
and the graph is the memory.

**So the operation restricts its input types instead.** With `Byte`, `Int8`,
`UInt16` or `Int16` input, `Σ` and `Σ²` accumulate exactly in `int64`:

| Input type | `Σ²` bound at 10⁹ px in one segment | Status |
|---|---|---|
| `Byte`, `Int8` | 6.5×10¹³ | exact |
| `UInt16`, `Int16` | 4.3×10¹⁸ | exact, within `int64` |
| 32- and 64-bit integers | overflows `int64` | unsupported in v1 |
| `Float16/32/64` | not associative | unsupported in v1 |

Integer addition is associative, so `combine` is order-free, worker count
cannot perturb a merge decision, and no canonical ordering or per-tile graph is
needed.

**Float callers convert explicitly, and that is better than converting
silently.** `pixel.stain_deconvolution` outputs `Float32` (06 §6.3) and optical
density is the most useful input this operation has, so excluding floats is not
a corner case. The caller inserts `pixel.convert_type` with a declared target,
`RoundingMode` and `OverflowMode`.

The arithmetic is identical to quantising inside MRS. What differs is where the
precision loss is visible. §6.6 calls `pixel.convert_type` "the one place a
caller opts into it deliberately", and UR-IMG-11 forbids every other operation
from narrowing silently. An MRS that quantised internally would be the single
exception to a rule the catalog states about itself — and the exception would
be invisible in the recorded configuration, which is exactly where a
reproducibility question gets asked.

Rejection needs no new code: UR-IMG-11's existing mechanism raises an error
naming the operation and the type.

**Merging uses local mutual best fitting**, not the best-neighbour
simplification of Happ et al. A pair merges only when each is the other's best
partner, which makes an iteration's merge set order-independent by construction
rather than by visiting discipline. Ties break on anchor order. It costs
nothing and removes the whole class of reproducibility risk that Happ's frontier
machinery exists to manage.

**05 §5.1 gains a stated rule, not an API:** a global-context accumulator must
be exactly associative under `combine`, and an operation whose accumulation
would not be must restrict `supportedTypes()` accordingly. That is §5.8's rule
written for the group that lacked it, and it records why this operation's type
list looks narrow.

## 5. Memory, the seed scale, and UR-EXE-2

UR-EXE-2 requires peak memory bounded by a ceiling **"independent of both slide
size and object count"**. MRS's accumulator is proportional to seed count, and
seed count is level pixels ÷ `seedScalePx²`. At a fixed seed scale, memory grows
with slide size. The feasibility note stated the figures without noticing they
collide with a Must.

### What the operation declares

`Operation` gains `workingSetBytesPerLevelPixel()`, taken from the
[whole-level note](2026-10-11-whole-level-operations.md) §3.2 — **that member
alone**, not §3.1's `tileRequirement` and not §3.3's layout forcing, neither of
which this operation needs. MRS returns:

```
320 / seedScalePx²
```

320 bytes per seed is the note's 160-byte node total at the upper end of its
1.5–2× mutable-adjacency allowance; merging takes the union of two neighbour
lists, so a static CSR figure would understate it. At a 40 px seed scale that is
0.2 bytes per level pixel, or 1.6 GB for the reference slide's 8×10⁹ pixels,
which reproduces the note's five-million-seed estimate.

### What the framework adds

The declared figure is the **converged, single-graph** cost. Peak is higher,
because each worker holds a partial graph and seeds on tile boundaries appear in
two of them. The operation cannot compute that — it does not know the tile grid
— so `validate()` applies the allowance:

```
D(s) ≈ 1 + 2s / tileEdge
```

At a 40 px seed scale against 512 px tiles, 1.16×. The framework knows both
numbers; the operation knows neither. That division is why the member declares
per level pixel rather than a total.

### Automatic resolution, and what it costs

`seedScalePx(0)` means *derive it*. `validate()` solves for the finest seed scale
satisfying

```
320 · D(s) · levelPixels / s²  ≤  accumulatorBudgetBytes
```

by bisection, where `accumulatorBudgetBytes` is `MemoryBudget::ceilingBytes()`
less the per-worker tile blocks and store-buffer reservation that `validate()`
already computes for the existing budget check. On the reference slide at a
2 GiB ceiling that resolves to **about 44 px**, or 11 µm at 0.25 µm/px —
nucleus-to-small-cluster scale, which is the right primitive for tissue
architecture.

An explicitly given seed scale is never adjusted. It is checked against the same
inequality and rejected before a pixel is read.

**This is what satisfies UR-EXE-2.** The default call bounds memory by the
ceiling on a slide of any size, and what varies with slide size is output
granularity, not memory. The alternative — reject and make the caller derive a
seed scale from a memory budget by hand — is compliant and a poor answer.

**The caveat, stated because it is real:** in automatic mode the same slide at a
different ceiling yields a different partition. That is not a UR-QA-3 failure —
QA-3 governs worker count and tile size, both byte-identical here — but it must
be documented.

**It is also recorded, which is what makes it safe** — but not in the parameter
bag. An earlier draft of this section said `validate()` writes the resolved
seed scale back into the bag, which is wrong: the bag is part of the spec, the
spec defines staleness through `equals`, and a stage whose stored configuration
differs from the one its caller declares is marked `Reconfigured` after every
run, permanently (04 §4.5). The resolved seed scale goes into the run's
**resolved configuration**, a second record that is written once per run and
never compared — see
[analysis resolution](2026-10-13-analysis-resolution.md) §5, which proposes it
for `analysisLevel` and finds the same need here.

With that in place, a run under automatic mode records the number it chose and
replaying that record reproduces the partition even against a different ceiling.

**There is also an ordering dependency.** `seedScalePx(0)` solves against
`levelPixels`, so the analysis level must be resolved first. That ordering is
specified in [analysis resolution](2026-10-13-analysis-resolution.md) §4, and
until the level request of that note exists, the default level is undefined and
so is the derived seed scale.

It also discharges UR-SEG-9's "reported with the result" clause with no new
mechanism — though the requirement's wording needs amending, since it currently
says the seed scale is *declared by the caller*, and under this design it may
instead be derived and reported. §10.

## 6. Errors — seven rejections, no degradation

| Condition | Where | Error |
|---|---|---|
| `minimumRegionSizePx < seedScalePx` (resolved) | `validate()` | `InvalidArgumentError`, naming both values |
| Accumulator exceeds the budget at an explicit seed scale | `validate()` | `InvalidArgumentError`, naming the seed scale, the implied bytes and the ceiling |
| Float, or 32/64-bit integer, input | `validate()` | UR-IMG-11's existing error, naming the operation and the type |
| A basin boundary depends on a marker beyond `maximumMarkerSpacingPx` | runtime, in `accumulate` | `segment.watershed`'s existing reach error, unchanged |
| `seedScalePx(0)` with `AuxiliaryRaster` | `validate()` | `InvalidArgumentError`: nothing to derive the scale from before reading |
| A supplied partition materially finer than the declared `seedScalePx` | runtime, in `accumulate` | `InvalidArgumentError`, naming declared and realised seed counts |
| Growth merged essentially nothing | after `finalizeAccumulation` | `InvalidArgumentError`, naming `scale` and the resolved seed scale |

The first and last are the two halves of UR-SEG-9's bound, and they are two
halves rather than one because `scale` is not a length. `f < scale²` compares
against a weighted heterogeneity increase whose units are variance × pixel
count, so no arithmetic comparison between `scale` and a pixel distance is
possible. The declared floor catches the stated mistake cheaply, before a pixel
is read; the post-growth check catches the case where the floor was satisfied
and the scale still resolved nothing.

That two-level shape is not new. 06 §6.7 declares `maximumMarkerSpacingPx` up
front *and* throws at runtime when an actual pixel violates it, for the same
reason: the declaration is checkable and the violation is not.

## 7. Framework changes

| Where | Change |
|---|---|
| 05 §5.1 | `virtual int64_t workingSetBytesPerLevelPixel() const;` default 0 |
| 05 §5.1 | stated rule: global-context accumulators must be exactly associative, or restrict `supportedTypes()` |
| 05 §5.5 | `validate()`: accumulator budget check, boundary-duplication allowance, seed-scale resolution and write-back |
| 05 §5.7 | note that a global-context segmentation inverts the seam-load assumption |
| 08 D.6.1 | the new member |

Every existing operation compiles unchanged and behaves identically: the default
is zero, and zero times anything is the budget the framework already computed.

The member is deliberately the one the whole-level note proposed, rather than a
parallel `accumulatorBytesPerLevelPixel`. The semantics are the same —
operation-private state per level pixel, excluding input and output blocks — and
the difference between a whole-level working set and a cross-tile accumulator is
how `validate()` sums it, which `needsGlobalContext()` and `tileRequirement()`
already tell it. One member, two users, and §8 of the feasibility note called
them siblings for this reason.

Adopting §3.2 alone leaves the whole-level note's own fate untouched. It stands
or falls on 06 §6.7's coarse watershed, as its 2026-10-11 amendment says.

## 8. Correctness and tests

§6.9 gains a row:

| Operation | Reference kind |
|---|---|
| Multiresolution segmentation | Published fusion-factor definition applied to a supplied partition, on a synthetic mosaic of known regions; region count and per-region area exact |

The reference is exact rather than tolerance-bounded in the supplied-partition
case, because with `AuxiliaryRaster` and a known partition the output **is** the
published algorithm — the approximation of §9 lives entirely in where the seeds
come from, not in what is done with them. That is the test UR-ALG-6 needs and
the reason decision one went the way it did.

Beyond it:

- **UR-SEG-3** — identical results across at least three tile sizes.
- **UR-QA-3** — identical results across worker counts, which the integer
  accumulator and mutual best fitting make structural rather than incidental.
- **Monotonicity** — increasing `scale` decreases region count, on the synthetic
  mosaic and on a real slide.
- **The seven rejections of §6**, each asserted to fail with its own error rather
  than to produce a plausible partition.
- **Round-trip** — `create(name(), parameters())` after `validate()` has
  resolved an automatic seed scale yields an operation that reproduces the run,
  which is the conformance test of 05 §5.1 extended to the write-back.

## 9. What this is not

**It is not Baatz & Schäpe's published algorithm on pixels.** Boundaries can
only fall on seed edges. Because internal seeds are watershed basins computed
from level-0 gradients, those edges follow real image structure at full
precision — a constrained choice among genuine boundaries, not a coarse grid —
but it is not the same output and no tuning makes it so. The seed scale is the
declared parameter bounding achievable boundary precision, in the same spirit as
`maximumMarkerSpacingPx`.

**It does not provide a multi-pass stage.** The iteration lives inside
`finalizeAccumulation`, within one operation. 06 §6.10's gap is untouched.

**It does not make unseeded watershed shippable**, and it does not adopt the
whole-level contract.

**It does not change coarse-to-fine.** MRS at level 0 may be the coarse half of
a pair, with `readsLayerAsRaster` refining its regions.

## 10. Document changes

| Document | Change |
|---|---|
| `requirements.md` | → 0.12. The catalog size appears twice: line 1259 is live and changes; **line 66 is inside the 0.5 changelog and stays at twenty-two on purpose**, because a changelog records what was true then. UR-SEG-9's "v1 ships nothing for it" rationale rewritten; **the "declared by the caller" clause amended** to admit the derived-and-reported case of §5; question 7's amendment row reverted — the mapping is a derivation again |
| `sds/06-algorithm.md` | catalog table → twenty-three; mapping row UR-SEG-9 → `segment.multiresolution`; the "One row no longer holds" paragraph removed; "Not in v1: scale-controlled region segmentation" replaced by the operation's specification in §6.7; §6.9 row |
| `sds/05-pipeline.md` | §5.1 member and rule, §5.5 validation, §5.7 seam-load note |
| `sds/08-class-diagrams.md` | D.6.1 member |
| `sds/README.md` | line 78, "the twenty-two-operation catalog of 06 §6.10" |
| `2026-10-11-multiresolution-segmentation.md` | status: decided; §7's cost table and §11's open items resolved against this document |
| `2026-10-11-whole-level-operations.md` | §3.2 adopted independently; §3.1 and §3.3 still open |
| `2026-10-13-analysis-resolution.md` | prerequisite: the resolved-configuration record of its §5 and the level-resolution ordering of its §4 |
| `project.md` | link this document |

## 11. Open

- **Band weights default.** Equal weighting across bands is the obvious default
  and is what the paper assumes, but H&E after deconvolution has two meaningful
  channels and a residual, and weighting the residual equally is probably wrong.
  Left to implementation with a documented default.
- **`D(s) ≈ 1 + 2s/tileEdge` is a first-order estimate.** It is the fraction of
  seeds whose extent crosses a tile edge. It is conservative for small seeds and
  should be replaced by a measured figure once an implementation exists, which
  is what the declared member makes checkable.
- **The 320 bytes per seed is an estimate**, carried from the feasibility note's
  §5 at the pessimistic end of its range. Same remedy.
- **Two "materially" thresholds are unquantified.** The post-growth "merged
  essentially nothing" check and the `AuxiliaryRaster` partition-density check
  both reject on a margin this document does not set. A fraction of unmerged
  seeds and a ratio of realised to declared seed count are the obvious forms;
  the numbers, and whether either is a parameter, are not settled here. They
  are the only two places in §6 where a rejection is not exact.
