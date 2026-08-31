"""Mutation check for the n8ro-capture/1 conformance reader.

A reader that says CONFORMS is only worth having if it can say the opposite. This takes a
real capture, cuts a small conformant prefix out of it, then introduces one deliberate
defect at a time and asserts that the reader catches each. A mutation that survives is a
check the reader is not really making - the same discipline tests/entity-picture/ is held to.

    python tests/capture-reader/mutate.py <capture.n8rocap.jsonl> <capture_reader.exe> <spec.md>

Exit 0 if every mutation is caught and the unmutated baseline passes.
"""

import json
import os
import subprocess
import sys
import tempfile


def build_baseline(source_path, sample_count=6):
    """A small, valid capture: header, segment_open, N samples, segment_close, trailer."""
    header = None
    segment_open = None
    samples = []
    with open(source_path, "r", encoding="utf-8", newline="") as handle:
        for line in handle:
            record = json.loads(line)
            kind = record.get("type")
            if kind == "header":
                header = line.rstrip("\n")
            elif kind == "segment_open":
                segment_open = line.rstrip("\n")
            elif kind == "sample":
                samples.append(line.rstrip("\n"))
                if len(samples) >= sample_count:
                    break
    if header is None or segment_open is None or len(samples) < sample_count:
        raise SystemExit("source capture did not yield a usable baseline")

    last_time = json.loads(samples[-1])["sim_time_s"]
    segment_close = json.dumps(
        {"type": "segment_close", "sim_time_s": last_time, "segment": 0,
         "scenario": json.loads(segment_open)["scenario"], "reason": "shutdown"},
        separators=(",", ":"))
    trailer = json.dumps(
        {"type": "trailer", "sim_time_s": last_time, "end_reason": "shutdown",
         "counts": {"segments": 1, "samples": len(samples), "entities_added": 0,
                    "entities_removed": 0, "verdicts": 0},
         "drops": {"samples_not_recorded": 0, "samples_orphaned": 0,
                   "samples_unnamed": 0, "samples_untimed": 0},
         "bus_metrics": {"schema_hash_drops": 0, "message_id_drops": 0, "decode_failures": 0,
                         "missing_schema_passthrough": 0, "legacy_payload_passthrough": 0}},
        separators=(",", ":"))
    return [header, segment_open] + samples + [segment_close, trailer]


def dumps(record):
    return json.dumps(record, separators=(",", ":"))


# --- the mutations. Each takes the baseline line list and returns a defective one. --------

def m_unknown_version(lines):
    record = json.loads(lines[0])
    record["format_version"] = "n8ro-capture/99"
    return [dumps(record)] + lines[1:]


def m_version_not_first(lines):
    record = json.loads(lines[0])
    reordered = {"type": "header"}
    for key, value in record.items():
        if key != "type":
            reordered[key] = value
    return [dumps(reordered)] + lines[1:]


def m_field_order_swapped(lines):
    record = json.loads(lines[3])
    keys = list(record["fields"].keys())
    keys[2], keys[3] = keys[3], keys[2]
    record["fields"] = {k: record["fields"][k] for k in keys}
    return lines[:3] + [dumps(record)] + lines[4:]


def m_undeclared_field(lines):
    record = json.loads(lines[3])
    record["fields"]["notInTheSchema"] = 1
    return lines[:3] + [dumps(record)] + lines[4:]


def m_sample_outside_segment(lines):
    return [lines[0]] + lines[2:]          # drop segment_open


def m_wrong_counts(lines):
    record = json.loads(lines[-1])
    record["counts"]["samples"] += 1
    return lines[:-1] + [dumps(record)]


def m_no_trailer(lines):
    return lines[:-1]


def m_record_after_trailer(lines):
    return lines + [lines[3]]


def m_wall_clock(lines):
    record = json.loads(lines[0])
    record["platform"]["recorded_at"] = "2026-08-30T14:22:05Z"
    return [dumps(record)] + lines[1:]


def m_entity_disagrees(lines):
    record = json.loads(lines[3])
    record["entity"] = "SomethingElse"
    return lines[:3] + [dumps(record)] + lines[4:]


def m_bad_segment_reason(lines):
    record = json.loads(lines[-2])
    record["reason"] = "because"
    return lines[:-2] + [dumps(record), lines[-1]]


def m_occupancy_zero(lines):
    record = json.loads(lines[3])
    record["occupancy"] = 0
    return lines[:3] + [dumps(record)] + lines[4:]


def m_sample_after_removal(lines):
    """A sample for an (entity, occupancy) pair after that pair's entity_remove (spec 8.1)."""
    sample = json.loads(lines[3])
    removal = dumps({"type": "entity_remove", "sim_time_s": sample["sim_time_s"], "segment": 0,
                     "entity": sample["entity"], "occupancy": sample["occupancy"],
                     "reason": "destroyed"})
    trailer = json.loads(lines[-1])
    trailer["counts"]["entities_removed"] = 1
    return lines[:3] + [removal] + lines[3:-1] + [dumps(trailer)]


def m_unknown_record_type(lines):
    return lines[:3] + [dumps({"type": "sample_v2", "sim_time_s": 1.0, "segment": 0})] + lines[3:]


def m_missing_schemas(lines):
    record = json.loads(lines[0])
    del record["schemas"]
    return [dumps(record)] + lines[1:]


def m_crlf(lines):
    return lines          # handled specially: written with CRLF terminators


# --- BTB-CAP-6: the bound and the rotation linkage (spec 6.6, 6.7) -----------------------

def m_bad_on_size_limit(lines):
    """header.limits.on_size_limit outside the closed set (spec 6.6)."""
    record = json.loads(lines[0])
    record["limits"] = {"max_bytes": 0, "max_samples": 0, "on_size_limit": "truncate"}
    return [dumps(record)] + lines[1:]


def m_limits_missing_max_bytes(lines):
    """A limits object that states an action but not the bound it applies to (spec 6.6)."""
    record = json.loads(lines[0])
    record["limits"] = {"max_samples": 0, "on_size_limit": "stop"}
    return [dumps(record)] + lines[1:]


def m_continues_from_on_part_zero(lines):
    """A first part that claims a predecessor (spec 6.7)."""
    record = json.loads(lines[0])
    record["part"] = 0
    record["continues_from"] = "capture-something-000.n8rocap.jsonl"
    return [dumps(record)] + lines[1:]


def m_part_without_continues_from(lines):
    """A continuation part with no back-link, so the set cannot be walked (spec 6.7)."""
    record = json.loads(lines[0])
    record["part"] = 2
    record.pop("continues_from", None)
    return [dumps(record)] + lines[1:]


def m_continues_from_is_a_path(lines):
    """A linkage key carrying a host path rather than a bare filename (spec 6.7)."""
    record = json.loads(lines[0])
    record["part"] = 1
    record["continues_from"] = "C:\\captures\\capture-something-000.n8rocap.jsonl"
    return [dumps(record)] + lines[1:]


def m_continued_in_without_size_limit(lines):
    """A file that says it continues but did not end on its size bound (spec 6.7, 11)."""
    record = json.loads(lines[-1])
    record["end_reason"] = "shutdown"
    record["continued_in"] = "capture-something-000.part001.n8rocap.jsonl"
    return lines[:-1] + [dumps(record)]


MUTATIONS = [
    ("unknown format_version is rejected outright", m_unknown_version),
    ("format_version is not the first key", m_version_not_first),
    ("two sample fields swapped out of schema order", m_field_order_swapped),
    ("a field the schema does not declare", m_undeclared_field),
    ("a sample outside any open segment", m_sample_outside_segment),
    ("trailer.counts disagrees with the records present", m_wrong_counts),
    ("no trailer - a truncated capture", m_no_trailer),
    ("a record after the trailer", m_record_after_trailer),
    ("a wall-clock timestamp in the header", m_wall_clock),
    ("envelope entity disagrees with fields.scenarioEntityName", m_entity_disagrees),
    ("segment_close.reason outside the closed set", m_bad_segment_reason),
    ("occupancy below 1", m_occupancy_zero),
    ("a sample after its own occupancy's entity_remove", m_sample_after_removal),
    ("a record type outside the closed vocabulary", m_unknown_record_type),
    ("header with no schemas array", m_missing_schemas),
    ("CRLF line endings", m_crlf),
    ("header.limits.on_size_limit outside the closed set", m_bad_on_size_limit),
    ("header.limits states an action but not the bound", m_limits_missing_max_bytes),
    ("a first part that claims a predecessor", m_continues_from_on_part_zero),
    ("a continuation part with no back-link", m_part_without_continues_from),
    ("continues_from carrying a path, not a filename", m_continues_from_is_a_path),
    ("continued_in on a file that did not end on its size bound",
     m_continued_in_without_size_limit),
]


def run_reader(reader, path, spec):
    result = subprocess.run([reader, path, "--spec", spec], capture_output=True, text=True)
    return result.returncode, result.stdout


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    source, reader, spec = sys.argv[1:4]

    lines = build_baseline(source)
    workdir = tempfile.mkdtemp(prefix="n8rocap-mutate-")
    failures = 0

    baseline_path = os.path.join(workdir, "baseline.n8rocap.jsonl")
    with open(baseline_path, "wb") as handle:
        handle.write(("\n".join(lines) + "\n").encode("utf-8"))
    code, output = run_reader(reader, baseline_path, spec)
    if code == 0:
        print("  PASS  baseline (unmutated) conforms")
    else:
        failures += 1
        print("  FAIL  baseline does not conform - every mutation below is meaningless")
        print(output)

    for index, (name, mutate) in enumerate(MUTATIONS):
        mutated = mutate(list(lines))
        path = os.path.join(workdir, "mutation-%02d.n8rocap.jsonl" % index)
        terminator = "\r\n" if mutate is m_crlf else "\n"
        with open(path, "wb") as handle:
            handle.write((terminator.join(mutated) + terminator).encode("utf-8"))
        code, output = run_reader(reader, path, spec)
        if code != 0:
            print("  PASS  caught: %s" % name)
        else:
            failures += 1
            print("  FAIL  SURVIVED: %s" % name)
            print(output)

    print("\n%d mutations, %d survivors" % (len(MUTATIONS), failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
