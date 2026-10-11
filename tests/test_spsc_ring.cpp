#include "vertexladder/spsc_ring.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <thread>

using vl::SpscRing;

TEST(SpscRing, RoundsCapacityUpToPowerOfTwo) {
    SpscRing<int> r(5);
    EXPECT_EQ(r.capacity(), 8u);
}

TEST(SpscRing, FifoAndFullEmpty) {
    SpscRing<int> r(4);
    int v = 0;
    EXPECT_FALSE(r.try_pop(v));
    for (int i = 0; i < 4; ++i) EXPECT_TRUE(r.try_push(i));
    EXPECT_FALSE(r.try_push(99));  // full: caller has to handle it, nothing is dropped
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(r.try_pop(v));
        EXPECT_EQ(v, i);
    }
    EXPECT_FALSE(r.try_pop(v));
}

TEST(SpscRing, WrapsAroundManyTimes) {
    SpscRing<std::uint64_t> r(8);
    std::uint64_t next_in = 0, next_out = 0, v = 0;
    for (int round = 0; round < 10'000; ++round) {
        for (int k = 0; k < 5; ++k) ASSERT_TRUE(r.try_push(next_in++));
        for (int k = 0; k < 5; ++k) {
            ASSERT_TRUE(r.try_pop(v));
            ASSERT_EQ(v, next_out++);
        }
    }
}

// Two real threads and a small ring, so it is constantly full or empty. Every message
// must arrive exactly once, in order, with both fields intact. CI also runs this under
// ThreadSanitizer. Neither is proof of the memory ordering: weakening the release/acquire
// pair to relaxed still passed both on an M3. The argument for correctness is the
// happens-before reasoning in spsc_ring.hpp; this test catches logic errors.
TEST(SpscRing, TwoThreadStress) {
    struct Msg {
        std::uint64_t seq;
        std::uint64_t check;
    };
    constexpr std::uint64_t kCount = 2'000'000;
    SpscRing<Msg> r(64);
    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kCount; ++i) {
            const Msg m{i, i * 0x9E3779B97F4A7C15ull};
            while (!r.try_push(m)) std::this_thread::yield();
        }
    });
    std::uint64_t expect = 0;
    Msg m{};
    while (expect < kCount) {
        if (!r.try_pop(m)) continue;
        ASSERT_EQ(m.seq, expect);
        ASSERT_EQ(m.check, expect * 0x9E3779B97F4A7C15ull);
        ++expect;
    }
    producer.join();
    EXPECT_FALSE(r.try_pop(m));
}
