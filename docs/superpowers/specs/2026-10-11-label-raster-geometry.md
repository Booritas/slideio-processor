# Label rasters as object geometry — assessment

**Status:** design note. Nothing amended; §6 is a recommendation.
**Date:** 2026-10-11
**Question:** should a layer's objects be stored as a raster whose pixels hold
the object's identifier, in HDF5, instead of (or alongside) polygons?
**Touches:** SDS [01](sds/01-core.md) §1.3, §1.9, §1.17 ·
[03](sds/03-storage.md) §3.5, §3.8, §3.12 · `requirements.md` open question 5,
UR-RES-3, UR-RES-8, UR-QA-3, UR-SEG-3, UR-OBS-1, UR-STG-6, UR-STG-9

---

## 0. Verdict

| Part of the proposal | Assessment |
|---|---|
| A per-layer label raster holding object identity | **Yes, as a persisted companion to the object table.** The design already computes this raster. `makeLayerRasterSource` with `ObjectAnchor` (01 §1.9) yields `Int64` anchor values, and today it does that per tile on every read. Persisting it caches work the pipeline already pays for. |
| The raster **replacing** polygons | **No.** It replaces the geometry column and nothing else. Identity, bbox R\*Tree, labels, measurements, edges and the journal all still need the SQLite store. It also gives up overlap, vector import, coarse-level exactness and export, and saves no disk (§3). |
| HDF5 as the container | **No.** Four properties work against this workload: crash consistency, the global lock, SWMR limits and unreclaimed space (§4). Pyramidal TIFF through the existing raster store (03 §3.8), or OME-Zarr with its `labels` convention, does the job without them. |

The useful idea is sound: **many object operations are cheaper as pixel
operations.** The design should take that gain and leave the rest of the
proposal aside.

---

## 1. What the design already has

This proposal is closer to the current design than it first looks:

- `TileRasterSource` plus `makeLayerRasterSource(..., ObjectAnchor)` gives,
  for any tile, a raster whose pixel value is the anchor of the object that
  covers it (`(y << 31) | x`, `Int64`). That is exactly "a raster filled with
  object id."
- The rasteriser of 01 §1.17 and a conformance test already require the scalar
  `Polygon::contains` and the raster answer to agree.
- `RasterRegistry` (03 §3.8) already persists stage-attributed pyramidal
  rasters, handles discard and sweeps orphaned files.

So the real question is narrower than "polygons or raster": **should the
anchor raster be persisted, and if so, which representation is canonical?**

---

## 2. What gets easier

The claim holds for operations that are **pixel-local or zonal**, and it
holds strongly:

| Operation | Polygon path | Label-raster path |
|---|---|---|
| Which object covers pixel *p*? | R\*Tree query, then `contains` on each candidate | Read one value. One chunk decode when the chunk is cold, nothing when it is cached. |
| Parent assignment (nucleus → cell → tissue), UR-RES-5 | Rasterise both layers per tile, or do pairwise polygon tests | Read the child and parent rasters together and take a majority vote per child. One streaming pass. |
| Per-object intensity measurements | Rasterise each polygon over the tile | `bincount` keyed by label over the tile. Zonal statistics. |
| Overlap area between objects of two layers | Polygon clipping, which gets fragile on degenerate rings | Joint histogram of the two label tiles |
| Holes, multi-outer regions, even-odd ambiguity (01 §1.3) | A specified rule plus a conformance test | Cannot arise: membership is the value itself |
| Distance to nearest object, rings and expansion | No shape-changing operation in `core`, by decision | Distance transform over the label tile |

**Single-pixel lookups are not much faster.** A cold lookup decodes a whole
chunk. A 256² `Int64` chunk is 512 KB, which costs roughly a millisecond, and
the spike measured a 1,024 px viewport query at 1.83 ms. The gain comes when
many lookups share a chunk, which is the usual case for anything tile-driven.

---

## 3. What gets harder or is lost

### 3.1 It does not replace the store

A viewport query (UR-RES-3) is answered by the R\*Tree in time proportional
to the **result**. On a label raster it costs time proportional to the
**pixels**. A 16,384 px viewport is 2.7×10⁸ pixels to decode and `unique()`,
against the measured 192 ms join. Cursors, counts, labels, measurements,
assertions, edges and the event journal of UR-OBS-2 have no raster form. The
object table stays. What the raster can replace is the geometry blob, and
only for some layers.

### 3.2 No overlap within a layer

One value per pixel means two objects in one layer cannot share a pixel.
Today the store forbids only a shared **anchor** (03 §3.5), and overlapping
shapes are legal. Three cases would break:

- **GeoJSON import (UR-RES-8):** QuPath annotations overlap routinely.
- **Cell expansion** where neighbours' cytoplasm regions are not partitioned.
- **Manually drawn regions** that nest or cross.

Each of these would need a separate layer or a multi-plane raster. "No
overlap within a raster-backed layer" would become a new constraint at
requirement level.

### 3.3 Identity and the seam

The pixel value must be tile-invariant (UR-QA-3, UR-SEG-3), so it cannot be a
per-tile counter. It must be the anchor, or a dense number assigned
deterministically after the stage completes.

- **Anchor values (`Int64`)** are invariant by construction, but an object
  that crosses a seam does not know its anchor until the seam merge finishes.
  The tile that holds it cannot be finalised until its neighbours finish. The
  polygon path defers work in the same way, but its deferred unit is a few
  hundred bytes of contour. Here it is megabytes of pixels held against the
  UR-EXE-2 ceiling.
- **Dense `UInt32` values** halve the bytes, but they need a relabel pass over
  the whole level after the stage. That pass rewrites 8×10⁹ pixels, which is
  a second full write inside UR-EXE-8's 30 minutes.

The anchor's definition also changes subject. It is currently the
topmost-then-leftmost **vertex**. On a raster it is the topmost-then-leftmost
**pixel**. These coincide for pixel-traced contours, but the store-format
stability contract (01 §1.3) has to say which one rules.

### 3.4 Resolution is fixed by the written level

A label raster is exact at the level where it was written and lossy at every
other level:

- **Downsampling** has to use mode or nearest, never averaging. Objects
  smaller than the coarser pixel disappear from the pyramid.
- **Upsampling** a level-4 tissue region to level 0 produces 16-pixel
  staircases. A polygon gives the same integer coordinates "at every level"
  (01 §1.3).
- **Vector imports** have to be rasterised, which loses detail.

### 3.5 Shape and export still need contours

Area is cheap: a pixel count. Perimeter on a staircase overestimates by up to
about 27% for a circle, so UR-SEG-5-style shape descriptors need a traced
contour anyway. GeoJSON export (UR-RES-8) needs contour tracing for all 10⁷
objects, which moves that cost from segmentation to export rather than
removing it.

### 3.6 Disk: same order of magnitude, not a saving

This is an estimate, not a measurement. Python is not on this machine, so it
could not be checked here. A label raster compresses well because almost all
of its information sits at boundaries. At the reference density (10⁷ objects
in 8×10⁹ px, nucleus-sized, about 20 rows each) that is roughly 40 runs per
object, 4×10⁸ runs in total. At 6–10 compressed bytes per run, level 0 comes
to 2.4–4 GB. The pyramid adds about a third.

The polygon store's whole objects table is 3.0 GB, with geometry estimated at
about 200 B per object. **The raster is comparable, and it is additive
whenever the object table stays, which §3.1 says it must.** Open question 5
still decides the real disk budget.

---

## 4. Why not HDF5

HDF5 is a reasonable scientific container, and `h5py` interop is attractive.
Four of its properties conflict with requirements this design already met
elsewhere at some cost:

| Property | Conflict |
|---|---|
| **No crash consistency.** A crash during a metadata write can make the whole file unreadable. | §3.8 guarantees that a crash leaves an orphan file, never a lie, by using a generated file per raster and publishing only after the write completes. If one HDF5 file holds every layer, one crash puts all of them at risk. If each raster gets its own file, most of the reason to choose HDF5 is gone. |
| **Global lock.** The thread-safe build serialises every API call, filters included. | Compression runs single-threaded. Deflate on 64 GB of raw `Int64` at 100–300 MB/s takes 4–10 minutes of the 30-minute UR-EXE-8 budget. Writing pre-compressed chunks with `H5Dwrite_chunk` avoids this, but then the library does its own codec work. |
| **SWMR is restrictive.** No dataset creation in SWMR mode, append-oriented, and it needs the newer file format. | UR-OBS-1 requires a separate viewer process to read during a write. SQLite already measured this at 69,477 queries with zero errors. |
| **Deleted datasets do not shrink the file.** Getting the space back needs `h5repack`. | `discardStage` and re-runs (UR-STG-6) would grow the file without bound, on a workspace that already has no disk budget. |

**SlideIO does not read HDF5.** §3.8 chose pyramidal TIFF so that "a consumer
cannot tell whether the coarse product it reads was pixels or objects" and so
that QuPath and SlideIO can open it. HDF5 would need its own reader and drop
that interop. If a chunked n-dimensional container is wanted, **OME-Zarr**
fits better: it has a defined `labels` convention for exactly this kind of
image, multiscale metadata, a chunk-per-file layout that cannot corrupt
siblings on a crash, and parallel chunk writes with no global lock. Its cost
is file count: about 1.2×10⁵ chunk files per level-0 layer at 256².

---

## 5. Where a label raster should be canonical

The geometry has to be canonical in one place, or the polygon and the raster
can disagree. The natural split follows each layer's origin:

| Layer origin | Canonical | Derived |
|---|---|---|
| Pixel segmentation (watershed, threshold + connected components, ML label output) | **Label raster.** The algorithm produced labels, and the contour is a lossy trace of them. | Polygon, traced at write time or lazily |
| Vector import, coarse regions used at fine levels, overlapping layers | **Polygon** | Label raster, through the existing per-tile rasteriser |

Under this split, open question 5 gets a new option. For raster-canonical
layers the stored polygon only serves export and display, so a **simplified
contour** becomes acceptable. That could be the largest single cut in store
size.

---

## 6. Recommendation

1. **Add a raster companion to a layer, rather than replacing its
   geometry.** A layer declaration may name a persisted anchor raster, written
   by the producing stage through `RasterRegistry` (03 §3.8). It uses `Int64`
   anchor values, pyramidal TIFF to start, and mode downsampling. Readers that
   want pixels get it from `sourceFor` with no rasterisation. Readers that
   want objects use the store as before.
2. **Declare which representation is canonical, per layer** (§5), and extend
   the existing scalar-against-raster conformance test so it covers the
   persisted raster too.
3. **Keep SQLite** for identity, the spatial index, annotations, edges and the
   journal. None of them has a raster form.
4. **Do not adopt HDF5.** Revisit OME-Zarr only if a TIFF limitation actually
   shows up.
5. **Spike before committing**, in the style of the 2026-10-10 measurements:
   - compressed size of an anchor-valued label raster at reference density,
     `Int64` against `UInt32`, deflate against zstd;
   - write throughput from parallel tile workers;
   - cold and warm single-pixel lookup cost;
   - the memory held by seam-deferred tiles.

   §3.6 and §3.3 are estimates until then.

## 7. Open

- **Requirement change:** a raster-canonical layer forbids overlap. That needs
  a requirement, not a design note.
- **The seam deferral's memory cost** (§3.3) is unmeasured, and it competes
  with the UR-EXE-2 ceiling exactly where the whole-level-operations note
  already found the budget tight.
- **Disk budget:** still none anywhere. This proposal adds 3–5 GB per
  raster-backed layer to a workspace that measures about 3.9 GB before
  rasters.
