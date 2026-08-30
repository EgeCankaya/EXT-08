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
    void offer(CapturedSample sample);

    [[nodiscard]] bool atCapacity() const;
    [[nodiscard]] std::size_t size() const;

    // Samples the bus delivered after the budget was reached. Counted, never silent
    // (tenet 3), and reported in the trailer's `drops`.
    [[nodiscard]] std::uint64_t notRecorded() const;

    // Phase 2 only: valid after recording has stopped and the subscriptions are gone.
    // Returns the records in arrival order - FIFO per topic is the ordering the capture
    // promises (BTB-BP-2), and a single-producer append preserves it by construction.
    [[nodiscard]] const std::vector<CapturedSample>& records() const;

private:
    mutable std::mutex mutex_;
    std::vector<CapturedSample> records_;
    std::size_t capacity_ = 0;
    std::uint64_t notRecorded_ = 0;
};

}  // namespace n8ro::bridge
