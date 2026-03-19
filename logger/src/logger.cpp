#include <logger.h>

#include <iostream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <ctime>
#include <stdexcept>
#include <sys/stat.h>   // chmod — POSIX, used to restrict file permissions

// 
// levelToString 
// 
const char* levelToString(LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO:  return "INFO ";
        case LogLevel::WARN:  return "WARN ";
        case LogLevel::ERR:   return "ERROR";
    }
    return "????";
}


Logger::Logger(LoggerConfig config)
    : config_(std::move(config))
    , currentDateTag_(currentDateTag())
    , shutdown_(false)
    , worker_(&Logger::workerLoop, this)  // starts the thread 
{
    
    ::mkdir(config_.logDir.c_str(), 0755);

    openNewFile();
}
 
Logger::~Logger() {

    {
        std::lock_guard<std::mutex> lock(mutex_);
        shutdown_ = true;
    }

    cv_.notify_one();   
    worker_.join();     
    
}

// 
// public api
// 
void Logger::debug(const std::string& msg) { log(LogLevel::DEBUG, msg); }
void Logger::info (const std::string& msg) { log(LogLevel::INFO,  msg); }
void Logger::warn (const std::string& msg) { log(LogLevel::WARN,  msg); }
void Logger::error(const std::string& msg) { log(LogLevel::ERR,   msg); }

// 
// log() 
//  - generate the timestamp
//  - filter below minLevel
//  - if the queue is full (worker can't keep up), we block the
//    caller rather than dropping messages.
// 
void Logger::log(LogLevel level, const std::string& msg) {
    if (level < config_.minLevel) return;

    LogEntry entry{
        level,
        currentTimestamp(),
        sanitize(msg)
    };

    std::unique_lock<std::mutex> lock(mutex_);

    cv_.wait(lock, [this] {
        return queue_.size() < config_.maxQueueSize || shutdown_;
    });

    if (shutdown_) return;  // logger is dying. -> die.

    queue_.push(std::move(entry));
    lock.unlock();
    cv_.notify_one();  // wake the worker
}

// 
// workerLoop()
//
// wait -> dequeue -> write -> repeat -> on shutdown, drain then exit.
//
// 
void Logger::workerLoop() {
    while (true) {
        std::unique_lock<std::mutex> lock(mutex_);

        cv_.wait(lock, [this] {
            return !queue_.empty() || shutdown_;
        });

        while (!queue_.empty()) {
            LogEntry entry = std::move(queue_.front());
            queue_.pop();
            lock.unlock();
            cv_.notify_all(); 

            //  Write to file 

            rotateIfNeeded();

            // Format: 2025-03-15T14:23:01.123 [INFO ] 
            std::string line = entry.timestamp
                             + " [" + levelToString(entry.level) + "] "
                             + entry.message + "\n";

            file_ << line;
            file_.flush();
            bytesWritten_ += line.size();

            lock.lock();
        }

        if (shutdown_) break;
    }
}

// 
// openNewFile() — creates a new log file with a timestamped name.
//
// chmod(0600) file owner-only read/write.
// 
void Logger::openNewFile() {
    if (file_.is_open()) file_.close();

    currentDateTag_ = currentDateTag();
    std::string path = config_.logDir + "/" + config_.baseFilename
                     + "_" + currentDateTag_ + ".log";

    file_.open(path, std::ios::out | std::ios::app);
    if (!file_.is_open()) {
        // We can't log this error (we ARE the logger), so stderr is the fallback.
        std::cerr << "[Logger] FATAL: cannot open log file: " << path << "\n";
        throw std::runtime_error("Logger: failed to open log file: " + path);
    }

    
    ::chmod(path.c_str(), 0600);

    bytesWritten_ = 0;
}

// 
// rotateIfNeeded() 
//
//   1. File size exceeds maxFileSizeBytes -> size-based rotation.
//   2. Calendar date changed -> daily rotation.
// 
void Logger::rotateIfNeeded() {
    bool sizeExceeded = bytesWritten_ >= config_.maxFileSizeBytes;
    bool dateChanged  = currentDateTag() != currentDateTag_;

    if (sizeExceeded || dateChanged) {
        openNewFile();
    }
}

// 
// currentTimestamp() — with milliseconds.
// 
std::string Logger::currentTimestamp() const {
    using namespace std::chrono;
    auto now     = system_clock::now();
    auto ms      = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t t = system_clock::to_time_t(now);

    std::ostringstream oss;
    // localtime_r (POSIX)
    struct tm tm_buf;
    ::localtime_r(&t, &tm_buf);
    oss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%S")
        << "." << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

std::string Logger::currentDateTag() const {
    std::time_t t = std::time(nullptr);
    struct tm tm_buf;
    ::localtime_r(&t, &tm_buf);
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%d");
    return oss.str();
}

// 
// sanitize () -> replace \n, \r, and null bytes (user entry maybe)
// 
std::string Logger::sanitize(const std::string& msg) const {
    std::string out;
    out.reserve(msg.size());
    for (char c : msg) {
        switch (c) {
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\0': out += "\\0";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

// 
// Inspection helpers
// 
std::size_t Logger::queueSize() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

bool Logger::isRunning() const {
    return worker_.joinable() && !shutdown_;
}
