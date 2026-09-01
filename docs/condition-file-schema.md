# The referee's condition-file schema

**Status:** the artifact that crosses the repository boundary. Everything a consumer needs to
write a condition file this referee accepts, and to reproduce any verdict it emits, is in this
file. It is **not** frozen the way `capture-format-v1.md` is — the capture format is a contract
between a producer and every reader of its files, whereas this is a declaration shape a
downstream project may adopt **or supersede** (EXT-08 OQ-6). What it must be is *complete*, and
until 2026-09-01 it was not.

**Why this file exists.** These sections lived only in EXT-08's `README.md`, and a downstream
project vendoring them excerpted the two that declare the shape and stopped one heading before
the two that say what the numbers mean. From the excerpt alone a consumer could reasonably
compute a great-circle distance or a horizontal separation, and would then emit verdicts that
**disagree with this referee's on the same capture while parsing the same file** — in the same
vocabulary, against the same condition ids, with nothing to surface the disagreement. That is
silent divergence across a project boundary, which is what a vendored contract exists to
prevent. Reported by EXT-17 (its E-5); the sections below are now the whole of it, in one file
that can be vendored by identity and checked byte for byte at a re-pin.

**The text below is verbatim from `README.md`** — four consecutive sections, "Declaring
conditions" through "How distance is computed" and its "Boundary semantics". `README.md` remains
where they are maintained; this file is the copy that is meant to be vendored, and
`tests/referee/check_schema_digest.py` fails if the two ever drift apart. If they somehow do,
`README.md` is authoritative.

A working condition file is committed at `conditions/atacama.conditions.json`.

---

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
