# SDS 01 — `core`

**Module:** `core` · **Library:** `slideio-processor-data` · **Depends on:** SlideIO types, std

Shared type vocabulary and the interfaces every other module talks through.
Performs no I/O beyond the GeoJSON codec, and holds no state except the
process-wide log sink and threshold (1.15), which every module must share.

Conventions in [00-conventions.md](00-conventions.md).

---

## 1.1 Errors — `core/error.hpp`

```cpp
enum class ErrorCode : int32_t {
    InvalidArgument, UnsupportedDataType, ChannelArity, StoreFormat,
    Stage, Tile, Cancelled, Io, Internal
};

struct ErrorContext {                     // POD, exempt from pimpl per C-2
    const char* stageName = nullptr;
    const char* operationName = nullptr;
    const char* cause = nullptr;
    bool hasTile = false;
    int64_t tileX = 0;
    int64_t tileY = 0;
};

class ProcessorError : public std::exception {
public:
    ProcessorError(const ProcessorError& other) noexcept;
    ProcessorError& operator=(const ProcessorError& other) noexcept;
    ~ProcessorError() override;

    const char* what() const noexcept override;
    ErrorCode code() const noexcept;
    const char* stageName() const noexcept;      // "" if not applicable
    const char* operationName() const noexcept;  // "" if not applicable
    const char* cause() const noexcept;          // "" if none
    bool hasTile() const noexcept;
    int64_t tileX() const noexcept;              // 0 unless hasTile()
    int64_t tileY() const noexcept;

protected:
    ProcessorError(ErrorCode code, const char* message, const ErrorContext& context);
};

class InvalidArgumentError : public ProcessorError {
public:
    explicit InvalidArgumentError(const char* message, const ErrorContext& context = ErrorContext());
};
class UnsupportedDataTypeError : public ProcessorError {
public:
    UnsupportedDataTypeError(const char* message, DataType offendingType, DataTypeSet supportedTypes,
                             const ErrorContext& context = ErrorContext());
    DataType offendingType() const noexcept;
    DataTypeSet supportedTypes() const noexcept;
};
class ChannelArityError : public ProcessorError {
public:
    ChannelArityError(const char* message, int32_t offendingChannelCount,
                      const ErrorContext& context = ErrorContext());
    int32_t offendingChannelCount() const noexcept;
};
class StoreFormatError : public ProcessorError {
public:
    StoreFormatError(const char* message, int32_t storeVersion, int32_t libraryVersion,
                     const ErrorContext& context = ErrorContext());
    int32_t storeVersion() const noexcept;
    int32_t libraryVersion() const noexcept;
};
class StageError : public ProcessorError {
public:
    explicit StageError(const char* message, const ErrorContext& context = ErrorContext());
};
class TileError : public ProcessorError {
public:
    explicit TileError(const char* message, const ErrorContext& context = ErrorContext());
};
class CancelledError : public ProcessorError {
public:
    explicit CancelledError(const char* message, const ErrorContext& context = ErrorContext());
};
class IoError : public ProcessorError {
public:
    explicit IoError(const char* message, const ErrorContext& context = ErrorContext());
};
class InternalError : public ProcessorError {
public:
    explicit InternalError(const char* message, const ErrorContext& context = ErrorContext());
};
```

**Thread-safety:** `ProcessorError` and every subclass are Immutable;
`ErrorContext` is ThreadCompatible. **Requirements:** UR-API-5, UR-EXE-5,
UR-IMG-11, UR-RES-12.

The hierarchy is closed — UR-API-5 requires that no exception escapes it, so
the public boundary of every module catches and translates (C-6, which also
fixes the one exception, `std::bad_alloc`). `UnsupportedDataTypeError`
carries the supported set so the message can name both the type and the
conversion needed, as UR-IMG-11 requires.

**Context arrives as one plain struct.** Every subclass has a public
constructor taking the message, then its own data fields where it has them,
then an `ErrorContext` defaulting to empty. `ErrorContext` is zero-initialised
by default, so a throw site sets only the fields it knows; a null string reads
as empty, and the constructor copies every string, so the pointers need live
only for the call. A null `message` also reads as empty rather than throwing,
because an exception constructor must not fail for that reason. One struct
rather than a setter per field keeps the error Immutable from the moment it
exists.

**`InternalError` is the category of last resort.** It carries
`ErrorCode::Internal` and is used where a broken invariant, or a third-party
failure with no better category, has to cross the boundary — the target C-6
names for every translated exception that is not an I/O failure.

**`what()` is composed once, at construction:** the message, then
`; stage: S`, `; operation: O`, `; tile: (X, Y)` and `; cause: C`, each only
when present, in that order. Copies share the composed text, which is why the
copy members are `noexcept` — copying an exception while it propagates must
not be able to throw.

## 1.2 Data types — `core/data_type.hpp`

```cpp
enum class DataType : int32_t {
    Byte, Int8, UInt16, Int16, UInt32, Int32,
    UInt64, Int64, Float16, Float32, Float64, Unknown
};

const char* dataTypeName(DataType type) noexcept;
int32_t dataTypeSizeBytes(DataType type) noexcept;
bool dataTypeIsFloat(DataType type) noexcept;
bool dataTypeIsSigned(DataType type) noexcept;
DataType dataTypeFromSlideio(int32_t slideioDataType) noexcept;
int32_t dataTypeToSlideio(DataType type) noexcept;

class DataTypeSet {                       // POD-like value, 4 bytes
public:
    DataTypeSet() noexcept;               // empty
    static DataTypeSet all() noexcept;
    static DataTypeSet integerTypes() noexcept;
    static DataTypeSet floatTypes() noexcept;

    DataTypeSet& add(DataType type) noexcept;
    DataTypeSet& remove(DataType type) noexcept;
    bool contains(DataType type) const noexcept;
    bool isEmpty() const noexcept;
    int32_t count() const noexcept;
    DataType at(int32_t index) const;     // throws InvalidArgumentError
    uint32_t bits() const noexcept;

    DataTypeSet intersect(DataTypeSet other) const noexcept;
    bool operator==(DataTypeSet other) const noexcept;
};
```

**Thread-safety:** `DataTypeSet` is ThreadCompatible — `add`, `remove` and
assignment mutate it, so it is not Immutable (C-7); the free functions are
safe from any thread. **Requirements:** UR-IMG-8, UR-IMG-11, UR-IMG-12.

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

bool operator==(Point64 a, Point64 b) noexcept;

struct Rect64 {                           // POD; half-open
    int64_t x;
    int64_t y;
    int64_t width;
    int64_t height;

    int64_t right() const noexcept;       // x + width
    int64_t bottom() const noexcept;
    int64_t area() const noexcept;        // 64-bit: UR-IMG-2
    bool isEmpty() const noexcept;
    bool contains(Point64 p) const noexcept;
    bool intersects(const Rect64& other) const noexcept;
    Rect64 intersected(const Rect64& other) const noexcept;
    Rect64 united(const Rect64& other) const noexcept;
};

bool operator==(const Rect64& a, const Rect64& b) noexcept;

class Polygon {
public:
    Polygon();
    Polygon(const Polygon& other);
    Polygon(Polygon&& other) noexcept;
    Polygon& operator=(const Polygon& other);
    Polygon& operator=(Polygon&& other) noexcept;
    ~Polygon();

    int32_t ringCount() const noexcept;
    bool ringIsHole(int32_t ring) const;
    int64_t ringPointCount(int32_t ring) const;
    int64_t ringPoints(int32_t ring, Point64* buffer, int64_t capacity) const;

    void addRing(const Point64* points, int64_t count, bool isHole);
    void clear() noexcept;
    bool isEmpty() const noexcept;

    Rect64 boundingBox() const noexcept;
    Point64 anchor() const noexcept;      // topmost-then-leftmost vertex
    double area() const noexcept;         // outer rings minus holes
    double perimeter() const noexcept;
    Point64 centroid() const noexcept;
    bool contains(Point64 p) const noexcept;  // p is a pixel index
};
```

**Thread-safety:** `Point64`, `Rect64` and `Polygon` are ThreadCompatible.
**Requirements:** UR-IMG-2, UR-SEG-4, UR-SEG-5.

**Points are lattice corners; `contains` takes a pixel.** Vertices lie on the
pixel-corner lattice, so the unit square `(0,0)-(1,1)` is pixel `(0,0)`.
`Polygon::contains(p)` reads its argument as a *pixel index* and tests that
pixel's centre, `(p.x + 0.5, p.y + 0.5)`, by even-odd, so a centre never lies
on an edge and the answer needs no tie rule. `RasterMask::testLevel0` (1.17)
takes the other reading, a corner-lattice *point*. `Rect64` is half-open,
covering `x <= px < right()`, and two rectangles that only touch do not
intersect. `boundingBox()` is `{minX, minY, maxX − minX, maxY − minY}` over
every vertex.

**`addRing` validates, so every stored ring is usable.** It drops a last point
equal to the first, then rejects with `InvalidArgumentError`, leaving the
polygon unchanged: a null array; fewer than 3 points; points that all lie on
one line, because a zero-area ring contributes nothing to even-odd membership
and would make the bounding box empty; a ring that would make the polygon's
extent (`maxX − minX` or `maxY − minY`) overflow `int64_t`; and a hole added to
an empty polygon. `boundingBox()` therefore never overflows, and is non-empty
for every non-empty polygon.

**Precision does not depend on distance from the origin.** `area()`,
`perimeter()`, `centroid()`, `contains()` and the rasterisers of 1.17 keep
integer precision for coordinates above 2^53, where a `double` can no longer
tell neighbouring integers apart: a unit square at `x = 2^53` has area 1,
contains its own pixel and not its neighbour's, and rasterises to one pixel. A
polygon's results depend on its vertices relative to one another, not on where
it sits, and two tiles sharing a seam agree on it wherever the polygon is.

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
`contains()`, `area()` and the rasteriser of 1.17 all use it. They do not ask
the same question, though: `contains()` tests a pixel's centre, while the
rasteriser sets a pixel when the even-odd region covers *any* part of its
square. Raster coverage is therefore a superset of point-in-polygon
membership — every pixel `contains()` accepts is set, plus the boundary pixels
the region only clips — and the two are equal only for a polygon whose edges
are all axis-aligned, rasterised at level 0. The conformance test asserts the
inclusion in general and the equality only for that case.

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
    LayerId() noexcept;                   // invalid, value 0
    explicit LayerId(int32_t value) noexcept;
    int32_t value() const noexcept;
    bool isValid() const noexcept;        // value() > 0
    bool operator==(LayerId other) const noexcept;
    bool operator!=(LayerId other) const noexcept;
    bool operator<(LayerId other) const noexcept;
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
    static ObjectId fromAnchor(LayerId layer, Point64 anchor) noexcept;
    static bool fromString(const char* text, ObjectId& out) noexcept;

    bool isValid() const noexcept;
    LayerId layer() const noexcept;
    int64_t anchorPacked() const noexcept;   // (y << 31) | x; -1 if invalid
    Point64 anchorPoint() const noexcept;

    int32_t toString(char* buffer, int32_t capacity) const;
    bool operator==(const ObjectId& other) const noexcept;
    bool operator!=(const ObjectId& other) const noexcept;
    bool operator<(const ObjectId& other) const noexcept;   // layer, then anchor
};
```

**Thread-safety:** Immutable. **Requirements:** UR-RES-4, UR-QA-3, decision 8.

**An id is valid iff its value is positive.** `LayerId`, `StageId`, `LabelId`
and `RasterId` default to `0`, and `isValid()` is `value() > 0`. That leaves
`0` free to mean "none", which the `Label` and `Membership` rasters of 1.9 use
as their background.

**`fromAnchor` never wraps.** `(y << 31) | x` fits only `0 <= x < 2^31` and
`0 <= y < 2^32`. An anchor outside that range, or an invalid layer, gives an
invalid `ObjectId` — layer `0`, `anchorPacked()` `-1`, `anchorPoint()`
`{0, 0}` — rather than one that silently collides with another object's
identity.

**The text form is canonical.** `toString` writes `"<layer>:<x>:<y>"`, or
`"invalid"`, and `fromString` accepts exactly that form back: decimal with no
sign, spaces, trailing characters or leading zeros (a lone `0` is allowed for
`x` and `y`), a layer of at least 1 and an anchor in range. It returns `false`
for null or malformed text and leaves `out` unchanged. Rejecting every
non-canonical spelling means one id has exactly one string, so text written by
`toString` round-trips and two strings naming the same object are equal.

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
    Measurement() noexcept;                   // name "", value 0, Dimensionless

    const char* name() const noexcept;        // valid until the owning set changes
    double value() const noexcept;
    MeasurementUnit unit() const noexcept;
    bool isCalibrated() const noexcept;
};

class MeasurementSet {
public:
    MeasurementSet();
    MeasurementSet(const MeasurementSet& other);
    MeasurementSet(MeasurementSet&& other) noexcept;
    MeasurementSet& operator=(const MeasurementSet& other);
    MeasurementSet& operator=(MeasurementSet&& other) noexcept;
    ~MeasurementSet();

    int32_t count() const noexcept;
    Measurement at(int32_t index) const;
    bool find(const char* name, Measurement& out) const noexcept;

    void set(const char* name, double value, MeasurementUnit unit);
    void remove(const char* name) noexcept;
    void clear() noexcept;
};
```

**Thread-safety:** ThreadCompatible. **Requirements:** UR-SEG-5, UR-SEG-6.

`Measurement` has a public default constructor so a caller can declare the
`out` argument of `find`; populated instances come only from a set. The set
has the full copy and move set, keeps insertion order, and `set` on an
existing name replaces its value and unit in place.

`isCalibrated()` is derived from the unit alone: it is `false` iff the unit is
`Pixels` or `SquarePixels`. A measurement taken on a scene that lacked
resolution metadata is recorded in pixel units, so a consumer can tell
"3.2 µm²" from "3.2 px² and no calibration was available" — the distinction
UR-SEG-6 requires be documented rather than silently collapsed.

## 1.6 Object record — `core/object_record.hpp`

```cpp
class ObjectRecord {
public:
    ObjectRecord();
    ObjectRecord(const ObjectRecord& other);
    ObjectRecord(ObjectRecord&& other) noexcept;
    ObjectRecord& operator=(const ObjectRecord& other);
    ObjectRecord& operator=(ObjectRecord&& other) noexcept;
    ~ObjectRecord();

    ObjectId id() const noexcept;         // derived from layer + geometry anchor
    LayerId layer() const noexcept;

    /// The stage that produced this object. Read-only: it is stamped on
    /// emission, never set by the producer. The stamping route belongs to
    /// `storage` and does not exist yet, so in this release it is always
    /// invalid (see below).
    StageId stage() const noexcept;

    const Polygon& geometry() const noexcept;
    Rect64 boundingBox() const noexcept;

    bool hasLabel() const noexcept;
    LabelId label() const noexcept;       // invalid if !hasLabel()

    const MeasurementSet& measurements() const noexcept;
    MeasurementSet& measurements() noexcept;

    void setGeometry(const Polygon& geometry);
    void setLayer(LayerId layer);
    /// The producing stage's own classification of this object. Additional
    /// labels asserted later by other stages are not here; they live in the
    /// annotation store (`03-storage.md` 3.7) and reach a consumer through
    /// the effective label set.
    void setLabel(LabelId label);
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
is meant to read valid on every record obtained from a `Cursor` or
`ObjectStore::get`. Code that round-trips a record — reading one, modifying it,
emitting it again — gets the emitting stage's id on the new row, not the
original's, which is what re-running a stage over another's objects must do.

**Stamping is deferred to `storage`.** `core` holds the field, but no public
member can write it, and the route by which a store fills it on a record it
reads back is `storage`'s to add (`03-storage.md` 3.5). Until then `stage()`
returns an invalid id on every record. The alternative — a public setter as a
stopgap — would reopen the two-sources-of-truth problem above.

A moved-from record is a default record (C-8): empty geometry, invalid layer,
stage and label, no measurements.

There is deliberately no parent accessor. UR-RES-4 requires the parent relation
be a separate edge record so an object can be attached after it was written
without rewriting it; putting it here would make that impossible to honour.

## 1.7 Channel selection — `core/channel_selection.hpp`

```cpp
class ChannelSelection {
public:
    ChannelSelection();                   // empty
    ChannelSelection(const ChannelSelection& other);
    ChannelSelection(ChannelSelection&& other) noexcept;
    ChannelSelection& operator=(const ChannelSelection& other);
    ChannelSelection& operator=(ChannelSelection&& other) noexcept;
    ~ChannelSelection();

    static ChannelSelection single(int32_t channel);
    static ChannelSelection range(int32_t first, int32_t count);
    static ChannelSelection all() noexcept;

    ChannelSelection& add(int32_t channel);
    bool isAll() const noexcept;
    int32_t count() const noexcept;       // 0 and isAll() means "all"
    int32_t at(int32_t index) const;
    bool contains(int32_t channel) const noexcept;
};
```

**Thread-safety:** ThreadCompatible. **Requirements:** UR-IMG-6, UR-IMG-7,
UR-ALG-2.

Selection is explicit and ordered. Nothing in the library infers a channel's
meaning from its index; an operation that needs RGB asks the scene's metadata
and raises a documented error if it is absent (UR-IMG-7).

`add` rejects a negative or already-selected channel, and any channel on an
`all()` selection, which has `count()` 0 and cannot be indexed. The class has
the full copy and move set; a moved-from selection is valid and empty.

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

    BlockView(const BlockView& other);
    BlockView(BlockView&& other) noexcept;
    BlockView& operator=(const BlockView& other);
    BlockView& operator=(BlockView&& other) noexcept;
    ~BlockView();

    bool isNull() const noexcept;
    int64_t width() const noexcept;
    int64_t height() const noexcept;
    Rect64 slideRect() const noexcept;    // position in level-0 coordinates
    int32_t level() const noexcept;

    int32_t planeCount() const noexcept;
    void* planeData(int32_t plane) const;
    DataType planeType(int32_t plane) const;
    int64_t planeRowStrideBytes(int32_t plane) const;
    int32_t planeSourceChannel(int32_t plane) const;
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
| the offset of the last byte of a plane's last row overflows `int64_t` (named as that plane's `rowStrideBytes`) |

The fields are checked in the table's order, planes in index order, so the
message names the first offending one. A moved-from view is null (C-8).

Validation is at construction for the reason it is everywhere else in this
library: a malformed view discovered at the first pixel read fails inside a
worker thread, mid-tile, with a stage already running.

What the constructor **cannot** check is that each buffer is really
`height * rowStrideBytes` bytes long. No argument carries that, and adding one
would make the common case — a view over a buffer the caller just allocated to
exactly that size — carry a value it has no way to get wrong. Debug builds
probe the last byte of each plane's final row with a volatile read, so a buffer
that is too small faults at construction rather than mid-tile; that is best
effort, since no portable non-faulting check exists. Release builds read
nothing and trust the caller.

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
    virtual void emit(const ObjectRecord& record) = 0;
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
    virtual void writeTile(const BlockView& tile) = 0;

    virtual void flush() = 0;
    virtual int64_t tilesWritten() const = 0;
};

class AnnotationSink {
public:
    virtual ~AnnotationSink();

    /// Attaches a label to an object that already exists, without modifying
    /// it. Additive: an object may carry any number of labels from any number
    /// of stages (`03-storage.md` 3.7).
    virtual void assertLabel(ObjectId object, LabelId label) = 0;

    /// Attaches a named measurement to an object that already exists. One
    /// stage may assert a given name on a given object once.
    virtual void assertMeasurement(ObjectId object, const char* name, double value,
                                   MeasurementUnit unit) = 0;

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
    /// Returns at least every object with positive-area overlap with the
    /// query's rectangle; see "What a spatial query returns" below.
    virtual CursorPtr query(const ObjectQuery& query) = 0;
    virtual int64_t count(const ObjectQuery& query) const = 0;
    virtual bool get(ObjectId id, ObjectRecord& out) const = 0;
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
/// `source` for the objects of `layer` intersecting that tile — grown by the
/// dilation, then by one level-0 pixel on each side — and rasterises them,
/// then widens the result by `dilationLevel0Pixels` (1.17) and crops it to
/// the tile. The query carries `StageVisibility::CompletedOnly`, so a
/// restriction never sees a concurrently running stage's partial output
/// (UR-STG-5).
///
/// `source` is borrowed and must outlive the returned object (C-8). The
/// result is ThreadSafe when `source` is, which `storage`'s implementation is.
/// Throws InvalidArgumentError when `layer` is invalid or the dilation is not
/// finite and >= 0.
TileMaskSourcePtr makeLayerMaskSource(ObjectSource& source,
                                      LayerId layer,
                                      double dilationLevel0Pixels);

enum class LayerRasterContent : int32_t {
    Label,        // the covering object's own label id; 0 where none covers or it has none
    Membership,   // 1 inside any object of the layer, 0 outside
    ObjectAnchor  // ObjectId::anchorPacked() of the covering object; -1 where none
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
    virtual int32_t planeCount() const = 0;
};
using TileRasterSourcePtr = std::shared_ptr<TileRasterSource>;

/// A raster source backed by the objects of one layer: for each tile it
/// queries `source` for the objects of `layer` intersecting the tile's
/// rasterised grid, grown by one level-0 pixel on each side, and rasterises
/// the requested content into plane 0, at the tile's own level. Where objects
/// overlap, the smallest `ObjectId` wins. The query carries
/// `StageVisibility::CompletedOnly` (UR-STG-5).
///
/// This is how a fine stage reads a coarse stage's result as pixels rather
/// than as a go/no-go mask — the coarse-to-fine pattern of `06-algorithm.md`
/// 6.7. `source` is borrowed and must outlive the returned object (C-8).
/// Throws InvalidArgumentError when `layer` is invalid or `content` is unknown.
TileRasterSourcePtr makeLayerRasterSource(ObjectSource& source,
                                          LayerId layer,
                                          LayerRasterContent content);
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
`planeCount()` is 1 for every content.

**`ObjectAnchor`'s background is `-1`, not 0.** `0` is a valid packed anchor —
an object anchored at `(0, 0)`, such as a full-image annotation — so a 0
background would be indistinguishable from that object. Anchors are written
exactly, and a record whose id is invalid (an anchor outside the packable range
of 1.4) is skipped rather than written as a wrapped value. `Label` and
`Membership` keep 0 as their background, which is safe because no valid
`LabelId` is 0 (1.4).

**Where objects overlap, the smallest `ObjectId` wins.** Any fixed rule would
do; this one depends on nothing but the objects themselves, so the pixel
written is the same whatever order the cursor delivers them in, and the result
is identical across worker counts and tile sizes (UR-QA-3).

**`Label` writes the record's own label, not the effective set.** An
`ObjectSource` delivers `ObjectRecord`s, and the effective label set
(`03-storage.md` 3.7) is not reachable through that interface, so a label
another stage asserted by annotation does not appear in the raster. Reaching
it is an obligation on `storage`, recorded in 3.7; until it is met, a
coarse-to-fine pair that needs an annotated classification as pixels cannot
get it from this source.

**Both layer sources pad the query.** Each tile's query rectangle is grown by
one level-0 pixel on each side, beyond any dilation, so that rounding between
level and level-0 coordinates never drops an object whose sliver reaches the
tile. Padding only adds candidates, and an extra candidate that covers no
pixel of the tile sets none, so it costs query rows and never correctness.

**The mask source's cost per tile is documented, not capped.** Each
`fillMask` call allocates a scratch mask of `(W + 2r) × (H + 2r)` bytes and a
working buffer of `(W + 2r) × min(H + 2r, 2r + 2)` bytes, where `W × H` is the
tile and `r = ceil(dilationLevel0Pixels / levelScale)` the radius in level
pixels. There is no dilation cap, by the project rule against fixed limits: a
large dilation at a fine level costs memory quadratic in `r` on every tile, and
it is the caller's to size for the level it runs at. A level radius above
`2^31 − 1` cannot be represented as `dilateMask`'s `int32_t` and is rejected
with `InvalidArgumentError` rather than clamped, because a clamped radius would
silently under-include.

**What the returned sources throw.** `fillMask` throws `InvalidArgumentError`
for a null mask, a radius above `2^31 − 1`, a dilated region whose coordinates
or pixel count overflow `int64_t`, or a level-0 query extent outside the
supported range, and `std::bad_alloc` when the scratch memory cannot be
allocated. `fillTile` throws `InvalidArgumentError` when the view lacks plane 0
of `planeType()`, its scale or origin is unusable (1.17), or its level-0 query
extent is outside the supported range. Both let `ProcessorError` and
`std::bad_alloc` from `source` through, and translate any other
`std::exception` that `source` throws to `InternalError`, with its `what()` as
the cause (C-6).

**What a spatial query returns.** An `ObjectSource` honouring
`intersecting(rect)` must return at least every object whose geometry has
positive-area overlap with `rect`. It may return more — a bounding-box match is
a valid implementation — and an object that only touches `rect`, sharing an
edge or a corner with zero-area overlap, need not be returned. The contract is
one-sided so that an R\*Tree can answer it directly; a consumer that needs the
exact set filters the result itself, as the rasteriser does by construction.

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

struct MeasurementBound {                 // POD, exempt from pimpl per C-2
    const char* name;                     // owned by the query it came from
    double value;
    bool isLowerBound;                    // true: at least; false: at most
};

class ObjectQuery {
public:
    ObjectQuery();
    ObjectQuery(const ObjectQuery& other);
    ObjectQuery(ObjectQuery&& other) noexcept;
    ObjectQuery& operator=(const ObjectQuery& other);
    ObjectQuery& operator=(ObjectQuery&& other) noexcept;
    ~ObjectQuery();

    ObjectQuery& layer(LayerId layer);
    ObjectQuery& stage(StageId stage);
    /// At least every object with positive-area overlap with `rect`; may
    /// return more, and need not return objects that only touch it (1.9).
    ObjectQuery& intersecting(const Rect64& rect);
    ObjectQuery& withinPolygon(const Polygon& polygon, SpatialPredicate predicate);
    /// Matches an object whose **effective label set** contains this label:
    /// the producing stage's label on the object row, plus every label any
    /// stage has since asserted (`03-storage.md` 3.7). Existential — one
    /// match suffices, and an object labelled by three stages matches three
    /// different queries.
    ObjectQuery& label(LabelId label);

    /// `LabelledOnly` and `UnlabelledOnly` test the effective set, so an
    /// object the producer left unlabelled stops being unlabelled once some
    /// stage annotates it.
    ObjectQuery& labelFilter(LabelFilter filter);
    ObjectQuery& attachment(AttachmentFilter filter);
    /// Tests the producing stage's measurements together with every asserted
    /// one of that name. Existential, like `label`: where two stages asserted
    /// the same name, one satisfying value is a match. `AnnotationStore`
    /// (`03-storage.md` 3.7) is where a consumer who needs to know *which*
    /// stage said what resolves it.
    ObjectQuery& measurementAtLeast(const char* name, double value);
    ObjectQuery& measurementAtMost(const char* name, double value);
    ObjectQuery& visibility(StageVisibility visibility);   // default CompletedOnly
    ObjectQuery& limit(int64_t maxObjects);

    bool hasSpatialFilter() const noexcept;
    bool isUnfiltered() const noexcept;

    // Read accessors, for implementers of ObjectSource.
    bool hasLayer() const noexcept;
    LayerId layerValue() const noexcept;
    bool hasStage() const noexcept;
    StageId stageValue() const noexcept;
    bool hasIntersecting() const noexcept;
    Rect64 intersectingValue() const noexcept;
    bool hasWithinPolygon() const noexcept;
    const Polygon& withinPolygonValue() const noexcept;
    SpatialPredicate withinPolygonPredicate() const noexcept;
    bool hasLabel() const noexcept;
    LabelId labelValue() const noexcept;
    LabelFilter labelFilterValue() const noexcept;
    AttachmentFilter attachmentValue() const noexcept;
    StageVisibility visibilityValue() const noexcept;
    bool hasLimit() const noexcept;
    int64_t limitValue() const noexcept;
    int32_t measurementBoundCount() const noexcept;
    MeasurementBound measurementBoundAt(int32_t index) const;
};
```

**Thread-safety:** `ObjectQuery` is ThreadConfined (builder);
`MeasurementBound` is ThreadCompatible. **Requirements:** UR-RES-2,
UR-RES-3, UR-RES-4, UR-RES-5, UR-STG-3, decision 5.

Declared in `core` rather than `storage` because `algorithm` must be able to
query the objects of earlier stages (UR-STG-3) through `ObjectSource` without
linking `storage`.

`visibility()` defaults to `CompletedOnly`, which is what satisfies UR-STG-5:
a later stage querying a layer while another stage writes it sees nothing
partial. An observer opts in explicitly and accepts retraction (UR-OBS-4).

**The query is readable, because someone has to implement `ObjectSource`.**
Setters alone would leave a query that only its builder could inspect, and the
module rules forbid a sibling module from reaching into `core/detail` — so
neither `storage` nor a test fake could honour one. Each setter therefore has
a read accessor: a `has*()` for each optional filter and a `*Value()` for each
field, returning the default when the filter is unset. Measurement bounds are
a list, read back in insertion order through the C-5 count-plus-accessor pair
as `MeasurementBound` values whose `name` stays valid until the next non-const
call on the query. A repeated scalar setter keeps the last value; each
`measurementAtLeast` or `measurementAtMost` call appends a bound.

Setters do not validate (C-10): a null measurement name is stored as empty and
a negative limit as given, and the consuming `ObjectSource` rejects what it
cannot use. `isUnfiltered()` ignores visibility and limit, which select among
visible objects rather than filter them. The class has the full copy and move
set; a moved-from query is a default query (C-8).

## 1.11 Memory budget — `core/memory_budget.hpp`

```cpp
class MemoryBudget {
public:
    virtual ~MemoryBudget();
    virtual int64_t ceilingBytes() const = 0;
    virtual int64_t reservedBytes() const = 0;
    virtual int64_t availableBytes() const = 0;
    virtual bool tryReserve(int64_t bytes) = 0;
    virtual void reserveOrWait(int64_t bytes, const CancellationToken& token) = 0;
    virtual void release(int64_t bytes) = 0;
};
using MemoryBudgetPtr = std::shared_ptr<MemoryBudget>;

MemoryBudgetPtr makeMemoryBudget(int64_t ceilingBytes);
```

**Thread-safety:** ThreadSafe. **Requirements:** UR-EXE-2.

Declared in `core` because UR-EXE-2's ceiling must cover raster tiles, object
accumulation *and* store buffers together, and `pipeline` does not depend on
`storage`. `workspace` constructs one and hands the same instance to both, which
is the only way a single ceiling can span both.

Reservations are accounting only; the budget allocates nothing. `reserveOrWait`
rejects a negative request, or one above the ceiling, with
`InvalidArgumentError` at once, because such a request could never succeed and
would otherwise wait forever. While waiting it re-checks the token about every
10 ms and throws `CancelledError` with nothing reserved. A failure of the
budget's internal lock is translated to `InternalError` (C-6).

**Open question — fairness.** `reserveOrWait` gives no fairness guarantee: a
waiter is woken and retries whenever bytes are released, so a large request
can starve behind a stream of smaller ones that keep fitting first. Whether
`pipeline`'s reservation pattern can produce that, and whether the budget
should queue waiters in order instead, is not decided.

## 1.12 Progress and cancellation — `core/control.hpp`

```cpp
class CancellationToken {
public:
    CancellationToken() noexcept;         // never cancelled
    CancellationToken(const CancellationToken& other) noexcept;
    CancellationToken(CancellationToken&& other) noexcept;
    CancellationToken& operator=(const CancellationToken& other) noexcept;
    CancellationToken& operator=(CancellationToken&& other) noexcept;
    ~CancellationToken();

    bool isCancelled() const noexcept;
    void throwIfCancelled() const;        // throws CancelledError
};

class CancellationSource {
public:
    CancellationSource();
    ~CancellationSource();
    CancellationSource(const CancellationSource&) = delete;
    CancellationSource& operator=(const CancellationSource&) = delete;

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

**Thread-safety:** `CancellationToken` is ThreadCompatible: concurrent `const`
calls on one token are safe, and assigning to it needs exclusive access.
Copies of a token share one atomic flag, so distinct copies may be queried
from any number of threads while `CancellationSource::cancel()` runs
concurrently — which is how a token is meant to be used, one copy per worker.
`CancellationSource` is ThreadSafe. `ProgressObserver` implementations must be
ThreadSafe — the pool calls them from worker threads. **Requirements:**
UR-EXE-4.

A default token holds no state, so constructing one is `noexcept` and
allocates nothing — a function can take `const CancellationToken&` with a
default argument at no cost. Tokens issued by a source stay valid after the
source is destroyed, and a moved-from token is never cancelled (C-8). The
source is neither copyable nor movable, because it is the one place that may
cancel; cancellation is permanent and idempotent.

## 1.13 Provenance — `core/provenance.hpp`

```cpp
class Provenance {
public:
    Provenance();
    Provenance(const Provenance& other);
    Provenance(Provenance&& other) noexcept;
    Provenance& operator=(const Provenance& other);
    Provenance& operator=(Provenance&& other) noexcept;
    ~Provenance();

    const char* slideId() const noexcept;
    int32_t sceneIndex() const noexcept;
    const char* libraryVersion() const noexcept;
    int64_t createdAtUnixMs() const noexcept;

    void setSlideId(const char* slideId);
    void setSceneIndex(int32_t sceneIndex);
    int32_t toJson(char* buffer, int32_t capacity) const;
};
```

**Thread-safety:** ThreadCompatible. **Requirements:** UR-RES-11.

Per-stage configuration is recorded against the stage by `workspace`, not here;
this is the slide-level record. UR-RES-11 requires both be recoverable from the
store alone, without the code that produced them.

**`Provenance` is write-side only.** `libraryVersion()` is the running
library's version, not a stored field, and `createdAtUnixMs()` is the time the
object was constructed; there is no setter for either. A `Provenance`
therefore cannot represent a record read back from a store written by another
version or at another time, and restoring those values on import is deferred
to `storage` (`03-storage.md` 3.2); until then, code reading a store treats the
persisted JSON as the source of truth. `toJson` writes compact JSON with the
keys `slideId`, `sceneIndex`, `libraryVersion`, `createdAtUnixMs` in that
order.

Because the creation time is taken at construction, **byte-identical export
(UR-QA-3) holds when the same `Provenance` instance is used** — or a copy,
which keeps the source's time. Two instances built a millisecond apart produce
two different files. The class has the full copy and move set; a moved-from
record reads slide id `""`, scene 0 and creation time 0, and a setter
re-initialises it.

## 1.14 GeoJSON codec — `core/geojson.hpp`

```cpp
class GeoJsonReader {
public:
    explicit GeoJsonReader(const char* path);
    GeoJsonReader(GeoJsonReader&& other) noexcept;
    GeoJsonReader& operator=(GeoJsonReader&& other) noexcept;
    GeoJsonReader(const GeoJsonReader&) = delete;
    GeoJsonReader& operator=(const GeoJsonReader&) = delete;
    ~GeoJsonReader();

    bool next();                          // advance; false at end
    const Polygon& geometry() const noexcept;
    const char* classification() const noexcept;  // "" if none
    const char* layerName() const noexcept;       // "" if none
    int32_t propertyCount() const noexcept;
    const char* propertyName(int32_t index) const;
    const char* propertyValue(int32_t index) const;
    int64_t featuresRead() const noexcept;
};

class GeoJsonWriter {
public:
    explicit GeoJsonWriter(const char* path);
    GeoJsonWriter(GeoJsonWriter&& other) noexcept;
    GeoJsonWriter& operator=(GeoJsonWriter&& other) noexcept;
    GeoJsonWriter(const GeoJsonWriter&) = delete;
    GeoJsonWriter& operator=(const GeoJsonWriter&) = delete;
    ~GeoJsonWriter();

    void begin(const Provenance& provenance);
    /// `labelNames` and `labelIsWorkspaceLocal` are parallel arrays of
    /// `labelCount` entries holding the object's effective label set
    /// (`03-storage.md` 3.7), in that set's fixed order. `labelCount == 0`
    /// for an object no stage has labelled.
    void writeFeature(const ObjectRecord& record,
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
objects stays within the memory ceiling (UR-RES-8). The reader pull-parses:
`next()` reads only as far as the end of the next feature, so its memory is
bounded by one feature whatever the file size. Both are move-only — each owns
an open file, and a copy would have two objects advancing one stream.

**What the reader accepts.** A FeatureCollection, a top-level array of
Features, or a single Feature. A Polygon's first ring is an outer ring and the
rest are holes; a MultiPolygon contributes each member polygon the same way.
The reader is locale-independent — coordinates parse the same under any C
locale — and skips a leading UTF-8 byte order mark. A plain integer coordinate
token that fits `int64_t` is kept exact, so values above 2^53 that the writer
emits round-trip; any other number is rounded to the nearest integer, halves
away from zero.

**What it skips, and what it throws.** A feature whose geometry is null,
missing, empty, or of any type other than Polygon or MultiPolygon is skipped
with a `LogLevel::Warning` message naming its index and type, because one
feature the library cannot represent should not stop the rest of the file
being read. Malformed JSON, or a top-level value that
is neither an object nor an array, throws `InvalidArgumentError` naming the
path and byte offset, and is final: every later `next()` throws again. A
feature with an invalid ring — fewer than 3 distinct points after rounding,
all points on one line, or wider than `int64_t` can span (1.3) — or with a
non-finite or out-of-range coordinate throws `InvalidArgumentError` naming the
feature, but leaves the reader usable: the next call continues with the
following feature. Properties are the scalar members of `properties` other
than `classification` and `layer`, then the scalar members of
`properties.measurements` as `measurements.<key>`; nulls,
arrays and nested objects are skipped. That includes the array form of
`measurements` that QuPath wrote before 0.3, which therefore yields no
measurement properties (`03-storage.md` 3.11).

**Holes get an owner on the way out.** `Polygon` deliberately has none (1.3),
but GeoJSON's MultiPolygon nests each hole inside one polygon. A polygon with
exactly one outer ring is written as a GeoJSON Polygon; any other is a
MultiPolygon in which each hole goes to the first outer ring, in ring order,
that contains the hole's first vertex (a vertex on the boundary counts as
inside), or to the first outer ring when none does. The rule is
deterministic, so it keeps exports byte-identical, and on well-formed input it
puts every hole where it belongs. Rings are written closed, with integer
coordinates.

A feature is built whole before any of it reaches the file, so a rejected one
leaves no trace. The writer's destructor never throws and does not complete
the document; after any write failure `end()` throws `IoError`, so a failed
export never looks successful.

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
(UR-QA-3) when both are given the same `Provenance` (1.13).

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
    virtual void write(LogLevel level, const char* message) = 0;
};
using LogSinkPtr = std::shared_ptr<LogSink>;

void setLogSink(LogSinkPtr sink) noexcept;
void setLogLevel(LogLevel level) noexcept;
LogLevel logLevel() noexcept;
void logMessage(LogLevel level, const char* format, ...)
    SLIDEIO_PROCESSOR_PRINTF_FORMAT(2, 3);
```

**Thread-safety:** the functions are ThreadSafe. `LogSink::write` may be
called concurrently from several threads, so an implementation must be
thread-safe. **Requirements:** none directly; supports UR-EXE-5 diagnostics.

`logMessage` carries `SLIDEIO_PROCESSOR_PRINTF_FORMAT(2, 3)`, which expands to
`__attribute__((format(printf, 2, 3)))` on GCC and Clang and to nothing
elsewhere, so a mismatched argument is a compile-time warning rather than
undefined behaviour at run time. Logging never fails its caller: a null
format, a formatting error, an allocation failure, a lock failure and any
exception the sink throws are all swallowed, and nothing is formatted below the
threshold, which defaults to `Warning`. The sink is called outside the
library's lock, so a sink may itself log. `setLogLevel` and `logLevel` are a
lock-free atomic and cannot fail. `setLogSink` swaps the sink under the
library's lock, and because it is `noexcept` a failure to acquire that lock is
fatal: `std::terminate` is called (C-6).

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
    /// an empty region, a region whose area, `x + width` or `y + height`
    /// overflows `int64_t`, a negative level, or a scale that is not finite
    /// or is below 1e-6.
    RasterMask(const Rect64& regionAtLevel, int32_t level, double levelScale);

    RasterMask(const RasterMask& other);
    RasterMask(RasterMask&& other) noexcept;
    RasterMask& operator=(const RasterMask& other);
    RasterMask& operator=(RasterMask&& other) noexcept;
    ~RasterMask();

    bool isNull() const noexcept;
    Rect64 regionAtLevel() const noexcept;
    int32_t level() const noexcept;
    double levelScale() const noexcept;

    /// Row `y` of the region, indexed from its top edge, `width` bytes long.
    const uint8_t* row(int64_t y) const;
    uint8_t* row(int64_t y);

    /// Level coordinates, absolute — not relative to the region. A point
    /// outside the region is false, never an error: a caller testing an
    /// object that strayed past the tile gets the right answer.
    bool test(Point64 levelPoint) const noexcept;

    /// The same question asked in level-0 slide coordinates, which is the
    /// space every emitted polygon and every anchor lives in (1.3).
    /// `slidePoint` is a corner-lattice POINT, not a pixel index: it answers
    /// "is the level pixel CONTAINING this slide point set", by
    /// `floor(v / levelScale)` per ordinate. Contrast `Polygon::contains`,
    /// which takes a PIXEL INDEX tested at its centre.
    bool testLevel0(Point64 slidePoint) const noexcept;

    void clear() noexcept;
    bool isEmpty() const noexcept;         // no pixel set
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

/// Widens the set region by `radius` level pixels, in place, with the square
/// structuring element of side `2 * radius + 1`. Separable and exact, and
/// confined to the mask's region; `radius` of 0 is a no-op and a negative
/// one throws.
void dilateMask(RasterMask& mask, int32_t radius);

/// Fills `polygon` into plane `planeIndex` of `plane` with `value`, for a
/// `TileRasterSource` building an auxiliary input (1.9). `polygon` is in
/// level-0 coordinates; the level scale is recovered from the view itself as
/// `sx = slideRect().width / width()` and `sy = slideRect().height / height()`,
/// so the mapping comes from integers the framework already agreed on rather
/// than from a stored float. The view's origin is rounded to the nearest
/// level pixel (see below).
///
/// Throws `InvalidArgumentError` when `planeIndex` is out of range, when
/// `value` is not exactly representable in the plane's `DataType`, when the
/// derived scale is not finite or is below 1e-6, or when the derived origin
/// does not fit. Uses the same conservative fill rule as the mask overload.
void rasteriseInto(BlockView& plane, int32_t planeIndex,
                   const Polygon& polygon, int64_t value);
```

**Thread-safety:** ThreadCompatible — one mask per tile, built and read by the
worker that owns it; a `const` mask may be read from many threads, and the
free functions are safe on distinct masks or views. **Requirements:**
UR-IMG-5, UR-STG-3, UR-QA-2, UR-QA-3.

The scale floor of 1e-6 is not a pyramid constraint — real scales are at
least 1 — but the bound that keeps every coordinate division finite. A
moved-from mask is null (C-8).

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
backend's polygon fill happens to do. Precisely: a pixel is set iff the
even-odd region has positive-area overlap with the pixel's square. A polygon
that only touches a pixel along an edge or at a corner does not set it.
Degenerate input — coincident edges that cancel under even-odd — may
over-include a pixel, which is the direction this rule is allowed to err in.
Because the test is coverage rather than the centre test of
`Polygon::contains`, the set pixels are a superset of the pixels `contains`
accepts, equal only for an axis-aligned polygon at level 0 (1.3).

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

The tile-independence is kept far from the origin too: the coverage kernel's
results do not depend on the polygon's distance from the origin (1.3), so
seams agree for coordinates above 2^53 as they do near zero.

`dilateMask` supplies the additional margin a coarse source layer needs. A
layer analysed at level 4 has a boundary good to ±16 level-0 pixels
(`03-storage.md` 3.3), and `makeLayerMaskSource` converts that half-step into
`ceil(halfStep / levelScale)` level pixels. For a `cells` layer written at
level 0 the half-step is 0 and the radius is 0 — the fill rule's sub-pixel
conservatism is the whole margin, which is the correct answer rather than a
missing one, because a level-0 boundary needs no slack.

The structuring element is the square of side `2r + 1`, applied as a
horizontal pass and then a vertical one. The square is chosen because it is
the separable element: two one-dimensional passes give exactly the
two-dimensional result, with no approximation of a disc. Dilation stays inside the
mask's region and never creates a pixel outside it, which is why
`makeLayerMaskSource` rasterises over the tile grown by the radius and crops
afterwards (1.9): an object just outside the tile that dilation brings into it
must already be in the scratch mask.

**Open question — dilation at coarse levels.** At a level whose scale exceeds
1, the radius is rounded up to whole level pixels, `ceil(d / levelScale)`, and
the element is a square. The widening actually applied, measured in level-0
pixels, is therefore `levelScale × ceil(d / levelScale)` along the axes and
more along the diagonals, and it depends on the level the mask is built at: the
same `dilationLevel0Pixels` widens a restriction more at level 4 than at
level 2. That errs in the conservative direction, but whether the
level-dependence is acceptable, or the radius should be fitted per level, is
not decided.

### The view overload works on a rounded grid

`rasteriseInto(BlockView&, …)` has no mask region to anchor it, so it derives
one. With `sx = slideRect().width / width()` and
`sy = slideRect().height / height()`, the view's pixel `(i, j)` is the absolute
level pixel `(originLevelX + i, originLevelY + j)`, where
`originLevelX = llround(slideRect().x / sx)` and likewise for `y`; vertices map
as `x / sx` with no origin term, so neighbouring views that derive the same
scale agree on a shared seam. A derived scale that is not finite or is below
1e-6 is rejected.

**The conservative guarantee holds on that rounded grid**, the grid of level
pixels starting at `(originLevelX, originLevelY)` with pixel size `(sx, sy)`.
When `slideRect().x / sx` (or `y / sy`) is not an integer, that grid sits up to
half a level pixel off the true level grid, so "covered" is exact for the
rounded grid and not for the true one. Integer slide rects over a non-integer
pyramid, such as 4.0001, can also derive slightly different scales per view,
and then their seams may differ: this overload does **not** keep such a
pyramid in register. How a view learns its exact level scale is an open item
for `pipeline` to revisit (`05-pipeline.md` 5.6), for example by having the
view carry its scale, or by guaranteeing slide rects whose division is exact.

`value` must be exactly representable in the plane's type: within the type's
range for integer types, and `|value|` at most 2048 for `Float16` (stored as
the IEEE half bit pattern), 2^24 for `Float32` and 2^53 for `Float64`. Every
run is computed before the first write, so a throw leaves the plane untouched,
and pixels the polygon does not cover are left unchanged.
