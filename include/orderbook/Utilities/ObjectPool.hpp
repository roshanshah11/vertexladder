#pragma once
#include <vector>
#include <memory>
#include <cassert>
#include <mutex>
#include <atomic>
#include <type_traits>
#include "../Core/Order.hpp"

namespace orderbook {

/**
 * @brief Thread-safe object pool for high-performance object reuse
 * Reduces memory allocation overhead by reusing objects
 */
template<typename T>
class ObjectPool {
public:
    template<typename U>
    class has_reset {
        template<typename V, typename = decltype(std::declval<V&>().reset())>
        static std::true_type test(int);
        template<typename>
        static std::false_type test(...);
    public:
        static constexpr bool value = decltype(test<U>(0))::value;
    };
    static_assert(std::is_default_constructible_v<T>, "T must be default constructible");
    
    /**
     * @brief RAII wrapper for pooled objects
     * Automatically returns object to pool when destroyed
     */
    class PooledObject {
    public:
        PooledObject(T* obj, ObjectPool<T>* pool) : object_(obj), pool_(pool) {}
        
        ~PooledObject() {
            if (object_ && pool_) {
                pool_->returnObject(object_);
            }
        }
        
        // Move semantics only
        PooledObject(PooledObject&& other) noexcept 
            : object_(other.object_), pool_(other.pool_) {
            other.object_ = nullptr;
            other.pool_ = nullptr;
        }
        
        PooledObject& operator=(PooledObject&& other) noexcept {
            if (this != &other) {
                if (object_ && pool_) {
                    pool_->returnObject(object_);
                }
                object_ = other.object_;
                pool_ = other.pool_;
                other.object_ = nullptr;
                other.pool_ = nullptr;
            }
            return *this;
        }
        
        // Delete copy operations
        PooledObject(const PooledObject&) = delete;
        PooledObject& operator=(const PooledObject&) = delete;
        
        T* get() const { return object_; }
        T& operator*() const { return *object_; }
        T* operator->() const { return object_; }
        
        explicit operator bool() const { return object_ != nullptr; }
        
    private:
        T* object_;
        ObjectPool<T>* pool_;
    };
    
    /**
     * @brief Constructor with initial pool size
     * @param initial_size Number of objects to pre-allocate
     */
    explicit ObjectPool(size_t initial_size = 1000) {
        pool_.reserve(initial_size);
        for (size_t i = 0; i < initial_size; ++i) {
            pool_.emplace_back(std::make_unique<T>());
        }
        available_count_.store(initial_size);
    }
    
    /**
     * @brief Get an object from the pool
     * @return RAII wrapper containing the object
     */
    PooledObject acquire() {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (pool_.empty()) {
            // Pool is empty, create new object
            auto obj = std::make_unique<T>();
            T* raw_ptr = obj.release();
            ++total_created_;
            return PooledObject(raw_ptr, this);
        }
        
        // Get object from pool
        auto obj = std::move(pool_.back());
        pool_.pop_back();
        available_count_.fetch_sub(1);
        
        T* raw_ptr = obj.release();
        
        // Reset object if it has a reset method (C++17-compatible detection)
        if constexpr (has_reset<T>::value) {
            raw_ptr->reset();
        }
        
        return PooledObject(raw_ptr, this);
    }

    /**
     * @brief Allocate an object from the pool and return raw pointer.
     * Caller is responsible for returning it using release().
     */
    T* allocate() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pool_.empty()) {
            auto obj = std::make_unique<T>();
            T* raw_ptr = obj.release();
            ++total_created_;
            if constexpr (has_reset<T>::value) raw_ptr->reset();
            return raw_ptr;
        }
        auto obj = std::move(pool_.back());
        pool_.pop_back();
        available_count_.fetch_sub(1);
        T* raw_ptr = obj.release();
        if constexpr (has_reset<T>::value) raw_ptr->reset();
        return raw_ptr;
    }

    /**
     * @brief Release a previously allocated object back to the pool.
     * @param obj Pointer returned from allocate()
     */
    void release(T* obj) {
        // In debug builds, detect double-release which can indicate use-after-free
#ifndef NDEBUG
        for (const auto& p : pool_) {
            if (p.get() == obj) {
                // Double release detected - assert to help debugging
                assert(false && "Double release to ObjectPool detected");
            }
        }
#endif
        returnObject(obj);
    }
    
    /**
     * @brief Get pool statistics
     */
    struct Stats {
        size_t available;
        size_t total_created;
        size_t pool_size;
    };
    
    Stats getStats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return {
            available_count_.load(),
            total_created_,
            pool_.size()
        };
    }
    
private:
    /**
     * @brief Return object to pool (called by PooledObject destructor)
     * @param obj Object to return
     */
    void returnObject(T* obj) {
        if (!obj) return;

        std::lock_guard<std::mutex> lock(mutex_);
        pool_.emplace_back(obj);
        available_count_.fetch_add(1);
    }

    
    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<T>> pool_;
    std::atomic<size_t> available_count_{0};
    size_t total_created_{0};
};

/**
 * @brief Global object pools for common types
 */
class ObjectPools {
public:
    static ObjectPool<Order>& getOrderPool() {
        static ObjectPool<Order> pool(2000); // Pre-allocate 2000 orders
        return pool;
    }
    
    static ObjectPool<Trade>& getTradePool() {
        static ObjectPool<Trade> pool(1000); // Pre-allocate 1000 trades
        return pool;
    }
    
    /**
     * @brief Get statistics for all pools
     */
    struct AllStats {
        ObjectPool<Order>::Stats order_pool;
        ObjectPool<Trade>::Stats trade_pool;
    };
    
    static AllStats getAllStats() {
        return {
            getOrderPool().getStats(),
            getTradePool().getStats()
        };
    }
};

} // namespace orderbook