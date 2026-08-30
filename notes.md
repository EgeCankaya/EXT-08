# EXT-08 — notes on what the bus actually carries

Working notes, written as the work happens. The brief asks for "what the stream contained
that we did not expect", so the surprises are called out rather than smoothed over.

Environment for everything below: N8RO 2.1.328, `C:\N8RO`, Windows 11, one host.
Observation tool: the shipped `n8ro-shark.exe` — passive, records JSONL. We did not write
an observer and we do not link against shark.

---

## M1 — Watch the traffic

### How the observations were made

`n8ro-sim-app.exe` is a host with **no scenario argument** (`--sim-config`, `--model-path`,
`--schema-file` and nothing else), so it cannot drive a run on its own. The shipped
`n8ro-sim-local.exe` does: it hosts an engine, publishes `load_scenario`, starts it, and
runs for a wall-clock budget.

```
n8ro-sim-local.exe --scenario "<name>" --model-path C:\N8RO\data\db --run-ms <ms>
    defaults: --host-config SimEngineHost_SharedMemory
              --client-config SimEngineClient_SharedMemory
              --schema-file N8roSimSchema
```

Shark attaches to that host as a client and records:

```
n8ro-shark.exe --sim-config SimEngineClient_SharedMemory
               --model-path C:\N8RO\data\db
               --schema-file N8roSimSchema
               --topic-pattern "sim/**"
               --recording-dir <dir>
```

Two practical points the docs do not state:

- **`--recording-dir` arms the recorder; it does not start it.** Shark still needs its
  **Start Capture** button pressed. Scripted from PowerShell via UI Automation
  (`InvokePattern` on the button named `Start Capture` / `Stop`); shark's Qt widgets are
  visible to UIA, so this automates cleanly.
- **Shark can start a capture before any host exists.** The client attaches when the host
  appears, so `start shark → Start Capture → start the simulator` captures the run from
  before scenario load. That ordering is what makes the roster-from-events observation
  below possible at all.

### The topic is `sim/entity/state` — confirmed at runtime

Confirmed by observation, not by trusting the doc. Full topic inventory from the reference
capture — `sim/**`, armed before the host existed, so it covers bring-up, scenario load,
45 s of engagement and teardown. 40 873 packets, **zero decode failures and zero drops**:

| topic | packets | rate | note |
|---|---:|---:|---|
| `sim/entity/state` | 38 789 | 818 /s | the entity stream — one packet per entity per frame |
| `sim/engine/state` | 918 | 19.4 /s | one per frame, including idle frames before start |
| `sim/sensor/event` | 316 | 6.7 /s | acquisition / loss |
| `sim/entity/asset` | 172 | — | 3D model path, once per entity |
| `sim/entity/pose` | 172 | — | skeletal joints, animated entities only |
| `sim/system/telemetry` | 159 | — | per-system rates |
| `sim/entity/event` | 134 | — | create / delete — the roster source |
| `sim/physics/collision` | 106 | — | proximity pairs, not only contacts |
| `sim/mission/event` | 84 | — | per-entity mission stage |
| `sim/weapon/event` | 11 | — | launch / detonation |
| `sim/scenario/event` | 4 | — | load / unload — the segment source |
| `sim/engine/event` | 3 | — | engine state transitions with `fromState`/`toState` |
| `sim/engine/command` | 2 | — | **commands, not events** |
| `sim/damage/event` | 2 | — | hits, with `severityScore` and `missDistanceM` |
| `sim/scenario/command` | 1 | — | **command, not event** |

A denser scenario scales the first row and nothing else: 126 entities produced 2487 /s on
`sim/entity/state` while everything else stayed in the same range.

> **`sim/**` carries the command direction too.** `sim/scenario/command` and
> `sim/engine/command` are the messages the *controlling client* published — the recorder
> sees them. Subscribing to a wildcard would record other people's commands alongside the
> engine's own publications. Not wrong, but it is a different kind of data, and BTB-EP-2's
> narrow subscription to the entity-state topic avoids the question entirely.

Field lists for the topics EXT-08 will need beyond entity state:

| topic | fields |
|---|---|
| `sim/scenario/event` | `eventName`, `scenarioName`, `modelName`, `simulationTime` |
| `sim/entity/event` | `eventName`, `scenarioEntityName`, `profileName`, `teamName`, `positionGeodetic`, `reason`, `simulationTime` |
| `sim/engine/event` | `eventName`, `fromState`, `toState`, `scenarioName`, `simulationTime` |
| `sim/engine/state` | `state`, `scenarioState`, `frameNumber`, `simulationTime`, `deltaTimeS`, `wallElapsedS`, `pacingSleepS`, `platformPublishMode` |

> **`sim/engine/state` carries `wallElapsedS`.** A wall clock, on the stream, next to the
> simulation clock. Tenet 2 says it may appear in a log line and nowhere else — so this
> field is one to record deliberately or not at all, never by copying the message wholesale.

### The entity-state schema

Eleven fields observed on the wire — twelve declared, as M3 found. Decoded (shark's
`decodedJson`, which sorts keys alphabetically):

| field | type | notes |
|---|---|---|
| `simulationTime` | double | seconds; **the sample's own clock** — this is the value that belongs in the capture |
| `scenarioEntityName` | string | the identity key. Unique per run, e.g. `TruckLauncher_07_Shahed_03` |
| `name` | string | the entity **profile**, e.g. `Land_AirDefenseRadar_Generic`. Not a display name |
| `team` | string | `Blue` / `Red` / `Neutral` |
| `phase` | string | e.g. `operational` |
| `health` | string | e.g. `nominal` |
| `presence` | string | e.g. `active` |
| `conditions` | integer | a bitfield; `0` throughout everything observed so far |
| `positionGeodetic` | double[3] | lat °, lon °, altitude m (ellipsoidal here — see the geoid note) |
| `orientationYprRad` | double[3] | yaw / pitch / roll, **radians** — the unit is in the field name |
| `velocityNed` | double[3] | north / east / down, m/s |

**The wire order is not the alphabetical order**, and it is not the order above by accident
— the table is written in wire order. Decoding `rawPayloadHex` from a captured packet gives
the packed layout:

```
"N8RO" magic (4) | version 1,1 (2) | schemaHash (8) | u16 = 12 (2) | field-presence mask 0x07FF (2)
simulationTime (double)
scenarioEntityName, name, team, phase, health, presence   (each: u32 length + bytes)
conditions (8)
positionGeodetic[3], orientationYprRad[3], velocityNed[3]  (doubles)
```

209 payload bytes for a 23-character entity name. The presence mask `0x07FF` is exactly
eleven bits — every field present. **Orientation precedes velocity**, which is only visible
on a packet where the two differ; a stationary entity has both all-zero and cannot settle it.

> **Corrected in M3: the schema declares twelve fields, and this list is missing one.**
> "Eleven bits — every field present" is the wrong reading. The `u16 = 12` two bytes earlier
> is the declared field count, so `0x07FF` is eleven bits of *twelve* — the twelfth,
> `activeAnimation`, is simply absent from every packet ever observed. The count and the mask
> contradicted each other on the same line above and it was read as agreement. See
> [M3 — The entity picture](#m3--the-entity-picture). The eleven fields and their order below
> are correct; the list is just not the whole schema.

> **Trap for M4/M5.** Shark's `decodedJson` is alphabetised, so it is *not* a model for our
> capture's field order. BTB-CAP-3 requires field order to come from `MessageSchema::fields`,
> and the wire layout above says that vector is in a different order again. Do not copy
> shark's rendering; read the schema.

### Publish rate and entity count

- **20 Hz per entity, locked to the frame.** `deltaTimeS` is 0.05 and every entity publishes
  once per frame: 612 `sim/entity/state` packets per entity over 30.55 s of simulation time,
  and 612 `sim/engine/state` packets in the same window. The engine holds real time —
  `n8ro-sim-local` reported 0.018 s of drift over 160 s.
- **Aggregate rate is entity count × 20 /s.** 126 entities → 2487 packets/s → about
  520 KB/s of packed payload.
- **Entity count is a scenario property, and it varies by an order of magnitude.** Measured
  from `sim/entity/event` over a 90 s run of each (the "created" column counts mid-run
  spawns too, so it exceeds the at-load roster):

| scenario | created in 90 s | peak live | removals in 90 s (excluding unload) |
|---|---:|---:|---|
| `GenericTwoShipFormation` | 2 | 2 | none |
| `Baltic Sentinel` | 41 | 41 | none — non-kinetic scenario, entities only spawn |
| `Mariana Shield` | 29 | 28 | 6 `expended`, 1 `destroyed` |
| `Atacama Air Defense` | 60 | 51 | 7 `expended`, 5 `destroyed` |
| `Outback Kamikaze Swarm` | 126 | 126 | 4 `expended` in 240 s, no kills |

- **Throughput baselines for the PRD's performance section**, from the two extremes
  measured: 46 entities → 818 packets/s ≈ 170 KB/s of packed payload; 126 entities →
  2487 packets/s ≈ 520 KB/s. Both scale as `entities × 20 /s × ~210 bytes`. A 10-minute
  reference run is therefore on the order of 100 MB of payload before any text encoding.

### Reference scenario: **Atacama Air Defense**

Chosen against R6's four criteria, from the five surveyed above. Each criterion is met by
observation, not by expectation.

| R6 criterion | Atacama Air Defense |
|---|---|
| multiple entities | **42 at scenario load** — 4 fixed assets, 8 Blue air-defence systems, 30 Red loitering munitions in two groups (`RedUAV_N_01..12` north, `RedUAV_E_01..18` east). Peak 51 live once munitions are in flight |
| at least one removal | **yes, and of two distinct kinds.** `destroyed` for UAV kills, `expended` for SAM rounds that fly and detonate. First kill at simulation time 18.2 s; 12 non-unload removals in the first 90 s |
| a natural end | **yes.** The engagement resolves on its own: live tracks fall 29 → 23 → 16 → 10 → 7 between t=60 and t=180, then stay flat at 7 through t=270 with gun rounds spent frozen at 40. The scenario reaches a terminal, quiescent state at t ≈ 180 s, long before any timer cuts it off |
| duration that makes the rate measurable | **yes.** ~180 s of active engagement, 46 distinct entities publishing at 20 Hz — 818 packets/s aggregate over a 45 s window measured to ±1 % |

It also happens to exercise everything the later milestones need: a full scenario-load
roster burst, mid-run spawns, two removal reasons, `sim/damage/event`, `sim/sensor/event`
and `sim/weapon/event` traffic, and a mission script that publishes its own metrics.

The runners-up and why not:

- **`Outback Kamikaze Swarm`** — 126 entities, which is the most interesting *load* case
  and the one to keep for the M6 overload demonstration. Rejected as the reference because
  in 240 s it produced only 4 removals, no kills at all, and the defender goes winchester
  after four engagements with the swarm untouched. No end, natural or otherwise.
- **`Baltic Sentinel`** — 41 entities and the cleanest narrative (a director script that
  walks five named phases to a stated recovery). Rejected on the removal criterion: it is a
  non-kinetic scenario and produced **zero** removals in 90 s. Worth keeping as the
  *segment* test case, because its phase structure is a natural fit for a reload test.
- **`Mariana Shield`** — 29 entities, a BVR engagement with one `destroyed` and six
  `expended`. A perfectly usable second choice; Atacama simply has more of everything.
- **`GenericTwoShipFormation`** — 2 entities, no removals, no end. Useful as the smallest
  possible smoke test and nothing more.

Repeatable recipe:

```
n8ro-sim-local.exe --scenario "Atacama Air Defense" --model-path C:\N8RO\data\db --run-ms 200000
```

### The roster does come from `sim/entity/event`

BTB-EP-3 assumes it; it holds. At scenario load, one `entity_created` is published per
scenario entity, before any `sim/entity/state` traffic. Weapons spawned mid-run get their
own `entity_created`, and are removed with `entity_deleted`.

`sim/entity/event` fields: `eventName`, `scenarioEntityName`, `profileName`, `teamName`,
`positionGeodetic`, `reason`, `simulationTime`.

Removal reasons seen in practice: **`expended`** (a munition that flew and detonated),
**`destroyed`** (a kill), **`scenario_unload`**. `commanded` and `despawned` are in the
platform's vocabulary but were not produced by any scenario observed.

> `entity_created` carries a real `positionGeodetic` for entities materialised **at scenario
> load**, but `[0,0,0]` for anything spawned **mid-run** (weapons, scripted arrivals) — the
> position is not yet populated at spawn. `entity_deleted` always carries a real position.
> So the create event is not a substitute for a first sample.

### Scenario events, and a spurious one at bring-up

`sim/scenario/event` fires exactly twice per load cycle, and both carry
`simulationTime = 0.0` — the clock is reset at the boundary in both directions. The four
events from the reference run, in order:

```
{"eventName":"scenario_unloaded","scenarioName":"",                   "modelName":""}
{"eventName":"scenario_loaded",  "scenarioName":"Atacama Air Defense","modelName":"N8roSimSchema"}
{"eventName":"scenario_unloaded","scenarioName":"Atacama Air Defense","modelName":"N8roSimSchema"}
{"eventName":"scenario_loaded",  "scenarioName":"Atacama Air Defense","modelName":"N8roSimSchema"}
```

> **The first `scenario_unloaded` has an empty `scenarioName`.** It is published during
> engine bring-up, when nothing has ever been loaded. BTB-CX-4 says to close any open
> segment on an unload; a naive implementation opens a segment on this and emits an unnamed
> one. Treat an unload with an empty `scenarioName` as bring-up noise, not a boundary.

`sim/engine/event` gives the same lifecycle from the engine's side, and more usefully —
`engine_initialized` (`uninitialized`→`initialized`) and `engine_started`
(`idle`→`running`), each with `fromState`, `toState` and the scenario name. That is a
better segment trigger than the scenario topic if the ordering ever proves unreliable
(the PRD's "scenario reload timing" rabbit hole).

### The reset surprise — three findings that bite M5

1. **Stopping the engine unloads *and reloads* the scenario.** `engine.stop()` logs
   "Scenario reset completed after stop", which publishes `entity_deleted` with
   `reason="scenario_unload"` for every live entity — and then immediately publishes
   `entity_created` for the whole roster again. A run therefore ends with a full roster
   *present*, not empty. BTB-CX-4's segment logic has to key on the scenario event, not on
   the roster going quiet.
2. **Those teardown events carry `simulationTime = 0.0`.** The clock has already been reset
   when they are published, so the removal that ends a run is stamped at time zero, before
   every sample in the run it ends. Anything that assumes `simulationTime` is monotonic
   across a segment boundary is wrong. This is a real ordering hazard for the capture and
   is called out here so M5 designs for it rather than discovers it.
3. **`simulationTime` accumulates float error and it is visible.** Observed values include
   `35.20000000000014` and `65.74999999999841` — a 0.05 increment summed a few hundred
   times. Rounding those for readability would destroy the exactness the capture exists to
   preserve. They must be written round-trip exact, which is the next section.

### OQ-5 settled by test: `std::to_chars`, not `printf`

Not cosmetic — a locale-dependent writer breaks EXT-17's byte-for-byte determinism
self-test on any machine whose locale uses a decimal comma, which includes this one.

Probe: `tests/float-format/float_format_probe.cpp`. Corpus of 200 000 finite doubles —
4000 simulation clocks accumulated at 0.05 s, the geodetic and velocity values actually
seen on `sim/entity/state`, subnormals and extremes, and uniform random bit patterns.
Two independent axes: does re-reading the text give the identical bit pattern, and is the
text byte-identical under `"C"` and under a comma-decimal locale.

```
cl /std:c++17 /O2 /EHsc float_format_probe.cpp

corpus            200000 finite doubles
comma locale      de-DE

A  to_chars shortest          round-trip PASS  locale PASS  maxlen 24
B  to_chars scientific,16     round-trip PASS  locale PASS  maxlen 24
C  snprintf "%.17g"           round-trip PASS  locale FAIL  maxlen 24
        first locale divergence: C locale "0.050000000000000003"  vs  de-DE "0,050000000000000003"

2 of 3 contenders pass both axes
```

**Verdict: `std::to_chars` on both counts; `printf` family disqualified.** `%.17g` is
round-trip exact and it is *silently* locale-dependent — it does not fail, it emits
`0,05`, which is not JSON. `std::to_chars` is specified to ignore the locale, and the test
confirms it here rather than taking the standard's word for it.

Between the two passing forms, **shortest round-trip (A) is the one to use.** It satisfies
BTB-CAP-3's actual requirement — round-trip exact, locale independent, and uniquely
determined for a given double, so it is stable across runs and builds — while producing
shorter output than 17 fixed digits. BTB-CAP-3 says "17 significant digits"; 17 digits is
a *means* to round-trip exactness, and `to_chars` shortest reaches the same end. Flagging
the wording rather than changing it: the decision lands in M5, and the spec text should say
"round-trip exact" if shortest is adopted.

Buffer note: 24 characters is the observed maximum for either form. `to_chars` shortest
emits `0` for `0.0` and `-0` for `-0.0`; both are valid JSON numbers.

### Other things worth knowing

- **Shark's JSONL is not the format its own documentation describes.** `recording-and-jsonl.md`
  shows a flat object with `sequence` / `topic` / `decodedJson` at the top level. The file
  actually written wraps it: `{"payload":{...the documented fields...},"timestampIso":...,"type":"packet"}`,
  and non-packet rows (`"statsSnapshot"`) are interleaved. Anything parsing a shark capture
  has to read `.payload` and filter on `.type`. Independent of our own format, but it is a
  reminder of why BTB-DOC-1 exists.
- **Shark's recording is wall-clock everything** — `timestampIso`, `timestampMs`,
  `deltaGlobalMs` — which is exactly the property that makes it unusable as a capture for
  EXT-17 and exactly why EXT-08 exists.
- **Rotation is aggressive.** A `sim/**` capture of a 126-entity scenario produced 8 MB
  files every two seconds — ~4.4 MB/s of JSONL for ~520 KB/s of payload, because shark
  writes both hex and base64 copies of the raw bytes.
- **The geoid grid is missing from this install** (`data\geoid\earth_geoid_05m_g.n8grid`),
  so every run logs a terrain-datum warning and runs "degraded (sea-surface fallback)".
  Altitudes are ellipsoidal, not orthometric. It does not affect the bus contract, but a
  capture's altitudes carry that caveat and the README should say so.
- **`n8ro-sim-bot.exe` is an MCP server** exposing `sim_state`, `sim_list_scenarios`,
  `sim_load_scenario`, `sim_control`. It is a second way to drive a host over the bus. It
  did not answer a plain newline-delimited JSON-RPC `initialize` on stdin; not pursued,
  because `n8ro-sim-local` covers the need.
- **`phase` starts at `uninitialized`.** The first samples of a freshly loaded entity carry
  `phase="uninitialized"` before the systems have run a frame over it. Not an error state.

### The one that cost real disk: `n8ro-sim-local` writes into your working directory

Unasked, and it is not documented anywhere. Every run drops a tree at
**`./test_artifacts/n8ro-sim-local/`** relative to the process's current directory:

```
test_artifacts/n8ro-sim-local/
    sim_engine_state.jsonl          every sim/engine/state message
    sim_entity_state.jsonl          every sim/entity/state message, all entities
    entities/<scenarioEntityName>.jsonl    one file per entity, per entity name ever seen
```

Running it from the repo root put **284 MB across 250 files into the working tree**, and the
`entities/` directory accumulates across runs and scenarios — files from a scenario run an
hour earlier are still there. `test_artifacts/` is in `.gitignore` for that reason; run it
from a scratch directory if you can. It also holds an open handle on the directory for a
while after the process exits, so a delete immediately after a run fails with
"in use".

Two useful things came out of it, though:

**1. It is independent confirmation of the field order.** It writes each sample in schema
order rather than alphabetically, and the order is exactly the wire layout decoded above:

```json
{"simulationTime":0.05,"scenarioEntityName":"RedUAV_N_01","name":"Air_UAV_LoiteringMunition_Generic",
 "team":"Red","phase":"operational","health":"nominal","presence":"active","conditions":0,
 "positionGeodetic":[-23.41870470367031,-68.2802,400.0],
 "orientationYprRad":[3.141592653589793,0.0,0.0],
 "velocityNed":[-55.0,6.735557395310443e-15,0.0]}
```

Two independent derivations — the packed bytes and a shipped tool's own serialiser — agree
on `simulationTime, scenarioEntityName, name, team, phase, health, presence, conditions,
positionGeodetic, orientationYprRad, velocityNed`. The runtime `MessageSchema` remains the
authority, but there is now a check to hold it against.

**2. Its float rendering is shortest-round-trip, not `%.17g`.** `6.735557395310443e-15`,
`-23.41870470367031`, `400.0` — the platform's own JSON writer already emits the form the
OQ-5 test recommends. Adopting `to_chars` shortest puts EXT-08's numbers in the same shape
as the platform's, which is a small argument in its favour beyond the test result.

**3. `sim/engine/state` has enumerated states worth knowing.** `state` moves
`idle → running`; `scenarioState` moves `not_available → opened`; `platformPublishMode`
moves `inactive → daemon → active`. The middle one is a cleaner "is a scenario loaded"
signal than inferring it from entity traffic.

### Reading the brief against the runtime, and OQ-1 closed

The brief was read directly before deciding OQ-1, to check whether it settles the question.
**It does not** — it asserts the entity picture exists and points at
`include\n8ro-sim\infrastructure\EntityStateSample.h`, but says nothing about whether that
layer is shipping, planned, or ours. Reading it did turn up two things worth recording.

**The brief describes an entity picture that carries a field the schema has never had.** Its
"what the client gives you" section lists the latest sample as *"name, team, geodetic
position, orientation, velocity, **acceleration**, phase, health, presence, condition flags,
and the simulation time"*. There is no acceleration field anywhere in `simEntityStateUpdate` —
the twelve declared fields are listed above and acceleration is not among them, published or
otherwise. So the brief is not describing 2.1.328 slightly early; it is describing a different
API. That is now three independent discrepancies in one paragraph of the brief: a header that
does not exist, a field that does not exist, and a published-versus-predicted choice this
release cannot offer.

**The brief's own removal criterion is looser than the PRD's, and we satisfy it directly.**
The brief says *"Entity removal is reflected: nothing lingers in the output after a body is
gone."* The PRD tightened that into "no `sample` record for that entity appears after its
`entity_remove`", which is what turned out to be unsatisfiable. The brief's wording is about
the **output** — the capture — and the occupancy model satisfies it exactly: a closed
occupancy stops receiving samples, so nothing about a dead body reaches the file. Worth
recording because the brief is the outer contract, and ADR-6 complies with it rather than
merely working around the PRD.

**OQ-1 is closed: we own the layer.** The reasoning that decided it is that the layer is cheap
to change or delete in either direction — EXT-17 binds to the capture file, not to EXT-08
source, so nothing downstream constrains `EntityPicture`'s internal shape and it can be
rewritten at any point, before or after the format freeze. Given a reversible decision, the
robust choice is to own it: if a later release ships the type, deleting ours costs a day,
whereas being under-built while permanent costs every milestone after M3.

### What owning it actually changed

Three things, and one deliberate omission.

**Tests that need no simulator.** `tests/entity-picture/` drives the picture by handing it
`StreamValueMap`s directly — which is exactly what `MessageBusPacked`'s `DecodedHandler` does,
so the tests exercise the real entry points rather than a stand-in. 62 checks over occupancy
lifecycle, orphan counting, verbatim reasons and payloads, absent-field accounting,
deterministic ordering, the bounded event log, and concurrent handler/snapshot traffic. No
framework, matching `tests/float-format/`. It compiles against headers alone and links **no
N8RO import library**, which is itself a useful fact: the picture has no DLL dependency.

**The tests were mutation-checked, and that immediately paid.** Five deliberate defects were
introduced into `EntityPicture.cpp` and the suite re-run. Four were caught. One was **missed**:

```
[MISSED] keep stale sample on recreate : 59 checks, 0 failures
```

Deleting the `latest_.erase()` on re-creation left every test green, because every test that
re-created a name published a fresh sample immediately afterwards. The gap is the window
*between* the two — a stale sample from a closed tenure readable as the current state of the
new occupancy, which is precisely what BTB-EP-4's ordering criterion forbids. A test now
covers that window explicitly, and all five mutants are caught. **A suite that passes on
broken code is worth nothing, and the only way to know which one you have is to break it.**

**A snapshot API that cannot report a dead entity as live.** The latest-sample map keeps a
closed occupancy's final sample on purpose — "where was it when it died" is a question the
referee will ask — which means the map alone is not a liveness answer. Rather than leave every
future caller to join it against the roster correctly, the snapshot now offers
`liveSample()` (nothing for a removed entity) and `lastKnownSample()` (named so that reaching
for a dead entity's state is a visible choice). The retention rule is documented on the type:
retained while the occupancy stays closed, dropped when a new one opens under the same name.

**No interface, deliberately.** Owning a component is not automatically a reason to put a
pure-virtual base under it. There is no second implementation, and the seam that actually
matters already exists and is cheaper: the picture consumes a decoded `StreamValueMap`, so
M6's replay path can feed the same class from a stored capture instead of from the bus. Adding
indirection for a hypothetical would be exactly the speculative generality the PRD warns
against elsewhere. Ownership bought tests and a safer API, not architecture.

### Open questions this milestone touched

- **OQ-3 — the topic string and its schema fields: answered above**, by observation.
  Recording it here, not in the PRD, is the point: the code still has to read it from
  `MessageBusPackedSchemaRegistry` at runtime (BTB-EP-1), and this note is the check on
  that, not a substitute for it.
- **OQ-5 — float format: answered above.** `std::to_chars`; `printf` disqualified.
- **OQ-4 — bus backpressure policy: not settled, and M1 gives it a number.** The bus
  default is `KEEP_LATEST` with a queue of 100. At 2487 packets/s a 100-message queue is
  40 ms of headroom. That is the sizing input M6 needs.
- **R6 — reference scenario: closed.** `Atacama Air Defense`, justified above.

---

## M2 — Smallest possible client

Nothing surprising, which is the finding. `create()` → `startMessagePump()` → read the
local getters once a second. Build and run instructions are in `README.md`.

```
[INFO] (n8ro-bridge) creating client: config=SimEngineClient_SharedMemory modelPath=C:\N8RO\data\db schemaFile=N8roSimSchema
[INFO] (n8ro-bridge) message pump started; reporting engine state once a second from the local getters (Ctrl-C to stop)
engine=running      frame=229        simTime=    11.450 scenario=Atacama Air Defense
engine=running      frame=249        simTime=    12.450 scenario=Atacama Air Defense
engine=running      frame=269        simTime=    13.450 scenario=Atacama Air Defense
```

Frame advances by exactly 20 per printed second — independent confirmation of the 20 Hz
rate measured off the bus, this time from the client's own state cache.

Three things worth writing down:

- **The local getters really are local.** `getEngineState()`, `getFrameNumber()`,
  `getSimulationTimeS()` and `getLoadedScenarioName()` read a mutex-protected cache that the
  client's *own* `sim/engine/state` subscription fills. No round trip on the print path —
  but also **nothing at all until `startMessagePump()` has run and the engine has published
  a frame.** Before that they read empty / zero, which is indistinguishable from a
  connected-but-idle engine. Do not treat a zero frame number as "not connected". Observed
  both ways: a host brought up without a scenario prints
  `engine=idle frame=0 simTime=0.000 scenario=(none)` indefinitely, which is a *correct*
  report of a real state and not a failure to attach.
- **The rotating file logger writes into the release tree.** `GlobalLogger::initializeRotatingConsoleFile`
  resolves its path to `<N8RO_RELEASE>\logs\`, and `C:\N8RO` is read-only for this project.
  Used `LogReportingFactory::createLogger<ConsoleLoggerConfig>()` instead — console only,
  nothing written outside this repo. Anything later that wants a log file has to name a
  path we own.
- **`create()` failure is diagnosed from below, not by us.** A wrong `--schema-file`
  produces four platform log lines naming the exact `.n8ro.instance` path that could not be
  opened, before our own diagnostic runs. Our contribution is echoing all three
  configuration values back as resolved, and exiting 3. Exercised: wrong schema file, wrong
  option, missing value, no arguments. No exception reaches `main` in any of them.

---

## M3 — The entity picture

The layer the brief assumed was free. `SimulationEngineClient` holds a
`MessageBusPackedSchemaRegistry` and a `MessageBusPacked` privately and exposes **neither** —
only `messageBus()`. So the packed layer is ours to build over the client's bus, from our own
`DbModel`. That is the passive-observer recipe `n8ro-shark`'s developer docs prescribe, and it
is four lines; the work is everything downstream of it.

### The headline: the entity-state schema has **twelve** fields, not eleven

M1 derived the field list two independent ways and both said eleven. The runtime
`MessageSchema` declares twelve:

```
simulationTime:double, scenarioEntityName:string, name:string, team:string, phase:string,
health:string, presence:string, conditions:int, positionGeodetic:double[3],
orientationYprRad:double[3], velocityNed:double[3], activeAnimation:string
```

**`activeAnimation` was never published — not once in 132 188 samples** on the reference run,
and not once in the 28 370 lines of `n8ro-sim-local`'s own `sim_entity_state.jsonl`. It is
declared in the schema and absent from the wire.

M1's own decode said so and it was misread. The packed header M1 wrote down is

```
"N8RO" magic (4) | version 1,1 (2) | schemaHash (8) | u16 = 12 (2) | field-presence mask 0x07FF (2)
```

That `u16 = 12` is the **declared field count**. The presence mask `0x07FF` is eleven bits, and
M1 read it as "exactly eleven bits — every field present". It is eleven of *twelve*: the
twelfth field is absent. The count and the mask disagreed on the same line, and the
disagreement was read as agreement.

So the three-way check comes out as: **all three derivations agree on the first eleven fields
and their order; only the runtime schema knows about the twelfth.** The bridge logs this as a
`DISAGREES` warning at startup and keeps running — the runtime schema is authoritative by
definition, so a mismatch is a fact about the notes, not a reason to refuse to work.

> **This is BTB-CAP-4's verbatim rule earning its place before M4 has even started.** A
> curated `EntitySample` struct built from M1's careful, twice-checked observations would have
> been born with a field missing, and nothing would ever have said so. The "what the stream
> contained that we did not expect" deliverable would have been unwritable, because the
> unexpected was filtered out before anyone could see it. The rabbit-hole warning in the PRD
> is not hypothetical.

The corollary is the platform's own rule, now demonstrated rather than quoted: *a packed
payload carries only the fields the publisher wrote, so a subscriber reads a field's presence
rather than assuming every field arrives on every message.* Every field read in
`EntityPicture` returns `std::optional` and counts its own absence.

### Schema identity, for M4's capture header

```
messageName  simEntityStateUpdate      messageName  simEntityEvent
topic        sim/entity/state          topic        sim/entity/event
fields       12                        fields       7
schemaHash   2652370635                wireVersion  1
messageId    1308183250
```

Registry size on a good configuration: **35 message schemas.**

### The entity-event field order also differs from the M1 table

Runtime: `eventName, simulationTime, scenarioEntityName, profileName, teamName,
positionGeodetic, reason`. The M1 table listed `simulationTime` last. That table was a field
*inventory* and never claimed to be wire order, so it is not an error there — but M4 must take
event field order from the schema too, not from that table.

### Resolving topics without a literal

BTB-EP-1 says the topic comes from the registry, never from a literal — but the registry is
indexed *by topic* and *by message name*, so resolution needs an anchor. Two chains, both
anchored on something checked by the compiler or by the database:

```
entity state   "simEntityStateUpdate"  (--entity-state-message, default)
                 -> registry.getByName() -> MessageSchema::topic == "sim/entity/state"
                 -> structural check: declares scenarioEntityName + simulationTime?

entity event   kEventEntityCreated / kEventEntityDeleted   (EventNames.h constants)
                 -> EventConfigReader::readVocabularyFromModel()
                 -> EventConfigData::topic == "simEntityEvent"     <- a MESSAGE NAME
                 -> registry.getByName() -> MessageSchema::topic == "sim/entity/event"
```

**`EventConfigData::topic` is not a topic string.** It names the Message instance whose
envelope carries the event; the topic string is on that message's schema. The header says so
in a comment, and the field name actively works against it. Two hops, not one.

The event chain needs no configuration at all, because `EventNames.h` gives compile-checked
constants and states the rule outright: *"The topic each event travels on is the Event
instance's own `topic` field, not a constant here: a consumer reads the pairing from the
database, and a second copy in a header would drift from it."*

The state chain has no event to anchor on, so it anchors on a message name and then **checks
the shape** of what that name resolved to. A name alone is not enough: `simEntityTrackUpdate`
and `simEntityPoseUpdate` are plausible neighbours in the same database, and resolving to one
would subscribe successfully and roster nothing, forever. Verified by pointing it at
`simEngineState`:

```
[ERROR] message simEngineState resolves to topic sim/engine/state but does not declare the
        fields the entity picture keys on: missing scenarioEntityName. This is not the
        entity-state message; refusing to subscribe to a topic that would roster nothing
[ERROR]   declared fields: state:string, scenarioState:string, scenarioName:string, ...
```

The three EP-1 failure modes, each a distinct exit code, all verified:

| condition | how provoked | exit |
|---|---|---:|
| registry empty | `--schema-file NoSuchSchema` | 7 |
| message name unresolvable | `--entity-state-message simNoSuchMessage` | 8 |
| resolves, wrong shape | `--entity-state-message simEngineState` | 9 |

The empty-registry case is the one that matters, because it is the failure that looks like
success. It names the model path and the schema file, and refuses to run.

### BTB-EP-3, and the criterion that could not be met as written

M1 found that stopping the engine deletes every entity with `reason="scenario_unload"` and
immediately re-creates the whole roster under the same names. BTB-EP-3's acceptance criterion
— *"No `sample` record for that entity appears after its `entity_remove`"* — is therefore
**unsatisfiable read literally**, because samples demonstrably resume under a name that has
been removed.

Resolved by decision on the day by the project owner: make a name **not** the identity. The roster keys
on name but tracks a monotonically increasing **occupancy generation**: `entity_created` opens
generation *N+1*, `entity_deleted` closes it, and a sample belongs to the occupancy open when
it arrives. The criterion then reads *"no sample after the removal, within that occupancy"* —
exactly satisfiable, and strictly stronger than the segment-scoped reading because it needs no
segment machinery, which belongs to M5.

The real violation the criterion was reaching for gets its own counter: **a sample for an
entity with no open occupancy** is `orphaned`, counted, and never entered into the map.

**The PRD now says this too** — it is not a note-level workaround around a contract that still
says something else. Rev 2 rewrites BTB-EP-3's requirement and criterion, records the decision
and its rejected alternatives as **ADR-6**, adds UAC-BTB-EP-3b for the re-created name, and
puts an `occupancy` ordinal on the `entity_add` / `entity_remove` / `sample` records so the
criterion is checkable in the capture and not only in memory. That last part matters for
EXT-17: without it, a reader seeing `entity_remove(destroyed)` followed by more samples under
the same name has every reason to call the file corrupt.

Confirmed on the reference run. `RedUAV_N_01` is the sharpest case — not merely unloaded but
*killed*, and then back:

```
entity_created RedUAV_N_01 gen=1 at simTime=0.000000
entity_deleted RedUAV_N_01 gen=1 at simTime=149.450000 reason=destroyed
entity_created RedUAV_N_01 gen=2 at simTime=0.000000
```

### The reference run

`n8ro-sim-local.exe --scenario "Atacama Air Defense" --model-path C:\N8RO\data\db --run-ms 200000`

```
engine=idle  frame=0  simTime=0.000  scenario=Atacama Air Defense
    live=42  names=90  samples=132188  drops=0(hash=0 decode=0 noschema=0)
    removals=destroyed:23 expended:48 scenario_unload:19  created=132 deleted=90 orphaned=0
```

| | |
|---|---:|
| entities at scenario load (gen 1, simTime 0) | **42** |
| distinct names ever seen | 90 |
| occupancies opened (90 gen-1 + 42 gen-2) | 132 |
| removals: `destroyed` / `expended` / `scenario_unload` | 23 / 48 / 19 |
| samples accepted | 132 188 |
| `schemaHashDrops + decodeFailures + missingSchemaPassthrough` | **0** |
| orphaned samples | **0** |

**42 at scenario load matches M1's count exactly**, arrived at independently — M1 counted it
off a shark capture, M3 counts it off our own roster.

Everything M3 was asked to show is in that one line: both removal kinds visible and carried
verbatim (the kills `destroyed`, the munitions `expended`), zero drops, zero orphans, across a
full load-run-teardown cycle.

Three things worth keeping:

- **The teardown churn is quantified, not just described.** 19 live entities removed with
  `scenario_unload` and 42 re-created, every one stamped `simTime=0.000000` — the clock is
  already reset when they are published, so the removals that end a run sort *before* every
  sample in the run they end. M1 flagged this as an ordering hazard for M5; it is now a
  measured one.
- **`orphaned=0` through that churn is the whole argument for the generation scheme.** A
  name-only roster would have counted every post-teardown sample as an orphan — or worse,
  accepted it silently against a closed entry.
- **`entity_updated` was never published.** The constant is in `EventNames.h` and the instance
  is in the database; nothing raised it in 200 s. The unhandled-event-name counter stayed empty
  all run, which is how we know rather than assume.

### Two operational traps, both of which cost real time

**1. `n8ro-sim-local` resolves its plugin directory from `N8RO_RELEASE`, and fails the scenario
load without it.** Launched from a scratch directory in an environment that had not run
`setup.cmd`, it scans `<cwd>\bin\plugins\sim`, finds no physics plugin, and *refuses* the load:

```
[WARN]  N8RO_RELEASE not set; falling back to current working directory for runtime path resolution.
[ERROR] Scenario load refused: Component type 'componentPhysics' has no registered factory, so 42
        entities, e.g. 'BlueBase_AmmoDepot' would load without it and the scenario would run with
        that capability missing.
```

The bridge attaches happily and reports `engine=running scenario=(none)` — a correct report of
a real state, and indistinguishable at a glance from a bridge that failed to attach.

**2. Start the bridge before the simulator.** The `entity_created` burst is published once, at
scenario load. A bridge started 4 s late missed all 42 of them:

```
engine=running  simTime=12.350  live=0  names=0  samples=0  orphaned=7740
```

Every sample orphaned, roster empty, and — the point — **`drops=0` and no error anywhere**.
Without the orphan counter this reads as a working bridge attached to an empty scenario. It is
the same shape of failure as the empty registry, caught by a different net. Surviving either
start order without operator intervention is BTB-CX-2 and belongs to M5; until then the run
recipe is bridge first, simulator second.

### Open questions this milestone touched

- **OQ-1 — do we own the entity-picture layer permanently? Closed: yes, we own it.** Decided
  by the project owner after checking the brief, which is silent on the question. The layer is
  reversible in either direction, so the robust choice wins. See the two sections above for
  what that changed and what it deliberately did not.
- **OQ-4 — backpressure. Unchanged, still M6's, but M3 adds a data point:** the run above took
  132 188 samples through the bus default (`KEEP_LATEST`, queue 100) with zero drops at ~660
  samples/s. The default is lossy, and on this scenario it lost nothing. That is a measurement
  of headroom on *this* load, not a reason to keep the default. The bridge warns about the
  policy at startup so that it is never an implicit choice.

## M4 — The capture format

The milestone where the stream stops being something we watch and becomes something we hand
to another repository. Almost everything below is a fact about what a real capture *contains*
that was not obvious before one existed.

### The reference capture

`n8ro-sim-local.exe --scenario "Atacama Air Defense" --model-path C:\N8RO\data\db --run-ms 200000`,
bridge started first, budget 100 000 samples.

```
capture=100000/100000 records notRecorded=114
capture written: captures\capture-atacama-000.n8rocap.jsonl
    (100000 sample records, segment 0, simTime 0.050000 to 129.900000, end_reason=size_limit)
declared but never published, so present in header.schemas and in no sample record:
    activeAnimation (1 of 12 declared fields)
```

| | |
|---|---:|
| sample records | 100 000 |
| file size | 48 444 217 bytes |
| bytes per sample record | ~484 |
| simulation time covered | 0.05 → 129.90 s |
| distinct entity names | 77 |
| distinct (name, occupancy) pairs | 77 |
| bus decoder drops | **0** |
| orphaned samples | **0** |
| samples not recorded | 114 |

**~484 bytes of JSONL per ~210 packed bytes on the wire — a 2.3× expansion.** Worth writing
down next to the M1 measurement of `n8ro-shark`, which produced ~4.4 MB/s of JSONL for
~520 KB/s of payload — roughly 26× — because it writes both a hex and a base64 copy of the
raw bytes. Decoded-and-schema-headed is an order of magnitude cheaper than raw-and-doubled,
which is the trade ADR-2 was making without a number attached. It is also the input BTB-CAP-6
needs: a ten-minute reference run is on the order of 240 MB of capture.

### The 114 that did not make it, and why that is the good news

`notRecorded=114` on the first real capture. The budget filled at 100 000 while the bridge was
asleep between two one-second status ticks, and 114 more samples arrived before the loop woke
up and unsubscribed. **The M4 harness's stop is tick-granular, not sample-exact.**

It is in the trailer as `drops.samples_not_recorded`, and the conformance reader surfaces it
unprompted. That is the whole design working on its first outing: a recorder that had quietly
dropped 114 samples and said nothing would be indistinguishable from one that had not, and
the file itself now says which. M5 replaces the budget with a real queue; the field keeps its
meaning and its name.

### What a `double` actually looks like in the file

Three shapes, all in the reference capture, all valid JSON numbers, all produced by
`std::to_chars` shortest round-trip:

```
"sim_time_s":72.09999999999805          accumulated frame error, preserved to the bit
"positionGeodetic":[-23.454302692591245,-68.25275,400]      <- 400, not 400.0
"velocityNed":[-55,6.735557395310443e-15,0]                 <- integer-looking, and scientific
```

**`400` is a `double` field.** So are `-55`, `0` and `3.141592653589793`. Shortest round-trip
emits no trailing `.0`, because it does not need one to round-trip — and that is a genuine
trap for a reader that types values from the JSON token instead of from the schema. Such a
reader silently gets an integer for every round altitude in the file and a double for every
other, and nothing anywhere reports a problem. §8.3 of the format spec says this in bold for
exactly that reason, and the conformance reader parses every `double`-declared field through
a double path regardless of how the token looks.

Scientific notation appears too (`6.735557395310443e-15`), which is legal JSON and worth
having seen before a reader meets it.

### Two header fields the platform would not tell us

Both are recorded honestly rather than filled in from somewhere plausible.

- **`platform.schema_version` came back empty.** `DbModel::getSchemaVersion()` returns `""` on
  this model database — it declares none. The spec says the field may be empty and what that
  means, rather than the producer inventing a value.
- **`platform.runtime_version` is the literal `"unknown"`.** The SDK's own accessor,
  `n8ro::core::getN8roVersion()`, is `constexpr` and returns `"unknown"` unless `N8RO_VERSION`
  was defined when *the including translation unit* was compiled, which for us it is not.
  `C:\N8RO\components.xml` does carry `2.1.328`, and reading it was rejected: parsing an
  installer manifest and writing the result into a field called "observed" would be the
  producer claiming to have measured something it looked up. `"unknown"` means the runtime did
  not tell us, and the spec says so in those words.

### `format_version` first beat `type` first

BTB-CAP-1 requires `format_version` to be the first key of the first record, so a reader can
reject an unknown version before parsing anything. The envelope's own shape wants `type`
first, on every record. They collide on exactly one line.

Resolved in favour of the requirement: the header is `{"format_version":…,"type":"header",…}`.
`type` is still present, so a reader dispatches every record in the file on one key, and the
version check still works on a prefix of the first line. Small, and the kind of thing that is
a one-line fix now and a format-version bump after the M7 freeze.

### The occupancy field is exercised, but only at generation 1

77 distinct names, 77 distinct (name, occupancy) pairs — **no name reuse in this capture.**
That is not the occupancy scheme failing; it is the budget stopping the recording at
t = 129.9 s, and the engine's stop-path teardown burst — the thing that re-creates the whole
roster under the same names — happens at engine stop, at t ≈ 200 s. The reference capture ends
before the interesting case.

Recorded as a known gap rather than left to be discovered. M3's evidence has the gen-2 case
measured (`RedUAV_N_01` destroyed at 149.45, re-created at teardown), the conformance reader
has a mutation that constructs the violation synthetically and catches it, and the first
capture that will contain a real second occupancy is M5's, once the run is allowed to end and
`entity_add` / `entity_remove` records exist to bracket it.

### The wall-clock check, and its two false positives

BTB-CAP-2 says no wall-clock value appears anywhere in the capture. Checked by walking every
JSON value of all 100 004 records rather than by grepping the text, which matters:

- **A bare-year regex (`20\d\d`) matches 1104 times, and every one is a scenario entity name.**
  The platform names weapons `BlueSAM_ShortRange_wpn_20900_2`, and `20900` contains `2090`.
- **An epoch-magnitude number check (1e9–2e9) matches exactly once:** `schemas[0].message_id`
  = `1308183250`, the platform's own message identifier, copied verbatim from the runtime
  schema and identical on every run.

Strict date and clock shapes (`YYYY-MM-DD`, `hh:mm:ss`) match **zero** times, and no key name
suggests a clock. Both false positives are now excluded by name in the conformance reader,
with the reason in a comment — because the next person to run this check will hit them too,
and a determinism check that cries wolf gets switched off.

### The reader is the test of the document, and it is mutation-checked

`tests/capture-reader/` is written from `docs/capture-format-v1.md` and links neither the
bridge nor the SDK — standard library only. That is what makes BTB-CAP-5's "complete enough
to write a reader from" checkable instead of assertable. Every rule it enforces cites the
spec section it came from; a rule with no citation would be a rule the document failed to
state.

Writing it found three things the first draft of the spec did not say, all now in it: that a
`double` may be written without a fractional part (§8.3), that `sim_time_s` is not monotonic
across a segment boundary so a reader must not sort by it (§5.1), and that the
`entity_remove.reason` vocabulary is deliberately **open** while every other vocabulary in the
format is closed (§9). All three are things a reader author would otherwise get wrong on their
first attempt and only discover against real data.

`tests/capture-reader/mutate.py` holds it honest: 16 deliberate defects injected into a valid
capture, **16 caught, 0 survivors** — wrong field order, an undeclared field, a sample outside
a segment, a miscounted trailer, a truncated file, a record after the trailer, an injected
timestamp, CRLF endings, an unknown `format_version`, a sample after its own occupancy's
removal. Same discipline as `tests/entity-picture/`, and for the same reason: a checker that
has never failed has not been shown to work.

### How M4 records, and why it is not a draft of M5

The handler is still a courier. It copies each accepted sample into a bounded, preallocated
buffer and returns; nothing consumes that buffer while it fills, because there is no consumer.
When the budget is reached the bridge unsubscribes, stops the pump, and only then does our own
thread read the buffer, format every record and write the file. All IO, all formatting and all
float conversion happen on our thread, which is the rule.

**This is deliberately not the M5 design and should not be mistaken for a first draft of one.**
M5's writer thread behind a bounded queue exists to bound *memory* and to make loss an explicit
decision at the second boundary (BTB-BP-4); M4's buffer does the opposite — it trades memory
for not having to build any of that yet. It costs about forty lines and M5 deletes them. What
survives is `src/CaptureFormat.cpp`, which is pure functions over their arguments and never
knew where its records came from.

### Open questions this milestone touched

- **OQ-5 — float formatting. Closed, and the PRD wording changed with it (rev 4).** BTB-CAP-3
  said "17 significant digits". Seventeen digits is a *means* to round-trip exactness, not the
  end, and stating the means excluded the shorter form that reaches the same end. The criterion
  now states round-trip exactness, locale independence and uniqueness directly, and §8.3 of the
  format spec carries the same wording, so the requirement and the cross-repo contract say one
  thing rather than two. Adopted at M4 rather than M5 because the first capture needed it.
- **OQ-4 — backpressure. Still M6's, one more data point:** 100 000 samples at ~860/s through
  the `KEEP_LATEST` default, zero bus-side drops. The 114 losses in this capture are entirely
  ours, at our own boundary, and counted there. The header records which policy the capture was
  taken under, so a future comparison between two captures can rule the policy in or out
  before anything else.

## M4 follow-up — the determinism experiment, and what it found

M4 shipped with three things flagged for follow-up. Two turned out not to need fixing, and
the third turned out to be a different and larger problem than it looked. The experiment that
separated them is the most useful hour spent on this milestone so far.

### The claim that had never been tested

`docs/capture-format-v1.md` §14 promised EXT-17, in writing, that two runs of the same
scenario under the same configuration produce **byte-identical** captures. BTB-CAP-3 said the
same. Nothing in M1–M4 had ever run two identical runs and compared them.

Two reference runs, bridge first, budget 100 000:

```
det-A  ad0bb50d71ddc39b...  48 445 101 bytes
det-B  60a7a1e4d6daff88...  48 446 649 bytes
```

**Not identical.** Header byte-identical, record counts equal, first 30 789 records identical
— then divergence at line 30790, where run B is a whole simulation frame ahead of run A at
the same line number.

### First hypothesis, and it was wrong

Frame `t = 36.10` had 33 samples in run A and **zero** in run B. A partial frame is not what a
skipped simulator frame looks like — the host publishes a frame's entities together — so the
obvious reading was the bus discarding under `KEEP_LATEST`, exactly as ADR-4 predicts.

Checking it turned up something worse than the hypothesis.

### `IMessageBus::getStatistics()` existed the whole time, and nothing here had ever read it

There are **two independent loss surfaces**, and until now this program watched only one:

| | what it counts | read since |
|---|---|---|
| `MessageBusPacked::metricsSnapshot()` | the message arrived and could not be **decoded** | M3 |
| `IMessageBus::getStatistics()` | the message **never arrived**, because the bus discarded it | *nothing, ever* |

`Statistics` carries `messagesDropped`, `droppedByBackpressure`, `droppedByQueueOverflow`,
`droppedByRateLimiting`. A message the bus discards never reaches the decoder, so **no amount
of watching the decoder can reveal it**. Every `drops=0` this project has printed since M1 —
including "132 188 samples, 0 drops" in M3's headline result and OQ-4's "the default lost
nothing" data point — was the decoder's counter answering a question nobody had asked.

That is R2's shape exactly: not a wrong answer, but a confident answer from the wrong object.
Both groups now go into every capture's `bus_metrics` and onto the status line (producer
0.4.2). Adding keys is non-breaking, so the format version does not move.

### Instrumented, re-run — and the bus says it lost nothing

```
busLoss=0(dropped=0 backpressure=0 queueOverflow=0 rateLimit=0)
```

Zero on both runs, while frames were still missing and the two captures still differed. **The
hypothesis was wrong.** Worth writing down as much as the finding: the instrumentation was
right to add and it did not explain the loss.

### The publisher's own record settles it

`n8ro-sim-local` writes a per-entity JSONL dump under `test_artifacts/` — the publisher's own
account of what it published, independent of our bus, our subscription and our code. Comparing
it against our capture over the window the capture covers, across all 77 entities:

```
published by the host   99 981
present in our capture   99 953
absent                       28   (0.028%)
```

and the 28 break down cleanly:

| where | count | what it is |
|---|---:|---|
| `t = 130.10` | 9 | the **final frame, cut mid-way by the record budget** — by design, and `end_reason: size_limit` says so |
| `t = 20.95` | 18 | one frame, mostly lost |
| `t = 95.60` | 1 | a single sample |

So the real, unexplained loss is **19 samples in 99 981 — 0.019%** — and every counter on the
platform reads zero for all of them. It is not the budget, not the decoder, not the bus's own
accounting, not our orphan counter.

**And the missing frames were mostly never published at all.** Over the same 130-second
window, a 0.05 s tick would give 2 601 frames; the host published **2 577**. `n8ro-sim-local`
paces against the wall clock and simply skips about **1% of frames** under load — a different
1% each run. Our capture faithfully records a stream that genuinely differs between runs.

### What that means, and what changed because of it

**Byte-for-byte identity across two live runs was never achievable on this platform, and the
reason is not ours.** Two runs are not the same sequence of published messages, so no property
of the recorder can make the captures match.

The requirement was wrong, not the implementation. BTB-CAP-3 now binds the recorder to what
the recorder controls — *given the same published stream, produce the same bytes* — which is
true, enforceable, and what EXT-17 actually needs (PRD rev 5). Its harness moves from "ten
identical-configuration pairs" to **ten replays of one stored capture**, because live pairs on
a wall-clock-paced host measure the host's repeatability, not ours, and would have made a
platform property look like a recorder defect. §14 of the format spec carries the same scoping
plus the practical advice: compare on content — per-`(entity, occupancy)` value sequences keyed
by `sim_time_s` — rather than on bytes, unless the publisher is known to be deterministic.

Two determinism leaks that **were** ours, both found by the same experiment and both fixed:

- **`drops.samples_not_recorded` was scheduler-dependent.** It counted samples arriving between
  the budget filling and the once-a-second loop noticing. Now structurally `0`: the buffer is
  sized to exactly the budget, so it never rejects a sample *while recording* — the buffer
  filling **is** the end of recording, which `end_reason: size_limit` already states. The
  residue goes to the log, where a scheduler-dependent number belongs. Keeping it in the file
  would also have been misleading: it counted the handful in the shutdown window and not the
  ~57 000 published after we stopped.
- **`attached_mid_run` was decided by a one-second race.** It read `isScenarioLoaded()` at the
  first status tick, so a simulator that won that race flipped the answer. Now derived causally
  from `orphansBeforeFirstAccepted` — whether samples arrived for entities whose creation we
  never saw, which is precisely M3's measured late-attach signature.

The prompt-stop fix (a condition variable, so recording ends at the budget rather than up to a
tick later) cut the shutdown residue from **114 to 9–12** samples. It cannot reach zero: a bus
subscription cannot be stopped atomically.

### R7, and why absence is not evidence

The 0.019% is now **R7** in the risk register, and the honest statement is in §14 for EXT-17 to
inherit rather than rediscover: *all-zero counters mean nothing the platform counts was lost —
not that nothing was lost.* A capture is a very high-fidelity sample of the published stream,
better than 99.98% complete on the reference scenario, and it is not a guaranteed-complete
transcript. A referee that reads the absence of a message as evidence that it never happened
would be drawing a wrong conclusion from a file that looks perfectly clean.

M6 re-runs this comparison under the 126-entity overload scenario, where the rate is 3× higher
and the mechanism should be easier to provoke and attribute. OQ-4 cannot honestly be closed
while a loss path exists that no counter reports.

### The two that did not need fixing

- **No second occupancy in the reference capture.** Not a defect and not fixable at M4. A
  gen-2 sample needs an `entity_add` to open it — a sample under an occupancy no record opened
  is a file the format spec itself calls malformed — and `entity_add` / `entity_remove` are
  M5's. It is also unreachable: the teardown burst is at t ≈ 200 s and the budget fires at
  t ≈ 130 s. ADR-6 is already proven in memory (M3) and against a synthetic capture
  (`mutate.py`). Proving it end-to-end in a real file is now an M5 acceptance item.
- **`runtime_version: "unknown"` — checked, and there is genuinely nothing to read.**
  `getN8roVersion()` is `constexpr` and resolves in *our* translation unit, where
  `N8RO_VERSION` is not defined. The remaining candidate was the DLL version resource, and
  **`n8ro-core.dll` and `n8ro-sim.dll` carry none** — `VersionInfo.FileVersion` is empty on
  both. That leaves compiling in a constant or parsing `components.xml`, and either would put
  a number nobody observed into a field documented as observed. `"unknown"` is the accurate
  answer. Recorded here so nobody re-derives it.

## M4 postscript — reading [S2] directly, and correcting an over-broad claim

Rev 5 rewrote BTB-CAP-3, a requirement that traces to **[S2] constraint 2**, from measurement
alone — without reading the constraint's own wording. Doing that afterwards found the rewrite
was right in scope and wrong in one claim.

### What [S2] actually says about determinism

> *"The simulation is deterministic by contract: the same scenario, the same inputs and the
> same ordering produce the same outputs, on every run and every machine."*

and it makes the check a **hard gate**, not an acceptance nicety:

> *"Prove determinism — the same-configuration self-test above. **Do not build further until
> it passes.**"*

Its acceptance criterion is byte-level: *"two identical runs produce identical captures, and
the tool checks this itself."*

### The correction

Rev 5 said byte-identity is "not achievable on this platform and never was". **That is not
what was measured.** The measurement was against `n8ro-sim-local.exe` — a *test driver* that
paces an engine against the wall clock for a wall-clock budget (`--run-ms`). Frame skipping is
what wall-clock pacing does under load. It says nothing about `n8ro-sim-app.exe`, the headless
host, which is what a campaign actually runs — and which [S2] is explicit about:

> *"Campaign runs are for the closed configuration."*

So the honest position is not "the platform is nondeterministic". It is: **the one host we
measured is not repeatable, the host that matters has never been measured, and the difference
between those two statements is the whole of EXT-17's step-4 gate.** Narrowed in
`docs/capture-format-v1.md` §14 and in BTB-CAP-3; the open question is now **R8**, and it is
half an hour of work once OQ-2 gives up the invocation.

The recorder-side half of rev 5 stands unchanged and was never in doubt: given the same
published stream, this producer emits the same bytes, and the two leaks it found — a
scheduler-dependent drop counter and a race-decided flag — were real and are fixed.

### [S2] confirms ADR-4 from the other side

> *"Anything externally timed feeding the simulation. If a live feed or an external bridge is
> active, runs are reproducible only as far as that input is."*

EXT-08 *is* an external bridge. It is not an input — it subscribes and never publishes — but a
recorder that selected `BLOCK` would stall the bus and become one, breaking the determinism
its own consumer gates on. ADR-4 rejected `BLOCK` on EXT-08's own reasoning; [S2] reaches the
same rejection independently. **Worth carrying into M5's backpressure decision**: the bus-side
policy is not only about our loss, it is about whether a campaign run is reproducible at all
with us attached.

### [S2] has the same defect [S1] does

It lists `include\n8ro-sim\infrastructure\EntityStateSample.h` as the surface for "what a run
publishes". **That file does not exist in 2.1.328** — the finding that cost M3 its budget, in a
second brief, independently. Both documents describe an API this release does not ship.

For EXT-17 that is good news rather than bad: the entity picture EXT-08 already owns (ADR-1)
consumes a decoded `StreamValueMap` and is fed just as easily from a stored capture as from the
bus, which is exactly the seam ADR-1 said it was keeping. EXT-17 does not need the missing
header; it needs the capture format, which it has.

### Two things in [S2] our own work already answers

- *"A diff between two runs identifies the first point of divergence, not just that they
  differ"* (acceptance criterion 6). The M4 follow-up did exactly this — identical headers,
  equal record counts, an identical 30 789-record prefix, then the first differing line — and
  that attribution is what separated "the publisher skipped a frame" from "the recorder lost
  one". Reusable as a method.
- *"A page of notes on determinism — what you had to do to make comparison meaningful, and
  anything you saw that you could not explain. The last part is the one to write carefully."*
  The M4 follow-up section above is that page, and the 19 unexplained samples (R7) are that
  last part.

## M5 — The output path and the lifecycle

The milestone where the capture stops being a snapshot dumped at the end and becomes a stream
written as it happens. Almost everything below came from watching the bus with a throwaway
probe *before* writing any of it, which was the right order: two of these findings would have
produced a malformed capture if they had been discovered afterwards.

### The finding that shaped the whole design: creation precedes the load event

The PRD flagged "scenario reload timing" as a rabbit hole and said what to do if the ordering
was unreliable — key the boundary on the load event. The ordering turned out to be perfectly
reliable, and *still* wrong for the obvious design, in a way nobody had predicted.

A probe subscribed to all four topics with one global arrival counter, so the numbers below
are true interleaving as delivered to a subscriber, not inference from timestamps.

**Bring-up:**

```
seq 1        engine_initialized   uninitialized->initialized
seq 6        scenario_unloaded    ""                          <- empty name: bring-up noise
seq 7..48    entity_created x 42                              <- the burst
seq 49       scenario_loaded      "Atacama Air Defense"
seq 62       engine_started       idle->running               (11 samples already through)
```

**Teardown, at the end of the same run:**

```
seq 136033   engine_stopped       running->idle    simT=200.05
seq 136034..136052  entity_deleted x 19   reason=scenario_unload   simT=0.0
seq 136053   scenario_unloaded    "Atacama Air Defense"            simT=0.0
seq 136054..136095  entity_created x 42                            simT=0.0
seq 136096   scenario_loaded      "Atacama Air Defense"            simT=0.0
             then 378 entity-state samples, all simT=0.0
```

**`scenario_loaded` is a completion announcement, not a start.** The engine materialises the
entities first and says so afterwards. That is uniform — first load and every reload.

The consequence is sharp. Close a segment on the unload and open one on the load, with nothing
in between, and 42 `entity_add` records fall outside any segment — a file the format spec's own
§7 calls malformed, produced by a producer that was following the requirement literally.

**Resolved with a staging area.** The close still keys on `scenario_unloaded`; the open still
keys on `scenario_loaded`; roster records arriving between them wait and flush into the segment
that opens next. A `sample` never waits — one arriving with no segment open forces the segment
open immediately, which is also what bounds the staging area in practice: it can only ever hold
one creation burst, which is entity-count sized. The measured high-water mark on the reference
run was **42**, against a bound of 8192.

The half of the question the PRD actually asked has a clean answer too: **no sample of an
outgoing run ever arrives after that run's `scenario_unloaded`.** The sample count was
identical at `engine_stopped`, at `scenario_unloaded` and at `scenario_loaded` — 131 861 in
all three. That direction of the boundary is exact.

### An ordinary run contains two segments

Because the engine's stop path unloads *and reloads*, a single run of one scenario produces:

| segment | what it is | samples | `sim_time_s` range | closed by |
|---:|---|---:|---|---|
| 0 | the run | 131 772 | 0.05 → 200.05 | `scenario_unloaded` |
| 1 | the teardown reload | 378 | 0.0 → 0.0 | `host_lost` |

This is not a producer artifact and it is not a defect — it is what the bus published. It is
recorded faithfully and `docs/capture-format-v1.md` §16 now warns a reader about it, because
"one run, one segment" is the natural assumption and it is wrong here.

It also means the reload acceptance criterion is met by an ordinary run. No operator reload,
and no scenario-command harness, was needed to demonstrate two segments — which was a real
saving, because driving a reload would have meant publishing on `sim/scenario/command`, and the
control direction is out of scope for v1.

### ADR-6 proved end-to-end, in a real file, at last

M4 could not show a second occupancy: its budget stopped recording at t ≈ 130 s and the
teardown burst is at t ≈ 200 s, and there were no `entity_add` records to bracket it with
anyway. Both gaps are closed. From the reference capture:

```
line 15       entity_add     RedUAV_N_01  occupancy=1  sim_time_s=0                    segment=0
              ... 2956 samples at occupancy 1 ...
line 110475   entity_remove  RedUAV_N_01  occupancy=1  sim_time_s=149.44999999999973   segment=0
                                                       reason="destroyed"
line 131969   entity_add     RedUAV_N_01  occupancy=2  sim_time_s=0                    segment=1
              ... 9 samples at occupancy 2 ...
```

A name **killed** mid-run and re-created at teardown, each tenure bracketed by its own records,
with samples under both. Across the file: 90 distinct names, **132 distinct (name, occupancy)
pairs**, 42 names reaching occupancy 2, 378 samples carried under a second occupancy. The
conformance reader — which enforces "no sample after its own occupancy's `entity_remove`"
independently, from the spec alone — reports CONFORMS.

### The heartbeat, and a guess that measurement corrected

BTB-CX-3 wanted a "bounded, documented" host-loss window and the PRD deferred the number to
M5. It is **3.0 s**, and it is derived rather than picked.

`sim/engine/state` is the signal, not `sim/entity/state`. Entity state goes silent legitimately
at every unload, so its silence means nothing; engine state publishes **through idle frames** —
4 017 messages across a 200 s run that was only running for part of it — so *its* silence is
evidence.

| | `Atacama Air Defense` (42 entities) | `Outback Kamikaze Swarm` (126) |
|---|---:|---:|
| engine-state messages | 4 017 | 617 |
| nominal period | ~51 ms (19.5/s) | ~51 ms |
| **largest inter-arrival gap** | **548 ms** at scenario load | 408 ms at scenario load |
| gaps over 150 ms, whole run | 2 | 7 (mid-run jitter to 305 ms) |

**The load stall does not scale with entity count.** The 126-entity scenario stalled *less* at
load than the 42-entity one. That was worth measuring rather than reasoning about: a window
derived by scaling the reference stall by entity count would have been three times too
generous for no reason. 3.0 s is 5.5× the largest gap seen anywhere and ~59 heartbeat periods.

Also worth recording: `SubscriptionOptions` carries an `activityThresholdS` that defaults to
**30 s**, and `Statistics::SubscriberActivity` exposes `timeSinceLastActivityS` and `isActive`
per subscription. That is the platform's own liveness notion and it was tempting to just use
it — but 30 s is two orders of magnitude too slow for a campaign that runs 20+ times
unattended, and the counter is per-subscription rather than per-host. Counting arrivals
ourselves is three lines and answers the actual question.

**Measured in the acceptance run:** the simulator was hard-killed with `taskkill /F` mid-run,
and the bridge declared host loss at **3.0075 s**, closed segment 0 with `reason: "host_lost"`
at the last real simulation time (33.4 s — no clock reset involved, because the engine never
got to publish one), wrote the trailer, closed the file and exited 0. The capture conforms.

### The handler cost, finally measured

BTB-BP-1 has always required handler time to be "bounded and **measured**", and the PRD's own
quality-gate notes flagged the p95 target as an unvalidated guess with the suggestion to
instrument it at M5 — "when the handler finally has a writer to hand off to and the number
means something". It does now:

```
entity-state  n=132150  p50<=1us  p95<=5us  p99<=10us  max=160us
entity-event  n=222     p50<=5us  p95<=10us p99<=20us  max=22us
```

against targets of p50 < 20 µs, p95 < 100 µs, p99 < 500 µs. **Roughly twenty times inside the
p95 target.** The handler does a map lookup, a roster update, a `StreamValueMap` copy and a
deque push, and that is genuinely all it does. The 160 µs maximum is a single outlier and is
still comfortably inside the p99 target.

Reported from a log-spaced histogram of two `steady_clock` reads per message, so percentiles
are upper bounds of a bucket rather than interpolated values — a histogram does not hold the
precision that interpolating would imply.

### Overload, and the reserve earning its keep

`--queue-size 4` on the reference scenario, deliberately absurd:

```
writer queue samplesDropped=2520 eventsDropped=0  (capacity 4+1024, policy drop_newest)
records 32149 written (segments=2 samples=32009 entity_add=88 entity_remove=46)
trailer drops {"samples_not_recorded":2520,"events_not_recorded":0,...}
```

**2 520 samples lost and not one roster or segment record.** The queue reserves 1024 slots that
only structural records may use, precisely so overload costs data and never structure — the
reasoning being that a lost sample loses a data point, while a lost `entity_created` orphans
every subsequent sample for that name and turns a bounded loss into an unbounded one. It was
an argument when it was written; it is a measurement now. The capture still contains two
correct segments and conforms.

This is also the first capture in which `drops.samples_not_recorded` carries a real number.
M4 made it structurally `0` on purpose — the buffer filling *was* the end of recording — and
reserved the field for exactly this. The name and meaning carried over unchanged.

### Attaching late, and a consequence that was not obvious

Bridge started **19 s after** the simulator, 60 s run:

```
attached_mid_run  true
picture  orphaned=33971  deleteOfUnknown=42
records  1138 written (segments=2 samples=1084 entity_add=45 entity_remove=3)
```

`attached_mid_run` is `true` and the orphan count is enormous, which is the signature M3
measured and M4 made causal rather than clock-derived. Started before the simulator, the same
build reports `false` and `samples_orphaned: 0`. Both orders work with no operator
intervention (BTB-CX-2).

The consequence worth writing down: **45 `entity_add` records but only 3 `entity_remove`.** The
teardown publishes `entity_deleted` for all 42 entities the late bridge never saw created, and
those land in `deleteOfUnknownEntity` rather than becoming records. That is correct — emitting
an `entity_remove` for an occupancy no `entity_add` ever opened would produce exactly the
malformed file the conformance reader rejects — but it means **`entities_added` and
`entities_removed` in a late-attached capture do not balance, and should not be expected to.**
A reader that treats an imbalance as corruption would be wrong.

### The clock-reset trap, now visible in a real file

M1 predicted it, M3 quantified it, and M5 is where it lands in an artifact somebody else will
read. In the reference capture:

```
segment_open   segment=0  sim_time_s=0                      <- correct, the clock IS zero at load
   ... 131 772 samples, sim_time_s 0.05 .. 200.05000000001124 ...
segment_close  segment=0  sim_time_s=0  reason=scenario_unloaded
```

**A segment whose samples ran to t = 200.05 is closed by a record stamped 0.0.** That is
faithful — the engine reset the clock before publishing the unload, and ADR-3 says a record
carries the time its cause carried, not a time the producer computed. Synthesising a
plausible-looking value here would have been the recorder inventing data to spare the reader a
surprise, which is the one thing a recorder must not do.

So the surprise is documented instead. §5.1 of the format spec now states it for
`segment_close` as well as `entity_remove`, and adds the rule that saves the reader an
afternoon: **a segment's time extent is `[first sample, last sample]`, never
`[segment_open, segment_close]`.** On a reloaded scenario both boundary records read `0.0`, so
computing a duration from them gives zero for a run of any length.

### Smaller things

- **`FIFO_DROP` with queue 1024 lost nothing**, exactly as `KEEP_LATEST` with queue 100 lost
  nothing at M3. Another OQ-4 data point at the reference load, and still not a resolution —
  it is a measurement of headroom, and M6's overload is what will actually discriminate.
- **The bring-up `scenario_unloaded` with an empty scenario name is real and arrives on every
  run** — `unload noise ignored=1` in every summary. M1 predicted a naive implementation would
  emit an unnamed segment from it; the producer ignores an unload with an empty name outright.
- **`sim_time_s` is written as the JSON token `0`, not `0.0`,** for every teardown record.
  Consistent with §8.3's warning that a `double` may be written without a fractional part, and
  worth seeing on a boundary record rather than only on a round altitude.
- **132 150 samples this run against M3's 132 188** for the same scenario and duration. Normal
  run-to-run variation of a wall-clock-paced host, and exactly the effect §14 tells EXT-17 to
  expect (and PRD rev 6 correctly attributes to `n8ro-sim-local` rather than to the platform).
- **The staging area's high-water mark was 42 and the queue's was 42**, on a queue of 8192.
  At the reference rate the writer keeps up completely; the queue is sized for a burst that
  did not happen. That is the right way round, and M6's overload is where it gets tested.

## M6 — The referee, and what re-measuring R7 actually found

Two halves. The referee was the straightforward one: three condition kinds, a declaration file,
and the same evaluation engine driven from a live bus or from a stored file. The measurement
half was not straightforward at all, and it changed what we believe about R7.

### Live verdicts and replay verdicts are byte-identical

BTB-REF-4's acceptance criterion is that a live run and an offline re-judgement of the same
records produce identical verdicts. They do, and the check is a hash rather than an inspection:

```
live    1718 bytes  7 lines  sha256 dca9c5fe63587cc6ad53ce42b91a05bb78790151bbdfd137fe6829e398652faa
replay  1718 bytes  7 lines  sha256 dca9c5fe63587cc6ad53ce42b91a05bb78790151bbdfd137fe6829e398652faa
```

That is true **by construction rather than by testing**, which is the part worth keeping. There
is one `Referee` class and one set of deciding rules; the two paths differ only in a
`FieldSource` that reads a named field out of either a decoded `StreamValueMap` or a parsed
`sample.fields` object. Nothing about proximity, containment or terminal state is written
twice, so the two paths cannot drift.

It also depends on two smaller things being right, and both were designed for rather than
discovered:

- **Floats survive the round trip exactly.** `std::to_chars` shortest round-trip out,
  `strtod` back in, identical bit pattern — so the distance computed live and the distance
  computed from the file are the same double, not merely close. OQ-5 was settled at M1 for the
  capture's sake; this is the second thing it bought.
- **End-of-run verdicts have to be anchored somewhere both paths can reach.** They are stamped
  from the last *data* record — the last `sample`, `entity_add` or `entity_remove` — rather
  than from the last record of any kind. A replay reading the file also sees the `segment_close`
  and the `trailer`, and anchoring on those would have put a different `sim_time_s` in the
  replayed verdict. One line, and it is the difference between identical and nearly identical.

Replay of a 64 MB, 132 454-line capture takes **1.02 s** against BTB-REF-4's target of under
60 s for a ten-minute capture.

### The verdicts, and what a verdict is worth

The reference run's seven, in the order they were decided:

```
airfield-reaches-operational    met   t=0.05     phase=operational
red-leader-reaches-airfield     met   t=149.05   distance_m=2999.9981116642175  within_m=3000
red-leader-enters-base-circle   met   t=149.05   distance_from_centre_m=2999.95  radius_m=3000
red-leader-crosses-corridor     met   t=149.05   polygon
red-leader-is-destroyed         met   t=149.45   removal_reason=destroyed
command-centre-is-destroyed     NOT MET
bases-implausibly-close         NOT MET
```

`RedUAV_N_01` closes to 2 999.998 m of the airfield — two millimetres inside a 3 000 m
threshold — and is shot down 0.4 s later. The narrative reads itself out of the verdict file,
and every number in it can be checked by hand against samples the capture names: a proximity
verdict carries each entity's occupancy and each sample's own `sim_time_s`, which is exactly
enough to find the two causing records.

**The two not-met verdicts are the ones that make the file trustworthy.** Without them, a
condition that was never evaluated and a condition that was evaluated and never satisfied look
identical — and BTB-REF-2 is right to insist that silence is not an answer.

### The boundary case is not reachable, and that is worth knowing

The PRD's test plan asks for the boundary case, "exactly at the threshold". Writing it found
that **a caller cannot arrange one.** A geodetic distance is a computed double: two points a
nominal 1 000 m apart come out a fraction of a millimetre off 1 000, so `within_m: 1000` does
not match them and no round threshold ever lands on the boundary.

The documented `<=` therefore matters for **reproducibility** — the same input always gives the
same answer, on every host and every build — and not because anyone will hit it deliberately.
The test asserts it by computing the distance first and using that value as the threshold,
which is the only honest way to test the comparison rather than approximately test it.

Region containment is different and the boundary there *is* reachable, because a region is
declared rather than computed: a point exactly on a circle's edge or a polygon's edge or
vertex is **inside**. The polygon case needed explicit handling — ray casting alone gives an
arbitrary answer on a vertex, depending on which way the parity happened to fall.

### The geodetic method, and why not the obvious one

Positions go to earth-centred, earth-fixed coordinates on WGS-84 and distance is the
straight-line Euclidean distance in metres. Haversine was the obvious choice and is wrong for
the question being asked: it ignores altitude, and two aircraft stacked 6 km apart vertically
are not close. Vincenty answers the surface question, iterates, and famously fails to converge
near-antipodally. ECEF is closed-form, has no convergence case, handles altitude naturally, and
is reproducible from the formulae in `src/Geodesy.h` by anyone with a calculator — which is
what BTB-REF-3's "reproducible by a third party" actually asks for.

Checked against a published figure rather than against our own output: one degree of latitude
at the equator comes out at 110 574 m.

### R7 re-measured, and the reference turned out to be lossy too

This is the finding that changed a belief rather than confirming one.

M4 measured our capture against the simulation host's own per-entity dump and found 19 samples
(0.019 %) absent with every platform counter reading zero — 18 of them in a single frame. That
became R7: *a capture is not a guaranteed-complete transcript, and no counter says so.* M6 was
tasked with re-running it under the 126-entity overload scenario, on the stated hypothesis that
three times the rate would provoke the mechanism and make it attributable.

Both runs, compared per `(entity, sim_time_s)` against the host's own record:

| | reference, 818/s | overload, 2 487/s |
|---|---:|---:|
| samples in the compared window | 131 744 | 135 581 |
| **absent from our capture** | **30 (0.023 %)**, all in **one** frame | **0** |
| **absent from the host's own dump**, though present in our capture | 30, in three frames | 203, in four frames |
| what every counter reported | zero | zero |

Three findings, and the third is the one that matters.

**The hypothesis was wrong.** Three times the rate did not provoke it; the overload run's
capture was complete by the host's own account across 135 581 samples. Whatever the mechanism
is, throughput is not the trigger. Worth recording as plainly as the finding itself — M6 was
commissioned to provoke it and instead falsified the reason for expecting to.

**The loss is frame-shaped, and reproducible in shape if not in place.** Where our capture is
short it is short by most of one frame — 30 of that frame's 39 samples at t = 73.10. M4 saw
18 of 19 in one frame. Whatever drops them drops a batch.

**And the host's own dump loses whole frames too.** It is missing samples our capture contains:
30 at the reference rate across three frames, 203 under the overload across four. That is the
same shape, in an artifact written **inside the host process**, with no bus, no subscription
and no consumer anywhere in its path.

That reframes R7 rather than closing it:

- The comparison bounds our completeness **from one side only**. The host's dump is not ground
  truth, so "30 absent" is an upper bound on the disagreement between two lossy artifacts, not
  a measurement of our loss.
- A frame-shaped gap appearing in an in-process writer is evidence that the mechanism sits
  **upstream of any consumer**. No subscription policy, queue size or backpressure choice can
  affect something that also happens to a file writer inside the publisher.
- The honest statement to EXT-17 is unchanged and now better founded: all-zero counters mean
  nothing the platform counts was lost, not that nothing was lost — and you cannot establish
  the difference from the host's own record either. §14 of the format spec carries all of it.

`tests/publisher-compare/compare.py` is the tool, kept in the repository rather than
reconstructed each time, and it now reports loss in **both** directions by frame, because the
direction that surprised us was the one M4 had not thought to print.

### OQ-4, resolved

**`FIFO_DROP` with a bus-side queue of 1024.** Three legs:

- **`BLOCK` is rejected on principle, and no measurement could overturn it.** A recorder that
  stalls the bus changes the run it is recording. ADR-4 says so, [S2] reaches the same
  conclusion independently from the downstream side (PRD rev 6), and §14 of the format spec
  now promises consumers in writing that this producer never blocks. Testing it would mean
  deliberately building a producer that violates its own published contract.
- **`FIFO_DROP` at 1024 is sufficient.** Zero bus-side drops on the overload scenario at
  2 487 samples/s — 136 000 samples, `dropped_by_backpressure: 0`. The internal queue's
  high-water mark was **54 of 8 192**.
- **The residual unexplained loss does not bear on the choice.** The PRD said OQ-4 could not
  honestly be resolved while a loss path existed that no counter reports. That path still
  exists, but it is now known to affect an in-process consumer with no subscription at all,
  so no backpressure policy can be implicated in it.

The overload scenario is not, on this design, an overload. The only way to make the internal
queue drop anything was `--queue-size 4`, which is three orders of magnitude below the default.

### Smaller things

- **The condition file needed a JSON parser, and replay needed the same one.** Both arrived
  together, which is the argument for `--replay` being cheap: the reader a capture consumer
  needs is the reader a condition file needs.
- **A duplicate object key is rejected rather than resolved.** Last-wins and first-wins are
  both defensible and neither is discoverable by the person who wrote the file twice.
- **The condition loader rejects an empty `conditions` array.** A run that evaluates nothing
  and reports nothing is indistinguishable from one where everything passed, which is the same
  failure mode BTB-REF-1 exists to prevent one level up.
- **This machine's locale really is comma-decimal.** PowerShell reported the replay time as
  `1,02 s`. That is the hazard OQ-5 catalogued, visible in passing — and the reason the capture
  and the verdict file both go through `std::to_chars` rather than the `printf` family.
- **`n8ro-sim-local`'s per-entity dump is ~58 MB for a 60-second 126-entity run**, on top of
  our own 72 MB capture. Running two lossy recorders to check one another is not free.
