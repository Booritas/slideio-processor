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
    PackedString   // single "layer:anchor" column
};

enum class LabelPolicy : int32_t {
    EffectiveSet,   // every label, one column, ';'-separated (default)
    ProducerOnly    // the producing stage's label alone
};

enum class UnitPolicy : int32_t {
    Calibrated,       // physical units where available, pixels otherwise
    PixelsOnly,
    BothColumns       // value_px and value_um side by side
};

class ColumnSchema {
public:
    ColumnSchema();
    static ColumnSchema standard() noexcept;

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

## 7.3 CSV — `reporting/csv_writer.hpp`

```cpp
class ExportResult {
public:
    int64_t objectsWritten() const noexcept;
    int64_t bytesWritten() const noexcept;
    bool    wasCancelled() const noexcept;
};

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

**Requirements:** UR-RES-9, UR-RES-11.

One row per object, driven by the same query mechanism as `UR-RES-2`'s cursors.
The acceptance criterion is that the file loads in pandas with the documented
column schema, so the provenance header uses `#` comment lines that
`pandas.read_csv(comment='#')` skips by default rather than a separate sidecar
file.

## 7.4 Parquet — `reporting/parquet_writer.hpp`

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
