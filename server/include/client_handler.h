#pragma once

#include "protocol.h"
#include "session_registry.h"
#include "logger.h"

#include <string>
#include <atomic>
#include <functional>

// ─────────────────────────────────────────────────────────────────────────────
// CommandHandler — a callable that processes one command and returns a response.
//
// Using std::function<> here is the Strategy pattern: the server registers
// handlers at startup, and the dispatch table maps command strings to them.
// Adding a new command means adding one entry to the table — no switch
// statement changes needed. This is the Open/Closed Principle in practice.
// ─────────────────────────────────────────────────────────────────────────────
using CommandHandler = std::function<JsonMessage(const JsonMessage& request,
                                                  uint64_t sessionId)>;

// ─────────────────────────────────────────────────────────────────────────────
// ClientHandler — manages the lifecycle of a single client connection.
//
// One ClientHandler is created per accepted connection and runs on its own
// std::thread. It owns the socket file descriptor (RAII: closes on destruction)
// and runs a read-parse-dispatch-respond loop until the client disconnects or
// the server signals shutdown.
//
// Thread safety: each ClientHandler is the *only* entity that reads from or
// writes to its socket fd. No external locking needed for the socket itself.
// ─────────────────────────────────────────────────────────────────────────────
class ClientHandler {
public:
    ClientHandler(int fd,
                  const std::string&   remoteAddr,
                  SessionRegistry&     registry,
                  Logger&              logger,
                  const std::unordered_map<std::string, CommandHandler>& handlers,
                  std::atomic<bool>&   serverRunning);

    // Non-copyable — owns a file descriptor and a session ID.
    ClientHandler(const ClientHandler&)            = delete;
    ClientHandler& operator=(const ClientHandler&) = delete;

    // Entry point: called on the client thread.
    void run();

private:
    // Read from socket until '\n' delimiter.
    // Returns empty string on disconnect or error.
    std::string readLine();

    // Send a framed JSON response.
    bool sendResponse(const JsonMessage& response);

    // Dispatch a parsed request to the correct CommandHandler.
    JsonMessage dispatch(const JsonMessage& request);

    int                  fd_;
    std::string          remoteAddr_;
    uint64_t             sessionId_;
    SessionRegistry&     registry_;
    Logger&              logger_;
    const std::unordered_map<std::string, CommandHandler>& handlers_;
    std::atomic<bool>&   serverRunning_;
};
