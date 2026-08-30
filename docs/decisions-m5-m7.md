# EXT-08 — decisions taken during M5–M7

Every judgment call made while the project owner was away, with what it turned on and what
would reverse it. This is a working record, not a contract: where a decision changes something
the PRD or `docs/capture-format-v1.md` says, those documents are edited too and this file says
which. Where a decision is provisional, it says what would settle it.

The four decisions the project owner made directly before M5 began are recorded first, so the
chain is complete.

---

## Decided by the project owner (pre-M5)

| # | Question | Answer |
|---|---|---|
| PO-1 | Internal queue: CLI flags at M5, or constants until M6? | **Flags now.** `--queue-size` and `--overflow-policy` land at M5 with provisional defaults. Both are already in the PRD's authorized CLI table, so no new surface |
| PO-2 | Bus-side policy at M5, or leave on `KEEP_LATEST` until M6? | **`FIFO_DROP`, explicit at every call site, now.** ADR-4 already rejects `KEEP_LATEST` in principle, and it is the single unrepeatable event messages M5's acceptance depends on that it would discard |
| PO-3 | Every ordinary run now contains two segments, because the engine's stop path reloads | **Record it faithfully.** Segment 0 is the run; segment 1 is the teardown reload |
| PO-4 | Three spec edits beyond the §16 conformance update | **All three approved:** expand §5.1 on the clock reset, add `trailer.drops.events_not_recorded`, fix the stale §1 determinism claim |

---

## M5 — output path and lifecycle

### D-1 — The segment boundary keys on **both** events, with a staging area between them

**Status:** Decided, and it supersedes what was proposed to the project owner.

What was proposed was "key on `scenario_loaded` only", on the strength of a first observation
showing the teardown `entity_created` burst arriving between `scenario_unloaded` and
`scenario_loaded`. A second, targeted observation showed the same thing happens at **bring-up**:

```
seq 6   scenario_unloaded  ""                     <- bring-up noise, empty scenario name
seq 7..48   entity_created x 42                   <- the burst, BEFORE the load event
seq 49  scenario_loaded    "Atacama Air Defense"
seq 62  engine_started     idle->running          (11 samples already through by here)
```

and at teardown:

```
seq 136033  engine_stopped     running->idle   simT=200.05
seq 136034..136052  entity_deleted x 19  reason=scenario_unload  simT=0.0
seq 136053  scenario_unloaded  "Atacama Air Defense"             simT=0.0
seq 136054..136095  entity_created x 42                          simT=0.0
seq 136096  scenario_loaded    "Atacama Air Defense"             simT=0.0
            then 378 entity-state samples, all simT=0.0
```

So the rule is uniform: **the `entity_created` burst that materialises a scenario precedes the
`scenario_loaded` that announces it.** The load event is the completion announcement, not the
start of the work.

Keying only on the load event would therefore put every creation burst in the *previous*
segment, or — at bring-up, where there is no previous segment — outside any segment at all,
which `docs/capture-format-v1.md` §7 calls malformed.

**Decision.** The **close** keys on `scenario_unloaded`; the **open** keys on `scenario_loaded`;
and a bounded staging area holds the `entity_add` / `entity_remove` records that arrive between
them, flushing them into the segment that opens next. A `scenario_unloaded` carrying an **empty
scenario name** is bring-up noise and is ignored outright (notes.md, M1).

This is what BTB-CX-4's requirement text actually says — *"WHEN a `scenario_loaded` or
`scenario_unloaded` event arrives … close any open segment … and open a new one"* — with the
staging area supplying the piece the requirement did not anticipate. It satisfies both of its
acceptance criteria, keeps every record inside a segment, and puts each creation burst in the
segment it belongs to rather than the one that happened to be open.

**What would reverse it:** a platform release that publishes `scenario_loaded` before it
materialises entities. The staging area would then always be empty and could be deleted.

### D-2 — A sample arriving while records are staged forces the segment open

The staging area exists for roster records only. A `sample` can never wait in it: if one
arrives while no segment is open, the segment opens immediately (the state model's attach-
mid-run branch), the staged roster records flush into it, and the sample follows. That is what
bounds the staging area in practice as well as in principle — it cannot grow with the sample
stream, only with a creation burst, which is entity-count sized (42 on the reference scenario,
126 on the overload one).

### D-3 — Host loss is detected on `sim/engine/state` silence, window **3.0 s**

**Derived, not guessed.** Measured across two full bring-up → load → run → teardown cycles:

| | `Atacama Air Defense` (200 s) | `Outback Kamikaze Swarm` (30 s) |
|---|---:|---:|
| engine-state messages | 4 017 | 617 |
| nominal period | ~51 ms (19.5/s) | ~51 ms |
| **largest inter-arrival gap** | **548 ms**, at scenario load | 408 ms, at scenario load |
| gaps over 150 ms, whole run | 2 | 7 (mid-run jitter to 305 ms) |

The window is **3.0 s** — 5.5× the largest gap observed anywhere, and ~59 heartbeat periods.

Two things this measurement settled that reasoning would have got wrong:

- **The load stall does not scale with entity count.** The 126-entity scenario stalled *less*
  at load (408 ms) than the 42-entity one (548 ms). A window derived by scaling the reference
  scenario's stall by entity count would have been too generous for no reason.
- **`sim/engine/state` publishes through idle frames**, before the scenario loads and after the
  engine stops — 4 017 messages across a 200 s run that was only *running* for part of it. That
  is why it is the heartbeat and `sim/entity/state` is not: entity state goes silent legitimately
  at every unload, so silence there means nothing.

Deliberately far below the bus's own `SubscriptionOptions::activityThresholdS` default of 30 s.
That is the platform's "this subscription looks idle" semantics; for a campaign runner doing 20+
unattended runs it is two orders of magnitude too slow to be a host-loss signal.

**What would reverse it:** a host whose legitimate stalls exceed ~1.5 s. Re-measure the largest
gap and re-derive; the number is a measurement, not a constant of nature.

### D-4 — Engine-state resolution failure is fatal

Resolving the engine-state topic gets its own message-name anchor (`simEngineState`, overridable
with `--engine-state-message`) and its own structural check, and failing to resolve it is a
named diagnostic and a non-zero exit rather than a degraded run.

The alternative — carry on without host-loss detection — produces exactly the failure BTB-CX-3
exists to forbid: a bridge that blocks indefinitely on a dead bus. A bridge that cannot detect
host loss should say so at startup, not at 3 a.m.

### D-5 — `BLOCK` is rejected at **both** boundaries, not only the bus

`docs/capture-format-v1.md` §14 already promises EXT-17, in writing, that *"this producer never
uses `BLOCK`"*, and ADR-4 gives the reason: a recorder that stalls the bus changes the run it is
recording. The same argument applies transitively to the internal queue — blocking it blocks the
handler, which stalls the bus delivery thread, which is the same perturbation by a longer route.

So BTB-BP-4's three permitted policies reduce to two in practice. `--overflow-policy` accepts
`drop_newest` and `drop_oldest`; `block` is rejected at parse time with the reason, rather than
silently absent.

### D-6 — Internal overflow policy defaults to `drop_newest`

Drop-oldest is `KEEP_LATEST`'s mistake wearing a different hat: it discards the sample already
committed to the run's history in favour of one that has not been written yet. That is the exact
reasoning ADR-4 uses to reject `KEEP_LATEST` at the bus boundary, and it does not stop being
true one thread later.

### D-7 — One queue for all topics, not one per topic

Per-topic queues would give per-topic FIFO trivially, but they would lose the *interleaving*
between topics — and the interleaving is what guarantees an `entity_add` is written before the
samples it opens. Any merge policy other than true arrival order can produce a file the format
spec calls malformed. A single FIFO queue preserves global arrival order, and per-topic FIFO
follows from it for free.

Per-topic drop attribution (BTB-BP-4) is kept by counting drops per kind rather than by having
separate queues.

### D-8 — The queue reserves headroom for roster and segment events

A sample burst that filled the queue could evict an `entity_created`, and a lost `entity_created`
orphans every subsequent sample for that name — turning a bounded sample loss into an unbounded
correctness loss. So the queue is bounded at `capacity + 1024`: samples are refused above
`capacity`, roster and segment records only above the hard bound. Both counted separately, both
in the trailer.

Still bounded, still counted, and the catastrophic loss becomes unreachable at any rate this
platform produces — the reference run's entire event traffic is 134 messages.

### D-9 — Default queue size 8192 records

~16 MB at M4's measured ~2 KB per held `StreamValueMap`. That is 10.0 s of headroom at the
reference scenario's 818 samples/s and 3.3 s at the overload scenario's 2 487/s, against a p95
enqueue-to-durable target of 250 ms — roughly 40× the target at the reference rate.

Provisional, and M6 owns confirming it under the overload (OQ-4).

### D-10 — `segment_close.reason` for a load-triggered close is `scenario_unloaded`

A `scenario_loaded` arriving while a segment is open implies the previous scenario was unloaded,
whether or not the unload event was seen. `scenario_unloaded` is the closed-set value that
describes it. Where an unload *was* seen and no load follows — an operator unloads and stops —
the segment is closed by whatever ends the capture (`host_lost` or `shutdown`), and the fact
that an unload preceded it is visible in the record order rather than in the reason.

### D-11 — The capture file is opened when the scenario name is known, not at startup

The filename convention is `capture-<scenario>-<run-label>.n8rocap.jsonl`, so the file cannot be
named until the scenario is. At bridge-first start the scenario is unknown for the first several
seconds, and the first records to arrive (the creation burst) precede the `scenario_loaded` that
names it.

The writer therefore opens the file at the first of: a `scenario_loaded` record (use its name),
or a `sample` record (attach-mid-run — use the client's last-known scenario name, or `unknown`).
The staging area from D-1 is what makes this work: it already holds pre-load roster records, and
D-2 already guarantees a sample forces the issue.

No clock is involved in the decision, so it does not reintroduce the timing dependence M4
removed from `attached_mid_run`.

### D-12 — Scenario slug is the full name, lowercased and hyphenated

`Atacama Air Defense` becomes `atacama-air-defense`, so the reference capture is
`capture-atacama-air-defense-000.n8rocap.jsonl`. M4's committed sample is
`capture-atacama-000.n8rocap.jsonl`, which was hand-named; the rule now has to be deterministic
and derivable from the scenario name alone, and truncating to the first word is neither.

Runs of characters outside `[a-z0-9]` collapse to a single hyphen and leading/trailing hyphens
are trimmed, so the transform is total and produces no surprising filenames.

### D-13 — `--capture-max-samples` is kept as a safety bound, and it is not BTB-CAP-6

The PRD's CLI table does not list a record-budget flag, and BTB-CAP-6 (bounded capture size) is
P2 and not in M5's scope. But an M5 run has to be able to end on something other than host loss,
and the flag already exists and is already documented from M4.

It is kept, unchanged, described as a safety bound rather than as CAP-6. CAP-6's actual content
— a size limit in bytes, the stop-or-rotate choice, and the statement of both in the `header` —
stays unbuilt. Flagged for the project owner: either the PRD's CLI table should gain this flag,
or CAP-6 should absorb it when it is built.

### D-14 — On host loss the bridge exits rather than returning to the wait state

BTB-CX-3 permits either ("exit cleanly or return to the wait-and-connect state as configured").
Exiting is chosen because a capture is closed by the host loss that ended it, and re-attaching
would mean either appending to a closed file or silently starting a second one — and deciding
which of those an operator wanted is a configuration surface nobody has asked for. A campaign
runner that wants another run starts another bridge, which is EXT-17's model anyway.

### D-15 — The internal-queue drop counters are in the file, and they are scheduler-dependent

BTB-CAP-3 says *"no scheduler-dependent or timing-dependent value is written into any record"*.
BTB-BP-4 says the internal queue's overflow count goes *in the trailer*. Under a streaming
writer these conflict: how much of the queue fills depends on how the writer thread was
scheduled.

Resolved in favour of BTB-BP-4, because `docs/capture-format-v1.md` §16 had already committed
to it in writing — *"From M5, when the producer gains a real writer queue, this field carries
that queue's genuine overflow count"* — and because tenet 3 ("loss is counted, never silent")
beats byte-identity for a number that is only ever non-zero when the capture is already an
incomplete record of its run.

The caveat is small and self-announcing: both counters read `0` on any run where nothing was
lost, which is every run a byte comparison is meaningful for. §14 now states the exception
explicitly rather than leaving a reader to find it.

**What stays out of the file** is the other scheduler-dependent number: samples arriving in the
*shutdown window*, between the subscriptions being cancelled and the bus actually stopping.
That can never be zero, and it is not a loss — those samples are after the end of recording,
which `end_reason` already says. It is counted separately and reported only in the log, which
is the same conclusion M4 reached about the same number.

### D-16 — `--capture-max-samples` defaults to 0, meaning unbounded

At M4 the budget was what ended a run, so it defaulted to 100 000. At M5 a run ends on host
loss, and the whole point of the milestone is to capture a run through its teardown — which is
where the second occupancy and the second segment live. A default budget would cut the
capture off before the evidence.

### D-17 — `--out-dir` is required; there is no report-only mode

M4's `--capture-out` was optional and its absence meant "report only". `--out-dir` is required
instead. The bridge's job is to produce a capture, and a mode that quietly produces nothing is
a mode somebody will run by accident. Anyone wanting diagnostics without an artifact can point
at a scratch directory.

### D-18 — The host-loss window is a documented constant, not a flag

BTB-CX-3 asks for a window that is "bounded, documented"; it does not ask for a configurable
one, and the PRD's CLI table does not list a flag for it. It is a named constant with its
derivation in the comment, in the README, and in D-3. Adding a flag would be surface the
contract does not authorise, for a value nobody has yet had a reason to change.

---

## M5 — what the measurements confirmed or corrected

Recorded because a decision that was only ever argued is worth less than one that was checked.

| Decision | Predicted | Measured |
|---|---|---|
| D-1 staging area | needed for the teardown burst | needed for **bring-up too** — the initial 42 creates also precede `scenario_loaded`. High-water 42 against a bound of 8192 |
| D-3 window 3.0 s | detection "within the window" | host loss declared at **3.0075 s** after a `taskkill /F`; trailer well-formed, exit 0 |
| D-3 stall scaling | load stall would grow with entity count | **wrong** — 126 entities stalled 408 ms, 42 entities stalled 548 ms. The window would have been 3× too generous if scaled |
| D-8 structural reserve | protects roster records under overload | **2 520 samples dropped, 0 events dropped** at `--queue-size 4`. Structure intact, file conforms |
| D-9 queue size 8192 | ~10 s of headroom at the reference rate | never exceeded **42** at the reference rate; the writer keeps up completely. Untested under real overload, which is M6's |
| PO-2 `FIFO_DROP` / 1024 | removes a known hazard | zero bus-side drops, same as `KEEP_LATEST` / 100 at M3. Neither is discriminating at this load — OQ-4 still needs M6 |
| BTB-BP-1 handler cost | p95 < 100 µs, unvalidated | **p50 ≤ 1 µs, p95 ≤ 5 µs, p99 ≤ 10 µs, max 160 µs** over 132 150 invocations |

One consequence nobody had written down, found in the late-attach run: **`entities_added` and
`entities_removed` do not balance in a capture from a bridge that attached mid-run** (45 against
3). The teardown deletes name entities whose creation the bridge never saw, and those cannot
become `entity_remove` records without producing a malformed file. It is now in notes.md so a
reader does not read the imbalance as corruption.

---

## M6 — the referee, live and offline

### D-19 — The condition file is JSON, and replay is what pays for the parser

BTB-REF-1 wants conditions declared outside the code; OQ-6 says design for EXT-08, document it
fully, and let EXT-17 adopt or supersede it. A line-oriented format would have needed no
parser, but `--replay` has to read a JSON capture anyway — so a JSON parser was going to exist
either way, and a second syntax would have been the extra cost rather than the saving.

`src/JsonParse.{h,cpp}` is a small DOM over the subset this project writes. Objects keep their
members in an **ordered** map, because nothing on the capture or verdict path may iterate an
unordered container (BTB-CAP-3), and a duplicate key is a named error rather than a
last-wins-or-first-wins guess nobody can discover.

### D-20 — Distance is 3D Euclidean over WGS-84 ECEF

BTB-REF-3 requires "a stated geodetic method, documented in the README, so a result is
reproducible by a third party". Positions are converted to earth-centred, earth-fixed
coordinates and the distance is the straight line between them.

Haversine was the obvious alternative and is wrong for the question: it ignores altitude, and
two aircraft 6 km apart vertically are not close. Vincenty answers the surface question,
iterates, and does not converge near-antipodally. ECEF is closed-form, has no convergence case,
takes altitude for free, and is reproducible from the formulae in `src/Geodesy.h` alone.

Checked against a published figure rather than our own output: one degree of latitude at the
equator comes out at 110 574 m.

### D-21 — Two region shapes: circle and polygon

BTB-REF-3 says "a declared geodetic region" without naming a shape. A circle alone would have
made "did red cross this corridor" inexpressible; a polygon alone would have made the common
case verbose. Both are ~30 lines given the distance function that proximity already needs.

**Boundary semantics, documented because the requirement demands it:** a point on a circle's
edge is inside (`<=`), and a point on a polygon's edge or vertex is inside. The polygon case
needed handling explicitly — ray casting alone gives an arbitrary answer on a vertex depending
on which way the parity falls, so an edge test runs first.

Polygons are treated as plane figures in lat/lon. Accurate at scenario scale, and stated
rather than hidden: one spanning the antimeridian or a pole is not supported.

### D-22 — One verdict per condition per run

At the first moment a condition is satisfied, or an explicit `met: false` at end of run. Not
re-emitted on every later sample that also satisfies it: "did the two aircraft come within
5 km" is answered by the first time they did, and re-emitting would have put thousands of
identical records in the capture.

BTB-REF-2 requires the not-met verdict, and it is the load-bearing half. Without it a condition
that was never satisfied and one that nobody evaluated look the same.

### D-23 — Verdicts are written twice, on purpose

Into the capture as `verdict` records, which is what the format specifies, **and** into a
`verdicts-<scenario>-<run-label>.jsonl` beside it.

The second is what makes BTB-REF-4 checkable as a file comparison. Replay produces no capture,
so without a separate verdict file "live verdicts equal replay verdicts" would have to be
checked by extracting records from one file and comparing them to another — a comparison whose
own correctness would then be in question. Two files, one `sha256`, no extraction step.

A replay writes `verdicts-<stem>.replay.jsonl`, so it never overwrites the live run's.

### D-24 — End-of-run verdicts are anchored on the last *data* record

Not the last record of any kind. A replay reading a capture also sees the `segment_close` and
the `trailer` that the live writer emitted *after* it decided its final verdicts, so anchoring
on "the last record" would have stamped the two paths differently. Anchoring on the last
`sample` / `entity_add` / `entity_remove` is the one point both paths reach identically.

One line, and it is the whole difference between byte-identical and nearly identical.

### D-25 — The referee gets an interface, and ADR-1 said not to

ADR-1 declined a pure-virtual seam for the entity picture, on the grounds that it buys
substitutability we have no second implementation for. `FieldSource` has one anyway, because
here there genuinely are two implementations — a decoded `StreamValueMap` off the bus and a
parsed `sample.fields` object out of a file — and single-sourcing the deciding logic across
them is what makes BTB-REF-4 true by construction rather than by testing.

The reasoning in ADR-1 is not contradicted; its condition is simply met this time.

### D-26 — OQ-4 resolved: `FIFO_DROP`, bus queue 1024

Three legs, and the first is not a measurement:

- **`BLOCK` is rejected on principle and no measurement could overturn it.** A recorder that
  stalls the bus changes the run it records — ADR-4's argument, [S2]'s independent one (PRD
  rev 6), and a written promise to consumers in §14 of the format spec. Testing it would mean
  building a producer that violates its own published contract.
- **`FIFO_DROP` at 1024 is sufficient**: zero bus-side drops across 136 000 samples at the
  overload scenario's 2 487/s, with the internal queue's high-water mark at 54 of 8 192.
- **The residual unexplained loss does not bear on the choice.** The PRD held OQ-4 open while a
  loss path existed that no counter reports. It still exists — but M6 established that it also
  affects a consumer *inside the host process* with no subscription at all (see D-27), so no
  backpressure policy can be implicated.

### D-27 — R7 is reframed, not closed, and the reference instrument is now suspect

M6 was tasked with re-running M4's publisher-versus-capture comparison under the overload
scenario, on the hypothesis that 3× the rate would provoke the mechanism.

**The hypothesis was falsified.** At 2 487 samples/s the capture was complete by the host's own
account across 135 581 samples — zero absent. At the reference rate it was short by 30 samples,
all in one frame, with every counter reading zero.

**And the host's own dump loses whole frames too** — 30 at the reference rate, 203 under the
overload, all present in our capture. That is an artifact written inside the host process, with
no bus and no subscription in its path.

Two consequences, both now in §14 of the format spec:

- The comparison bounds our completeness **from one side only**. "30 absent" is the
  disagreement between two lossy artifacts, not a measurement of our loss.
- A frame-shaped gap in an in-process writer is evidence the mechanism sits **upstream of any
  consumer**, which is why it does not block OQ-4.

R7 stays open as a documented caveat rather than an unexplained defect. `tests/
publisher-compare/compare.py` is kept in the repository and now reports both directions — the
direction that surprised us is the one M4 had not thought to print.

### D-28 — `--capture-max-samples` keeps its M5 meaning; no size limit in bytes

BTB-CAP-6 (bounded capture size, P2) is still unbuilt: no byte limit, no rotation choice, no
statement of either in the `header`. Flagged again for the project owner — it is P2 and M7 is
budgeted for shutdown, determinism and evidence, so it stays out unless asked for.
