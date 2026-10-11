#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <vector>

namespace vl {

// Latency histogram in clock ticks. Exact counts for values below 2^16 ticks (every
// latency this engine produces), power-of-two buckets above that. Recording is an
// increment, so it can sit inside a benchmark loop without distorting it much, and
// percentiles come from the full distribution instead of a sample.
class Histogram {
public:
    Histogram() : exact_(kExact, 0) {}

    void record(std::uint64_t ticks) noexcept {
        ++count_;
        sum_ += ticks;
        if (ticks < kExact) {
            ++exact_[ticks];
        } else {
            ++overflow_[static_cast<std::size_t>(std::bit_width(ticks))];
        }
        if (ticks > max_) max_ = ticks;
    }

    std::uint64_t count() const noexcept { return count_; }
    std::uint64_t max() const noexcept { return max_; }

    // Value at quantile q in [0, 1], in ticks (upper bound of the bucket above 2^16).
    std::uint64_t quantile(double q) const noexcept {
        if (count_ == 0) return 0;
        const auto rank = static_cast<std::uint64_t>(q * static_cast<double>(count_ - 1)) + 1;
        std::uint64_t seen = 0;
        for (std::uint64_t v = 0; v < kExact; ++v) {
            seen += exact_[v];
            if (seen >= rank) return v;
        }
        for (std::size_t b = 0; b < overflow_.size(); ++b) {
            seen += overflow_[b];
            if (seen >= rank) return (std::uint64_t{1} << b) - 1;
        }
        return max_;
    }

    double mean() const noexcept {
        return count_ ? static_cast<double>(sum_) / static_cast<double>(count_) : 0.0;
    }

private:
    static constexpr std::uint64_t kExact = 1u << 16;
    std::vector<std::uint64_t> exact_;
    std::array<std::uint64_t, 65> overflow_{};
    std::uint64_t count_ = 0;
    std::uint64_t sum_ = 0;
    std::uint64_t max_ = 0;
};

}  // namespace vl
