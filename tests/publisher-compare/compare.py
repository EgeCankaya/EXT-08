#!/usr/bin/env python3
"""Compare an EXT-08 capture against the simulation host's own record of what it published.

This is the only way to answer "did we record everything?" without trusting the counters we
are trying to check.  `n8ro-sim-local` writes `test_artifacts/n8ro-sim-local/
sim_entity_state.jsonl` in its working directory — the publisher's own account, independent
of our bus, our subscription and our code.

It is how R7 was first measured at M4 (19 unexplained samples in 99 981, with every platform
counter reading zero) and it is re-run at M6 under the 126-entity overload scenario, where
the rate is 3x higher and any loss mechanism should be easier to provoke.

    python tests/publisher-compare/compare.py <capture.n8rocap.jsonl> <sim_entity_state.jsonl>

The comparison is scoped to the window the capture covers, per `(entity, simulationTime)`
pair, because the capture starts when the bridge attached and ends when the host stopped
publishing, while the publisher's file covers the host's whole lifetime.

**Samples at `simulationTime == 0.0` are excluded from both sides.**  The engine's stop path
reloads the scenario and re-publishes the whole roster with the clock already reset, so t=0.0
holds two unrelated populations - the run's first frame and the teardown reload's - which the
publisher's flat dump cannot tell apart.  Keying on `(entity, time)` would then match a
teardown sample against a first-frame one and report loss that is really ambiguity.  Excluding
one frame out of ~1200 costs nothing and removes the confusion entirely.
"""

import collections
import json
import sys


def load_capture(path):
    """Returns (published-key set, first sim time, last sim time, trailer)."""
    keys = set()
    lo = None
    hi = None
    trailer = None
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            record = json.loads(line)
            kind = record.get("type")
            if kind == "trailer":
                trailer = record
                continue
            if kind != "sample":
                continue
            time = record["sim_time_s"]
            if time == 0.0:
                # See the module docstring: t=0.0 is ambiguous between the run's first frame
                # and the teardown reload's, on both sides of the comparison.
                continue
            keys.add((record["entity"], repr(time)))
            lo = time if lo is None else min(lo, time)
            hi = time if hi is None else max(hi, time)
    return keys, lo, hi, trailer


def load_publisher(path, lo, hi):
    """The host's own record, scoped to the capture's window."""
    keys = set()
    total = 0
    frames = set()
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError:
                # The host's dump is written live and its tail can be a partial line if the
                # process was killed. Skip it rather than failing the whole comparison.
                continue
            name = record.get("scenarioEntityName")
            time = record.get("simulationTime")
            if name is None or time is None:
                continue
            total += 1
            if time == 0.0:
                continue
            if lo is not None and (time < lo or time > hi):
                continue
            keys.add((name, repr(time)))
            frames.add(repr(time))
    return keys, total, frames


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2

    capture_keys, lo, hi, trailer = load_capture(argv[1])
    publisher_keys, publisher_total, frames = load_publisher(argv[2], lo, hi)

    print("capture window        sim_time_s %r .. %r" % (lo, hi))
    print("published by the host %d in that window (%d in the whole file)"
          % (len(publisher_keys), publisher_total))
    print("present in our capture %d" % len(capture_keys))

    absent = publisher_keys - capture_keys
    extra = capture_keys - publisher_keys

    print("absent from the capture %d  (%.4f%%)"
          % (len(absent), 100.0 * len(absent) / max(1, len(publisher_keys))))
    if extra:
        # We cannot record what was never published, so a non-empty set here does not mean we
        # invented samples - it means the *host's own dump is lossy too*. Measured at M6: it
        # is, in the same whole-frame shape. That matters, because it means this file bounds
        # our completeness from one side only and is not a clean ground truth.
        print("in the capture but NOT in the host's record: %d" % len(extra))

    # Both directions are reported by frame, because the shape of the loss is the finding.
    # Loss scattered thinly across many frames would point at a per-message path; loss
    # concentrated in a few whole frames points at something dropping a batch - and that is
    # what both sides turn out to do.
    for label, group in (("absent from our capture", absent),
                         ("absent from the HOST's own record", extra)):
        if not group:
            continue
        by_frame = collections.Counter(time for _, time in group)
        print("\n%d %s, by simulation frame (worst 10):" % (len(group), label))
        for time, count in by_frame.most_common(10):
            print("   t = %-22s %d sample(s)" % (time, count))
        print("   frames touched: %d of %d in the window" % (len(by_frame), len(frames)))

    if trailer:
        drops = trailer.get("drops", {})
        bus = trailer.get("bus_metrics", {})
        counted = sum(v for v in drops.values() if isinstance(v, (int, float)))
        counted += sum(v for v in bus.values() if isinstance(v, (int, float)))
        print("\nwhat the capture's own counters report as lost: %d" % counted)
        print("   drops       %s" % json.dumps(drops))
        print("   bus_metrics %s" % json.dumps(bus))
        if absent and counted == 0:
            print("\n*** %d samples are absent and every counter reads zero. ***" % len(absent))
            print("That is R7: all-zero counters mean nothing the platform counts was lost,")
            print("not that nothing was lost. See docs/capture-format-v1.md section 14.")
        elif not absent:
            print("\nNothing absent. The capture is a complete transcript of the published")
            print("stream over its window, by the publisher's own account.")

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
