#include "JsonParse.h"

#include <cctype>
#include <cstdlib>
#include <utility>

namespace n8ro::bridge::json {
namespace {

class Parser {
public:
    Parser(const std::string& text, std::string& error) : text_(text), error_(error) {}

    bool parseDocument(ValuePtr& out) {
        skipWhitespace();
        if (!parseValue(out, 0)) {
            return false;
        }
        skipWhitespace();
        if (pos_ != text_.size()) {
            return fail("trailing content after the end of the value");
        }
        return true;
    }

private:
    [[nodiscard]] bool fail(const std::string& why) {
        error_ = "at byte " + std::to_string(pos_) + ": " + why;
        return false;
    }

    void skipWhitespace() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    [[nodiscard]] bool literal(const char* word) {
        const std::size_t length = std::char_traits<char>::length(word);
        if (text_.compare(pos_, length, word) != 0) {
            return false;
        }
        pos_ += length;
        return true;
    }

    bool parseValue(ValuePtr& out, int depth) {
        if (depth > kMaxDepth) {
            return fail("nesting deeper than " + std::to_string(kMaxDepth) +
                        " levels; refusing to recurse further");
        }
        if (pos_ >= text_.size()) {
            return fail("unexpected end of input where a value was expected");
        }
        const char c = text_[pos_];
        switch (c) {
            case '{': return parseObject(out, depth);
            case '[': return parseArray(out, depth);
            case '"': {
                out = std::make_unique<Value>();
                out->kind = Value::Kind::String;
                return parseString(out->text);
            }
            case 't':
                if (!literal("true")) {
                    return fail("expected true");
                }
                out = std::make_unique<Value>();
                out->kind = Value::Kind::Bool;
                out->boolean = true;
                return true;
            case 'f':
                if (!literal("false")) {
                    return fail("expected false");
                }
                out = std::make_unique<Value>();
                out->kind = Value::Kind::Bool;
                out->boolean = false;
                return true;
            case 'n':
                if (!literal("null")) {
                    return fail("expected null");
                }
                out = std::make_unique<Value>();
                out->kind = Value::Kind::Null;
                return true;
            default:
                return parseNumber(out);
        }
    }

    bool parseObject(ValuePtr& out, int depth) {
        ++pos_;   // '{'
        out = std::make_unique<Value>();
        out->kind = Value::Kind::Object;
        skipWhitespace();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            return true;
        }
        for (;;) {
            skipWhitespace();
            if (pos_ >= text_.size() || text_[pos_] != '"') {
                return fail("expected a quoted key in an object");
            }
            std::string key;
            if (!parseString(key)) {
                return false;
            }
            skipWhitespace();
            if (pos_ >= text_.size() || text_[pos_] != ':') {
                return fail("expected ':' after an object key");
            }
            ++pos_;
            skipWhitespace();
            ValuePtr member;
            if (!parseValue(member, depth + 1)) {
                return false;
            }
            // A duplicate key is a defect in the writer, not something to silently resolve.
            if (out->members.find(key) != out->members.end()) {
                return fail("duplicate object key \"" + key + "\"");
            }
            out->members.emplace(std::move(key), std::move(member));

            skipWhitespace();
            if (pos_ >= text_.size()) {
                return fail("unterminated object");
            }
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == '}') {
                ++pos_;
                return true;
            }
            return fail("expected ',' or '}' in an object");
        }
    }

    bool parseArray(ValuePtr& out, int depth) {
        ++pos_;   // '['
        out = std::make_unique<Value>();
        out->kind = Value::Kind::Array;
        skipWhitespace();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            return true;
        }
        for (;;) {
            skipWhitespace();
            ValuePtr element;
            if (!parseValue(element, depth + 1)) {
                return false;
            }
            out->elements.push_back(std::move(element));
            skipWhitespace();
            if (pos_ >= text_.size()) {
                return fail("unterminated array");
            }
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == ']') {
                ++pos_;
                return true;
            }
            return fail("expected ',' or ']' in an array");
        }
    }

    // Decodes one \uXXXX escape into UTF-8, handling a surrogate pair as one code point.
    bool parseUnicodeEscape(std::string& out) {
        std::uint32_t code = 0;
        if (!readHex4(code)) {
            return false;
        }
        if (code >= 0xD800 && code <= 0xDBFF) {
            // A high surrogate must be followed by its low half; anything else would decode
            // to a lone surrogate, which is not valid UTF-8.
            if (pos_ + 1 >= text_.size() || text_[pos_] != '\\' || text_[pos_ + 1] != 'u') {
                return fail("high surrogate not followed by \\u");
            }
            pos_ += 2;
            std::uint32_t low = 0;
            if (!readHex4(low)) {
                return false;
            }
            if (low < 0xDC00 || low > 0xDFFF) {
                return fail("high surrogate followed by a non-low-surrogate");
            }
            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
        } else if (code >= 0xDC00 && code <= 0xDFFF) {
            return fail("unpaired low surrogate");
        }

        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
        return true;
    }

    bool readHex4(std::uint32_t& out) {
        if (pos_ + 4 > text_.size()) {
            return fail("truncated \\u escape");
        }
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_ + static_cast<std::size_t>(i)];
            std::uint32_t digit = 0;
            if (c >= '0' && c <= '9') {
                digit = static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                digit = static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                digit = static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                return fail("non-hex digit in a \\u escape");
            }
            out = (out << 4) | digit;
        }
        pos_ += 4;
        return true;
    }

    bool parseString(std::string& out) {
        ++pos_;   // opening quote
        out.clear();
        for (;;) {
            if (pos_ >= text_.size()) {
                return fail("unterminated string");
            }
            const char c = text_[pos_];
            if (c == '"') {
                ++pos_;
                return true;
            }
            if (c == '\\') {
                ++pos_;
                if (pos_ >= text_.size()) {
                    return fail("unterminated escape");
                }
                const char esc = text_[pos_++];
                switch (esc) {
                    case '"':  out.push_back('"');  break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/');  break;
                    case 'b':  out.push_back('\b'); break;
                    case 'f':  out.push_back('\f'); break;
                    case 'n':  out.push_back('\n'); break;
                    case 'r':  out.push_back('\r'); break;
                    case 't':  out.push_back('\t'); break;
                    case 'u':
                        if (!parseUnicodeEscape(out)) {
                            return false;
                        }
                        break;
                    default:
                        return fail("unknown escape \\" + std::string(1, esc));
                }
                continue;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                return fail("raw control byte in a string");
            }
            // Bytes at or above 0x20 pass through verbatim, so a multi-byte UTF-8 sequence
            // survives unaltered - the mirror of what the writer does.
            out.push_back(c);
            ++pos_;
        }
    }

    bool parseNumber(ValuePtr& out) {
        const std::size_t start = pos_;
        bool integral = true;
        if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) {
            ++pos_;
        }
        while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_])) != 0) {
            ++pos_;
        }
        if (pos_ < text_.size() && text_[pos_] == '.') {
            integral = false;
            ++pos_;
            while (pos_ < text_.size() &&
                   std::isdigit(static_cast<unsigned char>(text_[pos_])) != 0) {
                ++pos_;
            }
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            integral = false;
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) {
                ++pos_;
            }
            while (pos_ < text_.size() &&
                   std::isdigit(static_cast<unsigned char>(text_[pos_])) != 0) {
                ++pos_;
            }
        }
        if (pos_ == start) {
            return fail("expected a value");
        }

        const std::string token = text_.substr(start, pos_ - start);
        // strtod is locale-sensitive on its decimal separator, which is the hazard OQ-5
        // catalogued on the writing side. The capture is written by std::to_chars and always
        // uses '.', so a comma-decimal locale would misparse it. from_chars would be the
        // symmetric answer; strtod with the C locale forced is what is portable here, and
        // the check below is what makes a mis-parse loud rather than silent.
        const char* begin = token.c_str();
        char* end = nullptr;
        const double parsed = std::strtod(begin, &end);
        if (end != begin + token.size()) {
            return fail("number \"" + token +
                        "\" did not parse completely - if this machine uses a comma decimal "
                        "separator, that is the cause");
        }
        out = std::make_unique<Value>();
        out->kind = Value::Kind::Number;
        out->number = parsed;
        out->isInteger = integral;
        return true;
    }

    const std::string& text_;
    std::string& error_;
    std::size_t pos_ = 0;
};

}  // namespace

const Value* Value::find(const std::string& key) const {
    if (kind != Kind::Object) {
        return nullptr;
    }
    const auto it = members.find(key);
    return it == members.end() ? nullptr : it->second.get();
}

const Value* Value::findOfKind(const std::string& key, Kind wanted) const {
    const Value* found = find(key);
    return (found != nullptr && found->kind == wanted) ? found : nullptr;
}

bool Value::getString(const std::string& key, std::string& out) const {
    const Value* found = findOfKind(key, Kind::String);
    if (found == nullptr) {
        return false;
    }
    out = found->text;
    return true;
}

bool Value::getNumber(const std::string& key, double& out) const {
    const Value* found = findOfKind(key, Kind::Number);
    if (found == nullptr) {
        return false;
    }
    out = found->number;
    return true;
}

bool Value::getUint(const std::string& key, std::uint64_t& out) const {
    const Value* found = findOfKind(key, Kind::Number);
    if (found == nullptr || !found->isInteger || found->number < 0.0) {
        return false;
    }
    out = static_cast<std::uint64_t>(found->number);
    return true;
}

bool Value::getBool(const std::string& key, bool& out) const {
    const Value* found = findOfKind(key, Kind::Bool);
    if (found == nullptr) {
        return false;
    }
    out = found->boolean;
    return true;
}

bool parse(const std::string& text, ValuePtr& out, std::string& error) {
    error.clear();
    Parser parser(text, error);
    if (!parser.parseDocument(out)) {
        out.reset();
        return false;
    }
    return true;
}

}  // namespace n8ro::bridge::json
