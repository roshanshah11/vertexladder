#include "vertexladder/fix.hpp"

#include <charconv>
#include <chrono>
#include <cstring>
#include <ctime>

namespace vl::fix {

namespace {

constexpr std::string_view kBegin = "8=FIX.4.4\x01";

bool parse_uint(std::string_view s, std::uint64_t& v) noexcept {
    if (s.empty()) return false;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}

// Parses "tag=value<SOH>" at pos. Returns false if the bytes there aren't a field.
bool next_field(std::string_view buf, std::size_t& pos, Field& f) noexcept {
    const std::size_t eq = buf.find('=', pos);
    if (eq == std::string_view::npos || eq == pos) return false;
    std::uint64_t t = 0;
    if (!parse_uint(buf.substr(pos, eq - pos), t) || t == 0 || t > 99999) return false;
    const std::size_t soh = buf.find(kSoh, eq + 1);
    if (soh == std::string_view::npos) return false;
    f.tag = static_cast<int>(t);
    f.value = buf.substr(eq + 1, soh - eq - 1);
    pos = soh + 1;
    return true;
}

// Skip to the next plausible message start so one bad message doesn't wedge a session.
std::size_t resync(std::string_view buf) noexcept {
    const std::size_t next = buf.find(kBegin, 1);
    return next == std::string_view::npos ? buf.size() : next;
}

}  // namespace

std::optional<std::uint64_t> Message::get_uint(int t) const noexcept {
    std::uint64_t v = 0;
    if (!parse_uint(get(t), v)) return std::nullopt;
    return v;
}

std::uint8_t checksum(std::string_view bytes) noexcept {
    unsigned sum = 0;
    for (char c : bytes) sum += static_cast<unsigned char>(c);
    return static_cast<std::uint8_t>(sum % 256);
}

ParseStatus parse(std::string_view buf, Message& out, std::size_t& consumed) {
    out.count_ = 0;
    consumed = 0;
    if (buf.size() < kBegin.size()) {
        if (kBegin.substr(0, buf.size()) == buf) return ParseStatus::NeedMore;
        consumed = resync(buf);
        return ParseStatus::Garbled;
    }
    if (buf.substr(0, kBegin.size()) != kBegin) {
        consumed = resync(buf);
        return ParseStatus::Garbled;
    }
    // 9=BodyLength
    std::size_t pos = kBegin.size();
    if (buf.find(kSoh, pos) == std::string_view::npos) return ParseStatus::NeedMore;
    Field f{};
    if (!next_field(buf, pos, f) || f.tag != tag::BodyLength) {
        consumed = resync(buf);
        return ParseStatus::Garbled;
    }
    std::uint64_t body_len = 0;
    if (!parse_uint(f.value, body_len) || body_len > 8192) {
        consumed = resync(buf);
        return ParseStatus::Garbled;
    }
    const std::size_t body_start = pos;
    const std::size_t trailer = body_start + body_len;  // where "10=" must start
    if (buf.size() < trailer + 7) return ParseStatus::NeedMore;  // "10=XXX<SOH>"
    if (buf.substr(trailer, 3) != "10=" || buf[trailer + 6] != kSoh) {
        consumed = resync(buf);
        return ParseStatus::Garbled;
    }
    std::uint64_t sum = 0;
    if (!parse_uint(buf.substr(trailer + 3, 3), sum) || sum != checksum(buf.substr(0, trailer))) {
        consumed = resync(buf);
        return ParseStatus::Garbled;
    }

    out.fields_[0] = {tag::BeginString, buf.substr(2, 7)};
    out.fields_[1] = f;
    out.count_ = 2;
    while (pos < trailer) {
        if (out.count_ == Message::kMaxFields || !next_field(buf, pos, f) || pos > trailer) {
            out.count_ = 0;
            consumed = trailer + 7;
            return ParseStatus::Garbled;
        }
        out.fields_[out.count_++] = f;
    }
    if (out.count_ < 3 || out.fields_[2].tag != tag::MsgType) {
        out.count_ = 0;
        consumed = trailer + 7;
        return ParseStatus::Garbled;
    }
    consumed = trailer + 7;
    return ParseStatus::Ok;
}

std::optional<Price> parse_price(std::string_view s, int decimals) noexcept {
    if (s.empty()) return std::nullopt;
    bool neg = false;
    if (s[0] == '-') {
        neg = true;
        s.remove_prefix(1);
    }
    const std::size_t dot = s.find('.');
    const std::string_view whole = s.substr(0, dot);
    std::string_view frac = dot == std::string_view::npos ? std::string_view{} : s.substr(dot + 1);
    while (!frac.empty() && frac.back() == '0') frac.remove_suffix(1);
    if (frac.size() > static_cast<std::size_t>(decimals)) return std::nullopt;  // off the tick grid
    std::uint64_t w = 0, fr = 0;
    if (whole.empty() && frac.empty()) return std::nullopt;
    if (!whole.empty() && !parse_uint(whole, w)) return std::nullopt;
    if (!frac.empty() && !parse_uint(frac, fr)) return std::nullopt;
    std::uint64_t scale = 1;
    for (int i = 0; i < decimals; ++i) scale *= 10;
    for (std::size_t i = frac.size(); i < static_cast<std::size_t>(decimals); ++i) fr *= 10;
    // Overflow: the whole part times scale, plus the fraction, must fit in int64.
    if (w > (static_cast<std::uint64_t>(INT64_MAX) - fr) / scale) return std::nullopt;
    const auto v = static_cast<Price>(w * scale + fr);  // <= INT64_MAX, so negating is safe
    return neg ? -v : v;
}

std::size_t format_price(Price p, int decimals, char* out) noexcept {
    std::size_t n = 0;
    std::uint64_t v;
    if (p < 0) {
        out[n++] = '-';
        v = static_cast<std::uint64_t>(-(p + 1)) + 1;
    } else {
        v = static_cast<std::uint64_t>(p);
    }
    std::uint64_t scale = 1;
    for (int i = 0; i < decimals; ++i) scale *= 10;
    n += static_cast<std::size_t>(std::to_chars(out + n, out + n + 24, v / scale).ptr - (out + n));
    std::uint64_t frac = v % scale;
    if (frac) {
        out[n++] = '.';
        char digits[24];
        int k = decimals;
        while (k > 0 && frac % 10 == 0) {
            frac /= 10;
            --k;
        }
        for (int i = k - 1; i >= 0; --i) {
            digits[i] = static_cast<char>('0' + frac % 10);
            frac /= 10;
        }
        std::memcpy(out + n, digits, static_cast<std::size_t>(k));
        n += static_cast<std::size_t>(k);
    }
    return n;
}

Writer::Writer(std::string_view msg_type, std::string_view sender, std::string_view target, std::uint64_t seq,
               std::string_view sending_time) {
    add(tag::MsgType, msg_type);
    add(tag::SenderCompID, sender);
    add(tag::TargetCompID, target);
    add(tag::MsgSeqNum, seq);
    add(tag::SendingTime, sending_time);
}

void Writer::put(std::string_view s) {
    if (len_ + s.size() + 8 > kCapacity) {  // keep room for the 10=XXX trailer
        overflow_ = true;
        return;
    }
    std::memcpy(buf_.data() + len_, s.data(), s.size());
    len_ += s.size();
}

void Writer::put_tag(int t) {
    char tmp[16];
    auto r = std::to_chars(tmp, tmp + sizeof tmp, t);
    *r.ptr++ = '=';
    put({tmp, static_cast<std::size_t>(r.ptr - tmp)});
}

Writer& Writer::add(int t, std::string_view v) {
    put_tag(t);
    put(v);
    put({&kSoh, 1});
    return *this;
}

Writer& Writer::add(int t, std::uint64_t v) {
    char tmp[24];
    auto r = std::to_chars(tmp, tmp + sizeof tmp, v);
    return add(t, std::string_view(tmp, static_cast<std::size_t>(r.ptr - tmp)));
}

Writer& Writer::add(int t, char v) { return add(t, std::string_view(&v, 1)); }

Writer& Writer::add_price(int t, Price p, int decimals) {
    char tmp[48];
    return add(t, std::string_view(tmp, format_price(p, decimals, tmp)));
}

std::string_view Writer::finish() {
    if (overflow_) return {};
    const std::size_t body_len = len_ - kHeadRoom;
    char head[kHeadRoom];
    std::size_t h = 0;
    std::memcpy(head, kBegin.data(), kBegin.size());
    h += kBegin.size();
    head[h++] = '9';
    head[h++] = '=';
    h += static_cast<std::size_t>(std::to_chars(head + h, head + sizeof head, body_len).ptr - (head + h));
    head[h++] = kSoh;
    start_ = kHeadRoom - h;
    std::memcpy(buf_.data() + start_, head, h);
    const std::uint8_t sum = checksum({buf_.data() + start_, len_ - start_});
    char trailer[8] = {'1', '0', '=', static_cast<char>('0' + sum / 100), static_cast<char>('0' + sum / 10 % 10),
                       static_cast<char>('0' + sum % 10), kSoh, 0};
    std::memcpy(buf_.data() + len_, trailer, 7);
    len_ += 7;
    return {buf_.data() + start_, len_ - start_};
}

std::string_view utc_timestamp(std::array<char, 32>& out) {
    // gmtime + strftime cost about a microsecond, and every outgoing message needs a
    // SendingTime. The date-and-seconds part only changes once a second, so format it
    // once per second per thread and append the milliseconds by hand.
    using namespace std::chrono;
    thread_local std::time_t cached_sec = -1;
    thread_local char prefix[24];
    thread_local std::size_t prefix_len = 0;
    const auto now = system_clock::now();
    const auto ms = static_cast<int>(duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000);
    const std::time_t t = system_clock::to_time_t(now);
    if (t != cached_sec) {
        std::tm tm{};
        gmtime_r(&t, &tm);
        prefix_len = std::strftime(prefix, sizeof prefix, "%Y%m%d-%H:%M:%S", &tm);
        cached_sec = t;
    }
    std::memcpy(out.data(), prefix, prefix_len);
    char* p = out.data() + prefix_len;
    *p++ = '.';
    *p++ = static_cast<char>('0' + ms / 100);
    *p++ = static_cast<char>('0' + ms / 10 % 10);
    *p++ = static_cast<char>('0' + ms % 10);
    return {out.data(), prefix_len + 4};
}

}  // namespace vl::fix
