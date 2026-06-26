#include <iostream>
#include <iomanip>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <string>

#include "cache_interface.h"
#include "lru_cache.h"
#include "lfu_cache.h"
#include "workload.h"

// ============================================================================
// Benchmark configuration — edit here to change run parameters.
// ============================================================================
static constexpr int OPS      = 100'000;   // total operations per trace
static constexpr int KEYS     = 2'500;     // distinct keys in the universe
static constexpr int CAPACITY = 500;       // cache capacity (20 % of KEYS)
static constexpr int THREADS  = 4;         // worker threads for concurrent run

// ============================================================================
// Background janitor
// Calls pruneExpired() on a cache at a fixed interval until stopped.
// ============================================================================
static std::atomic<bool> runJanitor{true};

void startBackgroundJanitor(CacheInterface<int, std::string>& cache,
                             int intervalMs) {
    std::thread([&cache, intervalMs]() {
        while (runJanitor.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
            cache.pruneExpired();
        }
    }).detach();
}

// ============================================================================
// Benchmark result
// ============================================================================
struct BenchmarkResult {
    double      elapsedMs;
    double      hitRate;
    double      throughputOpsPerSec;
    long long   totalOps;
};

// ============================================================================
// Sequential benchmark
// Replays a workload trace single-threaded.
// ============================================================================
BenchmarkResult runSequential(CacheInterface<int, std::string>& cache,
                               const std::string& traceFile) {
    auto commands = loadWorkload(traceFile);
    std::chrono::seconds ttl(600);
    long long total = static_cast<long long>(commands.size());

    auto start = std::chrono::high_resolution_clock::now();
    for (const auto& cmd : commands) {
        if (cmd.op == "PUT") cache.put(cmd.key, cmd.value, ttl);
        else                  cache.get(cmd.key);
    }
    auto end = std::chrono::high_resolution_clock::now();

    double ms  = std::chrono::duration<double, std::milli>(end - start).count();
    double ops = (ms > 0) ? (total / (ms / 1000.0)) : 0.0;
    return { ms, cache.getHitRate(), ops, total };
}

// ============================================================================
// Concurrent benchmark
// Splits the trace into equal chunks and gives one chunk to each thread.
// Measures wall-clock time from first thread launch to last thread join.
// ============================================================================
void workerTask(CacheInterface<int, std::string>& cache,
                const std::vector<CacheCommand>& commands) {
    std::chrono::seconds ttl(600);
    for (const auto& cmd : commands) {
        if (cmd.op == "PUT") cache.put(cmd.key, cmd.value, ttl);
        else                  cache.get(cmd.key);
    }
}

BenchmarkResult runConcurrent(CacheInterface<int, std::string>& cache,
                               const std::string& traceFile,
                               int numThreads) {
    auto all = loadWorkload(traceFile);
    int chunk = static_cast<int>(all.size()) / numThreads;

    std::vector<std::thread> workers;
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < numThreads; ++i) {
        auto s = all.begin() + i * chunk;
        auto e = (i == numThreads - 1) ? all.end() : s + chunk;
        workers.emplace_back(workerTask, std::ref(cache),
                             std::vector<CacheCommand>(s, e));
    }
    for (auto& t : workers) t.join();
    auto end = std::chrono::high_resolution_clock::now();

    double ms    = std::chrono::duration<double, std::milli>(end - start).count();
    long long tot = static_cast<long long>(all.size());
    double ops   = (ms > 0) ? (tot / (ms / 1000.0)) : 0.0;
    return { ms, cache.getHitRate(), ops, tot };
}

// ============================================================================
// Output helpers
// ============================================================================
static void printHeader() {
    std::cout
        << "\n"
        << "==========================================================================\n"
        << " TTL Cache Benchmark  |  cap=" << CAPACITY
        << "  keys=" << KEYS << "  ops=" << OPS << "\n"
        << "==========================================================================\n"
        << std::left  << std::setw(36) << " Config"
        << std::right << std::setw(12) << "Latency"
        << std::setw(12) << "Hit Rate"
        << std::setw(16) << "Throughput"
        << "\n"
        << "--------------------------------------------------------------------------\n";
}

static void printRow(const std::string& label, const BenchmarkResult& r) {
    std::cout
        << std::left  << std::setw(36) << label
        << std::right << std::setw(10) << std::fixed << std::setprecision(1)
        << r.elapsedMs << " ms"
        << std::setw(12) << std::setprecision(2) << r.hitRate     << " %"
        << std::setw(16) << std::setprecision(0)  << r.throughputOpsPerSec << " ops/s"
        << "\n";
}

static void printDivider() {
    std::cout << "--------------------------------------------------------------------------\n";
}

// ============================================================================
// main
// ============================================================================
int main() {
    const std::string uniformFile = "workload_uniform.txt";
    const std::string zipfFile    = "workload_zipf.txt";

    // ── Generate traces ──────────────────────────────────────────────────
    std::cout << "Generating workload traces...\n";
    generateWorkloadFile(uniformFile, OPS, KEYS, /*useZipf=*/false);
    generateWorkloadFile(zipfFile,    OPS, KEYS, /*useZipf=*/true);

    // ── Benchmark table ──────────────────────────────────────────────────
    printHeader();

    { TTL_LRUCache<int,std::string> c(CAPACITY); printRow(" [SEQ /Uniform]  LRU", runSequential(c, uniformFile)); }
    { TTL_LFUCache<int,std::string> c(CAPACITY); printRow(" [SEQ /Uniform]  LFU", runSequential(c, uniformFile)); }
    printDivider();
    { TTL_LRUCache<int,std::string> c(CAPACITY); printRow(" [SEQ /Zipf  ]   LRU", runSequential(c, zipfFile)); }
    { TTL_LFUCache<int,std::string> c(CAPACITY); printRow(" [SEQ /Zipf  ]   LFU", runSequential(c, zipfFile)); }
    printDivider();
    { TTL_LRUCache<int,std::string> c(CAPACITY); printRow(" [CONC/Uniform]  LRU", runConcurrent(c, uniformFile, THREADS)); }
    { TTL_LFUCache<int,std::string> c(CAPACITY); printRow(" [CONC/Uniform]  LFU", runConcurrent(c, uniformFile, THREADS)); }
    printDivider();
    { TTL_LRUCache<int,std::string> c(CAPACITY); printRow(" [CONC/Zipf  ]   LRU", runConcurrent(c, zipfFile,    THREADS)); }
    { TTL_LFUCache<int,std::string> c(CAPACITY); printRow(" [CONC/Zipf  ]   LFU", runConcurrent(c, zipfFile,    THREADS)); }

    std::cout << "==========================================================================\n\n";

    // ── TTL / Active Purge demo ──────────────────────────────────────────
    std::cout << "======================== TTL & ACTIVE PURGING ============================\n";

    TTL_LRUCache<int,std::string> liveLRU(5);
    TTL_LFUCache<int,std::string> liveLFU(5);

    startBackgroundJanitor(liveLRU, 100);
    startBackgroundJanitor(liveLFU, 100);

    liveLRU.put(999, "LRU_Secure_Payload", std::chrono::seconds(1));
    liveLFU.put(999, "LFU_Secure_Payload", std::chrono::seconds(1));

    std::cout << " Immediate LRU get (key 999): " << liveLRU.get(999) << "\n";
    std::cout << " Immediate LFU get (key 999): " << liveLFU.get(999) << "\n\n";
    std::cout << " [!] Sleeping 1.5 s — entry TTL is 1 s...\n\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    auto lruPost = liveLRU.get(999);
    auto lfuPost = liveLFU.get(999);
    std::cout << " Post-expiry LRU get (key 999): "
              << (lruPost.empty() ? "EXPIRED" : lruPost) << "\n";
    std::cout << " Post-expiry LFU get (key 999): "
              << (lfuPost.empty() ? "EXPIRED" : lfuPost) << "\n";
    std::cout << "==========================================================================\n\n";

    runJanitor = false;
    return 0;
}