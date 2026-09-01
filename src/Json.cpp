#include "Json.h"

#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <system_error>

namespace n8ro::bridge::json {
namespace {

// to_chars shortest never exceeded 24 characters across the 200 000-double corpus in
// tests/float-format/. 32 is that with room to spare, and the length is checked rather
// than assumed - a truncated number is a corrupt capture, not a cosmetic defect.
constexpr std::size_t kNumberBuffer = 32;

constexpr const char* kHexDigits = "0123456789abcdef";

}  // namespace

void appendString(std::string& out, const std::string& text) {
    out.push_back('"');
    for (const char raw : text) {
        const unsigned char byte = static_cast<unsigned char>(raw);
        switch (byte) {
            // Each of these emits the two-character JSON escape, not the byte itself.
            case '"':  out += "\\\"";  continue;
            case '\\': out += "\\\\";  continue;
            case '\b': out += "\\b";   continue;
            case '\f': out += "\\f";   continue;
            case '\n': out += "\\n";   continue;
            case '\r': out += "\\r";   continue;
            case '\t': out += "\\t";   continue;
            default: break;
        }
        if (byte < 0x20) {
            out += "\\u00";
            out.push_back(kHexDigits[(byte >> 4) & 0x0F]);
            out.push_back(kHexDigits[byte & 0x0F]);
            continue;
        }
        // Includes every byte of a UTF-8 sequence. JSON permits them raw, and re-encoding
        // them would be a transformation of a value the capture promises verbatim.
        out.push_back(raw);
    }
    out.push_back('"');
}

std::string quoted(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 2);
    appendString(out, text);
    return out;
}

bool isNonFinite(double value) {
    return !std::isfinite(value);
}

void appendDouble(std::string& out, double value) {
    if (isNonFinite(value)) {
        if (std::isnan(value)) {
            out += "\"nan\"";
        } else {
            out += value > 0.0 ? "\"inf\"" : "\"-inf\"";
        }
        return;
    }

    std::array<char, kNumberBuffer> buffer{};
    // Shortest round-trip. No format argument, no precision argument: that overload is the
    // one specified to produce the shortest text that recovers the identical bit pattern.
    const std::to_chars_result result =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (result.ec != std::errc{}) {
        // Unreachable for a finite double in 32 bytes. Handled rather than asserted,
        // because the platform contract is return values plus logging, never a throw and
        // never an abort in a writer (never throw; PRD C3).
        out += "\"unrepresentable\"";
        return;
    }
    out.append(buffer.data(), static_cast<std::size_t>(result.ptr - buffer.data()));
}

void appendInt(std::string& out, std::int64_t value) {
    std::array<char, kNumberBuffer> buffer{};
    const std::to_chars_result result =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (result.ec != std::errc{}) {
        out += '0';
        return;
    }
    out.append(buffer.data(), static_cast<std::size_t>(result.ptr - buffer.data()));
}

void appendBool(std::string& out, bool value) {
    out += value ? "true" : "false";
}

}  // namespace n8ro::bridge::json
