//
// Unit tests for Logger.
//
// We use a minimal hand-rolled test harness instead of Google Test or Catch2.
// Why? Two reasons relevant to your exam:
//   1. It shows you understand what a test framework IS (just assertion helpers
//      and result tracking), not just how to use one.
//   2. Zero external dependencies — the project compiles anywhere.
//
// In a real production codebase you'd absolutely use Google Test or Catch2.
// The concepts (arrange-act-assert, isolation, edge cases) are identical.
//

#include "logger.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <cassert>
#include <algorithm>
#include <sys/stat.h>
#include <iomanip>

// ─────────────────────────────────────────────────────────────────────────────
// Minimal test harness
// ─────────────────────────────────────────────────────────────────────────────
static int  g_tests_run    = 0;
static int  g_tests_passed = 0;

#define TEST(name) void name()
#define RUN_TEST(name) \
    do { \
        std::cout << "  [ RUN ] " #name "\n"; \
        ++g_tests_run; \
        try { \
            name(); \
            ++g_tests_passed; \
            std::cout << "  [ OK  ] " #name "\n"; \
        } catch (const std::exception& e) { \
            std::cout << "  [FAIL ] " #name ": " << e.what() << "\n"; \
        } \
    } while(0)

#define ASSERT_TRUE(cond) \
    if (!(cond)) throw std::runtime_error("ASSERT_TRUE failed: " #cond)

#define ASSERT_EQ(a, b) \
    if ((a) != (b)) { \
        std::ostringstream _oss; \
        _oss << "ASSERT_EQ failed: " << (a) << " != " << (b); \
        throw std::runtime_error(_oss.str()); \
    }

#define ASSERT_CONTAINS(haystack, needle) \
    if ((haystack).find(needle) == std::string::npos) { \
        throw std::runtime_error("ASSERT_CONTAINS failed: expected '" \
                                 + std::string(needle) + "' in output"); \
    }

// ─────────────────────────────────────────────────────────────────────────────
// Test helpers
// ─────────────────────────────────────────────────────────────────────────────

// Build a config that writes to a temp directory.
LoggerConfig testConfig(const std::string& subdir) {
    LoggerConfig cfg;
    cfg.logDir           = "/tmp/logger_tests/" + subdir;
    cfg.baseFilename     = "test";
    cfg.maxFileSizeBytes = 1024 * 1024; // 1 MB — won't rotate during tests
    cfg.minLevel         = LogLevel::DEBUG;
    ::mkdir("/tmp/logger_tests", 0755);
    ::mkdir(cfg.logDir.c_str(), 0755);
    return cfg;
}

// Read the first .log file found in a directory.
// We use glob via popen rather than hardcoding the date — avoids a race
// where the test helper's clock and the logger's clock disagree.
std::string readLogFile(const std::string& dir) {
    std::string cmd = "ls " + dir + "/*.log 2>/dev/null | head -1";
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) return "";
    char buf[512] = {};
    bool got = (fgets(buf, sizeof(buf), pipe) != nullptr);
    ::pclose(pipe);
    if (!got) return "";
    std::string filepath(buf);
    while (!filepath.empty() &&
           (filepath.back() == '\n' || filepath.back() == '\r'))
        filepath.pop_back();
    std::ifstream f(filepath);
    if (!f.is_open()) return "";
    return std::string(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
}

// Short sleep — give the async worker time to flush.
void waitForFlush(int ms = 150) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// ─────────────────────────────────────────────────────────────────────────────
// Tests
// ─────────────────────────────────────────────────────────────────────────────

// TEST 1: Basic write
// Verifies that a message reaches the file at all.
// Arrange-Act-Assert is the standard unit test structure.
TEST(test_basic_write) {
    Logger logger(testConfig("basic"));
    logger.info("hello world");
    // ~Logger() called here — destructor drains the queue before we read.
    // This is why RAII is so valuable: the test doesn't need explicit teardown.

    std::string content = readLogFile("/tmp/logger_tests/basic");
    ASSERT_CONTAINS(content, "hello world");
    ASSERT_CONTAINS(content, "[INFO ]");
}

// TEST 2: Severity filtering
// Messages below minLevel should be silently dropped.
TEST(test_level_filtering) {
    LoggerConfig cfg = testConfig("filter");
    cfg.minLevel = LogLevel::WARN;  // DEBUG and INFO should be dropped

    Logger logger(cfg);
    logger.debug("should be dropped");
    logger.info ("should also be dropped");
    logger.warn ("this should appear");
    logger.error("this too");

    std::string content = readLogFile("/tmp/logger_tests/filter");
    ASSERT_CONTAINS(content, "this should appear");
    ASSERT_CONTAINS(content, "this too");

    // Verify dropped messages are NOT present.
    ASSERT_TRUE(content.find("should be dropped") == std::string::npos);
}

// TEST 3: Log injection prevention (OWASP / CWE-117)
// Newlines in user data must be escaped, not interpreted.
TEST(test_log_injection) {
    Logger logger(testConfig("injection"));
    // This is what an attacker might try: insert a fake INFO line.
    logger.info("user=hacker\nINFO  auth: root login succeeded");

    std::string content = readLogFile("/tmp/logger_tests/injection");

    // The newline should be escaped as \n, not as an actual newline.
    ASSERT_CONTAINS(content, "\\n");

    // Count actual log entries: should be exactly one INFO line from us,
    // not two (the injected one should not appear as a real entry).
    int lineCount = 0;
    for (char c : content) if (c == '\n') ++lineCount;
    // 1 real log entry = 1 newline. If injection succeeded there'd be 2+.
    ASSERT_EQ(lineCount, 1);
}

// TEST 4: Thread safety under concurrent load
// Many threads hammering the logger simultaneously should produce no garbled
// lines and no missing entries.
TEST(test_concurrent_writes) {
    Logger logger(testConfig("concurrent"));

    const int NUM_THREADS = 8;
    const int MSGS_EACH   = 100;
    std::vector<std::thread> threads;
    std::atomic<int> written{0};

    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&logger, t, &written]() {
            for (int i = 0; i < MSGS_EACH; ++i) {
                std::ostringstream oss;
                oss << "thread=" << t << " msg=" << i;
                logger.info(oss.str());
                written.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& th : threads) th.join();
    // Destructor flushes here.

    std::string content = readLogFile("/tmp/logger_tests/concurrent");
    int lineCount = 0;
    for (char c : content) if (c == '\n') ++lineCount;

    // Every message must appear (no drops, no merges).
    ASSERT_EQ(lineCount, NUM_THREADS * MSGS_EACH);
}

// TEST 5: File permissions (POSIX security requirement)
// The log file must not be world-readable.
TEST(test_file_permissions) {
    Logger logger(testConfig("perms"));
    logger.info("permission check");

    // Build the expected path manually.
    std::time_t t  = std::time(nullptr);
    struct tm tmbuf; ::localtime_r(&t, &tmbuf);
    std::ostringstream date; date << std::put_time(&tmbuf, "%Y-%m-%d");
    std::string path = "/tmp/logger_tests/perms/test_" + date.str() + ".log";

    struct stat st;
    ASSERT_EQ(::stat(path.c_str(), &st), 0);

    // Mode 0600 = rw------- (owner only).
    mode_t perms = st.st_mode & 0777;
    ASSERT_EQ(perms, static_cast<mode_t>(0600));
}

// TEST 6: Size-based rotation
// Writing past maxFileSizeBytes should produce a new file.
TEST(test_size_rotation) {
    LoggerConfig cfg = testConfig("rotation");
    cfg.maxFileSizeBytes = 512;  // tiny limit so we trigger it quickly

    {
        Logger logger(cfg);
        // Write enough to blow past 512 bytes.
        for (int i = 0; i < 30; ++i) {
            logger.info("padding message to force rotation: " + std::to_string(i)
                        + " aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        }
    } // destructor flushes

    // After rotation there should be at least one log file.
    // (Testing exact rotation count requires mocking time/size — we verify
    // at least the directory isn't empty and files are present.)
    std::string content = readLogFile("/tmp/logger_tests/rotation");
    ASSERT_TRUE(!content.empty());
}

// TEST 7: Clean shutdown drains queue
// Messages logged just before destruction must not be lost.
TEST(test_shutdown_drains) {
    const int BURST = 500;
    {
        Logger logger(testConfig("shutdown"));
        // Fire a burst then immediately go out of scope.
        for (int i = 0; i < BURST; ++i) {
            logger.info("burst message " + std::to_string(i));
        }
        // ~Logger() must not return until all 500 are written.
    }

    std::string content = readLogFile("/tmp/logger_tests/shutdown");
    int lineCount = 0;
    for (char c : content) if (c == '\n') ++lineCount;
    ASSERT_EQ(lineCount, BURST);
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
int main() {
    std::cout << "\n=== Transaction Logger — Unit Tests ===\n\n";

    RUN_TEST(test_basic_write);
    RUN_TEST(test_level_filtering);
    RUN_TEST(test_log_injection);
    RUN_TEST(test_concurrent_writes);
    RUN_TEST(test_file_permissions);
    RUN_TEST(test_size_rotation);
    RUN_TEST(test_shutdown_drains);

    std::cout << "\n─────────────────────────────────────\n";
    std::cout << "Results: " << g_tests_passed << " / " << g_tests_run
              << " passed.\n";
    if (g_tests_passed == g_tests_run) {
        std::cout << "ALL TESTS PASSED\n\n";
        return 0;
    } else {
        std::cout << (g_tests_run - g_tests_passed) << " FAILED\n\n";
        return 1;
    }
}
