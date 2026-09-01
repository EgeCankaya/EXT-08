# EXT-08 — bug-hunting code review of `src/`

**Date:** 2026-09-01 · **Scope:** all 30 files in `src/` (~6 500 lines) · **Kind:** defect hunt, not
style or architecture.

Measured against `docs/EXT-08-Bus-Telemetry-Bridge.docx` (the brief), `docs/prd.md` (27 FRs),
`docs/capture-format-v1.md` (frozen, §13 / §5.1 / §14), `docs/decisions-m5-m7.md` (D-1..D-42) and
CLAUDE.md's six hard rules.

Static review plus the simulator-free suites only; the simulator was not launched. All three
suites pass at the time of review — `entity_picture_test.exe` 72 checks, `referee_test.exe`
93 checks, `determinism_test.exe` 29 checks, exit 0 each — so every finding below is something
they do not cover.

---

## 1. Findings

### H1 — `--overflow-policy drop_oldest` evicts structural records, breaking the asymmetry the spec tells readers to rely on

**Severity: high · `src/RecordQueue.cpp:66-87` · CONFIRMED**

Clause: `docs/decisions-m5-m7.md` D-8 ("samples are refused above `capacity`, roster and segment
records only above the hard bound"); `docs/capture-format-v1.md` §16 ("the producer reserves queue
headroom for roster and segment records specifically so that overload costs data and never
structure. That asymmetry is deliberate and **a reader can lean on it**"); BTB-BP-4.

The two-threshold design is implemented only for `DropNewest`. On the `DropOldest` branch:

```cpp
const std::size_t limit = structural ? sampleCapacity_ + structuralReserve_ : sampleCapacity_;
if (records_.size() >= limit) {
    if (policy_ == OverflowPolicy::DropNewest) { /* refuse, counted */ return; }
    // DropOldest
    if (!records_.empty()) {
        if (isStructuralRecord(records_.front().kind)) { ++counters_.structuralDropped; }
        records_.pop_front();          // <-- evicts whatever is at the front
    }
}
```

An arriving **sample** whose threshold is `sampleCapacity_` pops `records_.front()` — which, during
a scenario load, is exactly the `entity_created` burst. D-8's overload experiment
(`--queue-size 4`, 2 520 samples dropped, **0** events dropped) was run under the default
`drop_newest`; the same run under `drop_oldest` inverts it.

**Failure scenario:** `--queue-size 4 --overflow-policy drop_oldest`, reference scenario. The
42-record creation burst is enqueued at load; the 818 samples/s stream immediately fills the queue
and each arriving sample pops one `entity_add` off the front. The capture then has no `entity_add`
for those names, so `EntityPicture` still rosters them (the picture is updated in the handler,
before the queue) but the *file* has samples under occupancies never opened — which EXT-17's
`(entity, occupancy)` join silently mis-keys. `trailer.drops.events_not_recorded` is non-zero, so
it is at least counted — but §16 promises a reader it *cannot* be non-zero from overload alone.

---

### H2 — Nothing catches an exception on the writer thread, and one call there can throw

**Severity: high · `src/main.cpp:974`, `src/CaptureWriter.cpp:86-111` · CONFIRMED**

Clause: CLAUDE.md hard rule 1 ("Never throw"); BTB-CX-1 AC3 ("No exception escapes `main` under any
configuration error"); BTB-CX-3 ("No crash, no exception").

`main()` wraps everything in `try/catch(...)`, but that guard covers the main thread only:

```cpp
std::thread writerThread([&writer, &queue] { writer.run(queue); });
```

An exception escaping that lambda is `std::terminate` — no trailer, no flush, no diagnostic, and
the exit code is not one of `ExitCodes.h`.

There is a concrete throwing call on that thread. `CaptureWriter::nextRunLabel` (reached from
`ensureOpen`, reached from `apply`, on the writer thread) constructs the iterator with an
`error_code` but then iterates with the **throwing** overload:

```cpp
std::filesystem::directory_iterator it(outDir, ec);   // non-throwing construction
if (!ec) {
    for (const std::filesystem::directory_entry& entry : it) {   // operator++ throws filesystem_error
```

Range-for calls `operator++()`, which has no `error_code` overload and throws
`std::filesystem::filesystem_error` on any error after construction.

**Failure scenario:** `--out-dir` on a network share (or a directory whose ACL changes) that becomes
unreadable between the `directory_iterator` construction and the end of the walk — enumeration
fails mid-iteration, `filesystem_error` propagates out of `nextRunLabel` → `ensureOpen` → `apply` →
`run` → the thread body → `std::terminate`. The pump is still running, the capture file is empty or
absent, and the operator sees an abort rather than exit code 13.

Every `std::string` build in `CaptureFormat.cpp` and every `staging_.push_back` is also a
`bad_alloc` site on that thread; the guard is what makes hard rule 1 structural rather than
incidental.

---

### M1 — `verdict` records can be written outside any open segment

**Severity: medium · `src/CaptureWriter.cpp:673-695` · CONFIRMED (traced)**

Clause: format spec §7 Segment rules — "`entity_add`, `entity_remove` and `verdict` records also
carry `segment` and also fall inside an open segment"; §7 — "A capture may legitimately contain zero
segments … the file is `header` then `trailer`."

`finish()` emits end-of-run not-met verdicts *before* `closeSegment()` — correct — but it never
checks that a segment is actually open:

```cpp
if (referee_) {
    for (const Verdict& verdict : referee_->finalVerdicts(lastDataSegment_, lastDataSimTimeS_)) {
        ... emit(line);            // no segmentOpen_ guard
    }
}
closeSegment(lastRecordSimTimeS_, endReasonName(reason));   // no-op if none is open
```

Three reachable paths:

1. **Bridge starts with `--conditions`, no scenario ever loads, Ctrl-C.** `!opened_` → `ensureOpen`
   writes the header; staging is empty; `finalVerdicts(0, 0.0)` emits N verdicts at `"segment":0`;
   `closeSegment` is a no-op. The file is `header`, N × `verdict`, `trailer` with
   `counts.segments: 0` — a zero-segment capture containing records the spec says must sit inside a
   segment.
2. **Ctrl-C in the window between a `scenario_unloaded` and the reload's creation burst.** The
   segment is closed, staging is empty, so the guard at line 669
   (`!staging_.empty() && !segmentOpen_`) does not fire. Verdicts land carrying the ordinal of an
   already-*closed* segment.
3. **`rotate()` bails at `CaptureWriter.cpp:372`** (bound too small to hold a header plus the
   reserve). It returns `false` *without* calling `openSegment`, even though `hadSegment` was true.
   `finish()` then emits verdicts stamped with `lastDataSegment_` — an ordinal belonging to the
   **previous part's** file.

`tests/capture-reader/capture_reader.cpp:921-929` does not enforce §7 for verdicts (only for
`sample`, line 1077, and `entity_add`/`entity_remove`, line 895), which is why the case has gone
unnoticed; a stricter EXT-17 reader written from §7 will reject these files.

---

### M2 — The delivered `MessageSchema` is discarded; the writer uses a startup snapshot

**Severity: medium · `src/main.cpp:851-853`, `src/CaptureWriter.cpp:581-582` · CONFIRMED (code fact), consequence conditional**

Clause: BTB-EP-2 AC2 — "The `MessageSchema` reference delivered with each message **is used for
field order** (see BTB-CAP-3), **not discarded**." Also BTB-CAP-4 ("every field the message's schema
declares, verbatim").

The entity-state handler takes the schema as an unnamed parameter and drops it:

```cpp
[&picture, &queue, &stateTiming](const n8ro::core::Message&,
                                 const n8ro::sim::MessageSchema&,   // unnamed - discarded
                                 const n8ro::sim::StreamValueMap& values) {
```

`writeSample` then formats against `stateSchema_`, the copy taken from the registry at
`resolveTopics` time. Today the two are the same object, so the bytes are identical — but the
requirement is written the way it is because the identity is not guaranteed by anything in the code.

**Failure scenario:** a model database in which a second message type shares the entity-state topic
(the `getByTopic` round-trip at `TopicResolution.cpp:276` proves the *topic index* maps back to one
message, not that the topic carries only one). Arrivals of the second type decode fine, pass
`onSample` if they carry `scenarioEntityName`/`simulationTime`, and are written with
`"message":"simEntityStateUpdate"` and only the fields that message declares — a mislabeled record
whose join to `header.schemas[].message_name` is wrong and whose extra fields are silently dropped,
violating the verbatim rule with no counter anywhere.

---

### M3 — `getLoadedScenarioName()` is called on the writer thread, against the file's own stated rule

**Severity: medium · `src/main.cpp:807-809` + `src/CaptureWriter.cpp:564-567` · PLAUSIBLE**

Clause: CLAUDE.md hard rule 2 / BTB-BP-1's threading model; and the code's own reasoning at
`CaptureWriter.h:92-96` and `main.cpp:784-791`.

`liveTrailerState_` is deliberately *not* given the SDK objects, with an explicit rationale:
"`RecordQueue::counters()` is mutex-guarded and safe, but nothing establishes that for the SDK's own
metric accessors, and reaching into them off the main thread to fill in a counter would be trading a
real invariant for a cosmetic one."

The `lastKnownScenario_` callback then does exactly that:

```cpp
[&client] { return client->getLoadedScenarioName().value_or(std::string{}); },
```

and it is invoked from `CaptureWriter::apply(Sample)` — the **writer thread** — while the pump
thread is updating the client's mirrored scenario state.

**Failure scenario:** BTB-CX-2's attach-mid-run case. `lastScenarioSeen_` is empty (no
`scenario_loaded` was ever seen), so the first accepted sample calls `lastKnownScenario_()` on the
writer thread at the same moment the pump thread processes a `scenario_loaded` and writes the
client's cached `std::string`. Concurrent read/write of a `std::string` is a data race: torn read,
or a use-after-free on the SBO→heap transition. The consequence is a garbage or crashing capture
*filename*. `finish()` calls the same lambda from the main thread after the pump is stopped, which
is safe; only the `apply(Sample)` call site is exposed.

---

### M4 — BTB-OBS-1's second acceptance criterion is not implemented

**Severity: medium · `src/main.cpp:1068-1131`, `printRunSummary` · CONFIRMED**

Clause: BTB-OBS-1 AC2 — "A non-zero `schemaHashDrops` or `decodeFailures` produces a **distinct
warning naming the likely cause and the two things to check (model path, schema file)**."

All five counters are printed on the status line and in the summary and written into the trailer —
AC1 is met. But there is no conditional warning anywhere: `grep` for `schemaHashDrops` in `src/`
finds only trailer serialisation and `printf` sites. The named diagnostic naming model path and
schema file exists only in `TopicResolution.cpp` for the *empty registry* case, which is a different
fault.

**Failure scenario:** the bridge is pointed at a model path whose schema file is a release behind the
engine's. The registry is non-empty (so BTB-EP-1 passes), subscriptions succeed, and
`schemaHashDrops` climbs. The operator sees `decode=1234(hash=1234 fail=0 noschema=0)` buried in a
status line printed once a second and no warning at all — which is precisely the R2 failure ("three
independent detections for one fault") reduced to one and a half.

---

### M5 — BTB-OBS-2's silent-topic detection is not implemented

**Severity: medium (P2 requirement) · `src/main.cpp:1000-1135` · CONFIRMED**

Clause: BTB-OBS-2 — "WHILE the engine reports itself running, IF a subscribed topic has produced no
decoded messages for a configurable interval, THEN the system SHALL log a warning naming the topic
and the schema-mismatch hypothesis." PRD §Observability lists the metric as `topic_silence_s`.

The main loop tracks only the engine-state heartbeat, for host loss. There is no per-topic
last-decode timestamp, no configurable interval, and no warning. The FR's *second* half — the
one-screen exit summary — is fully implemented (`printRunSummary`), which is presumably why the FR
reads as delivered.

**Failure scenario:** the engine-state message decodes but the entity-state message does not (a hash
mismatch confined to one message type). `everAttached` is true, host loss never fires, the engine
reports `running`, and the bridge records a capture with zero samples for the whole run without one
warning line. This is the exact scenario BTB-OBS-2 exists for, and CLAUDE.md's claim that "every
requirement in the PRD is implemented" does not hold for it.

---

### M6 — BTB-BP-2's sequence-gap criterion is not implemented

**Severity: medium · `src/main.cpp:851, 878, 910, 947` · CONFIRMED**

Clause: BTB-BP-2 AC2 — "A sequence gap or out-of-order arrival is counted and reported, not silently
accepted"; UAC-BTB-BP-2 — "verified against `sequenceNumber`".

Every one of the four handlers takes `const n8ro::core::Message&` as an **unnamed** parameter.
`Message::sequenceNumber` is never read; there is no gap counter, no reorder counter, nothing in the
trailer or the summary. FIFO *through our own queue* is established by construction (single deque,
D-7) — that is BP-2's first criterion. The second criterion, which is about detecting loss and
reordering upstream of us, has no implementation and no waiver in `decisions-m5-m7.md`.

This matters more than it looks: §14 "Known loss" documents that this platform loses whole frames
with **every** counter reading zero. `sequenceNumber` is the one instrument that could have counted
it, and it is discarded.

---

### L1 — Staged roster records are dropped uncounted when the byte bound stops the run

**Severity: low · `src/CaptureWriter.cpp:435-471`, `669-671` · CONFIRMED**

Clause: CLAUDE.md tenet 3 / BTB-BP-4 ("Losses are counted, never silent").

`flushStaging()` bails on the first `admitData` refusal:

```cpp
if (!admitData([&]{ ... }, record.simTimeS)) { return; }
```

The already-popped record is counted once via `recordsPastBound_`, but every record still in
`staging_` behind it is left in the deque and never written or counted. `finish()` only re-enters
`flushStaging` via `openSegment` when `!segmentOpen_`, so with a segment open they are simply
discarded. Reachable with `--capture-max-bytes` + `--on-size-limit stop` when the bound is hit
inside a creation burst.

### L2 — `stagedDropped` is a per-part counter but is read as a run total

**Severity: low · `src/main.cpp:1166`, `src/CaptureWriter.cpp:359` · CONFIRMED**

`drops.eventsNotRecorded = finalQueue.structuralDropped + writer.counts().stagedDropped;` —
`counts_` is reset wholesale by `rotate()` (`counts_ = WriterCounts{}`), and
`runCounts_.stagedDropped` is never incremented at all. On a rotated run, staging overflow from
every part but the last vanishes from the last part's trailer and from the summary. Same shape at
`main.cpp:552`, where the summary prints `writer.counts().verdicts` (per part) while every
neighbouring line uses `runCounts()`, contradicting the comment at `CaptureWriter.h:189-192`.

### L3 — `run()` ignores the result of `file_.flush()`

**Severity: low · `src/CaptureWriter.cpp:630-636` · PLAUSIBLE**

Clause: BTB-CX-3 AC3 — "No crash, no exception, **no partially-written final line**."

`emit()` checks `!file_` immediately after the insert, but a buffered `ofstream` surfaces ENOSPC at
flush time, and the batch flush in `run()` discards its result. The failure is caught at the *next*
`emit()` (failbit is sticky), by which point a short final line may already be on disk. The window
is one drained batch wide.

### L4 — Rotated captures carry a run-label-dependent value that §14 does not list as environment-dependent

**Severity: low · `src/CaptureWriter.cpp:248-249`, `317` · CONFIRMED**

Clause: format spec §14 — "**The one host-dependent field** is `platform.model_path` … compare with
that field excluded"; BTB-CAP-3.

`header.continues_from` and `trailer.continued_in` embed `runLabel_`, which `nextRunLabel()` derives
from what already exists in `--out-dir`. Two byte-identical runs recorded into the same directory
produce labels `000` and `001`, so their rotated parts differ in those two keys — a second field a
determinism comparison must exclude, which §14's table and its "What a size bound does to a byte
comparison" subsection both assert does not exist.

### L5 — Host-loss teardown has no timeout of its own

**Severity: low · `src/main.cpp:1145-1153` · PLAUSIBLE**

Clause: BTB-CX-3 AC2 — "The bridge process exits within a bounded, documented detection window; **it
never blocks indefinitely on a dead bus**."

`startDrainWatchdog` fires only on a *second interrupt* (`gInterrupts >= 2`). On a host-loss
teardown with no operator present — the FR's own "unattended overnight run" scenario — the four
`packed.unsubscribe` calls and `client->stopMessagePump()` run against a dead bus with nothing
behind them. D-4 argues the resolution failure is fatal *because* it would produce "a bridge that
blocks indefinitely on a dead bus"; the teardown path has the same exposure and no guard. SDK
blocking behaviour could not be verified statically, hence PLAUSIBLE.

### L6 — A repeated `entity_deleted` emits a second `entity_remove` for one occupancy

**Severity: low · `src/EntityPicture.cpp:140-158` · PLAUSIBLE**

`onEntityEvent` does not check `entry->second.open` before closing. A second `entity_deleted` for
the same name with no intervening `entity_created` sets `open = false` again, increments
`entityDeleted` and `removalsByReason_` a second time, and returns another `Kind::Removed` —
producing two `entity_remove` records for one occupancy, breaking §8.1's "opened by an `entity_add`
and closed by **the matching** `entity_remove`" bracketing. Not observed on 2.1.328; the guard is
one line and the counter would make it visible.

---

## 2. Recommended fixes

### (a) Code fixes — make the implementation match what the PRD or spec already says

| # | Fix |
|---|---|
| **H1** | In `RecordQueue::offer`, make `DropOldest` evict only a **non-structural** record: scan from the front for the first `!isStructuralRecord` entry and erase that; if the queue holds only structural records and the arrival is a sample, refuse it (count `samplesDropped`). This restores D-8 and §16 without changing the counters' meaning. |
| **H2** | Wrap the writer-thread body: `std::thread([&]{ try { writer.run(queue); } catch (const std::exception& e) { N8RO_LOG_CRITICAL(...); } catch (...) { ... } })`, setting a `failed_`-equivalent flag the main loop already polls at `main.cpp:1062`. Separately, replace the range-for in `nextRunLabel` with the `error_code` increment: `for (auto it = std::filesystem::directory_iterator(outDir, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec))`. |
| **M1** | In `CaptureWriter::finish`, before emitting `finalVerdicts`, open a segment if none is open — reuse the existing staging-flush shape: `if (!segmentOpen_ && referee_ && referee_->conditionCount() > 0) openSegment(lastScenarioSeen_, lastDataSimTimeS_);`. Also make `rotate()`'s bail-out at line 372 call `openSegment` when `hadSegment` before returning false, so the new part is not left segment-less. |
| **M2** | Capture the delivered schema in the handler and carry it on `CaptureRecord` (a `const MessageSchema*` into the registry, which outlives the run, costs nothing per record), and have `writeSample` use it instead of `stateSchema_`. This is what BTB-EP-2 AC2 asks for verbatim. |
| **M3** | Remove the SDK call from the writer-thread path. Cache the loaded scenario name into the same `busSnapshotMutex`-guarded struct the status poll already fills (`main.cpp:1097-1098` reads it once a second anyway), and have `lastKnownScenario_` read the cache — exactly the pattern `liveTrailerState_` already uses, and for the reason stated at `CaptureWriter.h:92-96`. |
| **M4** | Add a latch in the status-poll block: on the first non-zero `schemaHashDrops` or `decodeFailures`, `N8RO_LOG_WARNING` naming the counter, the schema-mismatch hypothesis, and the two values to check (`--model-path`, `--schema-file`) — the same text `TopicResolution.cpp:216-219` already carries for the empty-registry case. |
| **M5** | Add per-topic last-decode counters (one `std::atomic<std::uint64_t>` per subscription, incremented in each handler — the heartbeat handler is already exactly this) and a `--topic-silence-s` interval. Warn only while `client->getEngineState()` reports running, per the FR's "no such warning fires while the simulation is paused". Note that `--topic-silence-s` is a **new CLI option** and the PRD's naming-conventions table is the authority on that set, so it needs a table entry in the same revision. |
| **L1** | In `flushStaging`, on an `admitData` refusal, count the remaining `staging_.size()` into `counts_.stagedDropped` / a run-wide equivalent and clear the deque, rather than leaving records unwritten and uncounted. |
| **L2** | Add `stagedDropped` to `runCounts_` and read the run-wide value at `main.cpp:1166`; change `main.cpp:552` to `writer.runCounts().verdicts`. |
| **L3** | Check `file_` after the batch flush in `run()` and set `failed_` there, so a full disk is detected before the next record is appended. |
| **L6** | Guard on `entry->second.open` in `onEntityEvent`'s delete branch; count the redundant delete under a named counter rather than emitting a second record. |

### (b) PRD-level clarifications — where the spec is silent and the code has picked a behaviour

1. **`finish()`'s final verdicts bypass the byte bound** (`CaptureWriter.cpp:683-688`), and so does
   the header (`ensureOpen` emits it unchecked). A capture can therefore exceed
   `header.limits.max_bytes` by up to the size of its end-of-run verdict block. Attach to
   **BTB-CAP-6**: *"`--capture-max-bytes` bounds every record the producer admits through the size
   check. The `header`, the closing `segment_close`, the end-of-run `verdict` records and the
   `trailer` are written from the reserved close budget and are not themselves refused; where the
   run declares more conditions than the reserve accommodates, the file may exceed the bound rather
   than omit a verdict. Never silently truncating outranks the bound."* This makes explicit the
   trade the code comment already states.

2. **Determinism scope of the rotation linkage keys** (finding L4). Attach to **BTB-CAP-3**:
   *"`header.continues_from` and `trailer.continued_in` embed the run label, which is derived from
   the contents of `--out-dir`. Two identical runs recorded into the same directory therefore differ
   in those keys. Like `platform.model_path`, they are environment-dependent and must be excluded
   from a byte comparison; `--run-label` supplied explicitly removes the dependence."*

3. **`--overflow-policy drop_oldest`'s relationship to the structural reserve.** Whatever H1's fix,
   the FR should say which invariant wins. Attach to **BTB-BP-4**: *"The structural reserve holds
   under every overflow policy: a `sample` is never permitted to evict a roster or segment record.
   `drop_oldest` evicts the oldest **sample**, not the oldest record."*

### (c) Non-breaking spec additions permitted by §13

None needed. Every fix above lands in existing keys or outside the file. Worth stating explicitly:
**H1, M1 and M2 all reduce the producer's output toward what `capture-format-v1.md` already
specifies** — none of them adds, renames, retypes or revocabularises anything, so none moves the
version.

### (d) Escalation / v2

- **`tests/capture-reader/capture_reader.cpp` does not enforce §7 for `verdict` records** (it does
  for `sample` at line 1077 and for `entity_add`/`entity_remove` at line 895). This is why M1
  survived. One line for `docs/escalations.md`: *"The conformance reader — BTB-CAP-5's evidence that
  the spec is complete enough to build a reader from — omits §7's 'verdict records fall inside an
  open segment' rule, so it accepted a producer defect the spec forbids. Reader coverage of §7
  should be completed before EXT-17 builds against it."*
- **BTB-BP-4 AC3, "Drop counts are per-topic, so a loss can be attributed."** The counters are per
  *kind* (sample vs structural), and "structural" merges the entity-event and scenario-event topics.
  D-7 states this substitution as the design ("per-topic drop attribution is kept by counting drops
  per kind") but the FR was never amended to match, and making it truly per-topic would require new
  `trailer.drops` keys — permitted by §13, but it is a contract change EXT-17 should be consulted
  on. One line: *"BTB-BP-4 AC3 says per-topic; the implementation and D-7 are per-kind, with two
  topics merged under `events_not_recorded`. Either amend the FR to per-kind or add per-topic keys
  under §13 with EXT-17's agreement."*

---

## 3. Checked and found correct

- **Determinism, whole path.** No wall-clock value reaches any record: every `steady_clock` read is
  in `HandlerTiming` or the main loop's host-loss window, and the trailer/segment times are
  `lastRecordSimTimeS_`/`lastDataSimTimeS_`, both message-derived. `header.schemas` sorted by
  `message_name`; `sample.fields` walked over `MessageSchema::fields` with a `find()` per field
  (never a `StreamValueMap` iteration); `Verdict::numberValues`/`stringValues` and every picture
  container are `std::map`; `FieldPresence::note` looks up rather than iterates. Binary-mode LF.
  `Json::appendDouble` is `std::to_chars` shortest with a length check and quoted
  `"nan"`/`"inf"`/`"-inf"` fallbacks; no `printf` family anywhere on the writing path, and no
  `setlocale`/`std::locale::global` in `src/` (so `strtod` in `JsonParse` runs in the C locale — the
  comment at `JsonParse.cpp:325-329` says "with the C locale forced", which is inaccurate but the
  behaviour is right).
- **No topic literals.** All four topics come from the registry: entity-state and engine-state via
  `--*-message` → `getByName` → `MessageSchema::topic` with a structural field check and a
  `getByTopic` round-trip; entity- and scenario-events via `EventNames.h` constants →
  `EventConfigReader` vocabulary → message-instance name → `getByName` (the two-hop the field name
  works against). Both event pairs are checked to share a topic. Empty registry, unresolvable name,
  wrong shape and inconsistent index each get a distinct exit code and a named diagnostic.
- **Record shapes against the spec.** Header key order matches §6 exactly (`format_version` first,
  `type` second, `limits` before `part`/`continues_from`, `continues_from` omitted on part 0);
  `sample` matches §8; `verdict` matches §10; `trailer` matches §11 with no `segment` key and
  `continued_in` last and conditional. Sparse-fields rule correct — a declared-but-unsent field
  produces no key, not `null`. String escaping matches §8.3 (short escapes, `\u00XX` below 0x20,
  UTF-8 bytes passed through).
- **The staged-roster flush and the two-segment path.** Roster records arriving with no open segment
  are staged, bounded at 8192 with counted overflow, and flushed in arrival order into the segment
  that opens next; a `sample` forces the segment open (attach-mid-run branch); the empty-name
  `scenario_unloaded` bring-up noise is ignored and counted; a `scenario_loaded` with a segment
  already open closes it as `scenario_unloaded` per D-10; leftover staging at `finish()` gets a
  segment rather than being discarded.
- **Occupancy handling.** `++generation` on every `entity_created` including the first sighting;
  `latest_.erase(name)` on re-creation so a new tenure starts empty; a sample with no open occupancy
  is counted as `samplesOrphaned` and excluded; `orphansBeforeFirstAccepted` frozen at the first
  acceptance, which is what makes `attached_mid_run` causal rather than timing-derived;
  `Referee::onEntityAdd` resets `hasPosition` so ADR-6 holds for proximity. All exercised by
  `entity_picture_test.exe` (72 checks).
- **Live/replay verdict identity.** One `Referee`, two `FieldSource`s, no deciding rule duplicated.
  Both paths anchor end-of-run verdicts on the last *data* record and update that anchor at the same
  points; `drainVerdicts` restamps `segment` at render time so a rotated verdict names a segment its
  own file has; `values` numbers go through the same `json::appendDouble`.
  `JsonFieldSource::tryGeodetic` correctly types from the schema and not the token (`400` parses as
  a double), per §8.3's bolded rule.
- **CAP-6 size accounting.** `wouldBreachBound` is checked against the record's exact length
  *before* the write, with `kCloseReserveBytes` held back, so no line is ever cut; `bytesWritten_`
  is exact (binary mode, one `emit` chokepoint); `render` is correctly re-invoked after a rotation
  so the re-written record names the new part's ordinal; the `rotating_` re-entrancy guard is
  RAII-cleared on all paths; `partFileName` keeps part 0's historical name and `nextRunLabel`'s
  all-digits middle correctly skips `.partNNN` siblings; the `--capture-max-bytes` floor of 16384
  matches the PRD; `counts_` per part vs `runCounts_` per run matches §11's "what is in this file".
- **Shutdown.** Handler does one relaxed `fetch_add` on a `static_assert`-proven lock-free atomic
  and re-arms; everything else happens on the main loop; teardown order is unsubscribe → stop pump →
  close queue → join → trailer, which is the only order that makes "every record enqueued before the
  signal is present" true; arrivals after `close()` are counted separately and kept out of the file
  (§14, §16).
- **CLI and lifecycle.** `--replay`/`--config` rejected together; `--replay` requires
  `--conditions`; `--overflow-policy block` rejected with the reason rather than a bare parse error;
  `--run-label` traversal check; `--out-dir` validated by existence, directory-ness,
  canonicalisation and a write probe before anything is subscribed; conditions loaded and validated
  before any subscription (BTB-REF-1); partial subscription bring-up unwinds the already-taken
  subscription ids; every exit code in `ExitCodes.h` has a logged diagnostic behind it. `Geodesy`
  boundary semantics (`<=` for the circle, explicit edge test before ray casting) match what
  `Geodesy.h` and the README document.
