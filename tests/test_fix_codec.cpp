#include "vertexladder/fix.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace vl;
using namespace vl::fix;

namespace {

std::string sample() {
    std::array<char, 32> ts;
    Writer w("D", "CLIENT", "VERTEXLADDER", 7, utc_timestamp(ts));
    w.add(tag::ClOrdID, "abc-1").add(tag::Symbol, "AAPL").add(tag::Side, '1');
    w.add(tag::OrderQty, std::uint64_t{100}).add(tag::OrdType, '2').add_price(tag::Price, 1012500, 4);
    return std::string(w.finish());
}

}  // namespace

TEST(FixCodec, WriterProducesValidFraming) {
    const std::string m = sample();
    ASSERT_EQ(m.rfind("8=FIX.4.4\x01" "9=", 0), 0u);
    Message msg;
    std::size_t used = 0;
    ASSERT_EQ(parse(m, msg, used), ParseStatus::Ok);
    EXPECT_EQ(used, m.size());
    EXPECT_EQ(msg.msg_type(), "D");
    EXPECT_EQ(msg.get(tag::ClOrdID), "abc-1");
    EXPECT_EQ(msg.get(tag::Price), "101.25");
    EXPECT_EQ(*msg.get_uint(tag::OrderQty), 100u);
    EXPECT_EQ(*msg.get_uint(tag::MsgSeqNum), 7u);
}

TEST(FixCodec, NeedsMoreUntilComplete) {
    const std::string m = sample();
    Message msg;
    std::size_t used = 0;
    for (std::size_t cut = 1; cut < m.size(); ++cut)
        ASSERT_EQ(parse(std::string_view(m).substr(0, cut), msg, used), ParseStatus::NeedMore) << cut;
}

TEST(FixCodec, ParsesBackToBackMessages) {
    const std::string two = sample() + sample();
    Message msg;
    std::size_t used = 0;
    ASSERT_EQ(parse(two, msg, used), ParseStatus::Ok);
    ASSERT_EQ(parse(std::string_view(two).substr(used), msg, used), ParseStatus::Ok);
}

TEST(FixCodec, RejectsBadChecksum) {
    std::string m = sample();
    m[m.size() - 2] = m[m.size() - 2] == '0' ? '1' : '0';
    Message msg;
    std::size_t used = 0;
    EXPECT_EQ(parse(m, msg, used), ParseStatus::Garbled);
    EXPECT_EQ(used, m.size());  // skip it and resynchronise
}

TEST(FixCodec, RejectsWrongBodyLength) {
    std::string m = sample();
    const auto p = m.find("9=");
    m[p + 2] = static_cast<char>(m[p + 2] + 1);
    Message msg;
    std::size_t used = 0;
    EXPECT_NE(parse(m, msg, used), ParseStatus::Ok);
}

TEST(FixCodec, ResyncsPastGarbage) {
    const std::string m = "garbage" + sample();
    Message msg;
    std::size_t used = 0;
    ASSERT_EQ(parse(m, msg, used), ParseStatus::Garbled);
    EXPECT_EQ(used, 7u);
    ASSERT_EQ(parse(std::string_view(m).substr(used), msg, used), ParseStatus::Ok);
}

TEST(FixCodec, PricesAreExactFixedPoint) {
    EXPECT_EQ(*parse_price("101.25", 4), 1012500);
    EXPECT_EQ(*parse_price("101", 4), 1010000);
    EXPECT_EQ(*parse_price("0.0001", 4), 1);
    EXPECT_EQ(*parse_price("3.10", 2), 310);
    EXPECT_EQ(*parse_price("-2.5", 4), -25000);
    EXPECT_FALSE(parse_price("1.00001", 4));  // finer than the tick
    EXPECT_FALSE(parse_price("abc", 4));
    EXPECT_FALSE(parse_price("", 4));
    EXPECT_FALSE(parse_price("1.2.3", 4));
    char buf[48];
    EXPECT_EQ(std::string(buf, format_price(1012500, 4, buf)), "101.25");
    EXPECT_EQ(std::string(buf, format_price(1010000, 4, buf)), "101");
    EXPECT_EQ(std::string(buf, format_price(1, 4, buf)), "0.0001");
    EXPECT_EQ(std::string(buf, format_price(-25000, 4, buf)), "-2.5");
}

TEST(FixCodec, ChecksumMatchesSpecExample) {
    // Sum of bytes mod 256.
    EXPECT_EQ(checksum(std::string_view("\x01\x02\xff", 3)), 2u);
}

TEST(FixCodec, UtcTimestampFormat) {
    std::array<char, 32> buf;
    const std::string_view ts = utc_timestamp(buf);
    ASSERT_EQ(ts.size(), 21u);  // YYYYMMDD-HH:MM:SS.sss
    EXPECT_EQ(ts[8], '-');
    EXPECT_EQ(ts[11], ':');
    EXPECT_EQ(ts[14], ':');
    EXPECT_EQ(ts[17], '.');
    EXPECT_EQ(ts.substr(0, 2), "20");
}
