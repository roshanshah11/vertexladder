#include "vertexladder/itch.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

using namespace vl;

namespace {

// Builds ITCH 5.0 messages in BinaryFILE framing (2-byte big-endian length prefix).
struct ItchWriter {
    std::vector<std::uint8_t> out;
    std::vector<std::uint8_t> m;
    void u8(std::uint8_t v) { m.push_back(v); }
    void u16(std::uint16_t v) { u8(static_cast<std::uint8_t>(v >> 8)); u8(static_cast<std::uint8_t>(v)); }
    void u32(std::uint32_t v) { u16(static_cast<std::uint16_t>(v >> 16)); u16(static_cast<std::uint16_t>(v)); }
    void u48(std::uint64_t v) { u16(static_cast<std::uint16_t>(v >> 32)); u32(static_cast<std::uint32_t>(v)); }
    void u64(std::uint64_t v) { u32(static_cast<std::uint32_t>(v >> 32)); u32(static_cast<std::uint32_t>(v)); }
    void stock(const char* s) { for (int i = 0; i < 8; ++i) u8(static_cast<std::uint8_t>(*s ? *s++ : ' ')); }
    void head(char type, std::uint16_t locate) { m.clear(); u8(static_cast<std::uint8_t>(type)); u16(locate); u16(0); u48(34'200'000'000'000ull); }
    void done() {
        out.push_back(static_cast<std::uint8_t>(m.size() >> 8));
        out.push_back(static_cast<std::uint8_t>(m.size()));
        out.insert(out.end(), m.begin(), m.end());
    }
    void add(std::uint16_t loc, std::uint64_t ref, char side, std::uint32_t shares, std::uint32_t px) {
        head('A', loc); u64(ref); u8(static_cast<std::uint8_t>(side)); u32(shares); stock("TEST"); u32(px); done();
    }
    void exec(std::uint16_t loc, std::uint64_t ref, std::uint32_t shares) { head('E', loc); u64(ref); u32(shares); u64(1); done(); }
    void cancel(std::uint16_t loc, std::uint64_t ref, std::uint32_t shares) { head('X', loc); u64(ref); u32(shares); done(); }
    void del(std::uint16_t loc, std::uint64_t ref) { head('D', loc); u64(ref); done(); }
    void replace(std::uint16_t loc, std::uint64_t old_ref, std::uint64_t new_ref, std::uint32_t shares, std::uint32_t px) {
        head('U', loc); u64(old_ref); u64(new_ref); u32(shares); u32(px); done();
    }
    void directory(std::uint16_t loc, const char* sym) {
        head('R', loc); stock(sym); for (int i = 0; i < 20; ++i) u8(0); done();
    }
};

}  // namespace

TEST(Itch, MessageLengthsMatchSpec) {
    ItchWriter w;
    w.add(1, 1, 'B', 1, 1);
    EXPECT_EQ(w.m.size(), itch::kLenAdd);
    w.exec(1, 1, 1);
    EXPECT_EQ(w.m.size(), itch::kLenExec);
    w.cancel(1, 1, 1);
    EXPECT_EQ(w.m.size(), itch::kLenCancel);
    w.del(1, 1);
    EXPECT_EQ(w.m.size(), itch::kLenDelete);
    w.replace(1, 1, 2, 1, 1);
    EXPECT_EQ(w.m.size(), itch::kLenReplace);
    w.directory(1, "AAPL");
    EXPECT_EQ(w.m.size(), itch::kLenStock);
}

TEST(Itch, ReplaysAddsExecutionsCancelsReplaces) {
    ItchWriter w;
    w.directory(7, "AAPL");
    w.add(7, 100, 'B', 300, 1'500'000);  // $150.0000
    w.add(7, 101, 'B', 200, 1'500'000);
    w.add(7, 102, 'S', 100, 1'501'000);
    w.exec(7, 100, 100);                 // front of the bid queue
    w.cancel(7, 101, 50);
    w.replace(7, 102, 103, 400, 1'502'000);
    w.del(7, 100);

    itch::Replayer r(/*check_executions=*/true);
    // Feed in awkward slices to exercise the partial-message carry.
    std::size_t pos = 0, carry_start = 0;
    while (pos < w.out.size()) {
        const std::size_t end = std::min(w.out.size(), pos + 13);
        const std::size_t used = r.feed(w.out.data() + carry_start, end - carry_start);
        carry_start += used;
        pos = end;
    }
    EXPECT_EQ(carry_start, w.out.size());

    const auto* b = r.find_book(7);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(r.symbol(7), "AAPL");
    EXPECT_EQ(*b->best_bid(), 1'500'000);
    EXPECT_EQ(b->top(Side::Buy)->qty, 150u);  // only ref 101 left, 200 - 50
    EXPECT_EQ(*b->best_ask(), 1'502'000);
    EXPECT_EQ(*b->open_qty(103), 400u);
    EXPECT_FALSE(b->open_qty(102));
    EXPECT_EQ(r.stats().unknown_ref, 0u);
    EXPECT_EQ(r.stats().exec_checked, 1u);
    EXPECT_EQ(r.stats().exec_first_in_queue, 1u);
    EXPECT_TRUE(b->check_invariants());
}

TEST(Itch, CountsReferencesToUnknownOrders) {
    ItchWriter w;
    w.del(1, 999);
    w.exec(1, 998, 10);
    itch::Replayer r;
    r.feed(w.out.data(), w.out.size());
    EXPECT_EQ(r.stats().unknown_ref, 2u);
}
