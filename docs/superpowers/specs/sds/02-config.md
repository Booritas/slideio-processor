# SDS 02 — `config`

**Module:** `config` · **Library:** `slideio-processor-data` · **Depends on:** `core`

Global settings, the shipped standard label vocabulary, and the backend
capability table. Small, and almost entirely immutable.

Conventions in [00-conventions.md](00-conventions.md).

---

## 2.1 Configuration — `config/configuration.hpp`

```cpp
class Configuration {
public:
    Configuration();                      // documented defaults
    Configuration(const Configuration&);
    Configuration(Configuration&&) noexcept;
    Configuration& operator=(const Configuration&);
    ~Configuration();

    static Configuration defaults() noexcept;

    /// The process-wide default, used when a Configuration is not supplied.
    static const Configuration& processDefault() noexcept;
    /// Set at startup only. Objects already constructed are unaffected.
    static void setProcessDefault(const Configuration&);

    int64_t  memoryCeilingBytes() const noexcept;      // default 2 GiB
    int32_t  workerCount() const noexcept;             // default hw concurrency
    int32_t  tileWidth() const noexcept;               // default 1024
    int32_t  tileHeight() const noexcept;              // default 1024
    bool     tileSizeFromSlide() const noexcept;       // default true
    const char* logFilePath() const noexcept;          // "" = stderr
    LogLevel logLevel() const noexcept;                // default Info
    int32_t  eventPollIntervalMs() const noexcept;     // default 150
    bool     eventSignalEnabled() const noexcept;      // default true
    int64_t  objectBatchSize() const noexcept;         // default 512
    /// Where a directly-constructed Pipeline spills seam fragments (5.7).
    /// "" = the platform temporary directory. A Workspace overrides this
    /// with a subdirectory of its own, so UR-STG-9 keeps holding.
    const char* scratchDirectory() const noexcept;     // default ""
    bool     equals(const Configuration&) const noexcept;
};

class ConfigurationBuilder {
public:
    ConfigurationBuilder();
    explicit ConfigurationBuilder(const Configuration& from);
    ~ConfigurationBuilder();

    ConfigurationBuilder& memoryCeilingBytes(int64_t);
    ConfigurationBuilder& workerCount(int32_t);        // 1 = single-threaded
    ConfigurationBuilder& tileSize(int32_t width, int32_t height);
    ConfigurationBuilder& tileSizeFromSlide(bool);
    ConfigurationBuilder& logFilePath(const char*);
    ConfigurationBuilder& logLevel(LogLevel);
    ConfigurationBuilder& eventPollIntervalMs(int32_t);
    ConfigurationBuilder& eventSignalEnabled(bool);
    ConfigurationBuilder& objectBatchSize(int64_t);
    ConfigurationBuilder& scratchDirectory(const char*);

    /// Validates the whole configuration; throws InvalidArgumentError naming
    /// every offending setting rather than the first.
    Configuration build() const;
};
```

**Thread-safety:** `Configuration` is Immutable. `ConfigurationBuilder` is
ThreadConfined. `processDefault()` / `setProcessDefault()` are ThreadSafe.
**Requirements:** UR-EXE-2, UR-EXE-3, UR-EXE-6, UR-OBS-6.

`Configuration` is an immutable value object, and every `Workspace`, `Pipeline`
and `Store` captures a copy at construction and never re-reads the global.
`setProcessDefault` after construction affects only objects made later.

That is not a style preference. A worker count or tile size changed mid-run
would change behaviour halfway through a stage, breaking UR-QA-3's determinism
in a way that reproduces only intermittently. Immutability also makes
thread-safety free — nothing mutable is shared — and lets a test hold two
configurations at once.

`tileSizeFromSlide` defaults to `true`, deriving tile size from the slide's
native tiling to avoid re-decoding the same compressed tiles (UR-EXE-6);
`tileSize()` supplies the fallback when the slide reports none, and overrides
when `tileSizeFromSlide(false)`.

`eventSignalEnabled(false)` disables the local wake-up and falls back to
polling. UR-OBS-6 requires that a polling subscriber observe an identical event
sequence, so this flag is also the switch the conformance test flips.

## 2.2 Standard vocabulary — `config/standard_vocabulary.hpp`

```cpp
class StandardVocabulary {
public:
    static const StandardVocabulary& instance() noexcept;

    int32_t version() const noexcept;
    int32_t labelCount() const noexcept;
    LabelId labelAt(int32_t index) const;
    const char* labelName(LabelId) const;         // "" if not standard
    bool    findLabel(const char* name, LabelId& out) const noexcept;
    bool    isStandard(LabelId) const noexcept;

    /// The layer kind a label is conventionally used with, for documentation
    /// only. The vocabulary is global to a store and never scoped by layer.
    const char* conventionalLayer(LabelId) const;

    /// An external ontology identifier for this label — Cell Ontology,
    /// SNOMED CT or NCI Thesaurus. Returns "" for every label in v1; see
    /// below for why the accessor exists before the data does.
    const char* ontologyRef(LabelId) const;
};

int32_t standardVocabularyVersion() noexcept;
```

**Thread-safety:** Immutable. **Requirements:** UR-RES-17.

Compiled in, immutable, and versioned independently of the store format. This
is the *shipped* vocabulary only; the effective vocabulary a store resolves
against is `storage::VocabularyRegistry`, which records the standard version it
was written against and adds workspace-local labels (decision 2).

`conventionalLayer` is documentation, not a constraint. UR-RES-17 requires the
vocabulary be global to the store — one set of labels shared by every layer,
semantic and primitive alike, never scoped or subsetted — so nothing in the API
may reject a label because of the layer it is applied to.

### The v1 standard vocabulary

Ten labels. This resolves open question 8 in `requirements.md`.

| Label | `conventionalLayer` | Definition |
|---|---|---|
| `tumour` | region or cell | Neoplastic tissue, or a neoplastic cell |
| `stroma` | region or cell | Supporting connective tissue, non-neoplastic |
| `necrosis` | region | Non-viable tissue |
| `immune` | cell | An immune cell of unspecified lineage |
| `lymphocyte` | cell | A lymphocyte specifically |
| `positive` | any | Marker-positive by the asserting stage's criterion |
| `negative` | any | Marker-negative by the asserting stage's criterion |
| `artefact` | region | Fold, bubble, pen mark, out of focus — present, not analysable |
| `ignore` | region | Deliberately excluded from analysis |
| `other` | any | Classified, and none of the above |

**The question's own resolution rule does not survive contact with the
catalog.** Open question 8 says the vocabulary "should cover what the shipped
algorithms actually emit, so it is downstream of question 7". No shipped
operation emits a label at all: `segment.tissue_detect`,
`segment.connected_components` and `segment.contours` (`06-algorithm.md` 6.7)
each take a `targetLayer` and emit unlabelled geometry. Read literally the rule
yields the empty set, and the dependency on question 7 is weaker than it looks
— none of the ten depends on which filters ship. Question 7 has since been
resolved at twenty-one operations (`06-algorithm.md` 6.10), none of which emits
a label, which confirms the reading rather than changing it. What the vocabulary is
actually for is the other two paths: what imports cleanly under
`ImportPolicy::Strict` (`03-storage.md` 3.11), and what a user's own
classification stage asserts against (`03-storage.md` 3.7). Both are v1.

**There is no `tissue` label**, and its absence is the rule the rest follow.
`tissue` is what `segment.tissue_detect` conventionally names its *layer*, and
`requirements.md` §4 uses it that way — "`tumour` or `stroma` inside `tissue`".
A label repeating its layer would cost a vocabulary entry and a column on ten
million rows to say what the layer already says. The same reasoning gives
`immune` rather than `immune_cell`: inside a `cells` layer, the "cell" is the
layer's contribution.

**`tumour` is deliberately one label used at two scales.** A tumour region and
a tumour cell are the same claim about different objects, and the layer already
distinguishes them — which is what the layer-versus-label split is for.
Splitting it into `tumour_region` and `tumour_cell` would fight UR-RES-17's
global vocabulary rather than use it, and would double every such entry.

**`positive` and `negative` are deliberately uninterpreted.** They are here
because QuPath emits them and `Strict` import would otherwise reject ordinary
files on day one, which is UR-RES-8's round-trip acceptance criterion failing
at the first attempt. The library cannot know which marker is meant, so the
label carries the call and a measurement carries the marker and its value. The
limitation is real and is documented rather than hidden: two bare `positive`
labels on one object are indistinguishable, so a multi-marker workspace should
use local labels — `ki67_positive`, `her2_positive` — which `addLocal` supplies
at any time without a migration (UR-RES-18).

**`other` is not `unlabelled`.** Unlabelled means nothing classified the
object. `other` means a stage looked and the answer was none of these. UR-RES-4
makes unlabelled a valid state in any layer, so the distinction costs one entry
and separates two situations a reviewer genuinely needs apart.

**The list errs small on purpose.** Adding a standard label later is a
vocabulary version bump that an older library reports through `isRecognised`
with identifier and string intact (UR-RES-17); removing one is a breaking
change with no such mechanism. Everything contested — `fibroblast`,
`endothelial`, `mitotic`, any grading scheme — belongs in `addLocal`, which
costs a call and no migration. The standard vocabulary should be the set nobody
argues about.

### Why `ontologyRef` exists while returning `""`

External identifiers are **not populated in v1**. The mappings are contested
for exactly the loose terms above — there is no clean SNOMED CT code for
"stroma" as used here — and each one becomes an obligation to maintain across
releases of three external vocabularies.

The accessor ships anyway, with its storage counterpart in the `labels` table
(`03-storage.md` 3.12), because the cost of reserving it now is zero and the
cost of adding it later is not.

The storage half of that argument is the firm one. UR-RES-18 promises a
workspace may add labels "without a store-format migration", and adding a
column to `labels` in v1.1 would break that promise for every store already
written. The API half is softer and worth stating as such: these classes are
pimpl'd (C-2), so a later accessor costs a recompile rather than a
redefinition. But UR-API-2 names ABI stability as a design constraint, and
open question 11 leaves a stable C ABI genuinely possible, under which adding
a method is a break rather than an inconvenience. Reserving it costs one
accessor returning `""`.

Populating the field later is neither problem: it is a vocabulary version bump,
which `isRecognised` and `recordedStandardVersion` already handle.

## 2.3 Backend capabilities — `config/backend_capabilities.hpp`

```cpp
class BackendCapabilities {
public:
    static const BackendCapabilities& instance() noexcept;

    const char* backendName() const noexcept;      // e.g. "OpenCV"
    const char* backendVersion() const noexcept;   // e.g. "4.9.0"
    int32_t     backendMajorVersion() const noexcept;

    DataTypeSet supportedTypes() const noexcept;
    bool        supports(DataType) const noexcept;

    /// Types carried losslessly by the library but not computed natively by
    /// the backend. Reading, selection, conversion and storage remain lossless
    /// for these (UR-IMG-10); only algorithms may decline them (UR-IMG-11).
    DataTypeSet storageOnlyTypes() const noexcept;
};
```

**Thread-safety:** Immutable. **Requirements:** UR-IMG-11, UR-IMG-12,
UR-IMG-10, UR-BLD-4.

Generated at build time from the backend actually linked. Under OpenCV 4 this
reports eight of the eleven types as computable, with `UInt32`, `Int64` and
`UInt64` in `storageOnlyTypes()`; under OpenCV 5 the three move across and
`storageOnlyTypes()` empties.

The distinction between `supportedTypes()` and `storageOnlyTypes()` is the
concrete form of UR-IMG-10 versus UR-IMG-11: the *library* carries all eleven
types losslessly through reading, channel selection, conversion and the store
regardless of backend, and only an *algorithm* may decline one. Nothing here
names the OpenCV 4 type set as a fixed limitation, which is what UR-BLD-4
requires (its acceptance criterion is a review confirming exactly that).

This is the input to the generated support matrix of UR-IMG-12, combined with
each operation's declared `supportedTypes()` from `pipeline::OperationCatalog`.

## 2.4 Logging setup — `config/logging.hpp`

```cpp
/// Installs a file or stderr sink behind core's log facade, per the
/// configuration's logFilePath() and logLevel().
void configureLogging(const Configuration&);

/// Installs a caller-supplied sink instead. The caller owns it and must keep
/// it alive until logging is reconfigured or the process ends.
void configureLogging(LogSinkPtr, LogLevel);
```

**Thread-safety:** ThreadSafe. **Requirements:** none directly.

The only function in `config` with a side effect. It exists here rather than in
`core` because the log *facade* must be available to every module including
`core` itself, while the *decision* about where logs go is configuration.

## 2.5 What is deliberately absent

| Not here | Where instead | Why |
|---|---|---|
| Workspace-local labels | `storage::VocabularyRegistry` | Per-store state that must be written back to the store file (decision 2, UR-RES-18) |
| Label import policy | `storage::VocabularyRegistry` | UR-RES-19 requires the policy in force and its outcome be recoverable from the store |
| Per-stage configuration | `workspace::StageDefinition` | UR-STG-4 records it against the stage |
| Slide or scene selection | `workspace::Workspace` | A property of the analysis, not of the process |
