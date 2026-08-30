#include "CaptureFormat.h"

#include "Json.h"

#include <algorithm>
#include <type_traits>
#include <variant>

namespace n8ro::bridge::capture {
namespace {

void appendKey(std::string& out, const std::string& key) {
    if (out.back() != '{' && out.back() != '[') {
        out.push_back(',');
    }
    json::appendString(out, key);
    out.push_back(':');
}

void appendStringMember(std::string& out, const std::string& key, const std::string& value) {
    appendKey(out, key);
    json::appendString(out, value);
}

void appendUintMember(std::string& out, const std::string& key, std::uint64_t value) {
    appendKey(out, key);
    json::appendInt(out, static_cast<std::int64_t>(value));
}

void appendDoubleMember(std::string& out, const std::string& key, double value) {
    appendKey(out, key);
    json::appendDouble(out, value);
}

void appendBoolMember(std::string& out, const std::string& key, bool value) {
    appendKey(out, key);
    json::appendBool(out, value);
}

// The record envelope opens the same way for every record type, so a reader can classify a
// line after two keys.
void openRecord(std::string& out, const std::string& type) {
    out.push_back('{');
    appendStringMember(out, "type", type);
}

// One decoded value, encoded from the alternative the StreamValue actually holds.
//
// The schema's declared type is deliberately not consulted. A field whose wire type differs
// from its declaration is a fact about the run, and the capture's job is to carry it rather
// than correct it - the declaration is in header.schemas for a reader that wants to compare
// the two.
void appendValue(std::string& out, const n8ro::sim::StreamValue& value) {
    std::visit(
        [&out](const auto& held) {
            using Held = std::decay_t<decltype(held)>;
            if constexpr (std::is_same_v<Held, std::int64_t>) {
                json::appendInt(out, held);
            } else if constexpr (std::is_same_v<Held, double>) {
                json::appendDouble(out, held);
            } else if constexpr (std::is_same_v<Held, bool>) {
                json::appendBool(out, held);
            } else if constexpr (std::is_same_v<Held, std::string>) {
                json::appendString(out, held);
            } else {
                // The four vector alternatives. A packed array field keeps the publisher's
                // element order - positionGeodetic is lat, lon, alt and stays that way.
                out.push_back('[');
                bool first = true;
                for (const auto& element : held) {
                    if (!first) {
                        out.push_back(',');
                    }
                    first = false;
                    using Element = std::decay_t<decltype(element)>;
                    if constexpr (std::is_same_v<Element, std::int64_t>) {
                        json::appendInt(out, element);
                    } else if constexpr (std::is_same_v<Element, double>) {
                        json::appendDouble(out, element);
                    } else if constexpr (std::is_same_v<Element, std::string>) {
                        json::appendString(out, element);
                    } else {
                        // std::vector<bool> yields a proxy reference; the cast is what
                        // makes it a bool again rather than the proxy.
                        json::appendBool(out, static_cast<bool>(element));
                    }
                }
                out.push_back(']');
            }
        },
        value.data);
}

void appendSchema(std::string& out, const n8ro::sim::MessageSchema& schema) {
    out.push_back('{');
    appendStringMember(out, "message_name", schema.messageName);
    appendStringMember(out, "topic", schema.topic);
    appendUintMember(out, "schema_hash", schema.schemaHash);
    appendUintMember(out, "message_id", schema.messageId);
    appendUintMember(out, "wire_version", schema.wireVersion);

    appendKey(out, "fields");
    out.push_back('[');
    // MessageSchema::fields is a std::vector, so this is declaration order, and it is the
    // order every sample record's `fields` object follows. Copied from what the runtime
    // delivered - never hand-transcribed (BTB-CAP-1).
    for (const n8ro::sim::FieldSchema& field : schema.fields) {
        if (out.back() != '[') {
            out.push_back(',');
        }
        out.push_back('{');
        appendStringMember(out, "name", field.name);
        appendStringMember(out, "type", std::string(n8ro::sim::toString(field.type)));
        appendUintMember(out, "size", static_cast<std::uint64_t>(field.size));
        out.push_back('}');
    }
    out.push_back(']');
    out.push_back('}');
}

}  // namespace

std::string writeHeader(const HeaderInfo& info) {
    std::string out;
    out.push_back('{');

    // First key of the first record, so an unknown version is rejected before anything else
    // is parsed (BTB-CAP-1). The header carries no sim_time_s: it has no cause on the bus,
    // and inventing one would mean reaching for a clock (ADR-3).
    appendStringMember(out, "format_version", kFormatVersion);

    // `type` comes second, not first. BTB-CAP-1 requires format_version to be the first key
    // so that an unknown version is rejected before anything else is parsed, and that
    // outranks the envelope's usual type-first shape. It is still present, so a reader can
    // dispatch every record - the header included - on one key.
    appendStringMember(out, "type", "header");

    appendKey(out, "producer");
    out.push_back('{');
    appendStringMember(out, "name", kProducerName);
    appendStringMember(out, "version", kProducerVersion);
    out.push_back('}');

    appendKey(out, "platform");
    out.push_back('{');
    appendStringMember(out, "engine_config", info.platform.engineConfig);
    appendStringMember(out, "model_path", info.platform.modelPath);
    appendStringMember(out, "schema_file", info.platform.schemaFile);
    appendStringMember(out, "schema_version", info.platform.schemaVersion);
    appendStringMember(out, "runtime_version", info.platform.runtimeVersion);
    out.push_back('}');

    appendBoolMember(out, "attached_mid_run", info.attachedMidRun);

    appendKey(out, "subscription");
    out.push_back('{');
    appendStringMember(out, "topic", info.subscription.topic);
    appendStringMember(out, "backpressure_policy", info.subscription.backpressurePolicy);
    appendUintMember(out, "queue_size", info.subscription.queueSize);
    out.push_back('}');

    // Sorted by message name. The registry's own order is not specified, and a header whose
    // bytes depend on it would break byte-for-byte comparison of two identical runs on a
    // detail that has nothing to do with the simulation (BTB-CAP-3).
    std::vector<n8ro::sim::MessageSchema> ordered = info.schemas;
    std::sort(ordered.begin(), ordered.end(),
              [](const n8ro::sim::MessageSchema& a, const n8ro::sim::MessageSchema& b) {
                  return a.messageName < b.messageName;
              });

    appendKey(out, "schemas");
    out.push_back('[');
    for (const n8ro::sim::MessageSchema& schema : ordered) {
        if (out.back() != '[') {
            out.push_back(',');
        }
        appendSchema(out, schema);
    }
    out.push_back(']');

    out.push_back('}');
    return out;
}

std::string writeSegmentOpen(double simTimeS, std::uint64_t segment, const std::string& scenario) {
    std::string out;
    openRecord(out, "segment_open");
    appendDoubleMember(out, "sim_time_s", simTimeS);
    appendUintMember(out, "segment", segment);
    appendStringMember(out, "scenario", scenario);
    out.push_back('}');
    return out;
}

std::string writeSegmentClose(double simTimeS, std::uint64_t segment, const std::string& scenario,
                              const std::string& reason) {
    std::string out;
    openRecord(out, "segment_close");
    appendDoubleMember(out, "sim_time_s", simTimeS);
    appendUintMember(out, "segment", segment);
    appendStringMember(out, "scenario", scenario);
    appendStringMember(out, "reason", reason);
    out.push_back('}');
    return out;
}

std::string writeSample(const CapturedSample& sample, std::uint64_t segment,
                        const n8ro::sim::MessageSchema& schema) {
    std::string out;
    openRecord(out, "sample");
    appendDoubleMember(out, "sim_time_s", sample.simulationTimeS);
    appendUintMember(out, "segment", segment);
    appendStringMember(out, "entity", sample.scenarioEntityName);
    appendUintMember(out, "occupancy", sample.occupancy);
    appendStringMember(out, "message", schema.messageName);

    appendKey(out, "fields");
    out.push_back('{');
    for (const n8ro::sim::FieldSchema& field : schema.fields) {
        // A lookup, never an iteration. Walking the schema and looking each field up is what
        // makes the output order the schema's rather than the hash table's, and it is also
        // what makes an absent field absent: a field the publisher did not send has no entry
        // to find, so nothing is written for it and nothing is defaulted in its place
        // (BTB-CAP-4).
        const auto found = sample.values.find(field.name);
        if (found == sample.values.end()) {
            continue;
        }
        appendKey(out, field.name);
        appendValue(out, found->second);
    }
    out.push_back('}');

    out.push_back('}');
    return out;
}

std::string writeTrailer(double simTimeS, const std::string& endReason,
                         const TrailerCounts& counts, const TrailerDrops& drops,
                         const TrailerBusMetrics& busMetrics) {
    std::string out;
    openRecord(out, "trailer");
    appendDoubleMember(out, "sim_time_s", simTimeS);
    // No `segment`: the trailer closes the file, not a segment, and every segment it covered
    // has already been closed by its own record.
    appendStringMember(out, "end_reason", endReason);

    appendKey(out, "counts");
    out.push_back('{');
    appendUintMember(out, "segments", counts.segments);
    appendUintMember(out, "samples", counts.samples);
    appendUintMember(out, "entities_added", counts.entitiesAdded);
    appendUintMember(out, "entities_removed", counts.entitiesRemoved);
    appendUintMember(out, "verdicts", counts.verdicts);
    out.push_back('}');

    appendKey(out, "drops");
    out.push_back('{');
    appendUintMember(out, "samples_not_recorded", drops.samplesNotRecorded);
    appendUintMember(out, "samples_orphaned", drops.samplesOrphaned);
    appendUintMember(out, "samples_unnamed", drops.samplesUnnamed);
    appendUintMember(out, "samples_untimed", drops.samplesUntimed);
    out.push_back('}');

    appendKey(out, "bus_metrics");
    out.push_back('{');
    appendUintMember(out, "schema_hash_drops", busMetrics.schemaHashDrops);
    appendUintMember(out, "message_id_drops", busMetrics.messageIdDrops);
    appendUintMember(out, "decode_failures", busMetrics.decodeFailures);
    appendUintMember(out, "missing_schema_passthrough", busMetrics.missingSchemaPassthrough);
    appendUintMember(out, "legacy_payload_passthrough", busMetrics.legacyPayloadPassthrough);
    // Delivery side. Added at producer 0.4.2; adding keys to an existing record is a
    // non-breaking change under the format's own rule (spec section 13), so this is still
    // n8ro-capture/1 and a 0.4.1 reader ignores them.
    appendUintMember(out, "messages_dropped", busMetrics.messagesDropped);
    appendUintMember(out, "dropped_by_backpressure", busMetrics.droppedByBackpressure);
    appendUintMember(out, "dropped_by_queue_overflow", busMetrics.droppedByQueueOverflow);
    appendUintMember(out, "dropped_by_rate_limiting", busMetrics.droppedByRateLimiting);
    out.push_back('}');

    out.push_back('}');
    return out;
}

std::vector<std::string> neverPublishedFields(const n8ro::sim::MessageSchema& schema,
                                              const std::vector<CapturedSample>& samples) {
    std::vector<std::string> absent;
    for (const n8ro::sim::FieldSchema& field : schema.fields) {
        const bool everSeen =
            std::any_of(samples.begin(), samples.end(), [&field](const CapturedSample& sample) {
                return sample.values.find(field.name) != sample.values.end();
            });
        if (!everSeen) {
            absent.push_back(field.name);
        }
    }
    return absent;
}

}  // namespace n8ro::bridge::capture
