# Draft — Section 9a: Live observation of a running analysis

**Status:** **MERGED** into `requirements.md` v0.2 on 2026-09-29. Superseded —
do not edit. `requirements.md` §9a is now the normative text.
**Date:** 2026-09-28
**Arises from:** [Module structure design](superpowers/specs/2026-09-28-module-structure-design.md)

This draft added one new section and amended two existing statements. It was
kept separate so the numbered requirements in `requirements.md` were not edited
without review.

**What was merged:** §9a verbatim; amendments A1, A2 and A3 as proposed; open
questions 12–14, renumbered into `requirements.md`'s own table. **What was
not:** the traceability table below, which maps requirements to modules and
components and therefore belongs with the design rather than the requirements —
`requirements.md` carries no traceability tables, and the per-module component
tables in the module structure design already cover the same ground. It is
retained here for reference.

---

## Motivating scenario

Two processes run concurrently against one workspace. A **script** process —
typically Python — runs a stage and writes objects. A **viewer** process
displays the slide and overlays results, reacting as layers are declared and
objects are produced, querying their geometry to draw them.

The viewer may start before, during or after the script; may be closed and
reopened; may run alongside other observers; and must not slow the script down
or be able to corrupt what it writes.

The current document does not cover this. Section 15 records concurrent readers
alongside a writer as desirable but not required, and UR-STG-5 forbids partial
objects being visible, without distinguishing an observer from a later stage.

---

## 9a. Observation

- **UR-OBS-1 (Must)** — A store being written by one process shall be readable
  concurrently by other processes. Readers shall neither block the writer nor
  be blocked by it, and shall observe a consistent snapshot rather than a
  partially applied transaction. Concurrent *writers* remain out of scope.
  *Acceptance:* while a stage writes the reference object count, a second
  process issues spatial queries continuously throughout and observes no error,
  no partially written batch, and no measurable reduction in writer throughput.

- **UR-OBS-2 (Must)** — The store shall hold an **event journal**: append-only,
  monotonically sequenced, and readable by any process that can read the store.
  Each event shall be written **in the same transaction as the data it
  describes**, so that an event and its data commit or fail together.
  *Acceptance:* a test asserts that no committed object batch lacks its event
  and no event references objects absent from the store, including after the
  writer process is killed at randomised points during a stage.

  > Stated as atomicity rather than as a delivery guarantee because this is the
  > property that makes the journal usable as a source of truth. A notification
  > sent separately from the write it announces can be lost, duplicated or
  > reordered relative to the data; one committed with it cannot.

- **UR-OBS-3 (Must)** — Events shall be readable through a forward-only
  **cursor** keyed on sequence number, in bounded memory. A subscriber shall be
  able to begin at any sequence, including the beginning of the journal or its
  current end, and shall be able to persist its position and resume from it in
  a later process.
  *Acceptance:* a subscriber started after N batches have been written replays
  all N in order; a subscriber stopped and restarted from its persisted
  position resumes with no gap and no duplicate; three independent subscribers
  at different positions each observe the complete sequence from their own
  starting point.

- **UR-OBS-4 (Must)** — Objects shall become visible to observers at
  **batch-commit granularity** while the producing stage is still running.
  Queries shall by default return only objects of completed stages, so that
  UR-STG-5 holds unchanged for later stages; an observer shall opt in
  explicitly to objects of in-flight stages. When a stage fails, is cancelled,
  or is re-run, the journal shall record a discard event, and the objects that
  stage wrote shall cease to be returned by any query.
  *Acceptance:* an observer sees objects appear during a running stage; a later
  stage querying the same layer while that stage runs sees none of them;
  killing the stage produces a discard event, after which the objects are
  returned by no query, with or without the opt-in.

- **UR-OBS-5 (Must)** — The journal shall record at least: **layer declared**,
  **objects added**, **edges asserted**, **label added**, **stage state
  changed**, **stage discarded**, and **primitive layer dropped**. An
  objects-added event shall carry the layer, the producing stage, the object
  count and the union bounding box of the batch, so that an observer can decide
  whether the batch is relevant before querying its geometry. Events shall not
  carry object geometry.
  *Acceptance:* each event kind is emitted and observed in a test; an observer
  whose viewport does not intersect a batch's bounding box issues no query for
  it; journal size for the reference object count is a documented and measured
  fraction of store size.

- **UR-OBS-6 (Must)** — Any mechanism used to notify a subscriber that new
  events exist shall be **local to the machine** and scoped by operating-system
  permissions. The library shall open no network socket. **Correctness shall
  not depend on that mechanism**: a subscriber that only polls the journal
  shall observe every event, in order, with no loss.
  *Acceptance:* no listening network socket exists in either process during a
  subscribed run, verified by inspection; with the notification mechanism
  disabled by configuration, a polling subscriber observes an event sequence
  identical to one observed with it enabled.

  > Whole-slide images routinely carry patient identifiers in their metadata.
  > A library that opens a localhost listener as a side effect of subscribing
  > to events creates an exposure that costs nothing to avoid, and one that a
  > downstream integrator handling identifiable data under the HIPAA Security
  > Rule or GDPR would have to assess and document. Keeping the channel to a
  > named semaphore or Unix domain socket keeps it inside the machine's own
  > permission model.

- **UR-OBS-7 (Should)** — The journal shall have a documented retention policy,
  and the store shall support truncating it below a recorded watermark without
  affecting objects, edges or stage state. A subscriber whose position has been
  truncated away shall be told so explicitly rather than silently resuming from
  a later point.
  *Acceptance:* truncating the journal of a store holding the reference object
  count leaves every query result unchanged; a subscriber resuming from a
  truncated position receives a documented error naming its position and the
  oldest retained sequence.

---

## Amendments to existing requirements

### A1 — Section 15, non-goals

The row currently reads:

> | **Concurrent writers to one workspace** | One writer at a time. Concurrent readers alongside a writer are desirable but not required in v1. |

Proposed replacement:

> | **Concurrent writers to one workspace** | One writer at a time. Concurrent *readers* alongside a writer are **required** — see UR-OBS-1. |

Assumption 6 ("a workspace is written by one process at a time") is unchanged
and remains correct. Only the status of concurrent reading changes.

### A2 — UR-STG-5

UR-STG-5 currently requires that a stage interrupted before completion leaves
the workspace in its prior consistent state, "with no partial objects visible
to later stages". That wording is retained. Proposed added note:

> An **observer** is not a later stage. Objects of an in-flight stage are
> visible to an observer that opts in under UR-OBS-4, and remain invisible to
> later stages by default. UR-STG-5 is satisfied by the query filter rather
> than by rollback, so an interrupted stage's objects may be physically present
> in the store while being returned by no stage-facing query, until the discard
> of UR-OBS-4 removes them.

### A3 — UR-QA-3, informative note only

No change to the requirement. The note may usefully record how invariance is
achieved, since the design settled it:

> Object identifiers are derived from each object's anchor pixel — its
> topmost-then-leftmost pixel in level-0 slide coordinates — which is a
> property of the geometry alone and therefore invariant to tile size and
> worker count by construction. Identifiers keyed off tile index or write order
> are **not** tile-size invariant and do not satisfy this requirement.

---

## Traceability

| Requirement | Module | Component |
|---|---|---|
| UR-OBS-1 | `storage` | `Store` (WAL, per-thread connections) |
| UR-OBS-2 | `storage` | `EventJournal`, `StoreSink` |
| UR-OBS-3 | `storage` | `EventCursor` |
| UR-OBS-4 | `storage`, `workspace` | `QueryBuilder`, `StageRunner`, `RerunPolicy` |
| UR-OBS-5 | `storage` | `EventJournal` |
| UR-OBS-6 | `storage` | `EventCursor` wake-up signal |
| UR-OBS-7 | `storage` | `Store` retention |

## Open questions raised by this section

| # | Question | Why it matters |
|---|---|---|
| 12 | Should an observer be able to *cancel* a running stage, or only watch it? | Turns the viewer from an observer into a controller, which needs a second channel and a permission model. Out of scope as drafted. |
| 13 | What is the retention default — unbounded, a row count, or truncate-on-stage-complete? | Trades journal size against how far back a late-joining observer can replay. |
| 14 | Should the journal record read activity, or only writes? | Relevant only if an audit trail is ever wanted; adds write load for no benefit to the stated scenario. |
