# EXT-08 — Bus Telemetry Bridge

> **One-liner:** A standalone C++17 console program that connects to a running N8RO simulation over the message bus and turns the published stream into a durable, versioned, self-describing capture file plus a pass/fail verdict — so a run can be analysed, and re-judged, long after it has ended.

**Date:** 2026-08-30
**Revision:** 3 — reconciled with M1–M3; OQ-1 decided. See §"Revision history".
**Status:** Draft
**Owner:** EXT-08 implementer
**DRI:** egemencankaya14@gmail.com
**Audience:** Engineering (implementer), Mentor (reviewer), EXT-17 author (downstream consumer)
**Target release:** EXT-08 v1.0 — capture format `n8ro-capture/1`
**Platform baseline:** N8RO runtime 2.1.328, SDK component `com.n8ro.dev` 2.1.328

## Revision history

| Rev | Date | Change |
|----:|------|--------|
| 1 | 2026-08-30 | Initial PRD. |
| 3 | 2026-08-30 | **OQ-1 decided: we own the entity-picture layer.** The brief was checked and is silent on the question, so it did not settle it. The layer is treated as a permanent component: unit tests that need no simulator (`tests/entity-picture/`), documented invariants, and a snapshot API that cannot report a removed entity as live. No interface was added, and ADR-1 says why. |
| 2 | 2026-08-30 | Reconciled with delivered M1–M3. **BTB-EP-3's second acceptance criterion was unsatisfiable as written** and is now scoped to an entity occupancy; `entity_add` / `entity_remove` / `sample` gain an `occupancy` field so the criterion is checkable in the capture itself (ADR-6). BTB-EP-1 gains the topic-anchoring criterion. Performance baselines and the reference scenario filled in from M1. OQ-3 and OQ-5 resolved; OQ-1 re-targeted. R3 and R6 closed. |

> Rev 2 changes the **logical capture shape** (a new field on three record types). This is
> pre-freeze, so no format version bump is required — `n8ro-capture/1` is not frozen until M7
> and no EXT-17 reader exists yet. After the M7 freeze the same change would be a version bump.

## Purpose and scope

This PRD covers **EXT-08 only**: a Track C standalone program (no plugin, no plugin ABI, no Qt, no scaffolder) that links four N8RO import libraries, subscribes to the simulation message bus, and produces two outputs — a **Recorder** capture of every published entity update, and a **Referee** verdict over declared conditions.

It exists as a separate document because EXT-08 and EXT-17 are **separate git repositories**. Nothing in EXT-08 is shared with EXT-17 as source. Everything EXT-17 needs from EXT-08 crosses that boundary as a **documented, versioned artifact**: the capture format specification and the capture files themselves. That constraint is not incidental — it is the reason several requirements in this document exist at all, and it is why the capture format is treated as a deliverable rather than an implementation detail.

The boundary of this work: EXT-08 observes and judges. It does not orchestrate runs, does not vary parameters, does not manage a campaign, and does not start or stop simulation hosts. Those are EXT-17's four pieces (execution, parameterisation, assertion, reporting), and EXT-08 supplies the third and fourth by way of the capture and the referee.

### Source inputs

**Authoritative (binding):**
- **[S1]** `EXT-08-Bus-Telemetry-Bridge.docx` — the client brief. Output shapes, the rules the code lives under, acceptance criteria, deliverables, step order, effort target.
- **[S3]** Verified-environment findings supplied with this request — release, toolchain, track classification, the API-availability corrections in §"Prior art and lessons learned", and the Recorder+Referee scope decision.

**Contextual (informational, not binding):**
- **[S2]** `EXT-17-Headless-Campaign-Runner.docx` — the downstream consumer. Read to constrain EXT-08's design; its requirements are *not* in EXT-08's scope, but four of its constraints bind EXT-08's outputs (see §"Cross-service impact").
- **[S4]** N8RO 2.1.328 public headers read directly during PRD preparation: `n8ro-sim/infrastructure/SimulationEngineClient.h`, `n8ro-sim/messaging/packed/MessageBusPacked.h`, `MessageBusPackedSchemaRegistry.h`, `MessageSchema.h`, `StreamValue.h`, `n8ro-sim/messaging/EventNames.h`, `n8ro-sim/entity/IEntityManager.h`, `n8ro-core/core/messaging/IMessageBus.h`, `Message.h`.
- **[S5]** `C:\N8RO\CLAUDE.md` — the release tree's authoring conventions, including the Arkheon proprietary-header rule for files created inside `C:\N8RO`.

## Problem statement

> **When** an engineer needs to know what a simulation run actually did — for analysis, for a regression baseline, or as evidence that a behaviour holds —
> **they struggle with** the fact that the run's truth exists only as transient traffic on the message bus and as pixels in a GUI that nobody is watching at 2 a.m.,
> **which means** every question about a past run costs a re-run, every regression check is a person watching a screen, and there is no artifact anyone can point at and say "this is what happened."

The current workaround is to watch a run and describe it. That does not scale past one run, it cannot be re-examined, and it produces no evidence. EXT-17 is commissioned to run **twenty or more unattended runs** and report on them; it cannot begin until something durably captures what a run published and can decide whether it passed. EXT-08 is that something.

There is a second, sharper cost. The bus is how **every third-party system will ever talk to the simulator** [S1]. It is the integration shape with the longest shelf life, because no plugin ABI binds to it and a bus client survives release upgrades that would force a plugin rebuild. Today nobody on the team has built one end-to-end, so the shape of the stream — what is actually published, how often, and what is silently absent — is undocumented folklore. Every future integration pays that discovery cost again.

### Prior art and lessons learned

Nothing of this shape has been built against this release. What we have instead is a set of **corrections to the brief**, established by reading the shipped 2.1.328 headers directly [S3, S4]. These are the lessons that shape the approach:

- **The brief describes an API this release does not ship.** [S1]'s "What the client gives you" section promises "the entity picture — the roster, plus each entity's latest sample." It is not there. `include\n8ro-sim\infrastructure\EntityStateSample.h` does not exist; `dumpbin` reports zero `EntityStateSample` symbols in `n8ro-sim.lib`; and `SimulationEngineClient` has no roster accessor and no per-entity sample cache. What it actually provides is `create()`, `startMessagePump()`/`stopMessagePump()`, the `send*` command family, three `subscribe*` entry points taking a **raw** `MessageHandler` over **raw** bus messages, `unsubscribe()`, local engine-state getters, and asynchronous catalog queries. **The entity picture is therefore something we build, not something we are given.** It is budgeted here as first-class work (~2–3 days), not as free.
- **The published-versus-predicted distinction cannot be exercised in this release.** [S1] devotes a section to choosing the published form over the predicted one. There is no predicted-sample API in 2.1.328. Everything that arrives off the bus is published, by construction. EXT-08's acceptance criterion "you can say how you know they are not predictions" is therefore satisfied structurally, and this PRD states that rather than designing around a distinction that does not exist here. See BTB-CAP-2.
- **The bus's own subscription default is lossy, and the brief does not mention it.** `SubscriptionOptions` in `IMessageBus.h` carries `queueSize = 100` and `backpressurePolicy = BackpressurePolicy::KEEP_LATEST` by default, with `FIFO_DROP` and `BLOCK` as the alternatives. That means there are **two** backpressure boundaries in this program, not one: bus→handler and handler→writer. Accepting the default would silently discard entity samples and produce a capture that looks complete and is not. [S1]'s rule that backpressure must be an explicit decision applies to both boundaries.
- **The decoded payload arrives in an `std::unordered_map`.** `MessageBusPacked::DecodedHandler` receives a `StreamValueMap`, which is `std::unordered_map<std::string, StreamValue>`. Writing fields in that map's iteration order would make the capture vary between runs and between builds — which breaks EXT-17's byte-for-byte determinism self-test before EXT-17 is even written. Field order must come from the `MessageSchema`'s declared `fields` vector, never from the map.
- **The decoded handler is also handed the schema, and that is the format's best asset.** `DecodedHandler` signature is `void(const Message&, const MessageSchema&, const StreamValueMap&)`. `MessageSchema` carries `messageName`, `topic`, an ordered `fields` vector of `{name, type, size}`, `schemaHash`, `messageId`, and `wireVersion`. Embedding that verbatim in the capture header is what makes the file **self-describing** — the single design decision that lets EXT-17 read a capture with no access to EXT-08's source.
- **A schema field can be declared and never published, and M3 found one.** The entity-state schema declares **twelve** fields; `activeAnimation` was published **zero times** in 132 188 samples of the reference scenario. Two independent hand-derivations at M1 — decoding the packed bytes, and reading the platform's own JSON serialiser — both concluded eleven, because both were looking at the wire rather than at the schema. This is the concrete case BTB-CAP-4's verbatim rule exists for: a curated struct built from those observations would have been born a field short, with nothing in the system able to say so. It also fixes the reading rule — *a packed payload carries only the fields the publisher wrote*, so field presence is read per message and never assumed.

## Goals and success metrics

### Goals

- **G1 — A run becomes an artifact.** Every published entity update from a followed scenario lands in a durable capture file, stamped with the simulation time the sample carried, with any loss counted rather than silent.
- **G2 — The artifact outlives the program that wrote it.** The capture format is versioned, self-describing, and specified field by field in a document that crosses a repo boundary. A reader can be written against the specification alone.
- **G3 — A run can be judged, and re-judged.** Declared conditions are evaluated against the run and produce verdicts, both live and offline against a stored capture, without re-running the simulation.
- **G4 — Identical runs produce identical captures.** Nothing EXT-08 emits varies between two runs of the same configuration.
- **G5 — The stream is documented.** What the bus actually carries — including what it does not carry — is written down.

### Success metrics

| Metric | Baseline (current) | Target | How measured | Timeline |
|--------|-------------------|--------|--------------|----------|
| Published entity samples captured, as a fraction of samples delivered to the handler | 0% (no capability exists) | 100%, with zero internal-queue drops on the reference scenario at nominal rate | Capture trailer counters cross-checked against `MessageBusPacked::metricsSnapshot()` | M6 |
| Determinism: byte-identical capture pairs from identical configurations | n/a (no capture exists) | 10 of 10 consecutive pairs | Local `fc /b` (or SHA-256) over paired capture files | M7 |
| Offline re-judge of a stored capture without re-running | Impossible | Referee replays a 10-minute capture and emits verdicts in < 60 s | `--replay` mode timed on the reference capture | M6 |
| Schema-decode drops on the reference scenario | **0, measured at M3** (was: never measured) | 0; any non-zero value surfaced in the log and the trailer, never silent | `metricsSnapshot().schemaHashDrops + decodeFailures + missingSchemaPassthrough` | M3 — **met**: 0 across 132 188 samples of a full load-run-teardown cycle |
| Clean Ctrl-C shutdown with no lost tail | n/a | 20 of 20 runs: final enqueued record present in the file, exit code 0 | Scripted signal-and-verify loop | M7 |
| Independent reader written from the format spec alone | n/a | 1 reader (the sample notebook/script) written without reading EXT-08 source | Deliverable review with the mentor | M6 |

### Non-goals / deferred scope

Brief orientation only; the contract-level deferrals with dates and targets live in §"Out of scope".

- **The other three output shapes from [S1]** — track exporter (KML/GeoJSON), live dashboard bridge, controller/headless script runner. They demo better; they leave EXT-17 starting from zero. Recorder and Referee become EXT-17's "capture the run" and "assertions over the captured run" directly.
- **Campaign orchestration** — starting hosts, sweeping parameters, aggregating across runs. That is EXT-17.
- **Writing back into the simulation** — the `send*` command family is linked and available but unused in v1. The control direction is a stretch goal.

## Out of scope

> Items explicitly **not** part of this MVP. Listing them here rather than omitting them gives reviewers a contract for what this PRD does *not* authorize, and prevents the design from quietly acquiring them.

| Item | Status | Rationale | Target | Added |
|------|--------|-----------|--------|-------|
| Track exporter (KML / GeoJSON output) | Deferred | A second serialiser over the same entity picture; cheap once the picture exists, but adds no capability EXT-17 needs. Bring it in when a mapping-tool demo is asked for. | MVP+1 | 2026-08-30 |
| Live dashboard bridge (HTTP / WebSocket server + web page) | Deferred | Requires an HTTP stack, a second concurrency model, and a browser deliverable. Highest demo value, lowest downstream value; it would consume the whole 1–2 week budget on its own. | Out of roadmap for v1; revisit if a stakeholder demo is scheduled | 2026-08-30 |
| Controller direction (load scenario, start, drive an entity, report) | Deferred | Touches the open question on the `n8ro-sim-app.exe` headless invocation (OQ-2), and duplicates EXT-17's execution piece. Listed in [S1] as a stretch goal. | Stretch after M7 if budget remains; otherwise EXT-17 | 2026-08-30 |
| Predicted-sample handling | Out of scope | No predicted-sample API exists in 2.1.328. There is nothing to handle. Revisit only if a release ships one. | N/A | 2026-08-30 |
| A general expression language for referee conditions | Out of scope | A parser is a project of its own and a well-known scope explosion. v1 ships a closed vocabulary of three condition kinds (BTB-REF-3). | N/A | 2026-08-30 |
| Running two bridge instances and confirming they agree | Deferred | [S1] stretch goal. Valuable as a correctness check on the bus, but not on EXT-17's critical path. | Stretch after M7 | 2026-08-30 |
| Cross-machine / networked bus operation | Deferred | v1 assumes bridge and simulator on one host. Nothing in the design forbids it; nothing validates it either. | TBD, on first request | 2026-08-30 |
| Capture compression or a binary container | Deferred | JSONL costs disk and gains inspectability, which matters more while the format is being learned. Revisit when a capture exceeds ~1 GB in normal use. | v1.1 | 2026-08-30 |

## Key hypotheses

- **H1:** We believe **embedding the live `MessageSchema` (field names, types, sizes, `schemaHash`, `messageId`, `wireVersion`) verbatim in the capture header** will make the format self-describing enough that EXT-17 needs no EXT-08 source, because the header then carries everything required to interpret every subsequent record.
  *Signal: an independent reader is written from `docs/capture-format-v1.md` alone. Validated by: the sample reader deliverable (BTB-DOC-2), written without consulting EXT-08 source.*
  *If false: the format needs a curated, hand-maintained field dictionary, and the spec document becomes a maintenance burden that will drift. Fall back to a frozen, explicitly-enumerated field list with a compatibility test.*
- **H2:** We believe **byte-for-byte determinism is achievable purely by controlling our own emission path** — ordered fields from the schema, a fixed round-trip float format, simulation time as the only clock — because the platform's determinism contract [S2] holds at the bus.
  *Signal: 10 consecutive identical-configuration pairs produce identical captures. Validated by: the M7 determinism harness.*
  *If false — identical runs differ in a field the engine itself published — that is a far more interesting finding than a harness bug, and it belongs in the determinism notes and in front of the mentor immediately. EXT-17's entire premise rests on it.*
- **H3:** We believe **the entity picture is ~2–3 days of work, not a hidden two weeks**, because the decoded stream carries its own schema and the picture is a roster plus a latest-sample map, not a semantic model.
  *Signal: M3 completes within its budget. Validated by: milestone burn-down.*
  *If false: the containment is BTB-CAP-4's verbatim-capture rule — record whatever fields the schema declares rather than mapping them onto a curated struct. See §"Rabbit holes".*
- **H4:** We believe **the bus-level `KEEP_LATEST` default is the dominant loss source, not our writer thread**, because a 100-message subscription queue at entity-update rates will overflow long before a buffered file writer does.
  *Signal: with the bus policy set explicitly and the internal queue sized, bus-side drops fall to zero while internal drops stay at zero. Validated by: the M6 overload demonstration.*
  *If false: the writer is the bottleneck and the internal-queue policy (BTB-BP-4) becomes the load-bearing decision rather than the bus policy.*

## Tenets

Decision tie-breakers for ambiguous trade-offs during implementation — *unless you know better ones.*

1. **The capture is the product.** When a choice is between what is convenient for this program and what makes a stored run re-judgeable a year from now, choose the stored run. EXT-08's own console output is disposable; its files are not.
2. **Simulation time is the only clock.** Wall-clock time may appear in a log line and nowhere else. If a value would differ between two identical runs, it does not belong in the capture path.
3. **Loss is counted, never silent.** A dropped sample, an undecodable message, a topic that never spoke — each produces a number in the trailer and a line in the log. A capture that quietly omits data is worse than one that admits it.
4. **Derive from the schema, never from memory.** Topic strings, field names, types and order come from the registry and the `MessageSchema` at runtime. Hand-spelled literals are how a working program becomes a silently-empty one after an upgrade.
5. **The handler is a courier.** It copies, hands off, and returns. Anything that parses, formats, allocates freely, locks broadly, or touches a file belongs on our own thread.

## Personas and access boundaries

### Analyst — the person asking what the run did
Loads a capture into Python, Excel, or a notebook and asks questions of a finished run: where did an entity go, when did two of them get close, what did the engine actually publish.
**Access level:** Reads capture files and verdict files. Never runs the simulator, never reads EXT-08 source. Their entire contract with this project is `docs/capture-format-v1.md`.

### Integration engineer — the EXT-17 author
Builds the headless campaign runner in a **different repository**. Consumes EXT-08's capture format as a versioned dependency and its referee as the model for offline assertion.
**Access level:** The format specification, the capture files, and the referee's condition-file schema. **No source-level coupling.** If they need something from EXT-08 that is not in the spec, that is a defect in the spec.

### Mentor / reviewer
Reviews the deliverables, answers the open questions this PRD records (OQ-1, OQ-2), and signs off that the demo shows what it claims.
**Access level:** Everything, including the notes on what the stream contained that we did not expect.

### The campaign runner — machine consumer (service persona)
EXT-17's executable reads captures produced by EXT-08 and re-judges them against assertions written after the run finished.
**Access level:** Capture files only, parsed programmatically against the versioned format. It must be able to reject a capture whose `format_version` it does not understand, rather than misreading it.

## Security posture and trust boundaries

> Included for completeness. EXT-08 introduces no authentication or authorization surface; the substantive concerns are data classification and file handling, not access control.

### Trust boundaries
- **Local host boundary.** The bridge and the simulator run as the same user on one machine, communicating over the platform's local message bus. There is no network listener in v1 (the dashboard shape, which would add one, is out of scope) and no remote input.
- **Filesystem boundary.** The capture directory is the only thing EXT-08 writes to. It is caller-supplied and must be validated as an existing, writable directory at startup rather than created implicitly at first write.
- **Content boundary.** Capture files contain scenario content — entity names, positions, teams, outcomes — originating from an Arkheon Technologies proprietary platform and its scenario database. Captures inherit the classification of the scenario they record.

### Enforcement model
- **No auth surface.** EXT-08 authenticates nothing and authorizes nothing. It has exactly the privileges of the user who launched it. Nothing in this PRD authorizes adding a credential store, a listener, or a remote-control path.
- **Fail-safe on output.** IF the capture directory is not writable at startup, THEN the program SHALL report the failure and exit non-zero before subscribing — never start capturing into a void.
- **No throw, ever.** Per [S1] and the platform contract, failures are return values plus logging. That is a robustness property as much as a style rule: an exception escaping a bus handler crosses a library boundary we do not own.

### Threat model

| Threat | Impact | Mitigation |
|--------|--------|------------|
| A capture is shared outside the organisation, carrying proprietary scenario content | Disclosure of scenario design and platform behaviour | Treat captures at the classification of their scenario. The README states this. Sample captures committed to the repo use a stock demo scenario only. |
| Capture path is a symlink or contains traversal components | Writes land outside the intended directory | Canonicalise and validate the output directory at startup; refuse traversal components in the run label used for filenames. |
| Disk exhaustion during a long run | Run lost, and possibly the host destabilised | Bounded capture with a documented size ceiling and an explicit stop-or-rotate decision (BTB-CAP-6); free-space check at startup. |
| A malformed or hostile packed message crashes the decoder | Bridge crash mid-run, capture truncated | Decode failures are counted, not fatal (`decodeFailures` in `metricsSnapshot()`); the handler never throws and never assumes a field is present. |
| Secrets in configuration (none expected) | n/a | Configuration is an engine config name, a model path, and a schema file name. No credentials. Do not add any. |

## Functional requirements

Priorities: **P1** = required for v1 acceptance. **P2** = valuable, ship if budget allows.

### Naming and interface conventions

> EXT-08 exposes no REST API and no SDK. It does expose three surfaces that EXT-17 and the analyst bind to, and drift in any of them is as damaging as endpoint drift would be. This table is the source of truth; FRs below reference it rather than restating it.

**CLI surface (authority: this table).**
- Invocation is `n8ro-bridge [options]`. Long options only, kebab-case, GNU style: `--config`, `--model-path`, `--schema-file`, `--out-dir`, `--run-label`, `--conditions`, `--replay`, `--queue-size`, `--overflow-policy`, `--log-level`.
- `--config`, `--model-path`, `--schema-file` map one-to-one onto `SimulationEngineClientConfig{simEngineConfigName, modelPath, schemaFileName}`. The names deliberately mirror the SDK struct so a reader of one can find the other.
- `--replay <capture>` selects offline mode: no bus, no client, referee only. Live mode and replay mode are mutually exclusive and the program SHALL reject an invocation supplying both `--replay` and `--config`.

**File and path conventions.**
- Capture: `<out-dir>/capture-<scenario>-<run-label>.n8rocap.jsonl`. Verdicts: `<out-dir>/verdicts-<scenario>-<run-label>.jsonl`.
- `<run-label>` defaults to a zero-padded ordinal (`000`, `001`, …) derived from what already exists in `<out-dir>`. **It never defaults to a timestamp**, because campaign tooling addresses runs by path and a wall-clock name makes two identical runs unaddressable as a pair.
- The format specification lives at `docs/capture-format-v1.md` in this repository, and its version string is `n8ro-capture/1`.

**Record-type vocabulary (authority: the format spec; this is the closed set for v1).**
- `header`, `segment_open`, `segment_close`, `entity_add`, `sample`, `entity_remove`, `verdict`, `trailer`. All lowercase snake_case. A record type not in this list is a format-version change, not an addition.

**Platform-string convention.**
- Topic strings, event names, and field names are **read at runtime** from `MessageBusPackedSchemaRegistry` and `MessageSchema`, or referenced through the SDK's own constants (`n8ro::sim::kEventScenarioLoaded`, `kEventEntityDeleted`, `n8ro::sim::entity_removal::*`). Hand-written topic or field literals are prohibited in the capture path — per [S5]'s golden rule, derive, don't guess.

### Connection and session lifecycle (CX)

#### BTB-CX-1 (P1): Client bring-up from explicit configuration
The system SHALL construct a `SimulationEngineClient` from a caller-supplied engine config name, model path, and schema file name, and SHALL report a named, actionable failure and exit non-zero if `create()` returns `std::nullopt`.

**Customer scenario:** An analyst on a fresh machine runs the bridge for the first time and needs to know *which* of the three configuration values is wrong, not merely that something failed.
**Pain removed:** [S1] warns that "most of the difficulty in this task is in the configuration, not the logic." A bare "failed to create client" turns a five-minute fix into an afternoon.

**Acceptance criteria:**
- All three values are supplied on the command line; none are compiled in.
- A failed `create()` produces a log line naming each configuration value as it was resolved, and exit code is non-zero.
- No exception escapes `main` under any configuration error.

**Trace:** UAC-BTB-CX-1

#### BTB-CX-2 (P1): No required start order
WHILE the simulator is not yet present, the system SHALL wait and retry bring-up at a bounded interval, and WHEN the simulator appears, the system SHALL connect and begin capturing without operator intervention.

**Customer scenario:** EXT-17 launches the bridge and the simulation host from one script and cannot guarantee which wins the race.
**Pain removed:** A required start order makes the bridge unusable from automation — every campaign run would need a sleep, and a sleep is the thing [S2] explicitly rejects.

**Acceptance criteria:**
- Bridge started 30 s before the simulator captures the full scenario.
- Bridge started while a scenario is already running captures from the point of attachment and records in the header that it attached mid-run.
- The retry interval is bounded and logged; the wait is interruptible by Ctrl-C.

**Trace:** UAC-BTB-CX-2

#### BTB-CX-3 (P1): The simulator exiting does not crash or hang the bridge
IF the simulator exits, stops publishing, or tears down while the bridge is connected, THEN the system SHALL detect the loss, close the current capture segment with a terminal record, flush and close the file, and either exit cleanly or return to the wait-and-connect state as configured.

**Customer scenario:** An unattended overnight run ends — or dies — and the analyst arrives to a complete, closed, readable capture rather than a truncated file and a hung process.
**Pain removed:** A hung bridge blocks the next campaign run and leaves a capture whose last record may be half-written. [S2] requires a campaign to survive a host that dies mid-run.

**Acceptance criteria:**
- Killing the simulator mid-run leaves a capture whose final record is a well-formed `trailer` marking the run as terminated by host loss.
- The bridge process exits within a bounded, documented detection window; it never blocks indefinitely on a dead bus.
- No crash, no exception, no partially-written final line.

**Trace:** UAC-BTB-CX-3

#### BTB-CX-4 (P1): Scenario load and reload are unambiguous in the capture
WHEN a `scenario_loaded` or `scenario_unloaded` event arrives on `sim/scenario/event`, the system SHALL close any open segment with a `segment_close` record and open a new one with a `segment_open` record naming the scenario and a monotonically increasing segment ordinal.

**Customer scenario:** An operator reloads a scenario to try it again, and the analyst — or EXT-17 — must never mistake the two attempts for one run.
**Pain removed:** [S1] and [S2] both require this explicitly: two runs silently mixed in one file makes every assertion over that file meaningless, and the mixing is invisible.

**Acceptance criteria:**
- A load–run–reload–run sequence produces exactly two `segment_open`/`segment_close` pairs with distinct ordinals.
- No `sample` record ever appears outside an open segment.
- Segment records carry the scenario name as reported by the event, not as supplied on the command line.

**Trace:** UAC-BTB-CX-4

### Entity picture (EP)

> **This group is the work item [S1] assumed was free.** The SDK provides no roster and no latest-sample cache; both are built here. Budget: 2–3 days.

#### BTB-EP-1 (P1): Schema registration and a loud empty registry
The system SHALL load the packed-message schemas from the model database into a `MessageBusPackedSchemaRegistry` before subscribing, and IF the registry is empty or the expected entity-state topic has no schema, THEN the system SHALL log a diagnostic naming the condition and SHALL NOT proceed to silent operation.

**Customer scenario:** An engineer points the bridge at a model path whose schema file does not match the engine's, and gets an immediate, named diagnosis instead of an empty capture file.
**Pain removed:** [S1]'s rule — "a packed message decodes only when its schema is registered on both sides; a silent topic is the first thing to check" — describes exactly the failure that looks like success. A schema mismatch drops messages with a warning, not an error, so an unchecked bridge produces a plausible empty file.

**Acceptance criteria:**
- Registry size and the resolved entity-state topic are logged at startup.
- An empty registry produces a non-zero exit with a message naming the model path and schema file.
- The entity-state topic string is obtained from the registry, never hand-written.
- Resolution is anchored on a **message-instance name** or an `EventNames.h` constant — never on a topic string — and the schema the anchor resolves to is checked to declare the fields the picture keys on before any subscription is made. A name that resolves to a plausible neighbour (`simEntityTrackUpdate`, `simEntityPoseUpdate`) must fail loudly rather than subscribe successfully and roster nothing.

**Trace:** UAC-BTB-EP-1

#### BTB-EP-2 (P1): Decoded subscription to entity state
The system SHALL subscribe to entity-state traffic via `MessageBusPacked::subscribeByTopic` with a `DecodedHandler`, so that each arrival delivers the raw `Message`, its `MessageSchema`, and the decoded `StreamValueMap`.

**Customer scenario:** The analyst wants field-level data — position, velocity, team, phase — not opaque bytes they would have to decode themselves.
**Pain removed:** Raw `MessageHandler` subscriptions on `SimulationEngineClient` deliver undecoded bus messages. Decoding by hand duplicates `MessageBinaryCodec` and would drift from the schema on the next release.

**Acceptance criteria:**
- The handler receives decoded values; no manual payload parsing exists in the codebase.
- The `MessageSchema` reference delivered with each message is used for field order (see BTB-CAP-3), not discarded.
- Subscription IDs are retained for a clean `unsubscribe` at shutdown.

**Trace:** UAC-BTB-EP-2

#### BTB-EP-3 (P1): Roster lifecycle from entity events
The system SHALL maintain a roster of live entities driven by `sim/entity/event`, adding on `entity_created`, and removing on `entity_deleted` while preserving the event's `reason` value (`destroyed`, `expended`, `commanded`, `despawned`, `scenario_unload`, or a supplier-specific string).

A scenario entity name is **not** a unique identity across a run. The engine's stop path deletes every entity with `reason="scenario_unload"` and immediately re-creates the whole roster under the same names, and a *destroyed* entity returns the same way. The system SHALL therefore track an **occupancy**: a monotonically increasing generation per name, opened by `entity_created` and closed by `entity_deleted`. A sample belongs to the occupancy open when it arrives, and a sample arriving for a name with **no** open occupancy SHALL be counted and excluded rather than attributed to a closed one. See ADR-6.

**Customer scenario:** The analyst asks why an entity stopped appearing and gets "destroyed at t=412.5" rather than having to infer it from an absence.
**Pain removed:** [S1] requires that entity removal be reflected and that nothing linger after a body is gone. Inferring removal from silence is unreliable — a quiet entity and a dead one look identical in a sample stream.

**Acceptance criteria:**
- An entity removed mid-run produces exactly one `entity_remove` record carrying the reason string verbatim.
- **No `sample` record for that entity's occupancy appears after that occupancy's `entity_remove`.** A later `sample` under the same name is permitted only after a new `entity_add` has opened the next occupancy, and carries that occupancy's ordinal. *(Rev 2: the previous wording — "no `sample` record for that entity appears after its `entity_remove`" — is unsatisfiable on this platform, because samples demonstrably resume under a re-created name. See ADR-6.)*
- A sample for a name with no open occupancy is counted as a named diagnostic and never enters the latest-sample map.
- A removal reason not in the engine's own set is recorded verbatim rather than coerced or dropped.

**Trace:** UAC-BTB-EP-3

#### BTB-EP-4 (P1): Latest-sample map with deterministic ordering
The system SHALL maintain a latest-published-sample map keyed by scenario entity name, using an ordered container, and SHALL expose a snapshot of the roster and latest samples to the referee.

**Customer scenario:** The referee asks "how far apart are these two aircraft right now" and needs both entities' most recent published state at a single, consistent moment.
**Pain removed:** Without a picture, every condition would have to be evaluated against a stream of one entity at a time, and cross-entity conditions — the interesting ones — would be impossible.

**Acceptance criteria:**
- The container is ordered (`std::map` or equivalent); no `unordered_*` container is iterated anywhere in the capture or verdict path.
- A snapshot is internally consistent — it is not read while a write is in progress.
- Sample staleness is visible: each entry carries the simulation time of its last published sample.
- Each entry identifies the occupancy its sample belongs to, so a stale entry from a previous tenure of the same name can never be read as current.

**Trace:** UAC-BTB-EP-4

### Capture format and recorder (CAP)

> **This group is the cross-repo contract.** Every requirement here is written to be read by someone in a different repository who cannot see this code.

#### BTB-CAP-1 (P1): A versioned, self-describing capture header
The system SHALL write, as the first record of every capture, a `header` record carrying: `format_version` (`n8ro-capture/1`), the producing tool's name and version, the engine configuration name, model path and schema file name, the resolved entity-state topic, whether the bridge attached mid-run, and a verbatim schema envelope for every message type it will record — `messageName`, `topic`, ordered `fields[]{name, type, size}`, `schemaHash`, `messageId`, `wireVersion`.

**Customer scenario:** The EXT-17 author, in a different repository with no access to EXT-08 source, opens a capture and can interpret every subsequent record from the file's own first line.
**Pain removed:** Without an embedded schema, the format is only as good as a prose document that will drift from the code within one release. With it, the file carries its own dictionary, and a `schemaHash` mismatch between two captures is detectable rather than mysterious.

**Acceptance criteria:**
- The header alone is sufficient to name and type every field in every later `sample` record.
- `format_version` is the first key of the first record, so a reader can reject an unknown version before parsing anything else.
- Schema values are copied from the `MessageSchema` delivered by the runtime, never hand-transcribed.

**Trace:** UAC-BTB-CAP-1

#### BTB-CAP-2 (P1): Simulation time is the only clock in the capture
The system SHALL stamp every capture record with the simulation time carried by the sample or event that produced it, and SHALL NOT write any wall-clock-derived value into any capture record.

**Customer scenario:** The EXT-17 author diffs two captures byte-for-byte to prove determinism, and needs the files to differ only when the simulation differed.
**Pain removed:** A single wall-clock field anywhere in the capture makes byte-for-byte comparison impossible, which removes the foundation EXT-17's entire self-test stands on. [S1] and [S2] both state the rule; this FR makes it checkable.

**Acceptance criteria:**
- No call to any wall-clock or steady-clock source appears in the capture-writing path. Log lines may carry wall-clock time; capture records may not.
- Records carry the published simulation time. **How we know these are not predictions:** release 2.1.328 ships no predicted-sample API — `EntityStateSample.h` does not exist and `SimulationEngineClient` exposes no prediction accessor. Every value in the capture arrives off the bus as an engine publication. The README states this and cites the release.
- The determinism harness (M7) is the enforcing test.

**Trace:** UAC-BTB-CAP-2

#### BTB-CAP-3 (P1): Byte-for-byte reproducibility of the capture
Given two runs of the same scenario under the same configuration, the system SHALL produce byte-identical capture files.

**Customer scenario:** EXT-17's determinism self-test runs before every campaign and must attribute any difference to the simulation, never to the recorder.
**Pain removed:** Three concrete non-determinism sources exist in this design and all three are ours: `StreamValueMap` is an `std::unordered_map` whose iteration order is not stable; floating-point formatting is lossy and locale-sensitive by default; and any container of entities iterated for output must be ordered. Each would silently break EXT-17 in a way that looks like a platform defect.

**Acceptance criteria:**
- Field order in every `sample` record follows the `MessageSchema::fields` vector, never `StreamValueMap` iteration order.
- Floating-point values are emitted with a fixed, round-trip-exact, locale-independent format (17 significant digits), so a value written and re-read is bit-identical.
- No container with unspecified iteration order is iterated anywhere in the capture path.
- Ten consecutive identical-configuration pairs hash identically.

**Trace:** UAC-BTB-CAP-3

#### BTB-CAP-4 (P1): The closed record-type vocabulary, captured verbatim
The system SHALL emit records of exactly the eight types named in §"Naming and interface conventions", and each `sample` record SHALL carry every field the message's schema declares, verbatim, without curation, renaming, or unit conversion.

**Customer scenario:** The analyst finds a field in the stream that nobody anticipated — and it is in the capture, because the recorder never decided which fields mattered.
**Pain removed:** Curating a field list means every new field the platform publishes is silently lost until someone updates a struct, and it makes the "page of notes on what the stream contained that we did not expect" [S1] impossible to write, because the unexpected was filtered out before anyone saw it.

**Acceptance criteria:**
- Adding a field to a message schema in the database causes it to appear in new captures with no code change.
- Units and frames are recorded as the platform publishes them (degrees, metres, m/s, NED where applicable); no conversion is applied. The format spec states the units, per [S5]'s schema reference.
- A `trailer` record closes every capture, carrying the counters required by BTB-BP-4 and BTB-OBS-1.

**Trace:** UAC-BTB-CAP-4

#### BTB-CAP-5 (P1): The format specification is a versioned repository artifact
The system's capture format SHALL be specified field by field in `docs/capture-format-v1.md`, versioned in this repository, stating for each record type every field's name, type, unit, and meaning, plus the compatibility rule for readers encountering an unknown `format_version`.

**Customer scenario:** The EXT-17 author begins work in a separate repository and needs a stable contract to build a reader against, not a conversation.
**Pain removed:** The two projects have separate git repositories [S3]. Without a documented, versioned artifact crossing that boundary, the only available contract is EXT-08's source — which is exactly the coupling the repo split exists to prevent.

**Acceptance criteria:**
- The document is complete enough that the sample reader (BTB-DOC-2) is written from it alone.
- The compatibility rule is explicit: a reader encountering an unrecognised `format_version` rejects the file with a named error rather than attempting a partial parse.
- The version string appears in exactly two places — the spec title and the `header` record — and they are checked against each other by a test.

**Trace:** UAC-BTB-CAP-5

#### BTB-CAP-6 (P2): Bounded capture size with an explicit end-of-space behaviour
The system SHALL enforce a configurable maximum capture size and SHALL, on reaching it, take one documented action — stop capturing with a terminal `trailer`, or roll to a numbered continuation file — never silently truncate.

**Customer scenario:** An overnight run fills a disk and the analyst finds a closed, valid, explicitly-truncated capture rather than a corrupt one.
**Pain removed:** An unbounded writer on an unattended run is a host-stability risk, and a capture cut off by ENOSPC mid-line is unparseable by every reader.

**Acceptance criteria:**
- The limit and the chosen action are stated in the README and in the `header`.
- Reaching the limit produces a well-formed `trailer`.

**Trace:** UAC-BTB-CAP-6

### Threading and backpressure (BP)

#### BTB-BP-1 (P1): The subscription handler does no work
The system's bus handlers SHALL copy what they need, hand it to a queue, and return. All file IO, formatting, condition evaluation, and allocation-heavy work SHALL occur on the bridge's own thread.

**Customer scenario:** The simulation runs at full rate with the bridge attached and is not slowed by it.
**Pain removed:** [S1] states the rule directly. A handler that writes a file holds up the bus's delivery thread, which at best drops messages and at worst perturbs the simulation the capture is supposed to observe faithfully.

**Acceptance criteria:**
- No file, socket, or formatting call exists inside any handler.
- Handler time is bounded and measured; the measurement appears in the run summary.
- No lock held inside a handler is also held during IO.

**Trace:** UAC-BTB-BP-1

#### BTB-BP-2 (P1): FIFO per topic across the thread boundary
The system SHALL preserve per-topic message order across the handler-to-writer queue.

**Customer scenario:** The analyst reads a trajectory in the order the engine produced it, and the referee sees a crossing before the departure that follows it.
**Pain removed:** [S1]: "Order is part of the meaning." A reordered capture produces trajectories that appear to jump backwards and conditions that fire in the wrong sequence — errors that look like simulation defects.

**Acceptance criteria:**
- Records for a given topic appear in the capture in arrival order; verified against `Message::sequenceNumber` where the platform populates it.
- A sequence gap or out-of-order arrival is counted and reported, not silently accepted.

**Trace:** UAC-BTB-BP-2

#### BTB-BP-3 (P1): The bus-side backpressure policy is set explicitly, never defaulted
The system SHALL set `SubscriptionOptions::backpressurePolicy` and `queueSize` explicitly on every subscription, and SHALL document the chosen values and the reasoning in the README.

**Customer scenario:** The analyst asks whether the capture is complete and gets an answer grounded in a stated policy rather than in an inherited default nobody chose.
**Pain removed:** The default is `KEEP_LATEST` with `queueSize = 100`. For a recorder that is precisely wrong: it discards the older of two samples, which is the one already committed to the run's history. Accepting the default would produce a capture that looks complete and is not — and nothing in [S1] warns about it.

**Acceptance criteria:**
- Both values appear as explicit arguments at the subscription call site, with a comment naming the default they override.
- The values are logged at startup and written into the capture `header`.
- The README explains why `KEEP_LATEST` is unsuitable for a recorder.

**Trace:** UAC-BTB-BP-3

#### BTB-BP-4 (P1): The internal queue is bounded and its overflow is counted
The system SHALL bound the handler-to-writer queue at a configurable size and SHALL apply one configurable, documented overflow policy — drop-oldest-with-count, drop-newest-with-count, or block — recording the resulting drop count in the capture `trailer` and in the run summary.

**Customer scenario:** The analyst reading a capture from a heavily-loaded run can see exactly how many samples were lost and where, instead of wondering.
**Pain removed:** [S1]: "not deciding is not an option." An unbounded queue turns an overload into an out-of-memory kill; a silent bounded one turns it into a quietly wrong file. The counter is what makes the third tenet true.

**Acceptance criteria:**
- Default policy and size are stated in the README with the reasoning.
- A deliberate overload (slow disk or an artificially throttled writer) produces a non-zero, accurate drop count in the `trailer`.
- Drop counts are per-topic, so a loss can be attributed.
- The demonstration is recorded in the 5-minute video (BTB-DOC-2).

**Trace:** UAC-BTB-BP-4

### Referee (REF)

#### BTB-REF-1 (P1): Conditions are declared outside the code
The system SHALL read its conditions from a caller-supplied declaration file, separate from the program, such that adding or changing a condition requires no rebuild.

**Customer scenario:** The analyst adds "did red get within 3 km of the corridor" to an existing set of checks without touching C++ or waiting for a build.
**Pain removed:** [S2] requires that assertions be declared separately from the code that runs the simulation. Conditions compiled into the binary cannot be re-applied to a stored run, which is the whole point of the capture.

**Acceptance criteria:**
- The condition file's schema is documented in the README.
- A malformed condition file produces a named parse error and a non-zero exit before any subscription is made — never a silent zero-condition run.
- Each condition carries a stable identifier used in its verdict.

**Trace:** UAC-BTB-REF-1

#### BTB-REF-2 (P1): Verdicts carry what was checked, on what data, and when
WHEN a declared condition is met, the system SHALL emit a `verdict` record naming the condition identifier, the entities involved, the values that satisfied it, and the **simulation time** at which it was met.

**Customer scenario:** A condition fires and the analyst can go straight to that moment in the capture and see the samples that caused it.
**Pain removed:** [S2] requires that a failure name what was checked and on what data. A bare "condition 3 met" sends the reader hunting through thousands of records with no anchor.

**Acceptance criteria:**
- Every verdict is reproducible from the capture alone: the named entities, values, and simulation time locate the exact samples that triggered it.
- A condition never met produces an explicit not-met verdict at end of run, not silence.
- Verdict simulation times obey BTB-CAP-2 — no wall clock.

**Trace:** UAC-BTB-REF-2

#### BTB-REF-3 (P1): A closed condition vocabulary of three kinds
The system SHALL support exactly three condition kinds in v1: **proximity** (two named entities within a distance threshold), **area** (a named entity inside or outside a declared geodetic region), and **terminal state** (a named entity reaching a removal reason or a declared state value).

**Customer scenario:** The analyst expresses the questions [S1] names — "did red cross this line", "did the two aircraft come within 5 km" — and gets an answer without learning a query language.
**Pain removed:** These three cover the brief's stated examples and EXT-17's assertion needs ("came within a distance", "reached the area", "reached a terminal state it should not have") with a fixed, testable surface. A general expression language is a project of its own and is explicitly out of scope.

**Acceptance criteria:**
- Each kind has documented parameters, units (metres, degrees — per the platform's own units, not converted), and semantics including the boundary case.
- An unrecognised condition kind is a named parse error at load, never a silently skipped condition.
- Distance uses a stated geodetic method, documented in the README, so a result is reproducible by a third party.

**Trace:** UAC-BTB-REF-3

#### BTB-REF-4 (P1): The referee runs offline against a stored capture
The system SHALL support a replay mode that evaluates a condition file against a stored capture file with no simulator, no bus, and no client, producing verdicts identical to those a live run would have produced from the same records.

**Customer scenario:** The EXT-17 author writes a new assertion on Tuesday and applies it to Monday's twenty captured runs without re-running a single one.
**Pain removed:** This is the cross-repo constraint made executable. [S2]: "a stored run should be re-assertable without re-running it; that is what makes a campaign cheap to iterate on." It is also the strongest possible conformance test for the format — if the referee can re-derive its own verdicts from the file alone, the file demonstrably contains enough.

**Acceptance criteria:**
- Live verdicts and replay verdicts over the same run and the same condition file are byte-identical.
- Replay mode rejects a capture whose `format_version` it does not recognise, with a named error.
- Replay of a 10-minute capture completes in under 60 seconds.

**Trace:** UAC-BTB-REF-4

### Observability and diagnostics (OBS)

#### BTB-OBS-1 (P1): Decode and drop diagnostics are surfaced, not buried
The system SHALL report `MessageBusPacked::metricsSnapshot()` counters — `decodeFailures`, `schemaHashDrops`, `missingSchemaPassthrough`, `messageIdDrops`, and received message/byte counts — periodically to the log and once into the capture `trailer`.

**Customer scenario:** An engineer whose capture is emptier than expected sees `schemaHashDrops` climbing and diagnoses a schema mismatch in seconds.
**Pain removed:** [S1] warns that a schema mismatch "drops the message with a warning rather than failing loudly." The platform already counts these; not surfacing them means re-deriving by hand a diagnosis the runtime is already holding.

**Acceptance criteria:**
- All five counters appear in the `trailer` and in the end-of-run summary.
- A non-zero `schemaHashDrops` or `decodeFailures` produces a distinct warning naming the likely cause and the two things to check (model path, schema file).

**Trace:** UAC-BTB-OBS-1

#### BTB-OBS-2 (P2): Silent-topic detection and end-of-run summary
WHILE the engine reports itself running, IF a subscribed topic has produced no decoded messages for a configurable interval, THEN the system SHALL log a warning naming the topic and the schema-mismatch hypothesis; and at exit the system SHALL print a one-screen run summary.

**Customer scenario:** The engineer walks away from a run and returns to a log that already told them the entity-state topic never spoke.
**Pain removed:** A silent topic is indistinguishable from a quiet simulation until someone thinks to check — turning the most common configuration failure into the slowest to find.

**Acceptance criteria:**
- The warning fires only while the engine reports running, so a paused simulation does not generate noise.
- The summary states segments, samples, entities seen and removed, verdicts, and every drop counter.

**Trace:** UAC-BTB-OBS-2

### Shutdown (SD)

#### BTB-SD-1 (P1): Clean Ctrl-C shutdown with no lost tail
WHEN an interrupt signal is received, the system SHALL unsubscribe every subscription, stop the message pump, drain the internal queue, write the `trailer`, flush and close every open file, and exit zero — with every record enqueued before the signal present in the capture.

**Customer scenario:** The analyst stops a long run with Ctrl-C and the capture is complete and valid up to the moment they stopped it.
**Pain removed:** [S1] requires it explicitly. A truncated final line makes the whole file unparseable by a strict reader, and a lost tail silently discards the most recent — often the most interesting — part of the run.

**Acceptance criteria:**
- Twenty scripted interrupt-and-verify cycles produce twenty valid captures, each ending in a well-formed `trailer`, exit code 0 each time.
- The signal handler itself does no work beyond setting a flag — no IO, no allocation, no locking.
- A second Ctrl-C during drain forces exit with a logged warning rather than hanging.

**Trace:** UAC-BTB-SD-1

### Documentation and evidence (DOC)

#### BTB-DOC-1 (P1): README and the field-by-field format specification
The repository SHALL contain a README covering build (VS 2026 v18.x, C++17, Release|x64, `setup.cmd` then `dev\setup-dev.cmd`), configuration (engine config name, model path, schema file name), invocation, the condition-file schema, both backpressure decisions with their reasoning, and the capture format documented field by field — the last either inline or in `docs/capture-format-v1.md`, which the README links.

**Customer scenario:** A new engineer clones the repository against a stock 2.1.328 install and has the bridge capturing within an hour.
**Pain removed:** [S1] lists this as a deliverable. Without the field-by-field format, the capture is a private data structure and the repo boundary with EXT-17 cannot be crossed.

**Acceptance criteria:**
- Build instructions verified on a clean machine with a stock install.
- Every record type and every field is documented with type, unit, and meaning.
- Both backpressure decisions (BTB-BP-3, BTB-BP-4) appear with their reasoning, not merely their values.

**Trace:** UAC-BTB-DOC-1

#### BTB-DOC-2 (P1): The evidence pack
The repository SHALL contain a sample capture from a real run, the reader that consumes it, a 5-minute end-to-end demo recording, and a page of notes on what the stream contained that was not expected.

**Customer scenario:** The mentor reviews the work and can see it running, read its output, and read what was learned — without a live session.
**Pain removed:** [S1] names all four as deliverables. The notes in particular are the transferable product: this is the first bus client anyone has built against this release, and what surprised us is what the next integrator most needs.

**Acceptance criteria:**
- The sample capture comes from a real run of a stock scenario, not a fabricated file.
- The reader is written from `docs/capture-format-v1.md` alone — validating H1.
- The recording shows: start-before-simulator, a scenario load, a reload producing two segments, an entity removal, a verdict firing, the backpressure demonstration, and a Ctrl-C with a clean tail.
- The notes page records at least the resolved values for OQ-3 and OQ-5, and any behaviour observed that this PRD did not predict.

**Trace:** UAC-BTB-DOC-2

## Scope authority

The FR sections above are the **contract** for this PRD. The design document (`docs/design.md` — to be added when the design is created) realizes these FRs as components, sequences, and milestone tasks.

**The design must not introduce surface area beyond this PRD's FR table without a corresponding PRD revision.** If the design proposes a new record type, a new CLI option, a fourth condition kind, a new output file, or a network listener not authorized by an FR, the PRD must be updated first — adding the FR through the revision flow. This matters more than usual here: the capture format is consumed across a repo boundary, so unauthorized surface added at design time becomes a contract EXT-17 depends on before anyone agreed to it.

Conversely, **this PRD must not specify implementation detail beyond FR shape.** Thread counts, queue implementations, class decomposition, the JSON serialiser, and the geodetic distance library belong in the design, not here. Where this document names a concrete mechanism — the `MessageSchema` field order, the 17-digit float format, the two backpressure boundaries — it is because that mechanism *is* the externally-observable contract, not because the design has been pre-empted.

## Data model: the capture as a logical entity

**Note:** This is the logical shape of the capture stream. The on-disk encoding (JSON Lines, UTF-8, LF-terminated, one record per line) is fixed by the format spec; nothing else about storage is implied.

### Record: common envelope

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `type` | string | Yes | One of the eight record types in the closed vocabulary |
| `sim_time_s` | double | Yes, except `header` | Simulation time the record's cause carried, in seconds. Never wall-clock |
| `segment` | int | Yes, except `header`/`trailer` | Ordinal of the enclosing scenario segment, from 0 |

### Record: `header`

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `format_version` | string | Yes | `n8ro-capture/1`. First key in the record; a reader checks it before parsing further |
| `producer` | object | Yes | Tool name and version that wrote the file |
| `platform` | object | Yes | Engine config name, model path, schema file name, release version observed |
| `attached_mid_run` | bool | Yes | True if the bridge connected to an already-running scenario |
| `subscription` | object | Yes | Resolved topic, `backpressure_policy`, `queue_size` — the values from BTB-BP-3 |
| `schemas` | array | Yes | One entry per recorded message type: `message_name`, `topic`, `schema_hash`, `message_id`, `wire_version`, and ordered `fields[]{name, type, size}` |

### Record: `segment_open` / `segment_close`

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `scenario` | string | Yes | Scenario name as carried by the `sim/scenario/event` payload |
| `reason` | string | `segment_close` only | `scenario_unloaded`, `host_lost`, `shutdown`, or `size_limit` |

### Record: `entity_add` / `entity_remove`

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `entity` | string | Yes | Scenario entity name — the key of the latest-sample map |
| `occupancy` | int | Yes | Which tenure of this name the record belongs to, from 1. `entity_add` opens it; the matching `entity_remove` carries the same value. A name re-created after removal gets the next ordinal (BTB-EP-3) |
| `reason` | string | `entity_remove` only | Verbatim from the event: `destroyed`, `expended`, `commanded`, `despawned`, `scenario_unload`, or a supplier-specific value |

### Record: `sample`

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `entity` | string | Yes | Scenario entity name |
| `occupancy` | int | Yes | The tenure of `entity` this sample belongs to. Always matches an `entity_add` that precedes it and has not been closed. This is what lets a reader tell two tenures of one name apart (BTB-EP-3) |
| `message` | string | Yes | `MessageSchema::messageName`, resolving this record's fields against `header.schemas` |
| `fields` | object | Yes | Every field the schema declares **that the message actually carried**, in schema order, values as published — no curation, no unit conversion. A schema-declared field the publisher omitted is absent from the object rather than defaulted; `header.schemas` remains the full declaration |

### Record: `verdict`

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `condition_id` | string | Yes | Stable identifier from the condition file |
| `met` | bool | Yes | Whether the condition was satisfied |
| `entities` | array of string | Yes | Entities the evaluation involved |
| `values` | object | Yes | The values that decided it, sufficient to locate the causing samples |

### Record: `trailer`

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `end_reason` | string | Yes | `shutdown`, `host_lost`, `size_limit`, or `replay_end` |
| `counts` | object | Yes | Segments, samples, entities added and removed, verdicts |
| `drops` | object | Yes | Per-topic internal-queue drops, plus bus-side loss where observable |
| `bus_metrics` | object | Yes | The five `metricsSnapshot()` counters from BTB-OBS-1 |

### State model: the capture session

`waiting` → `attached` → `segment_open` ⇄ `segment_closed` → `closing` → `closed`

| Transition | Trigger | Rules |
|-----------|---------|-------|
| `waiting` → `attached` | `create()` succeeds and the pump starts | Header is written on entry to `attached`, before any sample |
| `attached` → `segment_open` | `scenario_loaded`, or first sample when attached mid-run | No `sample` record may be written outside an open segment |
| `segment_open` → `segment_closed` | `scenario_unloaded`, host loss, shutdown, or size limit | Exactly one `segment_close` per `segment_open`; ordinals strictly increase and never repeat within a file |
| `segment_closed` → `segment_open` | A subsequent `scenario_loaded` | This is the reload case BTB-CX-4 exists for |
| any → `closing` | Ctrl-C or host loss | Drain, then `trailer`, then flush, then close |
| `closing` → `closed` | Drain completes or second interrupt | A `closed` capture always ends in a well-formed `trailer` |

## Migration plan

> EXT-08 is greenfield: no existing behaviour and no existing data to migrate. The migration concern that *does* exist is real, and it is forward-facing — **capture format version migration once EXT-17 consumes `n8ro-capture/1`.**

### Current state
No capture format exists. No consumer exists. Format changes cost nothing.

### Target state
`n8ro-capture/1` is published as a versioned artifact and consumed by a reader in a different repository. From that point, a format change is a breaking change to a downstream project.

### Migration steps
1. **Ship `n8ro-capture/1` with the spec** (M4). *Validation:* the spec is complete and the sample reader is written from it alone.
2. **Freeze the format at EXT-17 handoff** (end of M7). *Validation:* the mentor and the EXT-17 author acknowledge the spec as the contract. After this point, changes follow step 3.
3. **Version-bump for any breaking change.** Adding a field to a `sample` record is non-breaking (readers key by schema); adding a record type, renaming a key, or changing a unit is breaking and requires `n8ro-capture/2`. *Validation:* the reader's version check rejects the new version cleanly rather than misreading it.
4. **Keep the reader honest across versions.** *Validation:* an old capture must remain readable by the current reader, or the spec must state that it is not — never leave it ambiguous.

### Backward compatibility
- A reader encountering an unknown `format_version` rejects the file with a named error (BTB-CAP-5). That is the entire compatibility mechanism, and it is deliberately blunt: partial parsing of an unknown format is how silently-wrong analysis happens.
- Captures already written are never rewritten. A version bump means old files stay valid under their own version.

### Deprecation path
Not applicable in v1 — there is no prior format. If `n8ro-capture/2` ever ships, v1 captures remain readable and the spec carries both.

## Performance requirements

~~Baselines are unknown by construction~~ — **M1 measured them**, and the figures below are observation rather than assumption.

**Observed on release 2.1.328, one host.** Every entity publishes once per frame at **20 Hz**, locked to the frame (`deltaTimeS` = 0.05), so the aggregate entity-state rate is `entity count × 20 /s` at roughly 210 packed bytes per sample:

| Scenario | Entities | `sim/entity/state` rate | Packed payload |
|---|---:|---:|---:|
| `Atacama Air Defense` — the **reference** | 42 at load, 46 distinct in-window | 818 /s | ~170 KB/s |
| `Outback Kamikaze Swarm` — the **overload** case for M6 | 126 | 2487 /s | ~520 KB/s |

A 10-minute reference run is therefore on the order of **100 MB of payload** before any text encoding. Everything else on the bus is comparatively negligible: `sim/engine/state` runs one per frame (~20 /s), and `sim/entity/event` — the roster source — produced 134 messages across a whole run.

### Latency targets

| Operation | p50 target | p95 target | p99 target | Gating? |
|-----------|-----------|-----------|-----------|---------|
| Handler execution (copy + enqueue, per message) | < 20 µs | < 100 µs | < 500 µs | **Yes** — a slow handler perturbs the bus and violates BTB-BP-1 |
| Enqueue-to-durable (record written and flushed) | < 50 ms | < 250 ms | < 1 s | No — tracked; the bound that matters is queue depth, not latency |
| Startup to first capture record after simulator appears | < 2 s | < 5 s | — | No |
| Replay of a 10-minute capture | < 30 s | < 60 s | — | **Yes** — BTB-REF-4's acceptance criterion |

### Throughput
- **Sustained:** the full published entity-update rate of the reference scenario with zero internal-queue drops — **818 samples/s**, and the requirement is "all of it". Demonstrated at M3: 132 188 samples through a full load-run-teardown cycle with zero decoder drops and zero orphans.
- **Peak:** at least 3× the observed sustained rate — **≥ 2500 samples/s**, which is also the natural rate of the `Outback Kamikaze Swarm` overload case — absorbed for 10 s without drops, using the configured queue size.
- **Concurrency:** one simulator, one bridge. Two bridges against one simulator is a stretch goal, not a v1 target.

### Resource constraints
- Memory bounded by the internal queue size × record size, plus the roster and latest-sample map — all bounded by entity count. No unbounded growth over run length. A run of any duration must have a stated memory ceiling.
- Disk: capture size grows linearly with run length × entity count × update rate. The ceiling and end-of-space behaviour are BTB-CAP-6.
- CPU: one writer thread plus the client's pump. The bridge must not saturate a core at the reference rate.

### Optimization approach
- Batch writes behind a buffered stream; flush on segment boundaries, on the trailer, and on a bounded interval — never per record, and never so rarely that Ctrl-C loses a tail (BTB-SD-1 bounds this from the other side).
- Format floats once per value; avoid re-entering the formatter per field.
- Trade-off accepted: buffering improves throughput and widens the lost-tail window on an abnormal termination. BTB-SD-1 handles the normal case; a hard kill may lose up to one buffer, and the README states that plainly rather than implying durability the design does not provide.

## Observability

### Metrics

| Metric | What it measures | Alert threshold |
|--------|-----------------|-----------------|
| `internal_queue_drops` (per topic) | Samples lost at the handler→writer boundary | Any non-zero value is reported at exit; a rising value mid-run is a warning |
| `schema_hash_drops` | Messages dropped for schema mismatch | Any non-zero value — this is the silent-failure mode from [S1] |
| `decode_failures` | Messages that failed to decode | Any non-zero value |
| `missing_schema_passthrough` | Messages arriving with no registered schema | Any non-zero value at startup indicates a registry gap |
| `queue_depth_high_water` | Peak internal queue occupancy | > 80% of capacity indicates the queue is undersized for this scenario |
| `handler_duration_p95` | Time spent inside handlers | > 100 µs — BTB-BP-1 is being violated |
| `topic_silence_s` | Seconds since last decode on a subscribed topic, while the engine reports running | > configured interval (BTB-OBS-2) |

### Logging

| Event | When logged | Fields |
|-------|------------|--------|
| Startup configuration | Before first subscription | Engine config, model path, schema file, resolved topic, registry size, both backpressure settings |
| Attach / detach | On connect and on host loss | Simulation time, engine state, scenario name, `attached_mid_run` |
| Segment boundary | On `scenario_loaded` / `scenario_unloaded` | Segment ordinal, scenario name, simulation time, reason |
| Entity removal | On `entity_deleted` | Entity name, removal reason, simulation time |
| Drop threshold crossed | First drop, then rate-limited | Topic, cumulative count, queue depth |
| Verdict | On condition met, or at end of run for not-met | Condition id, entities, values, simulation time |
| Shutdown | On signal and on completion of drain | Records drained, trailer written, exit code |

Log lines may carry wall-clock time and a level; **capture records may not** (BTB-CAP-2). The two paths are deliberately separate.

### Alerts

There is no alerting infrastructure — this is a console program, and inventing one would be gate inflation. The equivalents:

| Condition | Surfacing | Severity | Response |
|-----------|-----------|----------|----------|
| Registry empty / topic has no schema | Startup error, non-zero exit | P1 | Check model path and schema file against the engine's (runbook row 1) |
| Topic silent while engine running | Warning line, repeated at interval | P1 | Same as above; this is the same fault seen later |
| Non-zero drops | Warning at first drop; totals in trailer and summary | P2 | Raise `--queue-size`, or accept and document the loss |
| Host lost | Warning, capture closed with `host_lost` | P2 | Expected during host-crash testing; a campaign continues (EXT-17's concern) |

### Health check
Not an HTTP service; no endpoint. The equivalent is the periodic status line: engine state, running flag, frame number, simulation time, scenario name, records written, queue depth, drop totals — every value obtainable from `SimulationEngineClient`'s local getters with no bus round trip.

## Cross-service impact

The two "services" here are two git repositories. That is the whole of the cross-service surface, and it is the reason several requirements above exist.

### Affected services

| Service | Impact | Changes required |
|---------|--------|-----------------|
| **EXT-08 (this repo)** | Everything | The program, the format spec, the evidence pack |
| **EXT-17 (separate repo)** | Consumes EXT-08's capture format and condition-file schema as versioned artifacts | Implements a reader against `docs/capture-format-v1.md`. **No EXT-08 source dependency, no shared headers, no shared build.** |
| **N8RO platform 2.1.328** | None. EXT-08 links published import libraries and subscribes to published topics | None. EXT-08 loads no plugin and modifies nothing in `C:\N8RO` |

### Interface changes
- **New artifact:** `n8ro-capture/1`, specified in `docs/capture-format-v1.md`. This is the contract.
- **New artifact:** the referee condition-file schema, documented in the README. EXT-17 may adopt it or supersede it, but it is the reference shape for "assertions declared outside the code."
- **No shared code.** If EXT-17 needs a behaviour from EXT-08, it is added to the spec or reimplemented — never linked.

### Deployment coordination
- EXT-08 ships first; it is EXT-17's stated prerequisite. Independent deployment is the norm — they are separate executables in separate repositories.
- **Version pinning:** EXT-17 pins the capture format version it understands and rejects others (BTB-CAP-5). That is what makes independent release safe.
- The freeze point is end of M7 (see §"Migration plan", step 2). Before it, format changes are cheap; after it, they cost a version bump and a downstream change.

### Testing implications
- **Contract test:** the sample reader must parse the committed sample capture. It runs in EXT-08's repository and is the standing check that spec and implementation agree.
- **Conformance test:** replay verdicts must equal live verdicts (BTB-REF-4). This proves the capture contains enough for a third party to reach the same conclusions.
- **Version-rejection test:** a capture with a bumped `format_version` must be rejected cleanly by the reader.

## Operational readiness

### Runbook

| Scenario | Detection | Response | Escalation |
|----------|-----------|----------|------------|
| Capture file is empty or missing entity samples | Startup log shows registry size 0, or `topic_silence_s` warning fires | Verify `--model-path` and `--schema-file` match what the engine loaded; a schema mismatch drops messages with a warning, not an error | Mentor, if the engine's own schema source is unclear |
| `schema_hash_drops` non-zero | Warning line; trailer counter | Engine and bridge have different schema versions for the same message. Re-point at the engine's schema file and re-run | Mentor — may indicate an install-tree inconsistency |
| Drops climbing during a run | Warning at first drop; `queue_depth_high_water` near capacity | Raise `--queue-size`; if drops persist, the writer is the bottleneck (H4 falsified) — profile the write path | — |
| Bridge appears hung at startup | No "attached" line; process alive | Expected while waiting for the simulator (BTB-CX-2). Confirm the retry line is being logged; Ctrl-C is honoured during the wait | — |
| Simulator exits mid-run | `host_lost` warning; capture closed with that `end_reason` | Expected and handled. Verify the trailer is present and the file parses | Mentor if the bridge did **not** detect it within the documented window |
| Bridge crashes, or the host AVs on teardown | Non-zero exit or a crash dialog | Capture the exit code and any dump; check whether the simulator process also terminated abnormally. See §"Risks" R1 | Mentor immediately — this is the carried platform risk and EXT-17's acceptance depends on it |
| Disk fills during a long run | `size_limit` trailer, or a write error | Confirm the capture closed cleanly; free space and re-run with a lower size cap | — |

### On-call impact
None. There is no on-call rotation and no production service. The operational reality is an engineer running a demo or a campaign, so the runbook above is written for the person at the keyboard, not for a pager.

### Deployment checklist
- [ ] Builds clean on a machine with only a stock 2.1.328 install plus `com.n8ro.dev`
- [ ] `setup.cmd` → `dev\setup-dev.cmd` documented and verified from a cold shell
- [ ] Both backpressure settings explicit at the call site, logged at startup, and in the README
- [ ] Format spec version string matches the `header` record's — checked by test
- [ ] Sample reader parses the committed sample capture
- [ ] Determinism harness green: 10 identical pairs
- [ ] Twenty Ctrl-C cycles, twenty valid trailers
- [ ] Host-loss path exercised deliberately at least once

### Capacity planning
Single desktop workstation. Memory bounded by queue size plus entity count. Disk is the only meaningful consumable: capture size grows linearly with run length × entity count × update rate — the M1 observation makes this a real number rather than a shape, and the README states bytes-per-minute for the reference scenario so a campaign author can size a disk for twenty runs.

### Dependencies and SLAs

| Dependency | SLA | Degraded behaviour | Owner |
|------------|-----|-------------------|-------|
| N8RO runtime 2.1.328 + `com.n8ro.dev` | None — a local install | Bridge cannot start; names the missing piece | Arkheon Technologies (external) |
| Visual Studio 2026 v18.x | None | Build only; no runtime impact | Local toolchain |
| The simulator process | None; may appear, disappear, or crash | BTB-CX-2 and BTB-CX-3 make all three ordinary states rather than errors | Whoever runs the simulator |

### Maintenance windows
None. Data retention is the user's: captures accumulate in `--out-dir` and are never deleted by the bridge. The README states that plainly — a tool that silently deletes evidence is worse than one that fills a disk.

## Dependencies and constraints

| Dependency | Owner | Status | Impact if delayed |
|-----------|-------|--------|-------------------|
| N8RO 2.1.328 install + `com.n8ro.dev` SDK component | Arkheon (external) | **Available and verified** | None |
| VS 2026 v18.x, C++17, Release\|x64 | Local | Available | Blocks all build work |
| A reference scenario that runs to completion with multiple entities | Mentor / scenario catalogue | **Not yet identified** | Blocks M1's baseline measurement and every acceptance demo. Identify in M1 |
| Mentor answer on the entity-picture layer (OQ-1) | Mentor | Open | Does not block v1; changes whether the built layer is permanent or transitional |
| Mentor answer on the `n8ro-sim-app.exe` invocation (OQ-2) | Mentor | Open | Blocks the controller stretch goal only. **Blocks EXT-17 outright** — flagged now for that reason |

**Constraints:**
- **C1** — Standalone C++17 console program. No plugin, no plugin ABI, no Qt, no scaffolder, no `.vcxproj` from `dev\samples\`. That tree is plugin-only and is not a starting point [S3].
- **C2** — Links exactly `n8ro-core.lib`, `n8ro-sim.lib`, `n8ro-schema.lib`, `n8ro-data.lib`; includes from `include\n8ro-{core,sim,schema,data}`.
- **C3** — Never throw. Return values plus logging, per the platform contract [S1].
- **C4** — Effort target 1–2 weeks [S1]. The entity-picture work (~2–3 days) is inside that budget and is the item most likely to breach it — see R3.
- **C5** — Files created inside `C:\N8RO` carry the Arkheon Technologies proprietary/confidential header [S5]. **This PRD's repository (`C:\Projects\EXT-08`) is ours and is not subject to that convention**; the rule applies only if work touches the install tree, which v1 does not.

## Risks and open decisions

| Risk | Impact | Likelihood | Mitigation |
|------|--------|------------|------------|
| **R1 — Host teardown reliability.** A `0xC0000005` host-side teardown access violation has been observed on this platform, with a `userPlugins/sim` plugin loaded. These projects load no plugins, so it may not apply | High for EXT-17: it requires 20+ unattended runs with clean teardown and demands surviving a mid-campaign host crash | Medium — unconfirmed in a plugin-free configuration | **Spike it during M7**, before EXT-17's acceptance criteria are locked: 20 consecutive load-run-teardown cycles with no plugins, exit codes recorded. Whatever the result, it goes in the notes and to the mentor. BTB-CX-3 already makes host loss a handled state rather than a crash |
| **R2 — Schema mismatch produces a plausible empty capture** | High — a silent wrong answer is the worst failure mode a recorder has | Medium — it is the most common configuration error [S1] | BTB-EP-1 (loud empty registry), BTB-OBS-1 (drop counters), BTB-OBS-2 (silent-topic warning). Three independent detections for one fault |
| **R3 — The entity-picture work overruns its 2–3 day budget** — **CLOSED at M3, did not materialise.** Delivered inside budget; H3 validated. Containment was not invoked and BTB-REF-3 keeps its full three-kind vocabulary | Medium — pushes past the 1–2 week target | Medium — it is unbudgeted in [S1] and therefore unvalidated as an estimate | Containment is BTB-CAP-4's verbatim rule: record what the schema declares, do not model it. If M3 exceeds three days, the referee's condition vocabulary (BTB-REF-3) drops to proximity-only and the rest moves to MVP+1 |
| **R4 — A determinism leak in our own emission path** | High — silently invalidates EXT-17's foundational self-test | Medium-high — three known sources exist (unordered map, float formatting, container iteration) and all three are easy to reintroduce | BTB-CAP-3 names all three; the M7 harness is the standing check, run on every change, not once |
| **R5 — Format churn after EXT-17 starts consuming it** | Medium — breaks a downstream repo | Medium | Freeze at end of M7; version-bump discipline in §"Migration plan"; the reader's version check makes a mismatch loud |
| **R6 — The reference scenario turns out to be unrepresentative** (too few entities, too short, no removals) — **CLOSED at M1.** `Atacama Air Defense`: 42 entities at load, two distinct removal reasons (`destroyed`, `expended`), a natural quiescent end at t ≈ 180 s. `Outback Kamikaze Swarm` (126 entities) is retained as the M6 overload case | Medium — acceptance demos prove less than they appear to | ~~Medium — no scenario has been chosen yet~~ | Choose it in M1 against explicit criteria: multiple entities, at least one removal, a natural end, and a duration that makes the rate measurable |

### Open questions

| # | Question | Status | Decision target | Rationale (why open / what would resolve it) |
|---|----------|--------|-----------------|----------------------------------------------|
| OQ-1 | Is a newer release expected to ship the `EntityStateSample` / entity-picture layer, or do we own it permanently? | **Resolved — we own it** (decided by the project owner, rev 3) | Closed | [S1] describes the layer as shipped; 2.1.328 does not have it, and [S1] is silent on whether it is coming — checked directly, it specifies the layer's *contents* but says nothing about ownership or roadmap. Decided in favour of owning it, on the reasoning that the layer is cheap to change or delete either way: EXT-17 binds to the capture file, not to EXT-08 source, so nothing downstream constrains its internal shape. Owning it is therefore the robust choice — if a later release ships the type, deleting ours costs a day; being under-built while permanent costs every milestone after M3. **Consequence:** the layer gets tests, documented invariants and a misuse-resistant API (ADR-1, rev 3) |
| OQ-2 | What is the exact `n8ro-sim-app.exe` headless invocation? | Needs input | Before the controller stretch goal; **before EXT-17 starts** | [S2] itself says "confirm the invocation with your mentor." EXT-17 depends on it outright. Raised here because EXT-08's controller stretch goal touches the same surface, and because raising it now buys EXT-17 lead time it would otherwise lose |
| OQ-3 | What is the entity-state topic string, and what fields does its schema actually declare? | **Resolved (M1, corrected at M3)** | M1 (observe the bus) | `sim/entity/state`, message `simEntityStateUpdate`, **twelve** declared fields — eleven of which are ever published. Recorded in the notes deliverable, not restated here as a constant: the code still resolves both at runtime (BTB-EP-1), and this PRD deliberately does not become the second copy that drifts |
| OQ-4 | Which bus-side backpressure policy is correct for a recorder — `FIFO_DROP` with a large queue, or `BLOCK`? | Provisional | M6 (backpressure) | `KEEP_LATEST` is ruled out: it discards the older of two samples, which is the one already part of the run's history. Between the remaining two, `BLOCK` risks perturbing the observed simulation and `FIFO_DROP` risks losing data. Provisional answer: `FIFO_DROP` with a queue sized from the M1 rate, because a recorder that changes the run it records is worse than one that admits a gap. Confirm under the M6 overload. **M1/M3 sizing input:** 100 messages is ~120 ms of headroom at the reference scenario's 818 packets/s and ~40 ms at the 126-entity overload scenario's 2487/s. M3 ran the reference scenario on the `KEEP_LATEST` default and lost nothing — a measurement of headroom at this load, not a reason to keep the default |
| OQ-5 | What float formatting guarantees round-trip-exact, locale-independent output on this toolchain? | **Resolved (M1, by test)** | M5 (output path) | `std::to_chars` shortest round-trip. The `printf` family is **disqualified**: `%.17g` is round-trip exact but silently locale-dependent, emitting `0,05` under a comma-decimal locale — which this machine has. Probe and corpus at `tests/float-format/`. Note for M5: BTB-CAP-3 says "17 significant digits", which is a *means* to round-trip exactness; shortest round-trip reaches the same end, and the spec text should say "round-trip exact" if shortest is adopted |
| OQ-6 | Should the referee's condition-file schema be designed for EXT-17 to adopt directly, or purely for EXT-08's needs? | Provisional | M6 | Designing for a consumer that does not exist yet risks speculative generality. Provisional answer: design for EXT-08, document it fully, and let EXT-17 adopt or supersede it — an over-designed schema is harder to supersede than a simple documented one |

### Rabbit holes

- **Modelling the entity state instead of recording it.** The tempting move is a typed `EntitySample` struct with named members. It looks cleaner and it is a trap: every schema change silently drops a field, and the "what the stream contained that we did not expect" deliverable becomes unwritable because the unexpected was filtered out before anyone saw it. **Containment:** BTB-CAP-4's verbatim rule is non-negotiable. Timebox any curated-struct exploration to zero.
- **Float formatting and locale.** Looks like a one-line `printf`. It is the difference between EXT-17's self-test working and not. **Containment:** OQ-5 with a test, resolved at M5 — before there is a capture anyone depends on.
- **Referee condition expressiveness.** The third condition kind invites a fourth, then a boolean combinator, then a parser. **Containment:** the vocabulary is closed at three kinds by BTB-REF-3 and by the Out-of-Scope entry. A fourth kind requires a PRD revision, which is the point.
- **Scenario reload timing.** Whether a `scenario_unloaded` reliably precedes the last sample of the outgoing run is an empirical question, and getting it wrong mixes two runs — the exact failure BTB-CX-4 exists to prevent. **Containment:** test reload explicitly at M5; if the ordering is not guaranteed, the segment boundary keys on the load event and the spec says so.
- **"Is the run finished?"** [S2] warns that this is harder than it sounds. EXT-08 does not have to answer it — it follows a scenario rather than deciding when one ends. **Containment:** resist implementing end-detection here. It is EXT-17's problem, and solving it early means solving it twice.
- **The controller stretch goal.** It touches OQ-2, adds the write direction, and duplicates EXT-17's execution piece. **Containment:** it is Out of Scope, after M7, budget permitting — and it stays there unless the mentor asks otherwise.

## Alternatives considered

### Option 1: Build the entity picture; capture as schema-headed JSON Lines (selected)
Build the roster and latest-sample map on top of `MessageBusPacked`'s decoded subscription, and write a JSON Lines capture whose first record embeds the live `MessageSchema` for every message type recorded.

**Pros:**
- The file is self-describing: EXT-17 reads it with the spec alone, satisfying the repo-boundary constraint without shared source.
- Text is inspectable — during M1–M3, when nobody yet knows the stream's shape, being able to `head` the capture is worth real money.
- Schema-ordered fields give determinism for free, from a source the runtime already hands us.
- New platform fields appear automatically (BTB-CAP-4).

**Cons / trade-offs accepted:**
- Larger on disk than binary — several times over. Accepted because inspectability during the learning phase outweighs disk, and BTB-CAP-6 bounds the damage. Compression is a documented v1.1 deferral.
- JSON number formatting must be handled carefully or determinism breaks (OQ-5). Accepted because the same care is required by any text format, and the alternative moves the problem rather than removing it.

### Option 2: Log raw packed messages; decode offline
Write the undecoded bus payloads and build a separate decoder that runs over the log.

**Pros:**
- Minimal work in the handler; smallest possible files; captures bytes exactly as they arrived.

**Cons:**
- The capture is unreadable without a decoder **and** the matching schema registry — so the cross-repo artifact becomes "a file plus a program plus a database," which is precisely the source coupling the repo split forbids.
- The `MessageSchema` is available at capture time and thrown away, then reconstructed later. That is strictly more work for strictly less capability.

**Why not chosen:** It optimises the wrong side of the boundary. The decisive factor is EXT-17: a capture EXT-17 cannot read alone is not a deliverable.

### Option 3: SQLite capture database
Write records into a SQLite file with a schema table and a samples table.

**Pros:**
- Queryable without writing a reader; indexed lookups; a natural fit for the referee's replay mode.

**Cons:**
- Byte-for-byte comparison becomes unreliable — page layout, free lists, and write ordering can differ between runs that produced identical logical content. Making two SQLite files byte-identical is a project in itself.
- Adds a dependency to a program whose whole appeal is that it links four import libraries and nothing else.

**Why not chosen:** The decisive factor is R4/H2. EXT-17's determinism self-test compares captures byte-for-byte; a container that does not guarantee that property under identical input is disqualified regardless of its other merits. Logical-equivalence diffing would work, but it puts the burden on every consumer forever.

### Option 4: Wait for the SDK to ship `EntityStateSample`
Defer EXT-08 until a release provides the entity picture the brief describes.

**Pros:**
- Saves 2–3 days; the resulting code would use a supported API rather than one we maintain.

**Cons:**
- No commitment exists that such a release is coming — that is OQ-1, and it is open.
- EXT-17 is blocked for the entire wait.
- The work would still need doing if the answer is "you own it permanently."

**Why not chosen:** It trades a known 2–3 days for an unbounded wait on an unanswered question, while blocking the downstream project. Ask OQ-1 anyway — the answer changes how much the layer should be abstracted, not whether to build it.

### Option 5: Do nothing / status quo
Continue observing runs through the GUI.

**Why not acceptable:** EXT-17 cannot start — it is a stated prerequisite. Every question about a past run costs a re-run. There is no artifact anyone can point at as evidence. And the shape of the bus stream, which every future third-party integration must learn, stays undocumented folklore that each new integrator rediscovers.

## Validation and test plan

- **Unit — the entity picture:** `tests/entity-picture/` covers occupancy lifecycle, orphan counting, verbatim reasons and payloads, absent-field accounting, deterministic ordering, the bounded event log, and concurrent handler/snapshot traffic. Needs no simulator, no bus and no model database, so it runs on any checkout in seconds. Its own adequacy is checked by mutation — deliberate defects introduced into the picture must fail it.
- **Unit — determinism primitives:** float formatting is round-trip exact and locale-independent; field ordering follows `MessageSchema::fields` and not map iteration; record serialisation is stable for fixed input. These are the three known non-determinism sources (R4), tested directly rather than only end-to-end.
- **Unit — format:** every record type round-trips through the reader; an unknown `format_version` is rejected with a named error; the spec's version string equals the `header`'s.
- **Unit — condition evaluation:** each of the three condition kinds against synthetic sample sequences, including the boundary case (exactly at the threshold) and the never-met case.
- **Integration — lifecycle:** start-before-simulator; attach mid-run; load; reload producing two segments; entity removal with each reason the reference scenario produces; simulator killed mid-run.
- **Integration — occupancy (ADR-6):** a full load-run-teardown cycle asserts that no `sample` follows its occupancy's `entity_remove`, that a name re-created by the engine's stop-path burst opens the next ordinal, and that the samples-with-no-open-occupancy count is zero. The mid-run kill-and-recreate case (`destroyed`, then re-created at teardown) is the one that distinguishes this from a segment-scoped check, so it is asserted by name rather than by aggregate count.
- **Integration — backpressure:** deliberate overload with a throttled writer; verify drops are counted accurately, per-topic, and land in the trailer. This is also the recorded demonstration for BTB-BP-4.
- **Integration — replay conformance:** live verdicts equal replay verdicts over the same run and condition file, byte for byte (BTB-REF-4).
- **Integration — contract:** the sample reader parses the committed sample capture. This is the standing spec-versus-implementation check.
- **System — determinism harness:** ten identical-configuration pairs, hashed and compared. Run on every change touching the capture path, not once at the end (H2, [S2]).
- **System — shutdown:** twenty scripted interrupt-and-verify cycles; twenty valid trailers; exit code 0 each time.
- **System — teardown spike (R1):** twenty consecutive plugin-free load-run-teardown cycles, exit codes recorded, result written into the notes deliverable and taken to the mentor whichever way it goes.
- **CI:** none configured — this is a workstation project against a local install. The determinism harness and the shutdown loop are scripted so they run as one command, which is what makes running them habitually realistic.

## Rollback strategy

The rollback surface is not a deployment — it is the **format contract**, because that is the only thing another project depends on.

### Trigger conditions
- The determinism harness fails and the cause is EXT-08's emission path (R4).
- The format spec and the implementation disagree — the contract test fails.
- A format change lands that EXT-17's reader cannot parse.
- The teardown spike (R1) shows the bridge implicated in host instability.

### Rollback steps
1. **Stop producing captures with the suspect build.** Immediate — a wrong capture is worse than none, and captures accumulate silently. (< 1 min)
2. **Identify the last build whose determinism harness passed**, via the tagged commit that harness runs against. (< 10 min)
3. **Revert to that tag and re-run the harness plus the contract test** to confirm the baseline is genuinely good. (< 30 min)
4. **Quarantine captures produced by the suspect build** — move them aside rather than deleting them; a non-deterministic capture is evidence for the investigation.
5. **If the format itself is at fault, do not patch in place.** Bump to the next version and leave v1 files valid under v1. Silently changing what a version means is the one unrecoverable move.

### Data rollback
Captures are immutable once closed and are never rewritten. "Rolling back" a capture means marking it quarantined, not editing it. There is no in-place migration of a stored capture in v1 — a reader either understands its version or rejects it.

### Partial rollback
The referee and the recorder are independently disableable: a run can capture without conditions (omit `--conditions`), and replay mode exercises the referee with no bus at all. A fault in one does not force reverting the other.

### Communication
Any rollback that touches the format after the M7 freeze is communicated to the mentor and the EXT-17 author before the next capture is produced, because their reader is pinned to a version. Before the freeze, format changes need no announcement — which is precisely why the freeze point is a milestone gate rather than a date.

## Rollout and milestones

Milestone order follows [S1]'s prescribed step order deliberately: observe, then minimal client, then subscribe, then output, then lifecycle, then backpressure, then shutdown. Effort target 1–2 weeks total [S1].

### M1 — Watch the traffic (0.5 day) — **delivered**
Run the simulator; observe what the bus actually carries. Identify the reference scenario against R6's criteria. Record the entity-state topic, its schema fields, the update rate, and the entity count.
**Validation:** OQ-3 answered from observation, not memory. Throughput baselines for §"Performance requirements" exist as numbers. Reference scenario chosen and justified. First entries written into the notes deliverable.

### M2 — Smallest possible client (0.5 day) — **delivered**
`create()`, start the pump, print engine state once a second. Nothing else. [S1] is explicit that most of the difficulty is configuration, not logic — this milestone exists to hit that wall alone.
**Validation:** BTB-CX-1. Engine state, frame number, simulation time, and scenario name print correctly from the local getters, with no bus round trip.

### M3 — The entity picture (2–3 days) — *the item [S1] assumed was free* — **delivered, inside budget**
Register schemas; subscribe decoded; build the roster from `sim/entity/event`; build the latest-sample map with ordered containers.
**Validation:** BTB-EP-1 through BTB-EP-4. Roster fills and empties correctly across a full scenario. Registry size and resolved topic logged. **Gate: if this exceeds three days, invoke R3's containment before proceeding.**
**Result:** met. Both topics resolved from the registry with no topic literal in the codebase; all three BTB-EP-1 failure modes exercised on distinct exit codes. Reference run: 42 entities at load, 90 distinct names, 132 occupancies, removals `destroyed:23 expended:48 scenario_unload:19`, 132 188 samples, **0 drops, 0 orphans**. Two findings changed this document — the twelfth schema field (§"Prior art"), and BTB-EP-3's unsatisfiable criterion (ADR-6). R3 closed; the gate was not reached.

### M4 — Capture format and the spec (1 day)
Design and document `n8ro-capture/1`; write the header with its embedded schema envelope; emit `sample` records; write `docs/capture-format-v1.md` alongside the code, not after it.
**Validation:** BTB-CAP-1, BTB-CAP-4, BTB-CAP-5. A capture from a real run parses. The spec is complete enough to hand to someone who has not seen the code.

### M5 — Output path and lifecycle (1.5 days)
The writer thread; the handler-to-writer handoff; segment boundaries on scenario load and reload; entity-removal records; host-loss handling; float formatting resolved (OQ-5).
**Validation:** BTB-CX-3, BTB-CX-4, BTB-BP-1, BTB-BP-2, BTB-CAP-2, BTB-CAP-3. A reload produces two segments. Killing the simulator produces a `host_lost` trailer. No wall-clock value appears anywhere in the capture path.

### M6 — Referee and backpressure (2 days)
Condition file parsing; the three condition kinds; live verdicts; replay mode; both backpressure boundaries set explicitly; the overload demonstration.
**Validation:** BTB-REF-1 through BTB-REF-4, BTB-BP-3, BTB-BP-4, BTB-OBS-1. Replay verdicts equal live verdicts. Overload produces accurate per-topic drop counts. OQ-4 resolved under real load.

### M7 — Shutdown, determinism, evidence (1.5 days)
Signal handling and drain; the determinism harness; the twenty-cycle shutdown loop; the R1 teardown spike; README, sample capture, reader, recording, notes.
**Validation:** BTB-SD-1, BTB-DOC-1, BTB-DOC-2, and every success metric. **Format freeze at the end of this milestone** (§"Migration plan" step 2). R1 spike result documented and taken to the mentor whichever way it goes.

**Total: 9–10 working days**, inside [S1]'s 1–2 week target. The entity picture (M3) is the largest single item and the one most likely to breach it — flagged per R3, with containment defined rather than hoped for.

## Review checklist

- [x] All requirements have acceptance criteria
- [x] All P1 FRs have **Customer scenario** + **Pain removed** fields populated
- [x] All P1 FRs have a corresponding `UAC-{FR-ID}` entry in Appendix B
- [x] Naming and interface conventions subsection present at top of FR section (adapted: CLI, file, record-vocabulary and platform-string conventions in place of REST/SDK paths)
- [x] Scope Authority subsection present
- [x] Out of Scope section present with structured entries (status / rationale / target / date)
- [x] Open Questions table has decision target + rationale on every entry; no owner column
- [x] Security implications assessed
- [x] Cross-repo dependencies documented
- [x] Migration plan reviewed (format-version migration)
- [x] Test plan covers all P1 requirements
- [x] Rollback strategy defined
- [x] Key hypotheses are falsifiable, each with a stated consequence if false
- [x] Success metrics have baselines

## Appendix A: Traceability

| Requirement | Source | Goal served |
|-------------|--------|-------------|
| BTB-CX-1 | [S1] step 2, deliverables | G1 |
| BTB-CX-2 | [S1] acceptance criterion 1 | G1 |
| BTB-CX-3 | [S1] acceptance criterion 6; [S2] host-crash survival | G1 |
| BTB-CX-4 | [S1] acceptance criterion 4; [S2] constraint 3 | G1, G4 |
| BTB-EP-1 | [S1] packed-schema rule; [S3] entity-picture correction | G1, G5 |
| BTB-EP-2 | [S3]; [S4] `MessageBusPacked.h` | G1 |
| BTB-EP-3 | [S1] acceptance criterion 5; [S2] constraint 4; [S4] `IEntityManager.h` | G1 |
| BTB-EP-4 | [S3] entity-picture work item; [S2] determinism | G1, G3, G4 |
| BTB-CAP-1 | [S2] constraint 1; [S4] `MessageSchema.h` | G2 |
| BTB-CAP-2 | [S1] time rule; [S2] constraint 2; [S3] corollary | G1, G4 |
| BTB-CAP-3 | [S2] constraint 2; [S4] `StreamValue.h` | G4 |
| BTB-CAP-4 | [S1] deliverable "field by field"; [S1] notes deliverable | G1, G5 |
| BTB-CAP-5 | [S2] constraint 1; [S3] repo-boundary rule | G2 |
| BTB-CAP-6 | Operational (disk exhaustion) | G1 |
| BTB-BP-1 | [S1] rule 1 | G1 |
| BTB-BP-2 | [S1] rule 2 | G1, G4 |
| BTB-BP-3 | [S1] rule 6; [S4] `IMessageBus.h` defaults | G1 |
| BTB-BP-4 | [S1] rule 6, acceptance criterion 7 | G1 |
| BTB-REF-1 | [S1] Referee shape; [S2] assertion separation | G3 |
| BTB-REF-2 | [S2] "names what was checked and on what data" | G3 |
| BTB-REF-3 | [S1] Referee examples | G3 |
| BTB-REF-4 | [S2] "re-judge stored runs without re-running" | G2, G3 |
| BTB-OBS-1 | [S1] schema-mismatch rule; [S4] `metricsSnapshot()` | G1, G5 |
| BTB-OBS-2 | [S1] "a silent topic is the first thing to check" | G5 |
| BTB-SD-1 | [S1] acceptance criterion 8 | G1 |
| BTB-DOC-1 | [S1] deliverables | G2, G5 |
| BTB-DOC-2 | [S1] deliverables | G5 |

## Appendix B: User acceptance criteria

### UAC-BTB-CX-1: Client bring-up from explicit configuration
**GIVEN** a machine with a stock 2.1.328 install and no bridge configuration compiled in
**WHEN** the bridge is run with an incorrect `--schema-file`
**THEN** it logs each resolved configuration value, names the failure, exits non-zero, and no exception escapes `main`

### UAC-BTB-CX-2: No required start order
**GIVEN** no simulator is running
**WHEN** the bridge is started, and the simulator is started 30 seconds later
**THEN** the bridge connects without intervention and captures the scenario from its beginning

### UAC-BTB-CX-3: Simulator exit does not crash or hang the bridge
**GIVEN** the bridge is attached and capturing
**WHEN** the simulator process is killed
**THEN** the bridge detects the loss within the documented window, writes a `trailer` with `end_reason: host_lost`, closes the file, and exits without crashing or hanging

### UAC-BTB-CX-4: Scenario reload is unambiguous
**GIVEN** the bridge is capturing a loaded scenario
**WHEN** the scenario is unloaded and the same scenario is loaded again
**THEN** the capture contains exactly two `segment_open`/`segment_close` pairs with distinct increasing ordinals, and no `sample` record lies outside an open segment

### UAC-BTB-EP-1: Loud empty registry
**GIVEN** a `--model-path` whose schemas do not match the running engine's
**WHEN** the bridge starts
**THEN** it logs the registry size and the resolved topic, names the mismatch, and exits non-zero rather than capturing an empty file

### UAC-BTB-EP-2: Decoded subscription
**GIVEN** a registered schema for the entity-state topic
**WHEN** an entity update is published
**THEN** the handler receives the decoded `StreamValueMap` and its `MessageSchema`, and no manual payload parsing exists anywhere in the codebase

### UAC-BTB-EP-3: Roster lifecycle
**GIVEN** an entity present in the roster at occupancy *n*
**WHEN** it is destroyed and `sim/entity/event` reports `entity_deleted` with reason `destroyed`
**THEN** exactly one `entity_remove` record carries `destroyed` verbatim at occupancy *n*, and no later `sample` record names that entity **at occupancy *n***

### UAC-BTB-EP-3b: A re-created name is a new occupancy
**GIVEN** an entity that has been removed — by `destroyed` mid-run, or by the `scenario_unload` burst the engine's stop path publishes
**WHEN** the engine re-creates it under the same scenario entity name
**THEN** an `entity_add` opens occupancy *n+1*, every subsequent `sample` for that name carries *n+1*, and the count of samples attributed to an entity with no open occupancy is zero

### UAC-BTB-EP-4: Deterministic latest-sample map
**GIVEN** a scenario with multiple entities publishing concurrently
**WHEN** the referee takes a roster snapshot
**THEN** the snapshot is internally consistent, each entry carries its last published simulation time, and no unordered container is iterated in the capture or verdict path

### UAC-BTB-CAP-1: Self-describing header
**GIVEN** a capture produced by a real run
**WHEN** a reader with no access to EXT-08 source opens it
**THEN** the first record names and types every field of every later `sample` record, and `format_version` is its first key

### UAC-BTB-CAP-2: Simulation time only
**GIVEN** a completed capture
**WHEN** every record is inspected for time values
**THEN** every stamp is the published simulation time carried by its cause, no wall-clock-derived value appears anywhere in the file, and the README states why no record can be a prediction in release 2.1.328

### UAC-BTB-CAP-3: Byte-for-byte reproducibility
**GIVEN** the same scenario and the same configuration
**WHEN** the bridge captures the run twice
**THEN** the two capture files hash identically, and this holds for ten consecutive pairs

### UAC-BTB-CAP-4: Verbatim schema-driven records
**GIVEN** a message schema in the database gains a new field
**WHEN** a capture is taken with no code change
**THEN** the new field appears in `sample` records with its published value and unit, in schema order

### UAC-BTB-CAP-5: Versioned format specification
**GIVEN** only `docs/capture-format-v1.md`
**WHEN** an engineer writes a reader
**THEN** the reader parses a real capture, and rejects a capture bearing an unknown `format_version` with a named error rather than parsing it partially

### UAC-BTB-CAP-6: Bounded capture size
**GIVEN** a configured maximum capture size
**WHEN** a long run reaches it
**THEN** the capture closes with a well-formed `trailer` carrying `end_reason: size_limit`, and no line is truncated mid-write

### UAC-BTB-BP-1: The handler does no work
**GIVEN** the bridge is attached at the reference scenario's full rate
**WHEN** handler execution time is measured
**THEN** p95 is under 100 µs, and no file, socket, or formatting call exists inside any handler

### UAC-BTB-BP-2: FIFO per topic
**GIVEN** a burst of entity updates on one topic
**WHEN** the capture is read back
**THEN** records appear in arrival order, verified against `sequenceNumber`, and any gap or reordering is counted and reported

### UAC-BTB-BP-3: Explicit bus-side policy
**GIVEN** the bridge's source
**WHEN** any subscription call site is inspected
**THEN** `backpressurePolicy` and `queueSize` are passed explicitly with a comment naming the overridden default, the values are logged at startup and written into the `header`, and the README explains why `KEEP_LATEST` is unsuitable for a recorder

### UAC-BTB-BP-4: Bounded internal queue with counted overflow
**GIVEN** a deliberately throttled writer
**WHEN** the simulation publishes faster than the writer drains
**THEN** the configured policy applies, the `trailer` carries an accurate per-topic drop count, and the behaviour is shown in the demo recording

### UAC-BTB-REF-1: Conditions declared outside the code
**GIVEN** a running build
**WHEN** a new condition is added to the condition file
**THEN** it is evaluated on the next run with no rebuild; and a malformed file produces a named parse error and a non-zero exit before any subscription

### UAC-BTB-REF-2: Verdicts locate their evidence
**GIVEN** a condition that is met mid-run
**WHEN** its verdict is read
**THEN** the record names the condition id, the entities, the deciding values, and the simulation time — enough to find the causing samples in the capture; and a condition never met yields an explicit not-met verdict at end of run

### UAC-BTB-REF-3: Closed condition vocabulary
**GIVEN** a condition file
**WHEN** it declares a proximity, area, or terminal-state condition
**THEN** each is evaluated per its documented parameters, units, and boundary semantics; and any fourth kind is a named parse error, not a silent skip

### UAC-BTB-REF-4: Offline re-judgement
**GIVEN** a stored capture and a condition file written after the run ended
**WHEN** the bridge is run with `--replay`
**THEN** it produces verdicts identical to a live run's over the same records, with no simulator and no bus, in under 60 seconds for a 10-minute capture

### UAC-BTB-OBS-1: Decode diagnostics surfaced
**GIVEN** a schema mismatch between bridge and engine
**WHEN** the bridge runs
**THEN** `schemaHashDrops` is non-zero, a warning names the mismatch and the two things to check, and all five counters appear in the `trailer` and the exit summary

### UAC-BTB-OBS-2: Silent-topic detection
**GIVEN** the engine reports itself running
**WHEN** a subscribed topic produces no decoded message for the configured interval
**THEN** a warning names the topic and the schema-mismatch hypothesis; and no such warning fires while the simulation is paused

### UAC-BTB-SD-1: Clean Ctrl-C shutdown
**GIVEN** a capture in progress with records queued
**WHEN** Ctrl-C is pressed
**THEN** the bridge unsubscribes, stops the pump, drains the queue, writes the `trailer`, flushes, closes, and exits 0 — with every record enqueued before the signal present in the file, across twenty consecutive cycles

### UAC-BTB-DOC-1: README and format specification
**GIVEN** a clean machine with a stock 2.1.328 install and `com.n8ro.dev`
**WHEN** an engineer follows the README
**THEN** they build and run the bridge within an hour, and every record type and field is documented with type, unit, and meaning

### UAC-BTB-DOC-2: Evidence pack
**GIVEN** the repository at the end of M7
**WHEN** the mentor reviews it
**THEN** it contains a real sample capture, a reader written from the spec alone, a 5-minute recording showing start-before-simulator, reload, removal, a verdict, the backpressure demonstration and a clean Ctrl-C, and a notes page recording what the stream contained that was not expected

## Appendix C: Architecture decision records

> These ADR stubs capture the major decisions in this PRD. Expand them into full ADRs in the repository's decision log as implementation proceeds.

### ADR-1: The entity picture is ours to build, and ours to own
**Status:** Accepted (built at M3; ownership decided at rev 3)
**Context:** [S1] describes a roster and per-entity latest-sample cache as provided by `SimulationEngineClient`. Release 2.1.328 ships neither; `EntityStateSample.h` does not exist and the library exports no such symbols. [S1] also lists an `acceleration` field on the entity picture that the runtime schema does not declare at all, and describes a published-versus-predicted choice this release cannot offer — so it is not merely early, it is describing a different API. Checked directly at rev 3: [S1] says nothing about whether the layer is coming, so it does not settle the question.
**Decision:** Build the entity picture in EXT-08 on top of `MessageBusPacked`'s decoded subscription and `sim/entity/event` — and, from rev 3, **treat it as a permanent component rather than a shim.**
**What "permanent" buys, and what it deliberately does not:**
- **Tests, and they run without a simulator.** `tests/entity-picture/` drives the picture by handing it `StreamValueMap`s, which is exactly what `DecodedHandler` does — so the tests exercise the real entry points rather than a stand-in. Verified by mutation: five deliberate defects introduced into the picture, five caught.
- **Documented invariants**, chiefly the retention rule — the latest-sample map keeps a *closed* occupancy's final sample, so "where was it when it died" stays answerable, and drops it when a new occupancy opens under the same name.
- **A misuse-resistant snapshot API.** `liveSample()` returns nothing for a removed entity, so the safe reading is the easy one; `lastKnownSample()` exists for the caller who genuinely wants a dead entity's final state, and is named so that reaching for it is a choice.
- **No interface, deliberately.** A pure-virtual seam would buy substitutability we have no second implementation for. The seam that matters already exists and is cheaper: the picture consumes a decoded `StreamValueMap`, so M6's replay path feeds the same class from a stored capture instead of from the bus. Adding indirection for a hypothetical would be the speculative generality OQ-6 warns about elsewhere.
**Consequences:**
- The bridge is not blocked on an SDK release that may never come.
- We own the layer's correctness and its maintenance across platform upgrades — which the tests now make tractable rather than aspirational.
- If a later release ships the layer, this becomes a shim to delete. That stays cheap: EXT-17 binds to the capture file, not to EXT-08 source, so the layer's internal shape constrains nobody downstream and can change at any time — before or after the M7 format freeze.
- It stays schema-driven and verbatim regardless (BTB-CAP-4). Ownership is a reason to test the layer, never a reason to start modelling the payload.

**Supersedes:** None

### ADR-2: Schema-headed JSON Lines as the capture container
**Status:** Proposed
**Context:** The capture must cross a repo boundary as a versioned, self-describing, byte-comparable artifact, and must be inspectable while the stream's shape is still being learned.
**Decision:** JSON Lines, UTF-8, LF-terminated, with a first `header` record embedding the runtime `MessageSchema` for every recorded message type.
**Consequences:**
- EXT-17 reads captures with the spec alone; no shared source (H1).
- Byte-for-byte comparison is achievable with disciplined field ordering and float formatting.
- Larger files than a binary container, and JSON number formatting becomes a determinism concern (OQ-5). Compression deferred to v1.1.

**Supersedes:** None

### ADR-3: Simulation time is the only clock in the capture
**Status:** Proposed
**Context:** [S2]'s determinism self-test compares captures byte-for-byte; a single wall-clock value anywhere in the file defeats it.
**Decision:** Capture records carry only the simulation time their cause carried. Wall-clock time appears in log lines and nowhere else. Run labels default to ordinals, never timestamps.
**Consequences:**
- Identical runs produce identical files (G4).
- Correlating a capture with an external wall-clock event requires the log alongside it — accepted, and stated in the README.

**Supersedes:** None

### ADR-4: Both backpressure boundaries are explicit; `KEEP_LATEST` is rejected
**Status:** Proposed
**Context:** `SubscriptionOptions` defaults to `KEEP_LATEST` with `queueSize = 100`. For a recorder this discards the older of two samples — the one already part of the run's history. A second boundary exists at the handler-to-writer queue.
**Decision:** Set both explicitly. Provisionally `FIFO_DROP` with a queue sized from the M1-observed rate at the bus boundary (OQ-4, confirmed under M6 load), and a bounded internal queue with counted drops recorded in the `trailer`.
**Consequences:**
- Loss is bounded and counted rather than silent (tenet 3).
- `BLOCK` is rejected at the bus boundary because a recorder that stalls the bus changes the run it is recording.
- The chosen values are part of the documented contract and appear in the capture header.

**Supersedes:** None

### ADR-5: The referee runs both live and offline
**Status:** Proposed
**Context:** [S2] requires stored runs to be re-judgeable against new assertions without re-running them, across a repo boundary.
**Decision:** One evaluation engine, two sources: the live entity picture, or a stored capture via `--replay`. Verdicts from the two paths must be identical.
**Consequences:**
- The replay path is the strongest available conformance test for the capture format — if the referee can re-derive its own verdicts from the file, the file demonstrably contains enough.
- EXT-17 inherits a working model for offline assertion rather than starting from zero.
- Requires the reader to exist inside EXT-08, which also makes the contract test possible.

**Supersedes:** None

### ADR-6: An entity's identity is (name, occupancy), not name
**Status:** Accepted (M3)
**Context:** BTB-EP-3 originally required that "no `sample` record for that entity appears after its `entity_remove`". M1 established, and M3 confirmed and quantified, that this cannot hold: the engine's stop path deletes every live entity with `reason="scenario_unload"` and then immediately re-creates the entire roster under the same names, so samples resume under a removed name. The reference run also showed the sharper case — `RedUAV_N_01` was `destroyed` at t=149.45 and re-created at teardown. A scenario entity name is unique *within a tenure*, not across a run. Left unresolved, the criterion is either dead or forces the reader of a capture to treat a legal file as corrupt.
**Decision:** Identity is the pair (scenario entity name, **occupancy**). The occupancy is a per-name generation counter, from 1, opened by `entity_created` and closed by `entity_deleted`. A sample belongs to whichever occupancy is open when it arrives. A sample for a name with no open occupancy is counted as a named diagnostic and discarded rather than attributed to a closed tenure. The ordinal is carried in `entity_add`, `entity_remove` and `sample` records so the distinction survives into the capture.
**Alternatives rejected:**
- *Scope the criterion to a scenario segment.* Truer to BTB-CX-4's vocabulary, but it makes an M3 requirement depend on segment machinery that belongs to M5, and it is weaker — it says nothing about a mid-run kill-and-respawn within one segment.
- *Exempt `scenario_unload` as teardown rather than removal.* Simplest, and wrong: it puts a reason **string literal** in the roster's control flow, which sits badly against the same requirement's "reason preserved verbatim", and it would not have caught the `destroyed`-then-recreated case at all.
- *Declare the criterion unsatisfiable and drop it.* Discards a real invariant. The criterion was reaching for something true and worth enforcing — a sample must never be attributed to a dead entity — and occupancy scoping states exactly that.
**Consequences:**
- The criterion becomes exactly satisfiable and machine-checkable, in the capture as well as in memory. Verified at M3: zero orphaned samples across 19 `scenario_unload` removals and 42 re-creations.
- The counter of samples-with-no-open-occupancy becomes a first-class loss metric alongside the bus drop counters, and it earned its keep immediately — it is what made a bridge that attached after scenario load diagnosable (7 740 orphans, zero drops, no error) rather than merely wrong.
- Three record types gain a field, and EXT-17 must key on the pair rather than the name. Pre-freeze, so no format version bump (§"Revision history").
- Segment ordinals (BTB-CX-4) and occupancy ordinals are independent and both appear on a `sample`. M5 must not conflate them.

**Supersedes:** None

## Quality gate notes

Advisory only — these do not block the PRD.

**Notable gaps**

- ~~**Performance baselines are placeholders by design.**~~ **Closed in rev 2.** M1 measured the stream; §"Performance requirements" now carries observed rates, entity counts and payload bandwidth for both the reference and the overload scenario. The p95 handler target has not been measured directly and remains an unvalidated target — *suggestion: instrument it at M5, when the handler finally has a writer to hand off to and the number means something.*
- ~~**The reference scenario is unidentified.**~~ **Closed in rev 2.** `Atacama Air Defense`, chosen at M1 against R6's four criteria and justified in the notes deliverable. `Outback Kamikaze Swarm` is retained as the M6 overload case.
- **UAC-BTB-CAP-4 is not fully self-verifying.** It requires adding a field to a message schema in the database, which means touching `C:\N8RO`'s data tree. *Suggestion: confirm with the mentor whether a scratch model database is available for this test; if not, downgrade the criterion to a code-inspection check that no field allowlist exists.*

**Minor gaps**

- **OQ-4 and OQ-6 are Provisional with reasoned defaults rather than Open.** That is the intended use of the status, but per the OQ lifecycle discipline, if either survives a second revision without moving to Resolved it should be escalated to Needs Input rather than left provisional.
- **No customer quote was written.** The Working Backwards guidance suggests one for user-facing features. EXT-08's "customers" are an analyst, a mentor, and a downstream program; a quote would be invented rather than sourced, so the customer-scenario fields on each FR carry that weight instead.

**Notes added in rev 2**

- **One P1 acceptance criterion was found unsatisfiable by implementation, not by review.** BTB-EP-3's "no `sample` after `entity_remove`" survived the rev-1 smell scan because it is specific, testable and unambiguous — it was simply false about the platform. Corrected via ADR-6. The general lesson for the remaining milestones: a criterion that describes platform behaviour is only as good as the observation behind it, and M1-style observation is the cheapest place to catch that.
- **`fields` on a `sample` record is now explicitly "what the message carried", not "what the schema declares".** M3 found a schema-declared field that is never published. `header.schemas` remains the full declaration, so a reader can still tell a never-published field from a dropped one.

**Requirement smell scan**

Re-read of all P1 requirement statements found no vague adjectives, superlatives, loopholes ("where feasible", "if possible"), open-ended lists, or comparatives without baselines. Two items worth noting:
- BTB-CX-3's "bounded, documented detection window" defers a number to the design. That is intentional — the value depends on the bus's own liveness semantics, which M5 establishes — but it is a missing metric until then. *Suggestion: fix the number at M5 and revise.*
- BTB-CAP-6 says "roll to a numbered continuation file" as one of two permitted actions without choosing between them. The FR requires that the choice be documented, not that this PRD makes it; if the design should be constrained further, add the choice here rather than leaving it to the implementer.

**Backward traceability**

Source items checked: 41 (from [S1]'s rules, acceptance criteria, steps, and deliverables; [S2]'s four binding constraints; [S3]'s corrections and scope decision).
Fully covered: 37. Intentionally excluded: 4 — the three unselected output shapes and the predicted-sample handling, each with a dated §"Out of scope" entry. Dropped: 0.
