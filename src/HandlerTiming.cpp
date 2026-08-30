#include "HandlerTiming.h"

#include <cstdio>

namespace n8ro::bridge {

std::uint64_t HandlerTiming::percentileUpperBoundUs(double fraction) const {
    const std::uint64_t total = count_.load(std::memory_order_relaxed);
    if (total == 0) {
        return 0;
    }
    // The first bucket whose running total reaches the requested fraction. Ceiling rather
    // than rounding, so p95 of 20 messages is the 19th and not the 19th-or-18th.
    const std::uint64_t target =
        static_cast<std::uint64_t>(static_cast<double>(total) * fraction + 0.999999);
    std::uint64_t running = 0;
    for (std::size_t i = 0; i < kEdgesUs.size(); ++i) {
        running += buckets_[i].load(std::memory_order_relaxed);
        if (running >= target) {
            return kEdgesUs[i];
        }
    }
    // Everything at or past the overflow bucket. The exact maximum is the honest answer here,
    // because the bucket itself has no upper edge.
    return maxUs_.load(std::memory_order_relaxed);
}

std::string HandlerTiming::summary() const {
    const std::uint64_t total = count_.load(std::memory_order_relaxed);
    if (total == 0) {
        return "no handler invocations measured";
    }
    char line[256];
    std::snprintf(line, sizeof(line),
                  "n=%llu p50<=%lluus p95<=%lluus p99<=%lluus max=%lluus",
                  static_cast<unsigned long long>(total),
                  static_cast<unsigned long long>(percentileUpperBoundUs(0.50)),
                  static_cast<unsigned long long>(percentileUpperBoundUs(0.95)),
                  static_cast<unsigned long long>(percentileUpperBoundUs(0.99)),
                  static_cast<unsigned long long>(maxUs_.load(std::memory_order_relaxed)));
    return std::string(line);
}

}  // namespace n8ro::bridge
