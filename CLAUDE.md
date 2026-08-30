# CLAUDE.md — EXT-08 Bus Telemetry Bridge

## What this repo is

A **standalone C++17 console program** that connects to a running N8RO simulation over the
message bus, records every published entity update to a durable capture file, and evaluates
declared conditions against the run. It runs beside the simulator as its own process.

The contract is `docs/prd.md` — a Comprehensive PRD with 27 FRs prefixed `BTB-`. Read it
before designing anything; do not re-derive decisions it already settled.

## What this repo is NOT

- **Not a plugin.** No plugin ABI, no `IPlugin` exports, no `create_plugin`.
- **No Qt, no scaffolder, no `.vcxproj` template.** It is an ordinary console app that links
  four import libraries.
- **`C:\N8RO\dev\samples\` is plugin-only and is NOT a starting point.** Do not clone it.
  Ignore `C:\N8RO\CLAUDE.md` — that file is about authoring plugins inside the install tree
  and its guidance is wrong for this project.

## Verified environment — do not re-derive

- Install: `C:\N8RO`, runtime 2.1.328, SDK component `com.n8ro.dev` 2.1.328.
- Toolchain: Visual Studio 2026 (v18.x), C++17 (`stdcpp17`), `Release | x64`. MSBuild.
- Env: `call C:\N8RO\setup.cmd`, then `C:\N8RO\dev\setup-dev.cmd` for MSBuild.
- Includes: `include\n8ro-core`, `n8ro-sim`, `n8ro-schema`, `n8ro-data`.
- Link: `n8ro-core.lib`, `n8ro-sim.lib`, `n8ro-schema.lib`, `n8ro-data.lib` from `lib\`.
- **`EntityStateSample.h` does not exist in this release.** The brief claims a roster and a
  per-entity latest-sample cache on `SimulationEngineClient`; neither ships. We build that
  layer ourselves — see the PRD's `BTB-EP-*` requirements.
- There is no predicted-sample API. Everything off the bus is published, by construction.

## Guardrail

`C:\N8RO` is **read-only** for this project. Read headers, read docs, run binaries, observe
the bus. Never write into it — no files, no plugins, no schema edits. All work lands here.

## Hard rules

1. **Never throw.** Return values plus logging — the platform's contract, and the libraries
   we link follow it. An exception escaping a bus handler crosses a boundary we don't own.
2. **A subscription handler is a courier.** Copy, hand off, return. All IO, parsing and
   formatting happen on our own thread.
3. **Derive, don't guess.** Topic strings and field names come from
   `MessageBusPackedSchemaRegistry` / `MessageSchema` at runtime, never from a literal.
4. **Simulation time is the only clock** in anything durable. Wall-clock belongs in log
   lines and nowhere else. A capture that varies between identical runs is a defect.
5. **Order is meaning.** FIFO per topic across any thread boundary.
6. **Backpressure is an explicit decision at both boundaries** — the bus subscription
   (`SubscriptionOptions` defaults to the lossy `KEEP_LATEST`, queue 100) and our own
   handler-to-writer queue. Losses are counted, never silent.

## Things already learned (save yourself the rediscovery)

- Resolve a **`SimEngineClient_*`** engine-config entry, never a `SimEngineHost_*` one.
  This is the most common configuration trap.
- `C:\N8RO\docs\modules\n8ro-shark\dev\README.md` documents the exact passive-observer
  recipe the shipped bus monitor itself uses. Read it. Do not link against shark.
- The entity-state topic is `sim/entity/state` — **confirmed at runtime in M1 and M3.** Still
  resolve it from the registry, never from that literal (BTB-EP-1).
- **`SimulationEngineClient` exposes no registry and no `MessageBusPacked`** — only
  `messageBus()`. Build your own `MessageBusPackedSchemaRegistry` over your own `DbModel` and
  construct `MessageBusPacked(*client->messageBus(), registry)`. That is the whole recipe.
- **`EventConfigData::topic` is a *message instance name*, not a topic string.** Resolving an
  event to its topic is two hops: `EventConfigReader` → message name → `registry.getByName()`
  → `MessageSchema::topic`. The field name works against you here.
- **The entity-state schema declares twelve fields; only eleven are ever published.**
  `activeAnimation` appeared zero times in 132 188 samples. Read field *presence* per message;
  never assume a declared field arrives. This is why BTB-CAP-4's verbatim rule is not optional.
- **`n8ro-sim-local.exe` needs `N8RO_RELEASE` set**, or it resolves plugins to the working
  directory and *refuses* the scenario load (`componentPhysics has no registered factory`).
  The bridge then reports `engine=running scenario=(none)`, which looks like a bridge fault.
- **Start the bridge before the simulator.** The `entity_created` burst fires once at scenario
  load; attach late and every sample is orphaned with zero drops and no error anywhere.
- `n8ro-shark` records JSONL and is the fastest way to watch the bus. Its format is
  wall-clock stamped, which is exactly why we need our own.
- `MessageBusPacked::metricsSnapshot()` exposes `schemaHashDrops`, `decodeFailures`,
  `missingSchemaPassthrough` — a silent topic is a schema mismatch, and these prove it.

## Milestones (from the PRD)

M1 observe the bus · M2 smallest client · M3 entity picture · M4 capture format + spec
· M5 output path + lifecycle · M6 referee + backpressure · M7 shutdown, determinism, evidence

**M1 through M7 are delivered. The project is complete**, bar BTB-CAP-6 (P2, the byte-limited
capture) and the 5-minute demo recording, which needs a person. `docs/decisions-m5-m7.md` D-37
is the complete list of what is not delivered and why.

**`docs/capture-format-v1.md` is FROZEN.** After the M7 freeze, a change to what it specifies is
a version bump and a downstream change for EXT-17 — not an edit. Adding a key to an existing
record is still non-breaking (spec §13); renaming one, retyping one, changing a unit, or adding
a record type is not. `tests/determinism/determinism_test.cpp` carries golden lines for exactly
this reason: changing the spelling of a record now means editing a test that says "these bytes".

**OQ-1 is decided: we own the entity picture permanently** (PRD rev 3, ADR-1). It is not a
shim. It has tests in `tests/entity-picture/` that need no simulator — keep them passing, and
mutation-check them when the picture changes. It still stays schema-driven and verbatim:
owning the layer is a reason to test it, never a reason to start modelling the payload.

Build only the current milestone. Ask before adding anything outside it.

Field order comes from the runtime `MessageSchema::fields` (which disagrees with both
hand-derivations in `notes.md` — the schema wins), and `entity_add` / `entity_remove` /
`sample` records each carry an `occupancy` ordinal alongside the entity name (PRD ADR-6). In
the code that ordinal is `Occupancy::generation`; in the capture it is spelled `occupancy`.

**Every decision taken during M5–M7 is recorded in `docs/decisions-m5-m7.md`** — read it
before revisiting one, because most of them turned on a measurement rather than a preference.

Things established by measurement, which it would be expensive to re-derive:

- **A single run produces two segments**, because the engine's stop path unloads and reloads.
  Segment 1 is the teardown reload and holds the re-created roster at occupancy 2. Not a bug.
- **The `entity_created` burst is published *before* the `scenario_loaded` that announces it**,
  at first load and at every reload. The writer stages roster records that arrive with no open
  segment and flushes them into the segment that opens next; deleting that staging area
  produces a malformed capture.
- **Host loss is `sim/engine/state` silence for 3.0 s**, derived from a measured 548 ms worst
  observed gap. Entity-state silence is *not* a host-loss signal — it happens at every unload.
- **Both backpressure boundaries are already explicit**: `FIFO_DROP` / 1024 at the bus,
  `drop_newest` / 8192 + 1024 reserved internally. `BLOCK` is rejected at both, and
  `docs/capture-format-v1.md` §14 promises consumers in writing that this producer never
  blocks the bus. OQ-4 is still open — M6's overload run is what resolves it.
- **Handler cost is measured**: p50 ≤ 1 µs, p95 ≤ 5 µs, p99 ≤ 10 µs over 132 150 invocations,
  against targets of 20 / 100 / 500 µs. `src/HandlerTiming.h` is the instrument.
- The internal-queue drop counters are the one deliberately scheduler-dependent thing in the
  file. They are zero whenever a byte comparison is meaningful; §14 says so.
- **Live and replay verdicts are byte-identical**, and that holds by construction: one
  `Referee`, two `FieldSource` implementations. Do not duplicate a deciding rule across the
  two paths - that is the only way to break it. End-of-run verdicts are anchored on the last
  *data* record for the same reason.
- **OQ-4 and OQ-6 are resolved** (PRD rev 7). `FIFO_DROP` / 1024 at the bus, and the condition
  schema is EXT-08's own, documented in the README.
- **R7 was re-measured and reframed, not closed.** The rate hypothesis was falsified, and the
  host's own per-entity dump - the instrument M4 measured against - loses whole frames too.
  Treat it as a one-sided bound, never as ground truth.
- **R8 resolved, R1 closed** (PRD rev 8). The simulation is reproducible; its publication
  schedule is not. Two headless runs stopped at the same *frame* agree on 50 358 of 50 358
  samples but are not byte-identical, so a determinism gate must compare content -
  `tests/determinism/compare_captures.py`. R1's teardown access violation did not reproduce in
  20 of 20 plugin-free cycles.
- **OQ-2 answered:** `n8ro-sim-app.exe --sim-config SimEngineHost_* --model-path <dir>
  --schema-file <name>`, and it takes **no scenario argument** - load and start are published on
  `sim/scenario/command` and `sim/engine/command`. `tests/host-driver/` does it, and it lives in
  `tests/` because **the bridge subscribes and never publishes**. Keep it that way: ADR-4 and
  format spec section 14 both promise consumers the recorder cannot perturb the run.
- **`sim_time_s` is not a key.** A frozen-clock teardown segment carries ~93 samples per entity
  at one value, and such a segment cannot be aligned across two runs at all. Spec sections 5.1
  and 14 say so; the comparison tool detects one exactly (in a running segment each entity
  publishes once per frame).

## Conventions

- Files in this repo are ours — **no Arkheon proprietary header**. That convention applies
  only to files created inside `C:\N8RO`, which this project does not do.
- `docs/capture-format-v1.md` is a cross-repo contract consumed by EXT-17. Changing it after
  the M7 freeze means a version bump, not an edit.
- Keep `notes.md` current as you go — "what the stream contained that we did not expect" is
  a graded deliverable, not something to reconstruct at the end.
