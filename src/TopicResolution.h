// EXT-08 Bus Telemetry Bridge - M3: schema registration and topic resolution (BTB-EP-1).
//
// Every topic string this program subscribes to is read out of the model database at
// runtime. None is hand-written (derive from the schema; PRD tie-breaker 4): a literal is how a working
// program becomes a silently-empty one after an upgrade, and a schema mismatch drops
// messages with a warning rather than an error, so the failure looks exactly like success.
//
// Two resolution chains, both anchored on something the compiler or the database checks:
//
//   entity state  --entity-state-message (default simEntityStateUpdate)
//                   -> registry.getByName() -> MessageSchema::topic
//                   -> structural check that the schema declares the fields we key on
//
//   entity event  kEventEntityCreated / kEventEntityDeleted  (EventNames.h constants,
//                   compile-checked against the engine's own publish sites)
//                   -> EventConfigReader vocabulary -> message instance name
//                   -> registry.getByName() -> MessageSchema::topic
//
//   scenario evt  kEventScenarioLoaded / kEventScenarioUnloaded  (same chain) - M5's
//                   segment boundaries (BTB-CX-4). Never a topic literal, for the same
//                   reason as the other two.
//
//   engine state  --engine-state-message (default simEngineState)
//                   -> registry.getByName() -> MessageSchema::topic
//                   -> structural check that it declares the fields a heartbeat needs
//                 M5's host-loss signal (BTB-CX-3). Entity state cannot serve: it goes
//                 silent legitimately at every unload, so silence there means nothing.
//                 Engine state publishes through idle frames - 4 017 messages across a
//                 200 s reference run - so its silence is evidence.
//
// EventNames.h states the rule the second chain follows: "The topic each event travels on
// is the Event instance's own `topic` field, not a constant here: a consumer reads the
// pairing from the database, and a second copy in a header would drift from it."

#pragma once

#include <messaging/packed/MessageBusPackedSchemaRegistry.h>
#include <messaging/packed/MessageSchema.h>

#include <string>

namespace n8ro::schema {
class DbModel;
}

namespace n8ro::bridge {

// The outcome of resolution. `ok` is false for every failure; the caller maps `exitCode`
// straight to its return value so a script can tell the faults apart without scraping the
// log. Every failure has already been logged by the time this returns.
struct Resolution {
    bool ok = false;
    int exitCode = 0;
    n8ro::sim::MessageSchema entityState;   // copied out of the registry, not borrowed
    n8ro::sim::MessageSchema entityEvent;
    n8ro::sim::MessageSchema scenarioEvent;
    n8ro::sim::MessageSchema engineState;
};

// Loads every packed schema in the model into `registry`, then resolves both topics.
// Logs the registry size and both resolved topics. An empty registry, an unresolvable
// topic, or an entity-state schema that does not declare the fields we key on is a named
// diagnostic and a non-zero exit - never silent operation.
[[nodiscard]] Resolution resolveTopics(
    n8ro::schema::DbModel& model,
    n8ro::sim::MessageBusPackedSchemaRegistry& registry,
    const std::string& entityStateMessageName,
    const std::string& engineStateMessageName,
    const std::string& modelPath,
    const std::string& schemaFile);

// Logs the entity-state field order as the runtime schema declares it, and reports whether
// it agrees with the order M1 derived independently twice - once by decoding the packed
// bytes, once from n8ro-sim-local's own JSONL serialiser (notes.md). The runtime schema is
// the authority either way; this is a check on the notes, not a dependency of the code.
void reportFieldOrder(const n8ro::sim::MessageSchema& entityState);

}  // namespace n8ro::bridge
