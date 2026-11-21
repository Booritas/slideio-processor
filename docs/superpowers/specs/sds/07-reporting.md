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
    ColumnSchema& assertedMeasurement(const char* stageName, const char* name);
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
A hole inside no outer ring (a degenerate polygon) joins the nearest outer ring
before it in ring order. An empty polygon is `POLYGON EMPTY`.

**Measurements and units** (decision 5, UR-SEG-6). Storage keeps each
measurement in pixels under its base name and, when the scene was calibrated, a
twin under the base name followed by `.um` or `.um2`.

- `measurement(name)` adds base names in call order; a name added twice, one
  ending in `.um` or `.um2`, or one equal to any fixed column name in the table
  above (whether or not that column is included) is `InvalidArgumentError`, so a
  header never holds two columns of one name.
- `allMeasurements()` adds, when a writer starts, every store measurement base
  name, in ascending byte order, after the explicit names and without
  duplicates. A name ending in `.um` or `.um2` is a twin, reached through its
  base; a row twin whose base the store's name list lacks (an importer may write
  `len.um` alone) counts as that base, so the unit policy still gives it its
  column (final review, reporting plan B). In a table (`CsvWriter`) a name equal
  to a fixed column name is skipped; `GeoJsonExporter`, whose measurements are a
  nested object, keeps it (§7.5). Open edge cases (`docs/project.md`,
  reporting plan B): a baseless pair `n.um` + `n.um2` writes only `n.um2`;
  under `PixelsOnly` a baseless twin gives an empty `n` column (CSV) or no key
  (GeoJSON), hiding its value; and CSV skips a baseless twin whose base is a
  fixed column name (`layer.um2` alone). Storage
  lists the names with `ObjectStore::measurementNameCount/At` (SDS 03). That
  list is shared with assertions (reporting plan B, ruling C): a name that only
  later stages assert also appears as a row column, empty in every row; its
  values are in the stage-qualified columns (below). Telling the two apart
  would need a scan of every row measurement per name, since `measurements` has
  no index by name.
- Per base name `n`: `PixelsOnly` writes column `n`. `Calibrated` writes one
  column, named after the twin when the store holds that twin name (so the
  header says which unit it holds) and `n` with the pixel value otherwise. For a
  row column, a twin name that some stage asserts (it is in
  `AnnotationStore::assertedMeasurementColumnCount/At`) is not used: the name
  list is shared, so no row may hold it, and an empty twin column would hide the
  rows' values. `Calibrated` then writes `n` with the rows' pixel values. When
  rows do hold that twin too, `Calibrated` shows pixels rather than µm; no row
  value is lost, and `BothColumns` gives both. Stage-qualified columns are
  unaffected: a stage's own twins resolve per stage (below).
  `BothColumns` writes `n` and, when the store holds a twin name, the twin
  column too.
- A value an object lacks is an empty cell. Values are written as `%.17g` in the
  C locale, so they round-trip. Non-finite values are spelled explicitly, the
  same on every platform: NaN as `nan`, +Inf as `inf`, -Inf as `-inf`, which
  pandas parses.

**Later-stage (asserted) measurements** (reporting plan B, decision 9; user
decision 2026-11-21). A measurement a later stage asserted on an object
(`AnnotationStore`, SDS 03 §3.7) is exported in a stage-qualified column named
`<stage name>/<name>`, the stage name from `StageStateReader`, so two stages
asserting one name give two columns and a row value never hides an asserted one.

- `assertedMeasurement(stageName, name)` adds one stage's base name, in call
  order together with `measurement()`'s. A null or empty stage name or name, a
  name ending in `.um` or `.um2`, or a pair added twice is `InvalidArgumentError`.
  The stage name is looked up per store when a writer starts; a stage the store
  lacks gives a column that is empty in every row.
- `allMeasurements()` adds, after the row columns, every `(stage, base name)`
  pair the store's assertions hold (a twin a stage asserts without its base
  counts as that base, so its value still has a column), in the order of
  `AnnotationStore::assertedMeasurementColumnCount/At` (stage id, then name),
  without duplicates. The listing includes assertions whatever their
  visibility, so the columns do not depend on stage status; the cells do.
- The unit policy applies per stage exactly as it does per row: a stage's `n`
  and `n.um`/`n.um2` assertions are base and twin. `Calibrated` writes
  `<stage>/n.um2` (or `.um`) when that stage asserts the twin, `BothColumns`
  writes both, `PixelsOnly` writes `<stage>/n`.
- Per object, a writer reads the visible asserted measurements once
  (`assertedMeasurementCount`, `assertedMeasurementAt`, `measurementAssertedBy`)
  and serves every asserted column from them. A column the object lacks, or
  whose assertion is not visible (the asserting stage is not `Complete`), is an
  empty cell (CSV) or an absent key (GeoJSON).
- **Name clashes.** A resolved column name equal to another column's — for
  example a row measurement literally named `ki67/positive_fraction` beside
  that stage's assertion — is `InvalidArgumentError` at `write()`, after
  resolution and before any file is created. The message carries no name.

The resolved column list marks each asserted column with its own kind,
carrying the asserting stage id (invalid for a stage the store lacks) and the
store name to read.

`columnCount()` and `columnNameAt()` report the fixed columns and the explicit
names (an asserted one as `<stageName>/<name>`) as `PixelsOnly` would. The full list, with twins and the
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

`ExportResult` is a plain value type with its fields inline (no pimpl): adding a
field later is an ABI change.

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

**Format** (decision 6). RFC 4180: a field or header name holding the
delimiter, `"`, CR, LF or `#` is quoted, with `"` doubled. `#` is quoted because
`pandas.read_csv(comment='#')` cuts a line at an unquoted `#`, so a name such as
`Tumour #2` would otherwise truncate the row. Lines end in `\n`; the file is
UTF-8 without a BOM. The header line comes first, after any provenance lines.
`delimiter(c)` refuses `"`, CR, LF and `#` with `InvalidArgumentError`; the path
may not be null or empty. One writer per path: two writers on the same path
share `<path>.partial` and must not run at once.

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

`slide_id` is the slide path as the store holds it (the workspace records the
path it opened). A path may contain identifiers, such as an accession number, a
medical record number, a patient's name or a user name, that count as PHI or
personal data under HIPAA and GDPR; consumers who share an export must treat it
accordingly. `provenanceHeader(false)` drops every provenance line, and with it
the provenance UR-RES-11 asks for. Redaction is an open follow-up
(`docs/project.md`, reporting plan A).

**Writing, progress, cancellation** (decision 8).

- `write(store, q, observer, token)` resolves the columns, takes
  `total = store.objects().count(q)` and streams `store.objects().query(q)` in
  the cursor's order to `<path>.partial`.
- It calls `observer->onProgress("export", fraction, completed, total)` after
  every 1000 objects and once at the end (not repeated when the end falls on a
  multiple of 1000), when an observer is given. The final call comes before the
  file is finished and renamed, so an observer that throws leaves any file at
  `path` untouched.
- It checks `token.isCancelled()` before writing each object. On cancel the
  partial file is removed, any file already at `path` is untouched, and the
  result has `wasCancelled() == true`, the objects written before the check, and
  `bytesWritten() == 0`. The check comes after the cursor yields an object, so an
  empty result, or a cancel that arrives after the last object, completes
  normally.
- On success the partial file is renamed onto `path`, replacing any file there;
  `bytesWritten()` is the final file's size, counted as the bytes were written,
  so nothing can fail once the finished file is in place.
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

    ExportResult write(Store&, const ObjectQuery&);
    ExportResult write(Store&, const ObjectQuery&,
                       ProgressObserver*, const CancellationToken&);
};
```

**Requirements:** UR-RES-8, UR-RES-11, UR-RES-15, UR-RES-18, UR-SEG-6, UR-QA-3.

Writes through `core::GeoJsonWriter`, so there is one GeoJSON emitter in the
library. The import side lives in `storage` (decision 3); sharing the codec in
`core` is what keeps that split from meaning two parsers.

Objects exported here must open in QuPath with geometry and measurements
intact, and a QuPath-exported annotation file must import and be usable as a
processing ROI — the round trip is the acceptance criterion (UR-RES-8).

Reporting plan B (decision 3) dropped two setters of the original design:

- `coordinatePrecision` — store coordinates are integers and the writer writes
  them exactly, so there is nothing to round.
- `quPathCompatible` — the writer always emits the QuPath shape:
  `objectType: "detection"` and `classification: {"name": …}`.

The exporter is ThreadConfined and uses pimpl. `<slideio/processor/reporting/reporting.hpp>`
includes it.

**Document.** `GeoJsonWriter`'s FeatureCollection: a top-level `provenance`
object (`Store::provenance()`), then one Feature per object, one per line, in
the cursor's order. Each feature's properties are `objectId`, `parentId` (see
below), `objectType: "detection"`, `layer` (the layer name), `stageId` (for an
object with a producing stage), `classification` (the first label written;
absent without labels), `labels` (an array of `{name, workspaceLocal}`) and
`measurements`. A polygon with one outer ring is a `Polygon`, holes as further
rings; several outer rings make a `MultiPolygon`.

**What the schema means for GeoJSON** (plan B decision 4).

- **Default.** With no `schema()` call the schema is
  `ColumnSchema().allMeasurements().units(UnitPolicy::BothColumns)`. A default
  export carries every finite value an object holds and every visible value a
  later stage asserted on it, including names like the CSV fixed columns
  (`layer`, `labels`) and twins whose base the store lacks (`len.um` alone).
  The exceptions: non-finite values (below), and a base name with both a `.um`
  and a `.um2` twin, of which only `.um2` is read (one twin per base, §7.2).
- **Measurements.** The resolved measurement columns (`detail::resolveColumns`,
  §7.2, with layout `MeasurementsOnly`: no fixed columns, and names like them
  kept) decide which values a feature carries, under their column names:
  `area`, its calibrated twin `area.um2`, and a later stage's assertion
  `<stage>/<name>` (§7.2, plan B decision 9). The exporter builds a per-object
  copy of the record holding only those, in column order, each with its source
  unit. A value the object lacks, or an assertion that is not visible, is an
  absent key. Two resolved columns with the same name are an
  `InvalidArgumentError` at `write`, before the file is created.
- **Labels.** `LabelPolicy::EffectiveSet` writes the whole effective set
  (`AnnotationStore` order); `LabelPolicy::ProducerOnly` writes the row label
  alone, or none. The first label written becomes the `classification`.
- **Parent.** `includeParent(true)` writes `parentId`, the parent through a
  visible parent edge (`EdgeStore::parentOf`), right after `objectId`; a root
  has none.
- **Ignored.** `idFormat`, `includeLayer`, `includeStage`, `includeLabel(false)`,
  `includeBoundingBox` and `includeGeometryWkt`. A feature always carries its
  `objectId`, `layer`, geometry and `labels`.

**Non-finite values** (decision 5). JSON has no NaN or infinity, and the
writer refuses them. A non-finite value is left out of that feature's
`measurements`; the rest of the feature is written.

**Bytes and atomicity** (decision 6). The export runs on the driver the CSV
writer uses (§7.3): the file is written to `<path>.partial` through
`GeoJsonWriter`, then `end()`. `bytesWritten()` is the partial file's size,
taken before the rename, so nothing can throw after it; then the file is
renamed onto `path`. Progress, cancellation and failure behave as in
`CsvWriter`: a cancelled or failed export removes the partial file and leaves
any file at `path` untouched. `GeoJsonWriter`'s own `IoError` messages name the
path; the exporter replaces them with messages that carry no path.

**Provenance and PHI** (decision 7). `provenance.slideId` is the slide path as
the store holds it. A path may contain identifiers, such as an accession
number, a medical record number, a patient's name or a user name, that count as
PHI or personal data under HIPAA and GDPR; consumers who share an export must
treat it accordingly. Redaction is the open follow-up in `docs/project.md`
(reporting plan A). Nothing else in the document varies between runs, so two
exports of an unchanged store with the same query and schema are byte-identical
(UR-QA-3).

**Layers and parents on re-import.** The layer and parent id accompany each
feature so a hierarchy can round-trip through interchange rather than survive
only inside the store (UR-RES-15). Today `storage::GeoJsonImporter` imports
every feature into one target layer, with geometry, classification and
measurements; restoring layers by name and parent edges from `parentId` is an
open follow-up, a layered import mode in `storage` (`docs/project.md`,
reporting plan B; user decision 2026-11-21).

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
