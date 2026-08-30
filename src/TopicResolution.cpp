#include "TopicResolution.h"

#include "ExitCodes.h"

#include <DbModel.h>
#include <config/EventConfigReader.h>
#include <core/logging/GlobalLogger.h>
#include <messaging/EventNames.h>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace n8ro::bridge {
namespace {

constexpr const char* kCategory = "n8ro-bridge";

// The order M1 derived twice independently - by decoding a captured packed payload, and
// from n8ro-sim-local's own per-entity JSONL, which writes in schema order rather than
// alphabetically (notes.md, "It is independent confirmation of the field order"). Used
// only to report agreement or disagreement. Nothing in the capture path reads it: field
// order comes from MessageSchema::fields, always.
const std::vector<std::string>& notesDerivedFieldOrder() {
    static const std::vector<std::string> order = {
        "simulationTime", "scenarioEntityName", "name",             "team",
        "phase",          "health",             "presence",         "conditions",
        "positionGeodetic", "orientationYprRad", "velocityNed"};
    return order;
}

// The fields the entity picture keys on. An entity-state schema that does not declare both
// cannot be rostered or stamped, so resolving to one is a configuration fault rather than a
// runtime surprise - it is caught here rather than as a silent stream of orphaned samples.
constexpr const char* kRequiredNameField = "scenarioEntityName";
constexpr const char* kRequiredTimeField = "simulationTime";

// The fields an engine-state message must declare to be usable as a heartbeat. Nothing reads
// their values - arrival is the whole signal - but a message that declares neither is not the
// engine-state message, and keying host-loss detection on the wrong topic would produce a
// bridge that looks healthy and never notices a dead host.
constexpr const char* kEngineStateField = "state";
constexpr const char* kEngineFrameField = "frameNumber";

[[nodiscard]] bool schemaDeclares(const n8ro::sim::MessageSchema& schema, const char* field) {
    return std::any_of(schema.fields.begin(), schema.fields.end(),
                       [field](const n8ro::sim::FieldSchema& f) { return f.name == field; });
}

[[nodiscard]] std::string joinNames(const std::vector<std::string>& names) {
    std::string out;
    for (const std::string& name : names) {
        if (!out.empty()) {
            out += ", ";
        }
        out += name;
    }
    return out;
}

// A short sample of what the registry does hold, so a mistyped message name is diagnosed
// against reality instead of leaving the operator to guess. Sorted, so the list reads the
// same on every run.
[[nodiscard]] std::string sampleRegisteredNames(
    const n8ro::sim::MessageBusPackedSchemaRegistry& registry, std::size_t limit) {
    std::vector<std::string> names;
    names.reserve(registry.all().size());
    for (const n8ro::sim::MessageSchema& schema : registry.all()) {
        names.push_back(schema.messageName);
    }
    std::sort(names.begin(), names.end());

    const bool truncated = names.size() > limit;
    const std::size_t extra = truncated ? names.size() - limit : 0;
    if (truncated) {
        names.resize(limit);
    }

    std::string out = joinNames(names);
    if (truncated) {
        out += ", ... (" + std::to_string(extra) + " more)";
    }
    return out;
}

[[nodiscard]] std::string fieldListToString(const n8ro::sim::MessageSchema& schema) {
    std::string out;
    for (std::size_t i = 0; i < schema.fields.size(); ++i) {
        if (i != 0) {
            out += ", ";
        }
        const n8ro::sim::FieldSchema& field = schema.fields[i];
        out += field.name;
        out += ':';
        out += std::string(n8ro::sim::toString(field.type));
        if (field.size != 1) {
            out += '[' + std::to_string(field.size) + ']';
        }
    }
    return out;
}

// Resolves one event name to the topic its envelope travels on, through the database
// pairing rather than a literal. Returns nullopt and logs on any break in the chain.
[[nodiscard]] std::optional<n8ro::sim::MessageSchema> resolveEventTopic(
    const n8ro::data::config::EventVocabulary& vocabulary,
    const n8ro::sim::MessageBusPackedSchemaRegistry& registry,
    std::string_view eventName) {
    const auto entry = vocabulary.byName.find(std::string(eventName));
    if (entry == vocabulary.byName.end()) {
        N8RO_LOG_ERROR(std::string("event vocabulary declares no event named ") +
                           std::string(eventName) +
                           "; the roster cannot be built without it (BTB-EP-3)",
                       kCategory);
        return std::nullopt;
    }

    // EventConfigData::topic names the *Message instance* whose envelope carries the event,
    // not the topic string. The topic string comes from that message's schema.
    const std::string& messageName = entry->second.topic;
    if (messageName.empty()) {
        N8RO_LOG_ERROR(std::string("event ") + std::string(eventName) +
                           " names no message instance, so its topic cannot be resolved",
                       kCategory);
        return std::nullopt;
    }

    const n8ro::sim::MessageSchema* schema = registry.getByName(messageName);
    if (schema == nullptr) {
        N8RO_LOG_ERROR(std::string("event ") + std::string(eventName) + " names message " +
                           messageName + ", which has no registered packed schema",
                       kCategory);
        return std::nullopt;
    }
    if (schema->topic.empty()) {
        N8RO_LOG_ERROR(std::string("message ") + messageName + " declares an empty topic",
                       kCategory);
        return std::nullopt;
    }
    return *schema;
}

}  // namespace

void reportFieldOrder(const n8ro::sim::MessageSchema& entityState) {
    N8RO_LOG_INFO(std::string("entity-state schema: name=") + entityState.messageName +
                      " topic=" + entityState.topic +
                      " fields=" + std::to_string(entityState.fields.size()) +
                      " schemaHash=" + std::to_string(entityState.schemaHash) +
                      " messageId=" + std::to_string(entityState.messageId) +
                      " wireVersion=" + std::to_string(entityState.wireVersion),
                  kCategory);
    N8RO_LOG_INFO(std::string("entity-state field order (runtime MessageSchema::fields, the "
                              "authority for BTB-CAP-3): ") +
                      fieldListToString(entityState),
                  kCategory);

    std::vector<std::string> runtimeOrder;
    runtimeOrder.reserve(entityState.fields.size());
    for (const n8ro::sim::FieldSchema& field : entityState.fields) {
        runtimeOrder.push_back(field.name);
    }

    if (runtimeOrder == notesDerivedFieldOrder()) {
        N8RO_LOG_INFO(std::string("field order agrees with both M1 derivations - the decoded "
                                  "packed payload and n8ro-sim-local's own JSONL (notes.md). "
                                  "Three independent derivations agree"),
                      kCategory);
        return;
    }

    // Not fatal. The runtime schema wins by definition, and a disagreement is a finding for
    // the notes deliverable rather than a reason to refuse to run.
    N8RO_LOG_WARNING(std::string("field order DISAGREES with the order M1 derived in notes.md. "
                                 "The runtime schema is authoritative and is what the capture "
                                 "will use; notes.md needs correcting. notes.md order: ") +
                         joinNames(notesDerivedFieldOrder()),
                     kCategory);
}

Resolution resolveTopics(
    n8ro::schema::DbModel& model,
    n8ro::sim::MessageBusPackedSchemaRegistry& registry,
    const std::string& entityStateMessageName,
    const std::string& engineStateMessageName,
    const std::string& modelPath,
    const std::string& schemaFile) {
    Resolution result;

    if (!registry.loadAllFromDb(model)) {
        N8RO_LOG_ERROR(std::string("MessageBusPackedSchemaRegistry::loadAllFromDb failed; no "
                                   "packed schema was registered, so every packed message on "
                                   "the bus would be dropped undecoded"),
                       kCategory);
        N8RO_LOG_ERROR(std::string("  --model-path  = ") + modelPath, kCategory);
        N8RO_LOG_ERROR(std::string("  --schema-file = ") + schemaFile, kCategory);
        result.exitCode = kExitSchemaLoadFailed;
        return result;
    }

    N8RO_LOG_INFO(std::string("packed schema registry loaded: ") +
                      std::to_string(registry.size()) + " message schemas from modelPath=" +
                      modelPath + " schemaFile=" + schemaFile,
                  kCategory);

    // BTB-EP-1's loud empty registry. An empty registry is the shape a schema mismatch
    // takes, and it is the one failure that otherwise produces a plausible empty capture.
    if (registry.empty()) {
        N8RO_LOG_ERROR(std::string("packed schema registry is EMPTY. Nothing on the bus can be "
                                   "decoded, and capturing would produce a plausible but empty "
                                   "file. Refusing to proceed to silent operation (BTB-EP-1)"),
                       kCategory);
        N8RO_LOG_ERROR(std::string("  --model-path  = ") + modelPath, kCategory);
        N8RO_LOG_ERROR(std::string("  --schema-file = ") + schemaFile, kCategory);
        N8RO_LOG_ERROR(std::string("check that the model path is the directory holding the "
                                   "schema database, and that the schema file is the one the "
                                   "engine itself was started with - a schema registered on "
                                   "only one side drops messages with a warning, not an error"),
                       kCategory);
        result.exitCode = kExitRegistryEmpty;
        return result;
    }

    // --- entity state -------------------------------------------------------------
    const n8ro::sim::MessageSchema* stateSchema = registry.getByName(entityStateMessageName);
    if (stateSchema == nullptr) {
        N8RO_LOG_ERROR(std::string("no packed schema registered under message name ") +
                           entityStateMessageName +
                           "; the entity-state topic cannot be resolved (BTB-EP-1). Override "
                           "the name with --entity-state-message",
                       kCategory);
        N8RO_LOG_ERROR(std::string("registry holds: ") + sampleRegisteredNames(registry, 12),
                       kCategory);
        result.exitCode = kExitEntityStateUnresolved;
        return result;
    }
    if (stateSchema->topic.empty()) {
        N8RO_LOG_ERROR(std::string("message ") + entityStateMessageName +
                           " resolved but declares an empty topic; nothing to subscribe to",
                       kCategory);
        result.exitCode = kExitEntityStateUnresolved;
        return result;
    }

    // The structural check. Anchoring on a message name is only safe if the thing it
    // resolves to is actually shaped like entity state - otherwise a plausible name on a
    // neighbouring message (simEntityTrackUpdate, simEntityPoseUpdate) would subscribe
    // successfully and roster nothing.
    const bool hasName = schemaDeclares(*stateSchema, kRequiredNameField);
    const bool hasTime = schemaDeclares(*stateSchema, kRequiredTimeField);
    if (!hasName || !hasTime) {
        std::string missing;
        if (!hasName) {
            missing += std::string(" missing ") + kRequiredNameField;
        }
        if (!hasTime) {
            missing += std::string(" missing ") + kRequiredTimeField;
        }
        N8RO_LOG_ERROR(std::string("message ") + entityStateMessageName + " resolves to topic " +
                           stateSchema->topic +
                           " but does not declare the fields the entity picture keys on:" +
                           missing +
                           ". This is not the entity-state message; refusing to subscribe to a "
                           "topic that would roster nothing",
                       kCategory);
        N8RO_LOG_ERROR(std::string("  declared fields: ") + fieldListToString(*stateSchema),
                       kCategory);
        result.exitCode = kExitEntityStateShapeWrong;
        return result;
    }

    // Round-trip the topic back through the registry's own topic index. If the two indexes
    // disagree the registry is inconsistent, and the subscription would be aimed at a topic
    // the decoder cannot then match a schema to.
    const n8ro::sim::MessageSchema* byTopic = registry.getByTopic(stateSchema->topic);
    if (byTopic == nullptr || byTopic->messageName != stateSchema->messageName) {
        N8RO_LOG_ERROR(std::string("registry is inconsistent: message ") + entityStateMessageName +
                           " declares topic " + stateSchema->topic +
                           ", but that topic does not resolve back to the same message",
                       kCategory);
        result.exitCode = kExitEntityStateUnresolved;
        return result;
    }

    result.entityState = *stateSchema;   // copy; the registry's snapshot may be replaced

    N8RO_LOG_INFO(std::string("resolved entity-state topic ") + result.entityState.topic +
                      " from message " + result.entityState.messageName +
                      " via the registry - not from a literal (BTB-EP-1)",
                  kCategory);
    reportFieldOrder(result.entityState);

    // --- entity events ------------------------------------------------------------
    const std::optional<n8ro::data::config::EventVocabulary> vocabulary =
        n8ro::data::config::EventConfigReader::readVocabularyFromModel(model);
    if (!vocabulary) {
        N8RO_LOG_ERROR(std::string("EventConfigReader::readVocabularyFromModel failed; the Event "
                                   "type is not registered in this schema, so the entity-event "
                                   "topic cannot be resolved and no roster can be built "
                                   "(BTB-EP-3)"),
                       kCategory);
        result.exitCode = kExitEntityEventUnresolved;
        return result;
    }

    const std::optional<n8ro::sim::MessageSchema> createSchema =
        resolveEventTopic(*vocabulary, registry, n8ro::sim::kEventEntityCreated);
    const std::optional<n8ro::sim::MessageSchema> deleteSchema =
        resolveEventTopic(*vocabulary, registry, n8ro::sim::kEventEntityDeleted);
    if (!createSchema || !deleteSchema) {
        result.exitCode = kExitEntityEventUnresolved;
        return result;
    }

    // Both halves of the roster lifecycle must arrive on one topic, or one subscription
    // cannot see the whole of it.
    if (createSchema->topic != deleteSchema->topic) {
        N8RO_LOG_ERROR(std::string("entity_created travels on ") + createSchema->topic +
                           " but entity_deleted on " + deleteSchema->topic +
                           "; a single roster subscription cannot see both halves of the "
                           "lifecycle. M3 subscribes to one entity-event topic only",
                       kCategory);
        result.exitCode = kExitEntityEventUnresolved;
        return result;
    }

    result.entityEvent = *createSchema;

    N8RO_LOG_INFO(std::string("resolved entity-event topic ") + result.entityEvent.topic +
                      " from the database pairing for " +
                      std::string(n8ro::sim::kEventEntityCreated) + " / " +
                      std::string(n8ro::sim::kEventEntityDeleted) + " via message " +
                      result.entityEvent.messageName + " - not from a literal (BTB-EP-1)",
                  kCategory);
    N8RO_LOG_INFO(std::string("entity-event field order: ") + fieldListToString(result.entityEvent),
                  kCategory);

    // --- scenario events (M5, BTB-CX-4) -------------------------------------------
    // The segment source. Same two-hop chain as the entity events, and for the same reason:
    // EventConfigData::topic names a Message instance, not a topic string.
    const std::optional<n8ro::sim::MessageSchema> loadedSchema =
        resolveEventTopic(*vocabulary, registry, n8ro::sim::kEventScenarioLoaded);
    const std::optional<n8ro::sim::MessageSchema> unloadedSchema =
        resolveEventTopic(*vocabulary, registry, n8ro::sim::kEventScenarioUnloaded);
    if (!loadedSchema || !unloadedSchema) {
        N8RO_LOG_ERROR(std::string("the scenario-event topic could not be resolved, so scenario "
                                   "load and reload could not be told apart and two runs would "
                                   "be silently mixed in one capture (BTB-CX-4)"),
                       kCategory);
        result.exitCode = kExitScenarioEventUnresolved;
        return result;
    }
    if (loadedSchema->topic != unloadedSchema->topic) {
        N8RO_LOG_ERROR(std::string("scenario_loaded travels on ") + loadedSchema->topic +
                           " but scenario_unloaded on " + unloadedSchema->topic +
                           "; one subscription cannot see both halves of a segment boundary",
                       kCategory);
        result.exitCode = kExitScenarioEventUnresolved;
        return result;
    }
    result.scenarioEvent = *loadedSchema;

    N8RO_LOG_INFO(std::string("resolved scenario-event topic ") + result.scenarioEvent.topic +
                      " from the database pairing for " +
                      std::string(n8ro::sim::kEventScenarioLoaded) + " / " +
                      std::string(n8ro::sim::kEventScenarioUnloaded) + " via message " +
                      result.scenarioEvent.messageName + " - not from a literal (BTB-EP-1)",
                  kCategory);

    // --- engine state (M5, BTB-CX-3) ----------------------------------------------
    // The heartbeat. Its silence is what host loss looks like; entity-state silence is not,
    // because entity state stops legitimately at every unload. Measured across two full
    // cycles: engine state publishes at ~19.5/s through idle frames, largest observed gap
    // 548 ms at scenario load (docs/decisions-m5-m7.md, D-3).
    const n8ro::sim::MessageSchema* engineSchema = registry.getByName(engineStateMessageName);
    if (engineSchema == nullptr || engineSchema->topic.empty()) {
        N8RO_LOG_ERROR(std::string("no packed schema registered under message name ") +
                           engineStateMessageName +
                           ", or it declares an empty topic; the engine-state heartbeat cannot "
                           "be resolved and host loss could not be detected. A bridge that "
                           "cannot detect host loss blocks indefinitely on a dead bus, which is "
                           "the failure BTB-CX-3 exists to forbid - refusing to run rather than "
                           "running without it. Override the name with --engine-state-message",
                       kCategory);
        N8RO_LOG_ERROR(std::string("registry holds: ") + sampleRegisteredNames(registry, 12),
                       kCategory);
        result.exitCode = kExitEngineStateUnresolved;
        return result;
    }

    // The same structural guard the entity-state chain uses. A plausible neighbouring name
    // would subscribe successfully and heartbeat on the wrong traffic, which is worse than
    // not resolving at all: the failure would look like a working bridge.
    const bool hasState = schemaDeclares(*engineSchema, kEngineStateField);
    const bool hasFrame = schemaDeclares(*engineSchema, kEngineFrameField);
    if (!hasState || !hasFrame) {
        std::string missing;
        if (!hasState) {
            missing += std::string(" missing ") + kEngineStateField;
        }
        if (!hasFrame) {
            missing += std::string(" missing ") + kEngineFrameField;
        }
        N8RO_LOG_ERROR(std::string("message ") + engineStateMessageName + " resolves to topic " +
                           engineSchema->topic +
                           " but does not declare the fields an engine-state heartbeat needs:" +
                           missing +
                           ". This is not the engine-state message; refusing to key host-loss "
                           "detection on a topic that means something else",
                       kCategory);
        N8RO_LOG_ERROR(std::string("  declared fields: ") + fieldListToString(*engineSchema),
                       kCategory);
        result.exitCode = kExitEngineStateUnresolved;
        return result;
    }
    result.engineState = *engineSchema;

    N8RO_LOG_INFO(std::string("resolved engine-state topic ") + result.engineState.topic +
                      " from message " + result.engineState.messageName +
                      " via the registry - the host-loss heartbeat (BTB-CX-3)",
                  kCategory);

    result.ok = true;
    return result;
}

}  // namespace n8ro::bridge
