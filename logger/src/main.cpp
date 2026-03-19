#include <logger.h>

#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <sstream>
#include <atomic>
#include <random>
#include <iomanip>

// 
// Payment Worker: Simulador de pagos
//  
// 
void paymentWorker(Logger& logger, int workerId, int numTransactions,
                   std::atomic<int>& completedCount) {
    std::mt19937 rng(workerId * 42); 
    std::uniform_int_distribution<int> delayUs(100, 2000); 
    std::uniform_int_distribution<int> amountCents(100, 100000);

    for (int i = 0; i < numTransactions; ++i) {
        int txnId  = workerId * 10000 + i;
        int amount = amountCents(rng);

        // Simulate a transaction
        {
            std::ostringstream oss;
            oss << "TXN-" << txnId << " initiated: $"
                << (amount / 100) << "." << std::setfill('0') << std::setw(2)
                << (amount % 100);
            logger.info(oss.str());
        }

        //warning mock
        if (i % 7 == 0) {
            std::ostringstream oss;
            oss << "TXN-" << txnId << " slow response from payment gateway, retrying";
            logger.warn(oss.str());
        }

        // Insuf. funds mock
        if (i % 23 == 0) {
            std::ostringstream oss;
            oss << "TXN-" << txnId << " declined: insufficient funds";
            logger.error(oss.str());
        }

        //  LOG INJECTION TEST 
        if (i == 5) {
            logger.info("user=hacker\nINFO  auth: root login succeeded");
        }

        // debug level
        {
            std::ostringstream oss;
            oss << "TXN-" << txnId << " worker-" << workerId << " tick " << i;
            logger.debug(oss.str());
        }

        completedCount.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::sleep_for(std::chrono::microseconds(delayUs(rng)));
    }
}

// 
// Producer 1
// 
void atmWorker(Logger& logger, int atmId, int numOps) {
    const char* ops[] = {"WITHDRAWAL", "BALANCE_CHECK", "DEPOSIT", "PIN_CHANGE"};
    for (int i = 0; i < numOps; ++i) {
        std::ostringstream oss;
        oss << "ATM-" << atmId << " op=" << ops[i % 4]
            << " card=****" << (1000 + atmId * 7 + i);
        logger.info(oss.str());
        //completedCount.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

int main() {
    std::cout << "=== Logger Demo ===\n";
    std::cout << "Log files will be written to ./logs/\n\n";

    LoggerConfig cfg;
    cfg.logDir           = "./logs";
    cfg.baseFilename     = "txn";
    cfg.maxFileSizeBytes = 512 * 1024;  // 512 KB
    cfg.minLevel         = LogLevel::DEBUG;

    Logger logger(cfg);
    logger.info("=== System startup ===");
    logger.info("Logger initialized: dir=" + cfg.logDir +
                " maxSize=" + std::to_string(cfg.maxFileSizeBytes) + "B");

    //  Launch producer threads 
    const int NUM_PAYMENT_THREADS = 4;
    const int TXN_PER_THREAD      = 50;
    const int NUM_ATM_THREADS     = 2;
    const int OPS_PER_ATM         = 30;

    std::atomic<int> completedTransactions{0};
    std::vector<std::thread> threads;
    threads.reserve(NUM_PAYMENT_THREADS + NUM_ATM_THREADS);

    auto t0 = std::chrono::steady_clock::now();

    for (int i = 0; i < NUM_PAYMENT_THREADS; ++i) {
        threads.emplace_back(paymentWorker,
                             std::ref(logger), i, TXN_PER_THREAD,
                             std::ref(completedTransactions));
    }

    for (int i = 0; i < NUM_ATM_THREADS; ++i) {
        threads.emplace_back(atmWorker, std::ref(logger), i, OPS_PER_ATM ,std::ref(completedTransactions));
    }

    //  Wait for all producers to finish 
    for (auto& t : threads) t.join();

    auto t1 = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0);

    logger.info("=== All workers done in " + std::to_string(elapsed.count()) + " ms ===");
    logger.info("Completed transactions: " + std::to_string(completedTransactions.load()));

    //  Logger destructor runs here 
    // It will drain any remaining queued messages and close the file cleanly.
    std::cout << "Completed " << completedTransactions.load() << " transactions in "
              << elapsed.count() << " ms.\n";
    std::cout << "Check ./logs/ for output.\n";

    return 0;
}
