#pragma once
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <chrono>
#include "cache_interface.h"

// ============================================================================
// TTL_LFUCache<K, V>
//
// Thread-safe LFU cache with per-entry time-to-live expiry.
//
// Data structure: nodeMap (key->node) + freqMap (freq->bucket list)
// Concurrency:    single std::mutex — freqMap is global shared state that
//                 any operation may touch regardless of key.
// TTL:            lazy expiry on get() + background pruneExpired() sweep.
// ============================================================================
template <typename K, typename V>
class TTL_LFUCache : public CacheInterface<K, V> {
private:
    struct Node {
        K key; V val; int frequency = 1;
        std::chrono::steady_clock::time_point expiryTime;
        Node* prev = nullptr;
        Node* next = nullptr;
        Node(K k, V v, std::chrono::steady_clock::time_point exp)
            : key(std::move(k)), val(std::move(v)), expiryTime(exp) {}
    };

    struct FrequencyList {
        Node* head; Node* tail; int size = 0;
        FrequencyList() {
            auto forever = std::chrono::steady_clock::time_point::max();
            head = new Node(K{}, V{}, forever);
            tail = new Node(K{}, V{}, forever);
            head->next = tail; tail->prev = head;
        }
        ~FrequencyList() { delete head; delete tail; }
        void addAtHead(Node* node) {
            node->next = head->next; node->prev = head;
            head->next->prev = node; head->next = node; ++size;
        }
        void removeNode(Node* node) {
            node->prev->next = node->next;
            node->next->prev = node->prev; --size;
        }
        bool isEmpty() const { return size == 0; }
    };

    int minFrequency = 0;
    std::unordered_map<K, Node*> nodeMap;
    std::unordered_map<int, FrequencyList*> freqMap;
    std::mutex mtx;
    std::atomic<long long> hits{0};
    std::atomic<long long> misses{0};

    bool isExpired(const Node* node) const {
        return std::chrono::steady_clock::now() > node->expiryTime;
    }
    void updateFrequency(Node* node) {
        int old = node->frequency;
        freqMap[old]->removeNode(node);
        if (old == minFrequency && freqMap[old]->isEmpty()) ++minFrequency;
        ++node->frequency;
        int nw = node->frequency;
        if (!freqMap.count(nw)) freqMap[nw] = new FrequencyList();
        freqMap[nw]->addAtHead(node);
    }

public:
    explicit TTL_LFUCache(int cap) : CacheInterface<K, V>(cap) {}
    ~TTL_LFUCache() override { clear(); }

    V get(K key) override {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = nodeMap.find(key);
        if (it == nodeMap.end() || this->capacity == 0) { ++misses; return V{}; }
        Node* node = it->second;
        if (isExpired(node)) {
            freqMap[node->frequency]->removeNode(node);
            delete node; nodeMap.erase(it);
            ++misses; return V{};
        }
        ++hits; updateFrequency(node); return node->val;
    }

    void put(K key, V value, std::chrono::seconds ttl) override {
        std::lock_guard<std::mutex> lock(mtx);
        if (this->capacity == 0) return;
        auto expiry = std::chrono::steady_clock::now() + ttl;
        auto it = nodeMap.find(key);
        if (it != nodeMap.end()) {
            Node* node = it->second;
            node->val = std::move(value); node->expiryTime = expiry;
            updateFrequency(node); return;
        }
        if (static_cast<int>(nodeMap.size()) >= this->capacity) {
            FrequencyList* minList = freqMap[minFrequency];
            Node* victim = minList->tail->prev;
            nodeMap.erase(victim->key); minList->removeNode(victim); delete victim;
        }
        Node* node = new Node(key, std::move(value), expiry);
        nodeMap[key] = node; minFrequency = 1;
        if (!freqMap.count(1)) freqMap[1] = new FrequencyList();
        freqMap[1]->addAtHead(node);
    }

    void pruneExpired() override {
        std::lock_guard<std::mutex> lock(mtx);
        auto now = std::chrono::steady_clock::now();
        for (auto it = nodeMap.begin(); it != nodeMap.end(); ) {
            Node* node = it->second;
            if (now > node->expiryTime) {
                freqMap[node->frequency]->removeNode(node);
                delete node; it = nodeMap.erase(it);
            } else { ++it; }
        }
        while (minFrequency > 0 &&
               (!freqMap.count(minFrequency) || freqMap[minFrequency]->isEmpty())) {
            if (nodeMap.empty()) { minFrequency = 0; break; }
            ++minFrequency;
        }
    }

    double getHitRate() override {
        auto h = hits.load(), m = misses.load();
        return (h + m == 0) ? 0.0 : static_cast<double>(h) / (h + m) * 100.0;
    }

    void clear() override {
        std::lock_guard<std::mutex> lock(mtx);
        for (auto& [k, node] : nodeMap) delete node;
        for (auto& [f, list] : freqMap) delete list;
        nodeMap.clear(); freqMap.clear();
        hits = 0; misses = 0; minFrequency = 0;
    }
};