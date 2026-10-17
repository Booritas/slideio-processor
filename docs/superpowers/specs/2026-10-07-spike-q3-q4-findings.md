# Spike findings — open questions 3 and 4

**Status:** findings reported, no document amended yet
**Date:** 2026-10-07
**Resolves:** `requirements.md` open questions 3 and 4, both marked load-bearing
**Touches:** `project.md` key decisions · SDS [05](sds/05-pipeline.md),
[06](sds/06-algorithm.md) §6.3, [03](sds/03-storage.md) · `requirements.md`
Assumption 2, UR-IMG-2, UR-OBS-1, open questions 3, 4, 5, 10

Both questions were spiked before implementation, as
[the SDS README](sds/README.md) asked. Question 3 resolves **no** and
invalidates a stated key decision. Question 4 resolves **yes**, with a
performance finding that is load-bearing for the store schema.

Everything built for this spike is throwaway and was kept out of the project
tree. Nothing in `docs/` has been amended; the consequences in §5 are proposals,
not changes.

---

## 1. Question 3 — do float intermediates fit SlideIO's `Scene` model?

> *If not, Assumption 2 fails and the pipeline needs its own raster type.*

The question splits in two, and the halves have opposite answers.

### 1.1 The type model carries floats — yes

`slideio::DataType` (`core/slideio_enums.hpp:64`) declares `DT_Float16`,
`DT_Float32` and `DT_Float64` alongside the integer types. More to the point,
the mechanism is already exercised: `TransformerScene` holds a per-channel
`m_channelDataTypes`, populated from each transformation's
`computeChannelDataTypes()` (`transformer/sobelfilter.cpp:22`), so a filter
declares the output type of every channel independently of its input. Sobel and
Laplacian already use it to widen 8-bit input to a signed or float result.

Nothing about carrying float intermediates through a scene-shaped abstraction is
unsolved. SlideIO does it today.

### 1.2 But `slideio-processor` cannot reach the mechanism — no

The catch is reachability, not representation. Compiled against
`build/public-include` — the staged tree that is exactly what an out-of-tree
consumer of the package sees:

| Probe | Intent | Result |
|---|---|---|
| `#include "slideio/slideio/sceneinternal.hpp"` | reach the `CVScene`→`Scene` factory | `C1083` — not installed |
| `#include "slideio/core/cvscene.hpp"` | implement a scene of our own | `C1083` — not installed |
| `new slideio::Scene(cvScene)` | construct one directly | `C2248` — private ctor, `scene.hpp:389` |
| subclass public `Transformation`, call `transformScene` | supply our own operation | **compiles, links, runs** |

The first three fail exactly as the headers say they should. `scene.hpp:389`
carries the comment *"a Scene is made by the library from a CVScene, never by a
consumer, who has no way to name the type"*, and
`core/public-headers.cmake` records why `cvscene.hpp` is withheld: it includes
`<opencv2/core.hpp>`, and shipping it would make OpenCV a public dependency of
SlideIO. The exclusion is deliberate and reasoned, not an oversight.

The fourth row is the dangerous one, and it is worth stating precisely because
the failure is silent.

`slideio::Transformation` is public, abstract, and has exactly one pure virtual —
`getType()`, returning the closed enum `TransformationType`. A consumer can
therefore subclass it, and the subclass compiles. At runtime
`transformScene(scene, Transformation&)` calls `makeTransformationCopy`, which
is a switch over that enum whose every arm looks like
(`transformer/transformations.cpp:25`):

```cpp
GaussianBlurFilter& filter = (GaussianBlurFilter&)source;
std::shared_ptr<TransformationEx> transformation(new GaussianBlurFilter(filter));
```

That is a C-style downcast of the consumer's object to an unrelated type,
followed by a copy-construct reading whatever lies at those offsets. There is no
`dynamic_cast`, no type check, no diagnostic.

Measured, against a real slide: a consumer-defined `MyStainDeconvolution`
returning `TransformationType::GaussianBlurFilter` was accepted,
`transformScene` returned a working `Scene`, and the operation performed was a
Gaussian blur with parameters read out of an object that was never a
`GaussianBlurFilter`. The caller is given no indication that its operation was
discarded.

> An earlier reading of this spike predicted a thrown exception, on the strength
> of the `dynamic_cast<TransformationEx*>` at `transformer/transformerscene.cpp:38`
> and its adjacent `RAISE_RUNTIME_ERROR << "TransformScene: invalid
> Transformation"`. That guard never fires for this case: it runs against the
> *copy* produced by `makeTransformationCopy`, which is always a valid internal
> type. The substitution happens upstream of the check. Undefined behaviour, not
> a diagnosed error, is the actual outcome.

So the public `Transformation` base is not an extension point. It has the shape
of one, which is worse than not having it at all.

### 1.3 What this costs

`project.md` asserts two key decisions that cannot both hold:

| Decision | Status |
|---|---|
| *"Relationship to SlideIO — separate library depending on SlideIO's public reader API"* | holds |
| *"Raster execution — lazy pipeline exposed as a `Scene`"* | **unreachable under the first** |

SDS 05 rests on the second. `requirements.md` Assumption 2 — *"the SlideIO
`Scene` contract is stable enough to implement against"* — fails on
*implement against*: the contract is stable, and closed.

### 1.4 Options

1. **`pipeline` defines its own raster type.** Open question 3 resolves "no",
   Assumption 2 is rewritten, SDS 05 and SDS 06 §6.3 are reworked. SlideIO stays
   a pure reader dependency and the two projects keep independent release
   cycles. Interop with SlideIO is then by reading a `Scene` at the source and
   writing through `slideio-converter` at the sink, which both already support.
2. **Add a public extension seam to SlideIO** — an installed abstract scene
   interface that does not leak OpenCV into the public headers, plus a public
   factory. SlideIO is maintained in the same tree, so this is more available
   here than a third-party dependency change would be. It is still an API
   addition to SlideIO with its own design, review and release cost, and it
   couples slideio-processor's schedule to SlideIO's.
3. **Consume SlideIO's internal headers.** Cheapest now. It builds on headers the
   project explicitly reserves and deliberately does not install, and it makes
   OpenCV a shared ABI surface between the two projects. Not recommended.

**Recommendation: option 1**, unless the SlideIO extension seam of option 2 is
wanted for its own sake. Option 1 is the only one that leaves the two projects
independently releasable, which `project.md` lists as a key decision in its own
right.

Either way, a separate defect report against SlideIO is warranted for §1.2: a
public base class whose misuse is undiagnosed undefined behaviour should either
reject unknown subclasses or stop being public.

---

## 2. Question 4 — does SQLite + R\*Tree sustain the reference object count?

> *If not, the store needs a different backing and UR-BLD-5 changes.*

**It does.** 10 million objects were inserted and queried, twice, with a
concurrent reader throughout one of the runs. No failure, no corruption, no
unbounded growth. UR-BLD-5 stands.

The finding that matters is not whether SQLite copes but what the **object
identifier scheme costs**.

### 2.1 What was measured

A C harness on the SQLite amalgamation, modelling SDS 03: an `objects` table
keyed by a 64-bit id with a 200-byte geometry blob, an `objects_rtree` virtual
table, and an `events` journal row written **in the same transaction** as the
batch it describes (UR-OBS-2). 10,000 objects per transaction, over a
100,000 × 80,000 coordinate space.

Two runs, identical but for the identifier:

- **anchor-derived** — `id = (y << 32) | x` from the object's anchor pixel, as
  SDS 03 and the UR-QA-3 note specify. Sparse, large, and in random B-tree order
  by construction.
- **sequential** — a monotonic counter. The control.

### 2.2 Results

| | Anchor-derived | Sequential | Ratio |
|---|---|---|---|
| Total insert time | 1,895.1 s (31.6 min) | **461.5 s (7.7 min)** | **4.1×** |
| Mean insert rate | 5,277 obj/s | **21,667 obj/s** | 4.1× |
| Store size | 2.91 GB | 2.53 GB | 1.15× |
| Primary-key collisions | 80,750 (0.81%) | 0 | — |

Per-million interval rates, in obj/s:

| Million | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 |
|---|---|---|---|---|---|---|---|---|---|---|
| Anchor | 39,021 | 8,910 | 8,077 | 4,135 | 5,223 | 3,963 | 6,243 | 4,735 | 3,295 | 3,664 |
| Sequential | 102,493 | 67,981 | 53,999 | 43,732 | 13,066 | 17,521 | 14,508 | 17,754 | 10,553 | 23,780 |

Both degrade once the working set outgrows the 256 MB page cache, and both are
noisy afterwards — WAL checkpoints and OS cache behaviour, not a smooth curve.
The anchor run degrades roughly four times harder and never recovers.

Spatial queries, 50 random viewports per row, against the completed stores:

| Viewport | Anchor: R\*Tree | Anchor: + geometry | Seq: R\*Tree | Seq: + geometry | Rows |
|---|---|---|---|---|---|
| 1,024 px | 0.56 ms | 5.75 ms | 0.46 ms | **2.07 ms** | ~1,370 |
| 4,096 px | 4.14 ms | 38.68 ms | 3.81 ms | **18.42 ms** | ~21,000 |
| 16,384 px | 43.36 ms | 436.32 ms | 40.99 ms | **235.65 ms** | ~334,000 |
| 65,536 px | 612.44 ms | 6,475.46 ms | 577.45 ms | **3,924.05 ms** | ~5,320,000 |

Query cost scales with **result size, not store size** — the property the design
needs. R\*Tree lookup alone is near-identical between the runs, as it must be:
the spatial data is the same. The difference is entirely in the join to
`objects`, where scattered anchor ids cost roughly **2×** in random page reads.

The 65,536 px row returns half the store and is not a real case; a viewer at that
zoom reads a pyramid level, not 5.3 million objects. The realistic viewer
viewport — 1,024 to 4,096 px at level 0 — costs 2–39 ms including geometry.

### 2.3 UR-OBS-1 — concurrent reader

Measured during an active 10M write, for 60 s from a second process:

| | |
|---|---|
| Queries completed | 69,477 |
| Errors | **0** |
| Mean latency | 0.86 ms |
| Worst latency | 23.39 ms |
| Writer rate during reader load | 39,021 obj/s |
| Writer rate unloaded (1M calibration) | 35,915 obj/s |

Both halves of UR-OBS-1 hold: the reader was never blocked and never saw a
partial batch, and the writer showed no measurable slowdown — it ran marginally
*faster* under load, which is run-to-run variance, not an effect. WAL with
`synchronous=NORMAL` delivers what the requirement asks for.

### 2.4 What this does not settle

The acceptance bar. Open question 10 sets no absolute throughput target, so
"8 to 32 minutes to insert 10 million objects" can be reported but not graded.
**Question 4 cannot be fully closed until question 10 is answered.**

### 2.5 The schema change worth measuring next

Not measured, and so not asserted — but it follows directly from §2.2 and should
be tested before SDS 03 is frozen:

> Keep the anchor-derived identifier as the **logical** identity UR-QA-3
> requires — stable, geometry-derived, tile-size and worker-count invariant, and
> the only identifier ever exported — but make the **physical** primary key a
> monotonic rowid, with a unique index on the anchor id.

The random inserts then land in a comparatively small index rather than scattered
across a 2.9 GB table, and the geometry join becomes a clustered read. The risk
is that a unique index on a random 64-bit key carries its own random-insert cost;
whether the smaller node size recovers enough of the 4.1× is an empirical
question, not a deducible one. One short run settles it.

Sorting each batch by anchor id before insert is a cheaper partial measure worth
testing in the same run.

---

## 3. Side findings

Three findings outside the questions asked, recorded because each touches a
numbered requirement.

### 3.1 `Scene::getBlockSize` overflows above 2 GB

`slideio/slideio/scene.cpp:140` computes

```cpp
const int planeSize = blockWidth*blockHeight*ds;
return planeSize*numPlanes;
```

entirely in 32-bit `int`, and returns `int`. Any block whose byte size exceeds
2³¹ overflows silently — about 13,000 × 13,000 px at 3-channel `Float32`.

This is an arithmetic property of the code, read rather than executed; no
overflowing call was made. A tiled pipeline with ordinary tile sizes never
approaches it, but UR-IMG-2 asks for 64-bit safety throughout and this is a real
upper bound on tile size that no document currently states.

### 3.2 SQLite's R\*Tree is not 64-bit

`rtree` stores coordinates as 32-bit floats; `rtree_i32` stores 32-bit integers.
There is no 64-bit variant. Integers are exact only to 2²⁴ = 16,777,216, which
the 100,000 × 80,000 reference slide clears comfortably — but UR-IMG-2's 64-bit
coordinate rule has no 64-bit spatial index behind it, and the library claims no
fixed limit on image dimensions.

SQLite rounds bounding boxes **outward**, so a query over imprecise boxes yields
false positives and never false negatives; correctness survives and selectivity
degrades. That is an acceptable property, but it should be a documented and
tested one rather than an accident. Bears on open question 5.

### 3.3 Anchor-derived identifiers assume non-overlap

An identifier packed from an object's topmost-then-leftmost pixel is unique only
if no two objects **in the same layer** share that pixel. True of a disjoint
segmentation. Not true of overlapping objects, which no requirement currently
forbids — and a 0.81% primary-key collision rate appeared in the anchor run.

That rate is **not representative**: the harness places anchors at random and
re-traverses the coordinate space, manufacturing duplicates a real segmentation
would not produce. The rate is an artifact; the invariant it exposes is not.
SDS 03 should either state non-overlap within a layer as an invariant, or define
what happens when two objects collide.

---

## 4. Method and reproduction

| | |
|---|---|
| Machine | AMD Ryzen 9 3900X (12-core), 31.9 GB RAM, Windows 11 Pro 10.0.26200 |
| Storage | **Crucial BX500 1 TB SATA SSD (DRAM-less), 91% full** |
| Compiler | MSVC 2022 Professional, `/O2 /MD`, x64 |
| SQLite | 3.50.4 amalgamation (source id `2025-07-30 19:33:53 4d8adfb3…`), `SQLITE_ENABLE_RTREE=1`, `SQLITE_THREADSAFE=1` |
| SlideIO | working tree at `D:\Projects\slideio\slideio`, staged `build/public-include`, Release build |
| Pragmas | `journal_mode=WAL`, `synchronous=NORMAL`, `cache_size=-262144` (256 MB), `temp_store=MEMORY`, `mmap_size=1 GB` |
| Transaction size | 10,000 objects |
| Test slide (Q3 runtime probe) | `images/afi/fs_Alexa Fluor 488.svs`, 9,520 × 15,121, 1 channel, `DT_UInt16` |

**The storage caveat is important.** A DRAM-less consumer SATA SSD at 91%
capacity is close to a worst case for sustained random writes, which is exactly
the workload anchor-derived identifiers generate. Treat the absolute insert
rates as a **floor**, and expect the 4.1× anchor-to-sequential ratio to narrow on
NVMe or on a drive with headroom. The ranking of the two schemes is robust; the
magnitude is not. Re-measuring on representative hardware before any schema
decision is worthwhile — and if a target is set under question 10, it must name
the storage class it assumes.

Secondary caveats: object placement is synthetic and uniform, where real
segmentation output is spatially clustered; geometry is a fixed 200-byte blob,
where real contours vary; and each configuration was run once, so the per-million
figures carry run-to-run noise. Totals and the gap between schemes are
reliable; individual interval rates are not.

Throwaway artifacts — four compile probes, one runtime probe, the benchmark
harness and two multi-gigabyte stores — are in the session scratchpad under
`…/92158dab-a1ba-4a92-b724-57e217e5c692/scratchpad/spike/`. Nothing was written
to the project tree and no repository scaffolding was created.

---

## 5. Proposed consequences

Not yet applied. Question 3's route must be chosen first, because options 1 and 2
of §1.4 produce different rewrites.

| Document | Change |
|---|---|
| `project.md` | Key decision *"Raster execution — lazy pipeline exposed as a `Scene`"* is not achievable as stated. Rewrite per the §1.4 route chosen. |
| `requirements.md` Assumption 2 | Fails. The `Scene` contract is stable but closed to out-of-tree implementation. Rewrite, and settle the conditional it already carries. |
| `requirements.md` open question 3 | Resolve: **no**, with §1.2 as the reason. |
| `requirements.md` open question 4 | Resolve: **yes**, conditional on question 10 setting a bar (§2.4). |
| `requirements.md` open question 5 | Add the 32-bit R\*Tree constraint of §3.2 as an input to the geometry-encoding decision. |
| `requirements.md` open question 10 | Raise in priority. It now blocks question 4, and it must name an assumed storage class (§4). |
| `requirements.md` UR-IMG-2 | Note the two 32-bit boundaries found: `getBlockSize` (§3.1) and the R\*Tree (§3.2). |
| SDS 05, SDS 06 §6.3 | Rework once the route is chosen. SDS 05's raster architecture is the main casualty. |
| SDS 03 | Record the identifier-cost finding (§2.2), the schema variant to test (§2.5), and the non-overlap invariant (§3.3). |
| SDS README | Open-questions table: 3 and 4 move from "spike before implementation" to resolved; add the question 10 dependency. |
| — | File a defect against SlideIO for the undiagnosed downcast in `makeTransformationCopy` (§1.2). |
