#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>

namespace vl {

// Apple Silicon uses 128-byte cache lines, x86 uses 64 (but its adjacent-line
// prefetcher pulls pairs, so 128 is the safe padding on both).
inline constexpr std::size_t kCacheLine = 128;

// Bounded lock-free single-producer / single-consumer ring buffer.
//
// One thread may call try_push, one other thread may call try_pop. That restriction
// is what makes it cheap: each index has exactly one writer, so there are no CAS
// loops, only one acquire load and one release store per operation in the common case.
//
// - head_ (next slot to read) is written only by the consumer, tail_ (next slot to
//   write) only by the producer. They live on separate cache lines so the two threads
//   don't fight over one line (false sharing).
// - Each side keeps a private copy of the other side's index and only re-reads the
//   shared atomic when the copy says "full" or "empty". In steady state the producer
//   doesn't touch the consumer's line at all, and vice versa.
// - The release store on tail_ publishes the slot contents; the consumer's acquire
//   load of tail_ makes them visible before it reads the slot. Same in reverse for
//   head_, so the producer never overwrites a slot that is still being read.
// - Indices are 64-bit and only ever increase; slot = index & mask. They would take
//   centuries to wrap.
template <class T>
class SpscRing {
    static_assert(std::is_trivially_copyable_v<T>, "ring holds plain messages");

public:
    // capacity is rounded up to a power of two.
    explicit SpscRing(std::size_t capacity) {
        std::size_t cap = 2;
        while (cap < capacity) cap <<= 1;
        mask_ = cap - 1;
        slots_ = std::make_unique<T[]>(cap);
    }

    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    std::size_t capacity() const noexcept { return mask_ + 1; }

    // Producer only. Returns false if full; the caller decides whether to spin,
    // drop or apply backpressure.
    bool try_push(const T& v) noexcept {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        if (t - head_cache_ > mask_) {
            head_cache_ = head_.load(std::memory_order_acquire);
            if (t - head_cache_ > mask_) return false;
        }
        slots_[t & mask_] = v;
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    // Consumer only. Returns false if empty.
    bool try_pop(T& out) noexcept {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        if (h == tail_cache_) {
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (h == tail_cache_) return false;
        }
        out = slots_[h & mask_];
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    // Approximate; exact only when both threads are quiet.
    std::size_t size_approx() const noexcept {
        return tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire);
    }

private:
    // Consumer-owned line.
    alignas(kCacheLine) std::atomic<std::size_t> head_{0};
    std::size_t tail_cache_ = 0;
    // Producer-owned line.
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
    std::size_t head_cache_ = 0;
    // Read-mostly, shared.
    alignas(kCacheLine) std::size_t mask_ = 0;
    std::unique_ptr<T[]> slots_;
};

}  // namespace vl
