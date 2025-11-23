#pragma once
#include <vector>
#include <mutex>
#include <type_traits>

namespace orderbook {

template<typename T>
class ContiguousObjectPool {
public:
    static_assert(std::is_default_constructible_v<T>, "T must be default constructible");

    explicit ContiguousObjectPool(size_t initial_size = 0) {
        if (initial_size > 0) {
            storage_.resize(initial_size);
            free_list_.reserve(initial_size);
            for (size_t i = 0; i < initial_size; ++i) free_list_.push_back(&storage_[i]);
        }
    }

    T* allocate() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!free_list_.empty()) {
            T* ptr = free_list_.back();
            free_list_.pop_back();
            if constexpr (has_reset<T>::value) ptr->reset();
            return ptr;
        }
        // fallback to heap allocation when pool exhausted
        T* ptr = new T();
        if constexpr (has_reset<T>::value) ptr->reset();
        heap_allocated_.push_back(ptr);
        return ptr;
    }

    void release(T* obj) {
        if (!obj) return;
        std::lock_guard<std::mutex> lock(mutex_);
        free_list_.push_back(obj);
    }

    ~ContiguousObjectPool() {
        for (T* p : heap_allocated_) delete p;
    }

private:
    // Helper to detect reset method
    template<typename U>
    class has_reset {
        template<typename V, typename = decltype(std::declval<V&>().reset())>
        static std::true_type test(int);
        template<typename> static std::false_type test(...);
    public:
        static constexpr bool value = decltype(test<U>(0))::value;
    };

    std::vector<T> storage_;
    std::vector<T*> free_list_;
    std::vector<T*> heap_allocated_;
    std::mutex mutex_;
};

} // namespace orderbook
