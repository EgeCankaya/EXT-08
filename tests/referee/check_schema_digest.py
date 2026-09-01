#!/usr/bin/env python3
r"""Check that docs/condition-file-schema.md still carries README.md's condition sections verbatim.

The digest is the artifact that crosses the repository boundary: a downstream project vendors it
and pins it, and a re-pin is a byte comparison. README.md is where the sections are maintained,
so the two drifting apart is the failure mode this checks for -- and it is exactly how the digest
came to be missing two sections in the first place (EXT-17's E-5).

    python testseferee\check_schema_digest.py

Exit 0 if they agree, 1 with the first differing line if they do not. No simulator, no build.
"""
import sys
from pathlib import Path

FIRST = "### Declaring conditions"
README_END = "## Tests"

root = Path(__file__).resolve().parents[2]


def span(path, first, end=None):
    lines = path.read_text(encoding="utf-8").splitlines()
    try:
        i = lines.index(first)
    except ValueError:
        sys.exit("FAIL: %s does not contain %r" % (path.name, first))
    j = len(lines)
    if end is not None:
        for k in range(i + 1, len(lines)):
            if lines[k] == end:
                j = k
                break
        else:
            sys.exit("FAIL: %s has no %r after %r" % (path.name, end, first))
    while j > i and not lines[j - 1].strip():
        j -= 1
    return lines[i:j]


readme = span(root / "README.md", FIRST, README_END)
digest = span(root / "docs" / "condition-file-schema.md", FIRST)

if readme != digest:
    for n, (a, b) in enumerate(zip(readme, digest), start=1):
        if a != b:
            print("FAIL: first difference at line %d of the span" % n)
            print("  README.md                    : %r" % a)
            print("  docs/condition-file-schema.md: %r" % b)
            sys.exit(1)
    print("FAIL: the spans agree for %d lines and then one ends -- README %d lines, digest %d"
          % (min(len(readme), len(digest)), len(readme), len(digest)))
    sys.exit(1)

print("OK: %d lines identical -- README.md and docs/condition-file-schema.md agree" % len(readme))
