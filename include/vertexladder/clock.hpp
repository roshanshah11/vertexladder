#pragma once

// Cheap timestamps for benchmarking.
//
// x86-64:        RDTSC, invariant TSC, sub-nanosecond resolution.
// Apple Silicon: mach_absolute_time, ~6 ns per call, but the counter behind it ticks at
//                24 MHz, so one tick is 41.67 ns. An operation shorter than that can't
//                be timed on its own: per-op samples come out as 0 or 1 tick. The
//                benchmarks therefore report the mean from total time / ops, and
//                per-op percentiles only as "at tick resolution".
// Other:         std::chrono::steady_clock.

#include <chrono>
#include <cstdint>
#include <thread>

#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
#define VL_CLOCK_TSC 1
#elif defined(__APPLE__)
#include <mach/mach_time.h>
#define VL_CLOCK_MACH 1
#endif

namespace vl {

class Clock {
public:
    static std::uint64_t now() noexcept {
#if defined(VL_CLOCK_TSC)
        // rdtsc isn't ordered with the instructions around it; the fences keep the
        // timed work from leaking across the timestamp.
        _mm_lfence();
        const std::uint64_t t = __rdtsc();
        _mm_lfence();
        return t;
#elif defined(VL_CLOCK_MACH)
        return mach_absolute_time();
#else
        return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
    }

    // Nanoseconds per tick, measured once.
    static double ns_per_tick() {
        static const double v = calibrate();
        return v;
    }

    static const char* name() noexcept {
#if defined(VL_CLOCK_TSC)
        return "rdtsc";
#elif defined(VL_CLOCK_MACH)
        return "mach_absolute_time";
#else
        return "steady_clock";
#endif
    }

private:
    static double calibrate() {
#if defined(VL_CLOCK_MACH)
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        return static_cast<double>(tb.numer) / static_cast<double>(tb.denom);
#elif defined(VL_CLOCK_TSC)
        using sc = std::chrono::steady_clock;
        const auto w0 = sc::now();
        const std::uint64_t c0 = now();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const auto w1 = sc::now();
        const std::uint64_t c1 = now();
        const double ns = std::chrono::duration<double, std::nano>(w1 - w0).count();
        return ns / static_cast<double>(c1 - c0);
#else
        using P = std::chrono::steady_clock::period;
        return 1e9 * static_cast<double>(P::num) / static_cast<double>(P::den);
#endif
    }
};

}  // namespace vl
