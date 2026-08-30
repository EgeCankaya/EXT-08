# EXT-08 — Bus Telemetry Bridge

A standalone C++17 console program that attaches to a running N8RO simulation over the
message bus. The contract is [`docs/prd.md`](docs/prd.md); observations from the bus are in
[`notes.md`](notes.md).

**Status: M1 through M5.** The bridge registers the packed schemas, resolves **four** topics
*from the registry* — entity state, entity events, scenario events and engine state —
subscribes decoded to all of them, maintains a roster and a latest-sample map of its own, and
streams a self-describing `n8ro-capture/1` capture through a writer thread behind a bounded
queue. It splits a scenario reload into separate segments, writes the roster's transitions out
as `entity_add` / `entity_remove` records, detects the simulator disappearing and closes the
capture cleanly, and works whichever of the two processes starts first. Once a second it prints
the engine state, the entity picture, the capture's progress and both bus loss surfaces.

The capture format is specified in [`docs/capture-format-v1.md`](docs/capture-format-v1.md).
That document is a **cross-repo contract**: EXT-17 gets it and nothing else, and the
conformance reader in `tests/capture-reader/` was written from it alone — it links neither
this program nor the N8RO SDK — so that "complete enough to write a reader from" is a test
rather than a claim.

It does not judge anything yet — the referee, the condition file, `verdict` records and
`--replay` are M6, and §16 of the format spec states exactly what a current capture is missing.
There is no signal handling yet either: an M5 run ends when the simulation host stops
publishing, or on a record budget if you set one. Ctrl-C with a clean drain is M7.

Both backpressure boundaries are now set explicitly (BTB-BP-3, BTB-BP-4) — see
[Backpressure](#backpressure) below for the values and why they are those values.

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
the shipped `n8ro-sim-local.exe` will host one and run a scenario.

Start the **bridge first** — it waits for a host, and the `entity_created` burst that fills the
roster is published once, at scenario load:

```cmd
build\x64\Release\n8ro-bridge.exe ^
    --config      SimEngineClient_SharedMemory ^
    --model-path  C:\N8RO\data\db ^
    --schema-file N8roSimSchema ^
    --out-dir     captures
```

Then, in a second prompt that has also run `setup.cmd`, and **from a scratch directory** —
`n8ro-sim-local` drops a `test_artifacts\` tree into whatever directory it is launched from:

```cmd
n8ro-sim-local.exe --scenario "Atacama Air Defense" --model-path C:\N8RO\data\db --run-ms 200000
```

```
[INFO] (n8ro-bridge) message pump and writer thread started. Waiting for the simulation host; no start order is required (BTB-CX-2). Host loss is declared after 3.000000 s without an engine-state message (BTB-CX-3)
[INFO] (n8ro-bridge) waiting for a simulation host on sim/engine/state; the bridge is subscribed and will begin capturing the moment one publishes
[INFO] (n8ro-bridge) attached: the simulation host is publishing engine state
[INFO] (n8ro-bridge) capture opened: captures\capture-atacama-air-defense-000.n8rocap.jsonl format n8ro-capture/1 producer 0.5.0 attached_mid_run=false
engine=running    frame=249    simTime=  12.450 scenario=Atacama Air Defense live=42  names=45  samples=10098 orphaned=0
    capture=10145 records (seg=1 samples=10098 add=45 rm=2) queue=0/9216 drops=0/0
    busLoss=0(bp=0 qo=0 rl=0) decode=0(hash=0 fail=0 noschema=0)
```

Every value on the engine line is a local read on the client; nothing on the print path
touches the bus. When the simulator exits, the bridge notices within 3 s, closes the capture
with `end_reason: host_lost`, prints a run summary and exits 0.

### Options

| option | meaning |
|---|---|
| `--config` | client-side engine config entry, e.g. `SimEngineClient_SharedMemory`. Must be a `SimEngineClient_*` entry — a `SimEngineHost_*` one names the wrong side and will not connect |
| `--model-path` | directory holding the schema and instance database, e.g. `C:\N8RO\data\db` |
| `--schema-file` | schema name inside that database, e.g. `N8roSimSchema` |
| `--out-dir` | existing, writable directory for the capture. Validated at startup — the bridge never creates it, because a mistyped path would then look like success |
| `--run-label` | label for this run. Defaults to the next unused zero-padded ordinal in `--out-dir`. Optional |
| `--entity-state-message` | message instance name the entity-state topic is resolved *from*. Default `simEntityStateUpdate`. Optional |
| `--engine-state-message` | message instance name the host-loss heartbeat is resolved *from*. Default `simEngineState`. Optional |
| `--queue-size` | handler-to-writer queue bound, in sample records. Default `8192`. Optional |
| `--overflow-policy` | `drop_newest` (default) or `drop_oldest`. Optional |
| `--capture-max-samples` | stop after this many `sample` records and close with `end_reason: size_limit`. Default `0`, meaning no bound — an M5 run ends on host loss. Optional |

`--config`, `--model-path`, `--schema-file` and `--out-dir` are required; none are compiled in.

### The capture file's name

```
<out-dir>/capture-<scenario>-<run-label>.n8rocap.jsonl
```

`<scenario>` is the scenario name **as the platform reported it**, lowercased with runs of
anything outside `[a-z0-9]` collapsed to a single hyphen — so `Atacama Air Defense` gives
`capture-atacama-air-defense-000.n8rocap.jsonl`.

`<run-label>` defaults to the lowest zero-padded ordinal not already present in `--out-dir` for
that scenario. **It is never a timestamp.** Campaign tooling addresses runs by path, and a
wall-clock name makes two runs of the same configuration unaddressable as a pair — the same
reasoning that keeps wall-clock out of the capture itself (ADR-3).

The file is opened when the scenario name becomes known rather than at startup, since the name
is part of it. A bridge started before the simulator therefore creates nothing until a host
appears, which is also the honest behaviour: no host, no run, no artifact.

### Backpressure

There are **two** boundaries, and the PRD requires both to be an explicit decision rather than
an inherited default. Neither uses `BLOCK`.

| boundary | value | why |
|---|---|---|
| bus → handler | `FIFO_DROP`, queue **1024** | The `SubscriptionOptions` default is `KEEP_LATEST` with queue 100. For a recorder that is precisely wrong: it discards the *older* of two messages, which is the one already part of the run's history. 1024 is ~1.25 s of headroom at the reference scenario's 818 packets/s, against ~120 ms for the default. Provisional — M6 confirms it under overload (OQ-4) |
| handler → writer | `drop_newest`, **8192** sample records **+ 1024 reserved** | Drop-oldest is `KEEP_LATEST`'s mistake one thread later. 8192 records is ~16 MB and 10 s of headroom at the reference rate. The reserve is only usable by roster and segment records, so overload costs data and never structure — measured: at `--queue-size 4` a reference run dropped 2 520 samples and **zero** events, and the capture still contained two correct segments |

**`block` is not offered at either boundary.** Blocking the bus stalls the publisher and changes
the run being recorded; blocking the internal queue blocks the handler, which stalls the bus
delivery thread — the same perturbation by a longer route. §14 of the format spec states to
consumers, in writing, that this producer never blocks. Asking for `--overflow-policy block`
prints that reasoning rather than a bare rejection.

Overflow at the internal boundary is counted into `trailer.drops.samples_not_recorded` and
`trailer.drops.events_not_recorded`. Loss is counted, never silent.

### Host loss

The bridge treats the simulator appearing, disappearing and dying as ordinary states rather
than errors (BTB-CX-2, BTB-CX-3).

Host loss is **no `sim/engine/state` message for 3.0 s**. That topic is the heartbeat rather
than `sim/entity/state`, because entity state goes silent legitimately at every scenario unload
while engine state publishes through idle frames — 4 017 messages across a 200 s run that was
only running for part of it.

The 3.0 s is derived, not picked: the largest inter-arrival gap measured across two full
bring-up/load/run/teardown cycles on two scenarios was **548 ms**, at scenario load, so the
window is 5.5× the worst observed stall and about 59 heartbeat periods. It is deliberately far
below `SubscriptionOptions::activityThresholdS`, whose 30 s default is the bus's own
idle-subscription notion and is far too slow for a campaign runner. Measured end to end:
hard-killing the simulator mid-run, the bridge declared loss at 3.0075 s and left a conformant
capture ending in a `host_lost` trailer.

On host loss the bridge closes the capture and exits 0 — the capture is complete, and host loss
is a handled state rather than a failure.

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

**Either order works with no operator intervention** (BTB-CX-2). But they do not produce the
same capture, and the file says which you got.

Start the bridge **before** the simulator when you want the roster from scenario load. The
`entity_created` burst is published once, at load; a bridge that attaches after it sees samples
for entities it never saw created and counts them as `orphaned`. The capture records that
causally rather than by a clock: `header.attached_mid_run` is `true` and
`trailer.drops.samples_orphaned` is large. Measured — bridge 19 s late on a 60 s run:
`attached_mid_run: true`, 33 971 orphans. Bridge first: `false`, zero.

One consequence of a late attach that is easy to misread: **`entities_added` and
`entities_removed` will not balance.** The teardown publishes `entity_deleted` for entities the
bridge never saw created, and those cannot become `entity_remove` records without producing a
file the format calls malformed, so they are counted as `deleteOfUnknownEntity` instead. An
imbalance in a capture with `attached_mid_run: true` is expected, not corruption.

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
| 14 | the scenario-event topic could not be resolved, so reloads could not be told apart (BTB-CX-4) |
| 15 | the engine-state topic could not be resolved, so host loss could not be detected. The bridge refuses to run rather than block indefinitely on a dead bus (BTB-CX-3) |
| 16 | `--out-dir` is missing, is not a directory, or is not writable |

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

capture_reader.exe captures\capture-atacama-air-defense-000.n8rocap.jsonl ^
    --spec docs\capture-format-v1.md
```

```
  format n8ro-capture/1
  records  header=1 segment_open=2 segment_close=2 entity_add=132 entity_remove=90 sample=132150 verdict=0 trailer=1
  entities 90 distinct names, 132 distinct (name, occupancy) pairs
  schema   simEntityStateUpdate (sim/entity/state): 12 declared, 11 ever published
           declared and NEVER published: activeAnimation  (absent from every sample, present in header.schemas - spec 8.2)
  clock    no wall-clock-shaped value found (spec 1, 14)
  version  specification title and header agree: n8ro-capture/1

RESULT: CONFORMS to n8ro-capture/1
```

**90 names but 132 (name, occupancy) pairs** is the line to look at. Those 42 extra pairs are
names that lived twice — the engine's teardown reload re-creating the roster — and they are
what makes the capture a test of ADR-6 rather than an assertion of it.

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
docs/capture-format-v1.md                the cross-repo contract EXT-17 is handed
docs/decisions-m5-m7.md                  every judgment call made in M5–M7, and why
notes.md                                 what the bus actually carries (graded deliverable)
src/main.cpp                             CLI, wiring, the lifecycle loop, the run summary
src/TopicResolution.{h,cpp}              BTB-EP-1 — schemas, and all four topics from the registry
src/EntityPicture.{h,cpp}                BTB-EP-3/EP-4 — the roster and the latest-sample map
src/CaptureRecord.h                      what crosses the handler-to-writer boundary
src/RecordQueue.{h,cpp}                  BTB-BP-1/BP-2/BP-4 — the bounded queue, counted overflow
src/CaptureWriter.{h,cpp}                BTB-CX-3/CX-4 — the writer thread and the segment machine
src/CaptureFormat.{h,cpp}                BTB-CAP-1/CAP-4 — the `n8ro-capture/1` serialiser
src/HandlerTiming.{h,cpp}                BTB-BP-1 — how long a handler actually takes
src/Json.{h,cpp}                         JSON escaping and the round-trip-exact float format
src/ExitCodes.h                          one table of process exit codes
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
    --out-dir     captures

:: then, from a scratch directory, in a prompt that has also run setup.cmd
n8ro-sim-local.exe --scenario "Atacama Air Defense" --model-path C:\N8RO\data\db --run-ms 200000
```

The bridge records until the simulation host stops publishing, then closes the capture with
`end_reason: host_lost` and exits 0. There is no signal handling until M7, so **let the host
end the run** rather than interrupting the bridge; `--capture-max-samples` gives you a record
budget if you want a bounded file instead, and closes it with `end_reason: size_limit`.

A 200-second run of the reference scenario produces about **132 000 sample records and 64 MB**
of JSON Lines, at roughly 484 bytes per sample record.

### What you get, and what to check

```
records     132378 written (segments=2 samples=132150 entity_add=132 entity_remove=90 verdicts=0)
writer queue samplesDropped=0 eventsDropped=0  (capacity 8192+1024, policy drop_newest, highWater=42)
entity-state n=132150 p50<=1us p95<=5us p99<=10us max=160us
```

**Two segments from one run is correct**, and it is the first thing to understand about a real
capture. The engine's stop path unloads the scenario and immediately reloads it, re-creating
the whole roster under the same names — so segment 0 is the run, and segment 1 is the teardown
reload, holding the re-created entities at **occupancy 2** and a short tail of samples stamped
`sim_time_s = 0.0`. §16 of the format spec says the same thing to a reader who has never seen
this repository.

That second segment is also where a capture finally demonstrates ADR-6 end to end. In the
reference capture `RedUAV_N_01` is created at occupancy 1, publishes 2 956 samples, is
`destroyed` at `t = 149.45`, and returns at occupancy 2 with 9 more — each tenure bracketed by
its own `entity_add` / `entity_remove`.

### Checking a capture

```cmd
build\tests\capture_reader.exe captures\capture-atacama-air-defense-000.n8rocap.jsonl ^
    --spec docs\capture-format-v1.md
```

Exit 0 and `RESULT: CONFORMS` means the file satisfies every rule the specification states —
including the ones a producer bug would most plausibly break: no `sample` outside an open
segment, no `sample` after its own occupancy's `entity_remove`, `sample.fields` in schema
order, trailer counts agreeing with the records present, and no wall-clock-shaped value
anywhere.

### Demonstrating counted backpressure

Shrink the internal queue until it cannot keep up:

```cmd
build\x64\Release\n8ro-bridge.exe ... --out-dir captures --queue-size 4
```

```
writer queue samplesDropped=2520 eventsDropped=0
trailer drops {"samples_not_recorded":2520,"events_not_recorded":0,...}
```

Samples are lost and counted; **roster and segment records are not**, because the queue
reserves headroom only they may use. The capture still contains two correct segments and still
conforms — overload costs data, never structure.

## Notes on handling captures

Anything this program eventually records inherits the classification of the scenario it
records — entity names, positions, teams and outcomes originate from an Arkheon
Technologies proprietary platform. Treat captures accordingly.

This install is missing `C:\N8RO\data\geoid\earth_geoid_05m_g.n8grid`, so runs log a
terrain-datum warning and fall back to the ellipsoid. Altitudes observed on the bus are
ellipsoidal, not orthometric.
