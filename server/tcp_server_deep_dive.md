# TCP Transaction Server — Code-Level Deep Dive

This document explains how the server works from the inside out: the general
concept first, then exactly where in the source each mechanism lives, and why
specific syscalls, C++ types, and patterns were chosen over alternatives.

---

## 1. The core problem — why TCP needs framing

TCP is a **stream protocol**. When two processes exchange data, the OS does not
preserve message boundaries. If the client calls `send()` twice:

```cpp
send(fd, "hello", 5, 0);   // first call
send(fd, "world", 5, 0);   // second call
```

The server's `recv()` might return `"helloworld"` in one call, or `"hel"` and
`"loworld"` in two calls, or any other split. There are no "packets" at the
application layer.

**Solution: a delimiter.** We append `'\n'` to every message. The receiver
reads one byte at a time until it sees `'\n'`, then it knows one complete
message has arrived. This is called **newline-delimited JSON (NDJSON)** and is
the same approach used by Redis, Docker logs, and many streaming APIs.

```
Wire: {"cmd":"PING"}\n{"cmd":"STATUS"}\n
      └── message 1 ──┘└── message 2 ──┘
```

---

## 2. Architecture: five layers

```
server_main.cpp           — entry point, signal handling
    └── Server            — bind/listen/accept, command table, thread management
        └── ClientHandler — per-connection read/parse/dispatch/respond loop
            ├── protocol  — JSON serializer + parser
            └── SessionRegistry — thread-safe metadata for all active clients
```

Each layer has one responsibility and depends only on layers below it.

---

## 3. The socket setup sequence — `server.cpp:78`

Every TCP server follows the same four-call sequence. Understanding each call
is mandatory for any *nix systems exam.

```cpp
// 1. Create a socket file descriptor
listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
```

`AF_INET` = IPv4 address family. `SOCK_STREAM` = TCP (reliable, ordered,
connection-based). The alternative `SOCK_DGRAM` would give you UDP.
The `::` prefix bypasses any C++ namespace and calls the POSIX global directly.

```cpp
// 2. Set options BEFORE bind
int opt = 1;
::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
```

**`SO_REUSEADDR`** — without this, restarting the server within ~2 minutes of
the previous run gives `"Address already in use"`. When a TCP connection
closes, the OS keeps the port in `TIME_WAIT` state for 2×MSL (Maximum Segment
Lifetime) to handle delayed duplicate packets. `SO_REUSEADDR` allows `bind()`
to succeed even while the port is in `TIME_WAIT`.

**`TCP_NODELAY`** — disables Nagle's algorithm. Nagle buffers small writes into
larger packets to reduce overhead. For a command/response protocol where
latency matters more than throughput, disabling it sends each response
immediately without waiting to see if more data is coming.

```cpp
// 3. Bind to address and port
struct sockaddr_in addr{};
addr.sin_family = AF_INET;
addr.sin_port   = htons(config_.port);           // (A)
::inet_pton(AF_INET, config_.host.c_str(), &addr.sin_addr); // (B)
::bind(listenFd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
```

**(A) `htons()`** = host-to-network short. x86 CPUs are little-endian (least
significant byte first). TCP/IP requires big-endian (most significant byte
first). `htons()` does the byte-swap. Without it, port 9000 (0x2328) would be
sent as 0x2823 = 10275 — the server would bind to the wrong port.

**(B) `inet_pton()`** = "presentation to network". Converts the human-readable
string `"127.0.0.1"` to a 4-byte binary representation. The inverse is
`inet_ntop()` (used after `accept()` to log the client's address).

```cpp
// 4. Mark as passive (server) socket
::listen(listenFd_, config_.backlog);
```

`listen()` tells the kernel "this socket will accept incoming connections, not
initiate them." The `backlog` is how many connections can queue up waiting for
`accept()` before the kernel starts dropping incoming SYNs.

---

## 4. The accept loop — `server.cpp:130`

```cpp
void Server::acceptorLoop() {
    while (running_.load(std::memory_order_acquire)) {

        int clientFd = ::accept(listenFd_,
                                reinterpret_cast<struct sockaddr*>(&clientAddr),
                                &clientAddrLen);       // (1)

        if (clientFd < 0) {
            if (!running_.load()) break;               // (2)
            if (errno == EINTR)   continue;            // (3)
            ...
        }

        spawnClientThread(clientFd, remoteAddr);       // (4)
    }
}
```

**(1)** `accept()` **blocks** here — the acceptor thread sleeps until a client
connects. When it returns, `clientFd` is a brand-new file descriptor for
*that specific connection*. `listenFd_` stays open and will accept the next
connection on the next iteration. The two fds are completely independent.

**(2)** When `shutdown()` calls `::shutdown(listenFd_, SHUT_RDWR)`, the blocked
`accept()` wakes up and returns `-1`. We check `running_` to distinguish
"shutdown" from a real error.

**(3)** `EINTR` — a signal interrupted the system call. This is normal and
harmless; we just retry. Always handle `EINTR` on any blocking syscall.

**(4)** Each accepted connection gets its own thread. This is the
**thread-per-connection** model. It's simple, correct for up to ~hundreds of
connections, and makes the per-connection logic easy to reason about. The
alternative (epoll-based event loop) scales to millions of connections but
requires non-blocking I/O and a state machine — a significantly more complex
mental model.

---

## 5. `readLine()` — the stream-to-message boundary problem — `client_handler.cpp:98`

```cpp
std::string ClientHandler::readLine() {
    std::string line;
    char ch;

    while (true) {
        ssize_t n = ::recv(fd_, &ch, 1, 0);   // (1)

        if (n == 0)  return "";               // (2)
        if (n < 0) {
            if (errno == EINTR) continue;     // (3)
            return "";                        // (4)
        }

        if (ch == '\n') return line;          // (5)
        line += ch;
    }
}
```

**(1)** We read exactly **1 byte at a time**. This is simple and correct.
Production code would use a per-connection `char buf[4096]` and search for
`'\n'` within larger reads — but that adds buffer management complexity without
changing the concept.

**(2)** `recv()` returns `0` on **orderly close**: the client called `close()`
or `shutdown()`. This is not an error — it's the normal way a client
disconnects. Return `""` to signal to `run()` that the connection is done.

**(3)** `errno == EINTR` — a signal arrived during the blocking `recv()`. Safe
to retry.

**(4)** Any other negative return is a real error (connection reset, network
error). Return `""` to trigger cleanup.

**(5)** `'\n'` detected — one complete message received. Return it without the
delimiter (the caller never sees the framing character).

---

## 6. `sendResponse()` — the partial-write problem — `client_handler.cpp:133`

```cpp
bool ClientHandler::sendResponse(const JsonMessage& response) {
    std::string data = frame(response);   // toJson() + '\n'

    std::size_t totalSent = 0;
    while (totalSent < data.size()) {
        ssize_t sent = ::send(fd_,
                              data.c_str() + totalSent,   // (1)
                              data.size()  - totalSent,
                              MSG_NOSIGNAL);               // (2)
        if (sent < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        totalSent += static_cast<std::size_t>(sent);      // (3)
    }
    return true;
}
```

**(1)** `data.c_str() + totalSent` — pointer arithmetic. After a partial write,
we advance the pointer to where the next write should start. This is the only
correct way to handle partial writes.

**(2) `MSG_NOSIGNAL`** — critical on Linux. When the remote end closes the
connection, writing to the socket would normally raise `SIGPIPE`, which
**terminates the process by default**. `MSG_NOSIGNAL` suppresses the signal
and returns `-1` instead, letting us handle it gracefully.

**(3)** `static_cast<std::size_t>(sent)` — `sent` is `ssize_t` (signed), but
`totalSent` and `data.size()` are `std::size_t` (unsigned). The cast silences
a signed/unsigned comparison warning and is safe because we've already checked
`sent < 0`.

---

## 7. The command dispatch table — `server.cpp:218` and `client_handler.cpp:160`

This is the **Strategy pattern** applied to command routing.

```cpp
// server.cpp — registration
handlers_["PING"]     = [this](const JsonMessage& r, uint64_t s) {
    return handlePing(r, s);
};
handlers_["TXN_LOG"]  = [this](const JsonMessage& r, uint64_t s) {
    return handleTxnLog(r, s);
};
// ... etc

// client_handler.cpp — dispatch
auto handlerIt = handlers_.find(cmd);       // (1)
if (handlerIt == handlers_.end())           // (2)
    return makeError("unknown command");
return handlerIt->second(request, sessionId_); // (3)
```

**(1)** `std::unordered_map::find()` is O(1) average. If the map had 100
commands, dispatch cost is the same as if it had 5.

**(2)** `find()` returns `end()` for a missing key — **not** a default-constructed
value. Using `operator[]` instead would silently insert an empty
`std::function`, which would then crash when called. `find()` leaves the map
unchanged on a miss.

**(3)** `handlerIt->second(request, sessionId_)` calls the stored
`std::function`. The lambda captured `this` at registration time, giving the
handler access to server state (`txnCounter_`, `registry_`, `logger_`) without
any global variables.

**Why this beats a switch statement:** Adding a new command means one line in
`registerCommands()`. The dispatch logic in `client_handler.cpp` never changes.
This is the Open/Closed Principle — the server is open for extension (new
commands) but closed for modification (dispatch code unchanged).

---

## 8. Graceful shutdown — `server.cpp:60`

```cpp
void Server::shutdown() {
    bool expected = true;
    if (running_.compare_exchange_strong(expected, false,   // (1)
                                         std::memory_order_acq_rel)) {
        ::shutdown(listenFd_, SHUT_RDWR);                   // (2)
    }
}
```

**(1) `compare_exchange_strong`** (CAS — Compare And Swap) atomically:
- Reads `running_`
- If it equals `expected` (true), sets it to `false` and returns `true`
- If it doesn't match, writes the actual value into `expected` and returns `false`

This ensures that if `shutdown()` is called simultaneously from the SHUTDOWN
command handler and a `SIGTERM` signal handler, exactly one of them executes
the body. The other sees `running_` is already `false` and does nothing. No
mutex needed.

**(2) `::shutdown(listenFd_, SHUT_RDWR)`** — closes both directions of the
listening socket, which unblocks the `accept()` call in `acceptorLoop()`.
Without this, the acceptor thread would sleep in `accept()` forever even after
`running_` became `false`.

The client threads exit naturally: their `readLine()` returns `""` when the
server closes connections during teardown, and the `while(running_.load())`
check in `run()` is also checked on each loop iteration.

---

## 9. The JSON parser — `protocol.cpp:57`

```cpp
JsonMessage fromJson(const std::string& json) {
    std::size_t pos = 0;

    skipWhitespace(json, pos);
    if (json[pos] != '{') throw ...;
    ++pos;
    ...
    while (pos < json.size()) {
        std::string key   = parseString(json, pos);  // (1)
        skipWhitespace(json, pos);
        // expect ':'
        ++pos;
        std::string value = parseString(json, pos);  // (2)
        result[key] = value;
        // expect ',' or '}'
    }
}
```

This is a **recursive-descent parser** — the simplest correct parser structure
for grammars like JSON. The shared `pos` cursor advances through the string as
each token is consumed. There is no backtracking.

**(1)(2)** `parseString()` handles escape sequences inside a while loop that
checks `s[pos] != '"'`. It increments `pos` past each character, including
two-character escape sequences (`\"` → `"`).

**Why write our own instead of using a library?** For an exam, this
demonstrates you understand what JSON parsing *involves*: recognising tokens,
handling edge cases (empty objects, escaped characters, unterminated strings).
In production you would use `nlohmann/json` or `rapidjson`.

---

## 10. SessionRegistry — thread-safe state — `session_registry.cpp`

```cpp
uint64_t SessionRegistry::add(int fd, const std::string& remoteAddr) {
    uint64_t id = nextId_.fetch_add(1, std::memory_order_relaxed); // (1)
    ClientSession s;
    s.sessionId = id;
    ...
    std::lock_guard<std::mutex> lock(mutex_);   // (2)
    sessions_[id] = std::move(s);
    return id;
}
```

**(1)** `std::atomic<uint64_t>` with `fetch_add(1, memory_order_relaxed)` for
the ID counter. `relaxed` ordering is sufficient because we only need the
counter to be unique — we don't need it to synchronize any other memory with
the increment. Using a mutex for a counter is unnecessary overhead.

**(2)** The map `sessions_` is guarded by `mutex_`. Every method that reads or
writes the map takes the lock. Even `activeCount()` — a read-only operation —
takes it, because on x86 reads of non-atomic types are not guaranteed to be
indivisible if another thread is writing concurrently.

---

## 11. `atomic<bool> running_` and memory ordering — `server.h`, `server.cpp`

```cpp
// Writer (shutdown thread):
running_.store(false, std::memory_order_release);   // (in compare_exchange)

// Reader (client handler thread):
while (serverRunning_.load(std::memory_order_acquire)) { ... }
```

`memory_order_acquire` on the load and `memory_order_release` on the store form
an **acquire-release pair**. This guarantees:
- The store of `false` is visible to the loading thread before it proceeds past
  the load.
- All writes that happened *before* the store are visible to the thread after
  the load.

Without the correct memory ordering, the compiler or CPU could reorder
instructions and a thread might see `running_ == false` but still act on stale
data from before the shutdown.

---

## 12. Key C++ and POSIX patterns — exam summary

| Concept | Where in code | Why used |
|---|---|---|
| `socket()` / `bind()` / `listen()` / `accept()` | `server.cpp:78–110` | The mandatory POSIX sequence for any TCP server |
| `htons()` / `inet_pton()` | `server.cpp:92,95` | Byte-order conversion and string-to-binary IP address |
| `SO_REUSEADDR` | `server.cpp:118` | Survive `TIME_WAIT` on server restart |
| `TCP_NODELAY` | `server.cpp:121` | Low-latency responses — disable Nagle buffering |
| `MSG_NOSIGNAL` | `client_handler.cpp:141` | Prevent `SIGPIPE` from killing the process on broken pipe |
| `EINTR` retry loop | `client_handler.cpp:115,143` | Signal-interrupted syscalls must be retried |
| Partial-write loop | `client_handler.cpp:137` | `send()` may write fewer bytes than requested |
| `recv()` returns 0 | `client_handler.cpp:108` | Orderly client disconnect — not an error |
| Newline framing | `protocol.h:38`, `client_handler.cpp:121` | Re-establishes message boundaries over TCP stream |
| Recursive-descent parser | `protocol.cpp:57` | Correct minimal JSON parsing without a library |
| Strategy pattern | `server.cpp:218`, `client_handler.cpp:160` | O(1) command dispatch, Open/Closed Principle |
| `compare_exchange_strong` | `server.cpp:62` | Race-free single-trigger shutdown |
| `memory_order_acquire/release` | `server.cpp:45,55` | Cross-thread visibility of `running_` flag |
| `atomic<uint64_t>` + `relaxed` | `session_registry.cpp:7` | Lock-free unique ID generation |
| Thread-per-connection | `server.cpp:169` | Simple, correct model for moderate connection counts |
| `lock_guard` | `session_registry.cpp` throughout | Scoped mutex for map operations |
