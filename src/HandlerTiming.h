// EXT-08 Bus Telemetry Bridge - M5: how long a handler actually takes (BTB-BP-1).
//
// BTB-BP-1's acceptance criterion is "handler time is bounded and **measured**; the
// measurement appears in the run summary", and the PRD's quality-gate notes flag the p95
// target as unvalidated with the suggestion to instrument it at M5 - "when the handler
// finally has a writer to hand off to and the number means something". This is that
// instrument.
//
// Two steady_clock reads per message and one relaxed atomic increment. That is a control-
// plane measurement, and it goes to the log and the run summary - never into a capture
// record, where a wall-clock-derived value is forbidden outright (BTB-CAP-2, ADR-3).
//
// Log-spaced buckets rather than a reservoir: percentiles from a histogram need no
// allocation, no lock, and no per-sample storage, and the targets being checked (p50 < 20 us,
// p95 < 100 us, p99 < 500 us) are decades apart, so bucket resolution is not the limit on
// what the answer can tell us.

#pragma once

#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <string>

namespace n8ro::bridge {

class HandlerTiming {
public:
    // Upper edge of each bucket, in microseconds; the last bucket is everything above.
    static constexpr std::array<std::uint64_t, 13> kEdgesUs = {
        1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000};

    // Called from the bus pump thread, once per message, with the elapsed time of the
    // handler body. Relaxed ordering: these counters are read only after the pump has
    // stopped, and a lost increment would cost a digit in a diagnostic rather than
    // correctness anywhere.
    void note(std::chrono::steady_clock::duration elapsed) {
        const std::uint64_t us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
        std::size_t bucket = kEdgesUs.size();   // the overflow bucket
        for (std::size_t i = 0; i < kEdgesUs.size(); ++i) {
            if (us <= kEdgesUs[i]) {
                bucket = i;
                break;
            }
        }
        buckets_[bucket].fetch_add(1, std::memory_order_relaxed);
        count_.fetch_add(1, std::memory_order_relaxed);
        // A running maximum, for the one number a histogram cannot give back.
        std::uint64_t previous = maxUs_.load(std::memory_order_relaxed);
        while (us > previous &&
               !maxUs_.compare_exchange_weak(previous, us, std::memory_order_relaxed)) {
        }
    }

    [[nodiscard]] std::uint64_t count() const { return count_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t maxUs() const { return maxUs_.load(std::memory_order_relaxed); }

    // The bucket's upper edge at or below which `fraction` of samples fell. Reported as an
    // upper bound ("<= 20 us") rather than an interpolated value, because interpolating
    // inside a log-spaced bucket invents precision the histogram does not hold.
    [[nodiscard]] std::uint64_t percentileUpperBoundUs(double fraction) const;

    // One line for the run summary: "n=132188 p50<=2us p95<=10us p99<=50us max=3184us".
    [[nodiscard]] std::string summary() const;

private:
    std::array<std::atomic<std::uint64_t>, kEdgesUs.size() + 1> buckets_{};
    std::atomic<std::uint64_t> count_{0};
    std::atomic<std::uint64_t> maxUs_{0};
};

}  // namespace n8ro::bridge
