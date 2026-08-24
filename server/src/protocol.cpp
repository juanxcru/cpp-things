#include "protocol.h"

#include <sstream>
#include <stdexcept>
#include <cctype>

// ─────────────────────────────────────────────────────────────────────────────
// toJson — serialize a flat map to a JSON object string.
//
// We build it manually with ostringstream. This is deliberately low-tech:
// for an exam you need to show you understand the format, not just that you
// can call nlohmann::json::dump(). The output is always valid JSON for
// string-only values.
//
// Example:  {{"status","ok"},{"txn_id","txn-001"}}
//   →  {"status":"ok","txn_id":"txn-001"}
// ─────────────────────────────────────────────────────────────────────────────
std::string toJson(const JsonMessage& msg) {
    std::ostringstream oss;
    oss << "{";
    bool first = true;
    for (const auto& kv : msg) {
        if (!first) oss << ",";
        first = false;
        // Escape double-quotes inside key or value (basic injection defense).
        auto escape = [](const std::string& s) {
            std::string out;
            out.reserve(s.size());
            for (char c : s) {
                if (c == '"')  out += "\\\"";
                else if (c == '\\') out += "\\\\";
                else           out += c;
            }
            return out;
        };
        oss << "\"" << escape(kv.first) << "\":"
            << "\"" << escape(kv.second) << "\"";
    }
    oss << "}";
    return oss.str();
}

// ─────────────────────────────────────────────────────────────────────────────
// fromJson — minimal parser for flat {"key":"value",...} objects.
//
// Algorithm: skip whitespace, expect '{', then repeatedly parse:
//   "key" : "value"  (optionally followed by ',')
// until '}' is reached.
//
// This is not a general JSON parser. It handles:
//   - String values only (no numbers, booleans, nulls, arrays, nested objects)
//   - Basic \"-escape inside strings
//   - Leading/trailing whitespace
//
// For a production system you'd use a proper library. For an exam, writing
// your own teaches you exactly what JSON parsing involves.
// ─────────────────────────────────────────────────────────────────────────────
static std::string parseString(const std::string& s, std::size_t& pos) {
    // pos points to the opening '"'
    if (pos >= s.size() || s[pos] != '"')
        throw std::runtime_error("Expected '\"' at pos " + std::to_string(pos));
    ++pos;  // skip opening quote

    std::string result;
    while (pos < s.size() && s[pos] != '"') {
        if (s[pos] == '\\' && pos + 1 < s.size()) {
            ++pos;
            switch (s[pos]) {
                case '"':  result += '"';  break;
                case '\\': result += '\\'; break;
                case 'n':  result += '\n'; break;
                case 'r':  result += '\r'; break;
                case 't':  result += '\t'; break;
                default:   result += s[pos]; break;
            }
        } else {
            result += s[pos];
        }
        ++pos;
    }
    if (pos >= s.size())
        throw std::runtime_error("Unterminated string in JSON");
    ++pos;  // skip closing quote
    return result;
}

static void skipWhitespace(const std::string& s, std::size_t& pos) {
    while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos])))
        ++pos;
}

JsonMessage fromJson(const std::string& json) {
    JsonMessage result;
    std::size_t pos = 0;

    skipWhitespace(json, pos);
    if (pos >= json.size() || json[pos] != '{')
        throw std::runtime_error("JSON must start with '{'");
    ++pos;

    skipWhitespace(json, pos);
    if (pos < json.size() && json[pos] == '}')
        return result;  // empty object

    while (pos < json.size()) {
        skipWhitespace(json, pos);

        // Parse key
        std::string key = parseString(json, pos);

        skipWhitespace(json, pos);
        if (pos >= json.size() || json[pos] != ':')
            throw std::runtime_error("Expected ':' after key '" + key + "'");
        ++pos;
        skipWhitespace(json, pos);

        // Parse value
        std::string value = parseString(json, pos);

        result[key] = value;

        skipWhitespace(json, pos);
        if (pos >= json.size()) break;

        if (json[pos] == ',')      { ++pos; continue; }
        else if (json[pos] == '}') { break; }
        else throw std::runtime_error("Expected ',' or '}', got: " +
                                      std::string(1, json[pos]));
    }
    return result;
}


JsonMessage makeOk(const std::string& extraKey, const std::string& extraVal) {
    JsonMessage r{{"status", "ok"}};
    if (!extraKey.empty()) r[extraKey] = extraVal;
    return r;
}

JsonMessage makeError(const std::string& message) {
    return {{"status", "error"}, {"message", message}};
}
