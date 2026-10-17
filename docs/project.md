# SlideIO Processor

## Goal

An open-source, cross-platform C++ library providing **a catalog of
image-processing algorithms for images larger than memory** — in the spirit of
OpenCV, but where every algorithm is scale-aware by construction.

The user picks algorithms and composes them; the library handles tiling, halos,
memory budgeting, threading and persistence. An algorithm behaves the same on a
1,000-pixel thumbnail as on a 10-billion-pixel slide, and the caller writes the
same code either way.

The library builds on the [SlideIO](https://github.com/Booritas/slideio) reader
library, which supplies format decoding, zoom pyramids and the `Scene` raster
abstraction. `slideio-processor` is a separate project, versioned and released
independently.

**Research use only. Not for diagnostic use.**

## No fixed limits

The library imposes no limit of its own on image dimensions, pixel count,
channel count, or object count. Any limit is one inherited from SlideIO, the
platform, or available storage.

All eleven of SlideIO's channel data types are supported — `Byte`, `Int8`,
`UInt16`, `Int16`, `UInt32`, `Int32`, `UInt64`, `Int64`, `Float16`, `Float32`,
`Float64` — including scenes whose channels differ in type from one another.
Restrictions belong to individual algorithms, never to the library.

The figures below are **verification anchors, not ceilings**: they fix what the
test suite must demonstrate, and nothing in the design may treat them as bounds.

| Anchor | Value |
|---|---|
| **Reference slide** | 100,000 x 80,000 px, 3 x 8-bit channels (~24 GB uncompressed, 8x10^9 px) |
| **Reference object count** | 10 million objects in a single stage |
| **Memory ceiling** | Configurable, independent of both — default 2 GB |

## Key decisions

| Decision | Choice |
|---|---|
| **Relationship to SlideIO** | Separate library depending on SlideIO's public reader API, plus one additive contribution to it: the scene-source seam that makes a computed scene constructible from outside the tree (see *Raster execution*). No SlideIO internal header is consumed. Until a SlideIO release carries the seam, SlideIO is a git submodule (`extern/slideio`) pinned to a commit, installed locally and found with `find_package(slideio)` as a released package would be. |
| **What the library is** | A catalog of algorithms. The framework supplies scale; the catalog is the product. |
| **OpenCV compatibility** | Same spirit only — own names, parameters and numerics. Each algorithm therefore carries its own documented correctness reference. |
| **Channel contract** | The caller selects channels. Each algorithm declares its arity: *per-channel* (independent, mixed types always fine) or *joint* (may require uniform types, and must say so). |
| **Arithmetic** | 64-bit throughout for pixel counts, areas, indices and accumulators — 32-bit overflows at 2.1x10^9, which one reference slide already exceeds. |
| **Pixel types** | All 11 SlideIO types carried losslessly. Support declared per algorithm and matrix generated from the backend in use. |
| **Backend** | OpenCV 4 now, **migrating to OpenCV 5** when it reaches the Conan server — which closes the `UInt32`/`Int64`/`UInt64` gap with no requirement or API change. |
| **Raster execution** | Lazy pipeline exposed as a `Scene`, plus an explicit eager pass for global-context work. Reaching this required a **public extension seam in SlideIO** — `ISceneSource`, `SceneMetadata`, `makeScene` — because `Scene` is constructible only from a `CVScene`, whose header is deliberately not installed. The seam keeps OpenCV out of SlideIO's public headers, so the two projects stay independently releasable (open question 3). |
| **Object results** | Persistent, spatially-indexed store (SQLite + R\*Tree); read through streaming cursors, never materialised whole. Object identity is the geometry-derived anchor; the table is physically ordered by a monotonic rowid, because keying it by the anchor costs 4.1× on insert. |
| **Performance** | 30 minutes for segmentation and classification of the reference slide at the reference object count, on a named reference configuration (UR-EXE-8). One slide per workspace, so batch runs as independent single-writer stores. |
| **Multi-stage** | A durable *workspace* holds the object store and per-stage state. Stages are named, declare dependencies, and are individually re-runnable. |
| **Interruption** | Durable at stage boundaries in v1; format and execution model designed so tile-level checkpointing can be added without a breaking change. |
| **Object relations** | Parent/child, so stages attach to earlier stages (nucleus to cell to tissue region). |
| **Layers** | Objects belong to declared **layers**. Semantic layers — pixel layer, tissue, cells, nuclei — form a branching containment hierarchy and are the only participants in parent edges; primitive layers (superpixels, basins, fragments) are arbitrary, unordered, droppable, and linked to semantic objects by a many-to-many derived-from relation. |
| **Class labels** | A shipped standard vocabulary, documented and versioned; a workspace may add labels of its own at any time, marked as local wherever they are reported. |
| **Interop** | GeoJSON (QuPath-compatible), CSV/Parquet tables, pyramidal masks. |
| **Extensibility** | Public `Operation` interface with explicit halo, channel-arity and data-type contracts. |
| **Python** | Binding-friendly C++ API; bindings ship in a separate repository. |

## Documents

- [User Requirements](requirements.md) — the full specification: 90 numbered,
  testable requirements with acceptance criteria, non-goals, assumptions and
  open questions.
- [Module Structure](superpowers/specs/2026-09-28-module-structure-design.md) —
  the seven-module decomposition, its dependency rules, and the nine design
  decisions behind it.
- [Software Design Specification](superpowers/specs/sds/README.md) — the public
  API of all seven modules, one document per module plus shared conventions,
  with [class diagrams](superpowers/specs/sds/08-class-diagrams.md) of all
  seven and of the dependencies between them.
- [Observation requirements (draft)](requirements-9a-observation-draft.md) —
  UR-OBS-1..7 for live observation of a running analysis by a separate viewer
  process, plus two amendments to `requirements.md`. Not yet merged.
- [Scene source design](superpowers/specs/2026-10-08-scene-source-design.md) —
  the SlideIO addition (`ISceneSource`, `SceneMetadata`, `makeScene`) that makes
  "lazy pipeline exposed as a `Scene`" achievable, and the `Pipeline::asScene()`
  implementation on top of it. Resolves open question 3.
- [Scene source implementation plan](superpowers/plans/2026-10-08-scene-source.md) —
  eight TDD tasks delivering the SlideIO half, **delivered** as SlideIO PR #86
  (merged 2026-10-17). Excludes `Pipeline::asScene()`,
  which waits on the `pipeline` module existing.
- [Whole-level operations](superpowers/specs/2026-10-11-whole-level-operations.md)
  — an operation cannot declare that it needs the analysis level in one block,
  and §6.7's coarse watershed already depends on getting it. Proposes
  `tileRequirement()`, a declared per-pixel working set, and enforcement in
  `Pipeline::validate()`. Design note; nothing amended.
- [Multiresolution segmentation](superpowers/specs/2026-10-11-multiresolution-segmentation.md)
  — whether Baatz & Schäpe region merging fits this design. It does, seeded
  from a partition rather than from pixels: the fusion factor depends only on
  additive per-segment aggregates, so growth needs no pixels and the existing
  global-context contract carries it at level 0. Design note; nothing amended.
- [Label rasters as geometry](superpowers/specs/2026-10-11-label-raster-geometry.md)
  — whether objects should be stored as an id-filled raster in HDF5 instead of
  polygons. Yes as a persisted companion to the object table: the anchor raster
  already exists per tile and pixel-local operations become trivial. No as a
  replacement, and no to HDF5. Design note; nothing amended.
- [Storage measurements](superpowers/specs/2026-10-10-storage-measurements.md)
  — the two measurements the spike left open, taken: the physical key scheme
  (2.39×), the annotation schemas (`WITHOUT ROWID`, interned measurement names,
  808 MB per slide), and the factor that dwarfs them both — annotation write
  order, at 24.6×. Raw logs and benchmark sources attached. SDS 03 is now
  freezable.
- [Spike findings, open questions 3 and 4](superpowers/specs/2026-10-07-spike-q3-q4-findings.md)
  — the two load-bearing questions, measured. Question 3 resolved **no**, and
  the "lazy pipeline exposed as a `Scene`" decision above now rests on the
  scene-source seam rather than on implementing `Scene` from outside; question 4
  resolved **yes**, with a 4.1x insert cost that made the physical-key change of
  SDS 03 §3.12 mandatory once UR-EXE-8 set a 30-minute bar. Consequences applied;
  two measurements remain open and are named in SDS 03 §3.12.
