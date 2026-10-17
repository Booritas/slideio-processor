# Storage measurements — the two items question 4 left open

**Status:** measured; consequences applied
**Date:** 2026-10-10
**Closes:** the two measurements named in
[the 2026-10-07 spike](2026-10-07-spike-q3-q4-findings.md) §2.5 and in SDS 03
§3.12
**Touches:** SDS [03](sds/03-storage.md) §3.7 and §3.12 ·
[sds/README](sds/README.md) · `requirements.md` version 0.10, the question 4
resolution, UR-EXE-8

The 2026-10-07 spike resolved open question 4 **yes, conditional on the
physical-key scheme**, and named two things it had not measured: whether a
unique index on a random 64-bit key gives back enough of the 4.1× insert
penalty to matter, and the annotation write rate, which had no numbers at all
although UR-EXE-8's budget spans two ten-million-row write passes. Both are now
measured. The first confirmed the schema SDS 03 already specified. The second
changed two schemas and surfaced a factor larger than anything else in the
design.

Everything built here is throwaway and lives outside the project tree. The raw
logs and benchmark sources are in
[`2026-10-10-storage-measurements/`](2026-10-10-storage-measurements/), because
figures now quoted in the SDS should not have their only evidence in a
session-scoped temporary directory.

---

## 0. What was found

| # | Finding | Factor |
|---|---|---|
| 1 | Monotonic rowid key with a unique index on the anchor beats the anchor as primary key | **2.39×** faster insert, store **103 MB smaller** |
| 2 | Sorting each batch by anchor, proposed as a cheaper partial measure, is **worse** than not sorting | 0.84× |
| 3 | Annotation tables want `WITHOUT ROWID` on the natural key — the **reverse** of finding 1 | **2.6×** smaller |
| 4 | Interning measurement names halves the annotation store | **2.04×**, 808 MB per slide |
| 5 | **Write order dominates everything** — ordered against shuffled annotations | **24.6×** |

Finding 5 is the one that matters. It is larger than every schema decision in
this document combined, and it is not a property of the store at all.

---

## 1. Apparatus

**Storage, which is the whole measurement.** Two drives were used and neither
is UR-EXE-8's reference NVMe:

| | Device | Type | State during the run |
|---|---|---|---|
| **C:** | Crucial CT1000BX500SSD1 | DRAM-less consumer SATA SSD | 89% → 92% full |
| **F:** | Seagate ST3000DM001 | 3 TB spinning disk | 75% full |

C: is close to the worst realistic case for this workload: no DRAM cache, a
dynamic SLC write cache that shrinks as the drive fills, and roughly 30 GB
written to it over the session. F: was used only to re-run one comparison that
would not stabilise on C:, and it is slower in absolute terms but far more
*consistent*, which is what that comparison needed. **No absolute figure here
should be carried into a budget.** The ratios travel; the seconds do not.

**Software.** SQLite 3.50.4 amalgamation, MSVC 2022, `/O2 /MD`. Pragmas on
every connection, matching the 2026-10-07 spike so the two sets are comparable:
`journal_mode=WAL`, `synchronous=NORMAL`, `cache_size=-262144` (256 MB),
`temp_store=MEMORY`, `mmap_size=1073741824`. Batches of 10,000 rows per
transaction throughout.

**Dataset.** The reference slide — 100,000 × 80,000 px, 1024 px tiles — with
10,000,000 objects of 200-byte geometry, placed by a seeded xorshift so every
variant sees an identical anchor sequence. 71,228 of them (0.71%) collide on
the anchor and are rejected, leaving **9,928,772 objects**; see §4.3 of the
2026-10-07 spike for why that rate is an artifact and the invariant behind it
is not.

**Fairness checks.** Two, both passed. Every insert variant reported exactly
71,228 collisions, which confirms they saw the same anchors. Every repeated
annotation pass reported store growth identical **to the byte** — +1,585,762,304
for text names and +778,240,000 for interned ones, across four runs on two
drives.

---

## 2. Measurement 1 — the physical key scheme

> *Does a unique index on a random 64-bit key give back enough of the 4.1× to
> matter?* — SDS 03 §3.12

Three schemes, 10M objects each, C:.

| Mode | Scheme | Insert | Rate | Store |
|---|---|---|---|---|
| 0 | anchor **is** the `INTEGER PRIMARY KEY` | 25.9 min | 6,435 obj/s | 3.126 GB |
| 1 | mode 0, each batch sorted by anchor first | 30.7 min | 5,432 obj/s | 3.113 GB |
| 2 | **monotonic rowid PK + `UNIQUE(layer, anchor)`** | **10.8 min** | **15,409 obj/s** | **3.023 GB** |

**Yes — 2.39×, and it costs nothing in space.** Mode 2's store is 103 MB
*smaller* than mode 0's despite carrying an extra index, because random inserts
leave primary-key pages roughly two-thirds full while appended rowid pages pack
nearly full. The index is narrow; the slack it replaces was not.

**The cheaper partial measure is refuted.** SDS 03 §3.12 suggested sorting each
batch by anchor before insert. It measured 18% *slower*. Ten thousand keys
sorted inside a batch that spans the whole 64-bit space still land on ten
thousand different pages, so the sort buys no locality and costs its own time.
That sentence has been removed from the spec and replaced with this result.

**Queries.** Join cost, isolated as *join minus R\*Tree* within one store,
because that is the only comparison the data supports (see §4.2):

| Viewport | mode 0 | mode 1 | mode 2 |
|---|---|---|---|
| 1,024 px | 3.57 ms | 4.20 ms | **1.83 ms** |
| 4,096 px | 28.02 ms | 28.03 ms | **13.64 ms** |
| 16,384 px | 334.92 ms | 354.67 ms | **192.48 ms** |

**1.7–2.0× cheaper**, reproducing the spike's ~2× and confirming that ordered
rowids make the geometry join a clustered read.

**The one price is the point lookup.** `get(ObjectId)` in mode 2 resolves
through the index and then the table, paying one extra B-tree descent:
**11.3 µs against 5.1 µs**, stable across repeated runs and therefore
structural rather than cache. Six microseconds on an incidental path against
fifteen minutes on the bulk one.

**The first attempt to measure that was invalid and is worth recording.** It
probed synthetic anchors and hit 31 of 20,000, so it timed the *miss* path —
where mode 2 stops at the index and never takes the extra hop — and reported no
difference between the schemes. Sampling the keys from the store first made the
cost visible. A benchmark that reports "no difference" deserves a check that it
exercised the thing it was built to measure.

---

## 3. Measurement 2 — the annotation write rate

Six passes against fresh copies of the mode-2 store, so each annotation pass
contends with a real 3 GB objects table for page cache, as it will in
production. One label assertion per object; three measurement assertions per
object.

### 3.1 The annotation tables want the opposite schema to `objects`

| `label_assertions` scheme | Ascending | Shuffled | Store |
|---|---|---|---|
| **`WITHOUT ROWID`, PK(object, stage, label)** | **56.4 s** | **23.1 min** | **+137 MB** (13.8 B/row) |
| rowid PK + `UNIQUE INDEX(object, stage, label)` | 61.8 s | 29.0 min | +352 MB (35.5 B/row) |

The clustered form wins on both orders and is **2.6× smaller**. This inverts
measurement 1, and the reason is row width: an object row carries a 200-byte
geometry blob, so a unique index duplicates only the key and the insert-order
gain dominates; an annotation row is three integers, so a unique index
duplicates very nearly the whole row and you maintain two B-trees to hold it.

Applying §3.12's conclusion mechanically to these tables would have cost 215 MB
per slide and time on every pass. **A measured result is scoped to what it
measured.**

### 3.2 Interning measurement names halves the store

Thirty million measurement assertions, name stored as TEXT on every row against
interned to an id:

| | Store growth | Per row |
|---|---|---|
| `name TEXT` on every row | **+1,586 MB** | 53.2 B |
| interned `name_id` | **+778 MB** | 26.1 B |

**808 MB saved per slide**, reproducing byte-for-byte across four runs. This is
UR-RES-17's reasoning — the per-object cost of a name must not scale with the
length of the name — which SDS 03 §3.4 applies to labels and which nothing had
applied to measurement names. It applies equally to the `measurements` table,
the producing stage's own; the assertion table was measured because it is the
one with a ten-million-row pass behind it.

Where the timing was reproducible the interned form was also **about twice as
fast** — the same factor as the size, which is the expected result for a pass
whose cost is bytes written. Getting to a reproducible timing took three
attempts; see §4.1.

### 3.3 Write order dominates everything

The same ten million label assertions, same schema, differing only in the order
the objects are visited:

| Order | Time | Rate |
|---|---|---|
| ascending `object_id` | **56.4 s** | 176,032 rows/s |
| random | **23.1 min** | 7,163 rows/s |

**24.6×.** Larger than every schema decision in this document put together, and
larger than the 4.1× that forced the objects-table redesign.

UR-EXE-8 allows 30 minutes for segmentation and classification together. The
object pass costs 10.8 minutes. An ordered annotation pass adds a minute and the
budget is comfortable; a shuffled one adds 23 and the budget is gone. **So
getting annotations into the store in object order is a
correctness-of-schedule property of an annotating stage, not a tuning option** —
which is now recorded in SDS 03 §3.7 and as a note on UR-EXE-8, and which
`04-workspace.md` §4.4 still does not state.

This also corrects a claim made earlier in the same session, that the two write
passes would consume 21.6 of the 30 minutes and leave about 8 for everything
else. That extrapolated the object pass's cost onto the annotation pass. Ordered,
the annotation pass is a rounding error on the object pass.

### 3.4 The realistic order

Neither measured order is what a real run does. Workers complete tiles out of
sequence, but a tile's objects are *contiguous* in `object_id`, because the
producing stage emitted them tile by tile — so the store sees a few thousand
ascending runs rather than ten million random probes. 7,745 runs of about 1,282
objects, shuffled:

| Order | Insert | Rate |
|---|---|---|
| ascending `object_id` | 56.4 s | 176,032 rows/s |
| **tile-shuffled — the realistic case** | **36.6 s** | **269,814 rows/s** |
| random | 23.1 min | 7,163 rows/s |

**The realistic order behaves like ascending, not like shuffled.** It came out
*faster* than ascending here, which is drive variance rather than a real
advantage — the two differ by less than the spread seen between identical
repeats elsewhere in this document (§4.1). What matters is the direction and
the magnitude: a few thousand ascending runs recover essentially all of the
locality, and the 24.6× penalty needs genuinely random arrival, which no
tile-sweeping stage produces. Store growth is marginally larger, +139.8 MB
against +137.0 MB, the cost of 7,745 insertion points rather than one.

The measurement pass behaves the same way. Thirty million interned measurement
assertions in tile order took **117.3 s** and added 762 MB — faster than any
of the ascending runs of §3.2, which is drive variance again, but firmly in the
same regime rather than the shuffled one.

**UR-EXE-8 is therefore comfortable on this axis.** Summing the measured write
work at reference scale:

| Pass | Time |
|---|---|
| 10M objects | 10.8 min |
| 10M label assertions, tile order | 0.6 min |
| 30M measurement assertions, tile order | 2.0 min |
| **Total committed writes** | **~13.4 min** |

That leaves roughly 16 minutes of the 30 for decode, compute and seam merge —
on a near-full consumer SATA SSD, where UR-EXE-8 names an NVMe.

The ordering constraint of §3.3 is still worth stating, and this result is the
reason why rather than a reason not to. The budget is met because of how stages
*happen* to be scheduled, and nothing enforces it: a stage that annotates by
walking a spatial query, or one that reorders work across tiles, lands in the
shuffled case and loses the budget. The constraint is cheap to honour and
expensive to discover.

---

## 4. Results that are not trustworthy

Recorded because a measurement document that reports only its good numbers
teaches the reader to trust the bad ones.

### 4.1 The C: inversion, unexplained

The text-name against interned-name comparison would not stabilise on C:. Text
came in at 43.6 s then 41.4 s — stable. Interned came in at 157.0 s then
415.2 s — a 2.6× spread between identical runs, and in both cases the *smaller*
table was slower, which has no mechanism behind it.

My first explanation was wrong: I attributed it to the WAL checkpoint sitting
outside the timed region, with Windows write-back absorbing an unequal share of
two passes differing 2× in bytes written. Moving the checkpoint inside the timed
region disproved it — the checkpoint took 0.0 s and 1.0 s. There was nothing to
absorb.

Running the two schemes **interleaved** on F: resolved the comparison: text
413.3 s then 376.5 s, interned 187.3 s then 213.8 s. Interned is consistently
~2× faster, matching its 2× size advantage, which is the coherent answer for a
bandwidth-bound pass. The C: behaviour remains unexplained; the plausible
candidate is the BX500's DRAM-less design, whose dynamic SLC cache shrinks
sharply near capacity, making throughput depend on write history rather than on
the workload. That is a hypothesis, not a finding.

### 4.2 Cross-store query times are contaminated by cache warmth

The three measurement-1 stores were queried in write order, so m0 had been
sitting 40 minutes and m2 one. Mode 0's R\*Tree-only time is 7× mode 2's on a
table that is byte-identical in all three stores, which can only be OS page
cache. Only the *join minus R\*Tree* figure within a single store is quoted
above.

### 4.3 Per-object annotation read times are noise

0.0043, 0.0091, 0.0157, 0.0043 ms across the four label passes, with no
consistent ordering. The slowest is the rowid-plus-index scheme, which is the
expected direction, but the spread between two runs of the *same* schema is
larger than the difference between schemes. No claim is made from them. The
schema decision of §3.1 rests on size and write time, both of which are
decisive and reproducible.

### 4.4 Absolute times, generally

Every figure here is from a near-full consumer SATA SSD or a spinning disk.
UR-EXE-8 names an NVMe reference configuration precisely because this workload
is storage-bound. Treat the absolutes as a floor.

---

## 5. Consequences applied

| Document | Change |
|---|---|
| SDS 03 §3.12 | Measurement 1 recorded: the three-scheme table, 2.39×, the smaller store, the 6 µs lookup. The batch-sort proposal replaced by the result refuting it. The `objects` row of the schema table corrected — it still described the anchor as the primary key. `measurement_names` added. Store size at reference scale, ~3.9 GB per slide, with a note that no disk budget exists. |
| SDS 03 §3.7 | Both assertion tables specified `WITHOUT ROWID` on their natural key, with the measured justification and an explicit note that this inverts §3.12. Measurement names interned, extended to `measurements`. Write order written up as a scheduling constraint. |
| `sds/README.md` | Both measurements closed; SDS 03 freezable. The realistic write order named as the one input still resting on an argument. |
| `requirements.md` | Version 0.10. Question 4's resolution row carries both results. UR-EXE-8 gains a note making annotation write order an explicit condition of its budget. No requirement added or renumbered. |

---

## 6. What remains open

- **`04-workspace.md` §4.4 does not state the ordering constraint** that
  UR-EXE-8 now depends on. The `StageRunner` sketch binds an annotation writer
  without saying anything about the order in which objects reach it.
- **No disk budget exists anywhere.** The store measures ~3.9 GB per slide
  before rasters, and §3.8's persisted rasters are unbounded. UR-EXE-2 caps
  memory and nothing caps storage.
- **Open question 5 — geometry encoding** — is untouched and is where store
  size is actually decided. WKB against a compact custom encoding against
  simplified contours, at 200 bytes per object and ten million objects.
- **The annotation read path is unmeasured** in any trustworthy way (§4.3).
  It did not block the schema decision and may matter for UR-OBS-1's viewer.

---

## 7. Files

In [`2026-10-10-storage-measurements/`](2026-10-10-storage-measurements/):

| File | Contents |
|---|---|
| `measurement-1-physical-key.log` | Three insert schemes, query benchmarks, store sizes |
| `measurement-2-annotations.log` | Six annotation passes: two label schemas × two orders, two name schemes |
| `measurement-2-rerun-checkpoint-timed.log` | The re-run that disproved the write-back explanation |
| `measurement-2-interleaved-on-hdd.log` | Text against interned, interleaved on F:, two rounds |
| `measurement-2-tile-order.log` | The realistic write order |
| `q5_bench.c` | Measurement 1 — three physical key schemes |
| `q5_hit.c` | The corrected hit-path point lookup |
| `q6_bench.c` | Measurement 2 — annotation schemas and write orders |

The stores themselves — nine of them, about 51 GB — were deleted after the
figures were extracted. `q5_bench.c` regenerates the base store in roughly 11
minutes.
