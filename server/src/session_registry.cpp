#include "session_registry.h"

#include <sstream>

uint64_t SessionRegistry::add(int fd, const std::string& remoteAddr) {
    uint64_t id = nextId_.fetch_add(1, std::memory_order_relaxed);
    ClientSession s;
    s.fd          = fd;
    s.remoteAddr  = remoteAddr;
    s.sessionId   = id;
    s.connectedAt = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(mutex_);
    sessions_[id] = std::move(s);
    return id;
}

void SessionRegistry::remove(uint64_t sessionId) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(sessionId);
}

void SessionRegistry::recordCommand(uint64_t sessionId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(sessionId);
    if (it != sessions_.end())
        ++it->second.commandsHandled;
}

std::size_t SessionRegistry::activeCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.size();
}

std::string SessionRegistry::summary() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream oss;
    oss << sessions_.size() << " active session(s): ";
    bool first = true;
    for (const auto& kv : sessions_) {
        if (!first) oss << ", ";
        first = false;
        oss << "[" << kv.second.remoteAddr
            << " cmds=" << kv.second.commandsHandled << "]";
    }
    return oss.str();
}
