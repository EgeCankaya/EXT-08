# EXT-08 — Bus Telemetry Bridge

A standalone C++17 console program that attaches to a running N8RO simulation over the
message bus. The contract is [`docs/prd.md`](docs/prd.md); observations from the bus are in
[`notes.md`](notes.md).

**Status: M1 + M2 + M3 + M4.** The bridge registers the packed schemas, resolves the
entity-state and entity-event topics *from the registry*, subscribes decoded to both,
maintains a roster and a latest-sample map of its own, and — new at M4 — records a run into
a self-describing `n8ro-capture/1` capture file. Once a second it prints the engine state
plus the entity picture and the bus decoder's drop counters.

The capture format is specified in [`docs/capture-format-v1.md`](docs/capture-format-v1.md).
That document is a **cross-repo contract**: EXT-17 gets it and nothing else, and the
conformance reader in `tests/capture-reader/` was written from it alone — it links neither
this program nor the N8RO SDK — so that "complete enough to write a reader from" is a test
rather than a claim.

It does not yet split scenario reloads into separate segments, does not write roster
transitions out as `entity_add` / `entity_remove` records, and does not judge anything —
those are M5 onward, and §16 of the format spec states exactly which of them a current
capture is missing. The bus-side subscription still runs on the lossy `KEEP_LATEST`
default; M6 sets both backpressure boundaries explicitly. The bridge says so in a warning
at startup, and the policy it recorded under goes into every capture's header.

### Two things to know before you compare or trust a capture

**Two live runs of one scenario are not byte-identical, and that is the platform, not the
recorder.** `n8ro-sim-local` paces against the wall clock and skips about 1% of frames — a
different 1% each run — so the two runs are not the same published stream. The recorder
contributes no variation of its own. [`docs/capture-format-v1.md`](docs/capture-format-v1.md)
§14 has the measurement and what to do instead if you are building a determinism self-test.

**All-zero drop counters mean nothing the platform counts was lost — not that nothing was
lost.** Measured against the simulation host's own record, a reference capture held 99 953 of
99 981 published samples; nine of the 28 absent were the record budget cutting the final
frame, and the other 19 (0.019%) are unexplained with every available counter reading zero.
Risk R7 in the PRD, §14 of the spec. Do not read the absence of a message from a capture as
evidence that it never happened.

## Requirements

- Windows 10/11, x64
- N8RO runtime **2.1.328** installed at `C:\N8RO` (SDK component `com.n8ro.dev` 2.1.328)
- Visual Studio 2026 (v18.x) with the C++ desktop workload — toolset **v145**

Nothing else. The program links four import libraries from the release tree and no
third-party dependency.

## Build

From a plain `cmd` prompt:

```cmd
call C:\N8RO\setup.cmd
call C:\N8RO\dev\setup-dev.cmd
cd /d C:\Projects\EXT-08
msbuild n8ro-bridge.sln /p:Configuration=Release /p:Platform=x64
```

Output: `build\x64\Release\n8ro-bridge.exe`.

`setup.cmd` exports `N8RO_RELEASE` and puts `C:\N8RO\bin` on `PATH`; the project reads
`N8RO_RELEASE` for its include and library paths and falls back to `C:\N8RO`. If the release
lives elsewhere, pass it explicitly:

```cmd
msbuild n8ro-bridge.sln /p:Configuration=Release /p:Platform=x64 /p:N8roRelease=D:\N8RO
```

A missing or mistyped release path fails with one named error rather than a wall of missing
includes.

`C:\N8RO\bin` must be on `PATH` at **run** time too, for `n8ro-sim.dll` and friends —
`setup.cmd` does that.

## Run

The bridge is a passive client. Something has to be hosting an engine for it to attach to;
the shipped `n8ro-sim-local.exe` will host one and run a scenario:

```cmd
n8ro-sim-local.exe --scenario "Atacama Air Defense" --model-path C:\N8RO\data\db --run-ms 300000
```

Then, in a second prompt that has also run `setup.cmd`:

```cmd
build\x64\Release\n8ro-bridge.exe ^
    --config      SimEngineClient_SharedMemory ^
    --model-path  C:\N8RO\data\db ^
    --schema-file N8roSimSchema
```

```
[INFO] (n8ro-bridge) creating client: config=SimEngineClient_SharedMemory modelPath=C:\N8RO\data\db schemaFile=N8roSimSchema
[INFO] (n8ro-bridge) message pump started; reporting engine state once a second from the local getters (Ctrl-C to stop)
engine=running      frame=249        simTime=    12.450 scenario=Atacama Air Defense
engine=running      frame=269        simTime=    13.450 scenario=Atacama Air Defense
```

Every printed value is a local read on the client. Nothing on the print path touches the
bus.

### Options

| option | meaning |
|---|---|
| `--config` | client-side engine config entry, e.g. `SimEngineClient_SharedMemory`. Must be a `SimEngineClient_*` entry — a `SimEngineHost_*` one names the wrong side and will not connect |
| `--model-path` | directory holding the schema and instance database, e.g. `C:\N8RO\data\db` |
| `--schema-file` | schema name inside that database, e.g. `N8roSimSchema` |
| `--entity-state-message` | message instance name the entity-state topic is resolved *from*. Default `simEntityStateUpdate`. Optional |
| `--capture-out` | write a capture to this path. Without it the bridge only reports. **M4 only** — M5 replaces it with `--out-dir` / `--run-label` and the naming convention in the PRD |
| `--capture-max-samples` | stop recording after this many accepted samples, then write the file and exit 0. Default `100000`. Optional |

The first three are required; none are compiled in.

**No topic string is hand-written anywhere in this program.** `--entity-state-message`
names a *message instance*, and the topic is read off the schema that name resolves to, so
a topic rename in the database needs no rebuild. The entity-event topic is not even
configurable: it is resolved from the `entity_created` / `entity_deleted` constants in the
SDK's own `EventNames.h`, through the database's event-to-message pairing, to that
message's topic. `EventNames.h` prescribes exactly that chain — "the topic each event
travels on is the Event instance's own `topic` field, not a constant here."

### Startup diagnostics

Registry size and both resolved topics are logged before anything is subscribed:

```
[INFO] (n8ro-bridge) packed schema registry loaded: 35 message schemas
[INFO] (n8ro-bridge) resolved entity-state topic sim/entity/state from message simEntityStateUpdate via the registry - not from a literal (BTB-EP-1)
[INFO] (n8ro-bridge) entity-state schema: name=simEntityStateUpdate topic=sim/entity/state fields=12 schemaHash=2652370635 messageId=1308183250 wireVersion=1
[INFO] (n8ro-bridge) resolved entity-event topic sim/entity/event from the database pairing for entity_created / entity_deleted via message simEntityEvent
```

An empty registry, an unresolvable message name, or a message that resolves but is not
shaped like entity state each produce a named diagnostic and a distinct non-zero exit. The
bridge never proceeds to silent operation (BTB-EP-1).

### Start order

Start the bridge **before** the simulator when you want the roster from scenario load. The
`entity_created` burst is published once, at load; a bridge that attaches after it sees
samples for entities it never saw created, counts them as `orphaned`, and reports a live
count of zero. That is the counter working, not a fault — but it is not the picture you
wanted. Surviving either start order without operator intervention is BTB-CX-2, in M5.

Note also that `n8ro-sim-local.exe` resolves its plugin directory from `N8RO_RELEASE`. Run
it from a shell that has run `setup.cmd`, or the physics plugin will not be found and the
scenario load is *refused* — `Component type 'componentPhysics' has no registered factory`.

### Exit codes

| code | meaning |
|---:|---|
| 0 | clean exit (`--help`) |
| 2 | bad invocation — unknown option, missing value, missing required option |
| 3 | `SimulationEngineClient::create()` returned `nullopt`; all three configuration values are echoed back as resolved |
| 4 | an exception reached `main` — it is caught and logged, never propagated |
| 5 | `DbModel::Open()` failed for the given schema file |
| 6 | `MessageBusPackedSchemaRegistry::loadAllFromDb()` failed |
| 7 | the registry loaded but is **empty** — BTB-EP-1's loud empty registry |
| 8 | no schema registered under the entity-state message name; the registry's own contents are listed back |
| 9 | that name resolved, but the schema does not declare the fields the picture keys on — it is not the entity-state message |
| 10 | the entity-event topic could not be resolved, so no roster could be built |
| 11 | `subscribeByTopic` returned no subscription |
| 12 | the client was created but exposes no message bus |
| 13 | the capture file could not be opened or written |

### Loss reporting

The status line carries **two** independent loss surfaces, because a message lost before it
reaches the decoder is invisible to the decoder:

```
samples=100000 drops=0(hash=0 decode=0 noschema=0)
    busLoss=0(dropped=0 backpressure=0 queueOverflow=0 rateLimit=0)
```

`drops` is `MessageBusPacked::metricsSnapshot()` — the message arrived and could not be
decoded, which is what a schema mismatch looks like. `busLoss` is
`IMessageBus::getStatistics()` — the message never arrived, because the bus discarded it.
Both go into every capture's `trailer.bus_metrics`. Nothing in this program read the second
group before producer 0.4.2, which is why every "0 drops" printed up to M4 was answering only
half the question.

Configuration is where the difficulty lives. A wrong `--schema-file`, for example, names
the file it could not open and then names all three values back:

```
[ERROR] (BinaryDbFileIO) cannot open file: C:/N8RO/data/db/NoSuchSchema/Config/SimEngine/SimEngineClient_SharedMemory.n8ro.instance
[ERROR] (n8ro-bridge) SimulationEngineClient::create returned nullopt; no client was constructed and nothing was subscribed
[ERROR] (n8ro-bridge)   --config      = SimEngineClient_SharedMemory
[ERROR] (n8ro-bridge)   --model-path  = C:\N8RO\data\db
[ERROR] (n8ro-bridge)   --schema-file = NoSuchSchema
```

## Tests

The entity picture (`src/EntityPicture.*`) is a component we own permanently rather than a
shim awaiting an SDK type, so it has tests. They need **no simulator, no bus and no model
database** — they drive the picture by handing it `StreamValueMap`s, which is exactly what
the bus's `DecodedHandler` does, so they exercise the real entry points. They also link no
N8RO import library; headers alone are enough.

```cmd
call C:\N8RO\setup.cmd
cl /std:c++17 /EHsc /W4 /O2 ^
   /I %N8RO_RELEASE%\include\n8ro-core /I %N8RO_RELEASE%\include\n8ro-sim ^
   /Fe:entity_picture_test.exe ^
   tests\entity-picture\entity_picture_test.cpp src\EntityPicture.cpp
entity_picture_test.exe
```

Exit code 0 if every check passes, 1 otherwise with each failure named. 62 checks covering
occupancy lifecycle (ADR-6), orphan counting, verbatim reasons and payloads, absent-field
accounting, deterministic ordering, the bounded event log, and concurrent handler/snapshot
traffic.

The suite's own adequacy is checked by mutation: deliberate defects introduced into
`EntityPicture.cpp` must make it fail. That is worth re-running when the picture changes —
it is how the "stale sample survives a re-creation" gap was found, which every other test
had been passing over.

### Capture conformance reader

`tests/capture-reader/` is a reader for the capture format, **written from
[`docs/capture-format-v1.md`](docs/capture-format-v1.md) and not from this program's source.**
It links neither the bridge nor the N8RO SDK — standard library only — which is what makes
BTB-CAP-5's "the spec is complete enough to write a reader from" a test rather than a claim.
Every rule it enforces cites the spec section it came from.

```cmd
cl /std:c++17 /EHsc /W4 /O2 /Fe:capture_reader.exe tests\capture-reader\capture_reader.cpp

capture_reader.exe captures\capture-atacama-000.n8rocap.jsonl --spec docs\capture-format-v1.md
```

```
  format n8ro-capture/1
  records  header=1 segment_open=1 segment_close=1 entity_add=0 entity_remove=0 sample=100000 verdict=0 trailer=1
  sim_time 0.05 -> 129.9 s
  entities 77 distinct names, 77 distinct (name, occupancy) pairs
  schema   simEntityStateUpdate (sim/entity/state): 12 declared, 11 ever published
           declared and NEVER published: activeAnimation  (absent from every sample, present in header.schemas - spec 8.2)
  clock    no wall-clock-shaped value found (spec 1, 14)
  version  specification title and header agree: n8ro-capture/1

RESULT: CONFORMS to n8ro-capture/1
```

Exit 0 if the capture conforms, 1 if it does not with every failure named by line and spec
section, 2 on a usage or IO error.

A reader that has never rejected anything has not been shown to work, so its own adequacy is
mutation-checked the same way the entity picture's suite is:

```cmd
python tests\capture-reader\mutate.py ^
    captures\capture-atacama-000.n8rocap.jsonl build\tests\capture_reader.exe docs\capture-format-v1.md
```

Sixteen deliberate defects — wrong field order, an undeclared field, a sample outside a
segment, a miscounted trailer, a truncated file, a record after the trailer, an injected
timestamp, CRLF endings, an unknown `format_version`, a sample after its own occupancy's
removal — **16 caught, 0 survivors.**

### Comparing a capture against the publisher

`n8ro-sim-local` writes its own per-entity JSONL under `test_artifacts/` in its working
directory. That is the publisher's own account of what it published, independent of our bus,
our subscription and our code, and it is the only way to answer "did we record everything?"
without trusting the counters we are trying to check. It is how R7 was measured — see the M4
follow-up in [`notes.md`](notes.md) for the method and the numbers.

## Determinism probe

`tests/float-format/float_format_probe.cpp` settles which double-to-text format is
round-trip exact **and** locale independent on this toolchain. It is standalone:

```cmd
cl /std:c++17 /O2 /EHsc tests\float-format\float_format_probe.cpp
float_format_probe.exe
```

Exit code 0 if at least one candidate passes both axes. The result and what it means for
the capture format are in [`notes.md`](notes.md).

## Layout

```
docs/prd.md                              the contract — 27 FRs prefixed BTB-
notes.md                                 what the bus actually carries (graded deliverable)
src/main.cpp                             CLI, wiring, the once-a-second report
src/TopicResolution.{h,cpp}              BTB-EP-1 — schemas, and both topics from the registry
src/EntityPicture.{h,cpp}                BTB-EP-3/EP-4 — the roster and the latest-sample map
src/ExitCodes.h                          one table of process exit codes
src/CaptureFormat.{h,cpp}                BTB-CAP-1/CAP-4 — the `n8ro-capture/1` serialiser
src/Json.{h,cpp}                         JSON escaping and the round-trip-exact float format
src/SampleBuffer.{h,cpp}                 M4's bounded record buffer — M5 replaces it
tests/entity-picture/                    unit tests for the picture — no simulator needed
tests/capture-reader/                    conformance reader, written from the format spec alone
tests/float-format/                      the OQ-5 determinism probe
n8ro-bridge.sln / .vcxproj               Release|x64, v145, stdcpp17
```

## Recording a capture

```cmd
:: bridge first - the entity_created burst fires once, at scenario load
build\x64\Release\n8ro-bridge.exe ^
    --config      SimEngineClient_SharedMemory ^
    --model-path  C:\N8RO\data\db ^
    --schema-file N8roSimSchema ^
    --capture-out captures\capture-atacama-000.n8rocap.jsonl ^
    --capture-max-samples 100000

:: then, in a prompt that has also run setup.cmd
n8ro-sim-local.exe --scenario "Atacama Air Defense" --model-path C:\N8RO\data\db --run-ms 200000
```

The bridge records until the sample budget is reached, then unsubscribes, stops the pump,
writes the whole file from its own thread, and exits 0. **The budget is what ends the run** —
there is no signal handling until M7, so a bridge interrupted before the budget writes no
file. 100 000 samples is about 130 s of the reference scenario and about 48 MB of JSON Lines.

Two things about the current recording strategy, both deliberate and both temporary. The
subscription handler stays a courier: it copies each accepted sample into a bounded buffer
and returns, and nothing reads that buffer until recording has stopped, so all IO, formatting
and float conversion happen on our own thread. That trades memory for not yet having M5's
writer thread and bounded queue, which is where BTB-BP-1 and BTB-BP-4 actually land. And
`--capture-out` names a file directly; M5 replaces it with `--out-dir` / `--run-label` and
the naming convention in the PRD.

## Notes on handling captures

Anything this program eventually records inherits the classification of the scenario it
records — entity names, positions, teams and outcomes originate from an Arkheon
Technologies proprietary platform. Treat captures accordingly.

This install is missing `C:\N8RO\data\geoid\earth_geoid_05m_g.n8grid`, so runs log a
terrain-datum warning and fall back to the ellipsoid. Altitudes observed on the bus are
ellipsoidal, not orthometric.
