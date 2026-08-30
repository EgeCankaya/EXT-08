#!/usr/bin/env python3
"""Compare two EXT-08 captures on CONTENT rather than on bytes.

`docs/capture-format-v1.md` §14 tells a consumer that if their host does not publish the same
thing twice, they should "compare on content rather than bytes - for example per-`(entity,
occupancy)` value sequences keyed by `sim_time_s`, which are stable across runs where the raw
byte stream is not."  This is that comparison, written so EXT-17 does not have to invent it.

    python tests/determinism/compare_captures.py <a.n8rocap.jsonl> <b.n8rocap.jsonl>

Why it exists, measured rather than assumed (M7, the R8 spike). Two runs of one scenario on
the **headless** `n8ro-sim-app.exe`, each stopped at exactly frame 1200 rather than after a
wall-clock budget:

    byte comparison   FAILS  - the files differ at line 339, and differ in length
    content comparison PASSES - 50 400 samples present in both at the same
                                (entity, occupancy, sim_time_s), and **all 50 400 carry
                                byte-identical values**. Zero differ.

The two runs diverged only in *which frames were published*: one frame appeared only in run A
and two only in run B, out of about 1 198. So the simulation is reproducible and the
publication schedule is not, and a self-test built on byte equality would report a failure
that is not one.

**A segment whose clock is frozen cannot be content-compared at all**, and finding that out is
the other half of this tool's design.  The engine's stop path resets the simulation clock
before republishing the whole roster, so inside a teardown segment every sample carries
`sim_time_s = 0.0` - about 93 of them per entity in the run measured above.  There is then no
key that distinguishes one from the next, so the Nth sample at t=0 in run A is not necessarily
the same moment as the Nth in run B: lose one early sample and everything after it shifts by
one, and a naive comparison reports 35 "differences" that are really an alignment artifact.

So frozen-clock segments are **detected and excluded**, and reported separately.  A consumer
building a determinism self-test should do the same: compare running segments, where
`sim_time_s` is a real key, and treat a teardown segment as unalignable rather than as
evidence.

Exit codes: 0 if every comparable sample agrees, 1 if any disagrees, 2 on a usage error.
A difference in *which* samples are present is reported but does not fail the comparison -
that is the publication schedule, which this tool exists to see past.
"""

import collections
import json
import sys


def load(path):
    """(entity, occupancy) -> ordered [(sim_time, fields)], from comparable segments only."""
    raw = collections.defaultdict(list)          # segment -> [(track, time, fields)]
    times_per_segment = collections.defaultdict(set)
    counts_per_segment = collections.Counter()
    frames = set()
    total = 0
    header = None
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            record = json.loads(line)
            if record.get("type") == "header":
                header = record
                continue
            if record.get("type") != "sample":
                continue
            total += 1
            time = repr(record["sim_time_s"])
            segment = record["segment"]
            # sort_keys makes the comparison independent of key order, which the capture fixes
            # anyway - so a difference here is a difference in values, never in layout.
            raw[segment].append(((record["entity"], record["occupancy"]), time,
                                 json.dumps(record["fields"], sort_keys=True)))
            times_per_segment[segment].add(time)
            counts_per_segment[segment] += 1
            frames.add(time)

    # A segment is frozen when some entity publishes more than once at the same simulation
    # time. In a running segment each entity publishes once per frame, so that maximum is
    # exactly 1; in the engine's stop-path segment, where the clock has been reset before the
    # roster is republished, it is about 93. That makes the rule exact rather than a threshold
    # to tune. Samples in a frozen segment are unalignable across runs - see the docstring.
    frozen = set()
    for segment, entries in raw.items():
        per_track_time = collections.Counter((track, time) for track, time, _ in entries)
        if per_track_time and max(per_track_time.values()) > 1:
            frozen.add(segment)

    tracks = collections.defaultdict(list)
    excluded = 0
    for segment, entries in raw.items():
        if segment in frozen:
            excluded += counts_per_segment[segment]
            continue
        for track, time, fields in entries:
            tracks[track].append((time, fields))
    return tracks, frames, total, header, sorted(frozen), excluded


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2

    a_tracks, a_frames, a_total, a_header, a_frozen, a_excluded = load(argv[1])
    b_tracks, b_frames, b_total, b_header, b_frozen, b_excluded = load(argv[2])

    print("A  %d samples over %d (entity, occupancy) tracks, %d frames"
          % (a_total, len(a_tracks), len(a_frames)))
    print("B  %d samples over %d (entity, occupancy) tracks, %d frames"
          % (b_total, len(b_tracks), len(b_frames)))
    if a_frozen or b_frozen:
        print("")
        print("frozen-clock segments excluded, as unalignable across runs:")
        print("   A  segment(s) %s - %d samples" % (a_frozen, a_excluded))
        print("   B  segment(s) %s - %d samples" % (b_frozen, b_excluded))
        print("   (the engine's stop path resets the clock, so every sample in one of these")
        print("    carries the same sim_time_s and nothing distinguishes them - see the")
        print("    module docstring, and section 5.1 of the format specification)")

    # The one field that legitimately differs between two hosts is the install path. Compare
    # headers with it excluded, as §14 says to.
    if a_header and b_header:
        for header in (a_header, b_header):
            header.get("platform", {}).pop("model_path", None)
        if json.dumps(a_header, sort_keys=True) == json.dumps(b_header, sort_keys=True):
            print("headers identical (excluding platform.model_path)")
        else:
            print("HEADERS DIFFER - that points at the recorder, not the publisher (see §14)")

    common_tracks = set(a_tracks) & set(b_tracks)
    compared = 0
    agreed = 0
    differing = []
    presence_only = []

    # Within one (entity, occupancy) track, align on sim_time_s but keep the ordering: a
    # teardown segment carries many samples at the same frozen time, and they are distinct
    # observations rather than repeats of one.
    for track in sorted(common_tracks):
        a_seq = a_tracks[track]
        b_seq = b_tracks[track]
        a_by_time = collections.OrderedDict()
        b_by_time = collections.OrderedDict()
        for time, fields in a_seq:
            a_by_time.setdefault(time, []).append(fields)
        for time, fields in b_seq:
            b_by_time.setdefault(time, []).append(fields)

        for time in a_by_time:
            if time not in b_by_time:
                presence_only.append((track, time, len(a_by_time[time])))
                continue
            left = a_by_time[time]
            right = b_by_time[time]
            # Compare position by position over the shorter run; a length difference at one
            # frozen-clock frame is a presence difference, not a disagreement.
            for index in range(min(len(left), len(right))):
                compared += 1
                if left[index] == right[index]:
                    agreed += 1
                elif len(differing) < 3:
                    differing.append((track, time, index, left[index], right[index]))
                else:
                    differing.append(None)
            if len(left) != len(right):
                presence_only.append((track, time, abs(len(left) - len(right))))
        for time in b_by_time:
            if time not in a_by_time:
                presence_only.append((track, time, len(b_by_time[time])))

    only_a_tracks = set(a_tracks) - set(b_tracks)
    only_b_tracks = set(b_tracks) - set(a_tracks)

    print("")
    print("per (entity, occupancy) track, aligned on sim_time_s:")
    print("   tracks in both       %d" % len(common_tracks))
    if only_a_tracks or only_b_tracks:
        print("   tracks only in A     %d" % len(only_a_tracks))
        print("   tracks only in B     %d" % len(only_b_tracks))
    print("   samples compared     %d" % compared)
    print("   values AGREEING      %d" % agreed)
    print("   values DIFFERING     %d" % (compared - agreed))
    print("   present in one only  %d samples, at %d (track, frame) points"
          % (sum(count for _, _, count in presence_only), len(presence_only)))

    if presence_only:
        by_frame = collections.Counter(time for _, time, _ in presence_only)
        print("")
        print("   samples present in one run only, by frame (worst 6):")
        for time, count in by_frame.most_common(6):
            print("      t = %-22s %d track(s)" % (time, count))
        print("   frames touched: %d" % len(by_frame))

    real = [d for d in differing if d is not None]
    if compared != agreed:
        print("")
        print("the first disagreements:")
        for track, time, index, left, right in real[:3]:
            print("   %s at t=%s (copy %d)" % (track, time, index))
            print("      A: %s" % left[:200])
            print("      B: %s" % right[:200])
        print("")
        print("These are two runs producing DIFFERENT VALUES at the same simulation time.")
        print("That is a property of the simulation, not of the recorder.")
        return 1

    print("")
    print("Every comparable sample carries identical values.")
    if presence_only or only_a_tracks or only_b_tracks:
        print("The runs differ only in WHICH samples were published - the simulation is")
        print("reproducible, its publication schedule is not. A byte comparison of these two")
        print("files would fail, and would be reporting the schedule, not the simulation.")
    else:
        print("And both runs contain exactly the same samples.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
