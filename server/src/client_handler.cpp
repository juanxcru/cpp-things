#include "client_handler.h"

#include <sys/socket.h>
#include <unistd.h>
#include <sstream>
#include <cerrno>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
// Constructor — registers the session and logs the new connection.
// Note: no socket work happens here. The constructor is just setup.
// All I/O happens inside run().
// ─────────────────────────────────────────────────────────────────────────────
ClientHandler::ClientHandler(
        int fd,
        const std::string& remoteAddr,
        SessionRegistry&   registry,
        Logger&            logger,
        const std::unordered_map<std::string, CommandHandler>& handlers,
        std::atomic<bool>& serverRunning)
    : fd_(fd)
    , remoteAddr_(remoteAddr)
    , sessionId_(registry.add(fd, remoteAddr))
    , registry_(registry)
    , logger_(logger)
    , handlers_(handlers)
    , serverRunning_(serverRunning)
{
    logger_.info("CLIENT CONNECT sid=" + std::to_string(sessionId_)
                 + " from=" + remoteAddr_);
}

// ─────────────────────────────────────────────────────────────────────────────
// run() — the main loop for this client connection.
//
// Pattern: read a line → parse JSON → dispatch → send response → repeat.
//
// Exits when:
//   1. readLine() returns "" (client disconnected or recv error)
//   2. serverRunning_ becomes false (server-initiated shutdown)
//
// RAII: ::close(fd_) in the destructor-equivalent cleanup at the end.
// We don't use a destructor here because run() IS the thread entry point —
// by the time run() returns the ClientHandler object may still exist briefly
// while the thread is being joined.
// ─────────────────────────────────────────────────────────────────────────────
void ClientHandler::run() {
    while (serverRunning_.load(std::memory_order_acquire)) {
        std::string line = readLine();

        if (line.empty()) {
            // Client disconnected or recv() returned an error.
            break;
        }

        // ── Parse ─────────────────────────────────────────────────────────────
        JsonMessage request;
        try {
            request = fromJson(line);
        } catch (const std::exception& e) {
            // Malformed JSON — send error, keep connection alive.
            // Don't kill the client for one bad message (robust server behavior).
            logger_.warn("sid=" + std::to_string(sessionId_)
                         + " malformed JSON: " + e.what()
                         + " raw=" + line);
            sendResponse(makeError("malformed JSON: " + std::string(e.what())));
            continue;
        }

        // ── Dispatch ──────────────────────────────────────────────────────────
        JsonMessage response = dispatch(request);
        registry_.recordCommand(sessionId_);

        // ── Respond ───────────────────────────────────────────────────────────
        if (!sendResponse(response)) break;
    }

    // ── Cleanup ───────────────────────────────────────────────────────────────
    ::close(fd_);
    registry_.remove(sessionId_);
    logger_.info("CLIENT DISCONNECT sid=" + std::to_string(sessionId_)
                 + " from=" + remoteAddr_);
}

// ─────────────────────────────────────────────────────────────────────────────
// readLine() — read bytes until '\n' or connection closes.
//
// TCP delivers data as a byte stream with no message boundaries.
// We read one byte at a time until we see '\n'.
//
// Why one byte at a time? It's simpler and correct. In a high-throughput
// system you'd maintain a per-connection read buffer and search for '\n'
// in larger chunks — but that adds 40 lines of buffer management code
// without changing the concept being taught here.
//
// Returns "" on disconnect or unrecoverable error.
// ─────────────────────────────────────────────────────────────────────────────
std::string ClientHandler::readLine() {
    std::string line;
    char ch;

    while (true) {
        // recv() blocks until data arrives or the socket is closed.
        // MSG_WAITALL would block until all bytes arrive — we don't use it
        // because we don't know how many bytes to expect.
        ssize_t n = ::recv(fd_, &ch, 1, 0);

        if (n == 0) {
            // Orderly shutdown: the client called close() or shutdown().
            return "";
        }
        if (n < 0) {
            // Error. EINTR means interrupted by signal — retry.
            // Any other error is unrecoverable.
            if (errno == EINTR) continue;
            logger_.error("recv() error sid=" + std::to_string(sessionId_)
                          + " err=" + std::string(strerror(errno)));
            return "";
        }

        if (ch == '\n') return line;  // complete message received
        line += ch;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// sendResponse() — serialize and send a JSON response with '\n' framing.
//
// send() may send fewer bytes than requested (partial write) — we loop
// until all bytes are sent or an error occurs.
// This is a critical correctness detail: never assume send() sent everything.
// ─────────────────────────────────────────────────────────────────────────────
bool ClientHandler::sendResponse(const JsonMessage& response) {
    std::string data = frame(response);   // toJson + '\n'

    std::size_t totalSent = 0;
    while (totalSent < data.size()) {
        ssize_t sent = ::send(fd_,
                              data.c_str() + totalSent,
                              data.size()  - totalSent,
                              MSG_NOSIGNAL);  // Don't raise SIGPIPE on broken pipe
        if (sent < 0) {
            if (errno == EINTR) continue;   // signal interrupted — retry
            logger_.error("send() error sid=" + std::to_string(sessionId_)
                          + " err=" + std::string(strerror(errno)));
            return false;
        }
        totalSent += static_cast<std::size_t>(sent);
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// dispatch() — look up the command in the handler table and call it.
//
// This is the Strategy pattern: handlers_ is a map of
// "CMD_STRING" → std::function<JsonMessage(...)>.
// Dispatch is O(1) lookup — no switch statement needed.
// ─────────────────────────────────────────────────────────────────────────────
JsonMessage ClientHandler::dispatch(const JsonMessage& request) {
    auto cmdIt = request.find("cmd");
    if (cmdIt == request.end())
        return makeError("missing 'cmd' field");

    const std::string& cmd = cmdIt->second;

    auto handlerIt = handlers_.find(cmd);
    if (handlerIt == handlers_.end()) {
        logger_.warn("sid=" + std::to_string(sessionId_)
                     + " unknown command: " + cmd);
        return makeError("unknown command: " + cmd);
    }

    try {
        return handlerIt->second(request, sessionId_);
    } catch (const std::exception& e) {
        logger_.error("Handler exception cmd=" + cmd
                      + " sid=" + std::to_string(sessionId_)
                      + " err=" + e.what());
        return makeError("internal server error");
    }
}
