#pragma once

#include <string>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <fstream>
#include <cstdint>

// 
// Severity levels 
// DEBUG < INFO < WARN < ERROR
// 
enum class LogLevel {
    DEBUG = 0,
    INFO  = 1,
    WARN  = 2,
    ERR   = 3   
};

// Convert a LogLevel to its string tag
const char* levelToString(LogLevel level);

// 
// LoggerConfig — all tunable parameters in one place.
// ToDo: 
// 
struct LoggerConfig {
    std::string  logDir        = "./logs";   // directory for log files
    std::string  baseFilename  = "logger";      // prefix: logger_2024-01-15.log
    std::size_t  maxFileSizeBytes = 10 * 1024 * 1024; // 10 MB before rotation
    LogLevel     minLevel      = LogLevel::DEBUG;   
    std::size_t  maxQueueSize  = 8192;      
};

// 
// Logger — thread-safe, asynchronous, rotating file logger.
//
// Design choices worth knowing for your exam:
//
//  1. SINGLETON? No. A singleton Logger couples the whole codebase to one
//     instance and makes unit testing a nightmare. Instead, pass the Logger
//     by reference or pointer. This is the Dependency Injection principle.
//
//  2. RAII: The constructor starts the worker thread; the destructor stops it
//     and flushes the queue. This means you can't forget to shut down.
//
//  3. NON-COPYABLE: Copying a Logger would copy the thread handle and mutex,
//     which is nonsensical. We explicitly delete copy constructor/assignment.
// 
class Logger {
public:
    explicit Logger(LoggerConfig config = LoggerConfig{});

    // Destructor signals shutdown, waits for the worker thread to drain the
    // queue, and closes the file. Guaranteed clean exit.
    ~Logger();

    // Non-copyable, non-movable (thread + mutex semantics don't allow it).
    Logger(const Logger&)            = delete;
    Logger& operator=(const Logger&) = delete;

    //  Public logging interface 
    // These are the only functions callers ever touch.
    // They push to the queue (fast, O(1)) and return immediately.
    void debug(const std::string& msg);
    void info (const std::string& msg);
    void warn (const std::string& msg);
    void error(const std::string& msg);

    // Generic version — useful when level is determined at runtime.
    void log(LogLevel level, const std::string& msg);

    //  Inspection 
    std::size_t queueSize() const;   // snapshot (for monitoring/tests)
    bool        isRunning() const;

private:
    //  Internal message type 
    // We bundle level + message into one struct so the queue holds complete,
    // self-describing entries. Timestamp is generated at enqueue time, not
    // at write time — this preserves the order events actually happened.
    struct LogEntry {
        LogLevel    level;
        std::string timestamp;  // ISO 8601 e.g. "2024-01-15T14:23:01.123"
        std::string message;
    };

    //  Worker thread 
    // This is the single background thread that does all file I/O.
    void workerLoop();

    //  File rotation 
    void openNewFile();
    void rotateIfNeeded();
    bool shouldRotate() const;

    //  Helpers 
    std::string currentTimestamp() const;
    std::string currentDateTag()   const;  // "2024-01-15" for filename
    std::string sanitize(const std::string& msg) const; // strip log injection chars

    //  State 
    LoggerConfig           config_;
    std::string            currentDateTag_;   // detect midnight rollover
    std::ofstream          file_;
    std::size_t            bytesWritten_ = 0; // track size for rotation

    //  Concurrency primitives 
    // mutex_ guards queue_ — it's the "bathroom key" metaphor from earlier.
    // cv_ is the condition variable: the worker thread sleeps on it when the
    // queue is empty, and producers wake it up when they push a message.
    // This is much better than a spin-loop (which would waste a CPU core).
    mutable std::mutex      mutex_;
    std::condition_variable cv_;
    std::queue<LogEntry>    queue_;
    bool                    shutdown_ = false;

    // Thread is declared last — it starts in the constructor body, after all
    // other members are initialized. Order of declaration = order of init.
    std::thread             worker_;
};
