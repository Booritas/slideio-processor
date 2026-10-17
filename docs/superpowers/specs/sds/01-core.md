# SDS 01 — `core`

**Module:** `core` · **Library:** `slideio-processor-data` · **Depends on:** SlideIO types, std

Shared type vocabulary and the interfaces every other module talks through.
Holds no state and performs no I/O beyond the GeoJSON codec.

Conventions in [00-conventions.md](00-conventions.md).

---

## 1.1 Errors — `core/error.hpp`

```cpp
enum class ErrorCode : int32_t {
    InvalidArgument, UnsupportedDataType, ChannelArity, StoreFormat,
    Stage, Tile, Cancelled, Io, Internal
};

class ProcessorError : public std::exception {
public:
    ~ProcessorError() override;
    const char* what() const noexcept override;
    ErrorCode   code() const noexcept;

    const char* stageName()     const noexcept;  // "" if not applicable
    const char* operationName() const noexcept;  // "" if not applicable
    const char* cause()         const noexcept;  // "" if none
    bool        hasTile()       const noexcept;
    int64_t     tileX()         const noexcept;  // valid iff hasTile()
    int64_t     tileY()         const noexcept;
protected:
    ProcessorError(ErrorCode, const char* message);
};

class InvalidArgumentError    : public ProcessorError { /* ... */ };
class UnsupportedDataTypeError: public ProcessorError {
public:
    DataType offendingType() const noexcept;
    DataTypeSet supportedTypes() const noexcept;
};
class ChannelArityError       : public ProcessorError {
public:
    int32_t offendingChannelCount() const noexcept;
};
class StoreFormatError        : public ProcessorError {
public:
    int32_t storeVersion() const noexcept;
    int32_t libraryVersion() const noexcept;
};
class StageError              : public ProcessorError { /* ... */ };
class TileError               : public ProcessorError { /* ... */ };
class CancelledError          : public ProcessorError { /* ... */ };
class IoError                 : public ProcessorError { /* ... */ };
```

**Thread-safety:** Immutable. **Requirements:** UR-API-5, UR-EXE-5,
UR-IMG-11, UR-RES-12.

The hierarchy is closed — UR-API-5 requires that no exception escapes it, so
the public boundary of every module catches and translates. `UnsupportedDataTypeError`
carries the supported set so the message can name both the type and the
conversion needed, as UR-IMG-11 requires.

## 1.2 Data types — `core/data_type.hpp`

```cpp
enum class DataType : int32_t {
    Byte, Int8, UInt16, Int16, UInt32, Int32,
    UInt64, Int64, Float16, Float32, Float64, Unknown
};

const char* dataTypeName(DataType) noexcept;
int32_t     dataTypeSizeBytes(DataType) noexcept;
bool        dataTypeIsFloat(DataType) noexcept;
bool        dataTypeIsSigned(DataType) noexcept;
DataType    dataTypeFromSlideio(int32_t slideioDataType) noexcept;
int32_t     dataTypeToSlideio(DataType) noexcept;

class DataTypeSet {                       // POD-like value, 4 bytes
public:
    DataTypeSet() noexcept;               // empty
    static DataTypeSet all() noexcept;
    static DataTypeSet integerTypes() noexcept;
    static DataTypeSet floatTypes() noexcept;

    DataTypeSet& add(DataType) noexcept;
    DataTypeSet& remove(DataType) noexcept;
    bool  contains(DataType) const noexcept;
    bool  isEmpty() const noexcept;
    int32_t count() const noexcept;
    DataType at(int32_t index) const;     // throws InvalidArgumentError
    uint32_t bits() const noexcept;

    DataTypeSet intersect(DataTypeSet) const noexcept;
    bool operator==(DataTypeSet) const noexcept;
};
```

**Thread-safety:** Immutable. **Requirements:** UR-IMG-8, UR-IMG-11, UR-IMG-12.

All eleven SlideIO types are present regardless of what the backend supports.
Backend capability is reported separately by `config::BackendCapabilities`, so
the OpenCV 4→5 migration widens declarations without touching this enum
(UR-BLD-4).

## 1.3 Geometry — `core/geometry.hpp`

```cpp
struct Point64 {                          // POD, exempt from pimpl per C-2
    int64_t x;
    int64_t y;
};

struct Rect64 {                           // POD
    int64_t x, y, width, height;

    int64_t right() const noexcept;       // x + width
    int64_t bottom() const noexcept;
    int64_t area() const noexcept;        // 64-bit: UR-IMG-2
    bool    isEmpty() const noexcept;
    bool    contains(Point64) const noexcept;
    bool    intersects(const Rect64&) const noexcept;
    Rect64  intersected(const Rect64&) const noexcept;
    Rect64  united(const Rect64&) const noexcept;
};

class Polygon {
public:
    Polygon();
    Polygon(const Polygon&);
    Polygon(Polygon&&) noexcept;
    Polygon& operator=(const Polygon&);
    Polygon& operator=(Polygon&&) noexcept;
    ~Polygon();

    int32_t ringCount() const noexcept;
    bool    ringIsHole(int32_t ring) const;
    int64_t ringPointCount(int32_t ring) const;
    int64_t ringPoints(int32_t ring, Point64* buffer, int64_t capacity) const;

    void addRing(const Point64* points, int64_t count, bool isHole);
    void clear() noexcept;
    bool isEmpty() const noexcept;

    Rect64  boundingBox() const noexcept;
    Point64 anchor() const noexcept;      // topmost-then-leftmost vertex
    double  area() const noexcept;        // outer rings minus holes
    double  perimeter() const noexcept;
    Point64 centroid() const noexcept;
    bool    contains(Point64) const noexcept;
};
```

**Thread-safety:** ThreadCompatible. **Requirements:** UR-IMG-2, UR-SEG-4,
UR-SEG-5.

`anchor()` is load-bearing: decision 8 derives every object identifier from it,
so its definition — the vertex with the smallest `y`, ties broken by smallest
`x` — is part of the store format's stability contract, not an implementation
detail. Changing it would renumber every object in every existing store.

**Rings and membership.** Ring 0 is *an* outer ring; `ringIsHole(0)` is always
`false`. A polygon may hold **several outer rings** — a tissue section that
meets the slide edge in two places is one region with two disjoint outers — and
`area()`'s "outer rings minus holes" is plural for that reason (UR-SEG-4).

Holes are **not** associated with a particular outer ring, and no code may
infer such an association. Membership is the **even-odd rule**: a point is
inside when a ray from it crosses an odd number of ring edges, whatever order
the rings are in and whichever outer a hole happens to sit inside.
`contains()`, `area()` and the rasteriser of 1.17 all use it, and a conformance
test asserts the scalar and the raster answer agree on the same polygon.

Even-odd is chosen over a containment-derived association because the
association would have to be *computed* — a point-in-polygon pass quadratic in
the ring count, over input whose ring count nothing bounds — to answer a
question even-odd answers without it. On well-formed input, outer rings
disjoint and each hole inside exactly one of them, the two agree everywhere.

**Every `Polygon` that crosses a public boundary is in level-0 slide
coordinates**, whatever pyramid level produced it (UR-SEG-4). Integers, not
because boundaries are known to the pixel, but because identity
(`ObjectId::fromAnchor`) and seam merging both rest on exact vertex equality —
a tolerance-based comparison would make merge results depend on order and break
UR-QA-3 the way float reductions do (`05-pipeline.md` 5.8). Coordinates
therefore look exact at every level; how exact they *are* is a property of the
layer, reported by `LayerRegistry::coarsestAnalysisLevel` (`03-storage.md`
3.3).

**There is deliberately no `dilate(Polygon, double)`.** An earlier draft
carried one, for the single purpose of making a coarse layer's polygon safe as
a processing mask (`04-workspace.md` 4.4). That mask is now a raster (1.17) and
is widened by `dilateMask` instead, which is separable, exact on integers, and
bounded by the tile rather than by the ring count. Nothing else ever called
polygon dilation, and a shape-changing operation on the permanent ABI with no
caller is a liability rather than a convenience. `core` therefore contains no
geometric operation that changes a shape.

## 1.4 Identity — `core/ids.hpp`

```cpp
class LayerId {
public:
    LayerId() noexcept;                   // invalid
    explicit LayerId(int32_t value) noexcept;
    int32_t value() const noexcept;
    bool    isValid() const noexcept;
    bool operator==(LayerId) const noexcept;
    bool operator!=(LayerId) const noexcept;
    bool operator<(LayerId) const noexcept;
};

class StageId  { /* identical shape over int32_t */ };
class LabelId  { /* identical shape over int32_t */ };

/// Names a persisted raster, which is **not** a layer: it holds pixels, has
/// no anchors, no parents and no labels, and participates in no containment
/// hierarchy (`03-storage.md` 3.8). Kept a distinct type so that every
/// function taking a `LayerId` may go on assuming it names a collection of
/// objects.
class RasterId { /* identical shape over int32_t */ };

class ObjectId {
public:
    ObjectId() noexcept;                  // invalid
    static ObjectId fromAnchor(LayerId, Point64 anchor) noexcept;

    LayerId layer() const noexcept;
    int64_t anchorPacked() const noexcept;   // (y << 31) | x
    Point64 anchorPoint() const noexcept;
    bool    isValid() const noexcept;

    bool operator==(const ObjectId&) const noexcept;
    bool operator!=(const ObjectId&) const noexcept;
    bool operator<(const ObjectId&) const noexcept;   // layer, then anchor

    int32_t toString(char* buffer, int32_t capacity) const;
    static bool fromString(const char* text, ObjectId& out) noexcept;
};
```

**Thread-safety:** Immutable. **Requirements:** UR-RES-4, UR-QA-3, decision 8.

`ObjectId` is composite because anchors are unique only within a layer — a cell
and its nucleus can share a topmost-leftmost pixel, and one layer may be filled
by two stages. Ordering by `operator<` is layer, then anchor scan position,
which makes sorted iteration spatially ordered for free.

`fromAnchor` is the only construction path from geometry. The library never
allocates an identifier from a counter, which is what makes ids invariant to
worker count and tile size (UR-QA-3).

## 1.5 Measurements — `core/measurement.hpp`

```cpp
enum class MeasurementUnit : int32_t {
    Pixels, Micrometres, SquarePixels, SquareMicrometres,
    Intensity, Dimensionless
};

class Measurement {
public:
    const char*     name() const noexcept;    // valid while owner lives
    double          value() const noexcept;
    MeasurementUnit unit() const noexcept;
    bool            isCalibrated() const noexcept;
};

class MeasurementSet {
public:
    MeasurementSet();
    MeasurementSet(const MeasurementSet&);
    ~MeasurementSet();

    int32_t     count() const noexcept;
    Measurement at(int32_t index) const;
    bool        find(const char* name, Measurement& out) const noexcept;

    void set(const char* name, double value, MeasurementUnit);
    void remove(const char* name) noexcept;
    void clear() noexcept;
};
```

**Thread-safety:** ThreadCompatible. **Requirements:** UR-SEG-5, UR-SEG-6.

`isCalibrated()` reports `false` when the scene lacked resolution metadata, so
a consumer can tell "3.2 µm²" from "3.2 px² and no calibration was available"
— the distinction UR-SEG-6 requires be documented rather than silently
collapsed.

## 1.6 Object record — `core/object_record.hpp`

```cpp
class ObjectRecord {
public:
    ObjectRecord();
    ObjectRecord(const ObjectRecord&);
    ObjectRecord(ObjectRecord&&) noexcept;
    ~ObjectRecord();

    ObjectId id() const noexcept;         // derived from layer + geometry anchor
    LayerId  layer() const noexcept;

    /// The stage that produced this object. Read-only: it is stamped by the
    /// `ObjectWriter` on emission, never set by the producer. Invalid on a
    /// record being built for emission; valid on any record read back.
    StageId  stage() const noexcept;

    const Polygon& geometry() const noexcept;
    Rect64         boundingBox() const noexcept;

    bool    hasLabel() const noexcept;
    LabelId label() const noexcept;       // invalid if !hasLabel()

    const MeasurementSet& measurements() const noexcept;
    MeasurementSet&       measurements() noexcept;

    void setGeometry(const Polygon&);
    void setLayer(LayerId);
    /// The producing stage's own classification of this object. Additional
    /// labels asserted later by other stages are not here; they live in the
    /// annotation store (`03-storage.md` 3.7) and reach a consumer through
    /// the effective label set.
    void setLabel(LabelId);
    void clearLabel() noexcept;
    void reset() noexcept;
};
```

**Thread-safety:** ThreadCompatible. **Requirements:** UR-RES-4, UR-RES-13,
UR-STG-4, UR-STG-6.

`id()` is derived, not stored: it is `ObjectId::fromAnchor(layer(),
geometry().anchor())`. Setting geometry or layer changes the identifier, which
is correct — the identity *is* the position, and a re-run producing the same
geometry produces the same id (UR-STG-6).

**The label on the record is the producing stage's, and only its.** A stage
that detects and classifies in one pass sets it directly, which is both the
common case and correctly attributed, because the producing stage is the
asserting stage. A *later* stage that classifies objects it did not produce
cannot reach this field at all — it asserts an annotation instead
(`03-storage.md` 3.7), and `hasLabel()` on a record read back still answers for
the producer alone. Queries and exports work from the effective set, which is
this label together with the asserted ones; a reader holding a bare
`ObjectRecord` is holding one stage's view, not the whole of what is known
about the object.

**Stage is stamped, not set.** There is no `setStage`, and the field is not
part of the emission contract: `ObjectWriter` attributes every object to the
stage it was opened for (`03-storage.md` 3.5), so a producer cannot claim
another stage's provenance, whether by mistake or by design. An earlier draft
exposed `setStage` alongside the writer's own attribution, which left two
sources of truth for the one field that `discardStage`, re-run replacement
(UR-STG-6) and the provenance record (UR-STG-4) all depend on — and no stated
rule for which won.

Layer is the deliberate asymmetry. It stays writable because one writer may
legitimately emit into several layers in a pass, so the record is the only
place that information can travel (4.2). Stage cannot vary that way: a writer
belongs to exactly one.

Consequently `stage()` reads invalid while an operation builds a record, and
valid on every record obtained from a `Cursor` or `ObjectStore::get`. Code that
round-trips a record — reading one, modifying it, emitting it again — gets the
emitting stage's id on the new row, not the original's, which is what
re-running a stage over another's objects must do.

There is deliberately no parent accessor. UR-RES-4 requires the parent relation
be a separate edge record so an object can be attached after it was written
without rewriting it; putting it here would make that impossible to honour.

## 1.7 Channel selection — `core/channel_selection.hpp`

```cpp
class ChannelSelection {
public:
    ChannelSelection();                   // empty
    static ChannelSelection all() noexcept;
    static ChannelSelection single(int32_t channel);
    static ChannelSelection range(int32_t first, int32_t count);

    ChannelSelection& add(int32_t channel);
    int32_t count() const noexcept;       // 0 and isAll() means "all"
    int32_t at(int32_t index) const;
    bool    isAll() const noexcept;
    bool    contains(int32_t channel) const noexcept;
};
```

**Thread-safety:** ThreadCompatible. **Requirements:** UR-IMG-6, UR-IMG-7,
UR-ALG-2.

Selection is explicit and ordered. Nothing in the library infers a channel's
meaning from its index; an operation that needs RGB asks the scene's metadata
and raises a documented error if it is absent (UR-IMG-7).

## 1.8 Raster view — `core/block_view.hpp`

```cpp
struct BlockPlane {                       // POD, exempt from pimpl per C-2
    void*    data;                        // first byte of row 0
    DataType type;
    int64_t  rowStrideBytes;              // >= width * dataTypeSizeBytes(type)
    int32_t  sourceChannel;               // the scene channel this plane holds
};

class BlockView {
public:
    BlockView() noexcept;                 // null

    /// A view over memory the caller owns. Nothing is copied and nothing is
    /// freed: every `data` pointer must outlive the view (C-8). The `planes`
    /// array itself is read during the call only.
    ///
    /// `width` and `height` are level pixels; `slideRect` is the same block in
    /// level-0 coordinates. Throws InvalidArgumentError naming the offending
    /// field — see below for the conditions.
    BlockView(int64_t width, int64_t height,
              Rect64 slideRect, int32_t level,
              const BlockPlane* planes, int32_t planeCount);

    BlockView(const BlockView&);
    BlockView(BlockView&&) noexcept;
    BlockView& operator=(const BlockView&);
    BlockView& operator=(BlockView&&) noexcept;
    ~BlockView();

    bool    isNull() const noexcept;
    int64_t width() const noexcept;
    int64_t height() const noexcept;
    Rect64  slideRect() const noexcept;   // position in level-0 coordinates
    int32_t level() const noexcept;

    int32_t  planeCount() const noexcept;
    void*    planeData(int32_t plane) const;
    DataType planeType(int32_t plane) const;
    int64_t  planeRowStrideBytes(int32_t plane) const;
    int32_t  planeSourceChannel(int32_t plane) const;
};
```

**Thread-safety:** ThreadCompatible; the underlying buffer's safety is the
owner's contract. **Requirements:** UR-API-3, UR-IMG-9, UR-IMG-10.

Planar rather than interleaved, one plane per selected channel, each with its
own `DataType`. That is what lets a scene mixing `Byte` and `Float32` channels
be read and processed without special-casing (UR-IMG-9) — an interleaved layout
would have forced a common type and broken UR-IMG-10's losslessness guarantee.

Each plane is contiguous with an explicit stride, which is exactly what NumPy
needs for a zero-copy view (UR-API-3).

**A caller can construct one, and must be able to.** `Pipeline::readBlock`
(`05-pipeline.md` 5.5) is public and fills a `BlockView&` the caller supplies,
so a view that only the framework could build would leave that method
uncallable from outside the library — the same shape of defect the SlideIO
spike found in `slideio::Scene`, where every construction seam was withheld.
The constructor is therefore part of the public contract rather than an
internal convenience, and a binding generator exposes it so Python can read a
block into a buffer it already owns.

It is **non-owning in both directions**: the view frees nothing, and it keeps
no reference to the `planes` array, only to the pointers inside it. The caller
guarantees every `data` pointer outlives the view (C-8). Copying a `BlockView`
copies pointers, not pixels, which is why the copy members are declared rather
than left implicit — a reader should not have to infer whether copying a thing
called a *view* duplicates a gigabyte.

Planes are passed as a count-plus-pointer array rather than one call each
because C-5 requires that form for bulk data, and because a half-built view
with two of its three planes set is a state no caller should be able to
observe. The struct is a POD so that assembling one costs a stack variable.

`BlockPlane::sourceChannel` is supplied rather than inferred from the plane's
index: the selection is explicit and ordered (1.7), and a view over a subset —
channels 0 and 3 of a five-channel scene — must still report which channel each
plane holds.

The constructor throws `InvalidArgumentError` naming the field when:

| Condition |
|---|
| `width` or `height` is ≤ 0 |
| `level` is negative |
| `slideRect` is empty |
| `planes` is null, or `planeCount` is < 1 |
| any plane's `data` is null |
| any plane's `type` is `DataType::Unknown` |
| any plane's `rowStrideBytes` is < `width * dataTypeSizeBytes(type)` |
| any plane's `sourceChannel` is negative |

Validation is at construction for the reason it is everywhere else in this
library: a malformed view discovered at the first pixel read fails inside a
worker thread, mid-tile, with a stage already running.

What the constructor **cannot** check is that each buffer is really
`height * rowStrideBytes` bytes long. No argument carries that, and adding one
would make the common case — a view over a buffer the caller just allocated to
exactly that size — carry a value it has no way to get wrong. Debug builds
assert the last row is addressable where the platform allows it; release
builds trust the caller.

`width()` and `height()` are the buffer's extent in **level** pixels, while
`slideRect()` gives its position and size in **level-0** coordinates — so the
two disagree by the level's scale factor whenever `level() > 0`, and that is
not a defect. The buffer is what the operation indexes; the slide rect is where
it sits. `05-pipeline.md` 5.1 names the same two spaces on `OperationContext`
and supplies the conversion.

## 1.9 Flow interfaces — `core/flow.hpp`

```cpp
class ObjectSink {
public:
    virtual ~ObjectSink();
    virtual void emit(const ObjectRecord&) = 0;
    virtual void flush() = 0;
    virtual int64_t emittedCount() const = 0;
};

class EdgeSink {
public:
    virtual ~EdgeSink();
    virtual void assertParent(ObjectId child, ObjectId parent) = 0;
    virtual void assertDerivedFrom(ObjectId derived, ObjectId primitive) = 0;
    virtual void flush() = 0;
    virtual int64_t assertedCount() const = 0;
};

class RasterSink {
public:
    virtual ~RasterSink();

    /// Writes one tile of the run's final raster output, in the geometry the
    /// view carries. Called once per tile, concurrently from workers:
    /// implementations must be ThreadSafe.
    virtual void writeTile(const BlockView&) = 0;

    virtual void flush() = 0;
    virtual int64_t tilesWritten() const = 0;
};

class AnnotationSink {
public:
    virtual ~AnnotationSink();

    /// Attaches a label to an object that already exists, without modifying
    /// it. Additive: an object may carry any number of labels from any number
    /// of stages (`03-storage.md` 3.7).
    virtual void assertLabel(ObjectId, LabelId) = 0;

    /// Attaches a named measurement to an object that already exists. One
    /// stage may assert a given name on a given object once.
    virtual void assertMeasurement(ObjectId, const char* name, double value,
                                   MeasurementUnit) = 0;

    virtual void flush() = 0;
    virtual int64_t assertedCount() const = 0;
};

class Cursor {
public:
    virtual ~Cursor();
    virtual bool next() = 0;
    virtual const ObjectRecord& current() const = 0;
    virtual int64_t position() const = 0;
    virtual void close() = 0;
};
using CursorPtr = std::shared_ptr<Cursor>;

class ObjectSource {
public:
    virtual ~ObjectSource();
    virtual CursorPtr query(const ObjectQuery&) = 0;
    virtual int64_t   count(const ObjectQuery&) const = 0;
    virtual bool      get(ObjectId, ObjectRecord& out) const = 0;
};

class TileMaskSource {
public:
    virtual ~TileMaskSource();

    /// Fills `out` over the region, level and scale it already carries
    /// (1.17). Called concurrently from worker threads, once per tile:
    /// implementations must be ThreadSafe.
    virtual void fillMask(RasterMask& out) = 0;
};
using TileMaskSourcePtr = std::shared_ptr<TileMaskSource>;

/// A mask source backed by the objects of one layer. For each tile it queries
/// `source` for the objects of `layer` intersecting that tile and rasterises
/// them, then widens the result by `dilationLevel0Pixels` (1.17). The query
/// carries `StageVisibility::CompletedOnly`, so a restriction never sees a
/// concurrently running stage's partial output (UR-STG-5).
///
/// `source` is borrowed and must outlive the returned object (C-8). The
/// result is ThreadSafe when `source` is, which `storage`'s implementation is.
TileMaskSourcePtr makeLayerMaskSource(ObjectSource& source, LayerId layer,
                                      double dilationLevel0Pixels);

enum class LayerRasterContent : int32_t {
    Label,        // the object's effective label id; 0 where no object covers
    Membership,   // 1 inside any object of the layer, 0 outside
    ObjectAnchor  // ObjectId::anchorPacked() of the covering object; 0 where none
};

class TileRasterSource {
public:
    virtual ~TileRasterSource();

    /// Fills `out` over the region, level and geometry it already carries
    /// (1.8). Called concurrently from worker threads, once per tile:
    /// implementations must be ThreadSafe.
    virtual void fillTile(BlockView& out) = 0;

    /// The plane layout the framework must allocate before calling `fillTile`.
    virtual DataType planeType() const = 0;
    virtual int32_t  planeCount() const = 0;
};
using TileRasterSourcePtr = std::shared_ptr<TileRasterSource>;

/// A raster source backed by the objects of one layer: for each tile it
/// queries `source` for the objects of `layer` intersecting that tile and
/// rasterises the requested content into the plane, at the tile's own level.
/// The query carries `StageVisibility::CompletedOnly` (UR-STG-5).
///
/// This is how a fine stage reads a coarse stage's result as pixels rather
/// than as a go/no-go mask — the coarse-to-fine pattern of `06-algorithm.md`
/// 6.7. `source` is borrowed and must outlive the returned object (C-8).
TileRasterSourcePtr makeLayerRasterSource(ObjectSource& source, LayerId layer,
                                          LayerRasterContent);
```

**Thread-safety:** `Cursor` is ThreadConfined. `ObjectSink`, `EdgeSink`,
`AnnotationSink` and `ObjectSource` implementations declare their own;
`storage`'s are ThreadSafe. `TileMaskSource` and `TileRasterSource`
implementations must be ThreadSafe — the framework calls them from workers.
**Requirements:** UR-RES-2, UR-RES-5, UR-RES-6, UR-EXT-4, decision 7.

`RasterSink` is the fourth sink and the only one that takes pixels. The other
three carry objects and claims about them; this one carries the chain's final
raster output tile by tile, so a stage can persist a probability surface, a
distance transform or a background estimate — products that have no contour
and therefore cannot be expressed as objects at all (`03-storage.md` 3.8).
Like the others it names no stage: attribution belongs to the writer behind
the interface.

`TileRasterSource` is `TileMaskSource` with a value instead of a bit, and it
exists for one reason: a coarse stage's output is useful to a fine stage as
*pixels*, not only as a region to stay inside. A restriction answers "compute
here"; an auxiliary raster answers "and here is what the coarse pass concluded
about this pixel", which is what lets a fine operation find structure the
coarse level could not resolve — detail rising above a coarse surface. It
resolves one tile at a time for the same reason the mask does: a label raster
of the whole slide at level 0 is 32 GB, and one tile's worth is a megabyte.

`makeLayerRasterSource` lives in `core` on exactly the argument that put
`makeLayerMaskSource` here — it needs an `ObjectSource`, an `ObjectQuery` and
the rasteriser of 1.17, all of which are `core`. The alternative placement,
`reporting::rasteriseLabels` (`07-reporting.md` 7.6), is unreachable from the
module that needs it: `workspace` does not depend on `reporting` and must not
start to, so the export-side rasteriser stays an export-side concern.

**`ObjectAnchor` is how a refinement learns *which* object it is standing in**,
and it is the content a coarse-to-fine pair almost always wants. `Label` is
useless for that purpose and quietly so: no segmentation operation in the
catalog sets a label (`06-algorithm.md` 6.7), so a watershed's basins are
unlabelled and `Label` rasterises 0 across the whole plane. `Membership`
collapses every basin into one region. Only `ObjectAnchor` distinguishes them.

It works because identity in this library is already a point. `ObjectId` is a
layer plus `anchorPacked()` — `(y << 31) | x`, one `int64_t`, derived from
geometry rather than from a counter (decision 8). So the value is stable across
tile sizes and worker counts by construction, is globally unique within the
layer, and needs neither an enumeration of the layer nor a declared bound on
its size. The reader knows the layer, so it can rebuild the full `ObjectId` and
query the object for its polygon or measurements when identity alone is not
enough.

An earlier draft of this section refused pixel-granular identity outright. The
refusal was right about the mechanism it had in mind — a dense per-tile index
differs between tilings and breaks UR-QA-3 — and wrong to generalise from it,
because the anchor was already sitting there with none of that problem.

`planeType()` is therefore `Int64` for `ObjectAnchor`, against `Int32` for
`Label` and `Byte` for `Membership`. At eight bytes a pixel a 768-square halo
tile costs 4.7 MB, which is per-tile affordable and whole-slide impossible —
the usual shape here, and the reason this source resolves one tile at a time.

`AnnotationSink` is the write side of the classification path, and it is
deliberately shaped like `EdgeSink` rather than like `ObjectSink`: both assert
something *about* objects that already exist, neither creates or modifies one,
and both are deleted by the stage that made the assertion. An operation that
classifies therefore looks exactly like one that attaches — it takes the sink
from its context, asserts, and never learns whether a store is behind it.

These interfaces are the whole reason `algorithm` never links `storage`.
An operation emits into an `ObjectSink` and knows nothing about transactions,
batching or SQLite; the batching and journal atomicity of decision 6 exist in
exactly one implementation, which an algorithm author cannot bypass because
they never see a database.

`EdgeSink` is the same arrangement for containment. UR-RES-5 and UR-STG-3
require a stage to assert parent edges — a `cells` stage attaching
pre-existing nuclei is the standard case — and without an abstract sink there
was no route from an operation to `storage::EdgeWriter` that did not link the
two modules. Neither sink names a stage: attribution belongs to the writer
behind the interface (`03-storage.md` 3.5, 3.6), so an operation cannot
misattribute what it emits and does not have to know it could.

`Cursor` is ThreadConfined by decision 4 — sharing one across threads would
make per-thread iteration order non-deterministic and buy nothing.

`TileMaskSource` is the third member of the same family, and exists for the
same reason the other two do: `pipeline` must restrict a run to the objects of
an earlier stage (UR-STG-3, `04-workspace.md` 4.4) without linking `storage`.
It resolves one tile at a time rather than up front, which is what keeps the
restriction inside the memory ceiling — the union of a `cells` layer at the
reference object count is several GB of geometry, while one tile's worth of it
is a few hundred polygons and a 256 KB raster.

`makeLayerMaskSource` is concrete and lives here because it needs nothing but
an `ObjectSource`, an `ObjectQuery` and the rasteriser of 1.17, all of which
are `core`. A caller with no store — a directly constructed `Pipeline` — binds
no mask source at all, and a caller with some other notion of a region
implements the interface.

There is no method anywhere that materialises a result set. UR-RES-6 requires
that materialisation be an explicit, separate, filter-bounded call, and it is
provided by `reporting`, never here.

## 1.10 Object query — `core/object_query.hpp`

```cpp
enum class StageVisibility : int32_t { CompletedOnly, IncludeInFlight };
enum class SpatialPredicate : int32_t { Intersects, ContainedBy };
enum class AttachmentFilter : int32_t { Any, AttachedOnly, UnattachedOnly };
enum class LabelFilter      : int32_t { Any, LabelledOnly, UnlabelledOnly };

class ObjectQuery {
public:
    ObjectQuery();
    ObjectQuery(const ObjectQuery&);
    ~ObjectQuery();

    ObjectQuery& layer(LayerId);
    ObjectQuery& stage(StageId);
    ObjectQuery& intersecting(const Rect64&);
    ObjectQuery& withinPolygon(const Polygon&, SpatialPredicate);
    /// Matches an object whose **effective label set** contains this label:
    /// the producing stage's label on the object row, plus every label any
    /// stage has since asserted (`03-storage.md` 3.7). Existential — one
    /// match suffices, and an object labelled by three stages matches three
    /// different queries.
    ObjectQuery& label(LabelId);

    /// `LabelledOnly` and `UnlabelledOnly` test the effective set, so an
    /// object the producer left unlabelled stops being unlabelled once some
    /// stage annotates it.
    ObjectQuery& labelFilter(LabelFilter);
    ObjectQuery& attachment(AttachmentFilter);
    /// Tests the producing stage's measurements together with every asserted
    /// one of that name. Existential, like `label`: where two stages asserted
    /// the same name, one satisfying value is a match. `AnnotationStore`
    /// (`03-storage.md` 3.7) is where a consumer who needs to know *which*
    /// stage said what resolves it.
    ObjectQuery& measurementAtLeast(const char* name, double value);
    ObjectQuery& measurementAtMost(const char* name, double value);
    ObjectQuery& visibility(StageVisibility);     // default CompletedOnly
    ObjectQuery& limit(int64_t maxObjects);

    bool hasSpatialFilter() const noexcept;
    bool isUnfiltered() const noexcept;
};
```

**Thread-safety:** ThreadConfined (builder). **Requirements:** UR-RES-2,
UR-RES-3, UR-RES-4, UR-RES-5, UR-STG-3, decision 5.

Declared in `core` rather than `storage` because `algorithm` must be able to
query the objects of earlier stages (UR-STG-3) through `ObjectSource` without
linking `storage`.

`visibility()` defaults to `CompletedOnly`, which is what satisfies UR-STG-5:
a later stage querying a layer while another stage writes it sees nothing
partial. An observer opts in explicitly and accepts retraction (UR-OBS-4).

## 1.11 Memory budget — `core/memory_budget.hpp`

```cpp
class MemoryBudget {
public:
    virtual ~MemoryBudget();
    virtual int64_t ceilingBytes() const = 0;
    virtual int64_t reservedBytes() const = 0;
    virtual int64_t availableBytes() const = 0;
    virtual bool    tryReserve(int64_t bytes) = 0;
    virtual void    reserveOrWait(int64_t bytes, const CancellationToken&) = 0;
    virtual void    release(int64_t bytes) = 0;
};
using MemoryBudgetPtr = std::shared_ptr<MemoryBudget>;

MemoryBudgetPtr makeMemoryBudget(int64_t ceilingBytes);
```

**Thread-safety:** ThreadSafe. **Requirements:** UR-EXE-2.

Declared in `core` because UR-EXE-2's ceiling must cover raster tiles, object
accumulation *and* store buffers together, and `pipeline` does not depend on
`storage`. `workspace` constructs one and hands the same instance to both, which
is the only way a single ceiling can span both.

## 1.12 Progress and cancellation — `core/control.hpp`

```cpp
class CancellationToken {
public:
    CancellationToken();                  // never cancelled
    bool isCancelled() const noexcept;
    void throwIfCancelled() const;        // throws CancelledError
};

class CancellationSource {
public:
    CancellationSource();
    ~CancellationSource();
    CancellationToken token() const noexcept;
    void cancel() noexcept;
    bool isCancelled() const noexcept;
};

class ProgressObserver {
public:
    virtual ~ProgressObserver();
    virtual void onProgress(const char* stageName, double fraction,
                            int64_t completed, int64_t total) = 0;
};
```

**Thread-safety:** `CancellationToken` and `CancellationSource` are ThreadSafe.
`ProgressObserver` implementations must be ThreadSafe — the pool calls them
from worker threads. **Requirements:** UR-EXE-4.

## 1.13 Provenance — `core/provenance.hpp`

```cpp
class Provenance {
public:
    Provenance();
    ~Provenance();

    const char* slideId() const noexcept;
    int32_t     sceneIndex() const noexcept;
    const char* libraryVersion() const noexcept;
    int64_t     createdAtUnixMs() const noexcept;

    void setSlideId(const char*);
    void setSceneIndex(int32_t);
    int32_t toJson(char* buffer, int32_t capacity) const;
};
```

**Thread-safety:** ThreadCompatible. **Requirements:** UR-RES-11.

Per-stage configuration is recorded against the stage by `workspace`, not here;
this is the slide-level record. UR-RES-11 requires both be recoverable from the
store alone, without the code that produced them.

## 1.14 GeoJSON codec — `core/geojson.hpp`

```cpp
class GeoJsonReader {
public:
    explicit GeoJsonReader(const char* path);
    ~GeoJsonReader();

    bool next();                          // advance; false at end
    const Polygon& geometry() const noexcept;
    const char*    classification() const noexcept;  // "" if none
    const char*    layerName() const noexcept;       // "" if none
    int32_t        propertyCount() const noexcept;
    const char*    propertyName(int32_t) const;
    const char*    propertyValue(int32_t) const;
    int64_t        featuresRead() const noexcept;
};

class GeoJsonWriter {
public:
    explicit GeoJsonWriter(const char* path);
    ~GeoJsonWriter();

    void begin(const Provenance&);
    /// `labelNames` and `labelIsWorkspaceLocal` are parallel arrays of
    /// `labelCount` entries holding the object's effective label set
    /// (`03-storage.md` 3.7), in that set's fixed order. `labelCount == 0`
    /// for an object no stage has labelled.
    void writeFeature(const ObjectRecord&,
                      const char* layerName,
                      const char* const* labelNames,
                      const bool* labelIsWorkspaceLocal,
                      int32_t labelCount);
    void end();
    int64_t featuresWritten() const noexcept;
};
```

**Thread-safety:** ThreadConfined. **Requirements:** UR-RES-8, UR-RES-15,
UR-RES-18.

One codec, used from both directions by different modules: `storage` holds the
reader for import (decision 3), `reporting` holds the writer for export. That
placement is why splitting import from export across two modules does not mean
two GeoJSON parsers.

Both are streaming. `writeFeature` emits as it goes, so exporting ten million
objects stays within the memory ceiling (UR-RES-8).

`labelIsWorkspaceLocal` is written into the feature's properties so a consumer
can tell which labels are portable between workspaces (UR-RES-18) — one flag
per label, since a set may mix standard and local ones.

**Labels arrive as an array because an object may carry several.** An earlier
signature took one `const char*`, which was right while a label lived on the
object row and nowhere else. Once a stage can assert a classification on an
object another stage wrote (`03-storage.md` 3.7), a single pointer can only
export one member of the set and silently drop the rest, which is the worst of
the available failures: the file parses, the count is right, and the
classification is wrong. The caller passes the set in the order
`AnnotationStore` fixes, so two exports of an unchanged store are byte-identical
(UR-QA-3).

Measurements need no such change. `ObjectRecord::measurements()` is mutable, so
`reporting` merges the asserted measurements into the record's own set before
calling — the set is already a collection, and nothing about its shape depended
on there being one writer.

## 1.15 Logging — `core/log.hpp`

```cpp
enum class LogLevel : int32_t { Trace, Debug, Info, Warning, Error, Off };

class LogSink {
public:
    virtual ~LogSink();
    virtual void write(LogLevel, const char* message) = 0;
};
using LogSinkPtr = std::shared_ptr<LogSink>;

void     setLogSink(LogSinkPtr) noexcept;
void     setLogLevel(LogLevel) noexcept;
LogLevel logLevel() noexcept;
void     logMessage(LogLevel, const char* format, ...);
```

**Thread-safety:** ThreadSafe. **Requirements:** none directly; supports
UR-EXE-5 diagnostics.

Declared here so every module can log; *configured* by `config`
(`configureLogging`). That is the whole reason the `config → core` dependency
points the way it does rather than the reverse.

## 1.16 Version — `core/version.hpp`

```cpp
const char* libraryVersion() noexcept;
int32_t     libraryVersionMajor() noexcept;
int32_t     libraryVersionMinor() noexcept;
int32_t     libraryVersionPatch() noexcept;
```

**Requirements:** UR-API-6, UR-RES-11.

## 1.17 Raster mask — `core/raster_mask.hpp`

```cpp
/// A binary mask over a rectangular region of one pyramid level: one byte per
/// pixel, 0 outside and 255 inside, row-major with rows contiguous and no
/// padding. Allocation is `regionAtLevel.area()` bytes and is the caller's to
/// budget (1.11).
class RasterMask {
public:
    RasterMask() noexcept;                 // null

    /// `regionAtLevel` is in level pixels, `levelScale` the factor that maps
    /// them to level-0 — `LevelSelector::scaleOfLevel` (`05-pipeline.md` 5.6)
    /// supplies it. Every pixel starts clear. Throws InvalidArgumentError for
    /// an empty region, a negative level, or a scale that is not positive
    /// and finite.
    RasterMask(const Rect64& regionAtLevel, int32_t level, double levelScale);

    RasterMask(const RasterMask&);
    RasterMask(RasterMask&&) noexcept;
    RasterMask& operator=(const RasterMask&);
    RasterMask& operator=(RasterMask&&) noexcept;
    ~RasterMask();

    bool    isNull() const noexcept;
    Rect64  regionAtLevel() const noexcept;
    int32_t level() const noexcept;
    double  levelScale() const noexcept;

    /// Row `y` of the region, indexed from its top edge, `width` bytes long.
    const uint8_t* row(int64_t y) const;
    uint8_t*       row(int64_t y);

    /// Level coordinates, absolute — not relative to the region. A point
    /// outside the region is false, never an error: a caller testing an
    /// object that strayed past the tile gets the right answer.
    bool test(Point64 levelPoint) const noexcept;

    /// The same question asked in level-0 slide coordinates, which is the
    /// space every emitted polygon and every anchor lives in (1.3). Answers
    /// "is the level pixel CONTAINING this slide point set", by
    /// `floor(v / levelScale)` per ordinate.
    bool testLevel0(Point64 slidePoint) const noexcept;

    void    clear() noexcept;
    bool    isEmpty() const noexcept;      // no pixel set
    int64_t setPixelCount() const noexcept;
};

/// Sets every pixel of `mask` that `polygon` covers any part of. Additive:
/// pixels already set stay set, so a mask is built by walking a cursor and
/// calling this once per object.
///
/// `polygon` is in level-0 slide coordinates and the conversion happens
/// inside, against the mask's own `levelScale`. No intermediate integer
/// polygon exists, so nothing is rounded twice.
void rasteriseInto(RasterMask& mask, const Polygon& polygon);

/// Widens the set region by `radius` level pixels, in place. Separable and
/// exact; `radius` of 0 is a no-op and a negative one throws.
void dilateMask(RasterMask& mask, int32_t radius);

/// Fills `polygon` into plane `planeIndex` of `plane` with `value`, for a
/// `TileRasterSource` building an auxiliary input (1.9). `polygon` is in
/// level-0 coordinates; the level scale is recovered from the view itself as
/// `slideRect().width() / width()`, so the mapping comes from the two
/// integers the framework already agreed on rather than from a stored float —
/// which is what keeps a 4.0001 pyramid in register (`05-pipeline.md` 5.6).
///
/// Throws `InvalidArgumentError` when `value` does not fit the plane's
/// `DataType`. Uses the same conservative fill rule as the mask overload.
void rasteriseInto(BlockView& plane, int32_t planeIndex,
                   const Polygon& polygon, int64_t value);
```

**Thread-safety:** ThreadCompatible — one mask per tile, built and read by the
worker that owns it. **Requirements:** UR-IMG-5, UR-STG-3, UR-QA-2, UR-QA-3.

### Why the restriction is a raster and not a polygon

A polygon mask has to be *the whole mask*, because a polygon has no natural
crop. The union of a `tissue` layer is two or three rings and that is fine. The
union of a `cells` layer at the reference object count is ten million disjoint
rings — some gigabytes of geometry, built eagerly before the first tile is
read, against a 2 GB ceiling (UR-EXE-2) — and then each of ten million emitted
objects is tested against it.

A raster has a natural crop, so the mask never exists whole. Per tile: one
spatial query for the objects covering it, a few hundred polygons rasterised
into 256 KB, and an O(1) lookup per test. The query is the one the R\*Tree
already serves, measured at 2–6 ms for a tile-sized viewport including
geometry, and it scales with result size rather than store size
([spike findings](../2026-10-07-spike-q3-q4-findings.md) §2.2). Both terms are
bounded by the tile, so a restriction to `cells` costs what a restriction to
`tissue` costs.

The rasteriser is in `core`, not in `pipeline`, for the reason the GeoJSON
codec is (1.14): `reporting::rasteriseLabels` (`07-reporting.md` 7.6) already
turns objects into pixels, on the OpenCV-free side of the library split. One
kernel, used by a module that exports masks and a module that applies them,
rather than two that can disagree about which pixels a shape covers.

### The fill rule is pinned, and it is conservative

**A pixel is set when the polygon covers any part of its area**, by the
even-odd rule of 1.3. Not when its centre is inside, and not by whatever the
backend's polygon fill happens to do.

Three things follow, and all three are the point:

- **It is backend-independent.** `cv::fillPoly` is not specified by this
  document and is not stable across the OpenCV 4→5 migration UR-BLD-4 plans. A
  mask whose boundary pixels shifted with the backend version would move object
  counts across an upgrade with nothing in the release notes to explain it.
- **It is origin-independent, which makes it tile-independent.** Whether a
  polygon touches a given pixel does not depend on where the rasteriser started
  or which tile asked. Two tiles sharing a seam therefore agree about every
  pixel on it by construction. This is the same property `TileGrid` buys by
  laying the grid out in level coordinates (`05-pipeline.md` 5.6), and it is
  bought here for the same reason: the failure it prevents — a mask that
  disagrees with itself across a seam — is silent, and shows up as a count that
  is nearly right.
- **It over-includes by less than one level pixel, never under-includes.** That
  is the one-sided error `04-workspace.md` 4.4 argues for, obtained from the
  fill rule rather than from dilating the geometry. A mask that under-includes
  drops objects that genuinely belong and leaves no trace that anything is
  missing.

Holes take the same rule with the sign reversed: a pixel is cleared only when
a hole covers **all** of it. A mask therefore never loses a pixel to rounding
at either kind of boundary.

`dilateMask` supplies the additional margin a coarse source layer needs. A
layer analysed at level 4 has a boundary good to ±16 level-0 pixels
(`03-storage.md` 3.3), and `makeLayerMaskSource` converts that half-step into
`ceil(halfStep / levelScale)` level pixels. For a `cells` layer written at
level 0 the half-step is 0 and the radius is 0 — the fill rule's sub-pixel
conservatism is the whole margin, which is the correct answer rather than a
missing one, because a level-0 boundary needs no slack.
