// EXT-08 Bus Telemetry Bridge - M6: re-judging a stored run (BTB-REF-4, ADR-5).
//
// `--replay <capture>` evaluates a condition file against a capture with **no simulator, no
// bus and no client**. That is the cross-repo constraint made executable: an assertion
// written on Tuesday is applied to Monday's twenty runs without re-running one of them.
//
// It is also the strongest available conformance test for the format. The referee here is
// literally the same class the live path drives, fed from `sample.fields` in the file instead
// of from a decoded StreamValueMap. If it reaches the same verdicts, the file demonstrably
// contains enough for a third party - which is the claim `docs/capture-format-v1.md` makes to
// EXT-17 and could not otherwise check.
//
// Version rejection is blunt on purpose: an unrecognised `format_version` is a named error
// and a non-zero exit, never a partial parse (BTB-CAP-5, and section 3 of the format spec).
// Partial parsing of an unknown format is how silently-wrong analysis happens.

#pragma once

#include "Conditions.h"

#include <cstdint>
#include <string>
#include <vector>

namespace n8ro::bridge {

struct ReplayResult {
    std::uint64_t linesRead = 0;
    std::uint64_t samples = 0;
    std::uint64_t entityAdds = 0;
    std::uint64_t entityRemoves = 0;
    std::uint64_t segments = 0;
    std::uint64_t verdictsInInput = 0;   // verdicts the capture already carried, ignored
    std::uint64_t verdictsEmitted = 0;
    std::uint64_t met = 0;
    bool sawTrailer = false;
    std::string formatVersion;
    std::string endReason;
    std::string verdictPath;
};

// Reads `capturePath`, evaluates `conditions`, and writes the verdicts to `verdictPath` - one
// JSON object per line, byte-for-byte what a live run over the same records would have
// written.
//
// Returns false with a named error on: a missing or unreadable file, a first line that is not
// a `header`, an unrecognised `format_version`, a malformed line, or a `sample` outside an
// open segment. Never throws.
[[nodiscard]] bool replay(const std::string& capturePath, const std::string& verdictPath,
                          const std::vector<Condition>& conditions, ReplayResult& result,
                          std::string& error);

}  // namespace n8ro::bridge
