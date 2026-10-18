# Software Design Specification — `slideio-processor`

**Status:** draft for review
**Date:** 2026-09-28
**Parent:** [Module structure design](../2026-09-28-module-structure-design.md)

The public API of all seven modules. The module structure document decides
*what the modules are and why*; these documents decide *what they expose*.

## Documents

| # | Document | Module | Library |
|---|---|---|---|
| 00 | [Conventions](00-conventions.md) | — | — |
| 01 | [core](01-core.md) | `core` | data |
| 02 | [config](02-config.md) | `config` | data |
| 03 | [storage](03-storage.md) | `storage` | data |
| 04 | [workspace](04-workspace.md) | `workspace` | engine |
| 05 | [pipeline](05-pipeline.md) | `pipeline` | engine |
| 06 | [algorithm](06-algorithm.md) | `algorithm` | engine |
| 07 | [reporting](07-reporting.md) | `reporting` | data |
| 08 | [Class diagrams](08-class-diagrams.md) | all | — |

**Read 00 first.** It fixes the ABI rules, string and collection conventions,
error handling, ownership and thread-safety annotations that every signature in
01–07 assumes. The per-module documents do not restate them.

`UR-OBS-*` resolves against `requirements.md` §9a, merged in v0.2 from the
observation draft. Documents 02, 03 and 04 cite it in fourteen places — the
event journal (3.9) and the read-only observer path rest on it.

## Reading order by purpose

| If you want to | Read |
|---|---|
| Review the whole API | 00, then 01–07 in order |
| See the shape before the detail | 08, then 00 |
| Write a viewer | 00, 01, 03 (§3.9 especially), 07 |
| Write a processing script | 00, 01, 05 (§5.4), 04, 06 |
| Add an algorithm | 00, 01, 05 (§5.1), 06 |
| Review the store format | 00, 01, 03 |

## Cross-cutting facts worth knowing before reading any module

- **Two libraries.** `slideio-processor-data` holds `core`, `config`,
  `storage`, `reporting` and links no OpenCV. `slideio-processor-engine` holds
  `pipeline`, `algorithm`, `workspace`. A viewer links only the first.
- **Object identifiers derive from geometry**, not from a counter — packed from
  each object's topmost-then-leftmost pixel in level-0 slide coordinates. This
  is what makes results byte-identical across worker counts and tile sizes
  (UR-QA-3), and it makes identifiers sparse, large, and composite with the
  layer.
- **Nothing materialises a result set.** Every read path is a cursor. The one
  exception is `reporting::materialise`, which demands a filter and an explicit
  cap and throws rather than truncating.
- **Events are rows, not messages.** The event journal is a table in the store,
  written in the same transaction as the data it describes. Subscription is a
  cursor over a sequence number.
- **`algorithm` never links `storage`.** Operations emit into
  `core::ObjectSink`, so transaction and batching discipline exists in one
  implementation that no algorithm author can bypass.

## Status of open questions

These remain unresolved in `requirements.md` and are noted where they touch an
interface. None of them changes a signature here; all of them could change an
implementation.

| # | Question | Touches |
|---|---|---|
| 5 | How is geometry encoded in the store? | 03 §3.12 |
| 6 | Is `Float16` computed natively or promoted? | 02 §2.3, 06 |
| 15 | Canny's hysteresis against UR-PIX-3's untolerance'd acceptance | 06 §6.4 |

Two design questions were raised while `core` was implemented. They come from
the SDS rather than from `requirements.md`, so they carry no question number:

| Question | Touches |
|---|---|
| At coarse levels the mask dilation is rounded up to whole level pixels, so the widening in level-0 pixels depends on the level | 01 §1.17 |
| `MemoryBudget::reserveOrWait` gives no fairness guarantee, so a large request can starve behind smaller ones | 01 §1.11 |

Questions 3, 4, 7, 8, 9 and 10 were open when this document was written and are
now resolved: 3 by the scene-source extension seam, 4 by the spike plus the
physical-key change of 03 §3.12, 7 by the twenty-two-operation catalog of 06
§6.10, 8 by the ten-label vocabulary of 02 §2.2, 9 as one slide per workspace,
and 10 as UR-EXE-8's 30 minutes. Question 16 was raised later, when question 3
closed, and is resolved as no: OME-TIFF conversion rejects a channel range of
mixed types (SlideIO PR #86), so a mixed-type scene exports to OME-TIFF one
same-type channel range per file. SVS rejects a mixed-type scene outright.

**Both outstanding measurements were taken on 2026-10-10 and SDS 03 can be
frozen.** The rowid-versus-anchor physical key of 03 §3.12 confirmed the
specified scheme: 2.39× on insert, 1.7–2.0× on the geometry join, a store
103 MB smaller, 6 µs added to `get(ObjectId)`. The annotation pass of 03 §3.7
changed two schemas — both assertion tables become `WITHOUT ROWID` on their
natural key, and measurement names are interned, saving 808 MB per slide — and
surfaced the largest factor in the whole design: annotations written in object
order cost 56 seconds where shuffled ones cost 23 minutes.

**The realistic write order was then measured as well** — tiles out of
sequence with each tile's objects contiguous — at 36.6 seconds, which is the
ascending case rather than the shuffled one. UR-EXE-8 holds with room: 10.8
minutes for the object pass and under a minute for the annotation pass against
a 30-minute budget. Full results and apparatus in
[`../2026-10-10-storage-measurements.md`](../2026-10-10-storage-measurements.md).

## Not specified here

Schema DDL, internal `detail/` types, the CLI's command surface, the CMake
target layout, and algorithm internals. Those belong to the implementation
plan.
