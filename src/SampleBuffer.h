// EXT-08 Bus Telemetry Bridge - M4: the bounded record buffer.
//
// M4 has no writer thread and no handler-to-writer queue - both are M5's (BTB-BP-1,
// BTB-BP-2). What it needs is a real capture from a real run to prove the format against,
// so it records in two phases:
//
//   phase 1  the bus pump thread copies each accepted sample in here and returns. That is
//            the whole of the handler's work, so it stays a courier (CLAUDE.md rule 2).
//            Nothing consumes the buffer while it fills; there is no consumer to run.
//   phase 2  recording stops when the budget is reached. Only then does our own thread
//            read the buffer, format every record and write the file. All IO, parsing and
//            formatting happen there, on our thread, exactly as the rule requires.
//
// This is deliberately not M5's design and is not a first draft of it. M5 replaces both
// phases with a streaming writer thread behind a bounded queue; what survives unchanged is
// CaptureFormat's serialiser, which never knew where its records came from.
//
// The budget is what ends the run, so it is also what makes the capture deterministic: the
// same scenario recorded to the same budget stops at the same sample, with no clock
// involved anywhere in the decision.

#pragma once

// StreamValueMap is an alias declared in MessageBusPacked.h, not in StreamValue.h - the
// value type and the map over it live in different headers.
#include <messaging/packed/MessageBusPacked.h>
#include <messaging/packed/StreamValue.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace n8ro::bridge {

// One accepted entity-state sample, held verbatim until it is written.
//
// `values` is the decoded StreamValueMap exactly as the bus delivered it. Nothing is
// projected onto a curated struct here for the same reason the entity picture does not do
// it: BTB-CAP-4's verbatim rule is what makes a schema that gains a field carry it through
// with no code change.
struct CapturedSample {
    std::string scenarioEntityName;
    std::uint64_t occupancy = 0;      // Occupancy::generation, spelled `occupancy` in the file
    double simulationTimeS = 0.0;     // the sample's own clock - the only clock (ADR-3)
    n8ro::sim::StreamValueMap values;
};

class SampleBuffer {
public:
    explicit SampleBuffer(std::size_t capacity);

    // Called from the bus pump thread. Copies and returns; never formats, never allocates
    // the backing store (it is reserved up front), never blocks on anything but the lock.
    // Once the budget is reached every further sample is counted and discarded, so the
    // recorded prefix is a complete FIFO run rather than a sampled one.
    //
    // On the transition to full it notifies waitUntilFull() exactly once. That notify is the
    // "hand off" half of copy-hand-off-return - a futex wake, not work: no IO, no
    // formatting, no allocation, and it does not block. It is the only thing this handler
    // does that is not a plain copy, which is why it is called out here.
    void offer(CapturedSample sample);

    [[nodiscard]] bool atCapacity() const;
    [[nodiscard]] std::size_t size() const;

    // Blocks the calling thread until the budget is reached or `timeout` elapses, and
    // returns whether the buffer is now full. Called from our own thread in place of a flat
    // sleep, so that recording stops when the budget is reached rather than up to a status
    // tick later. The timeout is what keeps the once-a-second report going while it fills.
    [[nodiscard]] bool waitUntilFull(std::chrono::milliseconds timeout) const;

    // Samples the bus delivered *after* recording ended - the residue between the budget
    // being reached and the subscription actually being cancelled, which cannot be zero
    // because a bus subscription cannot be stopped atomically.
    //
    // This is deliberately NOT written into the capture. It is scheduler-dependent, so a
    // capture carrying it would differ between two identical runs and break BTB-CAP-3; and
    // it would be misleading anyway, because it counts the handful of samples that landed in
    // the shutdown window and not the tens of thousands published after we stopped. The
    // capture says "recording ended deliberately" with `end_reason: size_limit`, and this
    // number goes to the log, where a scheduler-dependent observation belongs.
    [[nodiscard]] std::uint64_t notRecorded() const;

    // Phase 2 only: valid after recording has stopped and the subscriptions are gone.
    // Returns the records in arrival order - FIFO per topic is the ordering the capture
    // promises (BTB-BP-2), and a single-producer append preserves it by construction.
    [[nodiscard]] const std::vector<CapturedSample>& records() const;

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable full_;
    std::vector<CapturedSample> records_;
    std::size_t capacity_ = 0;
    std::uint64_t notRecorded_ = 0;
};

}  // namespace n8ro::bridge
