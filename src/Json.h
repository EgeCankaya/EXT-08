// EXT-08 Bus Telemetry Bridge - M4: JSON emission primitives for the capture.
//
// Small on purpose. The capture is JSON Lines (ADR-2) and every byte of it is written
// through this header, so the two determinism hazards BTB-CAP-3 names as ours - float
// formatting and locale - are confined to one file that can be reasoned about whole.
//
// Float formatting is OQ-5, resolved by test at M1 and re-stated in BTB-CAP-3 at PRD
// rev 4: std::to_chars shortest round-trip. The printf family is disqualified - "%.17g"
// is round-trip exact and *silently* locale-dependent, emitting `0,05` under a
// comma-decimal locale, which this machine has and which is not JSON. std::to_chars is
// specified to ignore the locale; tests/float-format/ confirms it here rather than taking
// the standard's word for it.
//
// Shortest round-trip rather than 17 fixed digits: it is uniquely determined for a given
// double, so it is stable across runs, builds and machines - which is the property
// BTB-CAP-3 is actually reaching for - while producing shorter output.

#pragma once

#include <cstdint>
#include <string>

namespace n8ro::bridge::json {

// Appends `text` as a JSON string, quotes included. Output is UTF-8: bytes at or above
// 0x20 other than '"' and '\' pass through verbatim, so a multi-byte sequence survives
// unaltered. Control bytes below 0x20 take their short escape where one exists and \u00XX
// otherwise. Never throws.
void appendString(std::string& out, const std::string& text);

[[nodiscard]] std::string quoted(const std::string& text);

// Appends `value` as a JSON number in shortest round-trip form.
//
// A non-finite double has no JSON number spelling, so it is written as one of the three
// quoted tokens "nan", "inf", "-inf" and the format spec requires a reader to accept a
// string in a double-typed field for exactly those. Nothing observed on this platform has
// ever produced one; the rule exists so that if the bus ever carries one, the capture stays
// parseable and says so rather than emitting bare `nan` and corrupting the file.
void appendDouble(std::string& out, double value);

void appendInt(std::string& out, std::int64_t value);

void appendBool(std::string& out, bool value);

// True when `value` had to be written as a quoted token rather than a JSON number.
[[nodiscard]] bool isNonFinite(double value);

}  // namespace n8ro::bridge::json
