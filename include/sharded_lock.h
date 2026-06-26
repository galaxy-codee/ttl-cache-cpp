#pragma once
#include <shared_mutex>
#include <array>
#include <functional>

// ============================================================================
// ShardedRWLock<K, N>
//
// Splits the key space into N independent reader-writer mutexes.
// Threads whose keys hash to different shards never contend with each other,
// giving up to N× throughput improvement over a single global lock in
// read-heavy workloads.
//
// Why N=16?  A power of two keeps the modulo cheap (single AND instruction
// after std::hash).  16 shards is enough to saturate typical 4-8 core
// machines; raising it further yields diminishing returns and increases
// memory footprint.
//
// Deadlock prevention for global operations (clear, pruneExpired):
//   lockAllExclusive() acquires every shard in index order 0..N-1.
//   Because every caller uses the same order, circular-wait is impossible.
// ============================================================================
template <typename K, int N = 16>
class ShardedRWLock {
    static_assert((N & (N - 1)) == 0, "N must be a power of two");
    std::array<std::shared_mutex, N> shards;
public:
    std::shared_mutex& get(const K& key) {
        return shards[std::hash<K>{}(key) & (N - 1)];
    }

    void lockAllExclusive() {
        for (auto& m : shards) m.lock();
    }
    void unlockAllExclusive() {
        for (auto& m : shards) m.unlock();
    }
};