# SDS 03 — `storage`

**Module:** `storage` · **Library:** `slideio-processor-data` · **Depends on:** `core`, `config`

The store file and everything durable in it: objects, edges, layers, labels,
stage state, provenance, and the event journal. Implements `core`'s
`ObjectSink`, `ObjectSource` and `Cursor`.

This is the module a viewer process links without OpenCV (design 5.1), and the
largest public surface in the library.

Conventions in [00-conventions.md](00-conventions.md).

**The module ships in three parts.** The first delivers the store file, the
layer and vocabulary registries, objects, stage state and the journal (3.1–3.5,
3.9, 3.10, 3.10a). The second adds edges, annotations, GeoJSON import,
primitive-layer drop and the effective-label raster source (3.6, 3.7, 3.11);
the third adds rasters (3.8). `Store` declares only the accessors whose classes
exist, so `edges()`, `annotations()` and `rasters()` arrive with their parts.
**The v1 format is not frozen until the third part lands.** Each part adds its
tables to the same creation DDL and `format_version` stays 1 throughout; a
store written before then is a development artefact and nothing migrates it.
Freezing earlier would have meant designing the later tables before their
designs existed.

---

## 3.1 Enumerations — `storage/types.hpp`

```cpp
enum class OpenMode : int32_t {
    ReadOnly,        // no writer; safe alongside another process's writer
    ReadWrite,       // exclusive writer; readers may attach concurrently
    CreateNew        // creates the store; fails if the path exists
};

enum class LayerKind     : int32_t { Semantic, Primitive };
enum class LabelOrigin   : int32_t { Standard, WorkspaceLocal, ImportRegistered };
enum class ImportPolicy  : int32_t { Strict, Permissive };
enum class StageStatus   : int32_t { Pending, Running, Complete, Failed };

enum class EventKind : int32_t {
    LayerDeclared, ObjectsAdded, EdgesAsserted, ObjectsAnnotated,
    LabelAdded, StageStateChanged, StageDiscarded, LayerDropped,
    RasterDeclared, RasterWritten, RasterDropped
};
```

Every enumerator value is persisted in the store file, so none is ever
renumbered and new enumerators are appended only at the end. `EventKind`
already lists the kinds of all three parts, so the journal's encoding does not
change when edges and rasters arrive.

## 3.2 Store — `storage/store.hpp`

```cpp
class Store;
using StorePtr = std::shared_ptr<Store>;

class Store {
public:
    static StorePtr open(const char* path, OpenMode mode,
                         const Configuration& configuration);
    ~Store();                                  // calls close(); logs and swallows
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    // These four still answer after close().
    const char* path() const noexcept;
    int32_t     formatVersion() const noexcept;
    OpenMode    mode() const noexcept;
    bool        isReadOnly() const noexcept;

    Provenance provenance() const;
    void       setProvenance(const Provenance& provenance);

    EventJournal&             events() noexcept;
    LayerRegistry&            layers() noexcept;
    const LayerRegistry&      layers() const noexcept;
    VocabularyRegistry&       vocabulary() noexcept;
    const VocabularyRegistry& vocabulary() const noexcept;
    ObjectStore&              objects() noexcept;
    StageStateReader&         stageStates() noexcept;
    StageStateWriter&         stageStateWriter() noexcept;   // 3.10a

    // Added with their parts: EdgeStore& edges() noexcept (3.6),
    // AnnotationStore& annotations() noexcept (3.7),
    // RasterRegistry& rasters() noexcept (3.8).

    /// Flushes pending batches and releases every connection and the writer
    /// lock. Idempotent. Errors during teardown are reported here rather than
    /// in the destructor.
    void close();
};

int32_t storeFormatVersion() noexcept;   // storage/store_format.hpp; returns 1
```

**Thread-safety:** ThreadSafe. All methods callable from any thread with no
external locking. Sub-object references remain valid while the store lives and
usable until `close()`.
**Requirements:** UR-RES-12, UR-BLD-5, UR-OBS-1, UR-RES-11, decision 4.

`open` gates on the format version and throws `StoreFormatError` naming both
the store's version and the library's when they are incompatible, rather than
misreading the file (UR-RES-12). A file that is not a SQLite database, or has
no `meta` table, is refused the same way with store version 0.

**Modes.** `CreateNew` creates the file and throws `IoError` if the path
exists. `ReadWrite` and `ReadOnly` require an existing file and throw `IoError`
naming the path if it is missing. `mode()` returns the mode passed in, and
`isReadOnly()` is true only for `ReadOnly`. A null or empty path, or a value
outside `OpenMode`, throws `InvalidArgumentError`. Every mutating call on a
`ReadOnly` store throws `IoError` naming the path and the word `read-only`.

**After `close()`**, `path()`, `formatVersion()`, `mode()` and `isReadOnly()`
still answer, and every throwing member throws `IoError` containing `store is
closed`. **The `noexcept` members of the registries are the exception**:
`count()`, `find()`, `exists()`, `isDropped()`, `isRecognised()`,
`coarsestAnalysisLevel()`, `recordedStandardVersion()` and `importPolicy()`
cannot throw, so on a closed store, or when SQLite fails, they return the
documented fallback — 0, `false` or the default — instead. Each header states
its fallback. Dropping `noexcept` instead would have changed every one of these
signatures for a state a caller reaches only by misuse.

**`close()` flushes live writers before it closes.** Every live
`ObjectWriter` commits what it holds pending, as by its own `flush()`; every
writer is tried and the first failure is then reported, and the store is closed
either way. From the moment `close()` begins, `ObjectWriter::emit` is refused
with `IoError` "store is closed" rather than accepted and silently lost behind
the final flush. A writer or cursor used afterwards throws `IoError`. The
destructor calls `close()`, logs any error at `Error`, and swallows it.

**`provenance()` returns the stored record, not the reader's.** `meta`
holds `Provenance::toJson`, written at creation from a default `Provenance()`
— so it carries the creating library's version and the creation time — and
replaced by `setProvenance`. `provenance()` parses it and returns
`Provenance::restored(...)` (`01-core.md` 1.13), so the library version and
creation time are the persisted ones rather than the reading library's and the
time of the read (UR-RES-11). An unreadable record throws `InternalError`
naming the store path, the parse offset and the record size, but never the
record's text, which carries the slide identifier.

**Connections.** WAL mode, `synchronous=NORMAL`, `foreign_keys=ON`,
`busy_timeout=5000`. There is one write connection, guarded by a mutex, and
every write transaction runs on it. Read connections come from a pool: a point
read checks one out for the call, and an object cursor checks one out for its
lifetime and holds one read transaction, so it sees a single snapshot and never
a partially applied batch. That is what makes UR-OBS-1 achievable: a `ReadOnly`
store in a viewer process reads concurrently with a `ReadWrite` store in a
script process, neither blocking the other. SQLite is linked privately and
never exported; on Linux its symbols are hidden from the shared library, so it
cannot interpose on the copy SlideIO's converter carries, or be interposed by it.

### The writer lock

`ReadWrite` and `CreateNew` take an exclusive writer lock: a file at
`<path>-writer.lock` holding the owner's process id as decimal text, locked
with `LockFileEx` on Windows and `flock(LOCK_EX | LOCK_NB)` on POSIX, and held
until `close()`. A second writer throws `IoError` naming the path and, when
the lock file's text parses, `process <pid>` — concurrent writers remain out of
scope (Assumption 6, §15). Both platforms lock per handle, so a second
`ReadWrite` open in the same process fails the same way. `ReadOnly` takes no
lock.

- **A lock counts only on the file the path still names.** A handle opened
  just before another holder removed its lock file could otherwise lock the
  removed file while a later opener locks a new one. `open` therefore checks
  the locked file's identity (inode, or volume serial and file index) against
  the path and retries, up to eight times; a refusal on a file the path no
  longer names is retried the same way rather than reported as a writer.
- **The lock file is not removed on a normal close.** Removing it would race
  with a process that has just opened it. Only a failed `CreateNew` removes
  its own lock file, while still holding the lock.
- **`CreateNew` racing another creator reports "already exists"** and leaves
  that store and its lock file untouched. Whichever creator takes the lock
  rechecks the path; a creator that finds the store already made, or cannot
  inspect the path, never removes files, because they may be the other
  creator's store.
- **Stores on network shares are not supported for writing.** The identity
  check relies on local-filesystem file identity. On a Windows volume without
  POSIX delete semantics a removed lock file stays delete-pending; reopening it
  fails and `open` throws, which fails safe and never yields two writers.

**Error messages name the store path**, as C-6 requires, and paths therefore
appear in exceptions and logs. Callers should not put patient identifiers in
store paths or file names, since anything there reaches error reports and log
files (HIPAA, GDPR).

## 3.3 Layer registry — `storage/layer_registry.hpp`

```cpp
class LayerRegistry {
public:
    ~LayerRegistry();
    LayerRegistry(const LayerRegistry&) = delete;
    LayerRegistry& operator=(const LayerRegistry&) = delete;

    /// Declares a semantic layer beneath `parent`. Pass pixelLayer() for a
    /// layer rooted directly at the pixel layer. Branching is permitted.
    LayerId declareSemantic(const char* name, LayerId parent);
    LayerId declarePrimitive(const char* name);

    LayerId pixelLayer() const noexcept;                 // LayerId(1)

    int32_t count() const noexcept;
    LayerId at(int32_t index) const;
    bool    find(const char* name, LayerId& out) const noexcept;

    const char* name(LayerId layer) const;
    LayerKind   kind(LayerId layer) const;
    bool        exists(LayerId layer) const noexcept;

    // Semantic hierarchy. Throw InvalidArgumentError for primitive layers.
    LayerId parent(LayerId layer) const;
    int32_t depth(LayerId layer) const;
    int32_t childCount(LayerId layer) const;
    LayerId childAt(LayerId layer, int32_t index) const;

    /// True if `ancestor` is a proper ancestor of `descendant` — that is, if
    /// `descendant` lies strictly above `ancestor` in the hierarchy, at any
    /// distance. The predicate UR-RES-14 edges are validated against.
    bool isAbove(LayerId descendant, LayerId ancestor) const;

    bool isDropped(LayerId layer) const noexcept;

    /// Stages declared to write this layer, queryable before any stage runs.
    int32_t writerStageCount(LayerId layer) const;
    StageId writerStageAt(LayerId layer, int32_t index) const;

    /// The coarsest pyramid level any stage used to write this layer; 0 for a
    /// layer written only at full resolution, by import, or not yet written.
    /// Geometry in this layer carries level-0 coordinates whose precision is
    /// that of this level.
    int32_t coarsestAnalysisLevel(LayerId layer) const noexcept;

    /// Records the level a stage analysed at. Called by the framework when a
    /// writing stage completes; keeps the maximum over all writers.
    void recordAnalysisLevel(LayerId layer, int32_t level);
};
```

**Thread-safety:** ThreadSafe; declarations are serialised with every other
write. **Requirements:** UR-STG-10, UR-RES-13, UR-RES-14, UR-STG-12, UR-SEG-4,
UR-IMG-4.

**Ids and names.** `pixelLayer()` is `LayerId(1)`, named `pixel`, semantic,
depth 0, with an invalid parent; it exists from the moment the store is
created. Declared layers number from 2 upward in declaration order, and `at`
lists them in ascending id. Names are unique across all layers and compared
exactly, with no case folding. A null or empty name, a duplicate, or `pixel`
throws `InvalidArgumentError` naming it, and `declareSemantic` requires an
existing, semantic, undropped parent. `at`, `name`, `kind` and `parent` on an
unknown id throw `InvalidArgumentError`, and so does `isAbove` when either
layer is primitive or unknown. `writerStageAt` lists stages in ascending id.
A `ReadOnly` store sees layers a writer in another process declares later.

`isDropped` reads a stored flag. Nothing drops a layer until primitive-layer
drop arrives with edges (3.6), so until then layers are never removed and ids
and positions are stable.

Semantic layers form a branching tree rooted at the pixel layer — `vessels` and
`glands` may both sit above `tissue` without relating to each other.
`declarePrimitive` takes a name alone: primitive layers are arbitrary, any
number, in no order, outside the hierarchy.

`declareSemantic` and `declarePrimitive` append a `LayerDeclared` event to the
journal in the same transaction, so a subscribed viewer learns of a new layer
as it is declared (UR-OBS-5).

**`coarsestAnalysisLevel` is how a consumer learns what a boundary is worth.**
UR-SEG-4 puts every polygon in level-0 coordinates, and UR-IMG-4 requires
reading from the coarsest level that satisfies the request — so a `tissue`
layer detected at level 4 stores level-0 integers whose true precision is ±16
level-0 pixels. The coordinates look exact and are not, and nothing else in the
store says so. The level was always *recoverable* — it sits in the operation's
parameter bag, inside the `PipelineSpec`, inside the stage's configuration JSON
— but a viewer asking "how sharp is this outline?" should not have to parse a
stage record to find out, and the mask dilation of `04-workspace.md` 4.4 needs
the number directly.

It is the **maximum** over all writers, not the last one: a layer filled by a
level-4 detection stage and by a hand-drawn import is only as precise as its
coarsest contributor, and a bound that is sometimes optimistic is worse than
no bound. Levels are recorded on stage completion, so the value is 0 until
something has actually run — which is correct, not a placeholder, since an
empty layer has no geometry to qualify.

This records precision; it does not improve it. A level-4 contour mapped to
level 0 is a staircase with 16-pixel treads, and no later stage can recover
what was never sampled.

`recordAnalysisLevel` throws `InvalidArgumentError` for a negative level or an
unknown layer. `coarsestAnalysisLevel` of an unknown layer is 0, like that of
an unwritten one.

Writing an object to an undeclared layer throws `InvalidArgumentError` naming
the layer, per UR-STG-10's acceptance criterion.

`isAbove` is the single place the containment ordering is interpreted. Putting
it here rather than in `workspace` is deliberate: the rule constrains edges the
store must reject no matter who writes them, including GeoJSON import and any
future external tool, so it lives with the data.

## 3.4 Vocabulary registry — `storage/vocabulary_registry.hpp`

```cpp
class VocabularyRegistry {
public:
    ~VocabularyRegistry();
    VocabularyRegistry(const VocabularyRegistry&) = delete;
    VocabularyRegistry& operator=(const VocabularyRegistry&) = delete;

    /// The standard vocabulary version this store was written against, which
    /// may be older or newer than the linked library's. Fixed at creation.
    int32_t recordedStandardVersion() const noexcept;

    /// Adds a workspace-local label. Legal at any time, including after
    /// stages have completed. Never requires a format migration.
    LabelId addLocal(const char* name);

    bool        find(const char* name, LabelId& out) const noexcept;
    const char* name(LabelId label) const;
    LabelOrigin origin(LabelId label) const;

    /// False for a label from a newer standard vocabulary than this library
    /// knows. Its identifier and string are still returned intact.
    bool isRecognised(LabelId label) const noexcept;

    int32_t count() const noexcept;
    LabelId at(int32_t index) const;

    ImportPolicy importPolicy() const noexcept;          // default Strict
    void         setImportPolicy(ImportPolicy policy);
};
```

**Thread-safety:** ThreadSafe; additions are serialised with every other
write. **Requirements:** UR-RES-17, UR-RES-18, UR-RES-19.

**Ids.** A store is created holding the ten standard labels, ids 1 to 10.
**Ids 1 to 65535 are reserved for the standard vocabulary, and workspace-local
labels number from 65536 upward**, so a later standard version can append ids
without colliding with a local id already written. `at` lists labels in
ascending id — standard, then local. The store's own table is authoritative for
`name()`: a label of a newer standard version keeps the identifier and string
it was written with.

**`isRecognised` is computed, not stored.** It is false for an id in 1..65535
above the reading library's `StandardVocabulary::labelCount()`, and true for
every other held label. A stored flag would be wrong the moment a newer library
opened the store, so the `labels` table has none (3.12).

Labels are stored as integer identifiers, never as strings on the object row —
UR-RES-17 requires the per-object cost of a label not scale with the length of
the label string, and its acceptance test doubles every label string and
asserts store size is substantially unchanged.

`isRecognised` returning `false` is not an error. A store written against a
newer vocabulary reports its unknown labels as unrecognised with identifier and
string intact, rather than dropping or renaming them (UR-RES-17).

The contents of the standard vocabulary are in `02-config.md` 2.2 — ten
labels in v1, resolving open question 8. This registry holds the *effective*
vocabulary of one store: those ten, plus whatever `addLocal` and import have
added to it.

**`find` matches case-insensitively, and that is load-bearing rather than
convenient.** The standard vocabulary is spelled in British English, as the
requirements are — `tumour` — while QuPath emits `Tumor`. UR-RES-8's acceptance
criterion is a QuPath round trip, so a case- and spelling-sensitive lookup
would send an ordinary exported file down the `Strict` rejection path for a
reason that has nothing to do with the user's data. Matching folds ASCII case
and then consults a fixed alias table shipped with the vocabulary —
`tumor`→`tumour`, `artifact`→`artefact`, `immune cells`→`immune`,
`ignore*`→`ignore`, where the `*` is an ordinary character, not a wildcard.
`name()` always returns the canonical spelling, so nothing downstream sees the
variant. A null or empty name is not found.

`addLocal` returns the existing id, adding no row and no event, when `find`
already resolves the name, aliases included. Otherwise it stores the name as
given with origin `WorkspaceLocal` and appends a `LabelAdded` event carrying
the label id, in the same transaction. A null or empty name throws
`InvalidArgumentError`. It does not touch any previously written object, which
is what lets a label added after the `nuclei` stage completed be applied by a
later stage with the existing nuclei unchanged (UR-RES-18).

`ImportPolicy` governs GeoJSON import only (3.11). It defaults to `Strict`,
persists in `meta`, and is read from the store on every call, so a change made
through another handle is seen. Both the policy in force and its outcome are
recoverable from the store, per UR-RES-19.

## 3.5 Object store and writer — `storage/object_store.hpp`

```cpp
class ObjectWriter;
using ObjectWriterPtr = std::shared_ptr<ObjectWriter>;

class ObjectWriter : public ObjectSink {
public:
    ~ObjectWriter() override;                  // flushes; logs and swallows
    ObjectWriter(const ObjectWriter&) = delete;
    ObjectWriter& operator=(const ObjectWriter&) = delete;

    /// Writes the record with this writer's `stage()` as its provenance. The
    /// record's own `stage()` is not read and cannot override it; the layer,
    /// geometry, label and measurements come from the record.
    void emit(const ObjectRecord& record) override;
    void flush() override;
    int64_t emittedCount() const override;

    int64_t batchSizeObjects() const noexcept;
    void    setBatchSizeObjects(int64_t size);

    /// The stage every object emitted through this writer is attributed to.
    StageId stage() const noexcept;
};

class ObjectStore : public ObjectSource {
public:
    ~ObjectStore() override;
    ObjectStore(const ObjectStore&) = delete;
    ObjectStore& operator=(const ObjectStore&) = delete;

    /// A writer that attributes everything it emits to `stage`. Any number may
    /// be held concurrently, from any threads; writes are serialised internally
    /// into batched transactions.
    ObjectWriterPtr writerForStage(StageId stage);

    CursorPtr query(const ObjectQuery& query) override;
    int64_t   count(const ObjectQuery& query) const override;
    bool      get(ObjectId id, ObjectRecord& out) const override;

    /// Removes every object a stage wrote. Used by re-run and by failure
    /// handling; appends StageDiscarded.
    void discardStage(StageId stage);

    int64_t totalObjectCount() const;
    int64_t objectCountInLayer(LayerId layer) const;
};
```

**Thread-safety:** `ObjectStore` is ThreadSafe and a returned `CursorPtr` is
ThreadConfined. `ObjectWriter` is ThreadSafe for `emit`, `emittedCount`, the
batch-size pair and `stage`; `flush` requires exclusive access — the split
`EdgeWriter` and `AnnotationWriter` have (3.6, 3.7). Any number of writers may
be used concurrently; their commits are serialised.
**Requirements:** UR-RES-1, UR-RES-2, UR-RES-3, UR-EXE-7, UR-STG-5, UR-STG-6,
UR-OBS-2, UR-OBS-4, UR-QA-3, decisions 4, 5, 6, 8.

**The write path is the whole point of this class.** One `emit` call inserts
nothing by itself. Objects accumulate until `batchSizeObjects` is reached, then
one transaction inserts the object rows, the R\*Tree rows, and a single
`ObjectsAdded` journal row — together, or not at all:

```
BEGIN
  INSERT INTO objects      ... (batch)
  INSERT INTO object_rtree ... (batch)
  INSERT INTO measurements ... (batch)
  INSERT INTO events       ... (one row per distinct layer: layer, stage,
                                count, union bbox; ascending layer id)
COMMIT
```

Objects are inserted in arrival order; 3.12 measured per-batch sorting at 18%
slower. The batch size defaults to `Configuration::objectBatchSize()` and
`setBatchSizeObjects` (at least 1) takes effect at the next `emit`.
`emittedCount()` counts accepted records, pending and committed; a batch
dropped at commit no longer counts. The destructor flushes, logs a failure at
`Error`, and swallows it.

That atomicity is what makes the journal usable as a source of truth (UR-OBS-2):
a subscriber can never see an event for objects that are not yet queryable, and
objects can never appear unannounced. There is no separate publish step that a
crash can interrupt.

Writers do not need to be ordered, only serialised. Object identifiers come
from `ObjectRecord::id()`, which derives from the geometry's anchor — so a
worker computes its own identifiers and hands finished records to the writer
whenever it is ready. No reorder buffer, no scan-order frontier, no head-of-line
stalling, and identifiers identical across worker counts and tile sizes
(decision 8, UR-QA-3).

`emit` validates each record and throws `InvalidArgumentError`, adding
nothing, for: an undeclared or dropped layer, or the pixel layer; empty
geometry; an anchor outside the identifiable range (the message names the
anchor); a label the store does not hold; and an anchor already pending in the
same writer's batch for the same layer. `writerForStage` throws
`InvalidArgumentError` for an unknown stage and `IoError` on a `ReadOnly`
store.

**Objects within one layer may not overlap at their anchor, and `emit`
enforces it.** An anchor is the topmost-then-leftmost pixel of a shape
(decision 8), so two objects in one layer sharing that pixel derive the same
`ObjectId` — which is not a near-miss but a collision of the identity the
entire re-run, provenance and export story rests on. `emit` therefore throws
`InvalidArgumentError` naming both the layer and the anchor rather than
silently replacing, dropping or duplicating. A collision inside one writer's
pending batch is caught at `emit`. One with a committed row — from this
writer, another writer or another stage — is caught at commit by the
`UNIQUE(layer_id, anchor_id)` index: the whole batch is rolled back, its
pending objects are dropped, and the `emit` or `flush` that triggered the
commit throws. No partial batch is ever committed.

This is stated because nothing previously stated it and a benchmark found it:
a spike run placing ten million anchors at random produced an 0.81%
primary-key collision rate. That *rate* is an artifact — random placement
manufactures coincidences a real segmentation does not — but the invariant it
exposed is real, and no requirement forbids overlapping objects. A caller that
genuinely needs overlapping shapes puts them in separate layers, which costs a
declaration and preserves every identity guarantee.

**The writer owns the stage; the record owns everything else.** `stage` is the
one column a producer cannot supply: `emit` takes it from `stage()` and ignores
whatever the record holds, which is why `ObjectRecord` has no `setStage`
(`01-core.md` 1.6). Attribution is therefore a property of the handle a caller
was given rather than a value it fills in, and three guarantees rest on that
being unforgeable rather than merely conventional:

- `discardStage` and `discardEdgesOfStage` delete by stage. A record able to
  misattribute itself would let one stage's re-run delete another's objects, or
  survive its own (UR-STG-6).
- `StageVisibility::CompletedOnly` filters by the producing stage's status. A
  wrong stamp makes an in-flight object visible to a later stage, breaking
  UR-STG-5 by the query filter that is supposed to enforce it.
- The provenance UR-STG-4 records would answer "which stage produced this
  object" with whatever the producer asserted.

`GeoJsonImporter` (3.11) takes its `StageId` the same way and for the same
reason — imported objects are attributed to the import, not to whatever a file
claims.

An object read from the store and emitted again is attributed to the emitting
stage, not the original one. That is the required behaviour, not a caveat: the
new row is this stage's output and must be discarded with it.

**The read side is stamped through `ObjectRecord::restored`.** `core` exposes
no setter for the stage field (`01-core.md` 1.6); `Cursor::current()` and
`ObjectStore::get` deliver `ObjectRecord::restored(record, stage)`, a copy
whose `stage()` is the producing stage. That is not a setter in disguise:
`emit` ignores a record's stage, so a producer that calls `restored` with
another stage's id still writes under its own writer's stage, and the
two-sources-of-truth problem 1.6 closes stays closed.

`query` honours the `StageVisibility` of the query: `CompletedOnly` — the
default — admits objects whose stage status is `Complete`, stale or not, so a
later stage never sees another's partial output and UR-STG-5 holds by query
filter rather than by rollback. `IncludeInFlight` is the observer's opt-in
(UR-OBS-4). **`get` honours `CompletedOnly` too** — it is the default view, so
UR-STG-5 holds for point reads; an observer wanting an in-flight object uses
`query` with `IncludeInFlight`.

**Query semantics.** Results come in ascending `ObjectId` order — by layer,
then packed anchor — whatever the arrival order (UR-QA-3). A cursor holds one
read transaction for its lifetime, so it sees the snapshot taken by `query`:
objects committed later are not returned, and no batch is ever seen in part.
`Cursor::position()` is the zero-based index of the current record.

- A **rectangle** filter admits an object whose bounding box shares at least
  one pixel with the rectangle, edges half-open. The R\*Tree is SQLite's float
  `rtree`, which rounds outward, so it yields no false negatives even for
  coordinates beyond float precision; every candidate is then rechecked
  against the exact `int64` bounding box held in `objects`.
- A **polygon** filter tests the object's exact geometry in C++ on 64-bit
  integer coordinates. `Intersects` admits an object whose closed region meets
  the polygon's, touching included; `ContainedBy` admits one whose closed
  region lies inside or on the polygon's. Both are exact and implemented
  in-house — no clipping library, since a new third-party dependency in the
  viewer library would be a design change of its own.
- **Measurement bounds** test the producing stage's `measurements`; an object
  without the named measurement fails the bound. **Label filters** test the
  row label, which is the whole effective set until annotations arrive (3.7),
  and **attachment** filters see no parent edge until edges arrive (3.6), so
  `AttachedOnly` matches nothing and `UnattachedOnly` everything. Both become
  joins when those tables exist.
- Polygon filters and `limit` are applied in the cursor loop, after the exact
  checks, so `limit` counts records actually returned.

`totalObjectCount` and `objectCountInLayer` count every stored row, whatever
its stage's status; the latter is 0 for an unknown layer. `discardStage`
removes a stage's objects and their measurements and appends a
`StageDiscarded` event carrying the number removed, in one transaction.

No method returns all objects at once. UR-RES-6 permits materialisation only as
an explicit, filter-bounded call, and it is provided by `reporting`.

## 3.6 Edge store — `storage/edge_store.hpp`

```cpp
class EdgeWriter;
using EdgeWriterPtr = std::shared_ptr<EdgeWriter>;

class EdgeWriter : public EdgeSink {
public:
    ~EdgeWriter() override;

    /// Asserts that `child` is contained by `parent`. Both must lie in
    /// semantic layers, with parent strictly lower. Neither object is
    /// modified. Legal in either creation order.
    void assertParent(ObjectId child, ObjectId parent) override;

    /// Many-to-many, optional, valid when empty. One primitive may contribute
    /// to several objects and one object may be built from many primitives.
    void assertDerivedFrom(ObjectId derived, ObjectId primitive) override;

    void flush() override;
    int64_t assertedCount() const override;

    int64_t batchSizeEdges() const noexcept;
    void    setBatchSizeEdges(int64_t);

    /// The stage every edge asserted through this writer is attributed to.
    StageId stage() const noexcept;
};

class EdgeStore {
public:
    /// A writer that attributes every edge it asserts to `stage`. Any number
    /// may be held concurrently, from any threads; writes are serialised
    /// internally into batched transactions.
    EdgeWriterPtr writerForStage(StageId);

    bool      parentOf(ObjectId child, ObjectId& out) const;
    CursorPtr childrenOf(ObjectId parent) const;
    bool      isAttached(ObjectId child) const;
    CursorPtr originsOf(ObjectId derived) const;
    CursorPtr derivedFrom(ObjectId primitive) const;

    /// Removes only the edges a stage asserted. Objects are untouched,
    /// including objects of other stages that this stage had attached.
    void discardEdgesOfStage(StageId);

    int64_t parentEdgeCount() const;
    int64_t derivedFromEdgeCount() const;
};
```

**Thread-safety:** `EdgeStore` is ThreadSafe; `EdgeWriter` is ThreadSafe for
`assertParent` and `assertDerivedFrom`, and `flush` requires exclusive access.
**Requirements:** UR-RES-5, UR-RES-14, UR-RES-16, UR-STG-4, UR-STG-6.

Edges are first-class records of the form *(child, parent, asserting stage)*,
never a property of the object. That is what lets a `nuclei` stage run before
any cell exists, writing unattached nuclei, and a later `cells` stage assert the
nucleus-to-cell edges without rewriting the nuclei (UR-RES-5).

**The writer owns the asserting stage**, exactly as `ObjectWriter` owns the
producing stage (3.5). An earlier draft put `assertParent` and
`assertDerivedFrom` on `EdgeStore` with a trailing `StageId assertingStage`
argument, which left the edge path with the forgeable attribution the object
path had already given up: `discardEdgesOfStage` deletes by that field, so a
caller passing another stage's id could delete edges belonging to a stage it
does not own, or leave its own behind for a re-run to duplicate (UR-STG-6).
Attribution is now a property of the handle in both stores, and there is no
overload that takes a stage.

`EdgeWriter` batches the way `ObjectWriter` does — edges accumulate until
`batchSizeEdges`, then one transaction inserts the edge rows and a single
`EdgesAsserted` journal row, together or not at all. The default comes from
`Configuration::objectBatchSize()`; edges are small and arrive at a comparable
rate, so a separate configuration key would be a knob nobody could reason
about. A caller who has a reason can still set it per writer.

**Ordering against the object writer matters, and did not before.** An edge's
endpoints must be committed before the edge is, or a crash between the two
batches leaves an edge pointing at nothing. `flush` therefore verifies both
endpoints exist and throws `InvalidArgumentError` naming the missing one, and a
stage emitting objects and edges in the same pass flushes its `ObjectWriter`
first — `StageRunner` does this (`04-workspace.md` 4.4). This is the one cost of
batching edges rather than writing them individually, and it is worth paying:
the reference object count implies an edge count of the same order, and a
transaction each would dominate the run.

`assertParent` throws `InvalidArgumentError` naming both layers when the parent
is in the same or a higher layer, or when either endpoint is in a primitive
layer — a primitive layer has no position in the hierarchy (UR-RES-14). The
parent need not be in the immediately adjacent layer: attaching a nucleus
directly to a tissue region succeeds when the `cells` layer is skipped.

**Containment is asserted, never derived.** The only checks are the layer-order
checks above: no component performs a point-in-polygon test to validate, infer,
or reject a parent edge, and none may be added. The hierarchy is what a stage
asserted, and the geometry is evidence for it rather than the definition of it.

This is not a shortcut, it is the only rule that survives multi-level analysis.
A `tissue` layer detected at level 4 has a boundary good to ±16 level-0 pixels
(3.3), while nuclei segmented at level 0 have boundaries good to one. Near the
margin the two disagree by construction, and a nucleus genuinely inside the
tissue can sit outside the mapped tissue polygon. Deriving containment
geometrically would make membership depend on which level each layer happened
to be analysed at — and would silently reclassify objects when a stage is
re-run at a different level, while their edges said otherwise.

A consumer is free to run its own point-in-polygon test and is warned that it
may legitimately disagree with the hierarchy; `coarsestAnalysisLevel` on both
layers is what tells it by how much. Where the two differ, **the edge is
authoritative** — for queries, for `childrenOf`, for export, and for every
count the library reports.

**A processing restriction is not an exception to this.** A stage restricted to
a layer is masked by that layer's geometry (`05-pipeline.md` 5.5), which is a
geometric test and looks at first like the thing this rule forbids. It is a
different question: the mask decides *what gets computed*, and leaves the
hierarchy to whatever the stage then asserts. An object the mask admitted has
no parent edge by virtue of having been admitted, and an object inside a
region it was never restricted to is not thereby outside anything. The rule
above governs edges; nothing governs edges but assertion.

An object with no parent edge is **unattached**, which is a valid state at any
layer, not an inconsistency, and `ObjectQuery::attachment(UnattachedOnly)`
queries for it.

`derivedFrom` is deliberately separate from parent edges and survives its
primitive layer being dropped: after the drop, the derived objects remain
complete and queryable and report no origins (UR-RES-16, UR-STG-12).

Layer validation happens at `assertParent`, not at `flush`, so a
hierarchy violation is reported at the call that made it rather than hundreds
of edges later. Endpoint existence is the opposite case and is necessarily
deferred: the object it names may still be sitting in an unflushed object
batch, so checking it eagerly would reject edges that are perfectly valid by
the time they commit.

## 3.7 Annotation store — `storage/annotation_store.hpp`

```cpp
class AnnotationWriter;
using AnnotationWriterPtr = std::shared_ptr<AnnotationWriter>;

class AnnotationWriter : public AnnotationSink {
public:
    ~AnnotationWriter() override;

    /// Attaches `label` to an object that already exists, attributed to this
    /// writer's `stage()`. The object row is not touched: the label it
    /// carries, if any, remains the producing stage's own assertion
    /// (`01-core.md` 1.6). Idempotent — the same stage asserting the same
    /// label on the same object twice is one row, not two.
    void assertLabel(ObjectId, LabelId) override;

    /// Attaches a named measurement, attributed to this writer's `stage()`.
    /// Idempotent for an identical value; asserting the same name twice with
    /// different values throws `InvalidArgumentError` rather than letting
    /// arrival order decide (UR-QA-3).
    void assertMeasurement(ObjectId, const char* name, double value,
                           MeasurementUnit) override;

    void flush() override;
    int64_t assertedCount() const override;

    int64_t batchSizeAnnotations() const noexcept;
    void    setBatchSizeAnnotations(int64_t);

    /// The stage every annotation asserted through this writer is attributed to.
    StageId stage() const noexcept;
};

class AnnotationStore {
public:
    /// A writer that attributes every annotation it asserts to `stage`. Any
    /// number may be held concurrently, from any threads; writes are
    /// serialised internally into batched transactions.
    AnnotationWriterPtr writerForStage(StageId);

    /// The object's **effective label set**: the producing stage's label from
    /// the object row first, when it has one, then every asserted label in
    /// ascending `LabelId` order. The order is fixed rather than insertion
    /// order because annotations arrive from workers in no particular
    /// sequence and exports must be byte-identical across runs (UR-QA-3).
    int32_t labelCount(ObjectId) const;
    LabelId labelAt(ObjectId, int32_t index) const;

    /// Who asserted the label at `index` — the producing stage for the row
    /// label, the annotating stage for the rest.
    StageId labelAssertedBy(ObjectId, int32_t index) const;

    /// Measurements asserted by stages other than the producer. The
    /// producer's own stay on the record (`01-core.md` 1.6); these are the
    /// additions, in ascending name order.
    int32_t     assertedMeasurementCount(ObjectId) const;
    Measurement assertedMeasurementAt(ObjectId, int32_t index) const;
    StageId     measurementAssertedBy(ObjectId, int32_t index) const;

    /// Resolves the one thing the effective sets leave ambiguous: two stages
    /// asserting the same measurement name on one object. False when that
    /// stage asserted no measurement of that name.
    bool assertedMeasurement(ObjectId, const char* name, StageId,
                             Measurement& out) const;

    /// Removes only the annotations a stage asserted. Objects are untouched,
    /// including the objects of other stages that this stage had annotated.
    void discardAnnotationsOfStage(StageId);

    int64_t labelAnnotationCount() const;
    int64_t measurementAnnotationCount() const;
};
```

**Thread-safety:** `AnnotationStore` is ThreadSafe; `AnnotationWriter` is
ThreadSafe for `assertLabel` and `assertMeasurement`, and `flush` requires
exclusive access — exactly `EdgeWriter`'s split, for exactly its reason.
**Requirements:** UR-RES-4, UR-RES-17, UR-RES-18, UR-STG-4, UR-STG-6, UR-QA-3.

**An annotation is an assertion about an object, not a part of it.** This is
the same move 3.6 makes for containment, applied to classification: a record of
the form *(object, asserting stage, claim)*, stored separately, deleted by the
stage that made it. It exists because the design had no way to express the
commonest workflow in the domain — detect cells in one stage, decide what they
are in another. An object row has one `stage` column, which conflates *who
found this shape* with *who said something about it*; as long as a label lived
on that row, the second stage had nowhere to put its answer.

**The writer owns the asserting stage**, as `ObjectWriter` and `EdgeWriter` do
(3.5, 3.6). There is no overload taking a `StageId`, and for the same reason:
`discardAnnotationsOfStage` deletes by that field, so a forgeable attribution
would let one stage delete another's work or leave its own behind for a re-run
to duplicate (UR-STG-6).

**The object row keeps its label, and that is not a half-measure.** A stage
that detects and classifies in one pass — most of them — should not write two
rows to say one thing, and the label it sets is already correctly attributed,
because the producing stage *is* the asserting stage. The row label is
therefore an annotation stored inline for the case where the two stages
coincide. Nothing competes: the effective label set is the row label, when
present, together with the asserted ones, and no precedence rule is needed
because none of them overrides another. What changed is the cardinality a
*consumer* sees — `hasLabel()` on a record still answers for the producer
alone, and the effective set is what queries and exports use.

**The effective set must reach raster sources too.** `core`'s
`makeLayerRasterSource` with `LayerRasterContent::Label` writes each record's
own row label, because an `ObjectSource` delivers `ObjectRecord`s and the
effective set is not reachable through that interface (`01-core.md` 1.9). A
label asserted by annotation therefore never appears in a label raster built
from a store. `storage` must close that gap — by a store-backed raster source
that reads `AnnotationStore`, or by an extension of `ObjectSource` that carries
the effective set — and must keep the rule that the smallest `ObjectId` wins
where objects overlap, and a fixed choice among an object's several labels,
so the raster stays identical across runs (UR-QA-3).

**Two deletes, and they are deliberately not symmetric.** `discardStage`
(3.5) removes a stage's objects, and annotations on a removed object go with
it: the claim has no subject any more. `discardAnnotationsOfStage` removes a
stage's claims and leaves every object in place. A re-run of an annotating
stage takes the second path and disturbs nothing it did not write. A re-run of
a *producing* stage takes the first, which destroys the annotations downstream
stages had attached — and that is correct rather than a loss, because those
stages read the layer and are therefore already stale by the `LayerRead` path
(`04-workspace.md` 4.5), so each must re-run and re-assert. The alternative —
orphaned annotations waiting for identical geometry to reappear — would make a
stale annotation indistinguishable from a current one.

**Validation mirrors `emit`.** `assertLabel` throws `InvalidArgumentError` for
an unknown label and for an object that does not exist; the latter is a foreign
key on `(layer_id, anchor_id)`, so it costs an indexed lookup inside the batch
transaction rather than a separate round trip. A stage may only annotate layers
it declared with `annotatesLayer` (`04-workspace.md` 4.2), checked by
`StageRunner` before the run so the failure arrives before the work does — the
same two-place treatment `writesLayer` already gets.

**Visibility follows the asserting stage, not the object's.** An annotation
becomes queryable under the rules of 3.5: `StageVisibility::CompletedOnly` by
default, so a later stage never sees a running annotator's partial output
(UR-STG-5), and an observer opts in and accepts retraction (UR-OBS-4). A stage
that is complete but *stale* keeps its annotations visible, because stale is
not discarded — the flag says the answer may be out of date, and
`StageStateReader::isStale` (3.10) is how a consumer finds that out, exactly as
for objects.

Batched and journalled like everything else here: one transaction inserts the
annotation rows and a single `ObjectsAnnotated` event, so UR-OBS-2's guarantee
that no event outruns its data holds for this path too.

**Both annotation tables are `WITHOUT ROWID`, keyed by their natural key, and
measurement names are interned.** `label_assertions` is keyed
`(object_id, stage_id, label_id)`; `measurement_assertions` is keyed
`(object_id, stage_id, name_id)`, where `name_id` references a
`measurement_names` table instead of the name being repeated on every row.

**That is the opposite of what 3.12 concludes for `objects`, and the inversion
is the point.** An object row carries a geometry blob of a couple of hundred
bytes, so a unique index duplicates only the key and the gain on insert order
dominates. An annotation row is three integers, so a unique index duplicates
very nearly the whole row and you maintain two B-trees to hold it. Measured at
ten million assertions (2026-10-10):

| `label_assertions` scheme | Ascending | Shuffled | Store |
|---|---|---|---|
| **`WITHOUT ROWID`, natural key** | **56.4 s** | **23.1 min** | **+137 MB** (13.8 B/row) |
| rowid key + unique index | 61.8 s | 29.0 min | +352 MB (35.5 B/row) |

The clustered form wins on both orders and is **2.6× smaller**. Taking 3.12's
rule as a general one would have cost 215 MB per slide and time on every pass.

**Interning measurement names halves the store.** Thirty million measurement
assertions — three per object — cost **1,586 MB** with the name as TEXT on
every row against **778 MB** interned, a saving of **808 MB per slide**, with
both figures reproducing byte for byte across four runs. Where the timing was
reproducible the interned form was also about **twice as fast**, the same
factor as the size and the expected one for a pass whose cost is bytes written.
This is UR-RES-17's reasoning, which 3.4 applies to labels and which nothing
had yet applied to measurement names. **It applies equally to `measurements`**,
the producing stage's own table, which stores names the same way; the
assertion table was measured because it is the one with a ten-million-row pass
behind it.

**Write order dominates every schema choice here by an order of magnitude.**
The same ten million label assertions cost 56 seconds written in ascending
object order and 23 minutes written in random order — **24.6×**, the largest
single factor measured anywhere in this design. UR-EXE-8's budget survives the
first comfortably and fails on the second, so delivering annotations in object
order is a correctness-of-schedule property of an annotating stage rather than
a tuning option. `04-workspace.md` 4.4 does not state it, and should.

> The realistic order is neither bound, and it behaves like the good one.
> Workers complete tiles out of sequence, but a tile's objects are contiguous
> in `object_id`, so the store sees a few thousand ascending runs rather than
> ten million random probes. Measured: 7,745 runs of about 1,282 objects took
> **36.6 seconds**, indistinguishable from the ascending figure once drive
> variance is allowed for. The 24.6× penalty requires genuinely random arrival,
> which no tile-sweeping stage produces.

> **The constraint is stated because nothing enforces it, not because the
> current schedule violates it.** A stage that annotates by walking a spatial
> query, or one that reorders work across tiles, lands in the shuffled case and
> loses the budget. Per-batch sorting is not a remedy: 3.12 measured it
> worthless for the objects table, and nothing here suggests it would do
> better.

## 3.8 Raster store — `storage/raster_store.hpp`

```cpp
enum class RasterCompression : int32_t { None, Lzw, Deflate };

/// Declared before anything runs, like a layer (UR-STG-10): the dependency
/// graph must resolve before the first stage starts, and a reader declared
/// ahead of its writer must have something to name.
class RasterDeclaration {
public:
    RasterDeclaration(const char* name, DataType, int32_t planeCount);
    RasterDeclaration& compression(RasterCompression);   // default Deflate
    RasterDeclaration& description(const char*);
};

class RasterWriter;
using RasterWriterPtr = std::shared_ptr<RasterWriter>;

class RasterWriter : public RasterSink {
public:
    ~RasterWriter() override;

    /// Accumulates one tile at the run's analysis level. Tiles may arrive in
    /// any order and need not cover the raster; unwritten area reads as zero.
    void writeTile(const BlockView&) override;

    /// Completes the raster: builds the pyramid down from the written level
    /// and publishes it. Until this returns, the raster is not readable and
    /// `RasterRegistry::isPopulated` is false.
    void flush() override;
    int64_t tilesWritten() const override;

    RasterId raster() const noexcept;
    StageId  stage() const noexcept;
};

class RasterRegistry {
public:
    RasterId declare(const RasterDeclaration&);

    bool        find(const char* name, RasterId& out) const noexcept;
    const char* name(RasterId) const;
    DataType    planeType(RasterId) const;
    int32_t     planeCount(RasterId) const;
    bool        exists(RasterId) const noexcept;

    /// False until some stage has written and flushed it. A raster that was
    /// declared and never written is legal and reads as zero.
    bool    isPopulated(RasterId) const noexcept;
    StageId writerStage(RasterId) const;
    int32_t writtenAtLevel(RasterId) const;
    int64_t bytesOnDisk(RasterId) const;

    /// A writer that attributes the raster to `stage`. One at a time per
    /// raster: unlike objects, a raster has a single producing stage, because
    /// two stages writing overlapping tiles of one image have no merge rule.
    RasterWriterPtr writerForStage(StageId, RasterId);

    /// Reads a populated raster one tile at a time, at whatever level and
    /// region the view carries — the same interface a layer rasterises
    /// through (`01-core.md` 1.9), so a consumer cannot tell whether the
    /// coarse product it reads was pixels or objects.
    TileRasterSourcePtr sourceFor(RasterId) const;

    /// Removes the raster a stage wrote: deletes the file and clears the
    /// population state. The declaration survives, as a layer's does.
    void discardRasterOfStage(StageId);

    int32_t count() const noexcept;
    RasterId at(int32_t index) const;
    int64_t  totalBytesOnDisk() const;
};
```

**Thread-safety:** `RasterRegistry` is ThreadSafe; `RasterWriter` is
ThreadSafe for `writeTile`, and `flush` requires exclusive access.
**Requirements:** UR-RES-7, UR-STG-4, UR-STG-6, UR-STG-9, UR-STG-10.

**A raster is not a layer, and `RasterId` is not a `LayerId`.** Twenty-five
functions in this specification take a `LayerId`, and about half of them —
`parent`, `depth`, `childCount`, `isAbove`, `objectCountInLayer`,
`ObjectId::fromAnchor`, `ObjectQuery::layer`, `restrictToLayer`,
`annotatesLayer` — mean nothing for an image. Admitting rasters under the same
type would give every one of them a runtime precondition that the type system
does not express and a caller discovers by hitting it: `ObjectQuery().layer(
probabilityMap)` would return an empty cursor rather than refuse. A `LayerId`
therefore keeps its single meaning — it names a collection of objects — and
rasters get their own identifier and their own registry. The cost is a
parallel registry of about a dozen functions; the alternative was a dozen
preconditions.

**The pixels live beside the store, not in it.** A level-0 `Float32`
probability map for the reference slide is 8×10⁹ pixels, 32 GB, which belongs
in neither a SQLite file nor a backup of one. Each raster is a pyramidal TIFF
in a subdirectory of the workspace — *not* the scratch subdirectory, which
`Workspace::create` and `open` clear (`04-workspace.md` 4.1) — and the store
holds one row naming it. Reading it back is SlideIO opening a TIFF, which the
library already depends on.

This module therefore gains a dependency on SlideIO that it did not have, for
raster I/O only. It is a module-level edge, not a library-level one: the data
library already links SlideIO through `reporting`'s `MaskWriter`. The
alternative placement was `reporting`, and it fails on the same point
`makeLayerRasterSource` failed on — `workspace` does not depend on `reporting`
and must not start to. Persistence with stage attribution, lifecycle and
discard is this module's subject; `reporting` exports finished results to
consumers.

**One writing stage per raster, enforced.** Objects from two stages coexist in
a layer because they are distinct rows. Tiles from two stages overlap in one
image and there is no merge rule, so `writerForStage` refuses a second stage
while a raster is populated by a first. A caller wanting two contributions
declares two rasters.

### Writing a raster is the one durable operation that is not a transaction

Everything else here commits in a single SQLite transaction, which is what
UR-OBS-2 rests on: an event and its data commit or fail together. A raster is
a file, and no file write is atomic with a transaction. This is stated rather
than assumed, because the failure modes are real and the recovery has to be
designed:

1. `writerForStage` allocates a **generated file name** and appends
   `RasterDeclared` if this is the first write. The name is never derived from
   the raster's name, so a re-run cannot collide with the file its previous
   attempt left behind.
2. Tiles accumulate into that file. The registry row still says
   `isPopulated == false`, so nothing reads it.
3. `flush` builds the pyramid, closes the file, and **then** commits one
   transaction setting the row's path, size, level and population flag, and
   appending `RasterWritten`.

The window is between 2 and 3. A crash there leaves a complete or partial file
that no row references, and `isPopulated` stays false — so a reader sees a
declared, unpopulated raster, which is a legal state, rather than a corrupt
one. **`Workspace::open` sweeps unreferenced raster files**, the way it already
clears the scratch subdirectory, and reports the bytes reclaimed. Discard runs
the same way in reverse: clear the row and commit first, delete the file
after, so a crash leaves an orphan for the sweep rather than a row pointing at
nothing.

The asymmetry is deliberate. An orphaned file costs disk and is recoverable; a
row pointing at a missing file is a store that lies about its own contents.

**Size is reported because nothing else governs it.** `bytesOnDisk` and
`totalBytesOnDisk` exist because the memory ceiling of UR-EXE-2 has no disk
counterpart anywhere in this design, and raster layers are the first feature
able to add tens of gigabytes to a workspace without the caller doing anything
that looks expensive.

**Archiving now means the directory, not the file.** UR-STG-9's unit was a
single `.store`; with rasters the workspace directory is the artifact, and
copying the store file alone yields a workspace whose rasters are all
declared and unpopulated. That is a consequence worth stating plainly rather
than discovering: it is legal, it is detectable, and it is not what the person
copying the file intended.

## 3.9 Event journal — `storage/event_journal.hpp`

```cpp
class Event {
public:
    Event() noexcept;                      // reads as no event: sequence 0
    Event(const Event& other);
    Event(Event&& other) noexcept;
    Event& operator=(const Event& other);
    Event& operator=(Event&& other) noexcept;
    ~Event();

    int64_t   sequence() const noexcept;
    EventKind kind() const noexcept;
    int64_t   timestampUnixMs() const noexcept;

    StageId stage() const noexcept;        // invalid where not applicable
    LayerId layer() const noexcept;
    LabelId label() const noexcept;

    int64_t objectCount() const noexcept;  // ObjectsAdded / EdgesAsserted /
                                           // StageDiscarded; otherwise 0
    Rect64  boundingBox() const noexcept;  // union bbox of the batch
    StageStatus stageStatus() const noexcept;  // StageStateChanged
};

class EventCursor {
public:
    virtual ~EventCursor();

    /// Waits up to timeoutMs for the next event. Returns false on timeout.
    /// Pass 0 to poll without blocking, -1 to wait indefinitely.
    virtual bool next(int32_t timeoutMs) = 0;
    virtual const Event& current() const = 0;

    /// The sequence last returned. Persist it to resume in a later process.
    virtual int64_t position() const = 0;
    virtual void close() = 0;
};
using EventCursorPtr = std::shared_ptr<EventCursor>;

class EventJournal {
public:
    static const int64_t kFromBeginning;   // -1: replay everything retained
    static const int64_t kFromNow;         // -2: only events after subscribing

    ~EventJournal();
    EventJournal(const EventJournal&) = delete;
    EventJournal& operator=(const EventJournal&) = delete;

    int64_t oldestSequence() const;
    int64_t newestSequence() const;

    /// Begins at the first event with sequence > fromSequence. Throws
    /// InvalidArgumentError naming both positions if it has been truncated away,
    /// and naming it, newestSequence() and the store if it is beyond the newest
    /// event (a position from another store, or from a recreated one).
    EventCursorPtr subscribe(int64_t fromSequence);

    /// Discards events below `sequence`. Objects, edges and stage state are
    /// unaffected.
    void truncateBelow(int64_t sequence);
    int64_t eventCount() const;
};
```

**Thread-safety:** `EventJournal` is ThreadSafe; `EventCursor` is
ThreadConfined; `Event` is ThreadCompatible. **Requirements:** UR-OBS-2 to
UR-OBS-7, decision 6.

**Positions.** Sequences start at 1 and are never reused (`AUTOINCREMENT`).
`newestSequence()` is the last sequence ever assigned, or 0, and truncation
does not lower it. `oldestSequence()` is the smallest retained sequence, or
`newestSequence() + 1` when none is retained. `subscribe(f)` for `f >= 0`
throws `InvalidArgumentError` when `f + 1` is below the truncation watermark,
and when `f` is beyond `newestSequence()`; any negative `f` other than the two
constants throws too. A cursor's `position()` is the sequence last returned,
or the resolved start before the first `next()`. `next(t)` with `t < -1`
throws.

**Truncation.** `truncateBelow(s)` deletes every event below `s` and records
the watermark in `meta`, in one transaction. The watermark only rises, and **a
value beyond `newestSequence() + 1` is clamped to `newestSequence() + 1`**:
recording it as given would make `kFromBeginning` and later subscriptions skip
events that had not yet been assigned. A cursor already open is not moved past
a truncated range silently either — its next `next()` throws
`InvalidArgumentError` naming its position and the new start (UR-OBS-7).

**What an event carries.** A batch kind carries its count and union bounding
box; `StageDiscarded` carries the number of objects removed; every other kind
carries a count of 0 and an empty bounding box. `LayerDeclared` names the
layer, `LabelAdded` the label, and `StageStateChanged` the stage and the status
it entered.

Events are rows in the store, not messages sent to subscribers. Subscription is
a cursor over `seq` — a subscriber holds one integer and asks for events above
it. That single property produces late join, replay from any point, resumption
after a subscriber restart, survival of a writer crash, and any number of
independent subscribers the writer knows nothing about. There is no subscriber
list, so an additional observer costs the writer nothing.

Events carry enough to decide whether to look further — kind, stage, layer,
count, union bounding box — and never geometry. A viewer whose viewport misses
the bounding box does nothing; one that intersects it issues an ordinary
spatial query through `ObjectStore`. The journal stays small and the expensive
path stays the one already designed (UR-OBS-5).

A subscriber falling behind a fast writer causes no backpressure and no drops.
Its next `next()` simply has more rows waiting, which it may coalesce — a
thousand `ObjectsAdded` events for one layer become one redraw.

An event cursor holds one connection for its lifetime and reads the journal in
pages, each page in its own read transaction, so events committed meanwhile
become visible to it. It may outlive its `Store`; `next()` then throws
`IoError`.

`next(timeoutMs)` waits on the local signal when `eventSignalEnabled` and falls
back to the poll interval otherwise. **Correctness never depends on the
signal**: a cursor re-reads the journal after every wait and no wait exceeds
`eventPollIntervalMs`, so a missed signal makes a subscriber late by at most
one interval, never wrong, and a polling-only subscriber observes the
identical sequence (UR-OBS-6). The transport is local to the machine and the
library opens no network socket:

- The signal is named from a random 128-bit `store_uuid` written to `meta` at
  creation, never from the path.
- **Windows:** a manual-reset named event, `Local\slideio-processor-<uuid>`,
  set by the writer after every commit that appends an event and reset by a
  waiter that wakes. It wakes every waiter. Every handle creates or opens it,
  and the system destroys it with its last handle.
- **Linux:** a POSIX named semaphore, `/slideio-processor-<uuid>`, mode 0600,
  posted after every such commit; waiters use `sem_timedwait` and drain with
  `sem_trywait`. **Only the writer creates the name**, and unlinks it on
  close; a reader only opens an existing name, retrying at most once per wait
  until a writer appears, so a reader never leaves a semaphore behind.
  **Known limitation:** one post wakes one waiter, so when several cursors
  wait on one store, the others fall back to their poll interval for that
  commit.
- **macOS:** no signal, since `sem_timedwait` does not exist there;
  subscribers poll at `eventPollIntervalMs`.

A signal that cannot be opened is logged at `Warning` and never thrown; the
cursor polls.

## 3.10 Stage state reader — `storage/stage_state_reader.hpp`

```cpp
class StageStateReader {
public:
    ~StageStateReader();
    StageStateReader(const StageStateReader&) = delete;
    StageStateReader& operator=(const StageStateReader&) = delete;

    int32_t count() const noexcept;
    StageId at(int32_t index) const;
    bool    find(const char* name, StageId& out) const noexcept;

    const char* name(StageId stage) const;
    StageStatus status(StageId stage) const;
    bool        isStale(StageId stage) const;
    const char* configuration(StageId stage) const;     // JSON, "" if none
    int64_t     completedAtUnixMs(StageId stage) const; // 0 if never completed

    int32_t dependencyCount(StageId stage) const;
    StageId dependencyAt(StageId stage, int32_t index) const;
    int32_t writtenLayerCount(StageId stage) const;
    LayerId writtenLayerAt(StageId stage, int32_t index) const;
};
```

**Thread-safety:** ThreadSafe. **Requirements:** UR-STG-4, UR-STG-2,
UR-STG-10.

Read side only. Declaring, running and invalidating stages belong to
`workspace`, which writes through `StageStateWriter` (3.10a); this exists so an
observer can report "nuclei: complete, cells: running" without linking
`slideio-processor-engine` and therefore without loading OpenCV (design 5.1).

UR-STG-4 requires stage state be queryable without running anything, which this
satisfies directly — a `ReadOnly` store answers every method here.

Stages are listed in ascending `StageId`, dependencies in ascending dependency
id, and written layers in ascending `LayerId`. `find` compares names exactly.
Members taking a stage throw `InvalidArgumentError` for an unknown one.

**`configuration` returns a pointer that stays valid while the reader lives**,
and always holds the value that call read: a later change is seen by a later
call, which returns a different pointer. The reader therefore retains every
distinct configuration value it has read until it is destroyed. That growth is
bounded by the number of configuration changes a long-running viewer observes,
and is a known cost of returning `const char*` from a value another process can
change.

## 3.10a Stage state writer — `storage/stage_state_writer.hpp`

```cpp
class StageStateWriter {
public:
    ~StageStateWriter();
    StageStateWriter(const StageStateWriter&) = delete;
    StageStateWriter& operator=(const StageStateWriter&) = delete;

    /// Adds a stage in status Pending, not stale, never completed.
    StageId declare(const char* name, const char* configurationJson);
    /// Sets the status and appends StageStateChanged.
    void setStatus(StageId stage, StageStatus status);
    void setStale(StageId stage, bool stale);
    void setConfiguration(StageId stage, const char* configurationJson);
    void addDependency(StageId stage, StageId dependsOn);
    void clearDependencies(StageId stage);
    void addWrittenLayer(StageId stage, LayerId layer);
};
```

Returned by `Store::stageStateWriter()`.
**Thread-safety:** ThreadSafe; writes are serialised. **Requirements:**
UR-STG-4, UR-STG-2, UR-OBS-5.

**The write side of 3.10 is public, and `workspace` writes stage rows only
through it.** Declaring, running and invalidating stages stay `workspace`'s
decisions (`04-workspace.md`); this class is only how they reach the store, so
that `workspace` never touches the store's tables directly.

Every member is one write transaction. Every member throws `IoError` on a
closed or `ReadOnly` store — on a `ReadOnly` store the writer exists and
refuses everything — and `InvalidArgumentError` for an unknown stage.

- `declare` numbers stages from 1. Names are unique and compared exactly; a
  null, empty or duplicate name throws. A null configuration is stored as
  `""`. **No event is appended**: a stage that has not changed status has
  nothing to report.
- `setStatus` appends `StageStateChanged` in the same transaction. **Entering
  `Complete` sets `completedAtUnixMs` to now, and re-entering it refreshes the
  time**; no other transition changes it. **An unchanged status still appends
  an event**: every `setStatus` call is a transition the journal records.
- `setStale` and `setConfiguration` append no event.
- `addDependency` and `addWrittenLayer` are idempotent. A stage cannot depend
  on itself, and the pixel layer cannot be written. `clearDependencies`
  removes every dependency of a stage.

## 3.11 GeoJSON import — `storage/geojson_import.hpp`

```cpp
class ImportResult {
public:
    int64_t objectsImported() const noexcept;
    int32_t labelsRegistered() const noexcept;
    int32_t featuresSkipped() const noexcept;
    ImportPolicy policyApplied() const noexcept;
};

class GeoJsonImporter {
public:
    GeoJsonImporter(Store&, LayerId targetLayer, StageId assertingStage);
    ~GeoJsonImporter();

    ImportResult import(const char* path);
    ImportResult import(const char* path,
                        ProgressObserver*,
                        const CancellationToken&);
};

/// Reads the first polygon of a GeoJSON file as a processing ROI. No store
/// is involved; the result is passed to PipelineSpec::restrictToPolygon.
Polygon importRoiPolygon(const char* path);
```

**Thread-safety:** ThreadConfined. **Requirements:** UR-RES-8, UR-RES-13,
UR-RES-19, UR-IMG-5, decision 3.

Import lives here rather than in `reporting` because it is an input, not a
report, and because UR-RES-19's policy has to run next to the vocabulary
registry that enforces it (decision 3). It reads through `core::GeoJsonReader`,
so there is one GeoJSON parser in the library, not two.

Under `Strict`, a label belonging to neither the standard nor the workspace
vocabulary aborts the import with a diagnostic naming the label. Under
`Permissive`, it is registered as workspace-local with origin
`ImportRegistered`, so a consumer can later tell a label that was declared from
one that merely arrived (UR-RES-19).

Imported objects carry the target layer as a property of the object itself, so
they are returned by a layer query alongside objects written by a stage even
though no stage produced them (UR-RES-13).

`importRoiPolygon` is the UR-IMG-5 path: a QuPath-exported annotation becomes a
processing ROI without any store being involved.

**Two things `core::GeoJsonReader` leaves to this module.** First, QuPath
before 0.3 wrote `properties.measurements` as an array of `{name, value}`
objects rather than an object; the reader skips arrays, so such a file imports
with no measurements and no diagnostic (`01-core.md` 1.14). If import is to
support those files, `storage` must handle that form itself — through a
reader extension or a pre-pass — or at least report the skipped measurements in
`ImportResult`. Second, the reader does not expose a file's `provenance`
member; if import is to preserve the provenance of the file it reads,
restoring it is this module's obligation, through `Provenance::restored` as
3.2 does.

## 3.12 Schema and format

Written for reference; the DDL itself lives in the implementation. The first
part creates every table below down to `measurements`, plus `events`; the
edge, annotation and raster tables are added to the same creation DDL by their
parts, under the same `format_version` (see the note at the top).

| Table | Holds |
|---|---|
| `meta` | Key–value, `WITHOUT ROWID`: `format_version`, `vocabulary_version`, `provenance`, `import_policy`, `journal_truncated_below` (starts at 1), `store_uuid` |
| `layers` | Name, kind, parent, depth, dropped flag, coarsest analysis level |
| `labels` | Name, origin, `ontology_ref` (reserved, empty in v1 — 02 §2.2). No recognised flag: `isRecognised` is computed (3.4) |
| `stages` | Name, status, stale flag, configuration, completion time |
| `stage_deps` | Declared dependencies, keyed `(stage_id, depends_on_id)` |
| `stage_layers` | Layers each stage writes, keyed `(layer_id, stage_id)` |
| `objects` | Monotonic rowid key, unique index on `(layer_id, anchor_id)`, stage (indexed), label, exact `int64` half-open bbox, geometry blob |
| `object_rtree` | R\*Tree index over bounding boxes |
| `measurements` | Per-object ordinal, interned name, value and unit, from the producing stage — `WITHOUT ROWID`, keyed `(object_rowid, ordinal)` |
| `measurement_names` | Interned measurement names, shared by `measurements` and `measurement_assertions` (3.7) |
| `label_assertions` | Object, asserting stage, label — `WITHOUT ROWID`; labels attached after the object was written (3.7) |
| `measurement_assertions` | Object, asserting stage, interned name, value, unit — `WITHOUT ROWID` (3.7) |
| `parent_edges` | Child, parent, asserting stage |
| `derived_from` | Derived, primitive, asserting stage |
| `rasters` | Name, data type, plane count, writing stage, level, file path, bytes, populated flag (3.8) |
| `events` | Sequence (`AUTOINCREMENT`), kind, stage, layer, label, count, bbox, stage status, timestamp |

**The identifier a caller sees and the key the table is ordered by are not the
same thing.** `ObjectId` stays exactly what decision 8 defines — layer plus
geometry-derived anchor, stable across tile sizes and worker counts, the only
identifier ever exported or compared (UR-QA-3). The `objects` table is ordered
by a **monotonic rowid**, with a unique index on `(layer_id, anchor_id)`
carrying the logical identity.

That split is a schedule requirement, not a refinement. Inserting ten million
objects keyed physically by a random 64-bit anchor measured **31.6 minutes**
against **7.7 minutes** for monotonic keys — a 4.1× penalty from scattering
random writes across a 2.9 GB table rather than appending to its end — and
UR-EXE-8 allows 30 minutes for segmentation *and* classification together. The
anchor-keyed scheme exceeds the whole budget before a pixel is read.

**The scheme specified above has since been measured against the alternative it
replaces, and it holds.** A second run (2026-10-10, ten million objects, same
slide, same seed and same batch size, so every variant saw an identical anchor
sequence — confirmed by an identical collision count of 71,228 in all three):

| Scheme | Insert | Rate | Store |
|---|---|---|---|
| Anchor **is** the primary key | 25.9 min | 6,435 obj/s | 3.126 GB |
| Anchor PK, each batch sorted by anchor | 30.7 min | 5,432 obj/s | 3.113 GB |
| **Monotonic rowid PK + `UNIQUE(layer_id, anchor_id)`** | **10.8 min** | **15,409 obj/s** | **3.023 GB** |

The unique index gives back **2.39×** of the penalty, which is what this
measurement existed to ask. It costs nothing in space — the store is 103 MB
*smaller* than the anchor-keyed one, because random inserts leave primary-key
pages roughly two-thirds full while appended rowid pages pack nearly full. The
geometry join improves for the same clustered-read reason, by **1.7–2.0×**
measured as join cost minus R\*Tree cost within a single store. That is the only
cross-store query figure worth quoting: the three were read in write order and
differ in page-cache warmth by up to 7× on an R\*Tree table that is
byte-identical in all of them.

**Sorting each batch by anchor before insert is not a cheaper partial measure
and should not be attempted.** It measured 18% *slower* than not sorting — ten
thousand keys sorted within a batch spanning the whole 64-bit space still land
on ten thousand different pages, so the sort buys no locality and costs its own
time. Recorded because an earlier version of this section proposed it.

**The one price is the point lookup, and it is affordable.** `get(ObjectId)`
resolves through the index and then the table, paying one extra B-tree descent:
**11.3 µs against 5.1 µs**, stable across repeated runs and therefore structural
rather than cache. Six microseconds on an incidental path buys fifteen minutes
on the bulk one. The first attempt to measure this found no difference and was
wrong — it probed synthetic anchors, hit 31 of 20,000, and so timed the miss
path, where the extra hop never runs.

Every figure above comes from a DRAM-less consumer SATA SSD near capacity
rather than UR-EXE-8's reference NVMe. They are a floor, and the ratios travel
better than the absolutes.

**The annotation pass has since been measured, and it is cheap when it is
ordered.** Ten million label assertions cost 56 seconds in ascending object
order against the object pass's 10.8 minutes, so the earlier reading of this
section — that the two passes would consume 21.6 of UR-EXE-8's 30 minutes and
leave about 8 for everything else — was wrong. Ordered, the second pass is a
rounding error on the first. It becomes 23 minutes, and the budget fails, only
when the annotations arrive in random object order. The schema findings are in
3.7.

**Store size at the reference object count**, measured rather than estimated:
3.0 GB of objects and R\*Tree, 137 MB for one label assertion per object, and
778 MB for three interned measurement assertions per object — about **3.9 GB
per slide** before rasters. No requirement bounds that; the absence of a disk
budget anywhere, against UR-EXE-2's memory ceiling, remains an open action.

The realistic write order — tiles completed out of sequence, objects contiguous
within a tile — has since been measured at **36.6 seconds**, which is the
ascending case rather than the shuffled one. The budget holds with room.

**Geometry encoding.** Open question 5 in `requirements.md` — how geometry is encoded, WKB versus a
compact custom encoding versus simplified contours — is **closed: a compact
delta-varint encoding**, lossless, with no simplification. It affects no
signature here. Each object's geometry is one BLOB, private to the store and
covered by the format version:

- Byte 0 is the encoding version, `1`. Then a varint ring count, then for each
  ring a varint `(pointCount << 1) | isHole` followed by its points.
- A ring's first point is stored as zigzag-varint absolute `x` and `y`. Each
  later point is stored as zigzag-varint deltas from the previous point,
  computed in wrapping `uint64_t` arithmetic, so every `int64_t` pair
  round-trips exactly.
- Varints are unsigned LEB128 of at most 10 bytes.

A contour traced at unit steps — the common case for segmentation output —
costs about two bytes per point rather than WKB's sixteen. A blob that is
truncated, has trailing bytes, an unknown version, an over-long varint, a ring
of fewer than 3 points, or a ring `Polygon::addRing` refuses throws
`InternalError` naming the object: a stored blob that `core` refuses is
corruption, not caller error.

Open question 4 — whether SQLite with R\*Tree sustains the reference object
count — is **closed yes**, conditional on the physical-key scheme above. The
spike measured query cost scaling with result size rather than store size,
realistic viewer viewports at 2 to 39 ms including geometry, and UR-OBS-1
holding exactly: 69,477 queries from a second process during an active
ten-million-object write, zero errors, no measurable writer slowdown. The event
journal and the concurrent-reader guarantee rest on the same SQLite instance
and are covered by the same result.
