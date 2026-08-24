#include "protocol.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <iostream>
#include <string>
#include <sstream>
#include <cstring>
#include <stdexcept>

// ─────────────────────────────────────────────────────────────────────────────
// TcpClient — thin RAII wrapper around a connected socket.
// ─────────────────────────────────────────────────────────────────────────────
class TcpClient {
public:
    TcpClient(const std::string& host, uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) throw std::runtime_error("socket() failed");

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(port);
        if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
            throw std::runtime_error("inet_pton() failed");

        if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0)
            throw std::runtime_error("connect() failed: " + std::string(strerror(errno)));
    }

    ~TcpClient() { if (fd_ >= 0) ::close(fd_); }

    TcpClient(const TcpClient&) = delete;
    TcpClient& operator=(const TcpClient&) = delete;

    // Send a framed JSON message.
    void send(const JsonMessage& msg) {
        std::string data = frame(msg);
        std::size_t sent = 0;
        while (sent < data.size()) {
            ssize_t n = ::send(fd_, data.c_str() + sent,
                               data.size() - sent, MSG_NOSIGNAL);
            if (n <= 0) throw std::runtime_error("send() failed");
            sent += static_cast<std::size_t>(n);
        }
    }

    // Read one newline-delimited line.
    std::string recvLine() {
        std::string line;
        char ch;
        while (true) {
            ssize_t n = ::recv(fd_, &ch, 1, 0);
            if (n <= 0) return "";
            if (ch == '\n') return line;
            line += ch;
        }
    }

private:
    int fd_ = -1;
};

// ─────────────────────────────────────────────────────────────────────────────
// parseCommand — convert a REPL line to a JsonMessage.
//
// Simple format:
//   PING
//   ECHO hello world
//   TXN_LOG 500 USD PAYMENT
//   STATUS
//   SHUTDOWN
// ─────────────────────────────────────────────────────────────────────────────
JsonMessage parseCommand(const std::string& line) {
    std::istringstream iss(line);
    std::string cmd;
    iss >> cmd;

    JsonMessage msg;
    msg["cmd"] = cmd;

    if (cmd == "ECHO") {
        std::string rest;
        std::getline(iss, rest);
        if (!rest.empty() && rest[0] == ' ') rest = rest.substr(1);
        msg["payload"] = rest;
    } else if (cmd == "TXN_LOG") {
        std::string amount, currency, type;
        iss >> amount >> currency >> type;
        msg["amount"]   = amount;
        msg["currency"] = currency;
        msg["type"]     = type;
    }
    // PING, STATUS, SHUTDOWN need no extra fields

    return msg;
}

void printHelp() {
    std::cout << "\nCommands:\n"
              << "  PING                          — check server alive\n"
              << "  ECHO <text>                   — echo text back\n"
              << "  TXN_LOG <amount> <ccy> <type> — log a transaction\n"
              << "  STATUS                        — server status\n"
              << "  SHUTDOWN                      — shutdown server\n"
              << "  help                          — this message\n"
              << "  quit                          — disconnect client\n\n";
}

int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    uint16_t    port = 9000;

    if (argc >= 3) {
        host = argv[1];
        port = static_cast<uint16_t>(std::stoi(argv[2]));
    }

    std::cout << "Connecting to " << host << ":" << port << "...\n";

    TcpClient client(host, port);
    std::cout << "Connected. Type 'help' for commands.\n";
    printHelp();

    std::string inputLine;
    while (true) {
        std::cout << "> ";
        std::cout.flush();

        if (!std::getline(std::cin, inputLine)) break;  // EOF (Ctrl+D)

        if (inputLine.empty()) continue;
        if (inputLine == "quit" || inputLine == "exit") break;
        if (inputLine == "help") { printHelp(); continue; }

        try {
            JsonMessage req = parseCommand(inputLine);
            client.send(req);

            std::string raw = client.recvLine();
            if (raw.empty()) {
                std::cout << "Server disconnected.\n";
                break;
            }

            // Pretty-print the response
            JsonMessage resp = fromJson(raw);
            std::cout << "Response:\n";
            for (const auto& kv : resp)
                std::cout << "  " << kv.first << " = " << kv.second << "\n";
            std::cout << "\n";

        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << "\n";
        }
    }

    std::cout << "Disconnected.\n";
    return 0;
}
