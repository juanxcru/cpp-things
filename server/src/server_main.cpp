#include "server.h"
#include <iostream>
#include <csignal>

// Global pointer for signal handler — a necessary concession to POSIX signal API.
// Signal handlers can only be plain functions or function pointers,
// not member functions or lambdas.
static Server* g_server = nullptr;

void onShutdownSignal(int sig) {
    if (g_server) {
        g_server->shutdown();
    }
}

int main(int argc, char* argv[]) {
    ServerConfig cfg;
    if (argc >= 2) cfg.port = static_cast<uint16_t>(std::stoi(argv[1]));

    // Register signal handlers for clean shutdown on Ctrl+C (SIGINT)
    // and container termination (SIGTERM from Docker/k8s).
    ::signal(SIGINT,  onShutdownSignal);
    ::signal(SIGTERM, onShutdownSignal);

    try {
        Server server(cfg);
        g_server = &server;

        server.start();
        std::cout << "TCP Transaction Server running on port " << cfg.port << "\n";
        std::cout << "Send SHUTDOWN command or press Ctrl+C to stop.\n\n";

        server.waitForShutdown();

        g_server = nullptr;
        std::cout << "\nServer stopped.\n";
    } catch (const std::exception& e) {
        std::cerr << "Fatal: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
