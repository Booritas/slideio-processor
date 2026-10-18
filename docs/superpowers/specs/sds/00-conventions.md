# SDS 00 — Conventions

**Status:** draft for review
**Date:** 2026-09-28
**Parent:** [Module structure design](../2026-09-28-module-structure-design.md)

Every signature in documents 01–07 follows the rules fixed here. Read this
first; the per-module documents do not restate them.

---

## C-1. Language, namespace, headers

C++17 (UR-BLD-2). One flat public namespace, `slideio::processor`, with modules
expressed as header directories rather than nested namespaces — `core::Polygon`
and `storage::Query` would make cross-module code verbose for no gain, and
users think in terms of the library, not its internal decomposition. Doxygen
groups carry module identity in the generated documentation.

```
include/slideio/processor/
  core/       config/     storage/
  workspace/  pipeline/   algorithm/   reporting/
```

Each module also provides an umbrella header, `<module>/<module>.hpp`,
including its public headers.

## C-2. ABI rules

UR-API-2 forbids templates in the public API, STL containers in exported
signatures, and exposed implementation. Concretely:

| Rule | Form |
|---|---|
| No STL containers in signatures | Count-plus-accessor, or fill-caller-buffer (C-5) |
| No user-facing templates | Concrete types only; small value types are plain classes, not template instantiations |
| Implementation hidden | Every non-trivial class is pimpl'd; POD structs are the exception and are documented as such |
| Stable enum layout | Every `enum class` has explicit `: int32_t` |
| No inline logic in headers | Headers declare; definitions live in the translation unit |

**`std::shared_ptr` is permitted in signatures, and is the only exception.**
This is a deliberate reading of UR-API-2 rather than an oversight. SlideIO's own
public API uses `std::shared_ptr` for handle ownership (`slideio::ScenePtr`),
UR-API-1 requires this library to follow the `slideio-python` pattern, and
pybind11 handles `shared_ptr` natively without a holder declaration. Banning it
would mean raw owning pointers with explicit destroy functions, which is worse
for C++ users and no better for bindings. Every other template is excluded.

Each handle type declares an alias next to it:

```cpp
class Store;
using StorePtr = std::shared_ptr<Store>;
```

## C-3. Integer types

`int64_t` for every pixel count, area, linear index, histogram bin,
accumulator, object count and coordinate (UR-IMG-2). No exceptions on those
paths — 32-bit overflows at 2.1×10⁹, which the reference slide exceeds at
8×10⁹ pixels.

`int32_t` for counts bounded by structure rather than by image size: channel
indices, layer counts, ring indices, pyramid levels, worker counts.

Sizes in bytes are `int64_t`. `size_t` does not appear in any public signature,
since its width varies by platform and it maps poorly to bindings.

## C-4. Strings

**In:** `const char*`, UTF-8, never null unless documented. The callee copies
what it needs; no lifetime is retained.

**Out, owned by the object:** `const char*` valid until the next non-const call
on that object, or until it is destroyed. Documented per method.

**Out, variable length:** fill a caller buffer and return the length required,
excluding the terminator. A `nullptr` buffer queries the length.

```cpp
int32_t toString(char* buffer, int32_t capacity) const;
```

The convention is always: return value is the required length; the buffer is
filled and null-terminated if capacity allows; the result is truncated but
still terminated if it does not.

## C-5. Collections

No STL container crosses the boundary. Two forms, chosen by element size:

**Count plus indexed accessor** — for small or handle-like elements:

```cpp
int32_t count() const;
LayerId at(int32_t index) const;
```

**Fill-caller-buffer** — for bulk numeric data where per-element calls would
dominate:

```cpp
int64_t ringPoints(int32_t ring, Point64* buffer, int64_t capacity) const;
```

Same contract as C-4: the return value is the count required; `nullptr` queries
it. Out-of-range indices on accessors throw `InvalidArgumentError`; they do not
return sentinels.

## C-6. Errors

Exceptions only. No error codes, no output status parameters, no silent
failure. Every exception thrown across the public boundary derives from
`ProcessorError` (UR-API-5), with one exception: `std::bad_alloc`. It is the
only non-`ProcessorError` that may cross a public boundary, it is always listed
in `@throws` wherever it can occur, and it is never wrapped — translating an
allocation failure would itself need to allocate, and a caller that handles
memory exhaustion already catches it by its standard type. Every other
exception — from SQLite, OpenCV or the standard library (`std::system_error`,
`std::filesystem::filesystem_error`, `std::ios_base::failure`,
`std::length_error` and so on) — is caught at the boundary and translated, with
its `what()` preserved as `cause()`: to `IoError` when it is an input/output
failure, and to `InternalError` for anything else.

A `noexcept` function cannot report a failure, so where its only possible one
is a mutex error — `setLogSink`'s lock (`01-core.md` 1.15) — that failure is
documented as fatal: `std::terminate` is called.

Every error carries actionable context — stage, operation, tile coordinates,
underlying cause (UR-EXE-5). Fields not applicable read as empty string or
report `false` from their `has*` accessor.

Query methods that legitimately find nothing return `bool` with an out
parameter rather than throwing:

```cpp
bool find(const char* name, LayerId& out) const;   // false = absent, not an error
```

## C-7. Thread-safety annotations

Every public class is annotated with exactly one level. The per-module
documents state it for each class.

| Level | Meaning |
|---|---|
| **Immutable** | Cannot be modified after construction. Any number of threads, no synchronisation. |
| **ThreadSafe** | All methods callable concurrently from any thread with no external locking. |
| **ThreadCompatible** | Concurrent `const` calls are safe. Any non-`const` call requires exclusive access. |
| **ThreadConfined** | One thread at a time, for the object's whole lifetime. Ownership may be transferred between threads with external synchronisation. |

Unannotated means ThreadCompatible. `storage` is the module where ThreadSafe
appears (decision 4); `Cursor` and `EventCursor` are ThreadConfined
deliberately, so per-thread iteration order stays deterministic.

A class carries one level, not a level per member. In `core` the typed ids
(`LayerId`, `StageId`, `LabelId`, `RasterId`, `ObjectId`) are Immutable: they
have no mutating member other than assignment. The other small value types are
ThreadCompatible, because they have public data members or a mutating member
(`DataTypeSet::add`), or because their documented contract makes assignment
the caller's to serialise: `DataTypeSet`, `Point64`, `Rect64`, `ErrorContext`,
`Measurement`, `MeasurementBound` and `BlockPlane`.
`CancellationToken` is ThreadCompatible as an object, with a note that its
copies share one atomic flag, so distinct copies may be cancelled and queried
from any number of threads at once. Builders such as `ObjectQuery` are
ThreadConfined (C-10).

## C-8. Ownership and lifetime

- `shared_ptr` returned from a factory: the caller shares ownership.
- Reference returned from an accessor (`LayerRegistry& layers()`): owned by the
  parent, valid while the parent lives. Never stored beyond it.
- Raw pointer parameter: non-owning, borrowed for the call only. `nullptr`
  means absent where documented (`ProgressObserver*` is the common case).
- Interface implemented by the caller (`ObjectSink`, `LogSink`,
  `ProgressObserver`): the caller owns it and must outlive its registration.

No public API transfers ownership through a raw pointer.

**Moved-from objects.** After move construction *or* move assignment the
source is in its empty or default state, and every accessor on it is safe.
Where a class documents a null state (`BlockView`, `RasterMask`), the source
is null. Move assignment never swaps: the target's old contents are released,
never handed back to the source, so a moved-from object's state does not
depend on what it was assigned to. The GeoJSON reader and writer, which hold an
open file, are the one variation: their `noexcept` accessors still read as
empty, but every operation on a moved-from one (`next()`, `begin()`,
`writeFeature()`, `end()`) throws `InvalidArgumentError` (`01-core.md` 1.14).

## C-9. Naming

| Kind | Form | Example |
|---|---|---|
| Type | PascalCase | `ObjectRecord` |
| Method | camelCase | `boundingBox()` |
| Enum type | PascalCase, `enum class` | `LayerKind` |
| Enumerator | PascalCase | `LayerKind::Semantic` |
| Constant | `k` prefix | `kFromBeginning` |
| Free function | camelCase | `dataTypeName()` |
| Header | lower_snake | `object_record.hpp` |

Getters take no `get` prefix (`name()`, not `getName()`); setters take `set`
(`setLabel()`). Boolean getters read as predicates (`isValid()`, `hasLabel()`).

These are assumed to match SlideIO's existing style and should be confirmed
against its headers before implementation; consistency with the library this
one extends matters more than the specific choices.

## C-10. Builders and fluent configuration

Objects with many optional settings use a fluent builder returning `*this` by
reference. Builders are ThreadConfined; what they build is Immutable or
ThreadSafe.

```cpp
ObjectQuery q;
q.layer(nuclei).intersecting(viewport).visibility(StageVisibility::IncludeInFlight);
```

Setters in a fluent chain never validate: the only exception one may throw is
`std::bad_alloc`, since a setter may allocate (an `ObjectQuery` setter can, to
store the value behind its pimpl). Validation happens at the terminal operation
(`build()`, `run()`, or the call consuming the query), so an error names the
whole configuration rather than one setter.

## C-11. Const-correctness and `noexcept`

Every method that does not modify observable state is `const`. Accessors that
cannot fail are `noexcept`. Destructors are `noexcept` implicitly and must not
throw — `close()` exists on every resource-holding class so that failures
during teardown are reportable rather than swallowed.

## C-12. Documentation

Every public entity carries Doxygen (UR-API-7), and the documentation build
must produce no undocumented-entity warnings. Each entity documents its
parameters, its return value, the exceptions it throws, its thread-safety
level, and the requirement IDs it implements.

## C-13. Versioning constants

Three independent version lines, three constants:

```cpp
const char* libraryVersion() noexcept;      // semver, UR-API-6
int32_t     storeFormatVersion() noexcept;  // UR-RES-12
int32_t     standardVocabularyVersion() noexcept;  // UR-RES-17
```

## C-14. What this document does not fix

Header include order, formatting, the clang-format configuration, and internal
(`detail/`) naming. Those belong to the implementation plan.
