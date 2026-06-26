#pragma once
#include <vector>
#include <string>
#include <fstream>
#include <random>
#include <cmath>
#include <algorithm>
#include <stdexcept>

// ============================================================================
// Workload generation
//
// Two distributions are provided:
//
//   Uniform — every key is equally likely.  Represents a fully random access
//     pattern with no locality; gives a theoretical hit-rate ceiling of
//     capacity / uniqueKeys regardless of policy.
//
//   Zipf (skew s = 1.0) — P(rank k) ∝ 1 / k^s.  The most popular key
//     receives twice as many requests as the second, three times as many as
//     the third, and so on.  This is the standard model for real caches
//     (web pages, database rows, DNS records): a small "hot set" absorbs the
//     majority of traffic.  Under Zipf, LFU retains hot keys across eviction
//     cycles while LRU may silently discard them during a sequential scan.
//
// The Zipf sampler pre-computes a CDF over uniqueKeys ranks and inverts it
// with std::lower_bound — O(log uniqueKeys) per sample, negligible vs I/O.
// ============================================================================

struct CacheCommand {
    std::string op;   // "GET" or "PUT"
    int         key;
    std::string value;
};

// Build a lookup table of `samples` integers drawn from a Zipf distribution
// over [1, uniqueKeys] with the given skew parameter.
inline std::vector<int> buildZipfTable(int uniqueKeys, double skew, int samples) {
    std::vector<double> cdf(uniqueKeys);
    double total = 0.0;
    for (int i = 0; i < uniqueKeys; ++i)
        total += 1.0 / std::pow(i + 1.0, skew);

    double cumulative = 0.0;
    for (int i = 0; i < uniqueKeys; ++i) {
        cumulative += (1.0 / std::pow(i + 1.0, skew)) / total;
        cdf[i] = cumulative;
    }

    std::mt19937 rng(42);
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    std::vector<int> table(samples);
    for (int i = 0; i < samples; ++i) {
        double r = dist(rng);
        int idx  = static_cast<int>(
            std::lower_bound(cdf.begin(), cdf.end(), r) - cdf.begin());
        table[i] = std::min(idx + 1, uniqueKeys);
    }
    return table;
}

// Write an operation trace to `filename`.
// ~1-in-11 operations are PUTs; the rest are GETs.
inline void generateWorkloadFile(const std::string& filename,
                                  int operations,
                                  int uniqueKeys,
                                  bool useZipf) {
    std::ofstream file(filename);
    if (!file) throw std::runtime_error("Cannot open " + filename);

    std::mt19937 rng(1337);
    std::uniform_int_distribution<int> opDist(0, 10);
    std::uniform_int_distribution<int> uniformDist(1, uniqueKeys);

    std::vector<int> zipfTable;
    if (useZipf) zipfTable = buildZipfTable(uniqueKeys, 1.0, operations);

    for (int i = 0; i < operations; ++i) {
        int key = useZipf ? zipfTable[i] : uniformDist(rng);
        if (opDist(rng) == 0)
            file << "PUT " << key << " value_" << key << "\n";
        else
            file << "GET " << key << "\n";
    }
}

// Parse a workload file into a vector of CacheCommands.
inline std::vector<CacheCommand> loadWorkload(const std::string& filename) {
    std::ifstream infile(filename);
    if (!infile) throw std::runtime_error("Cannot open " + filename);

    std::vector<CacheCommand> commands;
    std::string op, val;
    int key;
    while (infile >> op >> key) {
        CacheCommand cmd;
        cmd.op  = op;
        cmd.key = key;
        if (op == "PUT") { infile >> val; cmd.value = val; }
        commands.push_back(std::move(cmd));
    }
    return commands;
}