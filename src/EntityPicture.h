// EXT-08 Bus Telemetry Bridge - M3: the entity picture.
//
// The roster (BTB-EP-3) and the latest-sample map (BTB-EP-4). The SDK ships neither -
// SimulationEngineClient has no roster accessor and no per-entity sample cache, and
// EntityStateSample.h does not exist in 2.1.328 - so this is the layer we build.
//
// Threading: every public method takes the same mutex. The bus calls onSample() and
// onEntityEvent() from the pump thread; our own thread calls snapshot() and
// drainEvents(). Nothing here does IO, parsing or formatting - a handler is a courier
// (the courier rule; PRD tie-breaker 5), so it copies, hands off, and returns.
//
// Ordering: every container iterated here is ordered (BTB-EP-4 — order is meaning).
// StreamValueMap is an std::unordered_map and is stored verbatim, but it is only ever
// *looked up* by name, never iterated - field order comes from MessageSchema::fields.

#pragma once

#include <messaging/packed/MessageBusPacked.h>
#include <messaging/packed/StreamValue.h>

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace n8ro::bridge {

// One entity's tenure in the roster under a single name.
//
// The generation is the answer to the ambiguity M1 found: the engine's stop path deletes
// every entity with reason="scenario_unload" and then immediately re-creates it under the
// same name, so samples resume. A name is therefore not a unique identity across a run -
// a (name, generation) pair is. BTB-EP-3's "no sample after the removal" holds within an
// occupancy, which is exactly satisfiable; read across occupancies it is not. ADR-6.
//
// Vocabulary, for M4: this struct is one occupancy, and `generation` is its ordinal. The
// capture's `entity_add` / `entity_remove` / `sample` records spell that ordinal
// `occupancy` (docs/prd.md, "Data model"). Same number, and the only translation between
// the picture and the file.
struct Occupancy {
    std::uint64_t generation = 0;    // 1 for the first tenure of this name, then 2, 3, ...
    bool open = false;               // created and not yet deleted
    std::string profileName;         // from entity_created
    std::string teamName;            // from entity_created
    std::string lastRemovalReason;   // verbatim, including a value outside the engine's set
    double createdSimTimeS = 0.0;
    double removedSimTimeS = 0.0;
};

// The most recent published sample for one entity, kept verbatim.
//
// `values` is the decoded StreamValueMap exactly as delivered. Nothing is mapped onto a
// curated struct: BTB-CAP-4's verbatim rule is what makes a schema that gains a field
// carry it through with no code change, and what keeps the "what the stream contained
// that we did not expect" deliverable writable.
struct LatestSample {
    double simulationTimeS = 0.0;    // the sample's own clock - the only clock (tenet 2)
    std::uint64_t generation = 0;    // the occupancy this sample belongs to
    n8ro::sim::StreamValueMap values;
};

// What onSample() decided, handed back so the caller can record the sample without
// re-deriving the roster lookup the picture just did. This is the whole coupling between
// the entity picture and the capture: the picture never learns that a capture exists, and
// the capture path never touches the roster.
//
// `accepted` false means the sample did not enter the map - it was unnamed, untimed, or
// orphaned - and the counter that says which has already been incremented. An accepted
// sample carries the (name, occupancy) pair the capture keys on (ADR-6).
struct SampleOutcome {
    bool accepted = false;
    std::string scenarioEntityName;
    std::uint64_t generation = 0;
    double simulationTimeS = 0.0;
};

// What onEntityEvent() decided, handed back so the caller can write the matching
// entity_add / entity_remove record without re-deriving the roster lookup the picture just
// did - the same coupling shape as SampleOutcome, and for the same reason.
//
// It has to be a return value rather than something our own thread drains later. The
// capture's records must be in true arrival order relative to the sample stream (BTB-BP-2):
// an entity_add that reached the file after the samples it opens would produce a file the
// format spec calls malformed. Draining a log on another thread cannot preserve that.
struct EventOutcome {
    enum class Kind { Ignored, Added, Removed };

    Kind kind = Kind::Ignored;
    std::string scenarioEntityName;
    std::uint64_t generation = 0;
    double simulationTimeS = 0.0;
    std::string reason;   // Removed only; verbatim, including a value outside the engine's set
};

// A roster transition, queued for our own thread to log. The handler must not format or
// write, so it pushes one of these and returns.
struct RosterEvent {
    std::string eventName;           // verbatim from the payload
    std::string scenarioEntityName;
    std::string reason;              // verbatim; empty on create
    double simulationTimeS = 0.0;
    std::uint64_t generation = 0;
};

// Counted losses and surprises. Nothing here is ever silent (tenet 3).
struct PictureCounters {
    std::uint64_t samplesAccepted = 0;
    std::uint64_t samplesOrphaned = 0;       // a sample whose entity has no open occupancy

    // How many samples had already been orphaned when the *first* sample was accepted.
    //
    // This is the late-attach signature, stated causally rather than by a clock. A bridge
    // present at scenario load witnesses the entity_created burst first, so its first
    // accepted sample arrives with this at zero. A bridge that attached after the burst sees
    // samples for entities it never saw created - every one orphaned - until the engine next
    // creates something, so its first accepted sample arrives with this non-zero. M3
    // measured that case: 7 740 orphans, zero drops, and no error anywhere.
    //
    // It exists so the capture's `attached_mid_run` can be derived from what happened rather
    // than from what a status tick happened to observe a second after start-up, which is a
    // race and would put a scheduler-dependent value in a file that must be byte-reproducible
    // (BTB-CAP-3). Frozen at the first acceptance and never updated again.
    std::uint64_t orphansBeforeFirstAccepted = 0;
    std::uint64_t samplesUnnamed = 0;        // no scenarioEntityName field in the payload
    std::uint64_t samplesUntimed = 0;        // no simulationTime field in the payload
    std::uint64_t entityCreated = 0;
    std::uint64_t entityDeleted = 0;
    std::uint64_t deleteOfUnknownEntity = 0; // entity_deleted for a name never created
    // entity_deleted for a name whose occupancy is ALREADY closed - a repeated delete with no
    // entity_created between the two. Counted and ignored rather than acted on: acting would
    // emit a second `entity_remove` for one occupancy, and docs/capture-format-v1.md section
    // 8.1 has a reader bracket an occupancy by "an `entity_add` and the matching
    // `entity_remove`", singular. Never observed on runtime 2.1.328, which is why it is a
    // counter and not a warning - if it ever moves, the stream did something the roster model
    // did not anticipate and notes.md should say so.
    std::uint64_t deleteOfClosedOccupancy = 0;
    std::uint64_t eventsUnnamed = 0;         // no eventName field in the payload
    std::uint64_t eventsWithoutEntity = 0;   // eventName present, scenarioEntityName absent
    std::uint64_t eventQueueDropped = 0;     // roster-event log overflowed (bounded, counted)
};

// What the referee and the reporter read. Internally consistent by construction: it is
// built under the lock in one pass, so no entry is observed mid-write (BTB-EP-4).
//
// Retention invariant: `latest` keeps the final sample of a *closed* occupancy. That is
// deliberate - "where was it when it died" is a question the referee will ask, and erasing
// on removal would make it unanswerable. It also means `latest` alone is not a liveness
// answer, so use liveSample() / isLive() rather than joining the two maps by hand. The
// brief's "nothing lingers in the output after a body is gone" is about the capture, and
// the capture stops emitting samples for a closed occupancy by construction.
struct PictureSnapshot {
    std::map<std::string, Occupancy> roster;
    std::map<std::string, LatestSample> latest;
    std::map<std::string, std::uint64_t> removalsByReason;   // verbatim reason -> count
    std::map<std::string, std::uint64_t> unhandledEventNames; // e.g. entity_updated
    PictureCounters counters;
    std::size_t liveCount = 0;    // occupancies currently open

    // True while `name` has an open occupancy. False for a name never seen, and for one
    // whose body is gone.
    [[nodiscard]] bool isLive(const std::string& name) const;

    // The latest sample for `name`, or nullptr if it has no open occupancy. This is the
    // accessor a condition should use: it makes the safe reading the easy one, so a
    // removed entity can never be evaluated as though it were still on the field.
    [[nodiscard]] const LatestSample* liveSample(const std::string& name) const;

    // The latest sample regardless of liveness, for a caller that genuinely wants the last
    // known state of something that has been removed. Named so that using it is a choice.
    [[nodiscard]] const LatestSample* lastKnownSample(const std::string& name) const;
};

class EntityPicture {
public:
    // The bounded roster-event log. 134 events was a whole run of the reference scenario,
    // so this is three orders of magnitude of headroom; overflow is counted, not silent.
    static constexpr std::size_t kEventLogCapacity = 4096;

    // Called from the bus pump thread. Copies, updates, returns - no IO, no formatting.
    //
    // The return value is not [[nodiscard]] on purpose: a caller that only wants the
    // picture maintained, which is every caller before M4, ignores it and reads correctly.
    SampleOutcome onSample(const n8ro::sim::StreamValueMap& values);

    // Like onSample, the return value is not [[nodiscard]]: a caller that only wants the
    // roster maintained ignores it and is still correct.
    EventOutcome onEntityEvent(const n8ro::sim::StreamValueMap& values);

    // Called from our own thread.
    [[nodiscard]] PictureSnapshot snapshot() const;
    [[nodiscard]] std::vector<RosterEvent> drainEvents();
    [[nodiscard]] std::size_t liveCount() const;

private:
    void pushEventLocked(RosterEvent event);

    mutable std::mutex mutex_;
    std::map<std::string, Occupancy> roster_;
    std::map<std::string, LatestSample> latest_;
    std::map<std::string, std::uint64_t> removalsByReason_;
    std::map<std::string, std::uint64_t> unhandledEventNames_;
    std::deque<RosterEvent> eventLog_;
    PictureCounters counters_;
};

// Field readers over a decoded payload. A packed payload carries only the fields the
// publisher wrote, so presence is read rather than assumed - these never throw and never
// dereference a missing or wrongly-typed field.
[[nodiscard]] std::optional<std::string> tryReadString(
    const n8ro::sim::StreamValueMap& values, const std::string& field);
[[nodiscard]] std::optional<double> tryReadDouble(
    const n8ro::sim::StreamValueMap& values, const std::string& field);

}  // namespace n8ro::bridge
