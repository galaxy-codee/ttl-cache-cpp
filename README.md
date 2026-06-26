# TTL Cache — LRU & LFU in C++17

A header-only, thread-safe, in-memory cache library with per-entry TTL expiry.  
Implements two eviction policies — **LRU** (Least Recently Used) and **LFU** (Least Frequently Used) — behind a common interface, with a benchmark harness that compares them across realistic workload distributions.

---

## Features

| Feature | Detail |
|---|---|
| Eviction policies | LRU and LFU, each O(1) amortised |
| Per-entry TTL | Lazy expiry on access + background janitor sweep |
| Thread safety | Sharded RW-locks (LRU) / single RW-lock (LFU) |
| Workload generation | Uniform and Zipf (skew = 1.0) distributions |
| Benchmark metrics | Latency (ms), hit rate (%), throughput (ops/s) |
| Header-only | Drop the `include/` folder into any C++17 project |

---

## Project Structure

```
ttl-cache-cpp/
├── include/
│   ├── cache_interface.h   # Abstract base — get / put / pruneExpired
│   ├── sharded_lock.h      # 16-shard reader-writer lock manager
│   ├── lru_cache.h         # TTL_LRUCache<K,V>
│   ├── lfu_cache.h         # TTL_LFUCache<K,V>
│   └── workload.h          # Uniform + Zipf trace generator
├── src/
│   └── main.cpp            # Benchmark harness
└── CMakeLists.txt
```

---

## Build

**Requirements:** C++17 compiler, CMake ≥ 3.16, pthreads (Linux/macOS) or MSVC (Windows).

```bash
# CMake (recommended)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/ttl_cache

# or directly with g++
g++ -O2 -std=c++17 -pthread -Iinclude src/main.cpp -o ttl_cache
./ttl_cache
```

---

## Sample Output

```
==========================================================================
 TTL Cache Benchmark  |  cap=500  keys=2500  ops=100000
==========================================================================
 Config                              Latency    Hit Rate      Throughput
--------------------------------------------------------------------------
 [SEQ /Uniform]  LRU                  3.4 ms      19.39 %    29185193 ops/s
 [SEQ /Uniform]  LFU                  3.7 ms      19.38 %    27082884 ops/s
--------------------------------------------------------------------------
 [SEQ /Zipf  ]   LRU                  5.3 ms      74.58 %    18752832 ops/s
 [SEQ /Zipf  ]   LFU                  7.1 ms      73.87 %    14022433 ops/s
--------------------------------------------------------------------------
 [CONC/Uniform]  LRU                 22.1 ms      19.55 %     4521977 ops/s
 [CONC/Uniform]  LFU                 18.0 ms      19.55 %     5551422 ops/s
--------------------------------------------------------------------------
 [CONC/Zipf  ]   LRU                 20.1 ms      74.66 %     4980749 ops/s
 [CONC/Zipf  ]   LFU                 37.5 ms      73.94 %     2664857 ops/s
==========================================================================
```

---

## Design

### Eviction Policies

**LRU** maintains a doubly-linked list in recency order plus a hash map for O(1) lookup. On every access the touched node moves to the head; eviction removes the tail.

**LFU** maintains a hash map of nodes and a second map from frequency → doubly-linked list. Every access promotes the node to the next frequency bucket in O(1). Eviction removes the tail of the lowest-frequency bucket.

Both engines use sentinel head/tail nodes to eliminate nullptr checks in list operations, and `std::atomic` counters for hit/miss stats to avoid lock contention on reads.

### Concurrency

Two different locking strategies are used, chosen to match each data structure's sharing pattern:

**LRU — 16-shard reader-writer locks**

The key space is divided into 16 shards (a power of two, so the modulo is a single bitwise AND). Each shard owns a `std::shared_mutex`. Operations on key `K` acquire only the mutex for shard `hash(K) & 15`. Two threads whose keys fall in different shards run in parallel, giving up to 16× throughput improvement over a single global lock on a read-heavy workload.

The intrusive linked list is safe because the shard lock covers both the hash map lookup and the list pointer update for the same key — no two threads can simultaneously touch the same node.

**LFU — single reader-writer lock**

LFU's `freqMap` (frequency → bucket) is global shared state that any write may touch regardless of the key involved. A node in bucket `f` can be moved to bucket `f+1` by a thread operating on a completely different key. A key-based shard boundary therefore cannot protect `freqMap` writes.

A single `std::shared_mutex` is the correct choice: it serialises writes (put, frequency promotion, eviction) while allowing concurrent reads. In practice caches are read-heavy (10:1 GET:PUT is typical), so the shared-read path still delivers meaningful concurrency.

**Global operations** (`pruneExpired`, `clear`) acquire all shard locks in fixed index order before mutating shared state, which prevents deadlock by eliminating circular-wait.

### TTL Expiry

Every entry stores an absolute expiry timestamp (`std::chrono::steady_clock::time_point`). Expiry is enforced in two ways:

- **Lazy:** checked on every `get()` — expired entries are evicted at access time without any background work.
- **Active:** a background janitor thread calls `pruneExpired()` at a configurable interval, scanning for and removing expired entries that were never accessed again.

### Workload Distributions

**Uniform** — every key is equally likely. Represents a random-access pattern with no locality; theoretical hit-rate ceiling is `capacity / uniqueKeys` regardless of policy.

**Zipf (skew = 1.0)** — `P(rank k) ∝ 1/k`. The most popular key receives roughly twice as many requests as the second, three times as many as the third, and so on. This is the standard model for real-world caches (web pages, database rows, DNS records). Under Zipf, LFU retains hot keys across eviction cycles; LRU may silently evict them during a sequential scan — which explains why LFU achieves a higher hit rate on Zipf traces in the benchmark output.

---

## Usage as a Library

All cache types are header-only and generic over key/value types:

```cpp
#include "lru_cache.h"
#include "lfu_cache.h"

// String → JSON blob cache, capacity 1000, entries expire after 5 minutes
TTL_LRUCache<std::string, std::string> lru(1000);
lru.put("user:42", R"({"name":"Alice"})", std::chrono::seconds(300));

auto val = lru.get("user:42");  // returns value or "" on miss/expiry

// Periodic cleanup (call from a background thread)
lru.pruneExpired();

// Statistics
double hitRate = lru.getHitRate();  // percentage in [0, 100]
```

---

## Possible Extensions

- **ARC / CLOCK-Pro** eviction policy — adaptive replacement without frequency tracking overhead.
- **Per-shard statistics** — break hit/miss counts down by shard to profile key distribution skew.
- **Persistence** — serialise the cache to disk on shutdown and restore on startup.
- **gRPC / HTTP API** — expose the cache over a network for multi-process use.
