# EXT-08 — shooting script for the 5-minute demo recording (BTB-DOC-2)

> **Shot on 2026-08-31 against producer 0.8.0.** All seven beats below were captured across
> four takes; what remains of BTB-DOC-2 is the edit and a link to the finished file from
> `README.md` §"Reproducing the evidence". This document is kept as the record of what was
> filmed and how to reshoot any take. Results are in §10.

BTB-DOC-2 was the last undelivered P1 requirement. Everything it must show is scripted and
runnable, which is what made it shootable in an afternoon.

**Where this requirement comes from:** both source documents. `EXT-08-Bus-Telemetry-Bridge.docx`
lists "a 5-minute recording of the end-to-end demo" under Deliverables; the PRD carries it
forward as **BTB-DOC-2 (P1)** with `UAC-BTB-DOC-2` in Appendix B. The PRD version is the stricter
one — it names seven specific beats the brief does not. Satisfy the PRD and you satisfy both.

**The seven required beats**, verbatim from BTB-DOC-2:

> start-before-simulator · a scenario load · a reload producing two segments · an entity removal ·
> a verdict firing · the backpressure demonstration · a Ctrl-C with a clean tail

Four takes. Take 1 records for about 3½ minutes and cuts to roughly 2; the rest are short, so
the finished piece lands near 4½. Every command is single-line for pasting into Warp.

---

## 0. Before you roll

### Pick the shell: Command Prompt, not PowerShell

Warp is the terminal, not the shell, and this choice matters more than usual here.
`C:\N8RO\setup.cmd` is a batch file — run it from PowerShell and it executes in a **child `cmd`
process whose variables die when that process exits**: `N8RO_RELEASE`, the `bin\` addition to
`PATH`, the Qt runtime paths. The pane looks fine, and then `n8ro-sim-local.exe` is not found.

Open both panes as **Command Prompt**. Set it from the Command Palette (`Ctrl+Shift+P` → search
"shell") or the `+` / launch-configuration dropdown. A PowerShell fallback is in §5.

### Stage the frame

1. **Two panes side by side.** Command Palette → "Split pane right". Left = bridge,
   right = simulator. Both must be in frame at once — that is what makes beat 1 legible.
2. **Zoom up.** `Ctrl+=` a few times. At 1080p the bridge's status line has to be readable.
3. **Quiet Warp down.** Turn off AI autosuggestions and command inspection for the shoot
   (Settings → AI). Ghost-text completions appearing mid-command are distracting on camera.
4. **Dry-run the Ctrl-C first.** This is the one beat that can fail silently. Start the bridge,
   press `Ctrl+C`, confirm a clean shutdown and exit 0, then throw the take away. Warp on Windows
   passes Ctrl+C through ConPTY as a real console event, so it should work — but find out now,
   not after Take 4. **Nothing selected in the pane**, or Ctrl+C copies instead of interrupting.
5. **Paste, then press Enter as a separate action.** Letting a paste auto-submit in Warp can
   land the command twice, concatenated with no newline between them — which produces a
   bewildering `error: unexpected argument` that has nothing to do with the tool. If you see
   the command echoed twice on one line, that is all it was; just run it again.

### One-time init — run in *both* panes

```
cd /d C:\Projects\EXT-08
```

```
call C:\N8RO\setup.cmd
```

Check it took. This should print `C:\N8RO`:

```
echo %N8RO_RELEASE%
```

Both panes stay in `C:\Projects\EXT-08` for the whole shoot. Every relative path below resolves
from there, and `--out-dir captures` is gitignored, already present and writable.

---

## 1. Take 1 — beats 1–5 (records ~3½ min, cuts to ~2)

Covers start-before-simulator, scenario load, entity removal, a verdict firing, the reload into
two segments, and a clean `host_lost` tail.

> **Run it for 200 seconds, not 60.** A 60 s run is technically valid — it produces two
> segments and a conformant capture — but it is a **weak film**: only one of the seven
> conditions (`airfield-reaches-operational`, at t=0.05) can fire, and only about seven entity
> removals happen during play. The conditions worth watching need simulation time past 100 s:
> `red-leader-crosses-corridor` at t≈103.9, `red-leader-reaches-airfield` at t≈149.05,
> `red-leader-is-destroyed` at t≈149.45. This host runs about 1:1, so `--run-ms 200000` gets
> all of them. Cut the quiet stretch (roughly t=20 to t=100) in editing.

**Step 1.** Start recording. **Terminal 1:**

```
build\x64\Release\n8ro-bridge.exe --config SimEngineClient_SharedMemory --model-path C:\N8RO\data\db --schema-file N8roSimSchema --out-dir captures --conditions conditions\atacama.conditions.json --run-label demo
```

**Step 2. Let it sit for five or six seconds. Do nothing.** It prints
`waiting for a simulation host on sim/engine/state`. This pause **is** beat 1 — the bridge
running with no simulator anywhere. Say so out loud; rush past it and the beat is not in the film.

**Step 3. Terminal 2:**

```
n8ro-sim-local.exe --scenario "Atacama Air Defense" --model-path C:\N8RO\data\db --run-ms 200000
```

**Step 4. Point at the left pane as it goes.** In order: `attached:` → `capture opened:` → the
status line starts ticking. At about `simTime=12` you get `live=42 names=45 … add=45 rm=2`.
**That `rm` counter climbing is the entity-removal beat** — the missiles go `expended` early, so
you do not have to wait for it.

> **Ignore the error flood in Terminal 2.** The simulator will print a stream of
> `[ERROR] (n8ro-sim.scripting.navigation) requestGoTo: the surface a 'agl' altitude is measured
> from did not resolve here…`. Those are **not the bridge**, and they are not new. Two gaps in
> this install cause them, and the DEM is not one of them — `data\terrain\dem\md30_grd.tif` is
> global 30-arc-second coverage:
>
> 1. **No terrain elevation service.** `TerrainElevationServiceClient: Failed to create the
>    shared-memory terrain message bus`. Elevation is served by a separate process over its own
>    bus, and it is not running — so there is no surface to query. This is the gap that produces
>    the errors.
> 2. **No geoid grid.** `data\geoid\` does not exist, so datum conversions fall back to the
>    ellipsoid. This is why altitudes on the bus are ellipsoidal, not orthometric.
>
> With no surface, `tryGroundAltitudeHaeM()` returns false and the documented contract is that
> the caller **must not substitute a value of its own** — so the platform rejects the call. It is
> correct behaviour, and loud only because ~30 UAVs retry every cycle. It does not affect capture
> validity. Mentioning it in one line on camera is better than letting a reviewer wonder. See
> §10 for why you should *not* fix it before filming.

**Step 5. Do not touch anything.** Let the host run out its 200 seconds and exit on its own.
This is the part that is easy to get wrong: the engine's stop path unloads the scenario and
immediately reloads it, and **that reload is what produces segment 1**. Interrupt here and you
lose the two-segment beat.

**Step 6.** Within 3 s the bridge declares host loss and prints its summary. Point at
`segments=2`, `end_reason: host_lost`, the `verdicts` figure in the counts line, and the
`sim_time` block, which now reads per segment:

```
sim_time    samples 0.000000 -> 199.9xxxxx s across 2 segments; last record 0.000000 s
            segment 0: 13xxxx samples, 0.000000 -> 199.9xxxxx s
            segment 1: 3xx samples, 0.000000 -> 0.000000 s
```

**That block is worth narrating** — segment 0 is the run, segment 1 is the teardown reload with
the clock reset to zero, which is the two-segment beat stated in numbers rather than asserted.

**Step 7.** Still rolling, **Terminal 1:**

```
build\tests\capture_reader.exe captures\capture-atacama-air-defense-demo.n8rocap.jsonl --spec docs\capture-format-v1.md
```

`RESULT: CONFORMS`, and its own per-segment `sim_time` block agreeing with the bridge's:

```
  sim_time 0 -> 199.9xxxxx s over 2 segment(s), samples only
           segment 0: 13xxxx samples, 0 -> 199.9xxxxx s
           segment 1: 3xx samples, 0 -> 0 s
```

**Say:** this reader links neither the bridge nor the SDK — it was written from the
specification document alone, and it reaches the same numbers from the file by itself.

---

## 2. Take 2 — verdicts, live equals replay (~45 s)

Not a required beat, but the strongest 45 seconds you have. The referee prints nothing as it
fires — it only writes records — so this is where the verdicts get read out one by one, and
where offline re-judgement is shown to need no simulator, no bus and no client at all.

**Terminal 1**, no simulator needed at all:

```
build\x64\Release\n8ro-bridge.exe --replay docs\sample-capture\capture-atacama-air-defense-sample.n8rocap.jsonl --conditions conditions\atacama.conditions.json --out-dir captures
```

All seven verdicts in about a second — five met, two not met.

**Say the not-met half out loud:** without an explicit not-met verdict, a condition that was
evaluated and never satisfied is indistinguishable from one nobody evaluated. Worth adding that
live and replay verdicts over the same run are byte-identical — one evaluation engine, two field
sources, so it holds by construction rather than by testing (ADR-5).

---

## 3. Take 3 — counted backpressure (~45 s)

**Step 1. Terminal 1** — same as Take 1, minus conditions, plus a deliberately absurd queue:

```
build\x64\Release\n8ro-bridge.exe --config SimEngineClient_SharedMemory --model-path C:\N8RO\data\db --schema-file N8roSimSchema --out-dir captures --run-label overload --queue-size 4
```

**Step 2. Terminal 2** — a short run is plenty:

```
n8ro-sim-local.exe --scenario "Atacama Air Defense" --model-path C:\N8RO\data\db --run-ms 30000
```

**Step 3.** Let it finish. Point at `samplesDropped=` (a few thousand) and `eventsDropped=0` in
the summary, and at the trailer's `drops` block.

**Step 4.** Prove the structure survived:

```
build\tests\capture_reader.exe captures\capture-atacama-air-defense-overload.n8rocap.jsonl --spec docs\capture-format-v1.md
```

**Say:** samples are lost **and counted**; roster and segment records are not, because the queue
reserves headroom only they may use. Overload costs data, never structure — and the file still
conforms.

> **The honest footnote, worth saying on camera.** The default queue is 8192 and the 126-entity
> overload scenario never dropped anything. `--queue-size 4` is three orders of magnitude below
> default. This demonstrates the mechanism; it is not evidence of a real overload.

---

## 4. Take 4 — Ctrl-C with a clean tail (~40 s)

**Step 1. Terminal 1:**

```
build\x64\Release\n8ro-bridge.exe --config SimEngineClient_SharedMemory --model-path C:\N8RO\data\db --schema-file N8roSimSchema --out-dir captures --run-label ctrlc
```

**Step 2. Terminal 2:**

```
n8ro-sim-local.exe --scenario "Atacama Air Defense" --model-path C:\N8RO\data\db --run-ms 60000
```

**Step 3.** Let it capture ~10 seconds — enough that the status line shows real numbers.

**Step 4.** Click into **Terminal 1** and press **`Ctrl+C`** by hand. Nothing selected in the
pane. A real interrupt on camera reads better than a script doing it.

**Step 5.** Immediately, in Terminal 1:

```
echo %ERRORLEVEL%
```

```
powershell -Command "Get-Content captures\capture-atacama-air-defense-ctrlc.n8rocap.jsonl -Tail 1"
```

Exit code `0`, and the last line is a well-formed trailer with `end_reason: shutdown`. Point at
the trailer's own `samples` count.

**Step 6.** Ctrl-C the simulator in Terminal 2 — it is still running out its 60 s.

**Step 7.** Close on the scripted proof, so it does not look like a lucky single take:

```
powershell -ExecutionPolicy Bypass -File tests\shutdown\shutdown_loop.ps1 -Cycles 2
```

Each cycle starts its own simulator, raises a real `CTRL_C_EVENT` on the bridge's console, and
checks exit code, trailer well-formedness, and that the trailer's sample count matches the
records in the file. **Film two cycles and say the committed result is 20 of 20 clean** — do not
film twenty.

---

## 5. If you insist on PowerShell panes

Import the batch environment rather than calling it. One line:

```
cmd /c "call C:\N8RO\setup.cmd && set" | ForEach-Object { if ($_ -match '^([^=]+)=(.*)$') { Set-Item "Env:$($matches[1])" $matches[2] } }
```

`$env:N8RO_RELEASE` should then be `C:\N8RO`. Two further changes to the commands above:

| Command Prompt | PowerShell |
|---|---|
| `cd /d C:\Projects\EXT-08` | `cd C:\Projects\EXT-08` |
| `echo %ERRORLEVEL%` | `$LASTEXITCODE` |
| `powershell -Command "Get-Content … -Tail 1"` | `Get-Content … -Tail 1` |

---

## 6. Beat coverage — check this before calling it done

| Beat (BTB-DOC-2) | Take | What proves it on screen |
|---|---|---|
| Start before simulator | 1, step 2 | `waiting for a simulation host`, no sim running |
| A scenario load | 1, step 4 | `attached:` then `capture opened:`, roster fills to `live=42` |
| A reload producing two segments | 1, step 6 | `segments=2` in the run summary |
| An entity removal | 1, step 4 | `rm=` climbing in the status line |
| A verdict firing | 1 step 6 + Take 2 | verdict count in summary; five met / two not met on replay |
| The backpressure demonstration | 3 | `samplesDropped=` non-zero, `eventsDropped=0`, still CONFORMS |
| Ctrl-C with a clean tail | 4 | exit 0 and a `shutdown` trailer as the last line |

---

## 7. Filenames this shoot writes

All into `captures\`, all confirmed free of collisions before the shoot:

```
capture-atacama-air-defense-demo.n8rocap.jsonl          Take 1
verdicts-atacama-air-defense-demo.jsonl                 Take 1
verdicts-atacama-air-defense-sample.replay.jsonl        Take 2
capture-atacama-air-defense-overload.n8rocap.jsonl      Take 3
capture-atacama-air-defense-ctrlc.n8rocap.jsonl         Take 4
```

**Take 1 will overwrite the 60-second capture already sitting there under the `demo` label.**
That is intended — the reshoot replaces it. Rename it first if you want to keep it.

The two pre-existing captures in that directory (`capture-atacama-000` and
`capture-atacama-air-defense-000`, 112 MB together) are left in place deliberately — the second
is the file the README's own example commands point at in three places, and the capture behind
the recorded M6 "replay of 64 MB in 1.02 s" measurement.

---

## 8. After the shoot

1. Put the file somewhere durable and link it from `README.md` §"Reproducing the evidence" and
   from `docs/decisions-m5-m7.md` D-37's not-delivered list.
2. Flip the status line at `README.md:7` and the **Result** paragraph for M7 in `docs/prd.md`.
3. That closes BTB-DOC-2 and leaves **BTB-CAP-6** (byte-limited capture, P2) as the only
   unimplemented requirement in the PRD.

Still open and not fixable by filming: the PRD's CLI table does not list `--capture-max-samples`,
OQ-2 wants a mentor's confirmation of the headless invocation, and the two findings in
`docs/escalations.md` need a ruling from outside this project.

---

## 9. One change made for this shoot

The first rehearsal take exposed a reporting defect and it was fixed before filming.

**What it did.** The run summary printed `sim_time samples 0.000000 -> 0.000000 s` after a full
60-second run. `CaptureWriter` tracked *first sample written* and *last sample written*, and the
last sample written is always in the teardown segment, where the engine has reset the clock. So
the line read `0.000000 -> 0.000000` on **every complete live run**, hiding the whole run behind
its own teardown. `tests/capture-reader` had the same shape, over all record types rather than
samples, and printed `sim_time 0 -> 0 s` for the same reason.

**What changed.** Both now track min and max **per segment, over `sample` records only**, and
print one line per segment plus the overall span. `src/CaptureWriter.{h,cpp}`, the summary block
in `src/main.cpp`, and `tests/capture-reader/capture_reader.cpp`.

**What did not change.** The capture format. Not one byte of a capture is different — this was
always console output only, so `docs/capture-format-v1.md` stays frozen and the producer version
stays `0.8.0`. Verified after the change: three unit suites pass (72 / 93 / 18 checks, 0
failures), the mutation harness still reports 16 mutations and 0 survivors, replay of the sample
capture still yields 5 met / 2 not met, and the 60-second rehearsal capture still `CONFORMS`.

---

## 10. What the takes produced

Recorded 2026-08-31 against producer 0.8.0. Numbers taken from the run summaries and
re-verified against the capture files.

### Take 1 — the reference run (`--run-ms 200000`)

Reproduces the M3 reference run almost exactly, which is the strongest thing the film can say:

| | This take | M3 reference (README) |
|---|---|---|
| Distinct names | 90 | 90 |
| Occupancies | 132 | 132 |
| Removals | `destroyed:23 expended:48 scenario_unload:19` | identical |
| Samples | 132 364 | 132 188 |
| Drops / orphans | 0 / 0 | 0 / 0 |

The 176-sample difference (0.13%) is the publication-schedule variance R8 measured, not loss.

- `sim_time` 0 → 200 s; segment 0 the run, segment 1 the teardown reload at 0 → 0.
- Five verdicts met, two explicitly not met. `red-leader-reaches-airfield` fired at t=149.05 at
  `distance_m=2999.9981116642175` against a 3000 m threshold — **bit-for-bit the value in the
  committed sample capture**, recomputed independently.
- **ADR-6 end-to-end in a real file:** `RedUAV_N_01` added at occupancy 1, `destroyed` at
  t=149.45, re-added at occupancy 2. 42 names carry more than one occupancy.
- Queue high-water 39 of 9216; handler p95 5 µs against a 100 µs budget; `RESULT: CONFORMS`.

### Take 3 — counted backpressure (`--queue-size 4`)

Samples dropped and counted, roster and segment records preserved, capture still `CONFORMS`.

### Take 4 — Ctrl-C (retake)

`end_reason: shutdown`, one segment, exit 0. Trailer is the last record and its own sample count
(6610) matches the samples in the file; single LF, no CR anywhere. Every loss counter zero.
`shutdown_loop.ps1 -Cycles 2`: 2 of 2 clean.

### A note on the shutdown-loop sample counts

The loop's per-cycle sample counts vary with how many simulators are publishing. A cycle showing
roughly double the expected samples means a simulator from a previous take was still running.
The loop's checks are internal-consistency checks and pass either way, but for a stable number on
film, stop Terminal 2 before running it.

### Known environment caveat, visible on camera

This install runs terrain degraded — no elevation service (`Failed to create the shared-memory
terrain message bus`) and no geoid grid (`data\geoid\` is absent) — so the simulator rejects
`agl` navigation calls and floods Terminal 2 with
`n8ro-sim.scripting.navigation` errors. Pre-existing, documented in the README, and it does not
affect capture validity. **Do not provision terrain to make the film cleaner:** every measurement
in the PRD, `notes.md` and `capture-format-v1.md` §14 was taken in this configuration, and
changing it would make the recording show a different system than the evidence describes.
