#pragma once

#include <string>
#include <unordered_map>
#include <mutex>
#include <chrono>
#include <atomic>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// ClientSession — metadata for one connected client.
//
// This is a plain data struct (no methods). The SessionRegistry owns all
// instances and is the only place that creates or mutates them.
// ─────────────────────────────────────────────────────────────────────────────
struct ClientSession {
    int         fd;               // socket file descriptor
    std::string remoteAddr;       // "127.0.0.1:54321"
    uint64_t    sessionId;        // monotonically increasing
    std::chrono::steady_clock::time_point connectedAt;
    uint64_t    commandsHandled = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// SessionRegistry — thread-safe map of active client sessions.
//
// Design: a simple mutex-guarded unordered_map. In a high-throughput system
// you'd reach for a concurrent hashmap (tbb::concurrent_hash_map) or
// sharded locks. For our exam-level server, one mutex is correct and
// explains the concept cleanly.
//
// The registry is *not* responsible for closing sockets — that's the client
// thread's job (RAII via ClientHandler). The registry only tracks metadata.
// ─────────────────────────────────────────────────────────────────────────────
class SessionRegistry {
public:
    // Register a new connection. Returns a session ID.
    uint64_t add(int fd, const std::string& remoteAddr);

    // Remove a session when the client disconnects.
    void remove(uint64_t sessionId);

    // Increment command counter for a session.
    void recordCommand(uint64_t sessionId);

    // Snapshot of active session count (for STATUS command).
    std::size_t activeCount() const;

    // Human-readable summary of all active sessions.
    std::string summary() const;

private:
    mutable std::mutex                           mutex_;
    std::unordered_map<uint64_t, ClientSession>  sessions_;
    std::atomic<uint64_t>                        nextId_{1};
};
