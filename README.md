# EXT-08 — Bus Telemetry Bridge

A standalone C++17 console program that attaches to a running N8RO simulation over the
message bus. The contract is [`docs/prd.md`](docs/prd.md); observations from the bus are in
[`notes.md`](notes.md).

**Status: M1 through M7 — complete. Every requirement in the PRD is implemented, and the demo recording is [published as four takes](https://drive.google.com/drive/folders/1L0lPs0wkDA_qGYx8Z0Q8-SMzNrOvLoXN?usp=sharing)** covering all seven BTB-DOC-2 beats. The bridge registers the packed schemas, resolves **four** topics
*from the registry* — entity state, entity events, scenario events and engine state —
subscribes decoded to all of them, maintains a roster and a latest-sample map of its own, and
streams a self-describing `n8ro-capture/1` capture through a writer thread behind a bounded
queue. It splits a scenario reload into separate segments, writes the roster's transitions out
as `entity_add` / `entity_remove` records, detects the simulator disappearing and closes the
capture cleanly, and works whichever of the two processes starts first.

It also **judges**: conditions declared in a JSON file are evaluated against the run, live or
offline against a stored capture, and produce `verdict` records. Live and replay verdicts over
the same run are byte-identical — see [The referee](#the-referee).

The capture format is specified in [`docs/capture-format-v1.md`](docs/capture-format-v1.md).
That document is a **cross-repo contract**: EXT-17 gets it and nothing else, and the
conformance reader in `tests/capture-reader/` was written from it alone — it links neither
this program nor the N8RO SDK — so that "complete enough to write a reader from" is a test
rather than a claim.

A live run ends on host loss, on Ctrl-C with a clean drain, or on a size or record bound if you
set one — see [Bounding a capture](#bounding-a-capture), which is also where the stop-or-rotate
choice lives. Both backpressure boundaries are set explicitly (BTB-BP-3, BTB-BP-4) — see
[Backpressure](#backpressure) for the values and why they are those values.

**`docs/capture-format-v1.md` is frozen.** It is a cross-repo contract consumed by EXT-17;
after the freeze, a change to what it specifies is a version bump and a downstream change, not
an edit.

**What is not here**, stated plainly rather than left to be discovered:

- **A single edited cut of the demo.** The recording is published as its **four source takes**
  rather than one assembled file — see [The demo recording](#the-demo-recording). All seven
  beats BTB-DOC-2 names are on camera and the take-by-take map below says where each one is, so
  the requirement's acceptance criterion is met; what does not exist is a single five-minute
  file with titles between the beats.


### Three things to know before you compare or trust a capture

**Every sample in a capture is as-published, never predicted — and the file says so.** The
platform distinguishes a *published* sample (a state the engine actually produced, carrying the
simulation time it was published at) from a *predicted* one (a published state carried forward,
its position and velocity advanced by arithmetic the engine never performed). A recorder must
use the first; a predicted series is a smooth curve nobody computed. On runtime 2.1.328 there is
nothing to get wrong — `SimulationEngineClient` exposes no prediction accessor, so everything
reaching a subscriber arrived as an engine publication — but the header now records
`sample_form: "published"` (producer 0.8.0) so the answer travels with the file rather than
living here. A capture written before 0.8.0 omits the key; read that as *unknown*, not as
"predicted", and check `producer.version`.


**Two live runs of one scenario are not byte-identical — and the simulation is still
reproducible.** Measured on both hosts: the wall-clock-paced `n8ro-sim-local` skips ~1% of
frames, a different 1% each run, and even the fixed-step headless `n8ro-sim-app` skips ~0.2%.
But comparing *content* rather than bytes on two headless runs stopped at the same frame,
**50 358 of 50 358 samples agree and none differ** — the runs disagree only about which frames
were published.

So a byte comparison of two captures fails, and it is reporting the publication schedule rather
than the simulation. Compare on content: `tests/determinism/compare_captures.py` does it, and
[`docs/capture-format-v1.md`](docs/capture-format-v1.md) §14 has the measurement and the two
traps (`sim_time_s` is not a key, and a frozen-clock segment cannot be aligned at all).

**All-zero drop counters mean nothing the platform counts was lost — not that nothing was
lost.** Measured against the simulation host's own record: at the reference rate a capture was
short by 30 samples in a single frame, with every counter reading zero. Under three times the
load it was complete. And the host's own record loses whole frames too — 203 under the overload
— so it bounds our completeness from one side only and is not ground truth. Risk R7 in the PRD,
§14 of the spec. Do not read the absence of a message from a capture as evidence that it never
happened.

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
[INFO] (n8ro-bridge) capture opened: captures\capture-atacama-air-defense-000.n8rocap.jsonl format n8ro-capture/1 producer 0.9.0 attached_mid_run=false
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
| `--overflow-policy` | `drop_newest` (default) or `drop_oldest`. Either way a `sample` is never permitted to evict a roster or segment record — `drop_oldest` evicts the oldest *sample*. Optional |
| `--capture-max-bytes` | maximum size of **one capture file**, in bytes (BTB-CAP-6). Default `0`, meaning no bound. Minimum 16384 when set. The limit and the action below are written into the capture's own `header`. Optional |
| `--on-size-limit` | what happens on reaching `--capture-max-bytes`: `stop` (default) or `rotate`. See [Bounding a capture](#bounding-a-capture). Optional |
| `--capture-max-samples` | stop after this many `sample` records **across the whole run** and close with `end_reason: size_limit`. Default `0`, meaning no bound — a live run ends on host loss. A record-count safety bound; it always stops, never rotates. Optional |
| `--topic-silence-s` | warn when the **entity-state** topic has decoded nothing for this many seconds **while the engine reports itself running** — the schema-mismatch fault, which is otherwise silent (BTB-OBS-2). Default `10.0`; `0` disables the check. Not applied to the two event topics, which are legitimately quiet, nor to engine-state, whose silence is already host loss at 3.0 s (D-49). Diagnostic only: no capture byte and no exit code depends on it. Optional |
| `--conditions` | JSON file of declared conditions. Without it the bridge records but judges nothing. Optional |
| `--replay` | offline mode: re-judge a stored capture with no simulator, no bus and no client. Requires `--conditions`, and is mutually exclusive with `--config` |

`--config`, `--model-path`, `--schema-file` and `--out-dir` are required for a live run; none
are compiled in. A replay needs only `--replay` and `--conditions`.

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

### Bounding a capture

An unattended run on a finite disk needs a bound, and a capture cut off by ENOSPC halfway
through a line is unreadable by every reader. `--capture-max-bytes` is the bound; `--on-size-limit`
is what happens when a run reaches it (BTB-CAP-6).

```cmd
:: stop at 100 MB with a clean, complete, explicitly-truncated capture
--capture-max-bytes 104857600

:: or keep recording, into numbered continuation files of 100 MB each
--capture-max-bytes 104857600 --on-size-limit rotate
```

| | `stop` (default) | `rotate` |
|---|---|---|
| At the limit | closes the capture and ends the run | closes this file and continues into the next |
| Total disk used | bounded by `--capture-max-bytes` | unbounded — the bound is per file |
| The run's tail | not recorded anywhere | recorded, in later parts |
| Files produced | one | `capture-<scenario>-<label>.n8rocap.jsonl`, then `.part001`, `.part002`, … |

**No line is ever cut.** The bound is checked against a record's exact length *before* the
record is written, and 8 KiB is reserved up front for the `segment_close`, the end-of-run
`verdict` records and the `trailer`. A capture that reached its limit is a complete, valid
file ending in a well-formed trailer carrying `end_reason: size_limit` — which is the whole
point of the requirement, and the difference between an analyst finding a closed capture and
an analyst finding a corrupt one.

**Every part of a rotated set is a complete capture.** Each carries its own `header` with its
own full schema table, its own segments numbered from 0, its own counts and its own trailer.
The conformance reader accepts any part on its own, and so should anything you write. The set
is tied together by three optional keys — `header.part`, `header.continues_from` and
`trailer.continued_in` — documented in [`docs/capture-format-v1.md`](docs/capture-format-v1.md)
§6.7, along with the rules for stitching parts back into one stream. **Segment ordinals restart
in each part**, so anything computed per segment across a set has to key on `(part, segment)`.

The bound in force is recorded in the file, as `header.limits` (spec §6.6):

```json
"limits":{"max_bytes":104857600,"max_samples":0,"on_size_limit":"rotate"}
```

That is there because a capture that stops early and a capture whose run ended look identical
from the outside, and the difference decides whether the analysis you are about to do is valid.
An unbounded capture states that it is unbounded rather than saying nothing.

`--capture-max-samples` is the older, separate, record-count safety bound (D-13). It counts
across the whole run rather than per part, and it always stops. Both bounds appear in
`header.limits`, because either can end a capture and a file that does not say so is the same
silent truncation from a reader's side.

### Backpressure

There are **two** boundaries, and the PRD requires both to be an explicit decision rather than
an inherited default. Neither uses `BLOCK`.

| boundary | value | why |
|---|---|---|
| bus → handler | `FIFO_DROP`, queue **1024** | The `SubscriptionOptions` default is `KEEP_LATEST` with queue 100. For a recorder that is precisely wrong: it discards the *older* of two messages, which is the one already part of the run's history. 1024 is ~1.25 s of headroom at the reference scenario's 818 packets/s, against ~120 ms for the default. Provisional — M6 confirms it under overload (OQ-4) |
| handler → writer | `drop_newest`, **8192** sample records **+ 1024 reserved** | Drop-oldest is `KEEP_LATEST`'s mistake one thread later. 8192 records is ~16 MB and 10 s of headroom at the reference rate. The reserve is only usable by roster and segment records, so overload costs data and never structure — measured: at `--queue-size 4` a reference run dropped 2 520 samples and **zero** events, and the capture still contained two correct segments |

The reserve is two mechanisms, not one, and it holds under **both** policies. The threshold — a
sample is refused above `--queue-size`, a structural record only above `--queue-size + 1024` —
is what stops a sample burst from *filling* the roster records' headroom. Under `drop_oldest`
that is not enough on its own, because an arriving sample does not merely fail to fit: it
chooses a victim, and the front of the queue during a scenario load is the `entity_created`
burst. So `drop_oldest` evicts the oldest **sample**, skipping past any roster or segment
record ahead of it, and refuses the arrival outright when the queue holds no sample to give up.
Either way the loss is counted, and `trailer.drops.events_not_recorded` cannot be non-zero from
overload alone — which is what §16 of the format spec tells a reader it may lean on. Regression
-tested for both policies in `tests/determinism/`; see D-43.

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
| 17 | the condition file is malformed — named parse error, **before any subscription** (BTB-REF-1) |
| 18 | a replay failed: the capture is missing, truncated, malformed, or declares a `format_version` this build does not implement |
| 19 | a second interrupt arrived while the queue was draining, and the drain was forced to stop (BTB-SD-1). The capture may lack its trailer — one interrupt alone would have saved it |

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

## The referee

The bridge evaluates **declared conditions** against a run and emits a verdict for each. The
conditions live in a JSON file, so adding or changing one needs no rebuild — which is the whole
point: conditions compiled into the binary cannot be re-applied to a stored run, and
re-applying them to a stored run is what makes a capture worth keeping.

```cmd
:: live - judge as the run happens
build\x64\Release\n8ro-bridge.exe ^
    --config      SimEngineClient_SharedMemory ^
    --model-path  C:\N8RO\data\db ^
    --schema-file N8roSimSchema ^
    --out-dir     captures ^
    --conditions  conditions\atacama.conditions.json

:: offline - re-judge a finished capture, no simulator, no bus, no client
build\x64\Release\n8ro-bridge.exe ^
    --replay     captures\capture-atacama-air-defense-000.n8rocap.jsonl ^
    --conditions conditions\atacama.conditions.json
```

Verdicts go to two places: into the capture as `verdict` records, and into a
`verdicts-<scenario>-<run-label>.jsonl` beside it. A replay writes
`verdicts-<stem>.replay.jsonl`, so it never overwrites the live run's.

### Live and replay agree, byte for byte

```
live    1718 bytes  sha256 dca9c5fe63587cc6ad53ce42b91a05bb78790151bbdfd137fe6829e398652faa
replay  1718 bytes  sha256 dca9c5fe63587cc6ad53ce42b91a05bb78790151bbdfd137fe6829e398652faa
```

That is BTB-REF-4's acceptance criterion, and it holds **by construction rather than by
testing**: there is one evaluation engine, and the two paths differ only in how a named field
is read out of a record — a decoded `StreamValueMap` off the bus, or a parsed `sample.fields`
object out of a file. No deciding rule is written twice, so the two cannot drift.

It is also the strongest available check on the format itself. If the referee can re-derive its
own verdicts from the file alone, the file demonstrably contains enough for a third party.

Replay of a 64 MB, 132 454-line capture takes **1.02 s**, against a target of under 60 s for a
ten-minute capture.

> **Vendoring this?** Take [`docs/condition-file-schema.md`](docs/condition-file-schema.md), not
> an excerpt of the four sections below. It carries all four — the declaration shape *and* the
> arithmetic and boundary rules every number in a verdict depends on — in one file, so a re-pin
> is a byte comparison. Excerpting these by hand is how a downstream consumer ended up with the
> shape and none of the arithmetic (EXT-17's E-5).

### Declaring conditions

The vocabulary is **closed at three kinds**. A fourth is a named parse error and a non-zero
exit before any subscription is made — never a silently skipped condition, because a run that
reports "all passed" after quietly dropping the one that mattered is the failure this design
exists to prevent.

```json
{
  "conditions": [
    {"id": "red-leader-reaches-airfield", "kind": "proximity",
     "entities": ["RedUAV_N_01", "BlueBase_Airfield"], "within_m": 3000},

    {"id": "red-leader-enters-base-circle", "kind": "area", "entity": "RedUAV_N_01",
     "test": "inside",
     "region": {"shape": "circle", "centre": [-23.49849, -68.25173, 7.5], "radius_m": 3000}},

    {"id": "red-leader-crosses-corridor", "kind": "area", "entity": "RedUAV_N_01",
     "region": {"shape": "polygon",
                "vertices": [[-23.47, -68.29], [-23.47, -68.23],
                             [-23.52, -68.23], [-23.52, -68.29]]}},

    {"id": "red-leader-is-destroyed", "kind": "terminal_state",
     "entity": "RedUAV_N_01", "removal_reason": "destroyed"},

    {"id": "airfield-reaches-operational", "kind": "terminal_state",
     "entity": "BlueBase_Airfield", "field": "phase", "equals": "operational"}
  ]
}
```

A working file is committed at
[`conditions/atacama.conditions.json`](conditions/atacama.conditions.json).

| key | applies to | meaning |
|---|---|---|
| `id` | all | Stable identifier, unique in the file. It is what the verdict is traced by, so a duplicate is a parse error |
| `kind` | all | `proximity`, `area` or `terminal_state`. Anything else is a named parse error |
| `entities` | proximity | Exactly two entity names. Naming the same one twice is rejected — it is met at distance zero |
| `within_m` | proximity | Threshold in **metres**. The comparison is `<=` |
| `entity` | area, terminal_state | One entity name |
| `test` | area | `inside` (default) or `outside` |
| `region.shape` | area | `circle` or `polygon` |
| `region.centre` | circle | `[latitude°, longitude°, altitude m]`. Altitude may be omitted and defaults to 0. `center` is accepted too |
| `region.radius_m` | circle | Radius in **metres**, positive |
| `region.vertices` | polygon | At least three `[latitude°, longitude°]` points |
| `removal_reason` | terminal_state | Matched **verbatim** against `entity_remove.reason`. The platform's vocabulary is open, so a supplier-specific reason this build has never seen still matches |
| `field` + `equals` | terminal_state | Matched against a sample's field value. Use one form or the other, never both |

Any key the loader does not recognise is ignored, which is what lets a `_comment` live in the
file. Units are the platform's own and are never converted: metres, degrees, and the
platform's `[lat, lon, alt]` order.

### Verdict semantics

**One verdict per condition per run.** At the first moment it is satisfied, or an explicit
`met: false` at end of run. It is not re-emitted on every later sample that also satisfies it —
"did the two aircraft come within 5 km" is answered by the first time they did.

**The not-met verdict is the load-bearing half.** Without it, a condition that was evaluated
and never satisfied is indistinguishable from one nobody evaluated.

A verdict carries enough to find the samples that caused it. A proximity verdict names both
entities, each one's **occupancy**, each one's sample `sim_time_s`, and the computed distance —
which is exactly the key needed to locate the two causing records in the capture.

```
red-leader-reaches-airfield  met  t=149.05  distance_m=2999.9981116642175  within_m=3000
red-leader-is-destroyed      met  t=149.45  removal_reason=destroyed
command-centre-is-destroyed  NOT MET
```

### How distance is computed

Positions are converted to **earth-centred, earth-fixed (ECEF) coordinates on WGS-84**, and
distance is the straight-line Euclidean distance between them in metres. The formulae are in
[`src/Geodesy.h`](src/Geodesy.h) with the constants spelled out, so a third party can reproduce
any verdict with a calculator — which is what BTB-REF-3 asks for.

Haversine was rejected because it ignores altitude, and two aircraft stacked 6 km apart
vertically are not close. Vincenty answers the surface question, iterates, and does not
converge for near-antipodal pairs.

**Boundary semantics**, because a threshold test that is ambiguous at the threshold is
untestable:

- A point exactly at `within_m`, or exactly on a circle's edge, is **inside** — the comparison
  is `<=`.
- A point exactly on a polygon's edge or vertex is **inside**.
- Polygons are treated as plane figures in latitude/longitude. Accurate at scenario scale; one
  spanning the antimeridian or a pole is not supported.

In practice the proximity boundary is not reachable: a geodetic distance is a computed double,
so two points a nominal 1 000 m apart come out a fraction of a millimetre off and `within_m:
1000` does not match them. The `<=` matters for **reproducibility** — the same input always
gives the same answer — not because anyone will land on it.

Altitudes carry the platform's own caveat: where the host's geoid grid is absent, as it is on
this machine, they are ellipsoidal rather than orthometric. That is the datum ECEF wants, so
the absence helps here.

## Tests

The entity picture (`src/EntityPicture.*`) is a component we own permanently rather than a
shim awaiting an SDK type, so it has tests. They need **no simulator, no bus and no model
database** — they drive the picture by handing it `StreamValueMap`s, which is exactly what
the bus's `DecodedHandler` does, so they exercise the real entry points. They also link no
N8RO import library; headers alone are enough.

Every suite below builds into `build\tests\`, which is where
[The four unit suites](#the-four-unit-suites--no-simulator-needed) and the rest of this README
expect to find them. `build\` is git-ignored, so a fresh clone has to create it once:

```cmd
call C:\N8RO\setup.cmd
call C:\N8RO\dev\setup-dev.cmd
if not exist build\tests mkdir build\tests
```

`setup.cmd` alone is not enough here: it exports `N8RO_RELEASE` and the runtime `PATH`, but it
is `setup-dev.cmd` that puts the compiler on `PATH`. Without it, `cl` is not recognised.

```cmd
cl /std:c++17 /EHsc /W4 /O2 ^
   /I %N8RO_RELEASE%\include\n8ro-core /I %N8RO_RELEASE%\include\n8ro-sim ^
   /Fe:build\tests\entity_picture_test.exe /Fo:build\tests\ ^
   tests\entity-picture\entity_picture_test.cpp src\EntityPicture.cpp
build\tests\entity_picture_test.exe
```

Exit code 0 if every check passes, 1 otherwise with each failure named. 81 checks covering
occupancy lifecycle (ADR-6), orphan counting, verbatim reasons and payloads, absent-field
accounting, deterministic ordering, the bounded event log, concurrent handler/snapshot
traffic, and the guard that stops a repeated `entity_deleted` closing one occupancy twice.

The suite's own adequacy is checked by mutation: deliberate defects introduced into
`EntityPicture.cpp` must make it fail. That is worth re-running when the picture changes —
it is how the "stale sample survives a re-creation" gap was found, which every other test
had been passing over.

### The referee

`tests/referee/` covers the three condition kinds against synthetic sample sequences, the
boundary case, the never-met case, the loader's rejections, and the one invariant that is
invisible from outside — that a re-created name does not inherit the previous tenure's position
(ADR-6). It drives the referee through the same `FieldSource` seam the live and replay paths
use, so it exercises the real entry points. **No simulator, no bus, no model database.**

```cmd
cl /std:c++17 /EHsc /W4 /O2 ^
   /I %N8RO_RELEASE%\include\n8ro-core /I %N8RO_RELEASE%\include\n8ro-sim ^
   /Fe:build\tests\referee_test.exe /Fo:build\tests\ ^
   tests\referee\referee_test.cpp src\Referee.cpp src\Conditions.cpp ^
   src\Geodesy.cpp src\JsonParse.cpp src\Json.cpp
build\tests\referee_test.exe
```

93 checks, exit 0 if all pass.

### The determinism harness

`tests/determinism/determinism_test.cpp` tests the emission path against each of the three
non-determinism sources R4 names, because all three are ours and all three are easy to
reintroduce: unordered-map iteration, locale-dependent float formatting, and unordered output
containers. It also carries **golden lines** — the exact bytes of an `entity_remove`, a
`segment_open`, the header's opening keys, the `limits` and rotation keys a bounded capture
carries (spec §6.6, §6.7), and a trailer with and without `continued_in` — so that after the
format freeze, changing a record's spelling means editing a test that says "these bytes". It also carries **BTB-CAP-4's schema-growth check**: a field inserted into the middle of a schema must appear in the `sample` record in the position the schema declares, with no code change — which is the half of UAC-BTB-CAP-4 that needs no simulator.

It also carries the one **writer-side** invariant that is checkable with no file and no
simulator: the handler-to-writer queue's structural reserve (BTB-BP-4, D-8, format spec §16) —
that an overload costs samples and never roster or segment records, under **both** overflow
policies. That is not a determinism property; it lives here because `RecordQueue` links no
import library and has no other simulator-free home, and a separate harness for three checks
would be a fourth build line for the same three checks.

The locale check is the one that earns its keep. `%.17g` is round-trip exact and *silently*
locale-dependent, and this machine's locale is comma-decimal, so the test runs for real rather
than being skipped.

```cmd
cl /std:c++17 /EHsc /W4 /O2 ^
   /I %N8RO_RELEASE%\include\n8ro-core /I %N8RO_RELEASE%\include\n8ro-sim ^
   /Fe:build\tests\determinism_test.exe /Fo:build\tests\ ^
   tests\determinism\determinism_test.cpp src\CaptureFormat.cpp src\Json.cpp ^
   src\Referee.cpp src\Conditions.cpp src\Geodesy.cpp src\JsonParse.cpp ^
   src\RecordQueue.cpp
build\tests\determinism_test.exe
```

39 checks, exit 0 if all pass. The end-to-end half — ten replays of one stored capture, hashed
— is `tests\determinism\replay_hashes.ps1`; see [Reproducing the
evidence](#reproducing-the-evidence).

### Capture conformance reader

`tests/capture-reader/` is a reader for the capture format, **written from
[`docs/capture-format-v1.md`](docs/capture-format-v1.md) and not from this program's source.**
It links neither the bridge nor the N8RO SDK — standard library only — which is what makes
BTB-CAP-5's "the spec is complete enough to write a reader from" a test rather than a claim.
Every rule it enforces cites the spec section it came from.

```cmd
cl /std:c++17 /EHsc /W4 /O2 /Fe:build\tests\capture_reader.exe /Fo:build\tests\ ^
   tests\capture-reader\capture_reader.cpp

build\tests\capture_reader.exe ^
    docs\sample-capture\capture-atacama-air-defense-sample.n8rocap.jsonl ^
    --spec docs\capture-format-v1.md
```

That runs against the trimmed sample, which is the one capture a fresh clone has. Point it at a
full reference run instead — 200 s of the reference scenario, made by
[Recording a capture](#recording-a-capture) — and the summary looks like this:

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
    docs\sample-capture\capture-atacama-air-defense-sample.n8rocap.jsonl ^
    build\tests\capture_reader.exe docs\capture-format-v1.md
```

Sixteen deliberate defects — wrong field order, an undeclared field, a sample outside a
segment, a miscounted trailer, a truncated file, a record after the trailer, an injected
timestamp, CRLF endings, an unknown `format_version`, a sample after its own occupancy's
removal — **16 caught, 0 survivors.**

### Comparing a capture against the publisher

`n8ro-sim-local` writes its own per-entity JSONL under `test_artifacts/` in its working
directory. That is the publisher's own account of what it published, independent of our bus,
our subscription and our code, and it is the closest thing available to answering "did we
record everything?" without trusting the counters we are trying to check.

```cmd
python tests\publisher-compare\compare.py ^
    captures\capture-atacama-air-defense-000.n8rocap.jsonl ^
    test_artifacts\n8ro-sim-local\sim_entity_state.jsonl
```

**Read the result carefully in both directions.** M6 established that the host's own dump is
lossy too — it is missing whole frames that our capture contains, 30 at the reference rate and
203 under the overload scenario. So this comparison bounds our completeness from one side only
and is not a ground truth. The tool reports both directions by frame for exactly that reason.

Risk R7 and §14 of the format spec carry the full picture; the M6 section of
[`notes.md`](notes.md) has the numbers and what changed because of them.

### The host driver

`tests/host-driver/` drives an already-running `n8ro-sim-app.exe` over the bus — it loads a
scenario, starts the engine, waits a **frame** budget and stops it. It is the only tool here
that **publishes**, which is why it lives in `tests/` and not in the bridge: ADR-4 and §14 of
the format spec both promise a consumer that the recorder cannot perturb the run.

Unlike the four suites above it links the SDK, so it needs the install and the same four
import libraries the bridge does:

```cmd
cl /std:c++17 /EHsc /W4 /O2 ^
   /I %N8RO_RELEASE%\include\n8ro-core /I %N8RO_RELEASE%\include\n8ro-sim ^
   /Fe:build\tests\host_driver.exe /Fo:build\tests\ ^
   tests\host-driver\host_driver.cpp ^
   /link /LIBPATH:%N8RO_RELEASE%\lib n8ro-core.lib n8ro-sim.lib n8ro-schema.lib n8ro-data.lib
build\tests\host_driver.exe --help
```

`--help` exits 2 and prints the usage, which is the cheapest check that the link resolved.
It is used by [The R8 spike](#the-r8-spike-is-the-headless-host-repeatable), and it is what
closed OQ-2 — the headless invocation is demonstrated rather than asserted.

## Determinism probe

`tests/float-format/float_format_probe.cpp` settles which double-to-text format is
round-trip exact **and** locale independent on this toolchain. It is standalone:

```cmd
cl /std:c++17 /O2 /EHsc /Fe:build\tests\float_format_probe.exe /Fo:build\tests\ ^
   tests\float-format\float_format_probe.cpp
build\tests\float_format_probe.exe
```

Exit code 0 if at least one candidate passes both axes. The result and what it means for
the capture format are in [`notes.md`](notes.md).

## Reproducing the evidence

Everything the project claims is re-runnable with one command. The scripts are in the
repository so a result is checkable rather than asserted. The demo recording was shot to a
script covering four takes, the command behind each of BTB-DOC-2's seven beats, and what to
say over them.

Each of these assumes a shell that has run `C:\N8RO\setup.cmd`.

### The demo recording

**[Four takes, on Google Drive.](https://drive.google.com/drive/folders/1L0lPs0wkDA_qGYx8Z0Q8-SMzNrOvLoXN?usp=sharing)** Shot 2026-08-31 to a prepared shooting
script. Every beat BTB-DOC-2 names is on camera:

| Beat | Take | What proves it on screen |
|---|---|---|
| Start before the simulator | 1, step 2 | `waiting for a simulation host`, with no simulator running |
| A scenario load | 1, step 4 | `attached:` then `capture opened:`, roster filling to `live=42` |
| A reload producing two segments | 1, step 6 | `segments=2` in the run summary |
| An entity removal | 1, step 4 | `rm=` climbing in the status line |
| A verdict firing | 1 step 6, and take 2 | verdict count in the summary; five met and two not met on replay |
| The backpressure demonstration | 3 | `samplesDropped=` non-zero, `eventsDropped=0`, and the capture still CONFORMS |
| Ctrl-C with a clean tail | 4 | exit 0 and a `shutdown` trailer as the last line |

Two things to know before watching, both of which look like faults and are not:

- **The takes were shot against producer 0.8.0**, and this repository now builds **0.9.0**. The
  difference is BTB-CAP-6 — the byte bound and the rotation option — which none of the seven
  beats exercises. Every command in the takes behaves identically on 0.9.0; the only visible
  difference is the `producer.version` string inside a capture header, and the two extra keys
  beside it. Nothing was re-shot to change a version string.
- **The install runs terrain degraded** — no elevation service and no geoid grid — so terminal 2
  floods with `n8ro-sim.scripting.navigation` errors throughout. Pre-existing, documented, and
  deliberately not fixed for the camera: every measurement in the PRD, `notes.md` and
  `docs/capture-format-v1.md` §14 was taken in this configuration, and provisioning terrain
  would make the recording show a different system than the evidence describes.

The shoot wrote its captures into `captures/` under the `demo`, `overload` and `ctrlc` labels.
**Those files are not in the repository** — `captures/` is git-ignored, because a single
reference run is about 64 MB and the three together are roughly 79 MB. What ships instead is
[`docs/sample-capture/`](docs/sample-capture/), a trimmed capture from a real run that is
un-ignored deliberately. To re-check a number visible on screen against a file, re-run that
beat's command from the sections below and compare against what it writes.

### The four unit suites — no simulator needed

```cmd
build\tests\entity_picture_test.exe     :: 81 checks - the roster and ADR-6
build\tests\referee_test.exe            :: 93 checks - the three condition kinds
build\tests\determinism_test.exe        :: 39 checks - R4's hazards, the locale, golden bytes,
                                       ::             CAP-4, and BP-4's structural reserve
build\tests\capture_reader.exe docs\sample-capture\capture-atacama-air-defense-sample.n8rocap.jsonl ^
    --spec docs\capture-format-v1.md     :: the format spec, checked against a real file
python tests\capture-reader\mutate.py docs\sample-capture\capture-atacama-air-defense-sample.n8rocap.jsonl ^
    build\tests\capture_reader.exe docs\capture-format-v1.md   :: 23 mutations, 0 survivors
python tests\referee\check_schema_digest.py  :: the vendorable condition digest still matches this file
```

The build lines for each are in the file's own header comment, and in [Tests](#tests) above.

### Clean Ctrl-C, twenty times (BTB-SD-1)

```cmd
powershell -ExecutionPolicy Bypass -File tests\shutdown\shutdown_loop.ps1 -Cycles 20
```

Each cycle starts its own simulator (bridge first), interrupts the bridge with a **real console
Ctrl-C**, and checks that it exited 0, that the file's last line is a well-formed trailer with
`end_reason: shutdown`, and that the trailer's own sample count matches the records in the
file. Result: **20 of 20 clean.**

### The determinism harness, both halves (BTB-CAP-3)

```cmd
build\tests\determinism_test.exe        :: the emission path, against R4's three hazards
powershell -ExecutionPolicy Bypass -File tests\determinism\replay_hashes.ps1
```

The second replays one stored capture ten times and hashes the verdicts. **10 of 10 identical.**
A live pair is deliberately *not* the test — on a wall-clock-paced host it measures the host's
repeatability, not the recorder's, which is what PRD rev 5 exists to say.

### The R1 teardown spike

```cmd
powershell -ExecutionPolicy Bypass -File tests\teardown-spike\teardown_spike.ps1 -Cycles 20
```

Twenty consecutive load-run-teardown cycles with the bridge attached. `0xC0000005` would appear
as exit code `-1073741819`. Result: **host 0 ×20, bridge 0 ×20 — it did not reproduce.**

### The R8 spike: is the headless host repeatable?

This is the one that changes what EXT-17 should build. It needs the headless host, driven over
the bus — the invocation OQ-2 asked about:

Build `host_driver` first if you have not — see [The host driver](#the-host-driver); it is
not built by the solution and terminal 3 below needs it.

```cmd
:: EVERY terminal below: call C:\N8RO\setup.cmd first. It exports N8RO_RELEASE and puts
:: C:\N8RO\bin on PATH, and BOTH are required here - see the two paragraphs under this block.

:: terminal 1 - the bridge, started first
build\x64\Release\n8ro-bridge.exe --config SimEngineClient_SharedMemory ^
    --model-path C:\N8RO\data\db --schema-file N8roSimSchema --out-dir captures --run-label r8a

:: terminal 2 - the headless host. Note: it takes NO scenario argument
n8ro-sim-app.exe --sim-config SimEngineHost_SharedMemory ^
    --model-path C:\N8RO\data\db --schema-file N8roSimSchema

:: terminal 3 - load, start, run to a FRAME budget, stop
build\tests\host_driver.exe --scenario "Atacama Air Defense" --frames 1200
```

**`N8RO_RELEASE` must be set for the headless host too, and its failure mode is the dangerous
one.** This block was first written as though the variable mattered only to the build and to
`n8ro-sim-local` ("Start order", above). It does not. With `N8RO_RELEASE` unset,
`n8ro-sim-app.exe` resolves its plugin directory from the **current working directory**, skips
the plugin scan, never registers `componentPhysics` from the stock
`bin\plugins\sim\n8ro-physics.dll`, and then **refuses every scenario load whose entities need
it** — all 42 of `Atacama Air Defense`. It does not exit and it does not report a failure: it
sits idle, so terminal 3 waits on a load that will never complete and an unattended run **hangs
rather than fails**. Measured downstream by EXT-17 (its E-6 / F-17), and confirmed by the
platform mentor on 2026-09-01 as the expected production provisioning rather than a workaround.

**`C:\N8RO\bin` on `PATH` is a second, separate precondition, and setting one does not cover
the other.** It is where `n8ro-sim.dll` and `n8ro-core.dll` resolve from, and there is nowhere
else. An SDK-linked binary — the bridge, `host_driver`, or anything EXT-17 builds — launched
from a directory without it exits **53** having produced no output at all, which reads like a
crash and is a missing DLL.

Repeat for `r8b`, then compare:

```cmd
fc /b captures\capture-atacama-air-defense-r8a.n8rocap.jsonl ^
      captures\capture-atacama-air-defense-r8b.n8rocap.jsonl        :: differs

python tests\determinism\compare_captures.py ^
    captures\capture-atacama-air-defense-r8a.n8rocap.jsonl ^
    captures\capture-atacama-air-defense-r8b.n8rocap.jsonl          :: 50358 of 50358 agree
```

**The frame budget is the point.** `--frames 1200` stops both runs at the same simulation
instant; a wall-clock budget stops them at different ones, and their captures would then differ
for a reason that has nothing to do with determinism.

`tests/host-driver/` is a **test tool, not part of the bridge**. The bridge subscribes and never
publishes, and that is what lets it promise it cannot perturb the run it records.

### Capture versus the publisher's own record (R7)

```cmd
python tests\publisher-compare\compare.py <capture> ^
    test_artifacts\n8ro-sim-local\sim_entity_state.jsonl
```

Read it in both directions: the host's dump is lossy too, so it bounds our completeness from
one side only.

### Making a committable sample capture

```cmd
python tests\evidence\trim_capture.py <big-capture> docs\sample-capture\<name>.n8rocap.jsonl
```

Keeps every non-sample record and the samples of three entities — `RedUAV_N_01`,
`BlueBase_Airfield` and `BlueSAM_ShortRange` — and rewrites one number in the trailer. The
result still reports CONFORMS, which is the check that it stayed valid.

## The sample capture

[`docs/sample-capture/`](docs/sample-capture/) holds a real capture from a real run, trimmed to
5.1 MB. It carries the whole story in one file:

- **both segments** — the run, and the teardown reload the engine's stop path produces
- **`RedUAV_N_01` at two occupancies** — created, `destroyed` at `t = 149.45`, re-created at
  occupancy 2. The end-to-end proof of ADR-6
- **all 132 `entity_add` and 90 `entity_remove` records**, with reasons verbatim
- **all seven verdicts**, including the two never-met ones

Its header says **`producer 0.9.0`**, which is the build this repository produces, so the file
demonstrates the current producer rather than an older one — including
`sample_form: "published"` (§6.3a) and `header.limits` (§6.6), the two keys that let a reader
answer "are these predictions?" and "was this file cut short?" from the file itself. §16 of the
format spec carries the producer version history.

Regenerating it is one run and one trim — see
[Making a committable sample capture](#making-a-committable-sample-capture). The counts above
are properties of the structure and survive a regeneration; per-entity **sample** counts do not,
because the publication schedule differs by about 1% between runs (§14).

## Layout

```
docs/prd.md                              the contract — 27 FRs prefixed BTB-
docs/capture-format-v1.md                the cross-repo contract EXT-17 is handed
docs/escalations.md                      five findings raised outward - three resolved, two open
notes.md                                 what the bus actually carries (graded deliverable)
src/main.cpp                             CLI, wiring, the lifecycle loop, the run summary
src/TopicResolution.{h,cpp}              BTB-EP-1 — schemas, and all four topics from the registry
src/EntityPicture.{h,cpp}                BTB-EP-3/EP-4 — the roster and the latest-sample map
src/CaptureRecord.h                      what crosses the handler-to-writer boundary
src/RecordQueue.{h,cpp}                  BTB-BP-1/BP-2/BP-4 — the bounded queue, counted overflow
src/CaptureWriter.{h,cpp}                BTB-CX-3/CX-4 — the writer thread and the segment machine
src/CaptureFormat.{h,cpp}                BTB-CAP-1/CAP-4 — the `n8ro-capture/1` serialiser
src/HandlerTiming.{h,cpp}                BTB-BP-1 — how long a handler actually takes
src/Conditions.{h,cpp}                   BTB-REF-1/REF-3 — the closed three-kind vocabulary
src/Geodesy.{h,cpp}                      the stated, reproducible distance method
src/Referee.{h,cpp}                      BTB-REF-2 — one engine, live and offline
src/Replay.{h,cpp}                       BTB-REF-4 — re-judging a stored capture
src/Json.{h,cpp}                         JSON escaping and the round-trip-exact float format
src/JsonParse.{h,cpp}                    reading JSON back — conditions and captures
src/ExitCodes.h                          one table of process exit codes
conditions/                              a working condition file for the reference scenario
src/Signals.{h,cpp}                      BTB-SD-1 — the handler sets a flag and nothing else
docs/sample-capture/                     a real capture, trimmed, for the evidence pack
tests/entity-picture/                    unit tests for the picture — no simulator needed
tests/referee/                           unit tests for the referee — no simulator needed
tests/determinism/                       R4's three hazards, plus the replay-hash harness
                                         and the content comparison EXT-17 should use
tests/capture-reader/                    conformance reader, written from the format spec alone
tests/publisher-compare/                 capture vs the host's own record — how R7 is measured
tests/shutdown/                          the twenty-cycle Ctrl-C loop
tests/teardown-spike/                    the R1 spike
tests/host-driver/                       drives the headless host — a TEST TOOL, not the bridge
                                         (links the SDK; built by hand, not by the solution)
tests/evidence/                          the capture trimmer
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
`end_reason: host_lost` and exits 0. **Ctrl-C ends it cleanly at any point** — the queue is
drained, the trailer is written with `end_reason: shutdown`, and the exit code is 0 (BTB-SD-1;
see [Clean Ctrl-C, twenty times](#clean-ctrl-c-twenty-times-btb-sd-1)). `--capture-max-samples`
gives you a record budget if you want a bounded file instead, and closes it with
`end_reason: size_limit`.

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
committed sample capture `RedUAV_N_01` is created at occupancy 1, publishes 2 954 samples, is
`destroyed` at `t = 149.45`, and returns at occupancy 2 with 9 more — each tenure bracketed by
its own `entity_add` / `entity_remove`. The destruction time is a property of the scenario and
repeats; the 2 954 is a sample count and will differ by about 1% in another run.

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
