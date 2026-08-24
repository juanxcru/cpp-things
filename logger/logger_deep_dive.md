# Transaction Logger — Code-Level Deep Dive

This document explains how the logger works **from the inside out**: the general
concept first, then exactly where in the source code each mechanism lives, and
why specific C++ syntax or standard-library types were chosen over alternatives.

---

## 1. The core problem — why a simple `fprintf` is not enough

The naive approach to logging is:

```cpp
// DON'T do this in a multi-threaded system
void log(const std::string& msg) {
    fprintf(logfile, "%s\n", msg.c_str());  // ← not thread-safe
}
```

Two threads calling this simultaneously produce **interleaved bytes** in the
file — half of thread A's message mixed with half of thread B's. This is a
**data race** on the `FILE*` object, which is undefined behavior in C++.

The solution has two parts:

1. **Mutual exclusion** — only one thread writes at a time.
2. **Asynchronous I/O** — disk writes happen on a background thread so callers
   are never blocked waiting for I/O.

Both are implemented through the **producer-consumer pattern**.

---

## 2. Architecture overview

```
Thread A ──┐
Thread B ──┤──► [std::queue<LogEntry>] ──► [Worker Thread] ──► file on disk
Thread C ──┘
           ▲                              ▲
     mutex_ guards                  only thread that
     access to queue                touches the file
```

- **Producers**: any thread that calls `log()`, `info()`, etc.
  They push a `LogEntry` onto the queue and return immediately.
- **Consumer**: one dedicated background thread (`worker_`) that pops entries
  and writes them to disk.
- **Shared resource**: `queue_`, guarded by `mutex_`.
- **Signaling**: `cv_` (condition variable) puts the worker to sleep when the
  queue is empty and wakes it when new entries arrive.

---

## 3. Data types — why each one was chosen

### `enum class LogLevel` — `logger.h:16`

```cpp
enum class LogLevel {
    DEBUG = 0,
    INFO  = 1,
    WARN  = 2,
    ERR   = 3
};
```

**Why `enum class` and not plain `enum`?**

Plain `enum` leaks its values into the enclosing scope (`DEBUG` would conflict
with system macros). `enum class` requires the fully-qualified name
(`LogLevel::DEBUG`) and **does not implicitly convert to `int`**, preventing
accidental comparisons like `if (level == 1)`. The integer values are explicit
so `level < config_.minLevel` works correctly as a numeric comparison — this is
the entire filtering mechanism.

**Why is `ERROR` named `ERR`?**

Some system headers (`<cerrno>`, Windows headers) `#define ERROR` as a macro.
Using `ERR` avoids a preprocessor collision that would cause a cryptic compile
error.

---

### `struct LoggerConfig` — `logger.h:31`

```cpp
struct LoggerConfig {
    std::string  logDir        = "./logs";
    std::size_t  maxFileSizeBytes = 10 * 1024 * 1024;
    LogLevel     minLevel      = LogLevel::DEBUG;
    std::size_t  maxQueueSize  = 8192;
};
```

**Why a separate config struct instead of constructor parameters?**

With a struct, adding a new option is a backward-compatible change — existing
code that constructs `LoggerConfig{}` still compiles with default values. If the
options were constructor parameters, adding one would break every call site.
This is the **Open/Closed Principle**: open for extension (new fields), closed
for modification (existing callers).

**Why `std::size_t` for sizes?**

`std::size_t` is the unsigned type returned by `sizeof` and container
`.size()`. Using it avoids signed/unsigned comparison warnings when writing
`queue_.size() < config_.maxQueueSize`.

---

### `struct LogEntry` — `logger.h:86` (private)

```cpp
struct LogEntry {
    LogLevel    level;
    std::string timestamp;
    std::string message;
};
```

**Why is the timestamp stored in the entry, not generated at write time?**

The worker thread might be busy when a message is enqueued. If the timestamp
were generated at write time, a message logged at 14:00:00 might appear in the
file with a timestamp of 14:00:03 — after other messages. Timestamping at
**enqueue time** (`logger.cpp:96`) preserves causal ordering even under load.

---

### `mutable std::mutex mutex_` — `logger.h:117`

```cpp
mutable std::mutex mutex_;
```

**Why `mutable`?**

`queueSize()` is a `const` method (it promises not to change observable state),
but reading `queue_.size()` safely requires locking `mutex_`. `mutable` allows
a member to be modified even in a `const` context. The lock itself doesn't
change the queue — it just ensures the read is safe — so `mutable` is
semantically correct here, not a hack.

---

### `std::condition_variable cv_` — `logger.h:118`

```cpp
std::condition_variable cv_;
```

A condition variable solves the "how does the worker know there's work?" problem
without burning a CPU core in a spin-loop. See Section 5 for how it's used.

---

### `std::thread worker_` — `logger.h:124`

```cpp
std::thread worker_;
```

**Why is `worker_` declared last?**

C++ initializes members **in declaration order**, not in the order they appear
in the member-initialization list. `worker_` starts the background thread
immediately on construction. If it were declared before `mutex_`, `cv_`, or
`shutdown_`, the thread could run before those members were initialized — a
classic race condition. Declaring it last guarantees everything is ready.

---

## 4. Constructor — RAII entry point

**File: `logger.cpp:34`**

```cpp
Logger::Logger(LoggerConfig config)
    : config_(std::move(config))        // (1)
    , currentDateTag_(currentDateTag()) // (2)
    , shutdown_(false)                  // (3)
    , worker_(&Logger::workerLoop, this) // (4) — thread starts HERE
{
    ::mkdir(config_.logDir.c_str(), 0755); // (5)
    openNewFile();                          // (6)
}
```

**(1) `std::move(config)`** — `config` is a local copy (passed by value). Moving
it into `config_` transfers ownership of the strings' heap memory without
copying them. Zero allocations for string data.

**(2)** Date tag is captured at construction so the rotation check has a
baseline to compare against.

**(3)** `shutdown_` must be `false` before the thread starts. This is why member
declaration order matters (see above).

**(4)** `&Logger::workerLoop` is a **pointer-to-member-function**. `std::thread`
accepts it as a callable when paired with `this` as the object argument. The
thread begins executing `workerLoop()` immediately.

**(5)** `::mkdir` is a POSIX call. The `::` prefix reaches past any C++ namespace
to the global C function. Returns `-1` if the directory already exists
(`errno == EEXIST`) which is silently ignored — the right behavior here.

**(6)** Opens the first log file with today's date in the filename.

---

## 5. The producer side — `log()`

**File: `logger.cpp:91`**

```cpp
void Logger::log(LogLevel level, const std::string& msg) {
    if (level < config_.minLevel) return;          // (1) filter — no lock needed

    LogEntry entry{
        level,
        currentTimestamp(),                         // (2) timestamp now
        sanitize(msg)                               // (3) strip injection chars
    };

    std::unique_lock<std::mutex> lock(mutex_);      // (4)

    cv_.wait(lock, [this] {                         // (5) back-pressure
        return queue_.size() < config_.maxQueueSize || shutdown_;
    });

    if (shutdown_) return;

    queue_.push(std::move(entry));                  // (6) O(1), no copy
    lock.unlock();                                  // (7) release before notify
    cv_.notify_one();                               // (8) wake the worker
}
```

**(1)** The filter check happens **before** acquiring `mutex_`. This means
high-frequency DEBUG calls in a production system configured at WARN level cost
essentially nothing — one integer comparison with no lock contention.

**(2)** Timestamp generated on the caller's thread. See Section 3 for why.

**(3)** `sanitize()` replaces `\n`, `\r`, `\0` with escaped literals. This runs
before enqueueing so the worker never sees raw injection characters.

**(4) `unique_lock` vs `lock_guard`** — `unique_lock` is required here because
`cv_.wait()` needs to **temporarily release the lock** while the thread sleeps.
`lock_guard` has no `unlock()` method and cannot be used with condition
variables.

**(5) `cv_.wait()` with a predicate** — this is the back-pressure mechanism. If
the queue has reached `maxQueueSize`, the producer sleeps here until the worker
makes space. The predicate lambda `[this]{...}` protects against **spurious
wakeups** (the OS can wake a sleeping thread for no reason; the predicate
re-checks the condition and goes back to sleep if it's not met).

**(6) `std::move(entry)`** — moves the `LogEntry` into the queue. The strings
inside `entry` are transferred (pointer swap), not copied. Without `move`, two
heap allocations would be needed for every log call.

**(7)** Releasing the lock before `notify_one()` is an optimization: if the
worker is waiting, it can acquire the lock immediately instead of waking up and
then blocking on it.

**(8)** `notify_one()` wakes exactly one waiting thread (the worker). If no
thread is waiting, it's a no-op.

---

## 6. The consumer side — `workerLoop()`

**File: `logger.cpp:123`**

```cpp
void Logger::workerLoop() {
    while (true) {
        std::unique_lock<std::mutex> lock(mutex_);

        cv_.wait(lock, [this] {                         // (1)
            return !queue_.empty() || shutdown_;
        });

        while (!queue_.empty()) {                        // (2) drain inner loop
            LogEntry entry = std::move(queue_.front());
            queue_.pop();
            lock.unlock();
            cv_.notify_all();                            // (3) unblock producers

            rotateIfNeeded();                            // (4)

            std::string line = entry.timestamp
                             + " [" + levelToString(entry.level) + "] "
                             + entry.message + "\n";

            file_ << line;
            file_.flush();                               // (5)
            bytesWritten_ += line.size();

            lock.lock();                                 // (6) re-acquire
        }

        if (shutdown_) break;                            // (7)
    }
}
```

**(1)** The worker sleeps here when the queue is empty. `cv_.wait()` atomically
releases `lock` and suspends the thread. When a producer calls `notify_one()`,
the worker wakes, re-acquires the lock, and re-checks the predicate.

**(2) The inner drain loop** — the worker processes **all available messages**
before going back to sleep. This is more efficient than waking once per message
because the wakeup/sleep cycle has overhead (OS context switch).

**(3) `cv_.notify_all()`** — when the worker pops an entry and the queue drops
below `maxQueueSize`, it notifies all threads that might be blocked in the
back-pressure wait in `log()`. `notify_all` (not `notify_one`) because multiple
producer threads might be waiting.

**(4) `rotateIfNeeded()`** is called **here** (on the worker thread), not in the
producers. This means `bytesWritten_` and `currentDateTag_` are only ever
touched by one thread — no mutex needed for those members.

**(5) `file_.flush()`** — forces the OS to write buffered data to disk after
every entry. This costs throughput but ensures durability: if the process
crashes after writing an entry, that entry is on disk. In a throughput-first
system you'd flush every N entries or every T milliseconds instead.

**(6)** The lock is re-acquired before the next iteration of the inner `while`
so that `queue_.empty()` and `queue_.front()` are protected.

**(7) Drain-then-exit** — `shutdown_` is only checked **after** the inner loop
drains the queue completely. This guarantees that even if the destructor sets
`shutdown_` while 500 messages are queued, all 500 will be written before the
thread exits.

---

## 7. Destructor — the RAII guarantee

**File: `logger.cpp:59`**

```cpp
Logger::~Logger() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shutdown_ = true;
    }                          // (1) lock released here
    cv_.notify_one();          // (2) wake the worker
    worker_.join();            // (3) wait for full drain
    // file_ destructor runs automatically — (4)
}
```

**(1)** `shutdown_` is set inside a `lock_guard` scope. The braces cause the
lock to be released before `notify_one()`. This prevents the worker from waking
up and immediately blocking on the mutex that the destructor still holds.

**(2)** If the worker is sleeping (queue was empty), this wakes it so it can
see `shutdown_` and exit.

**(3) `worker_.join()`** blocks until the worker thread exits. Without this, the
thread could still be running when the Logger's members are destroyed —
accessing freed memory (use-after-free, undefined behavior). This is the most
critical line in the destructor.

**(4)** `std::ofstream::~ofstream()` flushes the internal buffer and closes the
file descriptor automatically. No `file_.close()` needed — RAII again.

**What happens without `join()`?**

`std::thread::~thread()` calls `std::terminate()` if the thread is still
joinable. So forgetting `join()` would crash the program, not silently leak —
a safety net, but a harsh one.

---

## 8. File rotation — `rotateIfNeeded()` and `openNewFile()`

**File: `logger.cpp:199` and `167`**

```cpp
void Logger::rotateIfNeeded() {
    bool sizeExceeded = bytesWritten_ >= config_.maxFileSizeBytes;
    bool dateChanged  = currentDateTag() != currentDateTag_;
    if (sizeExceeded || dateChanged) openNewFile();
}

void Logger::openNewFile() {
    if (file_.is_open()) file_.close();
    currentDateTag_ = currentDateTag();
    std::string path = config_.logDir + "/" + config_.baseFilename
                     + "_" + currentDateTag_ + ".log";
    file_.open(path, std::ios::out | std::ios::app);   // (1)
    if (!file_.is_open()) throw std::runtime_error(...);
    ::chmod(path.c_str(), 0600);                        // (2)
    bytesWritten_ = 0;                                  // (3)
}
```

**(1) `std::ios::app`** — positions the write pointer at the **end of the file**
on every write. If the process restarts on the same day, it appends to the
existing file rather than overwriting it. Using `std::ios::trunc` here would
destroy existing log data on restart — a serious bug in a financial system.

**(2)** `::chmod(0600)` is called after `open()`. The file must exist before
you can chmod it. Mode `0600` in octal = `110 000 000` in binary =
`rw-------` (owner read+write, no group/others access).

**(3)** `bytesWritten_` is reset to 0. It is only ever read and written by the
worker thread (inside `workerLoop()`), so no mutex protection is needed — a
deliberate single-threaded access design.

---

## 9. Security — `sanitize()` and log injection

**File: `logger.cpp:250`**

```cpp
std::string Logger::sanitize(const std::string& msg) const {
    std::string out;
    out.reserve(msg.size());           // (1)
    for (char c : msg) {               // (2)
        switch (c) {
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\0': out += "\\0";  break;
            default:   out += c;      break;
        }
    }
    return out;
}
```

**The attack this prevents (CWE-117):**

```
Input:  "user=alice\nINFO  2024-01-15T14:00:00 [INFO ] auth: root login ok"
Without sanitize(): two lines appear in the log — a forged entry
With sanitize():    "user=alice\\nINFO  2024-01-15T14:00..." — one line, attack visible
```

**(1) `out.reserve(msg.size())`** — pre-allocates the output string's buffer to
at least the input length. For clean inputs (no injection chars) this avoids
any reallocation. For inputs with injection chars it slightly underestimates
(the escaped `\n` → `\\n` is 2 bytes), but that's acceptable — the reserve is
an optimization hint, not a hard limit.

**(2) Range-based for on `std::string`** — iterates over `char` values. This
handles the common case correctly. For fully internationalized text you would
iterate over Unicode code points (UTF-8 aware), but for log messages in a
payment system ASCII is sufficient.

---

## 10. Timestamp — thread-safe time formatting

**File: `logger.cpp:215`**

```cpp
std::string Logger::currentTimestamp() const {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto ms  = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t t = system_clock::to_time_t(now);

    struct tm tm_buf;
    ::localtime_r(&t, &tm_buf);                              // (1)
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%S")      // (2)
        << "." << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}
```

**(1) `localtime_r` vs `localtime`** — `std::localtime()` writes into a
**shared static buffer**. Two threads calling it simultaneously corrupt each
other's result — a data race. `::localtime_r()` is the POSIX reentrant version
that writes into a caller-supplied `struct tm` on the stack. Safe to call from
any thread simultaneously.

**(2) `std::put_time`** — a C++11 I/O manipulator that formats a `struct tm`
using `strftime`-style patterns. In C++17 you'd use `std::format`; in C++14 we
compose it manually with `ostringstream`. The `%` in the `ms.count()` line
extracts the sub-second remainder: dividing total milliseconds by 1000 gives
the millisecond component (0–999).

---

## 11. Key C++ patterns used — summary for exam

| Pattern / Feature | Where in code | Why used |
|---|---|---|
| RAII | Constructor + Destructor | Guarantees thread start/stop and file open/close without explicit teardown |
| Producer-Consumer | `log()` + `workerLoop()` | Decouples caller latency from disk I/O latency |
| `unique_lock` | `log()` L100, `workerLoop()` L125 | Required by `condition_variable::wait()` — must be able to release mid-scope |
| `lock_guard` | `~Logger()` L61, `queueSize()` L268 | Simpler scoped lock where no mid-scope unlock is needed |
| `condition_variable` | `cv_` throughout | Puts worker to sleep (not spinning) when queue is empty; wakes producers on drain |
| `std::move` | `log()` L110, `workerLoop()` L135 | Transfers string ownership into/out of queue without heap allocation |
| `enum class` | `LogLevel` | Scoped, type-safe, ordered — enables `<` comparison for level filtering |
| `mutable` | `mutex_` | Allows locking in `const` methods without breaking const-correctness |
| Member init order | `logger.h:107–124` | `worker_` last ensures all other members are ready before the thread starts |
| `::localtime_r` | `currentTimestamp()` | POSIX reentrant version — safe to call from multiple threads |
| `::chmod(0600)` | `openNewFile()` | POSIX file permission restriction — OWASP sensitive data exposure defense |
| `std::ios::app` | `openNewFile()` | Append mode — survives process restart without truncating existing log data |
| Log injection defense | `sanitize()` | Escapes `\n`, `\r`, `\0` — OWASP CWE-117 mitigation |
| Back-pressure | `cv_.wait()` in `log()` | Blocks producer if queue full — prevents silent data loss in payment system |
| Drain on shutdown | Inner `while` in `workerLoop()` | Ensures all queued messages are written before thread exits |
