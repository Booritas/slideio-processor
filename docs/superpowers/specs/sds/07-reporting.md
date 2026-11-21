# SDS 07 — `reporting`

**Module:** `reporting` · **Library:** `slideio-processor-data` · **Depends on:** `core`, `config`, `storage`

Export only. Never depends on `pipeline`, `algorithm` or `workspace`, which is
what lets a viewer link it without loading OpenCV (design 5.1).

Conventions in [00-conventions.md](00-conventions.md).

---

## 7.1 Shared rules

Three rules hold for every writer in this module, and they are the reason the
interfaces look alike:

1. **Query-driven.** Every export takes an `ObjectQuery` and streams through a
   `Cursor`. Nothing here materialises a result set — UR-RES-6 permits
   materialisation only as an explicit, filter-bounded call, and exporting ten
   million objects must stay within the memory ceiling (UR-RES-8).
2. **Provenance travels.** Every export carries the source slide identifier,
   scene index, library version and per-stage configuration, recoverable from
   the store alone without the code that produced it (UR-RES-11).
3. **Labels and layers are marked.** Every export names the object's layer and
   marks each label as standard or workspace-local, so a consumer can tell
   which labels are portable between workspaces (UR-RES-15, UR-RES-18). An
   object may carry several labels — the producing stage's and any a later
   stage asserted (`03-storage.md` 3.7) — and every export carries the whole
   effective set, in the fixed order that store defines, so two exports of an
   unchanged store compare equal byte for byte (UR-QA-3).

All four writers are ThreadConfined and report progress and cancellation
through `core`'s mechanisms.

## 7.2 Column schema — `reporting/column_schema.hpp`

```cpp
enum class IdFormat : int32_t {
    TwoColumns,    // layer_id, anchor_id  (default)
    PackedString   // single object_id column, ObjectId::toString() form
};

enum class LabelPolicy : int32_t {
    EffectiveSet,   // every label, ';'-separated, plus a local-flag list (default)
    ProducerOnly    // the producing stage's label alone
};

enum class UnitPolicy : int32_t {
    Calibrated,       // physical units where available, pixels otherwise
    PixelsOnly,
    BothColumns       // pixel column n and, when held, twin n.um / n.um2
};

class ColumnSchema {
public:
    ColumnSchema();
    static ColumnSchema standard();

    ColumnSchema& idFormat(IdFormat);
    ColumnSchema& units(UnitPolicy);
    ColumnSchema& includeLayer(bool);           // default true
    ColumnSchema& includeStage(bool);           // default true
    ColumnSchema& includeLabel(bool);           // default true
    ColumnSchema& labelPolicy(LabelPolicy);     // default EffectiveSet
    ColumnSchema& includeBoundingBox(bool);     // default true
    ColumnSchema& includeGeometryWkt(bool);     // default false
    ColumnSchema& includeParent(bool);          // default false
    ColumnSchema& measurement(const char* name);
    ColumnSchema& allMeasurements();

    int32_t columnCount() const noexcept;
    const char* columnNameAt(int32_t index) const;
    int32_t toHeaderLine(char* buffer, int32_t capacity) const;
};
```

**Requirements:** UR-RES-9, UR-RES-15, UR-SEG-6, decision 8.

`IdFormat::TwoColumns` is the default because object identifiers are composite
— `(layer_id, anchor_id)` — following decision 8. Two integer columns load
cleanly into pandas and sort meaningfully; the packed string is available for
consumers that want one opaque key.

`UnitPolicy::Calibrated` falls back to pixels for a scene without resolution
metadata and marks the column accordingly, so a reader can tell "3.2 µm²" from
"3.2 px² and calibration was unavailable" — the distinction UR-SEG-6 requires
be indicated rather than silently collapsed. `BothColumns` avoids the question
entirely and is the safer choice for published results.

`includeParent(true)` costs one edge lookup per object and is off by default,
since the parent relation is a separate edge record (UR-RES-5) rather than a
column on the object.

**One row per object, even when an object has several labels.** `EffectiveSet`
puts the whole set in a single delimited column rather than emitting one row
per label, because every other column here is per-object: a row-per-label
export would multiply measurements and geometry across rows, break any join on
the object identifier, and silently change what a row count means. The
delimiter costs the consumer a split; the alternative costs them a correct
aggregate. `ProducerOnly` exists for consumers whose schema is fixed at one
class per object, and it is honest about what it drops — the producing stage's
label, which is the only one those tools were ever receiving.

**Column names, exact and in this order** (reporting plan A, decision 4). Every
column is optional except the identifier.

| Setting | Columns | Cell |
|---|---|---|
| `IdFormat::TwoColumns` | `layer_id`, `anchor_id` | decimal `LayerId` value; decimal `ObjectId::anchorPacked()` |
| `IdFormat::PackedString` | `object_id` | `ObjectId::toString()` |
| `includeLayer` | `layer` | the layer name |
| `includeStage` | `stage` | the stage name; empty for an object with no stage (imported) |
| `includeLabel`, `EffectiveSet` | `labels`, `labels_local` | the effective set (SDS 03 §3.7 order) joined with `;`; the parallel list of `1` (not a standard label) or `0` |
| `includeLabel`, `ProducerOnly` | `label`, `label_local` | the row label alone, and its flag |
| `includeBoundingBox` | `bbox_x`, `bbox_y`, `bbox_width`, `bbox_height` | `ObjectRecord::boundingBox()`, level 0, half-open |
| `includeGeometryWkt` | `geometry_wkt` | WKT in level-0 integer coordinates (below) |
| `includeParent`, TwoColumns | `parent_layer_id`, `parent_anchor_id` | from `EdgeStore::parentOf`; empty without a parent |
| `includeParent`, Packed | `parent_id` | the same, as `ObjectId::toString()` |

Inside a label name `\` is written `\\` and `;` is written `\;`, so a name
holding the separator splits back unambiguously. A label is flagged `1` when its
`VocabularyRegistry::origin()` is not `Standard` (workspace-local or registered
by an import): such a label does not carry over to another workspace. An object
with no label has empty label cells.

The WKT is `POLYGON((outer),(hole),…)` for one outer ring and
`MULTIPOLYGON(((outer),(hole),…),…)` for several; rings are closed by
repeating the first point. Each hole joins the first outer ring, in ring order,
that contains the hole's first vertex by an even-odd test on that ring alone.

**Measurements and units** (decision 5, UR-SEG-6). Storage keeps each
measurement in pixels under its base name and, when the scene was calibrated, a
twin under the base name followed by `.um` or `.um2`.

- `measurement(name)` adds base names in call order; a name added twice, or one
  ending in `.um` or `.um2`, is `InvalidArgumentError`.
- `allMeasurements()` adds, when a writer starts, every store measurement name
  that does not end in `.um` or `.um2`, in ascending byte order, after the
  explicit names and without duplicates. Storage lists the names with
  `ObjectStore::measurementNameCount/At` (SDS 03).
- Per base name `n`: `PixelsOnly` writes column `n`. `Calibrated` writes one
  column, named after the twin when the store holds that twin name (so the
  header says which unit it holds) and `n` with the pixel value otherwise.
  `BothColumns` writes `n` and, when the store holds a twin name, the twin
  column too.
- A value an object lacks is an empty cell. Values are written as `%.17g` in the
  C locale, so they round-trip.
- Cells come from the object row's own measurements only. Values asserted by
  later stages (`AnnotationStore::assertedMeasurementCount/At`, SDS 03 §3.7) are
  not exported yet (open follow-up of reporting plan A). Asserted names share the
  store's `measurement_names` list, so `allMeasurements()` can produce a column
  that is empty in every row when a name was only ever asserted.

`columnCount()` and `columnNameAt()` report the fixed columns and the explicit
names as `PixelsOnly` would. The full list, with twins and the
`allMeasurements()` expansion, is resolved per store by the internal
`detail::resolveColumns(const ColumnSchema&, Store&)` (`src/reporting/detail/columns.hpp`;
it takes a non-const `Store` because `Store::objects()` is non-const), which
every writer uses.

## 7.3 CSV — `reporting/csv_writer.hpp`

`ExportResult` lives in its own header, `reporting/export_result.hpp`, because
every writer returns it (reporting plan A, decision 2):

```cpp
class ExportResult {
public:
    ExportResult(int64_t objectsWritten, int64_t bytesWritten,
                 bool wasCancelled) noexcept;
    int64_t objectsWritten() const noexcept;
    int64_t bytesWritten() const noexcept;
    bool    wasCancelled() const noexcept;
};
```

```cpp
class CsvWriter {
public:
    explicit CsvWriter(const char* path);
    ~CsvWriter();

    CsvWriter& schema(const ColumnSchema&);
    CsvWriter& delimiter(char);                      // default ','
    CsvWriter& provenanceHeader(bool);               // default true, '#' lines

    ExportResult write(Store&, const ObjectQuery&);
    ExportResult write(Store&, const ObjectQuery&,
                       ProgressObserver*, const CancellationToken&);
};
```

**Requirements:** UR-RES-9, UR-RES-11, UR-RES-15, UR-RES-18, UR-SEG-6, UR-QA-3.

One row per object, driven by the same query mechanism as `UR-RES-2`'s cursors.
The acceptance criterion is that the file loads in pandas with the documented
column schema, so the provenance header uses `#` comment lines that
`pandas.read_csv(comment='#')` skips by default rather than a separate sidecar
file.

**Format** (decision 6). RFC 4180: a field holding the delimiter, `"`, CR or LF
is quoted, with `"` doubled. Lines end in `\n`; the file is UTF-8 without a BOM.
The header line comes first, after any provenance lines. `delimiter(c)` refuses
`"`, CR, LF and `#` with `InvalidArgumentError`; the path may not be null or
empty.

**Provenance** (decision 7, `provenanceHeader(true)`, the default):

```
# slideio-processor export
# slide_id: <slideId>
# scene_index: <n>
# library_version: <v>
# created_at_unix_ms: <n>
# stage: <name> <configuration>
```

The values come from `Store::provenance()`. There is one `stage` line per stage,
in `StageStateReader` order, with the stored configuration JSON; a stage with an
empty configuration has no trailing space. CR and LF in any value are written as
the two characters `\r` and `\n`, so each stays on one line. Nothing else is
written — no export time, random value or address — so two exports of an
unchanged store with the same query and schema are byte-identical (UR-QA-3).

**Writing, progress, cancellation** (decision 8).

- `write(store, q, observer, token)` resolves the columns, takes
  `total = store.objects().count(q)` and streams `store.objects().query(q)` in
  the cursor's order to `<path>.partial`.
- It calls `observer->onProgress("export", fraction, completed, total)` after
  every 1000 objects and once at the end (not repeated when the end falls on a
  multiple of 1000), when an observer is given.
- It checks `token.isCancelled()` before writing each object. On cancel the
  partial file is removed, any file already at `path` is untouched, and the
  result has `wasCancelled() == true`, the objects written before the check, and
  `bytesWritten() == 0`.
- On success the partial file is renamed onto `path`, replacing any file there;
  `bytesWritten()` is the final file's size.
- On any exception the partial file is removed and the exception propagates;
  failures of the file itself are `IoError`, whose messages carry no path.
- The two-argument `write` calls the four-argument one with no observer and a
  token never cancelled. `CsvWriter` is ThreadConfined.

## 7.4 Parquet — `reporting/parquet_writer.hpp`

> **Deferred** (user decision 2026-11-21): reporting plan A does not implement
> the Parquet writer; it waits until its dependency is chosen. UR-RES-10 is not
> met until then. The interface below stands as the design.

```cpp
enum class ParquetCompression : int32_t { None, Snappy, Zstd };

class ParquetWriter {
public:
    explicit ParquetWriter(const char* path);
    ~ParquetWriter();

    ParquetWriter& schema(const ColumnSchema&);
    ParquetWriter& compression(ParquetCompression);  // default Zstd
    ParquetWriter& rowGroupSize(int64_t);            // default 100000

    ExportResult write(Store&, const ObjectQuery&);
    ExportResult write(Store&, const ObjectQuery&,
                       ProgressObserver*, const CancellationToken&);
};
```

**Requirements:** UR-RES-10.

The binary columnar format for result sets impractical as CSV — ten million
objects with full measurements is the case UR-RES-10 exists for. Provenance
goes into the Parquet file's key-value metadata rather than comment lines.

Row groups are written as they fill, so peak memory is bounded by
`rowGroupSize` rather than by the result set.

## 7.5 GeoJSON — `reporting/geojson_exporter.hpp`

```cpp
class GeoJsonExporter {
public:
    explicit GeoJsonExporter(const char* path);
    ~GeoJsonExporter();

    GeoJsonExporter& schema(const ColumnSchema&);
    GeoJsonExporter& quPathCompatible(bool);         // default true
    GeoJsonExporter& coordinatePrecision(int32_t);   // decimal places, default 1

    ExportResult write(Store&, const ObjectQuery&);
    ExportResult write(Store&, const ObjectQuery&,
                       ProgressObserver*, const CancellationToken&);
};
```

**Requirements:** UR-RES-8, UR-RES-15, UR-RES-18.

Writes through `core::GeoJsonWriter`, so there is one GeoJSON emitter in the
library. The import side lives in `storage` (decision 3); sharing the codec in
`core` is what keeps that split from meaning two parsers.

Objects exported here must open in QuPath with geometry and measurements
intact, and a QuPath-exported annotation file must import and be usable as a
processing ROI — the round trip is the acceptance criterion (UR-RES-8).

`quPathCompatible(true)` emits the `classification` property shape QuPath
expects. The layer accompanies each feature so a hierarchy round-trips through
interchange rather than surviving only inside the store: objects exported and
re-imported land in the same layers with parent relations intact (UR-RES-15).

Export streams. Ten million objects stay within the memory ceiling.

## 7.6 Mask rasters — `reporting/mask_writer.hpp`

```cpp
enum class MaskCompression : int32_t { None, Lzw, Deflate, Jpeg };

class MaskWriter {
public:
    explicit MaskWriter(const char* path);
    ~MaskWriter();

    MaskWriter& tileSize(int32_t width, int32_t height);
    MaskWriter& compression(MaskCompression);
    MaskWriter& pyramidLevels(int32_t);              // 0 = auto
    MaskWriter& resolution(double micronsPerPixel);

    /// Writes any scene as a pyramidal TIFF, including a lazy pipeline scene.
    ExportResult write(slideio::ScenePtr,
                       ProgressObserver*, const CancellationToken&);
};

/// Rasterises the objects of a query into a label mask scene, readable at any
/// level. Lazy: no pixels are computed until read.
slideio::ScenePtr rasteriseLabels(Store&, const ObjectQuery&,
                                  const Rect64& region,
                                  int32_t level);
```

**Requirements:** UR-RES-7.

UR-RES-7 offers two forms and this module provides both: `rasteriseLabels`
returns a scene readable at any level, and `MaskWriter` persists any scene —
including a lazy `pipeline` scene — as a pyramidal TIFF via slideio-converter.

`write` takes a `slideio::ScenePtr` rather than a `Pipeline`, which is what
keeps this module free of a `pipeline` dependency and therefore keeps it on the
OpenCV-free side of the library split.

The acceptance criterion is that a written mask reopens in SlideIO with the
expected levels and dimensions.

**A label mask encodes one label per pixel, and the query chooses which.**
Where the query carries a `label()` filter, the raster encodes membership of
that label; where it does not, it encodes the producing stage's row label, and
objects that have none read as unlabelled. There is no blending rule and no
priority order, because any would be arbitrary once an object can be both
`tumour` and `Ki67` (`03-storage.md` 3.7). A caller wanting both asks twice —
the scenes are lazy, so an unread second mask costs nothing but the call.

## 7.7 Bounded materialisation — `reporting/materialise.hpp`

```cpp
class ObjectArray {
public:
    ~ObjectArray();
    int64_t count() const noexcept;
    const ObjectRecord& at(int64_t index) const;
};
using ObjectArrayPtr = std::shared_ptr<ObjectArray>;

/// Reads a filtered query into memory. Throws InvalidArgumentError if the
/// query is unfiltered, and if the result would exceed maxObjects.
ObjectArrayPtr materialise(Store&, const ObjectQuery&, int64_t maxObjects);
```

**Requirements:** UR-RES-6.

The only call in the library that brings objects into memory as a set, and it
is deliberately awkward: it demands a filter, demands an explicit cap, and
throws rather than truncating.

UR-RES-6's acceptance criterion is that *the API offers no call that
materialises an unfiltered stage* — so `ObjectQuery::isUnfiltered()` is checked
before anything is read, and there is no overload without `maxObjects`.

The refusals (reporting plan A, decision 9) are all `InvalidArgumentError`,
thrown before any record is read: `maxObjects < 1`, `q.isUnfiltered()`, and
`store.objects().count(q) > maxObjects`. Because the count and the cursor are
separate reads, the cap is enforced again while reading. Otherwise every record
is read into the `ObjectArray`, in the cursor's order; `ObjectArray::at` throws
`InvalidArgumentError` out of range. `ObjectArray` and `ObjectArrayPtr` live in
`reporting/materialise.hpp`.

It lives in `reporting` rather than `storage` because it is a convenience for
consumers, not a storage capability, and keeping it out of `storage` means no
internal code path can reach for it by accident.

## 7.8 What is deliberately absent

| Not here | Where instead | Why |
|---|---|---|
| GeoJSON import | `storage::GeoJsonImporter` | An input, not a report; needs the vocabulary registry for UR-RES-19's policy (decision 3) |
| ROI import | `storage::importRoiPolygon` | Feeds processing, not reporting (UR-IMG-5) |
| Writing WSI formats | `slideio-converter` | Explicit non-goal in §15 |
| Annotation editing | — | Explicit non-goal in §15; the library imports and exports, it does not edit |
| Report templates, PDF, HTML | — | Not a requirement. UR-RES-9 and UR-RES-10 ask for tables a consumer analyses, not formatted documents |
