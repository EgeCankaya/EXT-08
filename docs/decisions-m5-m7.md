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

---

## M7 — shutdown, determinism, evidence

### D-29 — The signal handler increments a counter and nothing else

BTB-SD-1 specifies this and it is a correctness requirement, not a style one: a handler runs
asynchronously with respect to the thread it interrupted, so allocating, locking or writing
from one can deadlock against it. `src/Signals.cpp` increments a `std::atomic<int>` that a
`static_assert` proves lock-free, re-arms itself, and returns.

Everything the interrupt *means* happens on the main loop, which already wakes four times a
second — so the response latency is bounded by the poll interval and nothing else.

### D-30 — The second interrupt is handled by a watchdog thread, not by the handler

"A second Ctrl-C during drain forces exit with a logged warning rather than hanging." A signal
handler cannot write that warning. So the teardown path starts a detached watchdog that polls
the counter every 50 ms and, on seeing two, logs the warning and calls `std::_Exit`.

It is never joined, deliberately. The process is on its way out either way, and a watchdog that
must be joined is one more thing that can fail to be joined while something else is stuck.

### D-31 — Each shutdown cycle runs its own simulator

The first version of the loop shared one host across all twenty cycles, and every cycle after
the first passed while testing nothing: the `entity_created` burst fires once at scenario load,
so a bridge started later records nothing but orphans, and a capture with no records has no
tail to lose.

The harness now starts a simulator per cycle, bridge first, and **fails a cycle whose capture
has no samples**. It also counts the `sample` records in the file and compares them against the
trailer's own `counts.samples`, because "every record enqueued before the signal is present" is
the requirement and a trailer that merely exists does not establish it.

### D-32 — The determinism harness is two harnesses

BTB-CAP-3's literal criterion is ten replays of one stored capture, hashed
(`tests/determinism/replay_hashes.ps1`, ten of ten identical). That is the end-to-end check and
it removes the host from the experiment, which is the only way the answer is about the
recorder.

But it would not localise a regression, so `tests/determinism/determinism_test.cpp` tests the
emission path directly against each of R4's three named hazards — unordered-map iteration,
locale-dependent float formatting, and unordered output containers — plus a set of golden
lines. The locale test is the one that earns its keep: it is the failure `%.17g` produces
*silently*, and this machine's locale is comma-decimal so it runs for real.

Golden lines are deliberate friction. After the M7 freeze, changing the spelling of a record
should require editing a test that says "these exact bytes", not slip through.

### D-33 — OQ-2 answered by observation, and the driver lives in `tests/`

The headless invocation is
`n8ro-sim-app.exe --sim-config SimEngineHost_SharedMemory --model-path <dir> --schema-file
<name>`. It takes **no scenario argument** — that was the puzzle — because loading a scenario is
a separate step published on `sim/scenario/command`, and starting is `{"command":"start"}` on
`sim/engine/command`.

`tests/host-driver/` does that. It is in `tests/` and not `src/` because **the bridge is a
passive observer and must stay one**: it subscribes and never publishes, which is what lets
ADR-4 and §14 promise consumers that it cannot perturb the run it records. A control direction
inside the bridge would undermine that promise even unused.

The driver bounds its run by **frame number**, not wall-clock time. That is what makes the R8
experiment possible at all: two runs stopped after the same number of seconds have not covered
the same simulation, and their captures are guaranteed to differ for a reason that has nothing
to do with determinism.

OQ-2 is answered as far as EXT-08 can answer it — the invocation works and is demonstrated.
[S2] asked for the mentor to confirm it; that confirmation is still worth having, because what
is demonstrated here is that it *works*, not that it is the intended production shape.

### D-34 — R8 resolved: the simulation is reproducible, the schedule is not

Two runs on the headless host, each stopped at frame 1200:

| | |
|---|---|
| byte comparison | **fails** — differ at line 339, different lengths |
| content comparison over running segments | **50 358 samples compared, 50 358 agree, 0 differ** |
| difference | 83 samples across 4 frames, out of ~1 198 |

So EXT-17's step-4 gate cannot be met byte-for-byte on this platform, and the property it was
reaching for — that the simulation is reproducible — holds exactly.
`tests/determinism/compare_captures.py` is the comparison that works, kept in the repository so
EXT-17 inherits it rather than deriving it.

Frame loss on the fixed-step host is ~0.2%, against ~1% on `n8ro-sim-local`. Better, not clean,
and consistent with R7's unattributed mechanism still being present.

### D-35 — A frozen-clock segment cannot be content-compared, and §14 needed to say so

§14 recommended comparing "per-`(entity, occupancy)` value sequences keyed by `sim_time_s`".
Keyed by `sim_time_s` does not work, and the first version of the comparison tool reported 35
differences that were alignment artifacts.

The engine resets the clock before republishing the roster at teardown, so every sample in that
segment carries `sim_time_s = 0.0` — about 93 per entity. Nothing distinguishes them, so the
Nth at t=0 in one run is not the same moment as the Nth in another.

Detected **exactly** rather than by a threshold: in a running segment each entity publishes once
per frame, so the maximum number of samples any one `(entity, occupancy)` carries at a single
`sim_time_s` is 1; in a frozen segment it is ~93. §5.1 and §14 both now say so.

### D-36 — The committed sample capture is trimmed, not synthesised

BTB-DOC-2 wants a sample capture from a real run; a real run is 64 MB.
`tests/evidence/trim_capture.py` keeps every non-sample record and the samples of two entities,
and rewrites exactly one number — `counts.samples` — by editing that number in the trailer
*line* rather than re-encoding the record, so every other byte is the producer's own.

3.2 MB, still reports CONFORMS, and the mutation suite runs against it. It carries the whole
story: `RedUAV_N_01` created, destroyed at t = 149.45, re-created at occupancy 2; both
segments; all seven verdicts including the two never-met ones.

### D-37 — What is NOT delivered, and why

Recorded plainly rather than left for the project owner to discover.

> **This list is as it stood at the close of M7.** The first entry has since been partly
> discharged — see the note under it, and PRD rev 10.

- **~~The 5-minute demo recording (BTB-DOC-2).~~ Shot on 2026-08-31 and published as its four
  takes** — [linked from the README](https://drive.google.com/drive/folders/1L0lPs0wkDA_qGYx8Z0Q8-SMzNrOvLoXN?usp=sharing), with a beat-by-take map. Every beat the
  requirement names is on camera; what does not exist is a single assembled cut, which the
  README states rather than implies. Original entry follows.
  All seven beats were captured across four takes against producer 0.8.0, following
  `docs/demo-recording-script.md`, which is both the shooting script and the record of what was
  filmed. Shooting it also exposed and fixed a reporting defect in the run summary and the
  conformance reader (PRD rev 10 (b)); no capture byte changed and the format stays frozen.
- **~~BTB-CAP-6, the byte-limited capture (P2).~~ Built; see D-38 to D-41 and PRD rev 11.**
  As it stood at M7: `--capture-max-samples` bounded a run by record count, which is a safety
  bound and not CAP-6 — there was no size limit in bytes, no stop-or-rotate choice, and neither
  was stated in the `header` as the FR requires. It was P2 and M7's budget went to shutdown,
  the two spikes and the evidence pack. **It was the one P1-or-P2 requirement left
  unimplemented, and it no longer is: every requirement in the PRD is now built.**
- **~~The PRD's CLI table does not list `--capture-max-samples`.~~ Resolved, in two steps.**
  Rev 9 added it to the table with a note distinguishing it from the then-unimplemented CAP-6.
  Rev 11 answered the question this bullet actually asked — whether the table should gain the
  flag or CAP-6 absorb it — with **both, and they are different bounds**: CAP-6 is
  `--capture-max-bytes`, per file, with a stop-or-rotate choice; `--capture-max-samples` counts
  records, run-wide, and always stops. Neither absorbed the other. See D-38.
- **~~OQ-2's mentor confirmation.~~ Closed — see D-42.** The invocation is demonstrated to
  work, which was the half that was ever EXT-08's; the "intended production shape" half is
  carried downstream as EXT-17's OQ-3.

---

## BTB-CAP-6 — the bounded capture

Built after M7 closed, against the FR and UAC-BTB-CAP-6 as written. The five decisions below
are the ones that were not already made by the requirement.

### D-38 — `--capture-max-bytes` is a second bound, not a replacement for the record bound

D-13 and D-28 both flagged the question and both declined to answer it: either the PRD's CLI
table gains `--capture-max-samples`, or CAP-6 absorbs it when it is built. Now that it is
built, **neither. They are different bounds and both are kept.**

|  | `--capture-max-bytes` | `--capture-max-samples` |
|---|---|---|
| Measures | bytes | `sample` records |
| Scope | one file | the whole run, across every part |
| At the limit | stop **or** rotate, operator's choice | always stops |

Absorbing the record bound into CAP-6 would have meant either dropping it — it is the only way
to end a run at a repeatable place, which is what the determinism and demo work uses it for —
or redefining it as per-file, which would silently turn an existing documented run bound into a
per-file quota. Keeping both costs one option and one row in `header.limits`.

The counter that keys the record bound therefore had to move from the per-file count to a
run-wide one. That is a real bug if missed: `counts_` resets at every rotation, so a
`--capture-max-samples 100000` run with rotation on would have recorded 100 000 samples *per
part*, forever.

### D-39 — the stop-or-rotate choice is the operator's, not this project's

The FR permits either and requires that the choice be documented. The PRD's own quality-gate
notes flagged that it does not choose between them and asked whether the design should.

It should not, and both are built. Which one is right depends on whether the run's tail or the
host's free space matters more, and that is a property of the run, not of the recorder — an
overnight campaign wants `rotate`, a bounded-disk CI box wants `stop`. Compiling in either
would have made the other unreachable for no gain. The choice is `--on-size-limit`, it defaults
to `stop` (the conservative one: bounded total disk, which is the failure the FR's customer
scenario is about), and it is written into `header.limits.on_size_limit` so the file states
which was in force.

### D-40 — every part of a rotated set is a complete capture, and the linkage is three optional keys

The alternative shape — a set of continuation files that are fragments, with one header at the
front and one trailer at the end — is smaller and would have been a **format version bump**,
because a fragment is not a valid `n8ro-capture/1` file and every reader would need to know
about rotation before it could read one.

So each part carries its own `header` with its own full `schemas` array, its own segments
numbered from 0, its own `counts`, and its own `trailer`. The cost is a repeated schema table
per part — about 1 KB against a bound that must be at least 16 KB and is realistically
megabytes. What it buys is that **the whole feature fits under spec §13's non-breaking rule**:
four added keys on two existing record types, no new record type, and no new value in any
closed vocabulary — `size_limit` was already in both `trailer.end_reason` and
`segment_close.reason`, which is worth noticing, because it means the format anticipated this
requirement at M4 and the freeze was never in the way.

A reader that ignores `header.part`, `header.continues_from` and `trailer.continued_in` reads
every part correctly and completely. It simply does not know they are siblings.

Two consequences a consumer has to know, and §6.7 states both: **segment ordinals restart at 0
in each part** (so anything per-segment across a set keys on `(part, segment)`), and a segment
cut by a rotation appears as a `segment_close` with `reason: "size_limit"` in one part and a
`segment_open` in the next — one segment split, not two segments.

### D-41 — the bound is checked before the write, against a reserve

"Never silently truncate" is the requirement's actual content, and it is not something a bound
gives you for free — checking after a write can only report a breach, and letting the trailer
be the record that does not fit turns a size limit into exactly the corrupt file the FR exists
to prevent.

So: the record's line is rendered first, its exact length is known, and the check is
`bytesWritten + len + 1 + kCloseReserveBytes > maxBytes`. **8192 bytes** are held back from the
first record onward for the `segment_close`, any end-of-run `verdict` records and the
`trailer` — measured at ~520 bytes for a trailer with every counter present, ~110 for a
segment_close, and a few hundred per verdict. Structural records never go through the check;
they are what the reserve is for. A record is therefore written whole or not at all.

Three details that only show up once it is running:

- **A rotation resets the segment ordinal, so a record cannot be rendered once and reused.**
  The admission helper takes a *render callable* and calls it again after a rotation. Rendering
  once and writing that line into the next part would put `"segment":3` into a file whose only
  segment is 0 — malformed, and unrepairable by any reader.
- **Rotating needs a guard against rotating forever.** If a fresh part cannot hold one record
  after its own header, the run stops with a named error rather than producing an endless run
  of header-and-trailer files. `--capture-max-bytes` also has a floor of 16384 at parse time,
  so the ordinary version of this mistake is caught before the run starts.
- **An intermediate part's trailer is written mid-run, by the writer thread**, when the main
  thread is not there to supply the platform's counters. `RecordQueue::counters()` is
  mutex-guarded and would be safe to call from there, but nothing establishes that for
  `MessageBusPacked::metricsSnapshot()` or `IMessageBus::getStatistics()`. Reaching into the
  SDK off the main thread to fill in a counter would trade a real invariant for a cosmetic one,
  so main caches its own reading at each status poll and the writer reads the cache. The cost
  is that an intermediate part's `drops` and `bus_metrics` are as of the last poll rather than
  the instant of the rotation — stale by at most a second, understating rather than inventing,
  and **stated in spec §11** so no reader is misled. The last part's trailer is exact, as it
  has always been.

### D-42 — OQ-2 is closed on the half that was ours, and the other half goes downstream

OQ-2 asked two questions wearing one number: *what is the headless invocation*, and *is it the
intended production shape*. They have different owners and only the first was ever EXT-08's.

**The first is answered and demonstrated.** `n8ro-sim-app.exe --sim-config SimEngineHost_*
--model-path <dir> --schema-file <name>`, taking no scenario argument — load and start are
separate publishes on `sim/scenario/command` and `sim/engine/command`. `tests/host-driver/`
drives it end to end, and it is what made the R8 determinism experiment possible at all. If it
did not work, none of the M7 evidence would exist.

**The second is a question about the platform's documentation**, not about anything this
project produces. Nothing EXT-08 ships changes with the answer: the bridge subscribes and never
publishes, the host driver lives in `tests/` for exactly that reason (ADR-4), and no capture
byte, no requirement and no interface depends on whether the invocation is blessed or merely
functional.

It is therefore closed here and carried where it bites: **EXT-17's OQ-3 asks the same question**,
with a decision target of that project's M2, and EXT-17 is the project that actually has to run
the host in production. `[S2]` asks its own reader to confirm the invocation with a mentor,
which is an instruction to EXT-17's implementer as much as to this one.

What would reverse this: a mentor saying the invocation is *not* the intended shape and naming
a different one. That would change EXT-17's driver and this project's `tests/host-driver/`,
neither of which is a shipped interface — which is the measure of how little was riding on it.

The general point, because it recurred: **an open question kept open because its answer lives in
another organisation is not diligence, it is a document that never closes.** The test is whether
anything this project ships would change with the answer. Here, nothing does.

---

## Post-M7 — the four corrections EXT-17 raised

EXT-17 filed four defects against what EXT-08 hands it, as GitHub issues #1–#4. All four are
**documentation**: nothing about a capture, a verdict or the producer changes, and no test that
passed before fails after. They are recorded here because two of them touch a **frozen** file and
one changes what the boundary artifact *is*, and both of those are choices rather than typo
fixes.

**The numbering jumps to D-70 on purpose.** The 2026-09-01 defect sweep was still in flight
when these were written and is still numbering its own decisions upward from D-43; the gap is
reserved for it. These three were first written as D-49-D-51 and collided with it, which is
recorded rather than quietly fixed - the commit that introduced them names the old numbers.

### D-70 — E-3 and E-4 are clarifications, admissible under the freeze, and the freeze is why they are worded narrowly

`docs/capture-format-v1.md` is frozen: a change to what it specifies is `n8ro-capture/2`. Neither
of these changes what it specifies.

- **E-3 (#1), §6.7 stitching rule 2.** The rule said a run's totals are the sum of its parts'
  `counts`. For four of the five counters that is true; for `segments` it contradicts rule 1
  three paragraphs above it, which already says a cut segment is *one* segment appearing in two
  files. EXT-17 measured it: four parts reading 1, 1, 1, 2 for a run with 2 segments. **The fix
  narrows the sentence and states the correction** — subtract one per cut, a cut being a part
  carrying both `size_limit` and a `continued_in`. §11 restates the same rule and was corrected
  with it.
- **E-4 (#2), §5.1's frozen-clock test.** The *test* was never wrong and is unchanged. What was
  too narrow is the **reading** attached to it: "the clock was reset" is one cause of a positive
  result and EXT-17 measured two more — a burst published twice with identical values inside a
  segment whose clock ran normally (1 of 27 ordinary runs), and a pre-`start` update landing in
  that burst with values that differ (4 of 35 such runs). The fix states what a positive result
  actually establishes — *the segment cannot be aligned on `sim_time_s`* — lists the three
  shapes, and says they are distinguishable and why that is worth doing. §14's self-test guidance
  gained the consequence EXT-17 pays for: excluding these segments can leave a self-test with
  **nothing** to compare, which is a refusal and not a pass, and retrying until a pair comes out
  comparable would turn a real refusal into a silent one.

**Why this is admissible.** A capture written before these edits is byte-identical to one written
after; no key gains, loses or changes a meaning; no reader that conformed before fails after. §13
already permitted clarifications and the frozen banner says so. **What would have made them a
version bump** is changing the test in §5.1 or the meaning of `counts.segments` in a file —
neither was touched. Both are listed in §13 under "Clarifications made after the freeze" so a
downstream project holding a pinned copy can see what moved without diffing the whole file.

**What this does not do:** it does not correct any capture already written, and every capture was
readable and correctly readable throughout. What was at risk was two projects computing different
numbers from one frozen document, which is the thing a frozen document exists to stop.

### D-71 — the condition schema becomes a file, rather than staying a set of README sections

**E-5 (#3) is not a wording defect; it is a defect in the shape of the artifact.** The condition
schema had no file. It lived as four consecutive sections of `README.md`, and a downstream
project vendoring it excerpted the two that declare the file shape and stopped one heading before
the two that say what the numbers mean — "How distance is computed" and "Boundary semantics".

The excerpt was faithful, verbatim and not stale. It was still enough to produce **silent
divergence**: from the shape alone a consumer could reasonably compute a great-circle or
horizontal distance and then emit verdicts disagreeing with this referee's, on the same capture,
in the same vocabulary, against the same condition ids, with nothing anywhere to surface it.

**Three options were considered.**

1. *Reply "yes, take those two sections as well".* Rejected. It fixes one consumer and leaves the
   next one to make the same excerpt, because the thing that invited the excerpt — that there is
   no file to take — is untouched.
2. *Move the sections out of `README.md` into the new file and link them.* Rejected. The referee
   section of a README that a reader lands on should say how a distance is computed; sending them
   to another file to find out is worse for the larger audience.
3. **Adopted: `docs/condition-file-schema.md` carries all four sections verbatim, `README.md`
   keeps them, and a test fails if the two ever drift.** `tests/referee/check_schema_digest.py`
   compares the spans line by line and prints the first difference; it needs no simulator and no
   build. Verified to reject: changing one `<=` to `<` in the digest fails it, naming the line.

`README.md` now points at the file immediately above the sections, so "vendor this, do not
excerpt these" is answered where the excerpt would otherwise be made. This also closes EXT-17's
**F-19** as a side effect: its vendored digest could not be verified by identity because no file
of that name existed upstream, and now one does — so a re-pin is a byte comparison, exactly as it
is for the capture format.

**What would reverse it:** the duplication is real, and a test is a weaker guarantee than a single
copy. If these sections grow, option 2 becomes the right one and the README keeps a summary.

### D-72 — E-6 is fixed where the omission was, not where it was reported

**E-6 (#4)** was filed against `PROVENANCE.md` finding 6 — which is EXT-17's own manifest and not
an EXT-08 file at all. EXT-17 caught that itself and corrected the citation by a comment rather
than a silent edit (its F-37). The substance was unaffected and it is EXT-08's: the **R8 spike
block in `README.md`** gives the headless invocation with no mention of `N8RO_RELEASE`, and
following it exactly produces a host that refuses every 42-entity scenario load **while sitting
idle rather than failing** — the shape that hangs an unattended campaign instead of breaking it.

The block now says to run `setup.cmd` in every terminal, and states both preconditions under it
with their failure modes: `N8RO_RELEASE` unset gives the silent-idle refusal, and `C:\N8RO\bin`
missing from `PATH` gives exit **53** with no output, which reads like a crash and is a missing
DLL. They are two separate preconditions and setting one does not cover the other.

**Why it was worth an edit rather than "it works on a machine that has run `setup.cmd`".** The
README says elsewhere that `setup.cmd` does both, which is true and was not enough — the R8 block
is a self-contained recipe that a reader runs as written, and it was written as though the
variable mattered only to the build and to `n8ro-sim-local`. The platform mentor confirmed on
2026-09-01 that `N8RO_RELEASE` **is** expected to be set in production, which is what moved this
from "our machine is provisioned oddly" to a defect in the instructions.

### D-49 — silent-topic detection watches the entity-state topic, not all four

BTB-OBS-2 says "IF **a subscribed topic** has produced no decoded messages for a configurable
interval". Read literally that is all four subscriptions, and the first draft of the check did
exactly that. It is wrong, and wrong in the FR's own terms.

Two of the four are **event-driven and legitimately silent for most of every healthy run**:
`sim/entity/event` carries 134 messages across a 200 s reference run, and `sim/scenario/event`
carries two per scenario. Watching them means a warning every interval, on every run, forever —
which is not the requirement satisfied but the requirement's own pain restated. Its pain
statement is that "a silent topic is indistinguishable from a quiet simulation until someone
thinks to check"; a detector that cries wolf on two topics is how the operator stops reading
the line that would have named the third.

The fourth, `sim/engine/state`, publishes continuously at ~19.5/s — but its silence is
**already** detected, at 3.0 s, as host loss, and that detector ends the run. A ten-second
check on the same evidence can only ever fire after the three-second one has broken the loop.

That leaves `sim/entity/state`, which is the topic the FR's customer scenario is actually about
("the engineer ... returns to a log that already told them the entity-state topic never
spoke") and the one whose silence has exactly one cause worth naming.

So the flag is per topic (`TopicActivity::silenceIsEvidence`) rather than a hardcoded index,
and the reasoning for each of the four is written at its declaration — because the next person
to add a subscription has to make this decision, and the field is where they will meet it.

**Second, smaller call in the same check:** the silence clock is held at `now` while the engine
is not running, rather than left to accumulate. Otherwise a simulation paused for a minute
warns the instant it resumes, about a stretch the FR explicitly excludes ("the warning fires
only while the engine reports running, so a paused simulation does not generate noise").

**What would reverse it:** a deployment where the event topics are expected to be continuous —
a scenario that creates and destroys entities constantly. Then `silenceIsEvidence` is per-run
configuration rather than a constant, and the honest form is a per-topic interval rather than
one. Nobody has that scenario; `--topic-silence-s` is one number until somebody does.
