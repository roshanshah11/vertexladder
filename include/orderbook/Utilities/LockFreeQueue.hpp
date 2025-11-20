#pragma once
#include <atomic>
#include <vector>
#include <optional>

namespace orderbook {

/**
 * @brief Lock-free Single-Producer Single-Consumer (SPSC) queue
 * Optimized for high-throughput message passing between threads
 */
template<typename T, size_t Capacity = 1024>
class LockFreeQueue {
public:
    LockFreeQueue() : head_(0), tail_(0) {
        buffer_.resize(Capacity);
    }

    /**
     * @brief Push item to queue (Producer only)
     * @return true if successful, false if full
     */
    bool push(const T& item) {
        const size_t current_tail = tail_.load(std::memory_order_relaxed);
        const size_t next_tail = (current_tail + 1) % Capacity;

        if (next_tail == head_.load(std::memory_order_acquire)) {
            return false; // Full
        }

        buffer_[current_tail] = item;
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }

    /**
     * @brief Pop item from queue (Consumer only)
     * @return true if item retrieved, false if empty
     */
    bool pop(T& item) {
        const size_t current_head = head_.load(std::memory_order_relaxed);

        if (current_head == tail_.load(std::memory_order_acquire)) {
            return false; // Empty
        }

        item = buffer_[current_head];
        head_.store((current_head + 1) % Capacity, std::memory_order_release);
        return true;
    }

    /**
     * @brief Check if queue is empty
     */
    bool empty() const {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

private:
    std::vector<T> buffer_;
    alignas(64) std::atomic<size_t> head_;
    alignas(64) std::atomic<size_t> tail_;
};

}
