// EXT-08 Bus Telemetry Bridge - M5: the handler-to-writer queue (BTB-BP-1, BTB-BP-2,
// BTB-BP-4).
//
// This is the second of the program's two backpressure boundaries. The first is the bus
// subscription; this one is ours, and ADR-4 requires both to be an explicit decision rather
// than an inherited default.
//
// Shape:
//
//   producer   the bus pump thread, inside a DecodedHandler. offer() copies, counts if it
//              cannot, and returns. It never formats, never touches a file, never blocks on
//              anything but the mutex, and never waits on the consumer.
//   consumer   our own writer thread. Everything else - float conversion, JSON, IO - happens
//              there and nowhere else.
//
// One queue, all topics. Per-topic queues would give per-topic FIFO trivially and lose the
// interleaving between topics, and the interleaving is what guarantees an entity_add is
// written before the samples it opens. A single FIFO preserves global arrival order, and
// BTB-BP-2's per-topic order follows from it for free (docs/decisions-m5-m7.md, D-7).
//
// Two capacities, not one. A sample burst that filled the queue could evict an
// entity_created, and a lost entity_created orphans every subsequent sample for that name -
// a bounded sample loss turned into an unbounded correctness loss. So samples are refused
// above `sampleCapacity` while structural records (roster and segment events) are refused
// only above `sampleCapacity + structuralReserve`. Both bounded, both counted, and the
// catastrophic loss is unreachable at any rate this platform produces: the reference run's
// entire event traffic is 134 messages against 132 188 samples (D-8).
//
// BLOCK is not offered. The format specification already promises EXT-17 in writing that
// this producer never blocks the bus (docs/capture-format-v1.md section 14), and blocking
// here blocks the handler, which stalls the bus delivery thread - the same perturbation by a
// longer route (D-5).

#pragma once

#include "CaptureRecord.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace n8ro::bridge {

enum class OverflowPolicy {
    DropNewest,   // refuse the arriving record. The default - see below
    DropOldest,   // evict the front of the queue to make room
};

// Parses `--overflow-policy`. Returns false for anything unrecognised, and for `block`
// specifically it sets `error` to the reason rather than merely rejecting the spelling: a
// caller who asks for it has read BTB-BP-4, which lists it, and deserves to know why this
// producer does not offer it.
[[nodiscard]] bool parseOverflowPolicy(const std::string& text, OverflowPolicy& out,
                                       std::string& error);

[[nodiscard]] const char* overflowPolicyName(OverflowPolicy policy);

struct QueueCounters {
    std::uint64_t samplesOffered = 0;
    std::uint64_t structuralOffered = 0;

    // Genuine overflow, while recording: the queue was full and a record was refused or
    // evicted. These go into the trailer (BTB-BP-4).
    std::uint64_t samplesDropped = 0;        // -> trailer.drops.samples_not_recorded
    std::uint64_t structuralDropped = 0;     // -> trailer.drops.events_not_recorded

    // Arrivals *after* close() - the shutdown window. A bus subscription cannot be stopped
    // atomically, so this can never be zero, and it is scheduler-dependent. It is counted
    // apart from overflow and reported only in the log, never in the file, for exactly the
    // reason M4 removed the equivalent number: a capture carrying it would differ between two
    // runs on a value that has nothing to do with the run (BTB-CAP-3, and docs/
    // capture-format-v1.md section 16). These are not losses - they are after the end.
    std::uint64_t samplesAfterClose = 0;
    std::uint64_t structuralAfterClose = 0;

    std::size_t highWater = 0;               // peak depth, for the run summary
};

class RecordQueue {
public:
    RecordQueue(std::size_t sampleCapacity, std::size_t structuralReserve, OverflowPolicy policy);

    // Called from the bus pump thread. Copies and returns; the only thing it does beyond a
    // move into the deque is notify the writer, which is a futex wake rather than work.
    void offer(CaptureRecord record);

    // Called from the writer thread. Moves everything currently queued into `out` in FIFO
    // order and returns true, or returns false if the timeout elapsed with nothing to take
    // and the queue is still open. Draining in batches is what keeps the writer's per-record
    // cost to a serialise and an append.
    //
    // Returns true with an empty `out` only when the queue has been closed and drained, which
    // is the writer's signal to finish.
    [[nodiscard]] bool waitAndDrain(std::vector<CaptureRecord>& out,
                                    std::chrono::milliseconds timeout);

    // Called from our own thread after the subscriptions are cancelled and the pump is
    // stopped. Wakes the writer so it drains what is left and returns.
    void close();

    [[nodiscard]] bool isClosed() const;
    [[nodiscard]] QueueCounters counters() const;
    [[nodiscard]] std::size_t depth() const;

    [[nodiscard]] std::size_t sampleCapacity() const { return sampleCapacity_; }
    [[nodiscard]] std::size_t hardCapacity() const { return sampleCapacity_ + structuralReserve_; }
    [[nodiscard]] OverflowPolicy policy() const { return policy_; }

private:
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<CaptureRecord> records_;
    std::size_t sampleCapacity_ = 0;
    std::size_t structuralReserve_ = 0;
    OverflowPolicy policy_ = OverflowPolicy::DropNewest;
    bool closed_ = false;
    QueueCounters counters_;
};

}  // namespace n8ro::bridge
