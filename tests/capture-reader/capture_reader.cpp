// EXT-08 - a conformance reader for the `n8ro-capture/1` capture format.
//
// WRITTEN FROM docs/capture-format-v1.md, NOT FROM THE PRODUCER'S SOURCE. That is the whole
// point of it. BTB-CAP-5 requires the format specification to be complete enough that a
// reader can be built from it alone, and the only way to know whether that is true is to
// build one that way and see what the document fails to say. Every rule checked below cites
// the section of the spec it comes from, so a rule with no citation is a rule the reader
// invented - and there are none.
//
// It therefore includes NOTHING from src/ and NOTHING from the N8RO SDK. Standard library
// only. It does not link the bridge, does not know the bridge's types, and would work just
// as well against a capture written by a different producer entirely - which is what EXT-17
// will be.
//
// Build (no N8RO environment needed):
//     cl /std:c++17 /EHsc /W4 /O2 /Fe:capture_reader.exe tests\capture-reader\capture_reader.cpp
//
// Run:
//     capture_reader.exe <capture.n8rocap.jsonl> [--spec docs\capture-format-v1.md]
//
// Exit 0 if the file conforms, 1 if it does not, 2 on a usage or IO error. Every failure is
// named with the line number and the spec section it violates.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------------------
// A minimal, ORDER-PRESERVING JSON parser.
//
// Order preservation is not a nicety here. Spec section 8.2 makes the key order of a sample
// record's `fields` object normative - it must equal the schema's declared field order,
// restricted to the fields present - and a std::map or unordered_map parse throws away the
// one thing this reader most needs to check. So an object is a vector of pairs.
// ---------------------------------------------------------------------------------------

struct Value;
using Object = std::vector<std::pair<std::string, Value>>;
using Array = std::vector<Value>;

struct Value {
    enum class Kind { Null, Bool, Number, String, Object, Array };

    Kind kind = Kind::Null;
    bool boolean = false;
    std::string text;                   // string contents, or the raw number token
    std::shared_ptr<Object> object;
    std::shared_ptr<Array> array;

    [[nodiscard]] bool isObject() const { return kind == Kind::Object; }
    [[nodiscard]] bool isArray() const { return kind == Kind::Array; }
    [[nodiscard]] bool isString() const { return kind == Kind::String; }
    [[nodiscard]] bool isNumber() const { return kind == Kind::Number; }
    [[nodiscard]] bool isBool() const { return kind == Kind::Bool; }

    // Lookup by key, preserving the fact that the object is ordered.
    [[nodiscard]] const Value* member(const std::string& key) const {
        if (!isObject()) {
            return nullptr;
        }
        for (const auto& entry : *object) {
            if (entry.first == key) {
                return &entry.second;
            }
        }
        return nullptr;
    }

    [[nodiscard]] std::vector<std::string> keys() const {
        std::vector<std::string> out;
        if (isObject()) {
            for (const auto& entry : *object) {
                out.push_back(entry.first);
            }
        }
        return out;
    }
};

class Parser {
public:
    explicit Parser(const std::string& text) : text_(text) {}

    [[nodiscard]] bool parse(Value& out, std::string& error) {
        skipSpace();
        if (!parseValue(out, error)) {
            return false;
        }
        skipSpace();
        if (position_ != text_.size()) {
            error = "trailing content after the JSON value at offset " +
                    std::to_string(position_);
            return false;
        }
        return true;
    }

private:
    void skipSpace() {
        while (position_ < text_.size() &&
               (text_[position_] == ' ' || text_[position_] == '\t' || text_[position_] == '\r' ||
                text_[position_] == '\n')) {
            ++position_;
        }
    }

    [[nodiscard]] bool literal(const char* word) {
        const std::size_t length = std::char_traits<char>::length(word);
        if (text_.compare(position_, length, word) != 0) {
            return false;
        }
        position_ += length;
        return true;
    }

    [[nodiscard]] bool parseString(std::string& out, std::string& error) {
        if (position_ >= text_.size() || text_[position_] != '"') {
            error = "expected a string at offset " + std::to_string(position_);
            return false;
        }
        ++position_;
        out.clear();
        while (position_ < text_.size()) {
            const char c = text_[position_++];
            if (c == '"') {
                return true;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (position_ >= text_.size()) {
                break;
            }
            const char escape = text_[position_++];
            switch (escape) {
                case '"':  out.push_back('"');  break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/');  break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'u': {
                    if (position_ + 4 > text_.size()) {
                        error = "truncated \\u escape at offset " + std::to_string(position_);
                        return false;
                    }
                    const std::string hex = text_.substr(position_, 4);
                    position_ += 4;
                    const unsigned code =
                        static_cast<unsigned>(std::strtoul(hex.c_str(), nullptr, 16));
                    // Spec section 8.3: the producer emits \u only for control bytes below
                    // 0x20, and every other byte - including UTF-8 continuation bytes - is
                    // written through raw. So a one-byte decode is complete for this format.
                    if (code < 0x80) {
                        out.push_back(static_cast<char>(code));
                    } else {
                        out += "\\u" + hex;
                    }
                    break;
                }
                default:
                    error = std::string("unknown escape \\") + escape + " at offset " +
                            std::to_string(position_);
                    return false;
            }
        }
        error = "unterminated string";
        return false;
    }

    [[nodiscard]] bool parseValue(Value& out, std::string& error) {
        skipSpace();
        if (position_ >= text_.size()) {
            error = "unexpected end of input";
            return false;
        }
        const char c = text_[position_];

        if (c == '{') {
            ++position_;
            out.kind = Value::Kind::Object;
            out.object = std::make_shared<Object>();
            skipSpace();
            if (position_ < text_.size() && text_[position_] == '}') {
                ++position_;
                return true;
            }
            for (;;) {
                skipSpace();
                std::string key;
                if (!parseString(key, error)) {
                    return false;
                }
                skipSpace();
                if (position_ >= text_.size() || text_[position_] != ':') {
                    error = "expected ':' after object key at offset " +
                            std::to_string(position_);
                    return false;
                }
                ++position_;
                Value member;
                if (!parseValue(member, error)) {
                    return false;
                }
                out.object->emplace_back(key, std::move(member));
                skipSpace();
                if (position_ < text_.size() && text_[position_] == ',') {
                    ++position_;
                    continue;
                }
                if (position_ < text_.size() && text_[position_] == '}') {
                    ++position_;
                    return true;
                }
                error = "expected ',' or '}' in object at offset " + std::to_string(position_);
                return false;
            }
        }

        if (c == '[') {
            ++position_;
            out.kind = Value::Kind::Array;
            out.array = std::make_shared<Array>();
            skipSpace();
            if (position_ < text_.size() && text_[position_] == ']') {
                ++position_;
                return true;
            }
            for (;;) {
                Value element;
                if (!parseValue(element, error)) {
                    return false;
                }
                out.array->push_back(std::move(element));
                skipSpace();
                if (position_ < text_.size() && text_[position_] == ',') {
                    ++position_;
                    continue;
                }
                if (position_ < text_.size() && text_[position_] == ']') {
                    ++position_;
                    return true;
                }
                error = "expected ',' or ']' in array at offset " + std::to_string(position_);
                return false;
            }
        }

        if (c == '"') {
            out.kind = Value::Kind::String;
            return parseString(out.text, error);
        }

        if (literal("true")) {
            out.kind = Value::Kind::Bool;
            out.boolean = true;
            return true;
        }
        if (literal("false")) {
            out.kind = Value::Kind::Bool;
            out.boolean = false;
            return true;
        }
        if (literal("null")) {
            out.kind = Value::Kind::Null;
            return true;
        }

        const std::size_t start = position_;
        if (position_ < text_.size() && (text_[position_] == '-' || text_[position_] == '+')) {
            ++position_;
        }
        while (position_ < text_.size() &&
               (std::isdigit(static_cast<unsigned char>(text_[position_])) ||
                text_[position_] == '.' || text_[position_] == 'e' || text_[position_] == 'E' ||
                text_[position_] == '+' || text_[position_] == '-')) {
            ++position_;
        }
        if (position_ == start) {
            error = "unrecognised token at offset " + std::to_string(start);
            return false;
        }
        out.kind = Value::Kind::Number;
        out.text = text_.substr(start, position_ - start);
        return true;
    }

    const std::string& text_;
    std::size_t position_ = 0;
};

// ---------------------------------------------------------------------------------------
// The conformance checks.
// ---------------------------------------------------------------------------------------

// Spec section 13: the version string appears in exactly two places - the header record and
// the title of the specification - and they must be checked against each other.
constexpr const char* kSupportedVersion = "n8ro-capture/1";

struct FieldDecl {
    std::string name;
    std::string type;    // "int" | "double" | "string" | "bool"  (spec section 6.5)
    long long size = 1;
};

struct SchemaDecl {
    std::string messageName;
    std::string topic;
    std::vector<FieldDecl> fields;
};

class Report {
public:
    void fail(std::size_t line, const std::string& section, const std::string& message) {
        ++failures_;
        std::cout << "  FAIL  line " << line << "  [" << section << "]  " << message << "\n";
        if (failures_ >= kMaxReported) {
            if (!truncated_) {
                truncated_ = true;
                std::cout << "  ... further failures suppressed\n";
            }
        }
    }

    void note(const std::string& message) { std::cout << "  note  " << message << "\n"; }

    [[nodiscard]] bool shouldReport() const { return failures_ < kMaxReported; }
    [[nodiscard]] std::size_t failures() const { return failures_; }

private:
    static constexpr std::size_t kMaxReported = 40;
    std::size_t failures_ = 0;
    bool truncated_ = false;
};

[[nodiscard]] bool readIntegerMember(const Value& record, const std::string& key,
                                     long long& out) {
    const Value* value = record.member(key);
    if (value == nullptr || !value->isNumber()) {
        return false;
    }
    out = std::strtoll(value->text.c_str(), nullptr, 10);
    return true;
}

// Spec section 8.3: a field the schema declares as `double` is a JSON number that may be
// written without a fractional part, and must be parsed as a double whatever it looks like.
// It may also be one of exactly three quoted tokens for a non-finite value.
[[nodiscard]] bool readDoubleValue(const Value& value, double& out, bool& nonFinite) {
    nonFinite = false;
    if (value.isNumber()) {
        out = std::strtod(value.text.c_str(), nullptr);
        return true;
    }
    if (value.isString()) {
        if (value.text == "nan") {
            out = std::nan("");
            nonFinite = true;
            return true;
        }
        if (value.text == "inf" || value.text == "-inf") {
            out = value.text == "inf" ? HUGE_VAL : -HUGE_VAL;
            nonFinite = true;
            return true;
        }
    }
    return false;
}

// Spec section 1 and section 14: no wall-clock value appears anywhere in a capture. This
// looks for the shapes a leaked one would take.
//
// Number-valued identity fields (schema_hash, message_id) are deliberately excluded: they
// are platform identifiers that can land in the same numeric range as a Unix epoch, and
// flagging them would be a false positive rather than a finding.
[[nodiscard]] bool looksLikeWallClockString(const std::string& text) {
    // ISO-8601-ish: four digits, '-', two digits, '-', two digits.
    for (std::size_t i = 0; i + 10 <= text.size(); ++i) {
        const auto digit = [&](std::size_t k) {
            return std::isdigit(static_cast<unsigned char>(text[i + k])) != 0;
        };
        if (digit(0) && digit(1) && digit(2) && digit(3) && text[i + 4] == '-' && digit(5) &&
            digit(6) && text[i + 7] == '-' && digit(8) && digit(9)) {
            return true;
        }
    }
    // hh:mm:ss
    for (std::size_t i = 0; i + 8 <= text.size(); ++i) {
        const auto digit = [&](std::size_t k) {
            return std::isdigit(static_cast<unsigned char>(text[i + k])) != 0;
        };
        if (digit(0) && digit(1) && text[i + 2] == ':' && digit(3) && digit(4) &&
            text[i + 5] == ':' && digit(6) && digit(7)) {
            return true;
        }
    }
    return false;
}

void scanForWallClock(const Value& value, const std::string& path, std::size_t line,
                      Report& report, std::size_t& hits) {
    switch (value.kind) {
        case Value::Kind::String:
            if (looksLikeWallClockString(value.text)) {
                ++hits;
                if (report.shouldReport()) {
                    report.fail(line, "spec 1, 14",
                                "value at " + path + " looks like a wall-clock timestamp: \"" +
                                    value.text + "\"");
                }
            }
            return;
        case Value::Kind::Object:
            for (const auto& entry : *value.object) {
                // Identity numbers only; their string members are still scanned.
                if (entry.first == "schema_hash" || entry.first == "message_id") {
                    continue;
                }
                scanForWallClock(entry.second, path + "." + entry.first, line, report, hits);
            }
            return;
        case Value::Kind::Array: {
            std::size_t index = 0;
            for (const Value& element : *value.array) {
                scanForWallClock(element, path + "[" + std::to_string(index++) + "]", line,
                                 report, hits);
            }
            return;
        }
        default:
            return;
    }
}

struct Totals {
    std::size_t header = 0;
    std::size_t segmentOpen = 0;
    std::size_t segmentClose = 0;
    std::size_t entityAdd = 0;
    std::size_t entityRemove = 0;
    std::size_t sample = 0;
    std::size_t verdict = 0;
    std::size_t trailer = 0;
    std::size_t unknown = 0;
};

// Reads the version string out of the specification's own title line, so that BTB-CAP-5's
// "the version appears in exactly two places and they are checked against each other" is a
// test rather than a promise.
[[nodiscard]] bool versionFromSpec(const std::string& path, std::string& out,
                                   std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "could not open the specification at " + path;
        return false;
    }
    std::string line;
    while (std::getline(file, line)) {
        const std::size_t open = line.find("`n8ro-capture/");
        if (line.rfind("# ", 0) == 0 && open != std::string::npos) {
            const std::size_t close = line.find('`', open + 1);
            if (close == std::string::npos) {
                error = "the specification title has an unterminated version string";
                return false;
            }
            out = line.substr(open + 1, close - open - 1);
            return true;
        }
    }
    error = "no version string found in the specification title";
    return false;
}

int validate(const std::string& capturePath, const std::string& specPath) {
    std::ifstream file(capturePath, std::ios::binary);
    if (!file) {
        std::cerr << "error: could not open capture " << capturePath << "\n";
        return 2;
    }

    std::cout << "capture-reader - conformance check against docs/capture-format-v1.md\n";
    std::cout << "  file   " << capturePath << "\n";

    Report report;
    Totals totals;

    std::map<std::string, SchemaDecl> schemas;      // by message_name (spec 3, step 3)
    std::set<std::string> everPublished;            // "message_name/field_name"
    std::set<long long> openSegments;
    std::set<long long> closedSegments;
    long long highestSegmentOpened = -1;
    bool haveHeader = false;
    bool sawTrailer = false;
    std::size_t recordsAfterTrailer = 0;
    std::size_t wallClockHits = 0;
    std::size_t lineNumber = 0;
    std::size_t crlfLines = 0;

    // Occupancy tracking (spec 8.1), so the one invariant that does hold can be asserted:
    // no sample for an (entity, occupancy) pair after that pair's entity_remove.
    std::set<std::pair<std::string, long long>> closedOccupancies;

    // Sample simulation time, min to max, per segment.
    //
    // Deliberately not first-record-to-last-record. Every record carries `sim_time_s`, and a
    // complete run's first and last records are segment boundaries - the last of which follows
    // a teardown reload whose clock has been reset to 0 (spec 5.1). A first-to-last pair
    // therefore reads "0 -> 0" for a file holding a whole run, which tells a reader the
    // opposite of the truth. Per segment, over `sample` records only, says what the file
    // actually holds and shows the clock reset as its own segment.
    struct SampleSpan {
        std::uint64_t samples = 0;
        double minSimTime = 0.0;
        double maxSimTime = 0.0;
    };
    std::map<long long, SampleSpan> sampleSpans;
    bool haveSimTime = false;
    std::set<std::string> entities;
    std::set<std::pair<std::string, long long>> occupancies;

    std::string line;
    while (std::getline(file, line)) {
        ++lineNumber;

        // Spec section 2: LF, never CRLF. std::getline leaves a stray CR if the file was
        // written in text mode, which is exactly the determinism defect worth catching.
        if (!line.empty() && line.back() == '\r') {
            ++crlfLines;
            line.pop_back();
        }
        if (line.empty()) {
            report.fail(lineNumber, "spec 2", "blank line; a capture has none");
            continue;
        }
        Value record;
        std::string parseError;
        Parser parser(line);
        if (!parser.parse(record, parseError)) {
            report.fail(lineNumber, "spec 2", "not valid JSON: " + parseError);
            continue;
        }
        if (!record.isObject()) {
            report.fail(lineNumber, "spec 2", "line is valid JSON but not an object");
            continue;
        }

        const std::vector<std::string> keys = record.keys();

        // ---- the header -------------------------------------------------------------
        if (lineNumber == 1) {
            // Spec section 3, step 2: format_version is the first key, checkable before
            // anything else is parsed.
            if (keys.empty() || keys.front() != "format_version") {
                report.fail(1, "spec 3, 6",
                            "the first key of the first record is not `format_version` (found `" +
                                (keys.empty() ? std::string("<none>") : keys.front()) + "`)");
                std::cout << "\nRESULT: REJECTED - this is not a readable capture\n";
                return 1;
            }
            const Value* version = record.member("format_version");
            if (version == nullptr || !version->isString()) {
                report.fail(1, "spec 6", "`format_version` is not a string");
                return 1;
            }
            if (version->text != kSupportedVersion) {
                // Spec section 3, step 2: a named error, and stop. No partial parse.
                std::cout << "  FAIL  line 1  [spec 3, 13]  unsupported format_version \""
                          << version->text << "\"; this reader implements \""
                          << kSupportedVersion << "\" only\n";
                std::cout << "\nRESULT: REJECTED - unknown format version, file not parsed\n";
                return 1;
            }
            std::cout << "  format " << version->text << "\n";

            const Value* type = record.member("type");
            if (type == nullptr || !type->isString() || type->text != "header") {
                report.fail(1, "spec 6", "the first record does not carry `type`: \"header\"");
            }

            for (const char* required : {"producer", "platform", "attached_mid_run",
                                         "subscription", "schemas"}) {
                if (record.member(required) == nullptr) {
                    report.fail(1, "spec 6",
                                std::string("header is missing required key `") + required + "`");
                }
            }

            const Value* platform = record.member("platform");
            if (platform != nullptr && platform->isObject()) {
                for (const char* required : {"engine_config", "model_path", "schema_file",
                                             "schema_version", "runtime_version"}) {
                    if (platform->member(required) == nullptr) {
                        report.fail(1, "spec 6.2",
                                    std::string("header.platform is missing `") + required + "`");
                    }
                }
            }

            const Value* subscription = record.member("subscription");
            if (subscription != nullptr && subscription->isObject()) {
                const Value* policy = subscription->member("backpressure_policy");
                if (policy == nullptr || !policy->isString() ||
                    (policy->text != "KEEP_LATEST" && policy->text != "FIFO_DROP" &&
                     policy->text != "BLOCK")) {
                    report.fail(1, "spec 6.4",
                                "header.subscription.backpressure_policy is not one of "
                                "KEEP_LATEST / FIFO_DROP / BLOCK");
                }
                if (subscription->member("topic") == nullptr ||
                    subscription->member("queue_size") == nullptr) {
                    report.fail(1, "spec 6.4",
                                "header.subscription is missing `topic` or `queue_size`");
                }
            }

            // Spec 6.6: optional, added at producer 0.9.0, and absent means unknown rather
            // than unbounded. When it is present it has to be complete and its action has to
            // be in the closed set - a file that states a bound it cannot be checked against
            // is worse than one that states none.
            const Value* limits = record.member("limits");
            if (limits != nullptr) {
                if (!limits->isObject()) {
                    report.fail(1, "spec 6.6", "header.limits is not an object");
                } else {
                    long long ignored = -1;
                    for (const char* required : {"max_bytes", "max_samples"}) {
                        if (!readIntegerMember(*limits, required, ignored)) {
                            report.fail(1, "spec 6.6",
                                        std::string("header.limits is missing `") + required +
                                            "` or it is not an integer");
                        }
                    }
                    const Value* action = limits->member("on_size_limit");
                    if (action == nullptr || !action->isString() ||
                        (action->text != "stop" && action->text != "rotate")) {
                        report.fail(1, "spec 6.6",
                                    "header.limits.on_size_limit is not one of stop / rotate");
                    }
                    long long maxBytes = 0;
                    if (readIntegerMember(*limits, "max_bytes", maxBytes) && maxBytes > 0) {
                        report.note("recorded under a byte bound of " +
                                    std::to_string(maxBytes) + " bytes, on_size_limit=" +
                                    (action != nullptr && action->isString() ? action->text
                                                                             : "?") +
                                    " - this file may be part of a longer run (spec 6.6)");
                    }
                }
            }

            // Spec 6.7: the rotation linkage. `part` absent means 0; `continues_from` is
            // present exactly when `part` is not 0.
            long long part = 0;
            const bool hasPart = readIntegerMember(record, "part", part);
            if (hasPart && part < 0) {
                report.fail(1, "spec 6.7", "header.part is negative");
            }
            const Value* continuesFrom = record.member("continues_from");
            if (continuesFrom != nullptr && !continuesFrom->isString()) {
                report.fail(1, "spec 6.7", "header.continues_from is not a string");
            } else if (continuesFrom != nullptr && part == 0) {
                report.fail(1, "spec 6.7",
                            "header.continues_from is present but header.part is 0 - a first "
                            "part continues from nothing");
            } else if (continuesFrom == nullptr && part > 0) {
                report.fail(1, "spec 6.7",
                            "header.part is " + std::to_string(part) +
                                " but header.continues_from is absent - the set cannot be "
                                "walked backwards");
            } else if (continuesFrom != nullptr &&
                       continuesFrom->text.find_first_of("/\\") != std::string::npos) {
                report.fail(1, "spec 6.7",
                            "header.continues_from is a path, not a bare filename: \"" +
                                continuesFrom->text + "\"");
            }
            if (part > 0) {
                report.note("this is part " + std::to_string(part) +
                            " of a rotated set, continuing " + continuesFrom->text +
                            " - the run's earlier records are in the previous parts "
                            "(spec 6.7)");
            }

            const Value* attached = record.member("attached_mid_run");
            if (attached != nullptr && !attached->isBool()) {
                report.fail(1, "spec 6.3", "header.attached_mid_run is not a boolean");
            } else if (attached != nullptr && attached->boolean) {
                report.note("attached_mid_run is true - the roster's origin is not in this "
                            "file (spec 6.3)");
            }

            // Spec section 6.5: schemas, sorted ascending by message_name, each with fields
            // in declaration order.
            const Value* schemaArray = record.member("schemas");
            if (schemaArray == nullptr || !schemaArray->isArray()) {
                report.fail(1, "spec 6.5", "header.schemas is missing or not an array");
                std::cout << "\nRESULT: REJECTED - no schema table, nothing can be interpreted\n";
                return 1;
            }
            std::string previousName;
            for (const Value& entry : *schemaArray->array) {
                SchemaDecl decl;
                const Value* name = entry.member("message_name");
                const Value* topic = entry.member("topic");
                if (name == nullptr || !name->isString()) {
                    report.fail(1, "spec 6.5", "a schema entry has no `message_name`");
                    continue;
                }
                decl.messageName = name->text;
                decl.topic = topic != nullptr && topic->isString() ? topic->text : std::string();

                for (const char* required : {"schema_hash", "message_id", "wire_version"}) {
                    if (entry.member(required) == nullptr) {
                        report.fail(1, "spec 6.5",
                                    "schema " + decl.messageName + " is missing `" +
                                        required + "`");
                    }
                }

                if (!previousName.empty() && decl.messageName < previousName) {
                    report.fail(1, "spec 6.5",
                                "header.schemas is not sorted ascending by message_name (" +
                                    previousName + " precedes " + decl.messageName + ")");
                }
                previousName = decl.messageName;

                const Value* fields = entry.member("fields");
                if (fields == nullptr || !fields->isArray()) {
                    report.fail(1, "spec 6.5",
                                "schema " + decl.messageName + " has no `fields` array");
                    continue;
                }
                for (const Value& field : *fields->array) {
                    FieldDecl fieldDecl;
                    const Value* fieldName = field.member("name");
                    const Value* fieldType = field.member("type");
                    const Value* fieldSize = field.member("size");
                    if (fieldName == nullptr || !fieldName->isString() || fieldType == nullptr ||
                        !fieldType->isString() || fieldSize == nullptr || !fieldSize->isNumber()) {
                        report.fail(1, "spec 6.5",
                                    "a field of " + decl.messageName +
                                        " is missing name / type / size");
                        continue;
                    }
                    fieldDecl.name = fieldName->text;
                    fieldDecl.type = fieldType->text;
                    fieldDecl.size = std::strtoll(fieldSize->text.c_str(), nullptr, 10);
                    if (fieldDecl.type != "int" && fieldDecl.type != "double" &&
                        fieldDecl.type != "string" && fieldDecl.type != "bool") {
                        report.fail(1, "spec 6.5",
                                    "field " + decl.messageName + "." + fieldDecl.name +
                                        " has unknown type \"" + fieldDecl.type + "\"");
                    }
                    decl.fields.push_back(fieldDecl);
                }
                schemas[decl.messageName] = decl;
            }

            haveHeader = true;
            ++totals.header;
            scanForWallClock(record, "header", 1, report, wallClockHits);
            continue;
        }

        if (!haveHeader) {
            report.fail(lineNumber, "spec 3", "records precede the header");
            continue;
        }
        if (sawTrailer) {
            ++recordsAfterTrailer;
            continue;
        }

        const Value* typeValue = record.member("type");
        if (typeValue == nullptr || !typeValue->isString()) {
            report.fail(lineNumber, "spec 5", "record has no string `type`");
            continue;
        }
        const std::string type = typeValue->text;

        if (type == "header") {
            report.fail(lineNumber, "spec 4", "a second `header` record; there is exactly one");
            continue;
        }

        // Spec section 5: sim_time_s on every record except header; segment on every record
        // except header and trailer.
        const Value* simTimeValue = record.member("sim_time_s");
        double simTime = 0.0;
        bool simTimeNonFinite = false;
        bool simTimeOk = false;
        if (simTimeValue == nullptr || !readDoubleValue(*simTimeValue, simTime, simTimeNonFinite)) {
            report.fail(lineNumber, "spec 5", "record of type `" + type +
                                                  "` has no readable `sim_time_s`");
        } else {
            simTimeOk = true;
            haveSimTime = true;
        }

        long long segment = -1;
        const bool needsSegment = type != "trailer";
        if (needsSegment) {
            if (!readIntegerMember(record, "segment", segment)) {
                report.fail(lineNumber, "spec 5",
                            "record of type `" + type + "` has no integer `segment`");
            }
        } else if (record.member("segment") != nullptr) {
            report.fail(lineNumber, "spec 5", "a `trailer` carries no `segment`");
        }

        scanForWallClock(record, type, lineNumber, report, wallClockHits);

        if (type == "sample" && simTimeOk) {
            SampleSpan& span = sampleSpans[segment];
            if (span.samples == 0) {
                span.minSimTime = simTime;
                span.maxSimTime = simTime;
            } else {
                span.minSimTime = std::min(span.minSimTime, simTime);
                span.maxSimTime = std::max(span.maxSimTime, simTime);
            }
            ++span.samples;
        }

        if (type == "segment_open") {
            ++totals.segmentOpen;
            if (segment <= highestSegmentOpened) {
                report.fail(lineNumber, "spec 7",
                            "segment ordinal " + std::to_string(segment) +
                                " does not strictly increase (highest so far " +
                                std::to_string(highestSegmentOpened) + ")");
            }
            highestSegmentOpened = std::max(highestSegmentOpened, segment);
            if (!openSegments.insert(segment).second) {
                report.fail(lineNumber, "spec 7",
                            "segment " + std::to_string(segment) + " is already open");
            }
            if (record.member("scenario") == nullptr) {
                report.fail(lineNumber, "spec 7", "segment_open has no `scenario`");
            }
            continue;
        }

        if (type == "segment_close") {
            ++totals.segmentClose;
            if (openSegments.erase(segment) == 0) {
                report.fail(lineNumber, "spec 7",
                            "segment_close for segment " + std::to_string(segment) +
                                ", which is not open");
            }
            if (!closedSegments.insert(segment).second) {
                report.fail(lineNumber, "spec 7",
                            "segment " + std::to_string(segment) + " closed twice");
            }
            const Value* reason = record.member("reason");
            if (reason == nullptr || !reason->isString()) {
                report.fail(lineNumber, "spec 7", "segment_close has no string `reason`");
            } else if (reason->text != "scenario_unloaded" && reason->text != "host_lost" &&
                       reason->text != "shutdown" && reason->text != "size_limit") {
                report.fail(lineNumber, "spec 7",
                            "segment_close.reason \"" + reason->text +
                                "\" is outside the closed set");
            }
            if (record.member("scenario") == nullptr) {
                report.fail(lineNumber, "spec 7", "segment_close has no `scenario`");
            }
            continue;
        }

        if (type == "entity_add" || type == "entity_remove") {
            const Value* entity = record.member("entity");
            long long occupancy = 0;
            const bool haveOccupancy = readIntegerMember(record, "occupancy", occupancy);
            if (entity == nullptr || !entity->isString()) {
                report.fail(lineNumber, "spec 9", type + " has no string `entity`");
            }
            if (!haveOccupancy || occupancy < 1) {
                report.fail(lineNumber, "spec 9",
                            type + " has no `occupancy`, or one below 1");
            }
            if (openSegments.find(segment) == openSegments.end()) {
                report.fail(lineNumber, "spec 7",
                            type + " in segment " + std::to_string(segment) +
                                ", which is not open");
            }
            if (type == "entity_add") {
                ++totals.entityAdd;
                if (entity != nullptr && haveOccupancy) {
                    closedOccupancies.erase({entity->text, occupancy});
                }
            } else {
                ++totals.entityRemove;
                const Value* reason = record.member("reason");
                if (reason == nullptr || !reason->isString()) {
                    report.fail(lineNumber, "spec 9",
                                "entity_remove has no string `reason`");
                }
                // Spec section 9: the reason vocabulary is explicitly OPEN. Any string is
                // legal, so there is deliberately no membership check here.
                if (entity != nullptr && haveOccupancy) {
                    closedOccupancies.insert({entity->text, occupancy});
                }
            }
            continue;
        }

        if (type == "verdict") {
            ++totals.verdict;
            for (const char* required : {"condition_id", "met", "entities", "values"}) {
                if (record.member(required) == nullptr) {
                    report.fail(lineNumber, "spec 10",
                                std::string("verdict is missing `") + required + "`");
                }
            }
            continue;
        }

        if (type == "trailer") {
            ++totals.trailer;
            sawTrailer = true;
            for (const char* required : {"end_reason", "counts", "drops", "bus_metrics"}) {
                if (record.member(required) == nullptr) {
                    report.fail(lineNumber, "spec 11",
                                std::string("trailer is missing `") + required + "`");
                }
            }
            const Value* endReason = record.member("end_reason");
            if (endReason != nullptr && endReason->isString() &&
                endReason->text != "shutdown" && endReason->text != "host_lost" &&
                endReason->text != "size_limit" && endReason->text != "replay_end") {
                report.fail(lineNumber, "spec 11",
                            "trailer.end_reason \"" + endReason->text +
                                "\" is outside the closed set");
            }

            // Spec 6.7: `continued_in` names the next part, and only a file closed by its
            // size bound can have one. A file that says it continues for any other reason is
            // making a claim its own end_reason contradicts.
            const Value* continuedIn = record.member("continued_in");
            if (continuedIn != nullptr && !continuedIn->isString()) {
                report.fail(lineNumber, "spec 11", "trailer.continued_in is not a string");
            } else if (continuedIn != nullptr) {
                if (endReason == nullptr || !endReason->isString() ||
                    endReason->text != "size_limit") {
                    report.fail(lineNumber, "spec 6.7, 11",
                                "trailer.continued_in is present but end_reason is not "
                                "size_limit - only a file closed by its size bound continues");
                }
                if (continuedIn->text.find_first_of("/\\") != std::string::npos) {
                    report.fail(lineNumber, "spec 6.7",
                                "trailer.continued_in is a path, not a bare filename: \"" +
                                    continuedIn->text + "\"");
                }
                report.note("this file is continued in " + continuedIn->text +
                            " - it is not the end of the run (spec 6.7)");
            } else if (endReason != nullptr && endReason->isString() &&
                       endReason->text == "size_limit") {
                report.note("closed at its size limit with no continuation - the run went on "
                            "past what this file records (spec 6.6)");
            }

            // Spec section 11: counts describe this file, and a reader should count for
            // itself and compare.
            const Value* counts = record.member("counts");
            if (counts != nullptr && counts->isObject()) {
                const std::vector<std::pair<const char*, std::size_t>> expected = {
                    {"segments", totals.segmentOpen},
                    {"samples", totals.sample},
                    {"entities_added", totals.entityAdd},
                    {"entities_removed", totals.entityRemove},
                    {"verdicts", totals.verdict}};
                for (const auto& pair : expected) {
                    long long declared = -1;
                    if (!readIntegerMember(*counts, pair.first, declared)) {
                        report.fail(lineNumber, "spec 11",
                                    std::string("trailer.counts is missing `") + pair.first + "`");
                        continue;
                    }
                    if (declared != static_cast<long long>(pair.second)) {
                        report.fail(lineNumber, "spec 11",
                                    std::string("trailer.counts.") + pair.first + " says " +
                                        std::to_string(declared) + " but the file contains " +
                                        std::to_string(pair.second));
                    }
                }
            }

            const Value* busMetrics = record.member("bus_metrics");
            if (busMetrics != nullptr && busMetrics->isObject()) {
                // Decode side. Present since the first version of the format, so a reader may
                // require these (spec 11).
                for (const char* key : {"schema_hash_drops", "message_id_drops",
                                        "decode_failures", "missing_schema_passthrough",
                                        "legacy_payload_passthrough"}) {
                    long long value = 0;
                    if (!readIntegerMember(*busMetrics, key, value)) {
                        report.fail(lineNumber, "spec 11",
                                    std::string("trailer.bus_metrics is missing `") + key + "`");
                    } else if (value != 0) {
                        // Spec section 11: surface this prominently. A non-zero value means
                        // the producer and the host disagreed about a schema, and entire
                        // message types may be silently missing.
                        report.note(std::string("bus_metrics.") + key + " = " +
                                    std::to_string(value) +
                                    " - a schema disagreement; message types may be MISSING "
                                    "from this capture (spec 11)");
                    }
                }
                // Delivery side. Added at producer 0.4.2, so spec 11 requires a reader to
                // treat these as optional: absent means UNKNOWN, never zero. Reporting the
                // difference matters - "the bus lost nothing" and "nobody asked the bus"
                // are not the same statement about a capture.
                std::size_t deliveryPresent = 0;
                for (const char* key : {"messages_dropped", "dropped_by_backpressure",
                                        "dropped_by_queue_overflow", "dropped_by_rate_limiting"}) {
                    long long value = 0;
                    if (!readIntegerMember(*busMetrics, key, value)) {
                        continue;
                    }
                    ++deliveryPresent;
                    if (value != 0) {
                        report.note(std::string("bus_metrics.") + key + " = " +
                                    std::to_string(value) +
                                    " - the bus discarded messages; this capture is a sampled "
                                    "view of the stream, not a complete one (spec 11)");
                    }
                }
                if (deliveryPresent == 0) {
                    report.note("trailer.bus_metrics carries no delivery-side counters - written "
                                "by a producer before 0.4.2, so bus-side loss for this run is "
                                "UNKNOWN rather than zero (spec 11)");
                }
            }

            const Value* drops = record.member("drops");
            if (drops != nullptr && drops->isObject()) {
                long long orphaned = 0;
                if (readIntegerMember(*drops, "samples_orphaned", orphaned) && orphaned != 0) {
                    report.note("drops.samples_orphaned = " + std::to_string(orphaned) +
                                " - this capture is a partial view of the run (spec 11)");
                }
                long long notRecorded = 0;
                if (readIntegerMember(*drops, "samples_not_recorded", notRecorded) &&
                    notRecorded != 0) {
                    report.note("drops.samples_not_recorded = " + std::to_string(notRecorded) +
                                " - the producer discarded samples it received (spec 11)");
                }
            }
            continue;
        }

        if (type != "sample") {
            ++totals.unknown;
            report.fail(lineNumber, "spec 4",
                        "record type `" + type + "` is not in the closed vocabulary");
            continue;
        }

        // ---- sample -----------------------------------------------------------------
        ++totals.sample;

        // Spec section 7: no sample record appears outside an open segment.
        if (openSegments.find(segment) == openSegments.end()) {
            report.fail(lineNumber, "spec 7",
                        "sample in segment " + std::to_string(segment) + ", which is not open");
        }

        const Value* entity = record.member("entity");
        const Value* message = record.member("message");
        long long occupancy = 0;
        const bool haveOccupancy = readIntegerMember(record, "occupancy", occupancy);

        if (entity == nullptr || !entity->isString()) {
            report.fail(lineNumber, "spec 8", "sample has no string `entity`");
            continue;
        }
        if (message == nullptr || !message->isString()) {
            report.fail(lineNumber, "spec 8", "sample has no string `message`");
            continue;
        }
        if (!haveOccupancy || occupancy < 1) {
            report.fail(lineNumber, "spec 8.1", "sample has no `occupancy`, or one below 1");
            continue;
        }

        entities.insert(entity->text);
        occupancies.insert({entity->text, occupancy});

        // Spec section 8.1: within one (entity, occupancy) pair, no sample appears after
        // that pair's entity_remove. Across pairs, resumption is legal.
        if (closedOccupancies.count({entity->text, occupancy}) != 0) {
            report.fail(lineNumber, "spec 8.1",
                        "sample for " + entity->text + " occupancy " +
                            std::to_string(occupancy) + " after that occupancy was removed");
        }

        const auto schema = schemas.find(message->text);
        if (schema == schemas.end()) {
            report.fail(lineNumber, "spec 6.5, 8",
                        "sample.message \"" + message->text +
                            "\" resolves to no entry in header.schemas");
            continue;
        }

        const Value* fields = record.member("fields");
        if (fields == nullptr || !fields->isObject()) {
            report.fail(lineNumber, "spec 8.2", "sample has no `fields` object");
            continue;
        }

        // Spec section 8.2, the central check: the keys of `fields` are the schema's declared
        // field order, restricted to the fields present. Walk both in step - any key out of
        // order, or not declared at all, fails.
        const std::vector<std::string> presentKeys = fields->keys();
        std::size_t declaredIndex = 0;
        bool orderOk = true;
        for (const std::string& key : presentKeys) {
            bool found = false;
            while (declaredIndex < schema->second.fields.size()) {
                if (schema->second.fields[declaredIndex].name == key) {
                    found = true;
                    ++declaredIndex;
                    break;
                }
                ++declaredIndex;
            }
            if (!found) {
                orderOk = false;
                const bool declaredAtAll =
                    std::any_of(schema->second.fields.begin(), schema->second.fields.end(),
                                [&key](const FieldDecl& decl) { return decl.name == key; });
                report.fail(lineNumber, "spec 8.2",
                            declaredAtAll
                                ? "field `" + key + "` is out of the schema's declared order"
                                : "field `" + key + "` is not declared by schema " +
                                      message->text);
                break;
            }
        }
        if (!orderOk) {
            continue;
        }

        // Spec section 8.3: value encoding, checked against the declaration.
        for (const auto& entry : *fields->object) {
            const auto declaration =
                std::find_if(schema->second.fields.begin(), schema->second.fields.end(),
                             [&entry](const FieldDecl& decl) { return decl.name == entry.first; });
            if (declaration == schema->second.fields.end()) {
                continue;
            }
            everPublished.insert(message->text + "/" + entry.first);

            const Value* value = &entry.second;
            if (declaration->size > 1) {
                if (!value->isArray()) {
                    report.fail(lineNumber, "spec 8.3",
                                "field `" + entry.first + "` declares size " +
                                    std::to_string(declaration->size) + " but is not an array");
                    continue;
                }
                if (static_cast<long long>(value->array->size()) != declaration->size &&
                    report.shouldReport()) {
                    report.note("line " + std::to_string(lineNumber) + ": field `" + entry.first +
                                "` declares size " + std::to_string(declaration->size) +
                                " and carries " + std::to_string(value->array->size()) +
                                " elements (spec 8.3 permits this; reporting it)");
                }
                for (const Value& element : *value->array) {
                    if (declaration->type == "double") {
                        double parsed = 0.0;
                        bool nonFinite = false;
                        if (!readDoubleValue(element, parsed, nonFinite)) {
                            report.fail(lineNumber, "spec 8.3",
                                        "element of double field `" + entry.first +
                                            "` is neither a number nor a permitted non-finite "
                                            "token");
                        }
                    } else if (declaration->type == "string" && !element.isString()) {
                        report.fail(lineNumber, "spec 8.3",
                                    "element of string field `" + entry.first +
                                        "` is not a string");
                    }
                }
                continue;
            }

            if (declaration->type == "double") {
                double parsed = 0.0;
                bool nonFinite = false;
                if (!readDoubleValue(*value, parsed, nonFinite)) {
                    report.fail(lineNumber, "spec 8.3",
                                "double field `" + entry.first +
                                    "` is neither a number nor a permitted non-finite token");
                }
            } else if (declaration->type == "int" && !value->isNumber()) {
                report.fail(lineNumber, "spec 8.3",
                            "int field `" + entry.first + "` is not a number");
            } else if (declaration->type == "string" && !value->isString()) {
                report.fail(lineNumber, "spec 8.3",
                            "string field `" + entry.first + "` is not a string");
            } else if (declaration->type == "bool" && !value->isBool()) {
                report.fail(lineNumber, "spec 8.3",
                            "bool field `" + entry.first + "` is not a boolean");
            }
        }

        // Spec section 8.4: the envelope's entity and sim_time_s duplicate the message's own
        // fields and are always equal. Checking it is how a reader knows it may use either.
        const Value* nameField = nullptr;
        for (const auto& entry : *fields->object) {
            if (entry.first == "scenarioEntityName") {
                nameField = &entry.second;
            }
        }
        if (nameField != nullptr && nameField->isString() && nameField->text != entity->text) {
            report.fail(lineNumber, "spec 8.4",
                        "envelope entity \"" + entity->text +
                            "\" disagrees with fields.scenarioEntityName \"" + nameField->text +
                            "\"");
        }
    }

    if (crlfLines != 0) {
        report.fail(0, "spec 2",
                    std::to_string(crlfLines) +
                        " line(s) end in CRLF; the format is LF-terminated");
    }
    // Spec section 2: every record line, the last one included, is LF-terminated.
    // std::getline cannot tell a terminated final line from an unterminated one, so the last
    // byte is read directly.
    {
        std::ifstream tail(capturePath, std::ios::binary | std::ios::ate);
        const std::streampos size = tail.tellg();
        if (tail && size > 0) {
            tail.seekg(size - static_cast<std::streamoff>(1));
            char last = 0;
            tail.get(last);
            if (last != '\n') {
                report.fail(lineNumber, "spec 2",
                            "the final line is not LF-terminated; the file was truncated "
                            "mid-record");
            }
        }
    }
    if (!haveHeader) {
        std::cout << "\nRESULT: REJECTED - the file is empty or has no header\n";
        return 1;
    }
    if (!sawTrailer) {
        report.fail(lineNumber, "spec 3, 11",
                    "no `trailer` record; this capture was truncated, not closed");
    }
    if (recordsAfterTrailer != 0) {
        report.fail(lineNumber, "spec 4",
                    std::to_string(recordsAfterTrailer) +
                        " record(s) follow the trailer; it is the last line");
    }
    if (!openSegments.empty()) {
        report.fail(lineNumber, "spec 7",
                    std::to_string(openSegments.size()) +
                        " segment(s) opened and never closed");
    }

    // ---- summary --------------------------------------------------------------------
    std::cout << "\n  records  header=" << totals.header << " segment_open=" << totals.segmentOpen
              << " segment_close=" << totals.segmentClose << " entity_add=" << totals.entityAdd
              << " entity_remove=" << totals.entityRemove << " sample=" << totals.sample
              << " verdict=" << totals.verdict << " trailer=" << totals.trailer << "\n";
    std::cout << "  lines    " << lineNumber << "\n";
    if (haveSimTime && !sampleSpans.empty()) {
        bool first = true;
        bool anyZeroSpanSegment = false;
        double runMin = 0.0;
        double runMax = 0.0;
        for (const auto& entry : sampleSpans) {
            if (entry.second.samples == 0) {
                continue;
            }
            if (entry.second.minSimTime == 0.0 && entry.second.maxSimTime == 0.0) {
                anyZeroSpanSegment = true;
            }
            if (first) {
                runMin = entry.second.minSimTime;
                runMax = entry.second.maxSimTime;
                first = false;
            } else {
                runMin = std::min(runMin, entry.second.minSimTime);
                runMax = std::max(runMax, entry.second.maxSimTime);
            }
        }
        std::printf("  sim_time %.10g -> %.10g s over %zu segment(s), samples only\n", runMin,
                    runMax, sampleSpans.size());
        for (const auto& entry : sampleSpans) {
            std::printf("           segment %lld: %llu samples, %.10g -> %.10g s\n", entry.first,
                        static_cast<unsigned long long>(entry.second.samples),
                        entry.second.minSimTime, entry.second.maxSimTime);
        }
        // Only when such a segment is actually present. A capture ended by Ctrl-C mid-run has
        // no teardown reload, and explaining one there points at a record that is not here.
        if (anyZeroSpanSegment) {
            std::printf("           (a segment spanning 0 -> 0 is the teardown reload; the engine "
                        "resets\n            the clock before publishing those - spec 5.1)\n");
        }
    }
    std::cout << "  entities " << entities.size() << " distinct names, " << occupancies.size()
              << " distinct (name, occupancy) pairs\n";

    // Spec section 8.2: a declared field that no sample ever carried. Reported because it is
    // exactly the case the absent-not-defaulted rule exists for, and a reader that could not
    // tell it from a dropped field would be missing the point of the header.
    for (const auto& entry : schemas) {
        std::vector<std::string> never;
        for (const FieldDecl& field : entry.second.fields) {
            if (everPublished.count(entry.first + "/" + field.name) == 0) {
                never.push_back(field.name);
            }
        }
        std::cout << "  schema   " << entry.first << " (" << entry.second.topic << "): "
                  << entry.second.fields.size() << " declared, "
                  << entry.second.fields.size() - never.size() << " ever published\n";
        for (const std::string& field : never) {
            std::cout << "           declared and NEVER published: " << field
                      << "  (absent from every sample, present in header.schemas - spec 8.2)\n";
        }
    }

    std::cout << "  clock    " << (wallClockHits == 0 ? "no wall-clock-shaped value found"
                                                      : std::to_string(wallClockHits) +
                                                            " suspected wall-clock value(s)")
              << " (spec 1, 14)\n";

    // BTB-CAP-5: the version string lives in exactly two places and they are checked against
    // each other.
    if (!specPath.empty()) {
        std::string specVersion;
        std::string error;
        if (!versionFromSpec(specPath, specVersion, error)) {
            report.fail(0, "spec 13", "version cross-check: " + error);
        } else if (specVersion != kSupportedVersion) {
            report.fail(0, "spec 13",
                        "the specification title says \"" + specVersion +
                            "\" but this reader implements \"" + kSupportedVersion + "\"");
        } else {
            std::cout << "  version  specification title and header agree: " << specVersion
                      << "\n";
        }
    }

    if (report.failures() == 0) {
        std::cout << "\nRESULT: CONFORMS to " << kSupportedVersion << "\n";
        return 0;
    }
    std::cout << "\nRESULT: " << report.failures() << " conformance failure(s)\n";
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::string capturePath;
    std::string specPath;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--spec") {
            if (i + 1 >= argc) {
                std::cerr << "error: --spec requires a path\n";
                return 2;
            }
            specPath = argv[++i];
        } else if (capturePath.empty()) {
            capturePath = arg;
        } else {
            std::cerr << "error: unexpected argument " << arg << "\n";
            return 2;
        }
    }
    if (capturePath.empty()) {
        std::cerr << "usage: capture_reader <capture.n8rocap.jsonl> "
                     "[--spec docs/capture-format-v1.md]\n";
        return 2;
    }
    return validate(capturePath, specPath);
}
