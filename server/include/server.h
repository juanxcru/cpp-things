#pragma once

#include "client_handler.h"
#include "session_registry.h"
#include "logger.h"

#include <string>
#include <thread>
#include <vector>
#include <atomic>
#include <unordered_map>
#include <mutex>
#include <cstdint>

// 
// ServerConfig — all tunable parameters.
// Same design rationale as LoggerConfig: grouped struct with defaults.
// 
struct ServerConfig {
    std::string host          = "127.0.0.1";
    uint16_t    port          = 9000;
    int         backlog       = 10;       // listen() backlog — pending connections
    int         maxClients    = 64;       // refuse connections beyond this
    std::string logDir        = "./logs";
    std::string logFileBaseName = "server";
};

// 
// Server — TCP server that accepts multiple clients, each on their own thread.
//
// Lifecycle:
//   1. Server server(cfg);          — sets up sockets and command table
//   2. server.start();              — spawns acceptor thread, returns immediately
//   3. server.waitForShutdown();    — blocks until SHUTDOWN command received
//   4. ~Server()                    — joins all threads, closes all fds (RAII)
//
// The SHUTDOWN command sets running_ = false, which causes:
//   - The acceptor loop to exit (it checks running_ after each accept)
//   - Each client loop to exit (ClientHandler checks serverRunning_)
// 
class Server {
public:
    explicit Server(ServerConfig config = ServerConfig{});
    ~Server();

    Server(const Server&)            = delete;
    Server& operator=(const Server&) = delete;

    // Bind socket, start acceptor thread. Throws on bind failure.
    void start();

    // Block until running_ becomes false (triggered by SHUTDOWN command).
    void waitForShutdown();

    // Trigger graceful shutdown from outside (e.g. signal handler).
    void shutdown();

    uint16_t port() const { return config_.port; }

private:
    //  Socket setup 
    void     bindAndListen();
    void     setSocketOptions(int fd);

    //  Acceptor loop — runs on acceptor_ thread 
    void     acceptorLoop();

    //  Client thread launcher 
    void     spawnClientThread(int clientFd, const std::string& remoteAddr);

    //  Reap finished threads 
    // Called periodically in acceptorLoop to clean up completed threads.
    void     reapFinishedThreads();

    //  Command handlers (registered at construction) 
    void     registerCommands();
    JsonMessage handlePing    (const JsonMessage& req, uint64_t sid);
    JsonMessage handleEcho    (const JsonMessage& req, uint64_t sid);
    JsonMessage handleTxnLog  (const JsonMessage& req, uint64_t sid);
    JsonMessage handleStatus  (const JsonMessage& req, uint64_t sid);
    JsonMessage handleShutdown(const JsonMessage& req, uint64_t sid);

    //  State 
    ServerConfig   config_;
    int            listenFd_ = -1;

    std::atomic<bool>    running_{false};
    std::atomic<uint64_t> txnCounter_{0};

    Logger           logger_;
    SessionRegistry  registry_;

    // Command dispatch table: "CMD_NAME" → handler function
    std::unordered_map<std::string, CommandHandler> handlers_;

    // Acceptor thread
    std::thread acceptor_;

    // Client threads — guarded by clientsMutex_
    std::mutex               clientsMutex_;
    std::vector<std::thread> clientThreads_;
};
