// EXT-08 Bus Telemetry Bridge - M5: what crosses the handler-to-writer boundary.
//
// One type for everything a handler hands off, because the writer's segment state machine
// has to see every kind of record in true arrival order to place it correctly. M5's
// observation of the platform is the reason: the `entity_created` burst that materialises a
// scenario arrives *before* the `scenario_loaded` that announces it, at bring-up and at
// every reload, so a design that routed roster events and samples through separate paths
// could not know which segment a creation burst belonged to. See docs/decisions-m5-m7.md,
// D-1 and D-7.
//
// A record is copied whole on the pump thread and never touched again until the writer
// serialises it. That is the courier rule (CLAUDE.md hard rule 2): copy, hand off, return.

#pragma once

// StreamValueMap is an alias declared in MessageBusPacked.h, not in StreamValue.h - the
// value type and the map over it live in different headers.
#include <messaging/packed/MessageBusPacked.h>
#include <messaging/packed/StreamValue.h>

#include <cstdint>
#include <string>

namespace n8ro::bridge {

enum class RecordKind {
    Sample,             // an accepted entity-state sample, verbatim
    EntityAdd,          // entity_created - opens an occupancy
    EntityRemove,       // entity_deleted - closes one, reason verbatim
    ScenarioLoaded,     // opens a segment
    ScenarioUnloaded,   // closes one
};

// Everything the writer needs to emit one line, with no second look at the entity picture
// and no lookup that could race with the pump thread.
//
// `values` is populated for Sample only. It is the decoded StreamValueMap exactly as the bus
// delivered it - nothing is projected onto a curated struct, because BTB-CAP-4's verbatim
// rule is what makes a schema that gains a field carry it through with no code change.
struct CaptureRecord {
    RecordKind kind = RecordKind::Sample;

    // Sample / EntityAdd / EntityRemove: the scenario entity name.
    // ScenarioLoaded / ScenarioUnloaded: the scenario name the event carried.
    std::string subject;

    // Occupancy::generation, spelled `occupancy` in the file. Zero for scenario records.
    std::uint64_t occupancy = 0;

    // The sample's or event's own clock, and the only clock anything durable may carry
    // (ADR-3). Note that the engine resets this to 0.0 before publishing its teardown
    // events, so a record that ends a 200-second run is stamped 0.0 - see the format spec
    // section 5.1, which now says so.
    double simTimeS = 0.0;

    // EntityRemove only. Verbatim, including a value outside the engine's own set.
    std::string reason;

    // Sample only.
    n8ro::sim::StreamValueMap values;
};

// True for the kinds the queue protects with reserved headroom (D-8). These are the single,
// unrepeatable messages: losing one does not lose a data point, it loses the structure that
// makes every later data point interpretable.
[[nodiscard]] inline bool isStructuralRecord(RecordKind kind) {
    return kind != RecordKind::Sample;
}

}  // namespace n8ro::bridge
