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
// ToDo: env
// 
struct LoggerConfig {
    std::string  logDir        = "./logs";   // directory for log files
    std::string  baseFilename  = "logger";      // prefix: logger_2024-01-15.log
    std::size_t  maxFileSizeBytes = 10 * 1024 * 1024; // 10 MB before rotation
    LogLevel     minLevel      = LogLevel::DEBUG;   
    std::size_t  maxQueueSize  = 8192;
    LoggerConfig() = default;

    LoggerConfig(std::string _logDir, std::string _baseFileName) 
        : logDir(std::move(_logDir)), baseFilename(std::move(_baseFileName)) {}

};

// 
// Logger — thread-safe, asynchronous, rotating file logger.
//
//
//  2. RAII: The constructor starts the worker thread. the destructor stops it
//     and flushes the queue.
//
//  3. NON-COPYABLE: Copying a Logger would copy the thread handle and mutex,
//     which is nonsensical. We explicitly delete copy constructor/assignment.
// 
class Logger {
public:
    explicit Logger(LoggerConfig config = LoggerConfig{});


    ~Logger();

    Logger(const Logger&)            = delete;
    Logger& operator=(const Logger&) = delete;

    //  Public logging interface 
    void debug(const std::string& msg);
    void info (const std::string& msg);
    void warn (const std::string& msg);
    void error(const std::string& msg);

    // generic
    void log(LogLevel level, const std::string& msg);

    //  inspetcion 
    std::size_t queueSize() const;   // snapshot (for monitoring/tests)
    bool        isRunning() const;

private:
    //  Internal message type 
    struct LogEntry {
        LogLevel    level;
        std::string timestamp;  // ISO 8601 e.g. "2024-01-15T14:23:01.123"
        std::string message;
    };

    //  Worker thread 
    //worker responsible for file I/O.
    void workerLoop();

    //  File rotation 
    void openNewFile();
    void rotateIfNeeded();
    //bool shouldRotate() const;

    //  Helpers 
    std::string currentTimestamp() const;
    std::string currentDateTag()   const;  // "2024-01-15" for filename
    std::string sanitize(const std::string& msg) const; // strip log injection chars

    //  State 
    LoggerConfig           config_;
    std::string            currentDateTag_;   // detect midnight rollover
    std::ofstream          file_;
    std::size_t            bytesWritten_ = 0; // track size for rotation

    
    mutable std::mutex      mutex_;
    std::condition_variable cv_;
    std::queue<LogEntry>    queue_;
    bool                    shutdown_ = false;

    std::thread             worker_;
};
