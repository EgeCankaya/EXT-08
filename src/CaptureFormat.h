// EXT-08 Bus Telemetry Bridge - M4: the capture format `n8ro-capture/1`.
//
// The normative contract is docs/capture-format-v1.md, which is a cross-repo artifact:
// EXT-17 gets that file and nothing else, and a reader is written from it alone. This
// header is the producer side of the same contract, and where the two disagree the
// document wins - it is what was handed across the boundary.
//
// Everything here is a pure function over its arguments. No file handle, no clock, no
// global state, no member of a writer object: a record's text is a function of the record,
// so it can be produced from a live bus, from a buffer, or from M5's writer thread without
// changing. The only reason this file knows what a StreamValueMap is at all is that
// encoding one verbatim is the job.
//
// Two rules carry most of the weight:
//
//   Field order comes from MessageSchema::fields, never from the StreamValueMap. The map
//   is an std::unordered_map, so iterating it would make the capture's byte layout depend
//   on hash-table internals - a determinism leak that would look like a platform defect
//   from EXT-17's side (BTB-CAP-3, R4).
//
//   A schema-declared field the publisher did not send is ABSENT from the object, not
//   defaulted. header.schemas still carries the full declaration, so a reader can tell a
//   never-published field from a dropped one. This is not hypothetical: the entity-state
//   schema declares twelve fields and `activeAnimation` appeared zero times in 132 188
//   samples (notes.md, M3).

#pragma once

#include "CaptureRecord.h"

#include <messaging/packed/MessageSchema.h>
#include <messaging/packed/StreamValue.h>

#include <cstdint>
#include <string>
#include <vector>

namespace n8ro::bridge::capture {

// The version string lives in exactly two places: here and the title of
// docs/capture-format-v1.md. BTB-CAP-5 requires the two to be checked against each other,
// and tests/capture-reader does that check.
constexpr const char* kFormatVersion = "n8ro-capture/1";
constexpr const char* kProducerName = "n8ro-bridge";
// The format is unchanged - no key added, removed or retyped - so this stays
// n8ro-capture/1. What the producer version tracks is how this build fills the format, and
// 0.4.1 changed two answers: `drops.samples_not_recorded` is structurally 0 at this
// milestone, and `attached_mid_run` is derived causally rather than from a status tick.
// Both were run-to-run variable at 0.4.0 (see docs/capture-format-v1.md §16). 0.4.2 adds the
// bus's four delivery-side drop counters to `bus_metrics`, which nothing in this program had
// ever read - adding keys is non-breaking, so the format version does not move.
// 0.5.0 was M5: the writer thread behind a bounded queue, segment_open / segment_close
// driven by scenario events, entity_add / entity_remove from the roster, and a real overflow
// count in `drops.samples_not_recorded`.
//
// 0.7.0 is M7: clean interruption, so `end_reason: "shutdown"` is reachable. No format change.
//
// **The format is frozen from here.** docs/capture-format-v1.md is a cross-repo contract, and
// after the M7 freeze a change to what it specifies is a version bump and a downstream change,
// not an edit. Adding a key to an existing record stays non-breaking (spec section 13);
// renaming one, retyping one, changing a unit, or adding a record type does not.
//
// 0.6.0 was M6: `verdict` records are emitted, which completes the eight-type vocabulary. The
// referee also writes them to a `verdicts-*.jsonl` beside the capture, so a live run and a
// `--replay` of its own capture can be compared as files - and they are byte-identical, which
// is what makes BTB-REF-4 a test rather than a claim. Nothing about the format changed: every
// record type emitted was already specified and no key was renamed or retyped, so this is
// still n8ro-capture/1.
constexpr const char* kProducerVersion = "0.7.0";

// Every value here is observed, not asserted. `runtimeVersion` comes from the SDK's own
// getN8roVersion(), which reports "unknown" when the release headers were compiled without
// N8RO_VERSION defined - which is the case on this toolchain. Recording "unknown" is the
// honest reading; recording 2.1.328 would be this program claiming to have measured
// something it read off an installer manifest.
struct PlatformInfo {
    std::string engineConfig;
    std::string modelPath;
    std::string schemaFile;
    std::string schemaVersion;    // DbModel::getSchemaVersion()
    std::string runtimeVersion;   // n8ro::core::getN8roVersion()
};

// The values BTB-BP-3 requires to be an explicit decision rather than a default. At M4
// they are still the bus defaults and the header says so, which is the point: the policy
// a capture was recorded under is a property of the capture, not of the release notes.
struct SubscriptionInfo {
    std::string topic;
    std::string backpressurePolicy;
    std::uint64_t queueSize = 0;
};

struct HeaderInfo {
    PlatformInfo platform;
    SubscriptionInfo subscription;
    bool attachedMidRun = false;
    // One entry per message type that appears as a `sample` record's `message`. Sorted by
    // messageName on write, so the header's bytes do not depend on registry iteration.
    std::vector<n8ro::sim::MessageSchema> schemas;
};

struct TrailerCounts {
    std::uint64_t segments = 0;
    std::uint64_t samples = 0;
    std::uint64_t entitiesAdded = 0;
    std::uint64_t entitiesRemoved = 0;
    std::uint64_t verdicts = 0;
};

// Losses on our side of the bus. `samplesNotRecorded` was structurally 0 at M4, where the
// buffer filling *was* the end of recording; from M5 it carries the handler-to-writer
// queue's genuine overflow count, which is the meaning the field was reserved for.
//
// `eventsNotRecorded` is new at 0.5.0 and counts roster and segment records the queue could
// not take. It is expected to be zero - the queue reserves headroom for exactly these
// (RecordQueue, D-8) - but a counter that can only be zero by design should say so rather
// than not exist, because a non-zero value here means the file's structure is incomplete
// and not merely its data.
struct TrailerDrops {
    std::uint64_t samplesNotRecorded = 0;
    std::uint64_t eventsNotRecorded = 0;
    std::uint64_t samplesOrphaned = 0;
    std::uint64_t samplesUnnamed = 0;
    std::uint64_t samplesUntimed = 0;
};

// Everything the platform will tell us about what it lost, as BTB-OBS-1 requires. Two
// groups, and the distinction between them cost a determinism experiment to learn.
//
// The DECODE group is MessageBusPackedMetricsSnapshot: a message reached our subscription
// and could not be turned into values. These are the counters M1-M4 reported, and they were
// zero throughout - correctly, because nothing ever failed to decode.
//
// The DELIVERY group is IMessageBus::Statistics: a message never reached our subscription at
// all, because the bus discarded it. **Nothing in EXT-08 read these until now**, which is
// why a run could lose whole simulation frames and still report "0 drops" - we were watching
// the decoder while the loss was happening upstream of it. A capture that says zero because
// nobody asked the right object is the silent-wrong-answer failure R2 is about, so both
// groups now go into every capture and onto the status line.
struct TrailerBusMetrics {
    // Decode side - MessageBusPacked.
    std::uint64_t schemaHashDrops = 0;
    std::uint64_t messageIdDrops = 0;
    std::uint64_t decodeFailures = 0;
    std::uint64_t missingSchemaPassthrough = 0;
    std::uint64_t legacyPayloadPassthrough = 0;
    // Delivery side - IMessageBus::Statistics. Bus-wide, not per-subscription: the bus does
    // not attribute a discard to a subscriber, so a non-zero value here means the bus lost
    // something, not necessarily something of ours. It is still the only warning available.
    std::uint64_t messagesDropped = 0;
    std::uint64_t droppedByBackpressure = 0;
    std::uint64_t droppedByQueueOverflow = 0;
    std::uint64_t droppedByRateLimiting = 0;
};

// Each of these returns exactly one capture line, without its terminating LF. `format_version`
// is the first key of the header, so a reader rejects an unknown version before parsing
// anything else (BTB-CAP-1).
[[nodiscard]] std::string writeHeader(const HeaderInfo& info);

[[nodiscard]] std::string writeSegmentOpen(
    double simTimeS, std::uint64_t segment, const std::string& scenario);

[[nodiscard]] std::string writeSegmentClose(
    double simTimeS, std::uint64_t segment, const std::string& scenario,
    const std::string& reason);

// The verbatim, schema-ordered sample record (BTB-CAP-4). `schema` supplies the field
// order and nothing else - types and values both come off the StreamValue, so a field
// whose wire type differs from its declaration is recorded as it arrived rather than
// coerced to the declaration.
[[nodiscard]] std::string writeSample(
    const CaptureRecord& sample, std::uint64_t segment, const n8ro::sim::MessageSchema& schema);

// The roster's transitions (BTB-EP-3). `occupancy` is the tenure the record opens or closes,
// and on entity_remove `reason` is whatever the platform sent - including a supplier-specific
// value this build has never seen, because coercing an unrecognised reason destroys the only
// evidence that something new happened.
[[nodiscard]] std::string writeEntityAdd(
    double simTimeS, std::uint64_t segment, const std::string& entity, std::uint64_t occupancy);

[[nodiscard]] std::string writeEntityRemove(
    double simTimeS, std::uint64_t segment, const std::string& entity, std::uint64_t occupancy,
    const std::string& reason);

[[nodiscard]] std::string writeTrailer(
    double simTimeS, const std::string& endReason, const TrailerCounts& counts,
    const TrailerDrops& drops, const TrailerBusMetrics& busMetrics);

// Which of a schema's declared fields the run ever actually published.
//
// M4 answered this by scanning the whole buffer at the end. M5 streams, so there is no
// buffer to scan - presence is accumulated as records go past instead. The answer matters:
// the entity-state schema declares twelve fields and `activeAnimation` appeared zero times
// in 132 188 samples (notes.md, M3), which is the case BTB-CAP-4's absent-not-defaulted rule
// exists for. Reported at the end of a run so a never-published field is stated rather than
// left to be noticed.
//
// Called from the writer thread only.
class FieldPresence {
public:
    explicit FieldPresence(const n8ro::sim::MessageSchema& schema);

    void note(const n8ro::sim::StreamValueMap& values);

    // Declared field names, in schema order, that `note` never saw.
    [[nodiscard]] std::vector<std::string> neverPublished() const;

private:
    std::vector<std::string> names_;   // schema order
    std::vector<bool> seen_;
    std::size_t remaining_ = 0;        // unseen count, so a settled run stops looking
};

}  // namespace n8ro::bridge::capture
