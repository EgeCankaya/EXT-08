# Escalations — findings EXT-08 cannot close itself

Two findings from EXT-08 need a decision or a correction from someone outside this project.
Both are recorded here in full so they can be forwarded as they stand, and so that a reader who
finds them re-derived somewhere else can see they were raised.

Neither blocks EXT-08. The first blocks **EXT-17** outright.

> **Both were sent to the brief's author on 2026-08-31** by this project's DRI, and both remain
> open pending a reply. Nothing below has changed as a result; the status lines say "raised"
> rather than "open" so a later reader can tell "nobody has been told" from "told, awaiting an
> answer".
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

## Not escalated

For completeness, since a reader may expect them here:

- **The 5-minute demo recording** is shot and **published as its four takes**, linked from the
  README with a beat-by-take map. All seven beats were captured on 2026-08-31 following
  `docs/demo-recording-script.md`. Never a decision, and not one now either.
- **BTB-CAP-6** (byte-limited capture, P2) **is now implemented** — see `docs/decisions-m5-m7.md`
  D-38 to D-41 and PRD rev 11. It was a known, recorded gap inside EXT-08's own scope, never a
  decision for anyone else, and it is closed. Worth one line here for EXT-17's author, since it
  touches the shared contract: the capture format **stays frozen at `n8ro-capture/1`**. Four
  keys were added to two existing record types, which §13 makes non-breaking, and every capture
  written before it remains valid and unchanged. A reader that ignores `header.limits`,
  `header.part`, `header.continues_from` and `trailer.continued_in` is still a correct reader.
- **OQ-2's mentor confirmation.** **Closed in EXT-08 at PRD rev 12** (D-42). The headless
  invocation is demonstrated to work (`tests/host-driver/`), which was the half that was ever
  this project's; whether it is the intended production shape is a question about the
  platform's documentation and is carried as **EXT-17's OQ-3**, where the host is actually run
  in production and where `[S2]` asks its own reader to confirm it. Never blocking here, and
  nothing EXT-08 ships changes with the answer.
