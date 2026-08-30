# `n8ro-capture/1` — capture format specification

**Version string:** `n8ro-capture/1`
**Status:** **FROZEN** at the end of EXT-08 M7. From this point a change to what this document
specifies is a version bump, not an edit — see [Versioning and compatibility](#versioning-and-compatibility).
Clarifications that do not change what a conformant file contains (worked examples, warnings,
the producer-conformance section) may still be added.
**Produced by:** EXT-08 Bus Telemetry Bridge (`n8ro-bridge`)
**Platform baseline observed:** N8RO runtime 2.1.328

This document is the whole contract. It is written to be sufficient on its own: a reader for
this format can be implemented from this file with no access to the producer's source, and
that is not an aspiration — the conformance reader in `tests/capture-reader/` was written
from this document and is the check that it stays true.

---

## 1. What a capture is

A capture is a durable, replayable record of what one N8RO simulation run published on the
message bus, plus enough schema information to interpret every value in it without asking
anyone.

Two properties define it, and both are load-bearing:

**It is self-describing.** The first record carries the runtime `MessageSchema` for every
message type the file contains, verbatim — names, types, sizes, order, and the schema's
identity hashes. A reader never needs a compiled-in field list, and a field the platform
adds tomorrow appears in tomorrow's captures with no reader change.

**Its only clock is simulation time.** No wall-clock value appears anywhere in a capture, in
any record, for any reason. That is what lets a consumer attribute a difference between two
captures to the simulation rather than to the recorder: given the same published messages,
the producer emits the same bytes. Note the shape of that guarantee — it binds the recorder,
not the publisher, and whether *your* host publishes the same thing twice is a separate
question you have to answer for yourself. **§14 is where that distinction is spelled out, and
you should read it before building anything that compares two captures.** If you are looking
for the wall-clock time at which something happened, it is in the producer's log, not here,
and that is deliberate.

---

## 2. Container

| Property | Value |
|---|---|
| Encoding | UTF-8, no byte-order mark |
| Structure | JSON Lines — exactly one JSON object per line |
| Line terminator | LF (`0x0A`). Never CRLF, on any platform |
| Final byte | Every record line, including the last, is LF-terminated |
| File extension | `.n8rocap.jsonl` by convention; nothing in the format depends on it |

Each line is a complete, independent JSON object. No line spans a line break, no record is
split, and there is no enclosing array. A reader can therefore process a capture streaming,
one `getline` at a time, and can process a capture larger than memory.

There are no blank lines and no comment lines. A reader encountering one may reject the file.

---

## 3. How to read a capture

In this order. Steps 1 and 2 are mandatory before any other parsing.

1. **Read the first line and parse it as JSON.** If it is not a JSON object, or its `type` is
   not `header`, reject the file: this is not a capture.
2. **Read `format_version`.** It is the object's first key, so a reader that wants to check
   it without a full parse may. If it is not a version this reader implements, **reject the
   file with a named error naming the version found and the versions supported, and stop.**
   Do not attempt a partial parse. This is the entire compatibility mechanism and it is
   deliberately blunt — partial parsing of an unknown format is how silently-wrong analysis
   happens.
3. **Build a schema table** from `header.schemas`, keyed by `message_name`.
4. **Read the remaining lines in order.** Each is a record; dispatch on `type`.
5. **Stop at the `trailer`.** It is the last record of a complete capture. A file whose last
   line is not a `trailer` was truncated — the producer was killed, or the disk filled — and
   a reader should say so rather than treat the file as complete. Everything before the
   truncation point is still valid and may be used.

A reader **must** ignore a record whose `type` it does not recognise rather than fail on it,
*except* that within a given `format_version` the record vocabulary is closed (§4), so an
unrecognised type in a version-matched file indicates a producer defect and should be
reported.

A reader **must** ignore unrecognised **keys** inside a record it does recognise. Adding a
key to an existing record type is a non-breaking change (§13).

---

## 4. The record vocabulary

Exactly eight record types exist in `n8ro-capture/1`. The set is closed: a ninth type is a
new format version, not an addition.

| `type` | Purpose | Occurrences |
|---|---|---|
| `header` | Format version, provenance, and the embedded schemas | Exactly one, first line |
| `segment_open` | A scenario run begins | One per segment |
| `segment_close` | That scenario run ends | Exactly one per `segment_open` |
| `entity_add` | An entity occupancy opens | Zero or more |
| `sample` | One published message, verbatim | Zero or more |
| `entity_remove` | An entity occupancy closes | Zero or more |
| `verdict` | The result of one declared condition | Zero or more |
| `trailer` | The capture closes, with counters | Exactly one, last line |

---

## 5. The common envelope

Every record except `header` begins with the same keys, in this order:

| Key | Type | Present on | Meaning |
|---|---|---|---|
| `type` | string | all records | The record type, from the table above |
| `sim_time_s` | number | all except `header` | Simulation time, in seconds, carried by the event or message that caused this record |
| `segment` | number (integer) | all except `header` and `trailer` | The ordinal of the enclosing segment, from 0 |

`header` has neither `sim_time_s` nor `segment`: it has no cause on the bus to take a time
from, and it precedes every segment. `trailer` has `sim_time_s` but no `segment`: it closes
the file, not a segment, and every segment it covers has already been closed by its own
record.

### 5.1 `sim_time_s` — read this before you sort anything

`sim_time_s` is **the simulation time the record's cause carried**, not the time the record
was written and not a time the producer computed. It is the value the platform published.

Three properties of it will surprise a reader who assumes otherwise, and all three are
observed platform behaviour rather than format choices:

- **It is not monotonic across a segment boundary.** The engine's stop path resets the
  simulation clock *before* it publishes the teardown events, so the records that end a run
  carry `sim_time_s = 0.0` — sorting *before* every sample in the run they end. This affects
  `entity_remove`, and it equally affects **`segment_close`**: the record closing a segment
  whose samples ran to `t = 200.05` is itself stamped `0.0`. **Never sort a capture by
  `sim_time_s` globally.** Sort within a segment if you must sort at all; the file's own
  record order is the authoritative ordering.

  The practical consequence, stated directly because it is the thing a reader gets wrong
  first: **a segment's time extent is `[first sample, last sample]`, never
  `[segment_open.sim_time_s, segment_close.sim_time_s]`.** On a reloaded scenario *both*
  boundary records read `0.0` — the open legitimately, because the clock really is zero at
  load, and the close because of the reset above. Computing a segment's duration from its
  boundary records yields zero for a run of any length.

- **Within a segment, `sample` records are non-decreasing in `sim_time_s`.** That is the
  positive rule the three cautions above are exceptions to, and it is what makes a segment a
  usable unit of analysis. The boundary records — `segment_open`, `segment_close`, and the
  `entity_add` / `entity_remove` records published during a teardown burst — are the stated
  exceptions, and they are exceptions because the platform reset its clock before publishing
  them, not because the producer chose a time for them. The producer never computes a time
  for any record; where a record has no causing message at all (a `segment_close` for
  `host_lost` or `shutdown`, and the `trailer`) it carries the simulation time of the last
  record before it, which is the same rule §11 states for the trailer.
- **It is not unique, and in one case it is very far from unique.** Every entity publishes on
  the same simulation frame, so a 42-entity scenario produces 42 records sharing one
  `sim_time_s` value. That much is expected. What is not: **in the segment the engine's stop
  path opens, the clock is frozen, so one entity can publish dozens of samples all stamped
  `0.0`** — about 93 each in a measured run. Within such a segment `sim_time_s` does not
  identify a moment at all, and `(entity, occupancy, sim_time_s)` is not a key.

  This matters to anyone comparing two captures. Two runs cannot be aligned inside a
  frozen-clock segment: nothing distinguishes one sample from the next, so if one run is
  missing an early one, everything after it shifts by one and a comparison starts matching
  unrelated samples against each other. **Detect such a segment and exclude it.** The test is
  exact rather than a heuristic: in a running segment each entity publishes once per frame, so
  the maximum number of samples any one `(entity, occupancy)` carries at a single `sim_time_s`
  is 1; in a frozen one it is not. §14 says the same thing from the determinism side, and
  `tests/determinism/compare_captures.py` in the producer's repository implements it.
- **It carries accumulated floating-point error, and the capture preserves it exactly.**
  Values such as `35.20000000000014` and `65.74999999999841` are real and correct — a 0.05 s
  frame increment summed a few hundred times. They are not rounded, because rounding them
  would destroy the exactness the capture exists to preserve.

### 5.2 Record ordering

Records appear in the order the producer received their causes, FIFO per topic. That order is
part of the contract: it is what makes the capture a record of a run rather than a bag of
observations. A reader should treat file order as authoritative and must not reorder records
to make times monotonic.

---

## 6. Record: `header`

The first line of every capture. Its keys appear in exactly this order.

| Key | Type | Meaning |
|---|---|---|
| `format_version` | string | `"n8ro-capture/1"`. **Always the first key**, so a reader can check it before parsing further |
| `type` | string | `"header"`. Second key — see the note below the table |
| `producer` | object | The tool that wrote the file — see §6.1 |
| `platform` | object | The configuration the run was recorded under — see §6.2 |
| `attached_mid_run` | boolean | See §6.3 |
| `subscription` | object | The bus-side delivery policy in force — see §6.4 |
| `schemas` | array of object | One entry per message type appearing in this file — see §6.5 |

`header` carries `"type": "header"` like every other record, but it is the **second** key,
not the first: `format_version` must come first so that an unknown version can be rejected
before anything else is parsed, and that outranks the envelope's usual type-first shape. A
reader can still dispatch every record in the file, header included, on `type`.

### 6.1 `producer`

| Key | Type | Meaning |
|---|---|---|
| `name` | string | Tool name, e.g. `"n8ro-bridge"` |
| `version` | string | Tool version, e.g. `"0.4.0"` |

Informational. A reader must not change its behaviour based on it — that is what
`format_version` is for. It exists so that a capture found on disk in six months can be
traced to the build that wrote it.

### 6.2 `platform`

| Key | Type | Meaning |
|---|---|---|
| `engine_config` | string | The engine configuration entry the producer connected through, e.g. `"SimEngineClient_SharedMemory"` |
| `model_path` | string | Directory holding the schema and instance database the schemas were read from |
| `schema_file` | string | Schema name within that database, e.g. `"N8roSimSchema"` |
| `schema_version` | string | The model database's own schema version, as the database reports it. May be empty if the database declares none |
| `runtime_version` | string | The N8RO runtime version, as the runtime's own version accessor reports it. **May be the literal `"unknown"`** — the SDK's accessor is compile-time and returns `"unknown"` when the release headers were built without a version defined, which is the case on the 2.1.328 SDK. `"unknown"` means "the runtime did not tell us", not "the producer failed" |

Every value here is observed at run time, never compiled in. `model_path` is a filesystem
path and is therefore host-specific; two captures of the same scenario recorded on hosts with
different install locations will differ in this one field and nowhere else.

### 6.3 `attached_mid_run`

`true` when the producer attached to a simulation that had **already loaded a scenario**
before the producer had received a single sample.

Why it matters to a reader: the platform publishes its entity-creation events once, in a
burst, at scenario load. A producer that attaches after that burst never sees the roster
being built, so entities appear in `sample` records with no preceding `entity_add`, and the
capture's own view of who is on the field is incomplete through no fault of the file.

**A reader should treat `attached_mid_run: true` as a caveat on completeness**, not as an
error. Entity identity in such a capture is still sound; the roster's origin is simply not in
the file.

### 6.4 `subscription`

| Key | Type | Meaning |
|---|---|---|
| `topic` | string | The bus topic the samples in this file were subscribed from, e.g. `"sim/entity/state"` |
| `backpressure_policy` | string | `"KEEP_LATEST"`, `"FIFO_DROP"`, or `"BLOCK"` |
| `queue_size` | number (integer) | The bus-side delivery queue depth, in messages |

This records the delivery policy the capture was recorded under, because loss characteristics
are a property of the capture and not of the release notes. `KEEP_LATEST` in particular is
lossy in a way that matters to an analyst: under pressure it discards the *older* of two
queued samples — the one already part of the run's history. A reader comparing two captures
should compare this object before concluding anything about a difference between them.

Bus-side loss, where the platform reports it, is in `trailer.bus_metrics`.

### 6.5 `schemas`

An array of schema envelopes, **sorted ascending by `message_name`**. One entry exists for
each message type that appears as a `sample` record's `message` value. Message types the
producer consumed but never wrote out verbatim — for example the entity-lifecycle event
stream behind `entity_add` and `entity_remove` — are **not** listed, because no record in the
file needs them to be interpreted.

Each entry:

| Key | Type | Meaning |
|---|---|---|
| `message_name` | string | The platform's own name for the message type. This is the join key to a `sample` record's `message` |
| `topic` | string | The bus topic this message type travels on |
| `schema_hash` | number (integer) | The platform's schema hash. Two captures whose schemas share a `message_name` but differ in `schema_hash` were recorded against **different** versions of that message and must not be compared field-by-field without checking |
| `message_id` | number (integer) | The platform's numeric message identifier |
| `wire_version` | number (integer) | The packed wire-format version |
| `fields` | array of object | The declared fields, **in declaration order** — see below |

Each element of `fields`:

| Key | Type | Meaning |
|---|---|---|
| `name` | string | Field name. This is the key used inside a `sample` record's `fields` object |
| `type` | string | One of `"int"`, `"double"`, `"string"`, `"bool"` |
| `size` | number (integer) | Element count. `1` is a scalar; a value greater than `1` is a fixed-length array of `size` elements of `type` |

**The order of `fields` is normative and is the single most important thing in the header.**
It is the platform's own declaration order, copied verbatim from the runtime schema. It is
the order in which a `sample` record's `fields` object is written, and a reader that wants to
verify a capture's integrity should check that correspondence. It is *not* alphabetical, and
it is not the order in which any human wrote the fields down.

**A field declared here may never appear in any `sample` record.** That is legal and it is
common — see §8.3.

---

## 7. Records: `segment_open` and `segment_close`

A **segment** is one scenario run inside a capture. An operator who loads a scenario, runs
it, reloads it and runs it again produces one capture with two segments — and without them a
reader would have no way to tell that from a single long run, which would make every
statistic computed over the file meaningless.

### `segment_open`

| Key | Type | Meaning |
|---|---|---|
| `type` | string | `"segment_open"` |
| `sim_time_s` | number | Simulation time of the cause that opened the segment |
| `segment` | number (integer) | The segment ordinal. Starts at 0; strictly increasing within a file; never reused |
| `scenario` | string | The scenario name **as the platform reported it**, not as supplied on a command line. May be empty if the platform had not yet reported one |

### `segment_close`

| Key | Type | Meaning |
|---|---|---|
| `type` | string | `"segment_close"` |
| `sim_time_s` | number | Simulation time of the cause that closed the segment |
| `segment` | number (integer) | The ordinal of the segment being closed. Matches an earlier `segment_open` |
| `scenario` | string | The same scenario name the matching `segment_open` carried |
| `reason` | string | Why it closed — closed set below |

`segment_close.reason` is one of:

| Value | Meaning |
|---|---|
| `scenario_unloaded` | The platform unloaded or reloaded the scenario |
| `host_lost` | The simulation host stopped publishing, exited, or died |
| `shutdown` | The operator stopped the producer |
| `size_limit` | The capture reached its configured size or record bound |

### Segment rules

- Exactly one `segment_close` per `segment_open`.
- Segment ordinals strictly increase and are never reused within a file.
- **No `sample` record appears outside an open segment.** If a reader sees one, the file is
  malformed and the reader should say so.
- `entity_add`, `entity_remove` and `verdict` records also carry `segment` and also fall
  inside an open segment.
- A capture may legitimately contain **zero** segments — if the producer attached, recorded
  nothing, and closed, the file is `header` then `trailer`. That is a valid, complete,
  empty capture.

---

## 8. Record: `sample`

The bulk of a capture. One record per message the producer accepted, carrying every field
that message actually contained, verbatim.

| Key | Type | Meaning |
|---|---|---|
| `type` | string | `"sample"` |
| `sim_time_s` | number | The simulation time this sample carried |
| `segment` | number (integer) | Enclosing segment ordinal |
| `entity` | string | Scenario entity name |
| `occupancy` | number (integer) | Which tenure of that name this sample belongs to — see §8.1 |
| `message` | string | The message type. Join to `header.schemas[].message_name` to get the field declaration |
| `fields` | object | The message's contents — see §8.2 |

### 8.1 `occupancy` — why a name is not an identity

**A scenario entity name does not uniquely identify an entity across a run, and a reader that
assumes it does will be wrong on any capture containing a scenario teardown or a kill.**

The platform reuses names. Two observed cases, both real:

- The engine's stop path deletes every live entity with reason `scenario_unload` and then
  **immediately re-creates the entire roster under the same names.**
- An entity destroyed mid-run can be re-created later under the name it had.

An entity's identity in a capture is therefore the **pair** `(entity, occupancy)`.
`occupancy` is a per-name generation counter starting at 1. It is opened by an `entity_add`
and closed by the matching `entity_remove`, and every `sample` carries the occupancy that was
open when it arrived.

Consequences a reader must handle:

- **Samples under a name may legitimately resume after that name's `entity_remove`** — under
  a *higher* `occupancy`, and only after a new `entity_add` opened it. This is not corruption.
- Within one `(entity, occupancy)` pair, **no `sample` ever appears after that pair's
  `entity_remove`.** That invariant does hold, and it is the one worth asserting.
- Group, join and aggregate on the pair, never on `entity` alone. Two tenures of one name are
  two different things that happen to share a label.

### 8.2 `fields` — verbatim, schema-ordered, and sparse

`fields` is a JSON object whose keys are field names from the message's schema.

Three rules, all normative:

**Order.** The keys appear in the order `header.schemas[].fields` declares them, restricted to
the fields actually present. This is a guarantee about the bytes on disk. A reader using an
order-preserving JSON parse can verify it, and the conformance reader does. A reader using an
ordinary hash-map parse loses the order harmlessly — the header still supplies it.

**Verbatim.** Values are exactly as the platform published them. No unit conversion, no
renaming, no rounding, no curation, no derived fields. If the platform publishes an angle in
radians, the capture carries radians.

**Sparse.** A field the schema declares but the publisher **did not send** is **absent from
the object entirely**. It is not `null`, not `0`, not `""`. `header.schemas` still carries the
full declaration, so a reader can always distinguish:

- *declared and present* — the key is in `fields`
- *declared and never sent* — the key is in the schema and in no `fields` object
- *not declared at all* — the key is in neither, and a reader should reject a `fields` key
  the schema does not declare as a producer defect

This is not a hypothetical accommodation. On runtime 2.1.328 the entity-state message declares
twelve fields and only eleven are ever published: `activeAnimation` appeared zero times in
132 188 samples. A format that defaulted it would have invented 132 188 empty strings and
reported them as data.

### 8.3 Field-value encoding

| Schema `type` | `size` | JSON encoding |
|---|---|---|
| `int` | 1 | JSON number, integral |
| `double` | 1 | JSON number — see below |
| `bool` | 1 | JSON `true` / `false` |
| `string` | 1 | JSON string |
| any | > 1 | JSON array of the corresponding scalar encoding |

**Doubles are written in shortest round-trip form.** The text is the shortest decimal string
that reads back as the identical IEEE-754 bit pattern. It is locale-independent and uniquely
determined for a given double, so the same value always produces the same bytes on every host
and every build.

Two consequences for a reader, both important:

- **A `double` field may be written without a fractional part.** `5` is a valid encoding of
  `5.0`. **Parse any field the schema declares as `double` into a double**, whatever it looks
  like on the line. A reader that types values from the JSON token rather than from the schema
  will silently produce integers for round values.
- **Reading the text back with a correctly-rounding parser recovers the exact original bits.**
  Round-tripping a capture through parse-and-reserialise is lossless.

**Non-finite doubles** have no JSON number spelling. Should the platform ever publish one, it
is written as one of three quoted string tokens — `"nan"`, `"inf"`, `"-inf"` — and a reader
**must** accept a string in a `double`-typed field when and only when it is one of those three.
Nothing observed on this platform has produced one; the rule exists so that if it happens, the
capture stays parseable and says so rather than emitting a bare `nan` and corrupting the file.

**Array length.** A field with `size > 1` is a JSON array. Its length is normally `size`, but
the length is the publisher's, so a reader must accept an array of any length and should report
a mismatch rather than assume one.

**Type mismatch.** A value is encoded from what the publisher actually sent, not coerced into
the declared type. In practice they agree. If they ever disagree, the capture carries the fact
rather than hiding it: a reader should tolerate the mismatch and report it.

**Strings** are UTF-8. `"` and `\` are backslash-escaped; bytes below `0x20` use their short
JSON escape where one exists and `\u00XX` otherwise; every other byte, including every byte of
a multi-byte UTF-8 sequence, is written through unaltered.

### 8.4 The envelope repeats two field values, on purpose

`entity` duplicates the message's own entity-name field, and `sim_time_s` duplicates the
message's own simulation-time field. This is not redundancy to be cleaned up:

- The **envelope** values are what the producer keyed on. They are present on every record of
  every type, so a reader can filter and segment a capture without knowing any message schema.
- The **`fields`** values are the message's contents, verbatim, because `fields` promises to
  be exactly what arrived, and dropping two of its entries because the envelope happens to
  carry them would break that promise for the sake of a few bytes.

They are always equal. A reader may use either; using the envelope is cheaper and works
across message types.

---

## 9. Records: `entity_add` and `entity_remove`

The roster's transitions. Together they bracket every occupancy (§8.1).

### `entity_add`

| Key | Type | Meaning |
|---|---|---|
| `type` | string | `"entity_add"` |
| `sim_time_s` | number | Simulation time the creation event carried |
| `segment` | number (integer) | Enclosing segment ordinal |
| `entity` | string | Scenario entity name |
| `occupancy` | number (integer) | The tenure this record opens. From 1, incrementing per name |

### `entity_remove`

| Key | Type | Meaning |
|---|---|---|
| `type` | string | `"entity_remove"` |
| `sim_time_s` | number | Simulation time the deletion event carried |
| `segment` | number (integer) | Enclosing segment ordinal |
| `entity` | string | Scenario entity name |
| `occupancy` | number (integer) | The tenure this record closes. Matches an earlier `entity_add` for the same name |
| `reason` | string | Why the entity was removed, **verbatim from the platform** |

`reason` values observed on runtime 2.1.328 are `destroyed`, `expended`, `commanded`,
`despawned` and `scenario_unload`. **This list is not closed.** The producer records whatever
string the platform sends, including a supplier-specific value it has never seen, because
coercing an unrecognised reason to a known one destroys the only evidence that something new
happened. **A reader must accept any string here** and must not switch exhaustively on the
five above without a default branch.

Note that `scenario_unload` removals arrive during teardown, after the simulation clock has
been reset — so they carry `sim_time_s = 0.0`. See §5.1.

---

## 10. Record: `verdict`

The result of evaluating one declared condition against the run.

| Key | Type | Meaning |
|---|---|---|
| `type` | string | `"verdict"` |
| `sim_time_s` | number | Simulation time at which the condition was decided |
| `segment` | number (integer) | Enclosing segment ordinal |
| `condition_id` | string | Stable identifier of the condition, from the condition file |
| `met` | boolean | Whether the condition was satisfied |
| `entities` | array of string | Scenario entity names the evaluation involved |
| `values` | object | The values that decided it, sufficient to locate the causing samples. Keys and value types are condition-specific; a reader should treat this as an opaque object and render it rather than interpret it |

**Exactly one `verdict` per declared condition per capture.** A condition is decided once: at
the first moment it is satisfied, or — if it never is — at the end of the run, with
`met: false`. It is not re-emitted on every later sample that also satisfies it, because "did
these two aircraft come within 5 km" is answered by the first time they did.

**A `met: false` verdict is a positive statement, not an absence.** Its presence is what
distinguishes a condition that was evaluated and never satisfied from one nobody evaluated.
A reader that sees fewer verdicts than the condition file declares is looking at a capture
that was cut short, not at a run where the rest passed.

`sim_time_s` on a met verdict is the simulation time of the sample or event that decided it,
so it locates the causing records exactly. On a not-met verdict it is the time of the last
data record in the run, and `segment` the segment that record was in — there is no deciding
moment to point at, and the producer does not invent one.

`values` is condition-specific but always reproducible: numbers are written through the same
round-trip-exact, locale-independent formatter every other double in the capture uses (§8.3),
so a verdict's numbers can be checked by hand against the samples it names.

---

## 11. Record: `trailer`

The last line of a complete capture.

| Key | Type | Meaning |
|---|---|---|
| `type` | string | `"trailer"` |
| `sim_time_s` | number | Simulation time of the last record before it |
| `end_reason` | string | Why the capture ended — closed set below |
| `counts` | object | What this file contains |
| `drops` | object | What the producer did not record, and why |
| `bus_metrics` | object | What the platform's own decoder reported |

`end_reason` is one of:

| Value | Meaning |
|---|---|
| `shutdown` | The operator stopped the producer; the capture is complete to that point |
| `host_lost` | The simulation host stopped publishing or died |
| `size_limit` | A configured size or record bound was reached; recording stopped deliberately |
| `replay_end` | The capture was produced by re-processing another capture, which ended |

`counts` — **what is in this file**, not what happened on the bus:

| Key | Type | Meaning |
|---|---|---|
| `segments` | number (integer) | `segment_open` records in this file |
| `samples` | number (integer) | `sample` records in this file |
| `entities_added` | number (integer) | `entity_add` records in this file |
| `entities_removed` | number (integer) | `entity_remove` records in this file |
| `verdicts` | number (integer) | `verdict` records in this file |

A reader should count the records itself and compare. A disagreement means the file was
truncated, or the producer is defective; either way it is worth reporting.

`drops` — **what the producer received but did not write**. All are producer-side:

| Key | Type | Meaning |
|---|---|---|
| `samples_not_recorded` | number (integer) | Samples dropped because the producer's own buffer or queue was full |
| `events_not_recorded` | number (integer) | Roster and segment records (`entity_add`, `entity_remove`, and the scenario events behind `segment_open` / `segment_close`) the producer could not take, for the same reason. **Added at producer 0.5.0**; a reader must treat it as optional and absent-means-unknown, like the delivery-side `bus_metrics` keys. **A non-zero value here is much more serious than a non-zero `samples_not_recorded`** — it means the file's *structure* is incomplete, not merely its data, so an occupancy may be missing the record that opened or closed it and a segment boundary may be missing entirely. Treat such a file as structurally unreliable rather than merely sampled |
| `samples_orphaned` | number (integer) | Samples for an entity name with no open occupancy — see below |
| `samples_unnamed` | number (integer) | Samples carrying no entity-name field, so unkeyable |
| `samples_untimed` | number (integer) | Samples carrying no simulation-time field, so unstampable |

`samples_orphaned` is the diagnostic that matters most to a reader. A large orphan count with
zero drops and no other error is the signature of a producer that attached *after* scenario
load and missed the entity-creation burst; it usually accompanies `attached_mid_run: true`.
It means the file is a partial view of the run, and it is the difference between a capture
that is empty because nothing happened and one that is empty because nobody was watching.

`bus_metrics` — the platform's own counters, passed through. **Two groups, and the
distinction matters**: a message can be lost before it ever reaches a decoder, in which case
no decode counter can see it.

*Decode side* — the message arrived and could not be turned into values. Present since the
first version of this format; a reader may require these keys.

| Key | Type | Meaning |
|---|---|---|
| `schema_hash_drops` | number (integer) | Messages discarded because their schema hash matched no registered schema |
| `message_id_drops` | number (integer) | Messages discarded on message-id mismatch |
| `decode_failures` | number (integer) | Messages that failed to decode |
| `missing_schema_passthrough` | number (integer) | Messages passed through undecoded for want of a schema |
| `legacy_payload_passthrough` | number (integer) | Messages passed through as legacy payloads |

*Delivery side* — the message never reached the producer's subscription, because the bus
discarded it. Bus-wide rather than per-subscription: the bus does not attribute a discard to a
subscriber, so a non-zero value means the bus lost something, not necessarily something in
this capture. **Added at producer 0.4.2**, so a capture written by an earlier producer may
omit these keys; a reader must treat them as optional and absent-means-unknown, not
absent-means-zero.

| Key | Type | Meaning |
|---|---|---|
| `messages_dropped` | number (integer) | Total messages the bus discarded |
| `dropped_by_backpressure` | number (integer) | Discarded under the subscription's backpressure policy |
| `dropped_by_queue_overflow` | number (integer) | Discarded because a delivery queue was full |
| `dropped_by_rate_limiting` | number (integer) | Discarded by a rate limit |

Any non-zero value on the **decode side** means the producer and the simulation host disagreed
about a schema. A capture recorded under that condition may be missing entire message types
silently — the platform drops them with a warning rather than an error. Any non-zero value on
the **delivery side** means the bus could not keep up. **A reader should surface either
prominently**; the first is the difference between a quiet topic and a misconfigured one, and
the second is the difference between a complete capture and a sampled one.

**All zeros does not mean nothing was lost.** It means nothing the platform counts was lost.
Loss has been measured on this platform with every one of these counters reading zero — see
§14, "Known loss".

---

## 12. Worked example

A three-record capture, reformatted across lines for reading. **In the file each record is one
line**, with no whitespace between tokens.

```json
{"format_version":"n8ro-capture/1","type":"header",
 "producer":{"name":"n8ro-bridge","version":"0.4.0"},
 "platform":{"engine_config":"SimEngineClient_SharedMemory","model_path":"C:\\N8RO\\data\\db",
             "schema_file":"N8roSimSchema","schema_version":"1.0.0","runtime_version":"unknown"},
 "attached_mid_run":false,
 "subscription":{"topic":"sim/entity/state","backpressure_policy":"KEEP_LATEST","queue_size":100},
 "schemas":[{"message_name":"simEntityStateUpdate","topic":"sim/entity/state",
             "schema_hash":2652370635,"message_id":1308183250,"wire_version":1,
             "fields":[{"name":"simulationTime","type":"double","size":1},
                       {"name":"scenarioEntityName","type":"string","size":1},
                       {"name":"name","type":"string","size":1},
                       {"name":"team","type":"string","size":1},
                       {"name":"phase","type":"string","size":1},
                       {"name":"health","type":"string","size":1},
                       {"name":"presence","type":"string","size":1},
                       {"name":"conditions","type":"int","size":1},
                       {"name":"positionGeodetic","type":"double","size":3},
                       {"name":"orientationYprRad","type":"double","size":3},
                       {"name":"velocityNed","type":"double","size":3},
                       {"name":"activeAnimation","type":"string","size":1}]}]}
```

```json
{"type":"segment_open","sim_time_s":0.05,"segment":0,"scenario":"Atacama Air Defense"}
```

```json
{"type":"sample","sim_time_s":35.20000000000014,"segment":0,
 "entity":"TruckLauncher_07_Shahed_03","occupancy":1,"message":"simEntityStateUpdate",
 "fields":{"simulationTime":35.20000000000014,"scenarioEntityName":"TruckLauncher_07_Shahed_03",
           "name":"Air_UAV_Generic","team":"Red","phase":"operational","health":"nominal",
           "presence":"active","conditions":0,
           "positionGeodetic":[-23.5,-68.2,1204.5],
           "orientationYprRad":[1.5707963267948966,0,0],
           "velocityNed":[42.5,0,-1.25]}}
```

Everything the format is for is visible in that one sample record:

- `fields` follows the header's declared order exactly — `simulationTime` first, `velocityNed`
  last, alphabetical nowhere.
- **`activeAnimation` is declared in the header and absent from `fields`.** The publisher did
  not send it. It is not `""` and it is not `null`.
- `conditions` is `0` — a JSON integer, because the schema declares it `int`.
- `orientationYprRad` contains `0`, not `0.0`. It is a `double` field; parse from the schema.
- `sim_time_s` is `35.20000000000014`, preserved to the bit.
- `entity` and `occupancy` together, not `entity` alone, identify the thing being sampled.

---

## 13. Versioning and compatibility

**The version string appears in exactly two places:** the title of this document and the
`format_version` key of the `header` record. They must match, and a test checks it.

**Reader rule.** An unrecognised `format_version` is a rejection with a named error, never a
partial parse (§3, step 2).

**What is a breaking change** — requires `n8ro-capture/2`:

- Adding, removing or renaming a record type
- Renaming or removing a key in an existing record type
- Changing a key's type, unit, or meaning
- Changing the closed vocabulary of `segment_close.reason` or `trailer.end_reason`
- Changing the ordering guarantees of §8.2

**What is not breaking** — stays `n8ro-capture/1`:

- Adding a key to an existing record type. Readers ignore unknown keys (§3)
- A message schema gaining, losing or reordering a field. The header describes it and readers
  key off the header; this is the whole reason the header exists
- A new `entity_remove.reason` value. That vocabulary is explicitly open (§9)

**Old captures are never rewritten.** If `n8ro-capture/2` ships, files written under version 1
remain valid under version 1, and this document remains their specification.

---

## 14. Determinism guarantees

**Read this section before building anything that compares two captures.** The guarantee is
narrower than it is natural to assume, and the difference was measured rather than reasoned
about.

### What the producer guarantees

**The recorder contributes no run-to-run variation of its own.** Given the same sequence of
published messages, it produces the same bytes — on every host, every build, every run.

| Hazard | How it is closed |
|---|---|
| Wall-clock contamination | No wall-clock-derived value is written into any record, in any field, ever |
| Field ordering | `sample.fields` follows the schema's declared field order, never a hash-map iteration order |
| Schema ordering | `header.schemas` is sorted by `message_name` |
| Float formatting | Shortest round-trip, locale-independent, uniquely determined per double |
| Line endings | LF, written in binary mode, on every platform |
| Container iteration | No container with unspecified iteration order is iterated anywhere on the writing path |
| Scheduler-dependent counters | With one deliberate exception, no count whose value depends on thread timing is written into the file; where such a number exists it goes to the producer's log instead. **The exception is `drops.samples_not_recorded` / `drops.events_not_recorded` from producer 0.5.0 on** — see immediately below |
| Timing-dependent flags | `attached_mid_run` is derived from what the message stream contained, never from what a status tick happened to observe |

**The one host-dependent field** is `platform.model_path`, a filesystem path. Two captures of
the same run recorded on hosts with different install locations differ there and nowhere
else; compare with that field excluded.

**The one deliberately scheduler-dependent field** is the pair of internal-queue drop counters
in `trailer.drops`. From producer 0.5.0 the producer streams through a bounded queue, and how
much of it fills depends on how the writer thread was scheduled — so two captures of the same
published stream can differ on those two numbers. That is a knowing trade, not an oversight:
a recorder that lost data and did not say so in the artifact would be worse than one whose
byte comparison needs one caveat. **The caveat is small and self-announcing**: both counters
read `0` on any run where nothing was lost, which is every run that a byte comparison is
meaningful for in the first place. A capture with a non-zero value there is already an
incomplete record of its run, and a determinism test should say so rather than diff it.

Note what stays out of the file. The samples that arrive in the *shutdown window* — between
the producer cancelling its subscriptions and the bus actually stopping delivery — are also
scheduler-dependent, can never be zero, and are **not** counted here. They are not losses;
they are arrivals after the end of recording, which is what the `end_reason` already records.
They go to the producer's log. §16 has the detail.

### What the producer cannot guarantee: the publisher's own repeatability

A capture is a record of what was *published*. If the publisher does not publish the same
thing twice, two captures differ, and no property of the recorder can change that. **Whether
your publisher is repeatable is a question about your host, and it must be answered before
you rely on a byte comparison.**

On the host this producer was developed against, the answer is no. Two runs of
`Atacama Air Defense` recorded under identical configuration on runtime 2.1.328:

- The **headers were byte-identical**, the record counts equal, and the first 30 789 records
  identical.
- They then diverged, because **the host skipped different frames in each run.** Over one
  130-second window it published 2 577 of the 2 601 frames a 0.05 s tick would give — about
  **1 % of frames never published at all**, a different 1 % each run.

**Read that as a fact about one host binary, not about the platform.** The measurement was
taken against `n8ro-sim-local.exe`, a *test driver* that hosts an engine and paces it against
the wall clock for a wall-clock run budget (`--run-ms`). Frame skipping is the expected
behaviour of wall-clock pacing under load.

### The headless host, measured

The obvious follow-up question — does a headless, fixed-step host in a closed configuration do
better? — has now been answered, and the answer is the practically important one.

Two runs of `Atacama Air Defense` on the shipped `n8ro-sim-app.exe`, each stopped at **exactly
frame 1200** rather than after a wall-clock budget, so both cover the same simulation:

| | |
|---|---|
| **Byte comparison** | **fails.** The files differ at line 339 and differ in length |
| **Content comparison**, per `(entity, occupancy)` aligned on `sim_time_s`, running segments only | **50 358 samples compared, 50 358 agree, zero differ** |
| What actually differed | 83 samples, across 4 frames, out of about 1 198 |

**So the simulation is reproducible and its publication schedule is not.** Every sample present
in both runs at the same simulation instant carries byte-identical values; the runs disagree
only about which frames were published at all — roughly 0.2% of them, against `n8ro-sim-local`'s
~1%.

**If you are designing a determinism gate, this is the load-bearing consequence:** a
byte-for-byte comparison of two captures will fail on this platform, and it will be reporting
the publication schedule rather than the simulation. Compare on content. The producer's
repository carries `tests/determinism/compare_captures.py`, which does exactly that and
excludes frozen-clock segments (§5.1) — it exists so this comparison does not have to be
re-derived.

### If you are building a determinism self-test

The distinction above is the whole of the practical advice, so it is worth stating directly:

1. **Establish your host's repeatability first, as its own step**, before building anything
   that depends on it. Run the same configuration twice against the host you will actually
   use, capture both, and compare. This is cheap, and it is the difference between a self-test
   that measures your harness and one that measures the host's pacing.
2. **Use a closed configuration.** Anything externally timed that feeds the simulation makes a
   run reproducible only as far as that input is. *This producer is not such an input* — it
   subscribes and never publishes — but its backpressure policy matters: a recorder configured
   to **block** the bus would stall the publisher and change the run it is recording. This
   producer never uses `BLOCK` for that reason, and `header.subscription.backpressure_policy`
   records what it did use, so a capture always states whether the recorder could have
   perturbed the run.
3. **If your host does not turn out to be repeatable — and on the two measured here, neither
   is byte-repeatable — compare on content rather than bytes.** Per-`(entity, occupancy)` value
   sequences, aligned on `sim_time_s`, are stable across runs where the raw byte stream is not:
   measured at 50 358 of 50 358 agreeing on the headless host.

   Two cautions, both learned by getting them wrong first. **`sim_time_s` is not a key** —
   align on it, but compare *sequences*, because a frozen-clock segment carries dozens of
   samples per entity at one value (§5.1). And **exclude frozen-clock segments entirely**;
   they cannot be aligned across runs, and a comparison that tries reports differences that
   are artifacts of the alignment rather than of the data.
4. **When two captures do differ, find the first differing record rather than reporting only
   that they differ.** The header, the record counts and a long identical prefix are all
   diagnostic: identical headers with divergence deep in the sample stream points at the
   publisher, whereas a difference in the header or in a counter points at the recorder. That
   is exactly how the measurement above was attributed.

### Known loss, and the fact that no counter reports it

**A capture is a very high-fidelity sample of the published stream. It is not a guaranteed-
complete transcript, and the producer cannot always tell you when it is not.** This section
says exactly what has been measured, because the measurement is more useful than the headline.

The method: compare a capture against the simulation host's **own** record of what it
published. `n8ro-sim-local` writes a per-entity JSONL dump in its working directory, produced
inside the host process, independent of the bus, the subscription and this producer. It is the
only available check that does not rely on the counters being checked.

Measured twice at M6, on runtime 2.1.328, comparing per `(entity, sim_time_s)`:

| | reference scenario, 818 samples/s | overload scenario, 2 487 samples/s |
|---|---:|---:|
| samples in the compared window | 131 744 | 135 581 |
| **absent from the capture** | **30 (0.023 %)**, all in **one** frame | **0** |
| absent from the *host's own dump*, though present in the capture | 30, in three frames | 203, in four frames |
| what every counter reported | zero | zero |

Three things follow, and the third is the one to carry away.

**First, the loss is real and it is frame-shaped.** Where the capture is short, it is short by
most of one simulation frame rather than by scattered individual messages — 30 of that frame's
39 samples at the reference rate. M4 saw the same shape: 18 of its 19 missing samples were in
a single frame. Whatever drops them drops a batch.

**Second, it is not driven by rate.** The natural hypothesis was that three times the message
rate would provoke it three times as often. It did the opposite: at 2 487 samples/s the
capture was complete, by the host's own account, across 135 581 samples. Whatever the
mechanism is, throughput is not the trigger, and a consumer should not assume a quiet run is a
safer one.

**Third — and this is new at M6 — the host's own dump loses whole frames too.** It is missing
samples that the capture contains: 30 at the reference rate, 203 under the overload, in the
same whole-frame shape. That has two consequences. It means this comparison bounds the
capture's completeness **from one side only**, so the figures above are an upper bound on the
disagreement rather than a measurement of our loss. And it means a frame-shaped gap appears in
an artifact written *inside the host process*, with no bus and no subscription anywhere in its
path — which is evidence that the mechanism sits upstream of any consumer, and that no
consumer's configuration can avoid it.

**The honest statement for a consumer is therefore unchanged, and now better founded:** a
`bus_metrics` and `drops` block of all zeros means nothing the platform counts was lost. It is
not proof that nothing was lost. A consumer that must know whether a specific message existed
should not infer its absence from this file alone — and, per the third point, should not
expect to establish it from the host's own record either.

---

## 15. Units and frames

The capture applies **no unit conversion**. Values are recorded in whatever units the platform
publishes them, and the authoritative statement of a field's meaning is the platform's own
schema documentation, not this file.

The following is **informative** — an observation of runtime 2.1.328's `simEntityStateUpdate`,
recorded here because a reader will want it and should not have to guess. A reader must not
depend on it: the header describes the fields, and a platform release may change any of this.

| Field | Unit / frame |
|---|---|
| `simulationTime` | seconds since scenario start |
| `positionGeodetic` | `[latitude °, longitude °, altitude m]` |
| `orientationYprRad` | `[yaw, pitch, roll]` in **radians** — the unit is in the field name |
| `velocityNed` | `[north, east, down]` in m/s |
| `conditions` | a bitfield |
| `scenarioEntityName` | the entity's unique name within a tenure — *not* a display name |
| `name` | the entity **profile**, e.g. `Land_AirDefenseRadar_Generic` — *not* a display name |
| `team`, `phase`, `health`, `presence` | platform enumeration strings |

Two caveats worth carrying:

- `phase` begins at `uninitialized` for a freshly created entity, before the platform's systems
  have run a frame over it. That is not an error state.
- Altitudes are **ellipsoidal** where the host's geoid grid is absent, and orthometric where it
  is present. The capture does not record which; the producer's log does. Treat altitude with
  that caveat.

---

## 16. Producer conformance

This section describes **what the current producer emits**, as distinct from what this
specification requires. It exists so that a reader author is never surprised by a real file,
and it shrinks as the producer is completed.

`n8ro-bridge` **0.7.0** (EXT-08 milestone M7) emits **all eight record types**:

| Record | Status |
|---|---|
| `header` | Complete |
| `segment_open` / `segment_close` | Complete. A scenario reload is split into separate segments with distinct ordinals |
| `sample` | Complete |
| `entity_add` / `entity_remove` | Complete. Every occupancy the producer witnessed opening is bracketed by its own pair |
| `verdict` | Complete, when the producer is given a condition file. Without one it evaluates nothing and `trailer.counts.verdicts` is `0`, which is an accurate report of a run that declared no conditions |
| `trailer` | Complete, and all three live `end_reason` values are reachable: `host_lost` for an ordinary run — the producer follows the run rather than deciding when it ends — `shutdown` on an operator interrupt, and `size_limit` if a record budget was configured and reached. `replay_end` is specified but not emitted: replay produces verdicts, not a capture |

A reader written from this specification reads a 0.6.0 capture with no special casing.

**The producer also writes its verdicts a second time**, to a `verdicts-<scenario>-<run-label>
.jsonl` beside the capture, one `verdict` record per line and byte-identical to the ones in
the capture. That file is not part of this format and a reader needs nothing from it; it
exists so that a live run's verdicts and a re-judgement of its own capture can be compared as
files. They are byte-identical, which is the check that this specification carries enough for
a third party to reach the same conclusions from the file alone.

### What a real run's segments look like

Worth stating, because it surprises people and it is not a producer artifact: **an ordinary
single run of a scenario produces two segments, not one.** The engine's stop path unloads the
scenario and then immediately reloads it, re-creating the whole roster under the same names.
So a capture of one run typically contains:

- **segment 0** — the run. Samples from `t = 0.05` to wherever the run ended, the roster's
  `entity_add` records at the front, and any mid-run `entity_remove` records in place. Closed
  with `reason: "scenario_unloaded"`.
- **segment 1** — the teardown reload. The 42 re-created entities' `entity_add` records at
  **occupancy 2**, and a short tail of samples, all stamped `sim_time_s = 0.0` because the
  clock has already been reset. Closed with `reason: "host_lost"` when the host exits.

That second segment is where a reader first meets a real second occupancy, and it is why §8.1
insists that identity is `(entity, occupancy)` and not `entity`.

Two ordering facts a reader may rely on, both measured on runtime 2.1.328 rather than assumed:

- **The `entity_created` burst that materialises a scenario is published *before* the
  `scenario_loaded` that announces it**, at first load and at every reload. The producer holds
  those records and writes them inside the segment the load opens, which is where they belong;
  a reader simply sees `segment_open` followed by the `entity_add` records.
- **No sample of an outgoing run arrives after that run's `scenario_unloaded`.** The boundary
  is clean in that direction, so no sample is ever attributed to the wrong segment.

### `drops.samples_not_recorded` and `drops.events_not_recorded` at this version

**These now carry real values.** At 0.4.x `samples_not_recorded` was structurally `0`, because
the producer recorded into a buffer sized to its own budget and the buffer filling *was* the
end of recording. From 0.5.0 the producer streams through a bounded handler-to-writer queue,
and these two fields are that queue's genuine overflow count — which is what the field was
always reserved for.

Both are `0` on an unloaded run. Under deliberate overload they are not: with the queue
shrunk to four records, a reference-scenario run recorded `samples_not_recorded: 2520` and
**`events_not_recorded: 0`** — the producer reserves queue headroom for roster and segment
records specifically so that overload costs data and never structure. That asymmetry is
deliberate and a reader can lean on it: a capture with dropped samples is a sampled but
structurally sound record of its run.

See §14 for what these two counters mean for a byte comparison. In short: they are the one
deliberately scheduler-dependent thing in the file, they are zero whenever a byte comparison
is meaningful, and a non-zero value is itself the reason not to run one.

**Not counted here, and deliberately:** samples that arrive after the producer has cancelled
its subscriptions but before the bus has stopped delivering. That number can never be zero — a
bus subscription cannot be stopped atomically — and it is scheduler-dependent. It is also not
a loss: those samples are after the end of recording, which the `end_reason` already states.
It goes to the producer's log.

### Producer version history

| Version | Change |
|---|---|
| 0.4.0 | First producer. `drops.samples_not_recorded` carried a scheduler-dependent count and `attached_mid_run` was decided by a timing observation, so both could differ between two identical runs |
| 0.4.1 | Both made deterministic: the field above became structurally `0`, and `attached_mid_run` is now derived from what the message stream contained |
| 0.4.2 | `bus_metrics` gained the four delivery-side counters. Nothing had been reading them, so a capture could report all-zero drops while the bus was discarding messages. Adding keys is non-breaking (§13), so the format version is unchanged |
| 0.5.0 | The writer thread and the bounded queue. `segment_open` / `segment_close` are driven by scenario events, `entity_add` / `entity_remove` are emitted, `drops.samples_not_recorded` carries a real overflow count, and `drops.events_not_recorded` joins it. `end_reason: "host_lost"` is reachable. The bus subscription moved off the `KEEP_LATEST` default to `FIFO_DROP` with a queue of 1024, which `header.subscription` records. Every record type emitted was already specified and only keys were added, so the format version does not move |
| 0.6.0 | `verdict` records are emitted, completing the eight-type vocabulary. Nothing about the format changed — no key renamed, none retyped, no type added that was not already specified — so this is still `n8ro-capture/1` |
| 0.7.0 | Clean interruption: `end_reason: "shutdown"` is reachable, verified over twenty scripted interrupt-and-verify cycles. No format change |

---

## 17. Provenance

The behaviours this document describes as observed — name reuse across occupancies, the
teardown clock reset, the declared-but-never-published field, the locale hazard in float
formatting — were measured on runtime 2.1.328 against the `Atacama Air Defense` scenario and
are recorded with their numbers in the producer's `notes.md`. Where this specification says
"observed", there is a measurement behind it.
