# Multiresolution segmentation — partition-seeded, as a global-context operation

**Status:** design note. Nothing amended; §3–§7 are a proposal.
**Date:** 2026-10-11
**Question:** can Baatz & Schäpe multiresolution segmentation be implemented in
the current design, and how?
**Sources:** Baatz & Schäpe 2000; Happ, Ferreira, Bentes, Costa & Feitosa 2010,
`docs/algorithms/MULTIRESOLUTION-SEGMENTATION.pdf`
**Touches:** SDS [05](sds/05-pipeline.md) §5.1 (the global-context group),
§5.4 · [06](sds/06-algorithm.md) §6.7, §6.10 · [01](sds/01-core.md) §1.11 ·
`requirements.md` UR-QA-3, UR-EXE-2, UR-ALG-6, UR-SEG-8 ·
[whole-level operations](2026-10-11-whole-level-operations.md)

---

## 1. Verdict

**Yes, in one of three variants.** The one that works runs at level 0, needs no
new layout contract, and writes nothing to the store beyond its own output.

| | **A** whole-level, pixel-seeded | **B** partition-seeded, tiled | **C** full-resolution tiled |
|---|---|---|---|
| Initial segments | one per pixel | ~5×10⁶ seed regions | one per pixel at level 0 |
| Execution model | one whole-level block | `accumulate`/`combine`/`finalizeAccumulation`/`apply` | needs a multi-pass stage |
| Memory driven by | pixels at the analysis level | **seed count, not pixels** | ~800 GB graph |
| Finest level fitting 2 GiB | level 5 | **level 0** | none |
| Design changes | the whole-level contract | none to the layout; two smaller items (§8) | four structural blockers |
| | works, but 8 µm/px | **proposed** | not buildable |

Variant C is the faithful implementation and is listed to make the cost of
larger-than-memory explicit. It needs a multi-pass stage (absent, 06 §6.10),
object reads inside an operation (absent, `OperationContext` has a sink and no
source), an adjacency relation in the store (absent — `parent_edges` is
containment, `derived_from` is primitive-to-semantic), and a full layer rewrite
per iteration at 10.8 minutes per ten million objects, measured
([2026-10-10](2026-10-10-storage-measurements.md)).

**Note on the source paper.** Happ et al. is a shared-memory multicore speedup,
not an out-of-core method: its tiles are thread partitions of one in-memory
image and its largest test is 2,800 × 2,800 px. Its own conclusion names the
out-of-core version as future work. It supplies a parallelisation strategy,
not a larger-than-memory one.

## 2. The property that makes it tileable

MRS is bottom-up region merging. Each segment is compared with its neighbours,
the **fusion factor** `f` measures the heterogeneity increase from merging, and
the merge happens when `f` is below the squared scale parameter.

```
f        = w_color·h_color + (1 − w_color)·h_shape
h_color  = Σ_c w_c ( n₃σ₃ᶜ − (n₁σ₁ᶜ + n₂σ₂ᶜ) )
h_shape  = w_cmpct·h_cmpct + (1 − w_cmpct)·h_smooth
h_cmpct  = n₃·(l₃/√n₃) − ( n₁·(l₁/√n₁) + n₂·(l₂/√n₂) )
h_smooth = n₃·(l₃/b₃)  − ( n₁·(l₁/b₁)  + n₂·(l₂/b₂)  )
```

Every term is a **per-segment aggregate**, and each one is additive under a
merge:

| Quantity | Merge rule | Where it comes from |
|---|---|---|
| pixel count `n` | `n₃ = n₁ + n₂` | node attribute |
| per-band `σ` | from `Σ₃ = Σ₁+Σ₂`, `Σ²₃ = Σ²₁+Σ²₂` | node attribute |
| bbox perimeter `b` | bbox₃ = bbox₁ ∪ bbox₂ | node attribute |
| perimeter `l` | `l₃ = l₁ + l₂ − 2·shared(1,2)` | node attribute **plus one edge attribute** |

Only perimeter needs anything beyond node state: the shared boundary length
between two adjacent segments. Accumulate that as an edge attribute during the
tile pass and **the growth phase touches no pixels at all.** It is pure graph
work on a structure whose size is the segment count.

That is the whole argument. It is why MRS does not need the whole level, and
why variant A — which does — is unnecessary.

## 3. Mapping onto the existing global-context contract

`Operation` already declares `needsGlobalContext()` with
`accumulate`/`combine`/`finalizeAccumulation` (05 §5.1), today serving
whole-slide Otsu. MRS fits the same four phases; only the accumulator differs.

| Phase | Work | Resident |
|---|---|---|
| `accumulate(ctx)` | partition this tile (§4), accumulate node attributes and shared-boundary lengths | one tile + halo |
| `combine(other)` | graph union: node attributes add, edge attributes add, bboxes unite | the graph |
| `finalizeAccumulation()` | growth steps to a fixed point | the graph, **no pixels** |
| `apply(ctx)` | emit merged regions as polygons through `objectSink()` | one tile |

Otsu's accumulator is a histogram; MRS's is a region adjacency graph. The
contract does not care, which is the point — **no new execution model is
required.**

## 4. Seeding, internally

### 4.1 Why not pixels

One segment per pixel at level 0 is 8×10⁹ nodes. At the per-node cost of §5
that is roughly 1.3 TB, against UR-EXE-2's 2 GiB. Pixel initialisation confines
MRS to level 5 — 8 µm/px, where a nucleus is one pixel — which is variant A.

Baatz & Schäpe requires an initial *partition*, not single pixels. Seeding from
a coarser partition decouples the graph from the pixel count and is what moves
the algorithm to level 0.

### 4.2 The partition is computed inside `accumulate`, not read from the store

Three sources were considered:

| Source | Store cost | Note |
|---|---|---|
| **Internal, halo-bounded** | **none** | proposed |
| A prior stage's primitive layer, via `readsLayerAsRaster` + `ObjectAnchor` | **10.8 min** per 10M seeds, measured | any external partition, incl. GeoJSON import |
| v1's marker-controlled watershed | as above | it is the previous row's producer |

The second is fully composable with machinery that already exists, and it costs
a measured 10.8 minutes writing ten million objects that are discarded
immediately after. Operations in a chain pass pixel blocks only — objects go to
the sink, never to the next operation — so there is no way to feed a seeder's
output to MRS within one stage. That is what makes the internal option worth
the extra work inside the operation.

Internally, the seeder is a **marker-controlled watershed on the tile**, which
is exactly v1's `segment.watershed` (06 §6.7) with its `minimumMarkerDistancePx`
setting seed density and `maximumMarkerSpacingPx` bounding the halo. MRS
inherits that bound and its rejection rule, which is UR-SEG-8's declared-reach
pattern rather than a new one.

### 4.3 Seeds are identified by anchor, which makes tiles irrelevant

A seed straddling a tile boundary is computed by both tiles. Because seeding is
halo-bounded and deterministic, both compute the same geometry, and decision
8's anchor — topmost-then-leftmost pixel — gives both the same identifier
without any communication.

Double counting is then avoided by ownership rules rather than by coordination:

- **Node attributes**: a tile accumulates only the pixels inside its own tile
  rect, never its halo. Each pixel is counted exactly once, by exactly one tile.
- **Edge attributes**: an adjacent pixel pair is counted by the tile containing
  the pixel on the canonical side of the pair. Each boundary segment is counted
  exactly once.

Both rules are local, need no agreement between workers, and give the same
totals for any tiling.

## 5. Memory and scale

Per seed, with the project's 64-bit discipline for counts and areas:

| Field | Bytes |
|---|---|
| `n` | 8 |
| `Σ` per band × 3 | 24 |
| `Σ²` per band × 3 | 24 |
| perimeter `l` | 8 |
| bbox | 32 |
| id / union-find parent | 8 |
| adjacency, degree ≈ 6 | 48 |
| **node total** | **≈ 160** |

A static CSR adjacency would give ~9–10 million seeds in 1.5 GB. **Merging
requires a mutable adjacency**, since a merged node takes the union of two
neighbour lists, so plan on 1.5–2× that: **roughly 5 million seeds within a
2 GiB ceiling.**

At level 0 the reference slide is 8×10⁹ px, so 5×10⁶ seeds is ~1,600 px each —
**about 40 × 40 px, or 10 µm at 0.25 µm/px.** Nucleus to small-cluster scale,
which is the right primitive for tissue architecture.

Two things make this comfortable rather than marginal. The segment count only
**decreases** during growth, so peak memory is at initialisation and is known
before the first merge. And UR-EXE-2's ceiling is configurable: the UR-EXE-8
reference configuration has 32 GB of RAM, so an 8 GB ceiling gives 20 million
seeds and 20 × 20 px primitives.

## 6. Determinism — UR-QA-3

Happ et al. devote much of their design to reproducibility, deferring frontier
segments and interleaving them across tiles, because *"if the seeds are visited
in a different order, the result of the segmentation is modified."* Most of that
machinery is unnecessary here, for three reasons.

**Seed identity is tile-independent** (§4.3), so there are no frontier segments
to reconcile — only attribute sums, which are order-free.

**Prefer local mutual best fitting over best-neighbour.** The original
Baatz & Schäpe criterion merges a pair only when each is the other's best
partner; the paper simplifies to best-neighbour with one visit per segment per
iteration. Mutual best fitting makes the *merge set of an iteration*
order-independent by construction, so determinism stops depending on visiting
order at all. Ties are settled by anchor order. This is a recommendation, not a
restatement: it costs nothing and removes a whole class of reproducibility risk.

**`combine` must be exactly associative, which is a data-type question.**
Floating-point addition is not associative, so merging per-worker accumulators
in arbitrary order perturbs `Σ²`, perturbs `σ`, and can flip a merge decision —
a byte-identical-output failure under UR-QA-3 that would appear only at some
worker counts. Integer accumulation is exact and associative, and the input
type decides whether it is available:

| Input type | `Σ²` accumulator | Status |
|---|---|---|
| `Byte`, `Int8` | `int64` — max 6.5×10¹³ at 10⁹ px | exact |
| `UInt16`, `Int16` | `int64` — 4.3×10¹⁸ at 10⁹ px | exact, near the limit |
| `UInt32`, `Int32`, `UInt64`, `Int64` | overflows `int64` | needs 128-bit |
| `Float16/32/64` | — | needs a fixed reduction order or compensated summation |

The decision is cheap now and expensive later. It should be made before
implementation, not during debugging.

## 7. Cost against UR-EXE-8

| Phase | Cost |
|---|---|
| `accumulate` | one streaming pass over the slide at the analysis level — an ordinary tiled operation, fully parallel |
| `finalizeAccumulation` | 5×10⁶ nodes × degree 6 × k iterations. ~1.5 s for the first iteration; the node count falls geometrically, so **order 10 s total** |
| `apply` | 10⁴–10⁵ merged regions — negligible against the 10.8 min for 10⁷ objects |
| seeds | **zero store traffic** |

The growth phase is single-threaded by design, so Happ's 2.6× speedup is
unavailable on it. At ten seconds that does not matter; it is recorded because
it is a ceiling, and because it is the part that grows if seed counts rise.

## 8. What the design needs

Neither item is a blocker, and both are broader than MRS.

**The global-context contract declares no memory cost.** Otsu's accumulator is
a histogram, so nothing ever needed one; a region adjacency graph is over a
gigabyte. `Pipeline::validate()` cannot currently reject a chain whose
accumulators will not fit, and `combine(const Operation&)` means W workers each
hold a partial graph, so peak exceeds the final graph by boundary duplication.
This is the sibling of `workingSetBytesPerLevelPixel` proposed in the
[whole-level note](2026-10-11-whole-level-operations.md) §3.2, and belongs with
it.

**The accumulator type decision of §6** — 128-bit or fixed-order float
reduction for the wider input types.

## 9. What this is not

**It is not Baatz & Schäpe's published algorithm.** Boundaries can only fall on
seed edges. Because the seeds are watershed basins computed from level-0
gradients, those edges follow real image structure at full precision — the
result is a constrained choice among genuine boundaries, not a coarse grid —
but it is not the same output, and no amount of tuning makes it so.

That matters for **UR-ALG-6**, which requires a documented correctness
reference and a test asserting agreement within a stated tolerance. The
reference cannot be "eCognition's output". It has to be either the published
definition *applied to the supplied partition* — which is exact and testable on
synthetic input — or the pixel-initialised result at a scale where the two
provably coincide. The seed scale becomes a declared parameter bounding
achievable boundary precision, in the same spirit as
`maximumMarkerSpacingPx`.

## 10. What this does not change

- **No multi-pass stage.** The iteration lives inside `finalizeAccumulation`,
  within one operation. 06 §6.10's gap is untouched.
- **Unseeded watershed is still out**, and the
  [whole-level note](2026-10-11-whole-level-operations.md) still stands on its
  own merits — §6.7's coarse watershed depends on a layout condition its caller
  can silently violate, whether or not MRS is ever built. What changes is that
  MRS is no longer a motivation for it.
- **Coarse-to-fine is unaffected.** MRS at level 0 may still be the coarse half
  of a pair, with `readsLayerAsRaster` refining its regions.

## 11. Open

- **Seed scale against scale parameter.** Both control output granularity and
  they interact. Whether seed scale should be derived from the scale parameter,
  or stay independent with a validation rule relating them, is unresolved and
  is the main thing to settle before an API is drawn.
- **The 5M seed figure assumes degree 6 and a 1.5–2× mutable-adjacency
  overhead.** Both are estimates; the real figures arrive with an
  implementation, and §8's declared accumulator cost is what makes them
  checkable rather than assumed.
- **~~No requirement covers MRS.~~ Resolved 2026-10-11:** `requirements.md`
  0.11 adds **UR-SEG-9 (Should)**, scale-controlled region segmentation, with
  the seed scale as a declared and enforced bound in the shape of UR-SEG-8's
  declared reach. The requirement states the capability; it does not name this
  algorithm.

  **What that resolves and what it does not.** There is now a stated need, so
  building this is no longer catalog surface against nothing. But v1 ships no
  operation for it, which makes UR-SEG-9 the first Should in that document
  without a catalog entry and breaks the bidirectional mapping that was open
  question 7's answer (06 §6.10, amended). Three ways to close it — ship
  `segment.multiresolution` and make the catalog twenty-three, downgrade
  UR-SEG-9 to a *May*, or hold it for a later release and say so — and the
  choice belongs to whoever sets v1's scope, not to this note. §8 and the
  first two bullets above are the technical prerequisites either way.
