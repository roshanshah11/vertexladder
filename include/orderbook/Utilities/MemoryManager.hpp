#pragma once
#include "ObjectPool.hpp"
#include "MemoryAllocators.hpp"
#include "../Core/Order.hpp"
#include <memory>
#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <iostream>
#include <execinfo.h>

namespace orderbook {

/**
 * @brief Centralized memory management for high-performance operations
 * Coordinates object pools, custom allocators, and memory optimization
 */
class MemoryManager {
public:
    /**
     * @brief Memory statistics for monitoring
     */
    struct MemoryStats {
        // Object pool statistics
        ObjectPool<Order>::Stats order_pool;
        ObjectPool<Trade>::Stats trade_pool;
        
        // Allocator statistics
        size_t stack_allocations = 0;
        size_t heap_allocations = 0;
        size_t aligned_allocations = 0;
        
        // Performance metrics
        std::chrono::nanoseconds avg_allocation_time{0};
        std::chrono::nanoseconds avg_deallocation_time{0};
        
        // Memory usage
        size_t total_memory_used = 0;
        size_t peak_memory_used = 0;
    };
    
    /**
     * @brief Get singleton instance
     */
    static MemoryManager& getInstance() {
        static MemoryManager instance;
        return instance;
    }
    
    /**
     * @brief Acquire an order from the pool
     * @return RAII wrapper for pooled order
     */
    ObjectPool<Order>::PooledObject acquireOrder() {
        auto start = std::chrono::high_resolution_clock::now();
        auto result = ObjectPools::getOrderPool().acquire();
        auto end = std::chrono::high_resolution_clock::now();
        
        updateAllocationTime(end - start);
        return result;
    }
    
    /**
     * @brief Acquire a trade from the pool
     * @return RAII wrapper for pooled trade
     */
    ObjectPool<Trade>::PooledObject acquireTrade() {
        auto start = std::chrono::high_resolution_clock::now();
        auto result = ObjectPools::getTradePool().acquire();
        auto end = std::chrono::high_resolution_clock::now();
        
        updateAllocationTime(end - start);
        return result;
    }
    
    /**
     * @brief Create stack allocator for temporary objects
     * @return Stack allocator instance
     */
    template<size_t Size = 64 * 1024>
    StackAllocator<Size> createStackAllocator() {
        return StackAllocator<Size>();
    }
    
    /**
     * @brief Create pool allocator for fixed-size objects
     * @return Pool allocator instance
     */
    template<typename T, size_t BlockSize = 1024>
    std::unique_ptr<PoolAllocator<T, BlockSize>> createPoolAllocator() {
        return std::make_unique<PoolAllocator<T, BlockSize>>();
    }
    
    /**
     * @brief Allocate SIMD-aligned memory
     * @param size Number of bytes to allocate
     * @param alignment Alignment requirement (default 32 bytes for AVX)
     * @return Aligned memory pointer
     */
    void* allocateAligned(size_t size, size_t alignment = 32) {
        auto start = std::chrono::high_resolution_clock::now();
        // Ensure allocation size is a multiple of alignment for aligned_alloc
        size_t alloc_size = size;
        if (alignment > 0 && (alloc_size % alignment) != 0) {
            alloc_size = ((alloc_size + alignment - 1) / alignment) * alignment;
        }

        void* ptr = std::aligned_alloc(alignment, alloc_size);
        auto end = std::chrono::high_resolution_clock::now();
        
        if (ptr) {
            aligned_allocations_.fetch_add(1);
            updateAllocationTime(end - start);
            updateMemoryUsage(alloc_size, true);
            // Track allocation for debug/validation
            std::lock_guard<std::mutex> lock(allocation_mutex_);
            // Store allocation size and capture allocation backtrace
            AllocationInfo info;
            info.size = alloc_size;
            void* bt_arr[32];
            int bt_sz = backtrace(bt_arr, 32);
            char** bt_syms = backtrace_symbols(bt_arr, bt_sz);
            if (bt_syms) {
                for (int i = 0; i < bt_sz; ++i) {
                    info.bt.emplace_back(bt_syms[i]);
                }
                free(bt_syms);
            }
            allocations_[ptr] = std::move(info);
            if (std::getenv("MEM_DEBUG") ) {
                std::cerr << "MemoryManager: allocated ptr=" << ptr << " size=" << alloc_size << std::endl;
            }
        }
        
        return ptr;
    }
    
    /**
     * @brief Deallocate SIMD-aligned memory
     * @param ptr Pointer to deallocate
     * @param size Size of allocation (for statistics)
     */
    void deallocateAligned(void* ptr, size_t /* size */) {
        if (!ptr) return;

        auto start = std::chrono::high_resolution_clock::now();
        // Verify we allocated this pointer
        size_t alloc_size = 0;
        std::vector<std::string> alloc_bt;
        {
            std::lock_guard<std::mutex> lock(allocation_mutex_);
            auto it = allocations_.find(ptr);
            if (it == allocations_.end()) {
                std::cerr << "MemoryManager: attempt to free untracked or already freed pointer: " << ptr << std::endl;
                // Print backtrace to help find where the invalid free occurs
                void* bt[32];
                int bt_size = backtrace(bt, 32);
                char** bt_symbols = backtrace_symbols(bt, bt_size);
                if (bt_symbols) {
                    std::cerr << "Backtrace:" << std::endl;
                    for (int i = 0; i < bt_size; ++i) {
                        std::cerr << "  " << bt_symbols[i] << std::endl;
                    }
                    free(bt_symbols);
                }
                std::abort();
            }
            alloc_size = it->second.size;
            alloc_bt = it->second.bt; // copy backtrace for post-unlock logging
            allocations_.erase(it);
        }

        std::free(ptr);
        if (std::getenv("MEM_DEBUG") ) {
            std::cerr << "MemoryManager: deallocated ptr=" << ptr << " size=" << alloc_size << std::endl;
            if (!alloc_bt.empty()) {
                std::cerr << "  Allocated at (most recent frames):\n";
                for (size_t k = 0; k < alloc_bt.size() && k < 5; ++k) {
                    std::cerr << "    " << alloc_bt[k] << std::endl;
                }
            }
        }
        auto end = std::chrono::high_resolution_clock::now();
        
        updateDeallocationTime(end - start);
        updateMemoryUsage(alloc_size, false);
    }
    
    /**
     * @brief Pre-warm object pools
     * Allocates objects in advance to reduce allocation overhead during trading
     */
    void prewarmPools() {
        // Pre-allocate orders
        std::vector<ObjectPool<Order>::PooledObject> orders;
        orders.reserve(1000);
        for (int i = 0; i < 1000; ++i) {
            orders.push_back(acquireOrder());
        }
        orders.clear(); // Return all to pool
        
        // Pre-allocate trades
        std::vector<ObjectPool<Trade>::PooledObject> trades;
        trades.reserve(500);
        for (int i = 0; i < 500; ++i) {
            trades.push_back(acquireTrade());
        }
        trades.clear(); // Return all to pool
    }
    
    /**
     * @brief Get comprehensive memory statistics
     * @return Current memory statistics
     */
    MemoryStats getStats() const {
        MemoryStats stats;
        
        // Object pool stats
        auto pool_stats = ObjectPools::getAllStats();
        stats.order_pool = pool_stats.order_pool;
        stats.trade_pool = pool_stats.trade_pool;
        
        // Allocation counts
        stats.aligned_allocations = aligned_allocations_.load();
        
        // Performance metrics
        auto allocation_count = allocation_count_.load();
        if (allocation_count > 0) {
            stats.avg_allocation_time = std::chrono::nanoseconds(
                total_allocation_time_.load() / allocation_count);
        }
        
        auto deallocation_count = deallocation_count_.load();
        if (deallocation_count > 0) {
            stats.avg_deallocation_time = std::chrono::nanoseconds(
                total_deallocation_time_.load() / deallocation_count);
        }
        
        // Memory usage
        stats.total_memory_used = current_memory_used_.load();
        stats.peak_memory_used = peak_memory_used_.load();
        
        return stats;
    }
    
    /**
     * @brief Reset statistics
     */
    void resetStats() {
        aligned_allocations_.store(0);
        allocation_count_.store(0);
        deallocation_count_.store(0);
        total_allocation_time_.store(0);
        total_deallocation_time_.store(0);
        current_memory_used_.store(0);
        peak_memory_used_.store(0);
    }
    
    /**
     * @brief Optimize memory layout for better cache performance
     * This is a hint to the system to optimize memory layout
     */
    void optimizeMemoryLayout() {
        // Pre-warm pools to ensure objects are allocated in contiguous memory
        prewarmPools();
        
        // Hint to OS about memory usage patterns
        #ifdef __linux__
        // Use madvise to hint about memory access patterns
        // This is platform-specific optimization
        #endif
    }
    
private:
    MemoryManager() = default;
    
    // Statistics tracking
    std::atomic<size_t> aligned_allocations_{0};
    std::atomic<size_t> allocation_count_{0};
    std::atomic<size_t> deallocation_count_{0};
    std::atomic<uint64_t> total_allocation_time_{0};
    std::atomic<uint64_t> total_deallocation_time_{0};
    std::atomic<size_t> current_memory_used_{0};
    std::atomic<size_t> peak_memory_used_{0};
    
    void updateAllocationTime(std::chrono::nanoseconds duration) {
        allocation_count_.fetch_add(1);
        total_allocation_time_.fetch_add(duration.count());
    }
    
    void updateDeallocationTime(std::chrono::nanoseconds duration) {
        deallocation_count_.fetch_add(1);
        total_deallocation_time_.fetch_add(duration.count());
    }
    
    void updateMemoryUsage(size_t size, bool allocating) {
        if (allocating) {
            auto new_usage = current_memory_used_.fetch_add(size) + size;
            // Update peak if necessary
            auto current_peak = peak_memory_used_.load();
            while (new_usage > current_peak && 
                   !peak_memory_used_.compare_exchange_weak(current_peak, new_usage)) {
                // Retry if another thread updated peak
            }
        } else {
            current_memory_used_.fetch_sub(size);
        }
    }

    // Allocation tracking for debug/validation
    mutable std::mutex allocation_mutex_;
    struct AllocationInfo { size_t size; std::vector<std::string> bt; };
    std::unordered_map<void*, AllocationInfo> allocations_;
};

/**
 * @brief RAII wrapper for SIMD-aligned memory
 */
template<typename T>
class AlignedMemory {
public:
    explicit AlignedMemory(size_t count, size_t alignment = 32) 
        : size_(count * sizeof(T)), alignment_(alignment) {
        ptr_ = static_cast<T*>(MemoryManager::getInstance().allocateAligned(size_, alignment_));
        if (!ptr_) {
            throw std::bad_alloc();
        }
    }
    
    ~AlignedMemory() {
        if (ptr_) {
            MemoryManager::getInstance().deallocateAligned(ptr_, size_);
        }
    }
    
    // Move semantics only
    AlignedMemory(AlignedMemory&& other) noexcept 
        : ptr_(other.ptr_), size_(other.size_), alignment_(other.alignment_) {
        other.ptr_ = nullptr;
        other.size_ = 0;
    }
    
    AlignedMemory& operator=(AlignedMemory&& other) noexcept {
        if (this != &other) {
            if (ptr_) {
                MemoryManager::getInstance().deallocateAligned(ptr_, size_);
            }
            ptr_ = other.ptr_;
            size_ = other.size_;
            alignment_ = other.alignment_;
            other.ptr_ = nullptr;
            other.size_ = 0;
        }
        return *this;
    }
    
    // Delete copy operations
    AlignedMemory(const AlignedMemory&) = delete;
    AlignedMemory& operator=(const AlignedMemory&) = delete;
    
    T* get() const { return ptr_; }
    T& operator[](size_t index) const { return ptr_[index]; }
    
    size_t size() const { return size_ / sizeof(T); }
    size_t bytes() const { return size_; }
    
private:
    T* ptr_ = nullptr;
    size_t size_ = 0;
    size_t alignment_ = 0;
};

} // namespace orderbook