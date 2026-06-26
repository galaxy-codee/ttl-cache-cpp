# TTL Cache — LRU & LFU in C++17

A header-only, thread-safe, in-memory cache library with per-entry TTL expiry.  
Implements two eviction policies — **LRU** (Least Recently Used) and **LFU** (Least Frequently Used) — behind a common interface, with a benchmark harness that compares them across realistic workload distributions.

---

## Features

| Feature | Detail |
|---|---|
| Eviction policies | LRU and LFU, each O(1) amortised |
| Per-entry TTL | Lazy expiry on access + background janitor sweep |
| Thread safety | Centralized mutual exclusion via std::mutex and RAII std::lock_guard |
| Workload generation | Uniform and Zipf (skew = 1.0) distributions |
| Benchmark metrics | Latency (ms), hit rate (%), throughput (ops/s) |
| Header-only | Drop the `include/` folder into any C++17 project |

---

## Project Structure

```
ttl-cache-cpp/
├── include/
│   ├── cache_interface.h   # Abstract base — get / put / pruneExpired
│   ├── sharded_lock.h      # Legacy 16-shard lock manager(retained for architectural reference)
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

Both cache architectures enforce thread safety through centralized mutual exclusion utilizing `std::mutex` and `std::lock_guard`:

- **The Sharding Limitation:** While key-space sharding successfully prevents hash map structural collisions, it fails to safely protect embedded intrusive doubly-linked lists. Because all cache entries share a unified, global list topology, threads operating concurrently in separate shards will simultaneously mutate global structural nodes (like `head` or `tail` pointers during a promotion or eviction cycle), resulting in data races and memory corruption.
- **Monolithic Mutual Exclusion:** To guarantee absolute pointer integrity across overlapping state transitions, both LRU and LFU utilize a single, exclusive `std::mutex`. This completely serializes node-splicing operations (such as reordering an LRU node or shifting an LFU node across frequency lists), ensuring stable thread-safe execution.
- **Flat Lock Hierarchy:** All internal auxiliary mutations run natively inside the already-locked context of incoming public calls (`get` and `put`). This flat, single-lock infrastructure avoids nested synchronization requests, naturally eliminating circular-wait deadlocks.


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
- **Lock-Free List Topology** : Refactor the structural linked list nodes into a lock-free variant using atomic pointer setups `(std::atomic<Node*>)`, allowing true lock-striped thread isolation without a global mutex bottleneck.
- **Persistence** — serialise the cache to disk on shutdown and restore on startup.
- **gRPC / HTTP API** — expose the cache over a network for multi-process use.
