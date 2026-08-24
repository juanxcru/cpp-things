#pragma once

#include <string>
#include <unordered_map>
#include <stdexcept>

// ─────────────────────────────────────────────────────────────────────────────
// Protocol — newline-delimited JSON over TCP.
//
// Why newline-delimited? TCP is a *stream* protocol — it has no concept of
// message boundaries. When you recv() you might get half a message, or two
// messages merged together. A delimiter (here: '\n') lets the receiver know
// exactly where one message ends and the next begins. This is the same
// approach used by NDJSON, Redis inline protocol, and many other systems.
//
// Message format (request):
//   {"cmd":"PING"}\n
//   {"cmd":"ECHO","payload":"hello"}\n
//   {"cmd":"TXN_LOG","amount":"500","currency":"USD","type":"PAYMENT"}\n
//   {"cmd":"STATUS"}\n
//   {"cmd":"SHUTDOWN"}\n
//
// Message format (response):
//   {"status":"ok","pong":"true"}\n
//   {"status":"ok","echo":"hello"}\n
//   {"status":"ok","txn_id":"txn-00001"}\n
//   {"status":"error","message":"unknown command"}\n
//
// We implement a minimal JSON parser/builder rather than pulling in a
// third-party library (nlohmann/json etc.). This is intentional for the exam:
// it forces you to understand what JSON actually *is* (a flat key-value text
// format) and builds the habit of not reaching for a library before you
// understand the problem.
// ─────────────────────────────────────────────────────────────────────────────

// A JsonMessage is just a flat string→string map.
// For a transaction logger we don't need nested objects.
using JsonMessage = std::unordered_map<std::string, std::string>;

// ── Serializer ────────────────────────────────────────────────────────────────
// Converts a JsonMessage to a JSON string (without trailing newline).
// Caller appends '\n' for wire framing.
std::string toJson(const JsonMessage& msg);

// ── Parser ────────────────────────────────────────────────────────────────────
// Parses a single JSON object string into a JsonMessage.
// Throws std::runtime_error on malformed input.
// Only handles flat {"key":"value",...} — no nesting, no arrays, no numbers.
// Numbers arrive as strings and the handler converts them as needed.
JsonMessage fromJson(const std::string& json);

// ── Convenience builders ──────────────────────────────────────────────────────
JsonMessage makeOk(const std::string& extraKey = "",
                   const std::string& extraVal = "");

JsonMessage makeError(const std::string& message);

// ── Wire helpers ──────────────────────────────────────────────────────────────
// Append '\n' delimiter for sending over TCP.
inline std::string frame(const JsonMessage& msg) {
    return toJson(msg) + "\n";
}
