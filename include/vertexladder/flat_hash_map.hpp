#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vl {

// Open-addressing hash map from a non-zero 64-bit key to a 32-bit value.
//
// Why not std::unordered_map: it allocates a node per insert and every lookup chases
// a pointer into that node. Here the table is one flat array, so a lookup is a hash,
// a mask and (almost always) a single cache line.
//
// - Linear probing, power-of-two capacity, load factor kept at or below 1/2.
// - Key 0 marks an empty slot, so OrderId 0 is not a valid key.
// - Erase uses backward-shift deletion instead of tombstones: after removing a slot we
//   pull later entries of the same probe run back into the hole. Probe chains never
//   fill up with dead entries, so a long trading day doesn't slowly degrade lookups.
class FlatHashMap {
public:
    explicit FlatHashMap(std::size_t expected = 1024) { rehash(capacity_for(expected)); }

    std::size_t size() const noexcept { return size_; }
    std::size_t capacity() const noexcept { return slots_.size(); }
    bool empty() const noexcept { return size_ == 0; }

    // Returns nullptr if absent.
    const std::uint32_t* find(std::uint64_t key) const noexcept {
        assert(key != 0);
        for (std::size_t i = index_of(key);; i = (i + 1) & mask_) {
            const Slot& s = slots_[i];
            if (s.key == key) return &s.value;
            if (s.key == 0) return nullptr;
        }
    }

    bool contains(std::uint64_t key) const noexcept { return find(key) != nullptr; }

    // Inserts key -> value. Returns false (and changes nothing) if the key exists.
    bool insert(std::uint64_t key, std::uint32_t value) {
        assert(key != 0);
        if ((size_ + 1) * 2 > slots_.size()) rehash(slots_.size() * 2);
        for (std::size_t i = index_of(key);; i = (i + 1) & mask_) {
            Slot& s = slots_[i];
            if (s.key == key) return false;
            if (s.key == 0) {
                s.key = key;
                s.value = value;
                ++size_;
                return true;
            }
        }
    }

    // Returns false if the key was not present.
    bool erase(std::uint64_t key) noexcept {
        assert(key != 0);
        std::size_t hole = index_of(key);
        for (;; hole = (hole + 1) & mask_) {
            if (slots_[hole].key == key) break;
            if (slots_[hole].key == 0) return false;
        }
        // Backward shift: walk the rest of the run and move back any entry whose home
        // slot is at or before the hole (cyclically), so it stays reachable.
        for (std::size_t j = (hole + 1) & mask_;; j = (j + 1) & mask_) {
            Slot& s = slots_[j];
            if (s.key == 0) break;
            const std::size_t home = index_of(s.key);
            // Is `home` cyclically outside (hole, j]? Then s can move into the hole.
            const bool movable = ((j - home) & mask_) >= ((j - hole) & mask_);
            if (movable) {
                slots_[hole] = s;
                hole = j;
            }
        }
        slots_[hole].key = 0;
        --size_;
        return true;
    }

    void clear() noexcept {
        for (Slot& s : slots_) s.key = 0;
        size_ = 0;
    }

    void reserve(std::size_t expected) {
        const std::size_t want = capacity_for(expected);
        if (want > slots_.size()) rehash(want);
    }

private:
    struct Slot {
        std::uint64_t key = 0;
        std::uint32_t value = 0;
    };

    static std::size_t capacity_for(std::size_t expected) {
        std::size_t cap = 16;
        while (cap < expected * 2) cap <<= 1;
        return cap;
    }

    // Order ids are usually sequential, which would cluster badly under a plain mask.
    // Multiplying by 2^64/phi (Fibonacci hashing) spreads consecutive keys across the
    // table; we take the high bits, which are the well-mixed ones.
    std::size_t index_of(std::uint64_t key) const noexcept {
        return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ull) >> shift_);
    }

    void rehash(std::size_t new_cap) {
        std::vector<Slot> old;
        old.swap(slots_);
        slots_.assign(new_cap, Slot{});
        mask_ = new_cap - 1;
        shift_ = 64;
        for (std::size_t c = new_cap; c > 1; c >>= 1) --shift_;
        size_ = 0;
        for (const Slot& s : old) {
            if (s.key != 0) insert(s.key, s.value);
        }
    }

    std::vector<Slot> slots_;
    std::size_t mask_ = 0;
    unsigned shift_ = 64;
    std::size_t size_ = 0;
};

}  // namespace vl
