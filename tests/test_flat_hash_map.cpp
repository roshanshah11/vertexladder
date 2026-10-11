#include "vertexladder/flat_hash_map.hpp"

#include <gtest/gtest.h>

#include <random>
#include <unordered_map>

using vl::FlatHashMap;

TEST(FlatHashMap, InsertFindErase) {
    FlatHashMap m(4);
    EXPECT_TRUE(m.insert(1, 10));
    EXPECT_TRUE(m.insert(2, 20));
    EXPECT_FALSE(m.insert(1, 99));
    EXPECT_EQ(*m.find(1), 10u);
    EXPECT_EQ(*m.find(2), 20u);
    EXPECT_EQ(m.find(3), nullptr);
    EXPECT_TRUE(m.erase(1));
    EXPECT_FALSE(m.erase(1));
    EXPECT_EQ(m.find(1), nullptr);
    EXPECT_EQ(m.size(), 1u);
}

TEST(FlatHashMap, GrowsAndKeepsLoadUnderHalf) {
    FlatHashMap m(1);
    for (std::uint64_t k = 1; k <= 100'000; ++k) ASSERT_TRUE(m.insert(k, static_cast<std::uint32_t>(k)));
    EXPECT_EQ(m.size(), 100'000u);
    EXPECT_LE(m.size() * 2, m.capacity());
    for (std::uint64_t k = 1; k <= 100'000; ++k) ASSERT_EQ(*m.find(k), k);
}

// Random inserts and erases against std::unordered_map. Erase is the tricky part
// (backward-shift deletion has to keep every probe run intact), so the key space is
// kept small to force long collision runs and wraparound at the end of the table.
TEST(FlatHashMap, MatchesStdUnorderedMap) {
    for (std::uint64_t seed = 1; seed <= 5; ++seed) {
        FlatHashMap m(16);
        std::unordered_map<std::uint64_t, std::uint32_t> ref;
        std::mt19937_64 rng(seed);
        for (int i = 0; i < 200'000; ++i) {
            const std::uint64_t k = 1 + rng() % 2000;
            if (rng() % 3 == 0) {
                ASSERT_EQ(m.erase(k), ref.erase(k) == 1);
            } else {
                const auto v = static_cast<std::uint32_t>(rng());
                ASSERT_EQ(m.insert(k, v), ref.emplace(k, v).second);
            }
            if (i % 1000 == 0) {
                ASSERT_EQ(m.size(), ref.size());
                for (std::uint64_t q = 1; q <= 2000; ++q) {
                    auto it = ref.find(q);
                    const std::uint32_t* got = m.find(q);
                    if (it == ref.end()) ASSERT_EQ(got, nullptr);
                    else ASSERT_TRUE(got && *got == it->second);
                }
            }
        }
    }
}
