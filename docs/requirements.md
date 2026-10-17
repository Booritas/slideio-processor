# SlideIO Processor — User Requirements

| | |
|---|---|
| **Document** | User Requirements Specification |
| **Project** | `slideio-processor` |
| **Version** | 0.12 (draft) |
| **Date** | 2026-10-17 |
| **Status** | For review |

**Changes in 0.12** — **UR-BLD-7 gains a development route**: until a SlideIO
release carries the scene-source seam (`ISceneSource`, `SceneMetadata`,
`makeScene`, merged in SlideIO PR #86), SlideIO is a git submodule pinned to a
commit, built and installed to a local prefix, and consumed through
`find_package(slideio)` exactly as a released package would be. The
requirement's text is unchanged. Open question 2 is narrowed accordingly. No
requirement was renumbered.

**Changes in 0.11** — **UR-SEG-9 added**: scale-controlled region
segmentation, carrying a declared seed-scale bound in the shape of UR-SEG-8's
declared reach. It is the first requirement in this document added on a design
assessment rather than a documented user need, and the first **Should** with no
entry in the v1 catalog — both recorded in the requirement itself rather than
smoothed over, because the second is a decision someone has to take before v1
ships. Open question 7's resolution is amended accordingly: the two columns of
SDS 06 §6.10 no longer exhaust each other. No existing requirement was
renumbered or changed.

**Changes in 0.10** — **measurements 1 and 2 taken**, closing the two items
question 4's resolution left open. No requirement is added or renumbered. The
question 4 row records the results; **UR-EXE-8 gains a note making annotation
write order an explicit condition of its budget**, because that one factor
measured 24.6× — larger than anything else bearing on the requirement. The
schema consequences, including two `WITHOUT ROWID` annotation tables and
interned measurement names, are in SDS 03 §3.7 and §3.12.

**Changes in 0.9** — **UR-EXE-8 and UR-EXE-9 added**, setting the first
absolute performance bars in this document: 30 minutes for segmentation and
classification of the reference slide at the reference object count, and a
concurrency claim for batch. **Open questions 4, 9 and 10 closed.** The bar
makes the store's physical-key scheme a correctness-of-schedule matter rather
than an optimisation — SDS 03 amended accordingly — and resolves question 9 as
one slide per workspace, because batch needs independent single-writer stores.
Two measurements remain before SDS 03 is frozen and are named in the resolution.

**Changes in 0.8** — **open question 3 closed** on a direction rather than a
verdict: SlideIO gains a public extension seam and `slideio-processor`
implements it. Assumption 2 rewritten to say what is actually true of the
`Scene` contract — stable, and until now closed to out-of-tree implementation.
Three conditions on which the closure depends are recorded with it, two of them
upstream defects that must be filed and fixed. Question 16 added for the
mixed-channel-type limit on `convertScene`, which the closure bounds but does
not resolve. No requirement was added, renumbered or changed.

**Changes in 0.7** — **persisted rasters** added to the design: a stage may
write a pyramidal TIFF held beside the store, attributed to it, discarded with
it, and read by a later stage through the same auxiliary-input path as a
rasterised layer (SDS `03-storage.md` §3.8). Recorded here because it adds a
fifth staleness path and changes what UR-STG-9 archives. Partially covered by
UR-RES-7 and needing a requirement of its own — see the open action below. No
requirement was added, renumbered or changed.

**Changes in 0.6** — **UR-SEG-8 added**, requiring separation of touching
objects with a caller-declared, enforced reach bound. This closes the
traceability gap recorded in 0.5: `segment.watershed` was the only catalog
entry with no parent requirement. The requirement states a capability rather
than naming watershed, so the design retains freedom to satisfy it otherwise.
This is the first requirement added since 0.1; no existing requirement was
renumbered or changed.

**Changes in 0.5** — the question 7 resolution amended: `segment.watershed`
(marker-controlled) is **in** v1, reversing the exclusion recorded in 0.4,
which rested on an argument true only of unseeded watershed. The catalog is
twenty-two operations. Multi-resolution coarse-to-fine processing becomes a
supported pattern through a new auxiliary raster input. Watershed is the first
catalog entry with no parent requirement, recorded below as needing one before
implementation. No requirement was added, renumbered or changed.

**Changes in 0.4** — open question 7 resolved: the v1 algorithm catalog is
closed at twenty-one operations, enumerated in `superpowers/specs/sds/06-algorithm.md`
§6.10, with watershed and superpixel segmentation explicitly excluded and their
reasons recorded. Resolving it surfaced a conflict between UR-PIX-3's
acceptance criterion and Canny's hysteresis stage, added as open question 15.
No requirement was added, renumbered or changed.

**Changes in 0.3** — open question 8 resolved: the v1 standard label
vocabulary is fixed at ten labels, specified in `superpowers/specs/sds/02-config.md`
§2.2, and external ontology identifiers are deferred with their storage and API
surface reserved. The resolved-questions table records the reasoning, including
why the question's own "downstream of question 7" rule did not survive contact
with the algorithm catalog. No requirement was added, renumbered or changed.

**Changes in 0.2** — merged section 9a, *Observation* (UR-OBS-1 to UR-OBS-7),
from `requirements-9a-observation-draft.md`, with its three amendments: the
§15 non-goal on concurrent readers becomes a requirement, UR-STG-5 gains a note
distinguishing an observer from a later stage, and UR-QA-3 gains an informative
note on anchor-derived identifiers. Open questions 12 to 14 added. No existing
requirement was renumbered, and no normative text outside those three places
was changed.

---

## 1. Purpose and scope

`slideio-processor` is an open-source, cross-platform C++ library providing **a
catalog of image-processing algorithms for images larger than memory** — in the
spirit of OpenCV, but where every algorithm is scale-aware by construction.

The user picks algorithms and composes them; the library handles tiling, halos,
memory budgeting, threading and persistence. An algorithm behaves the same on a
1,000-pixel thumbnail as on a 10-billion-pixel slide, and the caller writes the
same code either way.

The catalog covers pixel-level filtering, segmentation and object extraction,
and multi-stage analysis over whole slides.

The library is a **separate project that depends on** the
[SlideIO](https://github.com/Booritas/slideio) reader library. SlideIO supplies
format decoding, zoom pyramids and the `Scene` raster abstraction; this project
supplies computation over them. It is versioned and released independently.

**In scope for v1:**

- Pixel-level filtering (colour transforms, stain deconvolution, convolution
  filters, morphology, thresholding).
- Segmentation and object extraction (tissue detection, labelling, contour
  extraction, per-object measurement).
- Multi-stage analysis, in which successive stages (for example background
  extraction, then nucleus extraction, then cell extraction) consume the
  objects produced by earlier stages, with durable results that survive
  interruption between stages.

Object counts at this scale are substantial: a single slide may contain on the
order of **ten million cells**. Results are therefore held in a persistent,
spatially-indexed store and read through streaming queries; they are never
required to fit in memory. This is a defining constraint of the design, not an
optimisation.

### No fixed limits

**The library shall impose no limit of its own on image dimensions, pixel count,
channel count, or object count.** Where a limit exists it shall be one inherited
from the platform, from SlideIO, or from available storage — never one
introduced by this library's own data structures or arithmetic.

The reference figures used throughout this document are **verification anchors,
not ceilings**: they fix what the test suite must demonstrate, and nothing
about them may be relied on as an upper bound in the design.

**Explicitly out of scope for v1** — see [§15 Non-goals](#15-non-goals).

## 2. Intended use and limitations

**This library is for research use only (RUO). It is not intended for
diagnosis, treatment decisions, or any clinical purpose.**

- **UR-SCOPE-1 (Must)** — The RUO statement shall appear in the repository
  README, the generated API documentation, and the LICENSE-adjacent notices.
  *Acceptance:* the exact statement above is present in all three locations.

- **UR-SCOPE-2 (Must)** — The library shall make no diagnostic claim and expose
  no API named or documented in diagnostic terms.
  *Acceptance:* public-header review confirms no such terms.

> **Note for downstream integrators.** RUO is a deliberate boundary, not an
> oversight. Incorporating this library into a medical device would make it SOUP
> under IEC 62304 and require, at minimum, a safety classification, a risk
> analysis under ISO 14971, and verification records held by the integrating
> manufacturer. Nothing in this repository satisfies those obligations, and this
> document is not a design-control record. Teams considering such use should
> consult Regulatory Affairs before starting.

## 3. Stakeholders

| Role | Interest |
|---|---|
| **Research software engineer** | Primary user. Embeds the C++ library in analysis tooling; needs a clear, stable, binding-friendly API. |
| **Computational pathology researcher** | Consumes results via Python or QuPath; needs interoperable outputs and reproducible numbers. |
| **Library maintainer** | Needs a small, testable core with a public extension point so features arrive as contributions, not forks. |
| **Downstream integrator** | Embeds the library in a larger product; needs stable versioning, a known dependency set, and an honest statement of limitations. |

## 4. Definitions

| Term | Meaning |
|---|---|
| **Slide** | A WSI file, opened through SlideIO; may contain several scenes. |
| **Scene** | SlideIO's raster abstraction: a multi-resolution, possibly multi-channel, 2D/3D/4D image. |
| **Level** | One resolution tier of a zoom pyramid; level 0 is full resolution. |
| **Tile** | A rectangular unit of work read and processed independently. |
| **Halo** | Border pixels read beyond a tile's bounds so neighbourhood operations are correct at tile edges. |
| **Pipeline** | An ordered, lazily-evaluated composition of operations over a source scene. |
| **Operation** | A single processing step implementing the public `Operation` interface. |
| **Object** | A segmented region with geometry, measurements, the layer it belongs to, and an optional class label. Its parent, if any, is a separate edge record rather than a property of the object. |
| **Layer** | A named collection of objects, declared by the workspace. Every layer is either **semantic** or **primitive**. A layer is normally produced by a single stage, but need not be. |
| **Semantic layer** | A layer whose objects represent a kind of entity at a level of containment (e.g. `tissue`, `cells`, `nuclei`). Semantic layers are ordered into a branching hierarchy rooted at the **pixel layer**, and only they take part in parent edges. |
| **Primitive layer** | A layer whose objects are geometric constructs with no entity meaning of their own — superpixels, watershed basins, connected components, contour fragments. Primitive layers are arbitrary: any number, in no order, outside the hierarchy. Semantic objects may record which primitives they were built from (UR-RES-16). |
| **Pixel layer** | The root of the semantic hierarchy: the pixels of the source scene. It holds no objects. |
| **Class label** | The classification of an object *within* its layer — `tumour` or `stroma` inside `tissue`, `mitotic` inside `nuclei`. Distinct from the layer, which says what kind of thing the object is. |
| **Label vocabulary** | The set of class labels a store may use: the library's documented, versioned **standard vocabulary**, plus any labels a workspace has added locally. One vocabulary per store, global — it is not scoped or subsetted by layer. |
| **Object store** | The persistent, spatially-indexed store holding all objects produced for a slide. |
| **Cursor** | A forward-only, bounded-memory iterator over the results of an object-store query. |
| **Stage** | A named unit of analysis (e.g. `background`, `nuclei`, `cells`) that consumes a source scene and, optionally, the objects of earlier stages, and produces objects of its own. |
| **Workspace** | The durable artifact binding a slide to its object store and the recorded state of every stage. |
| **Channel data type** | The numeric type of one channel's samples, per SlideIO's `DataType`: `Byte`, `Int8`, `UInt16`, `Int16`, `UInt32`, `Int32`, `UInt64`, `Int64`, `Float16`, `Float32`, `Float64`. A scene may use a different type for each channel. |
| **Reference slide** | A **verification anchor, not a limit**: a brightfield WSI of 100,000 x 80,000 px, 3 x 8-bit channels (~24 GB uncompressed, 8x10^9 pixels). Used to fix what the test suite must demonstrate. |
| **Reference object count** | A **verification anchor, not a limit**: 10 million objects in a single stage for one slide. |

---

## 5. Image access, scale and pixel types

### 5.1 Image size

- **UR-IMG-1 (Must)** — The library shall impose no limit of its own on image
  dimensions or pixel count. Any limit shall be one inherited from SlideIO, the
  platform, or available storage.
  *Acceptance:* no compile-time or runtime constant in the library caps image
  dimensions or pixel count; a documented review confirms this.

- **UR-IMG-2 (Must)** — **All pixel counts, areas, linear indices, histogram
  bins, accumulators and object counts shall use 64-bit arithmetic.** 32-bit
  arithmetic overflows at 2.1x10^9, which the reference slide already exceeds
  at 8x10^9 pixels; larger slides exceed it by more.
  *Acceptance:* a test processes an image whose pixel count exceeds 2^31 and
  asserts that reported areas, counts and statistics are correct; a static check
  confirms no 32-bit accumulator on a pixel-count or area path.

  > SlideIO's own `Rect` and `Size` are `int32_t`, bounding each axis at about
  > 2.1x10^9 pixels — far beyond any real slide, so not a practical constraint.
  > The constraint that bites is *area*, which is the product of two such axes
  > and overflows immediately. This is the concrete, testable content of "no
  > restriction on size".

- **UR-IMG-3 (Must)** — The library shall process slides whose full-resolution
  raster exceeds available RAM, without requiring the whole slide to be
  resident, for images of any size.
  *Acceptance:* the reference slide is processed end-to-end on a machine with
  8 GB RAM.

- **UR-IMG-4 (Must)** — The library shall use the source slide's zoom pyramid,
  reading from the coarsest level that satisfies the requested output resolution
  rather than decoding level 0 and downsampling.
  *Acceptance:* for a request at 1/16 resolution on a slide with a matching
  level, instrumentation shows no level-0 decode.

- **UR-IMG-5 (Must)** — Processing shall be restrictable to a region of
  interest, expressed either as a rectangle in slide coordinates or as polygon
  geometry imported per UR-RES-8.
  *Acceptance:* processing an ROI reads only tiles intersecting it; results
  match those obtained by processing the whole slide and cropping.

### 5.2 Channels

- **UR-IMG-6 (Must)** — The library shall impose no limit of its own on the
  number of channels in a scene, and shall allow operations to select any
  channel subset.
  *Acceptance:* scenes with 1, 3, 5 and at least 64 channels are processed;
  no constant in the library caps channel count.

- **UR-IMG-7 (Must)** — Channel selection and ordering shall be explicit, and
  an operation shall never assume a channel's meaning from its index (for
  example, that channels 0-2 are RGB) unless the scene's metadata states it.
  *Acceptance:* applying an RGB-specific operation to a scene without RGB
  metadata raises a documented error rather than silently misinterpreting
  channels.

### 5.3 Pixel data types

- **UR-IMG-8 (Must)** — The library shall accept scenes using **any of
  SlideIO's channel data types**: `Byte`, `Int8`, `UInt16`, `Int16`, `UInt32`,
  `Int32`, `UInt64`, `Int64`, `Float16`, `Float32`, `Float64`.
  *Acceptance:* a scene of each type is opened, read, converted and stored
  without loss.

- **UR-IMG-9 (Must)** — The library shall place **no restriction of its own** on
  scenes whose channels have different data types from one another. Such scenes
  are rare in pathology but legal in SlideIO's model, and shall be readable,
  selectable and storable without special-casing by the caller.
  *Acceptance:* a scene mixing `Byte` and `Float32` channels is read, its
  channels selected individually and jointly, and per-channel types preserved.

  > Any restriction on mixed types belongs to a **specific algorithm**
  > (UR-ALG-3), never to the library. This is the difference between "the
  > library cannot do this" and "this algorithm requires uniform types across
  > the channels you selected".

- **UR-IMG-10 (Must)** — Reading, channel selection, type conversion, and
  writing to the object store shall be **lossless for every type in UR-IMG-8**,
  regardless of which types the computational backend happens to support.
  *Acceptance:* a round trip through the library of `UInt32`, `Int64` and
  `UInt64` channel data returns bit-identical values.

- **UR-IMG-11 (Must)** — Each algorithm shall **declare the data types it
  supports**. Applying an algorithm to an unsupported type shall raise a
  documented error naming both the algorithm and the type, and pointing at the
  conversion needed. No algorithm shall silently narrow, saturate or lose
  precision.
  *Acceptance:* applying an algorithm to a type it does not declare raises an
  error naming both; no test observes silent conversion.

  > The set of unsupported types is **backend-version-dependent, not
  > architectural**. OpenCV 4's `cv::Mat` has no depth for `UInt32`, `Int64` or
  > `UInt64`; OpenCV 5 adds all three. Declaring support per algorithm rather
  > than hard-coding a global limitation means the gap closes on migration
  > (UR-BLD-4) with no requirement and no API changing — the generated matrix
  > (UR-IMG-12) simply fills in.

- **UR-IMG-12 (Must)** — The supported-type matrix — every operation against
  every data type — shall be documented and generated from the same declarations
  the runtime enforces, so documentation cannot drift from behaviour.
  *Acceptance:* the published matrix is generated by a build step, and a test
  asserts it matches runtime behaviour for every operation and type pair.

- **UR-IMG-13 (Should)** — Explicit type conversion between any two supported
  types shall be available as an operation, with documented rounding and
  saturation behaviour.
  *Acceptance:* conversions are tested at type boundaries (min, max, overflow)
  against the documented behaviour.

## 6. The algorithm catalog

The library's product is a **catalog of algorithms**, in the spirit of OpenCV
but scale-aware by construction. The requirements below apply to every algorithm
in it; sections 7 and 8 list what the catalog must contain.

The library commits to no compatibility with OpenCV's names, parameters or
numerical results. Design freedom is deliberate — but it means each algorithm
carries its own correctness reference (UR-ALG-6) rather than inheriting one.

- **UR-ALG-1 (Must)** — Every algorithm shall be usable on an image of any size
  without the caller writing tiling, halo, memory or threading code. The same
  call shall work unchanged on a thumbnail and on a whole slide.
  *Acceptance:* one code path applies an algorithm to a 1,000 px image and to
  the reference slide; only the input differs.

- **UR-ALG-2 (Must)** — **The caller selects which channels an algorithm
  processes.** Channel selection shall be explicit and uniform across the
  catalog.
  *Acceptance:* every algorithm accepting image input accepts a channel
  selection through the same mechanism.

- **UR-ALG-3 (Must)** — Every algorithm shall declare its **channel arity**:

  | Arity | Meaning | Mixed types across selected channels |
  |---|---|---|
  | **Per-channel** | Applied independently to each selected channel | Always fine — each channel is processed alone |
  | **Joint** | Consumes the selected channels together (e.g. colour deconvolution, RGB to grey) | May be restricted by that algorithm |

  A joint algorithm shall declare whether it requires the selected channels to
  share a data type, and shall raise a documented error naming itself and the
  offending channels when that requirement is unmet.
  *Acceptance:* a per-channel algorithm succeeds on a selection mixing `Byte`
  and `Float32`; a joint algorithm requiring uniform types fails on the same
  selection with an error naming the algorithm and both channels.

  > This is where a type or channel restriction is allowed to live. The library
  > imposes none (UR-IMG-9); an individual algorithm may, and must say so.

- **UR-ALG-4 (Must)** — Every algorithm shall declare the data types it supports
  (UR-IMG-11) and the halo it requires (UR-EXT-3), and the framework shall
  enforce both.

- **UR-ALG-5 (Must)** — Algorithms shall be **composable**: the output of one is
  a valid input to another, without materialising intermediates to disk unless
  the caller asks.
  *Acceptance:* a three-algorithm chain over the reference slide runs within the
  memory ceiling with no intermediate written.

- **UR-ALG-6 (Must)** — Each algorithm shall have a **documented correctness
  reference** — an analytic result on synthetic input, a published reference
  implementation, or a peer-reviewed definition — and a test asserting agreement
  with it within a stated tolerance.
  *Acceptance:* every algorithm in the catalog names its reference in its
  documentation and has such a test.

  > `UR-QA-2` proves that tiling does not change an algorithm's result. It does
  > not prove the result is correct. Since the catalog deliberately takes no
  > numerical compatibility with OpenCV, nothing external supplies that proof,
  > so each algorithm must bring its own.

- **UR-ALG-7 (Should)** — The catalog shall be consistent: algorithms of the
  same kind shall take comparable parameters in a comparable order, and shall
  report errors, progress and cancellation through the same mechanisms.
  *Acceptance:* an API review against a documented convention, applied to the
  whole catalog rather than per algorithm.

- **UR-ALG-8 (Should)** — Each algorithm's documentation shall state its
  supported types, channel arity, halo, and computational complexity in terms of
  pixels processed, so a user can predict cost on a large image.

## 7. Pixel-processing algorithms

- **UR-PIX-1 (Must)** — The library shall provide colour space conversion
  between the spaces relevant to brightfield pathology (RGB, greyscale, HSV,
  Lab).
  *Acceptance:* round-trip conversion is within a documented tolerance.

- **UR-PIX-2 (Must)** — The library shall provide stain colour deconvolution
  with built-in vectors for at least H&E and H-DAB, and shall accept
  user-supplied stain vectors.
  *Acceptance:* deconvolving a synthetic image composed from known stain
  vectors recovers the component intensities within a documented tolerance.

- **UR-PIX-3 (Must)** — The library shall provide convolution and neighbourhood
  filters: Gaussian blur, median blur, Sobel, Scharr, Laplacian, Canny and
  bilateral filtering.
  *Acceptance:* each filter's whole-slide output matches a single-tile
  reference computation on an image small enough to process in one tile.

- **UR-PIX-4 (Must)** — The library shall provide binary morphology (erode,
  dilate, open, close) with configurable structuring elements.
  *Acceptance:* as UR-PIX-3.

- **UR-PIX-5 (Must)** — The library shall provide thresholding: fixed, Otsu, and
  adaptive/local.
  *Acceptance:* global methods (Otsu) computed over a streamed whole slide match
  the value computed from a full in-memory histogram of the same slide.

- **UR-PIX-6 (Must)** — Operations shall compose into a pipeline whose output is
  itself usable as an input scene.
  *Acceptance:* a two-stage pipeline produces the same result as running the
  stages separately and materialising the intermediate.

## 8. Segmentation and object extraction

- **UR-SEG-1 (Must)** — The library shall provide tissue-versus-background
  detection over a whole slide.
  *Acceptance:* on a labelled test corpus, detected tissue area is within a
  documented tolerance of ground truth.

- **UR-SEG-2 (Must)** — The library shall provide connected-component labelling
  over a whole slide, producing distinct objects with stable identifiers.

- **UR-SEG-3 (Must)** — **Objects that span tile boundaries shall be merged into
  single objects, with geometry and measurements computed as if the slide had
  been processed as one image.**
  *Acceptance:* segmentation results — object count, and each object's area,
  perimeter, centroid and intensity statistics — are **identical across at least
  three different tile sizes** (e.g. 512, 1024, 4096). This is verified by a
  dedicated test suite, not incidentally.

  > This is the single hardest requirement in this document and the most common
  > failure mode in tiled WSI processing. It is stated as a user requirement,
  > not an implementation detail, because a library that gets it wrong produces
  > plausible-looking but incorrect results.

- **UR-SEG-4 (Must)** — The library shall extract object geometry as polygon
  contours in level-0 slide coordinates, including interior holes.
  *Acceptance:* an object with a known hole yields the outer contour and the
  hole as separate rings.

- **UR-SEG-5 (Must)** — The library shall compute per-object measurements: area,
  perimeter, centroid, bounding box, and per-channel intensity statistics
  (mean, standard deviation, min, max).
  *Acceptance:* measurements on synthetic objects of known dimensions match
  analytic values within a documented tolerance.

- **UR-SEG-6 (Should)** — Measurements with a physical dimension shall be
  available in both pixels and calibrated physical units, using the resolution
  metadata reported by SlideIO.
  *Acceptance:* area in um^2 equals area in px^2 scaled by the scene resolution;
  a scene lacking resolution metadata yields pixel units and a documented
  indication that calibration was unavailable.

- **UR-SEG-7 (Should)** — Objects shall be filterable by measurement predicates
  (e.g. minimum area) during extraction, so that discarded objects are never
  written to the store.
  *Acceptance:* extraction with a minimum-area filter on a slide with many small
  artefacts yields a materially smaller store than unfiltered extraction, and
  the filtered objects appear in no query.

- **UR-SEG-8 (Must)** — The library shall provide **separation of touching
  objects**: objects in contact within a single connected region shall be
  extractable as distinct objects, counted and measured individually rather
  than as one. Connected-component labelling (UR-SEG-2) does not satisfy this —
  it merges whatever touches.

  The **reach** of the separation decision shall be declared by the caller and
  enforced. An input requiring that decision to depend on evidence further away
  than the declared reach shall be **rejected with a diagnostic naming the
  operation and the distance**, and shall never be segmented approximately.
  *Acceptance:* on synthetic touching discs of known centres and radii, object
  count, area and centroid match analytic values within a documented tolerance;
  the same input segmented at three tile sizes yields identical results, as
  UR-SEG-3 requires of every segmentation; and an input whose objects lie
  further apart than the declared reach fails with that error rather than
  yielding a result that depends on tile size.

  > The reach bound belongs in a user requirement rather than in design
  > because it is user-visible in both directions: it is a value the caller
  > sets and an error the caller receives. It is stated because separation that
  > degrades silently at tile boundaries produces a count that is *nearly*
  > right — the failure mode UR-SEG-3 exists to prevent, and the one this
  > document calls the most common in tiled WSI processing.

  > This requirement states a capability, not an algorithm. Marker-controlled
  > watershed is what v1 ships for it (SDS `06-algorithm.md` §6.7); nothing
  > here obliges that choice, and a later release may satisfy it differently
  > provided the reach bound and the acceptance criteria hold.

- **UR-SEG-9 (Should)** — The library shall provide **scale-controlled region
  segmentation**: partitioning tissue into homogeneous regions whose
  granularity is governed by a single caller-supplied **scale parameter**, with
  the relative weight of spectral homogeneity against shape regularity also
  under caller control.

  The segmentation shall be **initialised from a partition rather than from
  individual pixels**, and the **seed scale shall be declared by the caller and
  reported with the result**, because it bounds the finest boundary the output
  can express. A scale parameter finer than the declared seed scale can express
  shall be **rejected with a diagnostic naming both values**, and never
  satisfied approximately.
  *Acceptance:* on synthetic mosaics whose regions are coarser than the seed
  scale, region count and per-region area match analytic values within a
  documented tolerance; the same input segmented at three tile sizes yields
  identical results, as UR-SEG-3 requires of every segmentation; increasing the
  scale parameter monotonically decreases region count; and a request finer
  than the seed scale fails with that error rather than returning a partition
  at the wrong granularity.

  > **This is a different capability from UR-SEG-1 and UR-SEG-2.** Tissue
  > detection answers *is this tissue*; connected-component labelling answers
  > *what touches what*. Neither partitions homogeneous tissue into regions at
  > a chosen scale, which is what tissue-architecture work — stroma against
  > tumour against necrosis — needs before classification, and which UR-RES-4's
  > class labels presuppose something produces.

  > **The seed-scale bound is stated here rather than left to design, for
  > UR-SEG-8's reason:** it is user-visible in both directions, a value the
  > caller sets and an error the caller receives. It exists because pixel
  > initialisation is not affordable at full resolution — the segment graph for
  > the reference slide would exceed UR-EXE-2's ceiling by three orders of
  > magnitude — so the approximation is structural rather than incidental. A
  > region segmentation that silently stops resolving detail yields a plausible
  > partition at the wrong granularity, which no downstream measurement
  > reveals.

  > **This requirement states a capability, not an algorithm.** Baatz & Schäpe
  > region merging seeded from a partition is the assessed candidate
  > (`superpowers/specs/2026-10-11-multiresolution-segmentation.md`); nothing
  > here obliges that choice.

  > **No documented user need drives this** — unlike every other requirement in
  > this document, it was raised by a design assessment. It is recorded as a
  > *Should*, and **v1 ships nothing for it** (SDS `06-algorithm.md` §6.10),
  > which makes it the first Should here without a catalog entry. The gap is
  > deliberate and should be closed in one direction or the other — an
  > operation, or a downgrade — before v1 ships.

## 9. Results, storage and interoperability

Objects are held in a persistent store and read through streaming queries. No
requirement in this document may be satisfied by an API that returns all
objects of a stage at once.

- **UR-RES-1 (Must)** — Objects shall be written to a persistent,
  spatially-indexed object store as they are produced, so that peak memory is
  independent of the number of objects.
  *Acceptance:* a stage producing the reference object count (10 million)
  completes within the configured memory ceiling (UR-EXE-2).

- **UR-RES-2 (Must)** — Objects shall be readable through a **cursor** over a
  query, iterating in bounded memory regardless of how many objects match.
  *Acceptance:* iterating all 10 million objects of a stage keeps measured peak
  RSS within the configured ceiling.

- **UR-RES-3 (Must)** — The store shall support **spatial queries** — retrieving
  objects intersecting or contained by a given rectangle or polygon — without a
  full scan.
  *Acceptance:* querying a small region of a 10-million-object stage returns in
  time proportional to the result size, not to the store size; a full scan of
  the same store is measurably slower by orders of magnitude.

- **UR-RES-4 (Must)** — Each object shall carry: a stable identifier, geometry
  as a polygon contour (including interior holes) in level-0 slide coordinates,
  a bounding box, the layer it belongs to, the identifier of the stage that
  produced it, its measurements, and an **optional** class label drawn from the
  store's vocabulary (UR-RES-17). An object with no label is **unlabelled** — a
  valid state in any layer, not an inconsistency — and the unlabelled objects of
  a layer shall be queryable.
  **The parent relation shall not be a property of the object** — it is held as
  a separate edge record (UR-RES-5), so that an object can be attached after it
  was written without rewriting it.

- **UR-RES-5 (Must)** — The store shall hold **parent edges** as first-class
  records of the form *(child, parent, asserting stage)*. Any stage may assert
  an edge between an object and an object in a lower layer (UR-RES-14),
  irrespective of which stage created either endpoint and of the order in which
  the two were created; asserting an edge shall not modify either object. The
  store shall support querying the children of a given object and the parent of
  a given object. An object with no parent edge is **unattached** — a valid
  state, not an inconsistency — and the unattached objects of a layer shall be
  queryable.
  *Acceptance:* a `nuclei` stage run before any cell exists writes unattached
  nuclei, and a later `cells` stage creates cells and asserts the
  nucleus-to-cell edges without rewriting the nuclei; a three-stage
  tissue/cell/nucleus run yields nuclei reachable from their tissue region by
  traversing two edges, in either execution order.

- **UR-RES-6 (Should)** — Materialising query results into memory shall be
  available as an explicit, separate call that operates on a filtered query, and
  shall be documented as bounded by the caller's filter.
  *Acceptance:* the API offers no call that materialises an unfiltered stage.

- **UR-RES-7 (Must)** — Label and probability maps shall be obtainable as a
  multi-resolution raster: either as a pipeline scene readable at any level, or
  written to a pyramidal TIFF.
  *Acceptance:* a written mask reopens in SlideIO with the expected levels and
  dimensions.

- **UR-RES-8 (Must)** — The library shall import and export object geometry and
  properties as GeoJSON, interoperable with QuPath. Export shall stream, and
  shall accept a query so that a subset can be exported without materialising
  the whole stage.
  *Acceptance:* objects exported from this library open in QuPath with geometry
  and measurements intact; a QuPath-exported annotation file imports and is
  usable as a processing ROI; exporting 10 million objects stays within the
  memory ceiling.

- **UR-RES-9 (Must)** — Object measurements shall be exportable as a table, one
  row per object, in CSV, driven by the same query mechanism as UR-RES-2.
  *Acceptance:* the file loads in pandas with the documented column schema.

- **UR-RES-10 (Should)** — Tabular export shall additionally support at least
  one binary columnar format (e.g. Parquet) for result sets impractical to
  handle as CSV.

- **UR-RES-11 (Must)** — The store shall carry provenance: source slide
  identifier, scene index, library version, and, per stage, the pipeline
  configuration that produced its objects.
  *Acceptance:* provenance is recoverable from the store alone, without the
  code that produced it, and is included in GeoJSON and tabular exports.

- **UR-RES-12 (Must)** — The store format shall be documented and versioned, and
  the library shall refuse to open a store written by an incompatible version
  with a clear diagnostic rather than misreading it.
  *Acceptance:* opening a store with a bumped incompatible version number fails
  with a message naming both versions.

- **UR-RES-13 (Must)** — Each object shall record the **layer** it belongs to, as
  a property of the object itself and independently of the stage that produced
  it, so that objects arriving without a stage still have a layer.
  *Acceptance:* a GeoJSON import places its objects in a named layer, and
  querying that layer returns them alongside objects written by a stage.
- **UR-RES-14 (Must)** — A parent edge (UR-RES-5) shall connect two objects in
  **semantic layers**, the parent in a **lower layer** than the child though not
  necessarily the immediately adjacent one. The store shall reject an edge whose
  parent is in the same or a higher layer, and shall reject an edge with either
  endpoint in a primitive layer, since a primitive layer has no position in the
  hierarchy. An object with no parent edge is unattached, which is valid at any
  layer.
  *Acceptance:* attaching a nucleus directly to a tissue region succeeds when
  the `cells` layer is skipped; attaching a cell to another cell, to a nucleus,
  or to a superpixel fails with a diagnostic naming both layers.

- **UR-RES-15 (Should)** — The layer shall accompany objects in GeoJSON and
  tabular exports, so that a hierarchy round-trips through interchange rather
  than surviving only inside the store.
  *Acceptance:* objects exported and re-imported land in the same layers, with
  parent relations intact.

- **UR-RES-16 (Must)** — The store shall hold a **derived-from** relation,
  distinct from parent edges, linking an object to the primitive objects it was
  built from. It shall be **many-to-many** — one primitive may contribute to
  several objects, and one object may be built from many primitives — optional,
  queryable in both directions, and **valid when empty**, so that dropping a
  primitive layer leaves the objects derived from it intact rather than
  dangling.
  *Acceptance:* a tissue region merged from superpixels reports the superpixels
  it was built from, and a superpixel straddling two tissue regions reports
  both; after the primitive layer is dropped the tissue regions remain complete
  and queryable, reporting no origins.

- **UR-RES-17 (Must)** — The library shall ship a **standard class-label
  vocabulary**: documented, versioned independently of the store format, and
  stable within a major version. Each store shall record the vocabulary version
  it was written against, and each label shall record whether it is standard or
  workspace-local. A store written against a newer vocabulary than the reading
  library knows shall report its unrecognised labels as such, with identifier
  and string intact, rather than dropping or renaming them. The per-object cost
  of a class label shall not scale with the length of the label string. The
  vocabulary shall be **global to the store**: one set of labels shared by every
  layer, semantic and primitive alike, never scoped or subsetted per layer.
  *Acceptance:* a store reports its vocabulary version; a label introduced in a
  later release is readable by an earlier library as an unrecognised standard
  label; doubling the length of every label string leaves store size
  substantially unchanged.

- **UR-RES-18 (Must)** — A workspace shall be able to **add class labels of its
  own** at any time — without modifying the library, without re-running
  completed stages, and without a store-format migration. Workspace-local labels
  shall be usable, queryable and exportable exactly as standard ones are, and
  shall be marked as local wherever a label is reported — in the store, in
  GeoJSON and in tabular exports — so that a consumer can tell which labels are
  portable between workspaces and which are not.
  *Acceptance:* a label added after the `nuclei` stage has completed is applied
  by a later stage and appears in exports flagged as workspace-local, with the
  previously written nuclei unchanged.

- **UR-RES-19 (Must)** — On import (UR-RES-8), a label belonging to neither the
  standard nor the workspace vocabulary shall be handled by a declared policy:
  **strict**, rejecting the import with a diagnostic naming the label, or
  **permissive**, registering it as a workspace-local label and recording that
  it arrived by import rather than by declaration. The policy in force and the
  outcome shall both be recoverable from the store.
  *Acceptance:* importing a QuPath annotation classified `my-weird-class` fails
  under strict with the label named, and under permissive adds it as
  workspace-local, marked as import-registered.

## 9a. Observation

Two processes run concurrently against one workspace. A **script** process —
typically Python — runs a stage and writes objects. A **viewer** process
displays the slide and overlays results, reacting as layers are declared and
objects are produced, querying their geometry to draw them.

The viewer may start before, during or after the script; may be closed and
reopened; may run alongside other observers; and must not slow the script down
or be able to corrupt what it writes.

- **UR-OBS-1 (Must)** — A store being written by one process shall be readable
  concurrently by other processes. Readers shall neither block the writer nor
  be blocked by it, and shall observe a consistent snapshot rather than a
  partially applied transaction. Concurrent *writers* remain out of scope.
  *Acceptance:* while a stage writes the reference object count, a second
  process issues spatial queries continuously throughout and observes no error,
  no partially written batch, and no measurable reduction in writer throughput.

- **UR-OBS-2 (Must)** — The store shall hold an **event journal**: append-only,
  monotonically sequenced, and readable by any process that can read the store.
  Each event shall be written **in the same transaction as the data it
  describes**, so that an event and its data commit or fail together.
  *Acceptance:* a test asserts that no committed object batch lacks its event
  and no event references objects absent from the store, including after the
  writer process is killed at randomised points during a stage.

  > Stated as atomicity rather than as a delivery guarantee because this is the
  > property that makes the journal usable as a source of truth. A notification
  > sent separately from the write it announces can be lost, duplicated or
  > reordered relative to the data; one committed with it cannot.

- **UR-OBS-3 (Must)** — Events shall be readable through a forward-only
  **cursor** keyed on sequence number, in bounded memory. A subscriber shall be
  able to begin at any sequence, including the beginning of the journal or its
  current end, and shall be able to persist its position and resume from it in
  a later process.
  *Acceptance:* a subscriber started after N batches have been written replays
  all N in order; a subscriber stopped and restarted from its persisted
  position resumes with no gap and no duplicate; three independent subscribers
  at different positions each observe the complete sequence from their own
  starting point.

- **UR-OBS-4 (Must)** — Objects shall become visible to observers at
  **batch-commit granularity** while the producing stage is still running.
  Queries shall by default return only objects of completed stages, so that
  UR-STG-5 holds unchanged for later stages; an observer shall opt in
  explicitly to objects of in-flight stages. When a stage fails, is cancelled,
  or is re-run, the journal shall record a discard event, and the objects that
  stage wrote shall cease to be returned by any query.
  *Acceptance:* an observer sees objects appear during a running stage; a later
  stage querying the same layer while that stage runs sees none of them;
  killing the stage produces a discard event, after which the objects are
  returned by no query, with or without the opt-in.

- **UR-OBS-5 (Must)** — The journal shall record at least: **layer declared**,
  **objects added**, **edges asserted**, **label added**, **stage state
  changed**, **stage discarded**, and **primitive layer dropped**. An
  objects-added event shall carry the layer, the producing stage, the object
  count and the union bounding box of the batch, so that an observer can decide
  whether the batch is relevant before querying its geometry. Events shall not
  carry object geometry.
  *Acceptance:* each event kind is emitted and observed in a test; an observer
  whose viewport does not intersect a batch's bounding box issues no query for
  it; journal size for the reference object count is a documented and measured
  fraction of store size.

- **UR-OBS-6 (Must)** — Any mechanism used to notify a subscriber that new
  events exist shall be **local to the machine** and scoped by operating-system
  permissions. The library shall open no network socket. **Correctness shall
  not depend on that mechanism**: a subscriber that only polls the journal
  shall observe every event, in order, with no loss.
  *Acceptance:* no listening network socket exists in either process during a
  subscribed run, verified by inspection; with the notification mechanism
  disabled by configuration, a polling subscriber observes an event sequence
  identical to one observed with it enabled.

  > Whole-slide images routinely carry patient identifiers in their metadata.
  > A library that opens a localhost listener as a side effect of subscribing
  > to events creates an exposure that costs nothing to avoid, and one that a
  > downstream integrator handling identifiable data under the HIPAA Security
  > Rule or GDPR would have to assess and document. Keeping the channel to a
  > named semaphore or Unix domain socket keeps it inside the machine's own
  > permission model.

- **UR-OBS-7 (Should)** — The journal shall have a documented retention policy,
  and the store shall support truncating it below a recorded watermark without
  affecting objects, edges or stage state. A subscriber whose position has been
  truncated away shall be told so explicitly rather than silently resuming from
  a later point.
  *Acceptance:* truncating the journal of a store holding the reference object
  count leaves every query result unchanged; a subscriber resuming from a
  truncated position receives a documented error naming its position and the
  oldest retained sequence.

## 10. Multi-stage processing and workspaces

Analysis proceeds as a sequence of named stages — for example background
extraction, then nucleus extraction, then cell extraction — which may run hours
or days apart, in separate processes. Results must therefore be durable and
resumable, not held in a running process.

**Layers and stages are different things.** A stage is a unit of *work* — named,
stateful, re-runnable. A layer is a unit of *meaning* — what the objects are and
what contains them. The common case is one stage producing one layer, which is
why the two are easy to conflate. They come apart when a single stage emits two
layers in one pass (cells and their nuclei), when one layer is filled by several
stages (tissue regions from a background stage plus imported manual
annotations), or when a layer arrives with no stage at all (GeoJSON imported
from QuPath). An object therefore records its layer as well as the stage that
produced it (UR-RES-13).

**The layer ordering is structural, not temporal.** It constrains what contains
what; it never constrains what runs when. Execution order is carried solely by
declared stage dependencies (UR-STG-2). A pipeline may segment nuclei first and
build cells around them afterwards — the nucleus still occupies a layer above
the cell, and the `cells` stage asserts the parent edges when it runs
(UR-RES-5). Nothing requires a containing object to exist before the objects it
will contain.

- **UR-STG-1 (Must)** — The library shall provide a **workspace**: a durable
  artifact binding a slide to its object store and the recorded state of every
  stage, openable across processes and machine restarts.
  *Acceptance:* a workspace written by one process is opened by a second process
  after the first has exited, with all objects and stage states intact.

- **UR-STG-2 (Must)** — Stages shall be named, shall declare the earlier stages
  they depend on, and shall be executable individually.
  *Acceptance:* running the `cells` stage on a workspace whose `nuclei` stage is
  complete consumes the existing nuclei without recomputing them.

- **UR-STG-3 (Must)** — A stage shall be able to read the objects of any
  completed earlier stage, including by spatial query, and to assert parent
  edges involving them, whether those earlier objects are the parents or the
  children of the objects it writes (UR-RES-5).
  *Acceptance:* a `cells` stage restricted to regions from a `background` stage
  produces no objects outside those regions.

- **UR-STG-4 (Must)** — Each stage shall record its state — at minimum
  `pending`, `running`, `complete`, `failed` — together with its configuration
  and completion time, and this state shall be queryable without running
  anything.
  *Acceptance:* a workspace can be inspected to report which stages are done and
  what configuration produced each.

- **UR-STG-5 (Must)** — The workspace shall be durable at **stage boundaries**:
  a completed stage's objects survive process termination, and a stage
  interrupted before completion shall leave the workspace in its prior
  consistent state, with no partial objects visible to later stages.
  *Acceptance:* killing the process mid-stage and reopening the workspace shows
  the interrupted stage as not complete and exposes none of its objects; the
  preceding stage is intact.

  > An **observer** is not a later stage. Objects of an in-flight stage are
  > visible to an observer that opts in under UR-OBS-4, and remain invisible to
  > later stages by default. UR-STG-5 is satisfied by the query filter rather
  > than by rollback, so an interrupted stage's objects may be physically present
  > in the store while being returned by no stage-facing query, until the discard
  > of UR-OBS-4 removes them.

- **UR-STG-6 (Must)** — An interrupted or failed stage shall be re-runnable, and
  re-running a completed stage shall replace the objects it wrote **and the
  parent edges it asserted** (UR-RES-5) rather than duplicating either. Objects
  written by other stages shall be left untouched, including objects that the
  re-running stage had attached.
  *Acceptance:* running the same stage twice yields the same object count and
  the same edge count as running it once; re-running a `cells` stage that had
  attached existing nuclei leaves those nuclei unchanged, and leaves them
  unattached until the re-run asserts its edges.

- **UR-STG-7 (Must)** — **The workspace format and the stage-execution interface
  shall be designed so that tile-level checkpointing within a stage can be added
  later without a breaking change to either.** Specifically, the stage state
  record shall reserve representation for per-tile completion, and stage
  execution shall be structured around a resumable unit of work even though v1
  only ever resumes at stage granularity.
  *Acceptance:* a documented design note identifies the schema fields and
  interface points that a future tile-level implementation would populate, and
  a review confirms no v1 API would need to change to support it.

  > Recorded as a requirement rather than an aspiration because the cost of
  > retrofitting resumability into a format and an execution model that did not
  > anticipate it is high, and the cost of anticipating it now is low.

- **UR-STG-8 (Should)** — Re-running a stage shall invalidate the stages that
  depend on it, or refuse to proceed until they are invalidated, so that
  downstream results cannot silently reference replaced parents.
  *Acceptance:* re-running `nuclei` marks a dependent `cells` stage as stale;
  querying `cells` afterwards reports staleness rather than returning objects
  whose parents no longer exist.

- **UR-STG-9 (Should)** — The workspace shall be a single file, or a directory
  whose location is a single path, so that a complete analysis can be copied or
  archived as one unit.

- **UR-STG-10 (Must)** — The workspace shall **declare its layers up front**,
  each as either semantic or primitive. Semantic layers carry an explicit
  ordering rooted at the pixel layer, branching permitted — `vessels` and
  `glands` may both sit above `tissue` without relating to each other. Primitive
  layers are declared by name alone: any number, in no order, outside the
  hierarchy. The declaration shall be queryable without running anything, and
  each stage shall declare the layers it writes.
  *Acceptance:* a workspace reports its semantic hierarchy and its primitive
  layers, and for each layer the stages that write it, before any stage has run;
  writing an object to an undeclared layer fails with a diagnostic.

- **UR-STG-11 (Should)** — Staleness (UR-STG-8) shall propagate along the
  semantic layer ordering and along the derived-from relation, as well as along
  declared stage dependencies, so that invalidating a layer marks the layers
  whose objects have parents in it or were built from it.
  *Acceptance:* re-running the stage that writes `tissue` marks `cells` and
  `nuclei` stale even where no stage dependency was declared between them;
  re-running a stage that writes a primitive layer marks the semantic layers
  derived from it stale.

- **UR-STG-12 (Must)** — A primitive layer shall be **droppable**: the workspace
  shall support discarding all objects of a primitive layer, and shall record
  whether each primitive layer was retained or dropped. Dropping shall not
  invalidate the objects derived from it (UR-RES-16) and shall not mark any
  semantic layer stale.
  *Acceptance:* dropping a superpixel layer holding more objects than the
  reference object count leaves every semantic layer complete and queryable, and
  the workspace reports that layer as dropped rather than as never run.

## 11. Execution and resources

- **UR-EXE-1 (Must)** — The library shall offer two execution modes:
  **lazy**, in which a pipeline is evaluated on demand per requested block, and
  **eager**, in which an explicit stage run streams the whole slide or ROI for
  operations needing global context or a persisted result.
  *Acceptance:* constructing a lazy pipeline performs no pixel computation,
  demonstrated by instrumentation.

- **UR-EXE-2 (Must)** — Peak memory shall be bounded by a caller-configurable
  ceiling that is independent of both slide size and object count, with a
  documented default. The ceiling shall cover raster tiles, object accumulation
  and store buffers together, not raster processing alone.
  *Acceptance:* a full three-stage run over the reference slide producing the
  reference object count, with a 2 GB ceiling, keeps measured peak RSS below the
  ceiling plus a documented allowance.

- **UR-EXE-3 (Must)** — The library shall parallelise work internally across a
  configurable worker pool, defaulting to hardware concurrency, with
  single-threaded operation available by configuration.
  *Acceptance:* throughput on the reference slide improves materially from 1 to
  4 workers; results are identical in both cases (see UR-QA-3).

- **UR-EXE-4 (Must)** — Long-running stages shall report progress and be
  cancellable, with cancellation taking effect promptly and leaving the
  workspace in the consistent state required by UR-STG-5.
  *Acceptance:* cancellation during a stage returns within a documented bound
  and leaves no partial objects visible.

- **UR-EXE-5 (Must)** — Errors shall be reported as documented exception types
  carrying actionable context (stage, operation, tile coordinates, underlying
  cause). A failure in one tile shall not corrupt the workspace or silently
  truncate results.
  *Acceptance:* an injected tile-level failure surfaces an exception naming the
  tile and stage; the stage is recorded as failed, not complete.

- **UR-EXE-6 (Should)** — Tile size shall be configurable, and the default shall
  be chosen from the source slide's native tiling where that information is
  available, to avoid re-decoding the same compressed tiles.

- **UR-EXE-7 (Should)** — Object writes shall be batched into transactions
  rather than committed per object, so that store throughput does not dominate
  a stage's runtime.
  *Acceptance:* writing the reference object count takes a documented and
  measured fraction of total stage time.

- **UR-EXE-8 (Should)** — On the **reference configuration** — a 12-core
  x86-64 machine, 32 GB RAM, and a local NVMe SSD with at least 20% free
  capacity — segmentation followed by classification of the reference slide
  shall complete **within 30 minutes end to end** at the reference object
  count, and within 10 minutes at 3 million objects. "End to end" means
  decode, compute, seam merge, object write and classification write,
  committed.
  *Acceptance:* both figures measured on the reference configuration and
  reported per run. Exceeding them is a performance regression, not a
  functional failure.

  > The reference configuration is named because every figure behind this
  > requirement is storage-bound. The measurements it rests on were taken on a
  > DRAM-less consumer SATA SSD near capacity, where the store's random-write
  > workload is close to worst case; a target quoted without its storage class
  > cannot be compared against anything.

  > 10 million objects on the reference slide is a packing limit, not a typical
  > case: 500 mm² of tissue at one object per 50 µm² is 8 µm mean spacing with
  > no background at all. A realistic section at the same density yields 3 to 6
  > million, which is why the requirement states both.

  > **The budget holds only if annotations reach the store in object order.**
  > Measured at ten million label assertions: 56 seconds ordered against 23
  > minutes shuffled, a factor of 24.6 that is larger than anything else
  > bearing on this requirement. It is a property of how an annotating stage is
  > scheduled, not of the store (SDS 03 §3.7). The realistic order — tiles
  > completed out of sequence, objects contiguous within a tile — measures 36.6
  > seconds, so the current schedule satisfies this comfortably. The condition
  > is stated because nothing enforces it: a stage annotating from a spatial
  > query rather than a tile sweep would fall into the shuffled case.

- **UR-EXE-9 (Should)** — Slides processed concurrently shall each hold their
  own workspace, and concurrency shall not degrade per-slide time by more than
  40% for a slide count up to the lesser of the core count and available RAM
  divided by the memory ceiling.
  *Acceptance:* per-slide wall-clock time at that concurrency is within 1.4× of
  the single-slide figure of UR-EXE-8, on the same configuration.

  > Stated as a scaling claim rather than an absolute, as UR-EXE-3 is.
  > Concurrent slides contend for one device, so linear scaling is not
  > available and should not be promised.

## 12. Extensibility

- **UR-EXT-1 (Must)** — The library shall expose a documented public
  `Operation` interface that users can implement to add processing steps.

- **UR-EXT-2 (Must)** — Built-in operations shall use the same public interface
  as user-supplied ones, with no privileged access to internals.
  *Acceptance:* at least one built-in filter is demonstrably implementable
  outside the library using only public headers.

- **UR-EXT-3 (Must)** — The `Operation` interface shall include an explicit halo
  declaration, and the framework shall supply the declared border context so
  neighbourhood operations are correct at tile seams.
  *Acceptance:* a custom operation declaring a halo of N produces output
  identical to single-tile processing; declaring a halo of 0 for an operation
  that needs one produces a detectable seam, confirming the mechanism is what
  provides correctness.

- **UR-EXT-4 (Must)** — The framework shall supply tiling, memory budgeting,
  threading, progress, cancellation and object persistence to every operation,
  so implementers write only per-tile computation and object emission.
  *Acceptance:* the reference custom operation from UR-EXT-2 contains no
  threading, memory-management or storage code.

- **UR-EXT-5 (Should)** — Users shall be able to define custom stages that emit
  objects into the workspace through the same mechanism as built-in stages, and
  to define custom per-object measurements.
  *Acceptance:* a user-defined stage's objects are indistinguishable in the
  store from built-in ones, and participate in parent relations and queries.

## 13. API and language bindings

- **UR-API-1 (Must)** — The public C++ API shall be designed to be wrappable in
  a Python extension module without modification, following the pattern used by
  `slideio-python`. Bindings are not delivered in this repository.

- **UR-API-2 (Must)** — Public headers shall avoid constructs hostile to
  binding and to ABI stability: no templates in the public API, no STL
  containers in exported signatures, implementation hidden behind opaque
  handles or pimpl.
  *Acceptance:* header review against a documented checklist, enforced in CI.

- **UR-API-3 (Must)** — Raster data shall be exposed as contiguous buffers with
  explicit shape, stride and data type, mappable to a NumPy array without a
  copy.
  *Acceptance:* a proof-of-concept binding reads a processed block as a NumPy
  array with no data copy.

- **UR-API-4 (Must)** — The cursor abstraction (UR-RES-2) shall be expressible
  as a Python iterator without materialising the underlying result set, and
  object geometry and measurements shall be convertible to NumPy arrays per
  object or per batch.
  *Acceptance:* a proof-of-concept binding iterates a 10-million-object stage
  from Python within the memory ceiling.

- **UR-API-5 (Must)** — All errors shall be reported through a documented,
  bounded exception hierarchy; no exception shall escape that hierarchy across
  the public API boundary.

- **UR-API-6 (Must)** — The library shall follow semantic versioning. Breaking
  changes to the public API shall be recorded in a maintained breaking-changes
  document, and changes to the workspace/store format shall be recorded there
  with their compatibility implications (UR-RES-12).

- **UR-API-7 (Should)** — The public API shall be documented with Doxygen, and
  every public entity shall carry documentation.
  *Acceptance:* the documentation build produces no undocumented-entity
  warnings.

## 14. Platform, build and quality

- **UR-BLD-1 (Must)** — The library shall build and pass its tests on Windows,
  Linux and macOS.
  *Acceptance:* CI is green on all three.

- **UR-BLD-2 (Must)** — The library shall target C++17, build with CMake, and
  manage dependencies with Conan, consistent with SlideIO.

- **UR-BLD-3 (Must)** — OpenCV shall be the computational backend for filtering,
  morphology, contour extraction and connected components.

- **UR-BLD-4 (Must)** — **The project intends to migrate to OpenCV 5 once it is
  available on the Conan server.** Until then, OpenCV 4's `cv::Mat` covers eight
  of the eleven channel data types in UR-IMG-8, having no depth for `UInt32`,
  `Int64` or `UInt64`; OpenCV 5 adds `CV_32U`, `CV_64S` and `CV_64U` and closes
  the gap.

  Nothing in the requirements shall encode the OpenCV 4 type set as a fixed
  limitation. Algorithms declare their supported types (UR-IMG-11) and the
  matrix is generated (UR-IMG-12), so migration widens the declarations without
  changing a requirement, an API, or a stored artifact.
  *Acceptance:* a review confirms no requirement, public API or store schema
  names the OpenCV 4 type set; the type-support matrix is derived at build time
  from the backend in use.

  > Recorded so that a temporary backend limitation is not mistaken for an
  > architectural one, and so the migration is a configuration change rather
  > than a redesign.

- **UR-BLD-5 (Must)** — SQLite, with its R*Tree module enabled, shall back the
  object store. It is already a SlideIO dependency, so this adds no new
  third-party surface.

- **UR-BLD-6 (Must)** — All dependencies shall carry licences compatible with
  the project's own open-source licence, and the dependency set with licences
  shall be documented and kept current.

- **UR-BLD-7 (Should)** — The build shall not require a SlideIO source tree;
  depending on released SlideIO packages shall be sufficient.

  > **Development route, pending a release.** No SlideIO release yet contains
  > the scene-source seam this project depends on. Until one does, SlideIO is
  > the git submodule `extern/slideio`, pinned to a commit, built with its own
  > `install.py` and installed to a local prefix. This project's build still
  > reaches it only through `find_package(slideio)` and `slideio::slideio`, so
  > it sees SlideIO's installed headers and nothing else, and switching to a
  > released package changes where the package is found, not the build.

- **UR-QA-1 (Must)** — Every requirement in this document shall be covered by at
  least one automated test, with a maintained traceability mapping from
  requirement ID to test.

- **UR-QA-2 (Must)** — Correctness of tiled processing shall be verified by
  comparing whole-slide output against single-tile reference output on images
  small enough to process in one tile, for every operation.

- **UR-QA-3 (Must)** — **Results shall be deterministic and invariant to worker
  count and tile size.** Repeated runs of the same pipeline on the same input
  shall produce identical output, including object identifiers and their
  ordering.
  *Acceptance:* a test runs a segmentation stage across the cross-product of
  {1, 4, 8} workers and {512, 1024, 4096} tile sizes and asserts byte-identical
  exported results.

  > Without this, no user can write a regression test against the library's
  > output, and no result can be reproduced from a publication. Note that it
  > constrains the implementation: object identifiers and any cross-tile
  > reduction must use a canonical ordering (for example by tile coordinate)
  > rather than completion order.

  > Object identifiers are derived from each object's anchor pixel — its
  > topmost-then-leftmost pixel in level-0 slide coordinates — which is a
  > property of the geometry alone and therefore invariant to tile size and
  > worker count by construction. Identifiers keyed off tile index or write
  > order are **not** tile-size invariant and do not satisfy this requirement.

- **UR-QA-4 (Must)** — **Interruption shall not change results.** A stage killed
  mid-run and then re-run shall produce output identical to the same stage run
  without interruption.
  *Acceptance:* a test terminates the process during a stage, reopens the
  workspace, re-runs the stage, and asserts its exported results are
  byte-identical to an uninterrupted run.

- **UR-QA-5 (Must)** — The test suite shall include scale tests exercising the
  memory ceiling, the reference object count, and a pixel count exceeding 2^31
  (UR-IMG-2), runnable in CI or clearly marked as requiring a larger runner.

- **UR-QA-6 (Must)** — Every operation shall be tested against **every channel
  data type** in UR-IMG-8: supported types for numerical correctness,
  unsupported types for the declared error of UR-IMG-11. Mixed-type
  multi-channel scenes shall be covered.
  *Acceptance:* the generated support matrix (UR-IMG-12) has a corresponding
  test for each cell.

- **UR-QA-7 (Should)** — The library shall be free of memory errors and leaks
  under sanitizer and Valgrind runs on the core test suite.

---

## 15. Non-goals

The following are deliberately **out of scope for v1**. They are recorded so
that they are added, if ever, by decision rather than by drift.

| Not in v1 | Rationale |
|---|---|
| **Tile-level checkpointing within a stage** | Deferred, but the format and execution model must accommodate it without a breaking change (UR-STG-7). |
| **Tiling / dataset generation for ML** | Valuable, but a distinct concern with its own requirements. Revisit after the core is stable. |
| **ML model inference** (ONNX, OpenVINO, libtorch) | Large dependency surface, and would raise EU AI Act considerations for any downstream clinical integrator. |
| **Distributed or multi-machine execution** | Single-machine scale is sufficient for the stated targets; the workspace format should not preclude it. |
| **Concurrent writers to one workspace** | One writer at a time. Concurrent *readers* alongside a writer are **required** — see UR-OBS-1. |
| **Image viewing or GUI** | Belongs in `slideio-view` or QuPath. |
| **Annotation authoring / editing** | The library imports and exports annotations; it does not provide editing. |
| **Writing WSI formats** | Delegated to `slideio-converter`. |
| **GPU acceleration** | Not required to meet the stated performance goals. The `Operation` interface should not preclude it later. |
| **Registration / stitching of multiple slides** | A separate problem domain. |
| **Python bindings in this repository** | The C++ API is constrained to be bindable; bindings ship separately. |

## 16. Assumptions and dependencies

1. SlideIO provides format decoding, pyramid access, scene metadata and
   resolution calibration; this project adds no format drivers.
2. The SlideIO `Scene` contract is **stable**, and its type model
   accommodates the channel data types intermediate results require — it
   carries `DT_Float16`, `DT_Float32` and `DT_Float64`, and `TransformerScene`
   already assigns a type per channel. It was, until the extension seam of
   question 3's closure, also **closed**: `cvscene.hpp` is deliberately not
   installed, because it includes `<opencv2/core.hpp>` and shipping it would
   make OpenCV a public dependency of SlideIO. This project therefore depends
   on SlideIO as a reader *and* contributes the seam by which a computed scene
   is constructed, rather than implementing `Scene` from outside. *The original
   wording of this assumption — "stable enough to implement against" — bundled
   two claims and failed on the second.*
3. `slideio-converter` is available for writing pyramidal TIFF output.
4. OpenCV is available on all target platforms via Conan.
5. SQLite is available with the R*Tree module enabled on all target platforms,
   and sustains the insert and query rates the reference object count implies.
   *Measured and holding, against UR-EXE-8's bar and with the physical-key
   scheme of SDS 03 §3.12 — see the resolution of question 4.*
6. A workspace is written by one process at a time.
7. Test slides representative of the supported formats are available, including
   at least one slide at or near the reference size and one analysis reaching
   the reference object count.
8. The project is open-source under a permissive licence; the specific licence
   is to be confirmed.

## 17. Open questions

Numbers are not reused. A question resolved after this document was first
issued keeps its number here as a gap and appears in the table below — question
8 is the first.

| # | Question | Why it matters |
|---|---|---|
| 1 | Which open-source licence? | Determines dependency compatibility (UR-BLD-6) and downstream adoption. |
| 2 | Minimum supported SlideIO version, pinned or a range? | Affects UR-BLD-7 and the release process. The floor is the first release carrying the scene-source seam (SlideIO PR #86); until it exists, development pins the `extern/slideio` submodule to a commit. |
| 5 | How is geometry encoded in the store — WKB, a compact custom encoding, or simplified contours? | Drives store size, query cost and GeoJSON export speed at 10 million objects. Coordinates must be 64-bit-safe per UR-IMG-2. |
| 6 | Is `Float16` computed natively or promoted to `Float32`? | OpenCV has `CV_16F` storage but limited arithmetic on it; promotion changes rounding and must be declared per algorithm under UR-IMG-11. |
| 11 | Is a stable C ABI wanted later for non-Python bindings? | Cheap to accommodate in the API design now, expensive to retrofit. |
| 12 | Should an observer be able to *cancel* a running stage, or only watch it? | Turns the viewer from an observer into a controller, which needs a second channel and a permission model. Out of scope as §9a stands. |
| 13 | What is the journal retention default — unbounded, a row count, or truncate-on-stage-complete? | Trades journal size against how far back a late-joining observer can replay (UR-OBS-7). |
| 14 | Should the journal record read activity, or only writes? | Relevant only if an audit trail is ever wanted; adds write load for no benefit to the scenario §9a states. |
| 15 | **UR-PIX-3 requires Canny and states its acceptance as "matches a single-tile reference", without tolerance. Canny's hysteresis stage tracks edge chains of unbounded length, which no fixed halo makes exact.** Should the acceptance gain a stated bound, should Canny be specified as a documented approximation, or should it move out of v1? | Surfaced by resolving question 7. As written the requirement cannot be met unconditionally by a tiled implementation, so one of the three must give. Affects UR-PIX-3 and `06-algorithm.md` §6.4. |
| 16 | **Does OME-TIFF accept a scene whose channels differ in data type?** SVS rejects them outright (scene-source design §9.1). | Bounds `convertScene` for any computed scene with mixed output types — the lazy pipeline's normal case once stain deconvolution feeds a chain. Raised by question 3's closure, which makes such scenes constructible without settling what can be written. |

**Resolved during review** — recorded so the reasoning is not relitigated:

| Question | Resolution |
|---|---|
| Mixed per-channel data types | No library-level restriction and no normalisation (UR-IMG-9). The caller selects channels; each algorithm declares its channel arity and any type requirement (UR-ALG-2, UR-ALG-3). |
| The OpenCV 4 type gap | Temporary. Migration to OpenCV 5 is planned once it reaches the Conan server (UR-BLD-4); nothing may encode the OpenCV 4 type set as fixed. |
| OpenCV API compatibility | None taken. Same spirit, own names, parameters and numerics — which is why every algorithm needs its own correctness reference (UR-ALG-6). |
| Layer ordering, parents and declaration | Layers branch — a tree rooted at the pixel layer, not a single stack. A parent must lie in *some* lower layer, not necessarily the adjacent one. Layers are declared by the workspace up front (UR-RES-13, UR-RES-14, UR-STG-10). |
| Primitive vs semantic layers | Two kinds of layer. Semantic layers form the branching containment hierarchy rooted at the pixel layer and are the only participants in parent edges. Primitive layers are arbitrary — any number, unordered, outside the hierarchy, droppable — and are linked to semantic objects by the many-to-many derived-from relation (UR-RES-16, UR-STG-10, UR-STG-12). |
| Free-form vs controlled class labels | Controlled, but not closed. The library ships a documented, versioned standard vocabulary, and a workspace may add labels of its own at any time, marked as local wherever they are reported (UR-RES-17 .. UR-RES-19). |
| The v1 standard vocabulary (was question 8) | Ten labels: `tumour`, `stroma`, `necrosis`, `immune`, `lymphocyte`, `positive`, `negative`, `artefact`, `ignore`, `other`. Specified with definitions and conventional layers in SDS `02-config.md` §2.2. The question's rule — "cover what the shipped algorithms emit, so downstream of question 7" — did not survive checking: no v1 operation emits a label, so that rule yields the empty set and the dependency on question 7 is weak. The vocabulary's actual job is clean import under `ImportPolicy::Strict` and a shared spine for user classification stages, both of which are v1. No `tissue` label, because that is a layer name; `tumour` is one label used at both region and cell scale, because the layer distinguishes them. |
| Target throughput (was question 10) | **30 minutes**, for segmentation and classification of the reference slide at the reference object count, on a named reference configuration — now UR-EXE-8, with UR-EXE-9 covering batch. The bar had to name hardware: every figure behind it is storage-bound, and the spike's measurements were taken on a DRAM-less SATA SSD near capacity. It also had to name the *work*, not a rate: a throughput figure in objects per second is gameable by moving work out of the measured phase. |
| SQLite and R\*Tree at the reference object count (was question 4) | **Yes, conditional on the physical-key scheme.** The spike measured query cost scaling with result size rather than store size — the property the design needs — realistic viewer viewports at 2 to 39 ms including geometry, and UR-OBS-1's concurrent reader holding exactly: 69,477 queries during an active 10M write, zero errors, no measurable writer slowdown. Against UR-EXE-8's 30 minutes the two insert schemes diverge decisively: sequential primary keys take 7.7 minutes, 26% of budget; **anchor-derived primary keys take 31.6 minutes and exceed the entire budget before a pixel is read.** So the anchor remains the logical identity UR-QA-3 requires and the physical key becomes a monotonic rowid with a unique index on it (SDS 03 §3.12). **Both measurements were taken on 2026-10-10.** The rowid scheme returns **2.39×** of the insert penalty, costs nothing in space — the store is 103 MB *smaller* than the anchor-keyed one, because random inserts leave pages two-thirds full while appends pack them — and adds 6 µs to `get(ObjectId)`; sorting each batch by anchor, offered as a cheaper partial measure, measured 18% **slower** and is ruled out. The annotation pass is cheap when ordered and ruinous when not: ten million label assertions cost **56 seconds** in ascending object order against **23 minutes** shuffled. Both assertion tables become `WITHOUT ROWID` on their natural key, which is the reverse of the objects table's answer and 2.6× smaller, and measurement names are interned for a further 808 MB per slide (SDS 03 §3.7). |
| One workspace per slide (was question 9) | **Exactly one.** Forced by UR-EXE-9: batch processing needs N slides to be N independent single-writer stores, which satisfies Assumption 6 without coordination. A shared multi-slide workspace serialises every writer and makes batch concurrency unavailable. It also keeps UR-STG-9's archiving unit coherent now that a workspace holds rasters beside its store — one directory is one slide's results. |
| Float intermediates and the `Scene` model (was question 3) | **Closed on a direction.** The type model carries floats and always did; the failure was reachability — `cvscene.hpp` and `sceneinternal.hpp` are not installed, `Scene`'s constructor is private, and the one public-looking extension point (`Transformation`) is undiagnosed undefined behaviour when subclassed from outside. The direction: **SlideIO gains a public extension seam** — `ISceneSource` (how to read), `SceneMetadata` (what the scene is, carrying a `DataType` per channel), and `makeScene` — none of which leaks OpenCV into public headers, which is the constraint that caused the closure in the first place. `slideio-processor` implements `ISceneSource`; `Pipeline::asScene()` becomes reachable and SDS 05's raster architecture survives intact. Designed in `superpowers/specs/2026-10-08-scene-source-design.md`, planned in `superpowers/plans/2026-10-08-scene-source.md`. Chosen over giving `pipeline` its own raster type (the spike's own recommendation) because it keeps one scene abstraction across both projects; chosen over consuming SlideIO's internal headers, which would make OpenCV a shared ABI surface. |
| Conditions on question 3's closure | The direction holds **only if** three things are done, and the closure should be reopened if any is refused. **(1)** The scene-source plan lands in SlideIO — until then the state is "resolved, remedy in progress", not "delivered". **(2)** `CVScene::initializeSceneBlock` is fixed upstream: it sizes a block from channel **0**'s type regardless of which channels were asked for, so a mixed-type scene silently returns a mis-sized block, and for a source-backed scene that is a buffer overrun rather than a wrong answer. `SourceCVScene` guards itself, but every other driver still inherits the defect. **(3)** `makeTransformationCopy`'s undiagnosed downcast is fixed: a public abstract class whose misuse is silent undefined behaviour must either reject unknown subclasses or stop being public. (2) and (3) are upstream defects and **neither has been filed**. |
| Mixed channel types are read one channel per call | Not a loss, and worth recording because it looks like one. `cv::Mat` carries one depth for all its channels, so no single `readBlock` can deliver a block spanning `Byte` and `Float32` — a restriction of every SlideIO scene, not of computed ones. Each channel reads back losslessly on its own call, `SceneMetadata::channelDataTypes` declares the mix so a consumer can group its requests, and `core::BlockView` is planar with a `DataType` per plane for exactly this reason (SDS 01 §1.8, SDS 05). |
| The v1 algorithm catalog (was question 7) | Closed at twenty-two operations, enumerated with arity and halo in SDS `06-algorithm.md` §6.10. The list is UR-PIX-1..6, UR-IMG-13 and UR-SEG-1..7 exhausted rather than a selection from them — every entry answers a Must or Should, and every Must and Should has an entry or a framework mechanism. Ten filter and morphology operations received catalog names for the first time, which is durable contract because a stage records the name and replays it (UR-STG-4). **Amended in 0.11:** UR-SEG-9 was added after this resolution was taken, and v1 ships no operation for it, so the two columns no longer exhaust each other. The catalog remains closed at twenty-two; what changed is that the closure is now a decision rather than a derivation, and SDS 06 §6.10 records it as such. |
| Watershed in v1 (was question 7) | **Marker-controlled watershed is in v1**; unseeded watershed is not. The earlier exclusion rested on basin reach being unbounded, which is true only of the unseeded variant. With markers, a basin grows until it meets a neighbour, so reach is bounded by marker spacing (~28 px for nuclei at 0.25 µm/px) rather than by component size — a clump inside a fifty-tile sheet is still a local decision. `maximumMarkerSpacingPx` derives the halo and is enforced: a pixel further than that from any marker in context throws, which keeps UR-QA-2 exact rather than letting the result depend on tile position. Unseeded watershed stays out, and not as a deferral — it needs a post-merge pass the design does not have. |
| Multi-resolution coarse-to-fine (arising from question 7) | A supported pattern, not a user-assembled workaround. A coarse stage runs at a level where the whole slide fits (10⁸ px at level 3–4) and writes regions; a fine stage binds those regions as an auxiliary **raster** — pixels, not a go/no-go mask — and segments detail the coarse level could not resolve. New `core::TileRasterSource` and `makeLayerRasterSource`, `Pipeline::auxiliaryRaster`, `OperationContext::auxiliary`, `StageDefinition::readsLayerAsRaster`. Per-tile and lazy, like the restriction mask it sits beside. The two halves are correct for different reasons — the coarse because the slide fits, the fine because marker flooding is local — and neither inherits the other's argument. Topology is fixed by the coarse pass: refinement moves boundaries, never adds or removes them. |
| Persisted rasters have no parent requirement | Open action. UR-RES-7 requires label and probability maps be *obtainable* as a multi-resolution raster — a lazy scene or a pyramidal TIFF — and the design satisfies it. It does not require them to be **stored as stage products**, with attribution, discard-on-re-run and derived staleness, which is what SDS §3.8 adds. The capability exists because products without a contour — a distance transform, a probability surface, a background estimate — cannot be expressed as objects and so had no way to cross a stage boundary at all. Two consequences need requirement-level decisions rather than design ones: **UR-STG-9's archivable unit changes** from one `.store` file to the workspace directory, and **there is no disk budget anywhere** to bound what rasters may consume, against UR-EXE-2's memory ceiling. |
| Watershed has no parent requirement (arising from question 7) | **Closed in 0.6** by adding UR-SEG-8, separation of touching objects. Recorded because the reasoning is worth keeping: UR-SEG-2 requires connected-component labelling, which merges whatever touches, so nothing obliged the library to separate — yet without separation a nuclear count is a count of clusters and any derived index, a Ki67 index above all, inherits that error. The gap was found by building the §6.10 requirement-to-operation mapping in both directions and noticing one column had an entry the other did not. |
| Superpixels in v1 (was question 7) | Excluded, breaking no commitment — no requirement names them. Primitive layers remain supported by store and workspace (UR-RES-16, UR-STG-12); v1 ships no algorithm that populates one, and GeoJSON import reaches them anyway. No architectural obstacle, unlike watershed. |
| Built-in stain vectors (was question 7) | H&E and H-DAB only, which is exactly UR-PIX-2's floor. Each further built-in vector is published coefficients the library asserts as correct, with a citation and a correctness test to maintain (UR-ALG-6), while `StainVectors::setVector` already covers every other stain at no cost to the caller. |
| External ontology identifiers (was question 8) | Deferred, surface reserved. `labels.ontology_ref` and `StandardVocabulary::ontologyRef` ship in v1 returning empty, because adding a column later would break UR-RES-18's no-migration promise for every store already written, and because UR-API-2 names ABI stability as a design constraint while question 11 still leaves a stable C ABI possible. Populating the field later is only a vocabulary version bump. The mappings themselves are contested for the looser terms and each is an ongoing maintenance obligation. |
