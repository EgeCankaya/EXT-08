#!/usr/bin/env python3
"""Trim a real capture down to a committable size, keeping it fully conformant.

A reference run of `Atacama Air Defense` produces about 64 MB of JSON Lines - a real artifact
to produce and check, not one to version.  BTB-DOC-2 still asks for "a sample capture from a
real run" in the repository, so this makes one: **the same file, with most entities' samples
removed and nothing else changed**.

    python tests/evidence/trim_capture.py <in.n8rocap.jsonl> <out.n8rocap.jsonl> \\
        [entity ...]

What it keeps, and why that keeps the file valid:

  * every non-sample record - the header, both segment pairs, every `entity_add` and
    `entity_remove`, every `verdict`.  So the roster picture, the segment structure and the
    referee's conclusions all survive intact.
  * `sample` records for the named entities only.

Every rule the conformance reader enforces still holds afterwards.  Samples stay inside their
segments because segments are untouched; no sample follows its occupancy's `entity_remove`
because removing samples cannot create one; field order is untouched because lines are copied
verbatim rather than re-serialised.  The only thing that has to change is the trailer's
`counts.samples`, which is rewritten to match - and it is rewritten by editing that one number
in the line, not by re-encoding the record, so every other byte of the trailer is preserved.

The result is a file `tests/capture-reader/` reports as CONFORMS, which is the standing check
that this trimming did not quietly invalidate anything.
"""

import json
import sys

# The default selection tells the whole story in three entities:
#   RedUAV_N_01        created, destroyed mid-run at t=149.45, re-created at occupancy 2 -
#                      the end-to-end proof of ADR-6, and the subject of four conditions
#   BlueBase_Airfield  the other half of the proximity condition
#   BlueSAM_ShortRange the launcher whose munitions are removed with reason "expended"
DEFAULT_ENTITIES = ["RedUAV_N_01", "BlueBase_Airfield", "BlueSAM_ShortRange"]


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2

    source, destination = argv[1], argv[2]
    keep = set(argv[3:]) or set(DEFAULT_ENTITIES)

    kept_samples = 0
    dropped_samples = 0
    other_records = 0
    trailer_line = None
    out_lines = []

    with open(source, encoding="utf-8") as handle:
        for line in handle:
            line = line.rstrip("\n")
            if not line:
                continue
            record = json.loads(line)
            kind = record.get("type")
            if kind == "sample":
                if record.get("entity") in keep:
                    kept_samples += 1
                    out_lines.append(line)
                else:
                    dropped_samples += 1
                continue
            if kind == "trailer":
                trailer_line = line
                continue
            other_records += 1
            out_lines.append(line)

    if trailer_line is None:
        print("the source capture has no trailer - it was truncated, and trimming it would "
              "produce a file that claims to be complete")
        return 1

    # Rewrite exactly one number, in place, rather than re-encoding the record. Re-encoding
    # would risk changing key order or float spelling, and the point of this tool is that
    # every byte it does not have to touch is preserved.
    trailer = json.loads(trailer_line)
    original = trailer["counts"]["samples"]
    needle = '"samples":%d' % original
    if needle not in trailer_line:
        print("could not locate %s in the trailer; refusing to guess" % needle)
        return 1
    trailer_line = trailer_line.replace(needle, '"samples":%d' % kept_samples, 1)
    out_lines.append(trailer_line)

    with open(destination, "w", encoding="utf-8", newline="\n") as handle:
        for line in out_lines:
            handle.write(line)
            handle.write("\n")

    print("kept    %d samples for %s" % (kept_samples, ", ".join(sorted(keep))))
    print("dropped %d samples for every other entity" % dropped_samples)
    print("kept    %d non-sample records - header, segments, roster, verdicts" % other_records)
    print("trailer counts.samples rewritten %d -> %d" % (original, kept_samples))
    print("")
    print("wrote %s" % destination)
    print("Check it: build\\tests\\capture_reader.exe %s --spec docs\\capture-format-v1.md"
          % destination)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
