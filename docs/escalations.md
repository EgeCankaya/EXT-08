# Escalations — findings EXT-08 cannot close itself

Five findings from EXT-08 needed a decision or a correction from someone outside this project.
**Three are now resolved; two remain open.** All are recorded here in full so they can be
forwarded as they stand, and so that a reader who finds them re-derived somewhere else can see
they were raised.

None blocks EXT-08. The first blocks **EXT-17** outright.

> **E-7, E-8 and E-9 were resolved on 2026-09-01 by EXT-17's author**, who is the consumer each
> was addressed to and therefore the party entitled to settle them. All three took EXT-08's
> recommended option, all three were documentation-only, and **no capture byte, record type,
> key, vocabulary or reader obligation changed** — which is what kept `n8ro-capture/1` frozen
> rather than bumped. E-7 and E-9 are admitted through §13's post-freeze clarification table
> (two new rows); E-8 amended EXT-08's own requirement to match the producer. The findings are
> kept in full below, each with a resolution note, because a resolved finding that vanishes
> leaves the next reader to re-derive it.

> **E-1 and E-2 were sent to the brief's author on 2026-08-31** by this project's DRI, and both
> remain open pending a reply. Nothing below has changed as a result; the status lines say
> "raised" rather than "open" so a later reader can tell "nobody has been told" from "told,
> awaiting an answer". **E-7 to E-9 were put to EXT-17's author on 2026-09-01 and answered the
> same day** — see the resolution note above and each finding's status line.

> **Numbering note.** This file's E-numbers run E-1, E-2, then E-7 onward. **E-3 to E-6 are
> EXT-17's findings against *us*, not ours against anyone** — they arrived on 2026-09-01 and
> carry EXT-17's own numbering. They are not repeated here because this file is the outbound
> list. **Each was answered in a different place, and §13 is only two of the four:**
>
> | | Answered in |
> |---|---|
> | **E-3** — §6.7's rotated-set totals | `docs/capture-format-v1.md` §13's post-freeze clarification table |
> | **E-4** — §5.1's frozen-clock test | `docs/capture-format-v1.md` §13's post-freeze clarification table |
> | **E-5** — the condition digest stopped one heading early | **A new file**, `docs/condition-file-schema.md`, carrying all four sections, plus `tests/referee/check_schema_digest.py` so it cannot drift from the README again. Not a clarification to the format spec and not in §13 |
> | **E-6** — the R8 headless invocation omitted `N8RO_RELEASE` | **This README's R8 preconditions block.** Nothing about it touches the capture format, so it is not in §13 either |
>
> The gap at E-3 to E-6 is what keeps one label from meaning two things **for those numbers, and
> it does not do that for E-1 and E-2** — which predate the shared sequence and are local to
> whichever file they appear in. `EXT-17/docs/escalations.md` numbers its own **E-1** as OQ-3 (the
> headless invocation, to the mentor) and its own **E-2** as OQ-2 (the gate basis) — and *its*
> E-2 is *this* file's **E-1**, one finding under two numbers. **This is stated rather than
> renumbered**: both files are on the record with issues filed against them, and silently
> re-labelling a raised question is worse than a footnote saying which is which.
>
> **E-1's substance has separately been resolved downstream, and that is the outcome it was
> written to produce.** EXT-17's PRD keys its determinism gate on content rather than bytes
> (its CR-DET-1), marks the deviation from its own brief as a named deviation rather than
> laundering it into a client requirement, records it as its ADR-1, and carries the ruling
> request as its own OQ-2. So the recommendation has been adopted by the project that was
> blocked; what is still outstanding is the brief author's ruling, which would either confirm
> that or require a host change or a waiver.

---

## E-1 — EXT-17's determinism gate cannot pass byte-for-byte, and it is a hard stop

**To:** whoever owns EXT-17's brief and its acceptance criteria
**Status:** raised with the brief's author 2026-08-31; awaiting a reply. Needed before EXT-17 passes its milestone 4 — though EXT-17 has adopted the content reading in the meantime and named it as its own decision (its ADR-1 and OQ-2)
**Evidence:** `docs/capture-format-v1.md` §14; PRD risk R8; `tests/determinism/compare_captures.py`

### What the brief requires

`EXT-17-Headless-Campaign-Runner.docx`, step 4:

> **Prove determinism** — the same-configuration self-test above. **Do not build further until
> it passes.**

and its acceptance criterion 2:

> The same-configuration self-test passes: **two identical runs produce identical captures**, and
> the tool checks this itself.

This is not a preference. As written, EXT-17 stops at step 4 until the gate passes.

### What was measured

Two runs of `Atacama Air Defense` on the shipped headless host `n8ro-sim-app.exe`, each stopped
at **exactly frame 1200** — a frame budget, not a wall-clock one, so both runs cover the same
simulation rather than the same duration.

| | |
|---|---|
| Byte comparison | **Fails.** The files differ at line 339 and differ in length |
| Content comparison, per `(entity, occupancy)` aligned on `sim_time_s`, running segments only | **50 358 samples compared, 50 358 agree, zero differ** |
| What actually differed | 83 samples, across 4 frames, out of about 1 198 (~0.2%) |

The wall-clock-paced test driver `n8ro-sim-local.exe` is worse, at ~1% of frames, a different
1% each run.

### What this means

**The property the gate was reaching for holds exactly. The test it specifies does not measure
that property.** Every sample present in both runs at the same simulation instant carries
byte-identical values. The runs disagree only about *which frames were published at all*.

A byte comparison of two captures therefore reports the publication schedule, not the
simulation — and it will fail on this platform every time, for a reason that has nothing to do
with the harness under test or with the thing the gate exists to protect.

### The decision needed

One of:

1. **Restate EXT-17's criterion in terms of content** — per-`(entity, occupancy)` value
   sequences aligned on `sim_time_s`, excluding frozen-clock segments. EXT-08 ships
   `tests/determinism/compare_captures.py`, which is exactly this comparison, so EXT-17 inherits
   it rather than re-deriving it. **This is what EXT-08 recommends.**
2. **Keep the byte criterion and accept that EXT-17 cannot pass step 4 on either available
   host** — which means either a host change or a waiver, decided by a person and not by the
   implementer at milestone 4.

### One caveat to carry with this

EXT-08's risk **R7 is open and unattributed**: a frame-shaped loss of ~19 samples in 99 981
(0.019%), 18 of them in a single frame, with every platform counter reading zero. Two pieces of
evidence point the mechanism away from EXT-08 and upstream of any consumer — at three times the
message rate the absence went to zero across 135 581 samples, and the host's *own in-process
dump*, with no bus anywhere in its path, loses whole frames that EXT-08's capture contains (30
at the reference rate, 203 under overload). But it is not positively localised.

So the attribution above — that the ~0.2% divergence is the publisher's schedule rather than
the recorder's loss — is well-supported and not proven. It does not change the recommendation:
content comparison passes either way. It is disclosed here so the ruling is made on the whole
picture.

---

## E-2 — `EntityStateSample.h` does not exist, and two briefs send implementers to it

**To:** whoever authors the EXT-* briefs
**Status:** raised with the brief's author 2026-08-31; awaiting a reply. A correction to the source documents, not to any code
**Evidence:** PRD ADR-1 and OQ-1; §"Prior art and lessons learned"

### The claim, in two documents

`EXT-08-Bus-Telemetry-Bridge.docx`, in its surface table:

> What an entity update carries — `include\n8ro-sim\infrastructure\EntityStateSample.h`

and in "What the client gives you":

> **The entity picture** — the roster, plus each entity's latest sample: name, team, geodetic
> position, orientation, velocity, acceleration, phase, health, presence, condition flags, and
> the simulation time the sample was published at.

`EXT-17-Headless-Campaign-Runner.docx` carries the same row, for "what a run publishes".

### What is actually in release 2.1.328

- `include\n8ro-sim\infrastructure\` contains **two** headers: `SimulationEngineClient.h` and
  `SimulationEngineHost.h`. There is no `EntityStateSample.h`, anywhere in the tree.
- `SimulationEngineClient` has **no roster accessor and no per-entity sample cache.** What it
  provides is `create()`, `startMessagePump()` / `stopMessagePump()`, the `send*` command
  family, three `subscribe*` entry points taking a **raw** handler over **undecoded** bus
  messages, `unsubscribe()`, local engine-state getters, and asynchronous catalog queries.
- The string `roster` does not appear anywhere under `include\`.
- `IEntityManager` does offer entity lookup, but it is the engine's own in-process manager and
  is unreachable from an out-of-process bus client — which is the shape both briefs specify.

**The entity picture is not something the client gives you. It is something you build.**

### What it cost, and what it will cost again

It was the largest single work item in EXT-08 — budgeted at 2–3 days against a 1–2 week project,
and the one flagged as most likely to breach the target. The brief presents it as free.

EXT-17 would have paid it a second time. Its step 3 says "Capture the run. Subscribe as in
EXT-08", so in practice EXT-17 now inherits EXT-08's capture format and does not need the
picture at all — but only because EXT-08 hit the wall first and wrote down what was on the
other side. A third project starting from the brief alone gets the same surprise.

### What is asked

Correct both documents: remove the `EntityStateSample.h` row, and either drop the
"entity picture" bullet from EXT-08's "What the client gives you" or move it to a "what you will
have to build" list with its real cost attached. If the type is planned for a future release,
saying so is enough — EXT-08's open question OQ-1 asked exactly that and had to be closed by
the implementer without an answer.

---


---

## E-7 — the format spec's determinism guarantee predates rotation and is now incomplete

**To:** EXT-17's author, as the consumer of `n8ro-capture/1`
**Status:** **RESOLVED 2026-09-01** — EXT-17's author took option 1. §14's exclusion paragraph
now names `header.continues_from` and `trailer.continued_in` alongside `platform.model_path`,
and records that neither appears in an unrotated capture and that an explicit `--run-label`
removes both. Admitted through §13's clarification table as a row attributed to E-7. No byte,
key or reader obligation changed. *(Raised 2026-09-01 by the defect sweep.)*
**Evidence:** `docs/capture-format-v1.md` §14 and its "What a size bound does to a byte
comparison" subsection; PRD BTB-CAP-3 (amended at rev 14); `tests/determinism/compare_captures.py`

### What §14 says

> **The one host-dependent field** is `platform.model_path` … compare with that field excluded.

### What is also host-dependent

`header.continues_from` and `trailer.continued_in` embed `<run-label>`, and `<run-label>`
defaults to *the next unused ordinal in `--out-dir`*. Two otherwise byte-identical rotated runs
recorded into the same directory therefore get labels `000` and `001` and differ in those two
keys. The keys were added by BTB-CAP-6 at rev 11 (§6.7); §14's sentence was written before they
existed and was not revisited.

The effect is narrow and worth stating precisely, so nobody reads this as bigger than it is:

- It cannot occur in an **unrotated** capture, which omits both keys. Every capture in this
  repository is unrotated.
- It disappears entirely when `--run-label` is supplied explicitly, which a campaign runner
  addressing runs by path would do anyway.
- It is not variation introduced by the recorder. It is a function of the directory the
  recorder was pointed at — the same class of thing as `platform.model_path`, which is why it
  belongs in the same sentence.

### Why this is not simply edited

`docs/capture-format-v1.md` was frozen at M7. A determinism guarantee is something the document
*specifies*, so restating it is a change to the contract rather than a correction to prose, and
EXT-17 is the party that binds to it. **EXT-08 has therefore stated the exclusion in its own
PRD (BTB-CAP-3, rev 14) and left §14 alone.**

**There is now a mechanism for exactly this, which is why this should be cheap to settle.**
Later on 2026-09-01 — after this finding was written — §13 gained a *"Clarifications made after
the freeze"* table, and two of EXT-17's own findings were admitted through it on the test that a
capture written before is byte-identical to one written after, no key gains or changes a
meaning, and a reader conformant before is conformant after (D-49). **This finding passes that
test on all three counts.** What it lacks is not admissibility but the consumer's agreement,
which is the column that table records. So the ask is narrow: agree it, and it becomes a third
row rather than a debate about a version bump.

### The decision needed

Either:

1. **Amend §14** to list `header.continues_from` and `trailer.continued_in` alongside
   `platform.model_path` as environment-dependent and excluded from a byte comparison. **This is
   what EXT-08 recommends** — it is a one-sentence correction that makes the section true, and
   §13 does not make it a version bump because no key is added, renamed, retyped or
   revocabularised. Or:
2. **Rule that a rotated set is out of scope for a byte comparison altogether**, and say so in
   §14 instead. Defensible, and simpler, if EXT-17 never compares rotated runs.

Either way `tests/determinism/compare_captures.py` is unaffected: it compares content, not
bytes, and reads neither key.

---

## E-8 — BTB-BP-4 AC3 says per-topic drop counts; the producer counts per kind

**To:** EXT-17's author, as the reader of `trailer.drops`
**Status:** **RESOLVED 2026-09-01** — EXT-17's author took option 1. BTB-BP-4 AC3 now reads
*per kind* and says a loss is attributable to data or to structure; `docs/capture-format-v1.md`
§16 states in writing that the two event topics are merged under `events_not_recorded`. Per-topic
keys were declined as keys nobody reads. No producer change, no format change. *(Raised
2026-09-01 by the defect sweep; the substitution itself was made at M5.)*
**Evidence:** PRD BTB-BP-4 AC3; `docs/capture-format-v1.md` §11
and §16

### The disagreement

BTB-BP-4's third acceptance criterion:

> Drop counts are **per-topic**, so a loss can be attributed.

The trailer carries two: `drops.samples_not_recorded` and `drops.events_not_recorded`. Those are
per **kind** — sample versus structural — and "structural" merges the entity-event topic and the
scenario-event topic under one number. D-7 states the substitution as the design and gives the
reason (one queue, so that arrival order across topics is preserved; the drop counters follow
the queue's own partition, which is the reserve's partition). The FR was not amended.

In practice this has cost nothing: `events_not_recorded` is 0 on every run recorded so far, and
after the `drop_oldest` fix in PRD rev 14 it cannot be non-zero from overload alone. The
attribution the criterion asks for is only interesting when the number is not zero.

### The decision needed

One of:

1. **Amend BTB-BP-4 AC3 to per-kind**, and say in §16 that the two event topics are merged.
   Costs nothing, changes no byte, and makes the two documents agree. **EXT-08's
   recommendation**, unless EXT-17 has a use for the split.
2. **Add per-topic keys under `trailer.drops`.** §13 makes added keys non-breaking, so this does
   not move the format version — but it *is* a contract change to a record EXT-17 parses, and
   adding keys nobody reads is how a format accumulates. Only worth doing if EXT-17 will
   attribute a loss to one of the two event topics specifically.

EXT-08 will implement (2) if asked; it is a small change to `RecordQueue`'s counters and to
`CaptureFormat::writeTrailer`.

---

## E-9 — §10 and §5.2 disagree about a not-met verdict's `segment` across a rotation

**To:** EXT-17's author, as the consumer of `verdict` records
**Status:** **RESOLVED 2026-09-01** — EXT-17's author took option 1. §10 governs and now says so
explicitly, including why the producer does not restamp; §7's segment rules carry the same
statement from the other side, with the `(part, segment)` guidance for a reader keying per
segment. Admitted through §13's clarification table as a row attributed to E-9. BTB-REF-4's
byte-identity is untouched. *(Raised 2026-09-01 by the defect sweep.)*
**Evidence:** `docs/capture-format-v1.md` §5.2, §7, §10

### The two rules

§10, on a not-met verdict:

> On a not-met verdict it is the time of the last data record in the run, and `segment` **the
> segment that record was in** — there is no deciding moment to point at, and the producer does
> not invent one.

§5.2 and §7, on every record:

> A reader should treat file order as authoritative … `entity_add`, `entity_remove` and
> `verdict` records also carry `segment` and also fall inside an open segment.

and §7 again, on rotation: ordinals "restart at 0 in every part, so a statistic computed per
segment across a set must key on `(part, segment)`".

### Where they collide

Almost nowhere — the two rules name the same ordinal in every ordinary run, which is why the
conflict was not visible until the sweep traced it. They part company in exactly one place: a
run that rotates and whose **last data record is in the previous part**. §10 says the verdict
carries that part's ordinal; §5.2/§7 say it must name a segment its own file has. The new part's
ordinals restart at 0, so the two answers differ.

`drainVerdicts` already restamps *mid-run* verdicts at render time for the §5.2 reason. The
end-of-run verdicts deliberately do not, because they are the ones BTB-REF-4 compares against a
replay, and a replay reading the finished file can only reach §10's anchor. Restamping them
would make a live run and a replay of its own capture disagree, which is the one invariant
ADR-5 exists to protect.

### The decision needed

One of:

1. **§10 wins, and §7 gains a sentence** saying a not-met verdict's `segment` may name a segment
   in an earlier part of a rotated set, and that a reader keying per segment should treat it as
   `(part, segment)` from the trailer chain. **EXT-08's recommendation** — it costs no code and
   keeps BTB-REF-4 exact.
2. **§5.2 wins**, the producer restamps end-of-run verdicts to the enclosing segment, and
   BTB-REF-4's byte-identity is re-derived by teaching the replay path the same rule — which
   means the deciding rule exists in two places, which is the thing ADR-5 says
   not to do.

Until it is decided the producer follows §10, which is the more specific rule and the one
written about this exact record.

---

## Not escalated

For completeness, since a reader may expect them here:

- **The 5-minute demo recording** is shot and **published as its four takes**, linked from the
  README with a beat-by-take map. All seven beats were captured on 2026-08-31 following
  the shooting script. Never a decision, and not one now either.
- **BTB-CAP-6** (byte-limited capture, P2) **is now implemented** — see PRD rev 11
  D-38 to D-41 and PRD rev 11. It was a known, recorded gap inside EXT-08's own scope, never a
  decision for anyone else, and it is closed. Worth one line here for EXT-17's author, since it
  touches the shared contract: the capture format **stays frozen at `n8ro-capture/1`**. Four
  keys were added to two existing record types, which §13 makes non-breaking, and every capture
  written before it remains valid and unchanged. A reader that ignores `header.limits`,
  `header.part`, `header.continues_from` and `trailer.continued_in` is still a correct reader.
- **The conformance reader's missing §7 rule for `verdict` records.** The review that opened
  E-7 to E-9 also found that `tests/capture-reader/` — BTB-CAP-5's evidence that the spec is
  complete enough to build a reader from — enforced §7's "falls inside an open segment" rule for
  `sample` and for `entity_add` / `entity_remove` but **not** for `verdict`, which is why it
  accepted a producer defect the spec forbids. **Closed in EXT-08 on 2026-09-01**, not
  escalated: the rule is one comparison, the producer defect it was hiding is fixed in the same
  change (D-44), and the mutation harness gains a case for it — 23 mutations, 0 survivors. Worth
  one line here because EXT-17 builds a reader against the same section, and this is the rule
  that was easiest to miss.
- **OQ-2's mentor confirmation.** **Closed in EXT-08 at PRD rev 12** (D-42). The headless
  invocation is demonstrated to work (`tests/host-driver/`), which was the half that was ever
  this project's; whether it is the intended production shape is a question about the
  platform's documentation and is carried as **EXT-17's OQ-3**, where the host is actually run
  in production and where `[S2]` asks its own reader to confirm it. Never blocking here, and
  nothing EXT-08 ships changes with the answer.
