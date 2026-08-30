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

**M1, M2 and M3 are delivered. M4 is next.**

Build only the current milestone. Ask before adding anything outside it.

M4 inherits two things from M3 worth knowing before starting: field order comes from the
runtime `MessageSchema::fields` (which disagrees with both hand-derivations in `notes.md` —
the schema wins), and `entity_add` / `entity_remove` / `sample` records each carry an
`occupancy` ordinal alongside the entity name (PRD ADR-6). In the code that ordinal is
`Occupancy::generation`; in the capture it is spelled `occupancy`.

## Conventions

- Files in this repo are ours — **no Arkheon proprietary header**. That convention applies
  only to files created inside `C:\N8RO`, which this project does not do.
- `docs/capture-format-v1.md` is a cross-repo contract consumed by EXT-17. Changing it after
  the M7 freeze means a version bump, not an edit.
- Keep `notes.md` current as you go — "what the stream contained that we did not expect" is
  a graded deliverable, not something to reconstruct at the end.
