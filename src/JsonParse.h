// EXT-08 Bus Telemetry Bridge - M6: reading JSON back.
//
// M4 and M5 only ever wrote JSON. M6 has to read it, twice over:
//
//   the condition file   BTB-REF-1 - conditions declared outside the code, so that adding
//                        one needs no rebuild
//   a stored capture     BTB-REF-4 - the referee re-judging a finished run with no
//                        simulator, no bus and no client
//
// The second is the load-bearing one. A capture is the artifact EXT-17 consumes, and replay
// is the strongest available conformance test for the format: if the referee can re-derive
// its own verdicts from the file alone, the file demonstrably contains enough (ADR-5).
//
// Deliberately small. A full DOM over one line of a capture, no streaming, no schema, no
// pointer chasing beyond what a `sample` record needs. It parses the subset of JSON this
// project writes and rejects the rest with a position and a reason - which is the whole
// requirement, because both inputs are files this project or its own specification defines.
//
// Never throws (never throw; PRD C3). Every failure is `false` plus a named error.

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace n8ro::bridge::json {

class Value;
using ValuePtr = std::unique_ptr<Value>;

// A parsed JSON value. Objects keep their members in an ordered map: nothing in the capture
// or verdict path may iterate an unordered container (BTB-CAP-3), and a reader that walks a
// parsed document to re-emit it must produce the same bytes every time.
class Value {
public:
    enum class Kind { Null, Bool, Number, String, Array, Object };

    Kind kind = Kind::Null;
    bool boolean = false;
    double number = 0.0;
    // Numbers are parsed as double, which is what the capture's own doubles are. `isInteger`
    // records whether the token had no fraction and no exponent, so a caller that wants an
    // ordinal can insist on one rather than silently accepting 2.5 as an occupancy.
    bool isInteger = false;
    std::string text;
    std::vector<ValuePtr> elements;
    std::map<std::string, ValuePtr> members;

    // Lookups, never iterations. Return nullptr when absent or when the type does not match,
    // so a caller reads presence rather than assuming it - the same discipline the packed
    // payload readers use, and for the same reason.
    [[nodiscard]] const Value* find(const std::string& key) const;
    [[nodiscard]] const Value* findOfKind(const std::string& key, Kind wanted) const;

    [[nodiscard]] bool getString(const std::string& key, std::string& out) const;
    [[nodiscard]] bool getNumber(const std::string& key, double& out) const;
    [[nodiscard]] bool getUint(const std::string& key, std::uint64_t& out) const;
    [[nodiscard]] bool getBool(const std::string& key, bool& out) const;
};

// Parses one complete JSON document from `text`. Trailing whitespace is allowed; trailing
// anything else is an error, because a capture is one record per line and a second value on
// a line means the line is not what it claims to be.
//
// `error` carries a byte offset and a reason on failure. Nesting is bounded - a hostile or
// corrupt file must not recurse the stack away.
[[nodiscard]] bool parse(const std::string& text, ValuePtr& out, std::string& error);

// The nesting bound. A capture's deepest structure is header.schemas[].fields[]{}, which is
// four levels; a condition file's is conditions[].area.polygon[][], which is five. 64 is
// three orders of magnitude of headroom and still far short of any stack this runs on.
constexpr int kMaxDepth = 64;

}  // namespace n8ro::bridge::json
