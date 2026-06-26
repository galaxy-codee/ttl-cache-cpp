#pragma once
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <chrono>
#include "cache_interface.h"

// ============================================================================
// TTL_LRUCache<K, V>
//
// Thread-safe LRU cache with per-entry time-to-live expiry.
//
// Data structure: doubly-linked list (recency order) + hash map (O(1) lookup)
// Concurrency:    single std::mutex — correct because the linked list is shared
//                 global state; any operation may touch head/tail pointers.
// TTL:            lazy expiry on get() + background pruneExpired() sweep.
// ============================================================================
template <typename K, typename V>
class TTL_LRUCache : public CacheInterface<K, V> {
private:
    struct Node {
        K key; V val;
        std::chrono::steady_clock::time_point expiryTime;
        Node* prev = nullptr;
        Node* next = nullptr;
        Node(K k, V v, std::chrono::steady_clock::time_point exp)
            : key(std::move(k)), val(std::move(v)), expiryTime(exp) {}
    };

    Node* head;
    Node* tail;
    std::unordered_map<K, Node*> cacheMap;
    std::mutex mtx;
    std::atomic<long long> hits{0};
    std::atomic<long long> misses{0};

    void addAtHead(Node* node) {
        node->next = head->next; node->prev = head;
        head->next->prev = node; head->next = node;
    }
    void removeNode(Node* node) {
        node->prev->next = node->next;
        node->next->prev = node->prev;
    }
    void moveToHead(Node* node) { removeNode(node); addAtHead(node); }
    bool isExpired(const Node* node) const {
        return std::chrono::steady_clock::now() > node->expiryTime;
    }

public:
    explicit TTL_LRUCache(int cap) : CacheInterface<K, V>(cap) {
        auto forever = std::chrono::steady_clock::time_point::max();
        head = new Node(K{}, V{}, forever);
        tail = new Node(K{}, V{}, forever);
        head->next = tail; tail->prev = head;
    }

    ~TTL_LRUCache() override {
        Node* curr = head->next;
        while (curr != tail) { Node* nx = curr->next; delete curr; curr = nx; }
        delete head; delete tail;
    }

    V get(K key) override {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = cacheMap.find(key);
        if (it == cacheMap.end()) { ++misses; return V{}; }
        Node* node = it->second;
        if (isExpired(node)) {
            cacheMap.erase(it); removeNode(node); delete node;
            ++misses; return V{};
        }
        ++hits; moveToHead(node); return node->val;
    }

    void put(K key, V value, std::chrono::seconds ttl) override {
        std::lock_guard<std::mutex> lock(mtx);
        if (this->capacity == 0) return;
        auto expiry = std::chrono::steady_clock::now() + ttl;
        auto it = cacheMap.find(key);
        if (it != cacheMap.end()) {
            Node* node = it->second;
            node->val = std::move(value); node->expiryTime = expiry;
            moveToHead(node); return;
        }
        if (static_cast<int>(cacheMap.size()) >= this->capacity) {
            Node* lru = tail->prev;
            cacheMap.erase(lru->key); removeNode(lru); delete lru;
        }
        Node* newNode = new Node(key, std::move(value), expiry);
        addAtHead(newNode); cacheMap[key] = newNode;
    }

    void pruneExpired() override {
        std::lock_guard<std::mutex> lock(mtx);
        auto now = std::chrono::steady_clock::now();
        Node* curr = head->next;
        while (curr != tail) {
            Node* nx = curr->next;
            if (now > curr->expiryTime) {
                cacheMap.erase(curr->key); removeNode(curr); delete curr;
            }
            curr = nx;
        }
    }

    double getHitRate() override {
        auto h = hits.load(), m = misses.load();
        return (h + m == 0) ? 0.0 : static_cast<double>(h) / (h + m) * 100.0;
    }

    void clear() override {
        std::lock_guard<std::mutex> lock(mtx);
        Node* curr = head->next;
        while (curr != tail) { Node* nx = curr->next; delete curr; curr = nx; }
        head->next = tail; tail->prev = head;
        cacheMap.clear(); hits = 0; misses = 0;
    }
};