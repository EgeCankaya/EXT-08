# The clean-room record

**What this is.** What was found by auditing this repository against the client brief, preparing
it for handover, and then **cloning it cold and following this README literally** — including the
things that were wrong. Run on 2026-08-31 and 2026-09-01.

**Why it is committed rather than kept in a notebook.** This project's own rule is that a
defective run is kept beside the clean one, because re-running until the numbers are welcome is
choosing evidence. That rule applies to a packaging audit as much as to a capture. **Everything
below was found by executing something, and four of the defects were invisible from the working
directory** — they existed only in what a clone contained.

**Verified versus relayed.** Sections 1 to 4 were measured directly in this repository. Section 5
was measured in the sibling project by its author; it is included because it is about this
repository's other half and the conclusion cuts both ways.

---

## 1. The verdict

EXT-08 **matches the brief** on substance: all eight acceptance criteria, all five rules, all
seven steps, all five deliverables.

**The program was never the problem.** Every defect found in the closing pass was in **how the
repository described itself** or **how it behaved on a machine that was not the development
machine**. That distinction is the whole point of writing this down: the engineering held up to
inspection; the packaging did not, until it was cloned.

---

## 2. Six defects in the documents, all fixed

An audit mapped every line of the brief to the deliverable. **No requirement was missing.** Six
defects were in the documents themselves:

| # | Finding | Why it mattered |
|---|---|---|
| 1 | README claimed the demo shoot's captures were "committed in `captures/`" | They never were — `.gitignore` excludes the directory. The claim invited a reviewer to verify something a clone cannot contain, and it backed three of the seven recorded beats |
| 2 | README said "there is no signal handling until M7, so let the host end the run" | M7 shipped. Ctrl-C drains cleanly and exits 0 (BTB-SD-1). The line steered readers away from a delivered acceptance criterion |
| 3 | README and CLAUDE.md called `escalations.md` "the two findings" | There were five |
| 4 | The recording script listed BTB-CAP-6 as the only unimplemented requirement | Delivered at PRD rev 11 |
| 5 | **The shipped sample capture was producer 0.5.0 while the build was 0.9.0** | It predated `header.sample_form` (0.8.0) and `header.limits` (0.9.0), so the one capture in the repository could not show, from the file itself, either that its samples are published rather than predicted (acceptance criterion 3) or that it was not cut short by a bound |
| 6 | A self-referential README link | Pointed "below" while linking upward |

### The sample capture, regenerated

Re-ran the reference scenario and re-trimmed. Bridge started first: `attached_mid_run=false`,
**132 155 samples, 0 drops, 0 orphans**. Trimmed 3.2 MB to 5.1 MB, two entities to three.

Structure unchanged and the story intact: two segments, 132 `entity_add`, 90 `entity_remove`,
seven verdicts, and `RedUAV_N_01` destroyed at `t = 149.45` returning at occupancy 2. Live and
replay verdicts over the new file are **byte-identical**, and `red-leader-reaches-airfield` still
fires at `distance_m=2999.9981116642175` — bit-for-bit the value the demo footage shows on
camera, so the regeneration did not invalidate any take.

**Defect 5 had a downstream cost nobody here could see, and it is worth following.** EXT-17
vendors this file. The regeneration was not an escalation, not an issue and not a version bump —
so nothing downstream had any way to notice, and EXT-17 carried the 0.5.0 file for three weeks
while its own README named 0.9.0 as the pinned producer. It found this by running its pin check
during the pair test (its F-47/F-49). **A producer-side regeneration is invisible to a consumer
that only watches for escalations.**

---

## 3. Fresh-clone defects — the highest-value findings in the pass

**None was visible from the working directory.** All were found by cloning into a fresh location
and following the README **literally**. All would have consumed an evaluator's first three
minutes.

### 3.1 A fresh clone failed the project's own conformance check

`core.autocrlf=true` is Git for Windows' default and there was no `.gitattributes`, so checkout
rewrote the sample capture to CRLF — violating section 2 of this project's own frozen format
specification (*"LF. Never CRLF, on any platform"*). The first command the README asks a reader
to run printed:

```
FAIL  line 0  [spec 2]  7180 line(s) end in CRLF; the format is LF-terminated
RESULT: 1 conformance failure(s)
```

**The producer was always correct** — it writes clean LF. Only checkout was not. Fixed with a
`.gitattributes` marking `*.n8rocap.jsonl` and `verdicts-*.jsonl` as `-text`, then verified by
cloning again: 0 CR bytes, CONFORMS. The CI job added afterwards checks it on every push, because
a `.gitattributes` line is exactly the kind of thing a later tidy-up deletes.

### 3.2 Test binaries built into the repo root while every run command looked in `build\tests\`

`build/` is git-ignored, so on a fresh clone the run commands failed with "not found".

### 3.3 The Tests section named only `setup.cmd`

`cl` is not on `PATH` after that — `setup-dev.cmd` provides the compiler. A reader following the
README verbatim hit *"cl is not recognized"* before reaching either problem above.

### 3.4 `host_driver.exe` had no build command anywhere

The R8 spike block instructs the reader to run `build\tests\host_driver.exe`. There was no `cl`
line in the README, and the tool was in neither `n8ro-bridge.vcxproj` nor the `.sln`. **R8 is the
spike that established the headless invocation** — the evidence that closed OQ-2 and covers the
brief's first stretch goal. Found by walking the R8 block as a fresh reader would.

Now documented, and verified by deleting the binary and rebuilding from the documented command.
All six test tools have a documented build target, checked mechanically rather than by eye.

---

## 4. The pair test — cloning BOTH repositories, in both orders

EXT-17 ran this on 2026-09-01 and its `docs/clean-room.md` is the full record. Three results
belong on this side.

### 4.1 This README never said where EXT-17 is, and EXT-17's never said where this is

Symmetrical, and neither was findable from inside one repository. This README names EXT-17 eight
times — what it is handed, what it measured downstream, which escalations it raised — and gave no
URL. EXT-17 requires this repository's `n8ro-bridge.exe` as its `--recorder`, devotes a section
titled *"The fourth binary is not in this repository"* to saying so, and named no repository
either.

**Only the order that arrives at a repository FIRST can see its half of this.** Build the
recorder first and you never go looking for it; that is why the pair test is run in both
directions. Both READMEs now carry the link.

### 4.2 The pair needs Visual Studio 18.x, and that had never been stated as a pair-level fact

`n8ro-bridge.vcxproj` pins `<PlatformToolset>v145</PlatformToolset>`. Measured on stock VS 2022
(cl 19.44) with no VS 18 in scope:

```
EXT-08:  error MSB8020: The build tools for v145 (Platform Toolset = 'v145') cannot be found.
EXT-17:  469 checks, 0 failures  - its SDK-free tier builds fine on 17.x
```

`v145` is defined only inside VS 18's `v180` directory; VS 2022 ships MSVC 14.44 and `C:\N8RO`
2.1.328 was built with 14.51. **The pin is correct** — mixing toolsets across an import library
is a bug class neither project should be discovering at 2am — but the consequence is that an
evaluator on stock VS 2022 can run every test EXT-17 has and **cannot build the recorder**, so no
capture, no judgement, no campaign. It is a precondition to state, not a defect to fix. This
README's Requirements section states it; EXT-17's Building section now states the pair-level
version.

### 4.3 E-7, E-8 and E-9 were resolved on EXT-17's ruling — and nothing told EXT-17's repository

All three were raised from here **to** EXT-17 as the consumer of `n8ro-capture/1`, and EXT-17's
author settled all three. All took this project's recommended option, all were documentation-only,
and **no capture byte, record type, key, vocabulary or reader obligation changed** — which is what
kept the format frozen rather than bumped.

- **E-7** — section 14's exclusion list named `platform.model_path` alone. `header.continues_from`
  and `trailer.continued_in` are host-dependent the same way (both embed the run label, which
  defaults to an ordinal derived from `--out-dir`). Section 14 now names all three.
- **E-8** — BTB-BP-4 AC3 asked for per-topic drop counts; the producer counts per **kind**, with
  the two event topics merged. The requirement now says what the producer does, and section 16
  states the merge in writing. Per-topic keys were **declined, not deferred** — they would have
  been keys nobody reads.
- **E-9** — section 10 governs a not-met verdict's `segment` anchor, and sections 10 and 7 now
  both say so, including why the producer does not restamp: a replay can reach only that anchor,
  and restamping would make a live run and a replay of its own capture disagree.

E-7 and E-9 were admitted through section 13's post-freeze clarification table, which they pass on
all three of its tests.

**The part worth keeping is what happened next.** These answers changed the frozen specification,
which EXT-17 vendors — and EXT-17's own escalations file recorded only *outbound* questions, so it
had no row for any of them. It ruled on three questions about the format it consumes and then read
a copy predating its own rulings for three weeks. It found that by running its documented pin
check during the pair test, and widened its rule accordingly: **an escalation a project answers
makes its vendored copy stale exactly as one it raises does.** Nothing on this side needs to
change, but the shape is worth remembering the next time a clarification is issued: **closing an
issue here is not an event anything downstream watches.**

---

## 5. The clean-room test — measured, not assumed

### 5.1 The true zero-install surface is narrower than hoped

Built with `N8RO_RELEASE` cleared and no SDK include paths at all:

| Target | Result |
|---|---|
| `capture_reader` | **builds**, CONFORMS, **23 mutations / 0 survivors** |
| `check_schema_digest.py` | **passes** — 100 lines identical |
| `entity_picture_test`, `referee_test`, `determinism_test` | **fail** — `MessageBusPacked.h` not found |

**The zero-install tier is the format-contract surface only. 213 of 236 checks need SDK headers**,
pulled in by six files: `CaptureFormat.h`, `CaptureRecord.h`, `CaptureWriter.h`, `EntityPicture.h`,
`Referee.h`, `TopicResolution.h`.

**Worth raising with N8RO's owner:** those three suites need SDK **headers only** — no import
libraries, no running install, no services. If a header-only subset were ever distributable, 213
checks become clean-room-able for free. Their call, not ours.

`.github/workflows/zero-install-tier.yml` runs the 23-check surface on a stock `windows-latest`
runner and asserts the runner has no `C:\N8RO` and no `N8RO_RELEASE` before it does. **That is the
only result in this repository that is not self-certified**: every other proof of the boundary is
a check this project wrote, run on the one machine that has the SDK, which can show a reader
compiles without SDK include paths and cannot show the install is unnecessary.

### 5.2 One number in the README had rotted, and the harness found it

The README read *"Sixteen deliberate defects … 16 caught, 0 survivors"*. The harness runs **23**.
It had grown by seven — the `limits` and `continues_from` / `continued_in` cases that arrived with
producer 0.9.0 — and the prose had not. **The number was wrong in the safe direction, which is why
nothing caught it**: it understated a result nobody would dispute, and it is visible only by
running the thing and reading the last line. Corrected, and the CI job now runs the harness on
every push, so the *result* cannot rot silently even though the *prose* did.

### 5.3 What the clean-room test is, and is not

It found four packaging defects that survived a full defect sweep, a delivery audit, and two
document consistency passes — **because those passes all read the repository, and only the clone
read what ships.**

It would **not** have caught the `drop_oldest` eviction bug, the stale 0.5.0 sample, or the three
specification contradictions. **It is the shipping gate, not the definition of done.** Run it
last, and do not let a green result stand in for the audits that found what actually mattered.

---

## 6. Loose ends

- **E-1 and E-2 remain with the brief's author**, unanswered since 2026-08-31. E-1 blocks EXT-17's
  determinism gate — which EXT-17 has since **decided** rather than had answered, on its DRI's
  authority and with its mentor's independent concurrence, and which it is careful to keep
  labelled `decided` rather than `answered`. A ruling would still be acted on.
- **A single five-minute cut does not exist.** The brief asks for "a 5-minute recording" and four
  unassembled takes stand in; accepted by the project owner. Take 2's on-screen counts (7180
  lines, 6945 samples) are from the pre-regeneration sample and now differ from the shipped one
  (11 150 and 10 915); its verdicts are unchanged and byte-identical.
- **AI context was removed for handover.** `CLAUDE.md`, `docs/code-review-2026-09-01.md`,
  `docs/decisions-m5-m7.md` and `docs/demo-recording-script.md` were deleted as
  engineering-process artifacts, and all 31 references to them rewritten rather than left
  dangling. If work continues here, restore the context (`git show bd7565d^:CLAUDE.md`) or
  regenerate it — **most of those decisions turned on a measurement, and the measurements went
  with them.** This file exists partly because that deletion went one file too far: a clean-room
  record is evidence, not process.

---

## 7. Verification record

Every claim above was re-checked after the final change.

| Check | Result |
|---|---|
| `msbuild` Release x64 | exit 0 |
| `entity_picture_test` | 81 checks, 0 failures |
| `referee_test` | 93 checks, 0 failures |
| `determinism_test` | 39 checks, 0 failures |
| `capture_reader` vs sample | **CONFORMS** to `n8ro-capture/1` |
| `mutate.py` | **23 mutations, 0 survivors** |
| `check_schema_digest.py` | OK, 100 lines identical |
| Live 200 s run | 132 155 samples, 0 drops, 0 orphans, exit 0 |
| BTB-REF-4, live vs replay (full and trimmed) | byte-identical |
| Fresh clone | sample checks out LF, CONFORMS |
| Cold clone + build, driven by EXT-17 as `--recorder`, both orders | recorded and judged, both times |
| README anchors and file links | 13 and 13, all resolve |
| Repo-wide dangling doc references | none |
