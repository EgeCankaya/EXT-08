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

Eleven fields. Decoded (shark's `decodedJson`, which sorts keys alphabetically):

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
