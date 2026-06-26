#pragma once
#include <chrono>

// ============================================================================
// CacheInterface
//
// Abstract base for all cache policies. Parameterised on key type K and
// value type V. Every concrete engine (LRU, LFU, …) must satisfy this
// contract so the benchmark harness can drive them polymorphically.
// ============================================================================
template <typename K, typename V>
class CacheInterface {
protected:
    int capacity;
public:
    explicit CacheInterface(int cap) : capacity(cap) {}
    virtual ~CacheInterface() = default;

    // Return the value for key, or a default-constructed V on miss/expiry.
    virtual V get(K key) = 0;

    // Insert or update key with the given value and TTL.
    virtual void put(K key, V value, std::chrono::seconds ttl) = 0;

    // Scan and delete every entry whose TTL has elapsed.
    // Called periodically by the background janitor thread.
    virtual void pruneExpired() = 0;

    // Hit rate as a percentage in [0, 100].
    virtual double getHitRate() = 0;

    // Evict all entries and reset statistics.
    virtual void clear() = 0;
};