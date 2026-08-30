// EXT-08 Bus Telemetry Bridge - M4: the capture format and its specification.
//
// Registers the packed schemas, resolves the entity-state and entity-event topics from the
// registry, subscribes decoded to both, maintains a roster and a latest-sample map, and -
// new at M4 - records the run into a `n8ro-capture/1` capture file.
//
// Scope: BTB-CAP-1, BTB-CAP-4 and BTB-CAP-5, on top of M2's BTB-CX-1 and M3's BTB-EP-1
// through BTB-EP-4. The normative description of what it writes is
// docs/capture-format-v1.md, which is the artifact EXT-17 is handed.
//
// What M4 deliberately does not have (docs/prd.md, M5-M7): no writer thread and no
// handler-to-writer queue, no event-driven segment boundaries, no entity_add /
// entity_remove records, no referee or verdicts, no --replay, no signal handling, no
// --out-dir. The recording strategy below - fill a bounded buffer on the pump thread, then
// write the whole file from our own thread once recording has stopped - exists so that M4
// can prove the format against a real run without building any of that early. M5 replaces
// it with the streaming writer; capture::write*() survives unchanged, because a record's
// text is a function of the record and of nothing else.

#include "CaptureFormat.h"
#include "EntityPicture.h"
#include "ExitCodes.h"
#include "SampleBuffer.h"
#include "TopicResolution.h"

#include <DbModel.h>
#include <core/logging/GlobalLogger.h>
#include <core/logging/LogReportingFactory.h>
#include <core/messaging/IMessageBus.h>
#include <core/version/BuildInfo.h>
#include <infrastructure/SimulationEngineClient.h>
#include <messaging/packed/MessageBusPacked.h>
#include <messaging/packed/MessageBusPackedSchemaRegistry.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace n8ro::bridge;

constexpr const char* kCategory = "n8ro-bridge";

// The default anchor for entity-state resolution. This is a *message instance name*, not a
// topic: the topic string is always read off the schema this name resolves to, so a topic
// rename in the database needs no rebuild. It is overridable because a message rename
// should be a flag change rather than a rebuild too.
constexpr const char* kDefaultEntityStateMessage = "simEntityStateUpdate";

// The recording budget. It is what ends an M4 run, so it is also what makes the capture
// deterministic: the same scenario recorded to the same budget stops at the same sample,
// with no clock involved in the decision. 100 000 samples is about 150 s of the reference
// scenario at its measured 818 samples/s, and about 200 MB of held StreamValueMaps.
constexpr std::size_t kDefaultCaptureMaxSamples = 100000;

constexpr const char* kUsage =
    "usage: n8ro-bridge --config <SimEngineClient_*> --model-path <dir> --schema-file <name>\n"
    "                   [--entity-state-message <name>]\n"
    "                   [--capture-out <file>] [--capture-max-samples <n>]\n"
    "\n"
    "  --config                 client-side sim engine config entry, e.g.\n"
    "                           SimEngineClient_SharedMemory. A SimEngineHost_* entry names\n"
    "                           the wrong side and will not connect.\n"
    "  --model-path             directory holding the schema and instance database, e.g.\n"
    "                           C:\\N8RO\\data\\db\n"
    "  --schema-file            schema name inside that database, e.g. N8roSimSchema\n"
    "  --entity-state-message   message instance name to resolve the entity-state topic\n"
    "                           from. Default simEntityStateUpdate. The topic itself is\n"
    "                           always read from the registry, never from a literal.\n"
    "  --capture-out            write a n8ro-capture/1 capture to this path. Without it the\n"
    "                           bridge only reports, exactly as at M3. M4 only: M5 replaces\n"
    "                           this with --out-dir / --run-label and the naming convention\n"
    "                           in the PRD.\n"
    "  --capture-max-samples    stop recording after this many accepted samples, then write\n"
    "                           the file and exit 0. Default 100000. This budget is what\n"
    "                           ends an M4 run - there is no signal handling until M7.\n";

struct Options {
    std::string config;
    std::string modelPath;
    std::string schemaFile;
    std::string entityStateMessage = kDefaultEntityStateMessage;
    std::string captureOut;
    std::size_t captureMaxSamples = kDefaultCaptureMaxSamples;
};

// Returns false and names the offending argument. Never throws: argv parsing is the first
// place an exception would escape main, and the platform contract forbids that (CLAUDE.md,
// hard rule 1).
bool parseOptions(int argc, char** argv, Options& out, std::string& error) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            error = "help";
            return false;
        }

        std::string* target = nullptr;
        bool isSampleBudget = false;
        if (arg == "--config") {
            target = &out.config;
        } else if (arg == "--model-path") {
            target = &out.modelPath;
        } else if (arg == "--schema-file") {
            target = &out.schemaFile;
        } else if (arg == "--entity-state-message") {
            target = &out.entityStateMessage;
        } else if (arg == "--capture-out") {
            target = &out.captureOut;
        } else if (arg == "--capture-max-samples") {
            isSampleBudget = true;
        } else {
            error = "unrecognised option " + arg;
            return false;
        }

        if (i + 1 >= argc) {
            error = arg + " requires a value";
            return false;
        }
        const std::string value = argv[++i];

        if (isSampleBudget) {
            char* end = nullptr;
            const unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
            if (end == value.c_str() || *end != '\0' || parsed == 0) {
                error = "--capture-max-samples needs a positive integer, got " + value;
                return false;
            }
            out.captureMaxSamples = static_cast<std::size_t>(parsed);
            continue;
        }
        *target = value;
    }

    if (out.config.empty()) {
        error = "--config is required";
        return false;
    }
    if (out.modelPath.empty()) {
        error = "--model-path is required";
        return false;
    }
    if (out.schemaFile.empty()) {
        error = "--schema-file is required";
        return false;
    }
    if (out.entityStateMessage.empty()) {
        error = "--entity-state-message cannot be empty";
        return false;
    }
    return true;
}

// create() reports failure as an empty optional and nothing else, so the diagnostic has to
// come from us. Echo all three values back exactly as resolved: [S1] warns the difficulty
// here is configuration, and the common fault - naming a SimEngineHost_* entry where a
// client is required - is only visible when the value is printed.
void reportCreateFailure(const Options& options) {
    N8RO_LOG_ERROR(std::string("SimulationEngineClient::create returned nullopt; no client was "
                               "constructed and nothing was subscribed"),
                   kCategory);
    N8RO_LOG_ERROR(std::string("  --config      = ") + options.config, kCategory);
    N8RO_LOG_ERROR(std::string("  --model-path  = ") + options.modelPath, kCategory);
    N8RO_LOG_ERROR(std::string("  --schema-file = ") + options.schemaFile, kCategory);
    N8RO_LOG_ERROR(std::string("check, in this order: the config entry exists in the model "
                               "database and is a SimEngineClient_* entry, not a SimEngineHost_* "
                               "one; the model path is the directory holding the schema database; "
                               "the schema file name matches the one the engine was started with"),
                   kCategory);
}

// The roster transitions our thread drained since the last report. This is where "destroyed
// at t=412.5" becomes visible - the handler could not log it, because a handler is a
// courier (CLAUDE.md hard rule 2).
void logRosterEvents(const std::vector<RosterEvent>& events) {
    for (const RosterEvent& event : events) {
        std::string line = event.eventName + " " + event.scenarioEntityName + " gen=" +
                           std::to_string(event.generation) + " at simTime=" +
                           std::to_string(event.simulationTimeS);
        if (!event.reason.empty()) {
            // Verbatim, including a value outside the engine's own set (BTB-EP-3).
            line += " reason=" + event.reason;
        }
        N8RO_LOG_INFO(line, kCategory);
    }
}

[[nodiscard]] std::string formatCountsByName(const std::map<std::string, std::uint64_t>& counts) {
    if (counts.empty()) {
        return "none";
    }
    std::string out;
    for (const auto& entry : counts) {   // std::map - ordered, so the line is stable
        if (!out.empty()) {
            out += " ";
        }
        out += entry.first + ":" + std::to_string(entry.second);
    }
    return out;
}

[[nodiscard]] const char* backpressurePolicyName(n8ro::core::BackpressurePolicy policy) {
    switch (policy) {
        case n8ro::core::BackpressurePolicy::KEEP_LATEST: return "KEEP_LATEST";
        case n8ro::core::BackpressurePolicy::FIFO_DROP:   return "FIFO_DROP";
        case n8ro::core::BackpressurePolicy::BLOCK:       return "BLOCK";
    }
    return "unknown";
}

// Phase 2 of M4's recording strategy: everything below runs on our own thread, after the
// subscriptions are cancelled and the pump is stopped. All IO, all formatting, all float
// conversion happen here and nowhere else (CLAUDE.md hard rule 2).
//
// The file is opened in binary mode on purpose. The format is LF-terminated, and Windows'
// text mode would translate every one to CRLF - which would make a capture written here
// differ byte-for-byte from the same capture written anywhere else, defeating the whole
// point of BTB-CAP-3.
[[nodiscard]] bool writeCaptureFile(const std::string& path, const capture::HeaderInfo& header,
                                    const std::string& scenario, const SampleBuffer& buffer,
                                    const n8ro::sim::MessageSchema& stateSchema,
                                    const PictureSnapshot& snapshot,
                                    const n8ro::sim::MessageBusPackedMetricsSnapshot& metrics,
                                    const n8ro::core::IMessageBus::Statistics& busStats,
                                    const std::string& endReason) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        N8RO_LOG_ERROR(std::string("could not open capture file for writing: ") + path, kCategory);
        return false;
    }

    file << capture::writeHeader(header) << '\n';

    const std::vector<CapturedSample>& samples = buffer.records();

    // M4 opens exactly one segment, and only the state model's attach-mid-run branch of it:
    // "attached -> segment_open on scenario_loaded, or first sample when attached mid-run"
    // (docs/prd.md, State model). The event-driven branch, reload, and therefore any second
    // segment are M5's BTB-CX-4. A segment is opened at all because BTB-CX-4 forbids a
    // sample record outside one and the envelope makes `segment` required - a capture with
    // samples and no segment would be a non-conformant file, which is the last thing to
    // hand across a repo boundary as a reference.
    const std::uint64_t segment = 0;
    const bool haveSamples = !samples.empty();
    const double firstSimTimeS = haveSamples ? samples.front().simulationTimeS : 0.0;
    const double lastSimTimeS = haveSamples ? samples.back().simulationTimeS : 0.0;

    if (haveSamples) {
        file << capture::writeSegmentOpen(firstSimTimeS, segment, scenario) << '\n';
        for (const CapturedSample& sample : samples) {
            file << capture::writeSample(sample, segment, stateSchema) << '\n';
        }
        file << capture::writeSegmentClose(lastSimTimeS, segment, scenario, endReason) << '\n';
    }

    capture::TrailerCounts counts;
    // These count records in this file, not transitions in the roster. M4 emits no
    // entity_add / entity_remove records, so both are zero however busy the roster was -
    // and the trailer would be lying if they were not.
    counts.segments = haveSamples ? 1 : 0;
    counts.samples = static_cast<std::uint64_t>(samples.size());
    counts.entitiesAdded = 0;
    counts.entitiesRemoved = 0;
    counts.verdicts = 0;

    capture::TrailerDrops drops;
    // Zero by construction at this version, and that is the honest value rather than a
    // convenient one. The buffer is preallocated to exactly the budget, so it never rejects
    // a sample *while recording* - the buffer filling IS the end of recording. Samples that
    // arrive afterwards are not dropped, they are after the end, which is precisely what
    // `end_reason: size_limit` tells a reader. Writing the observed tail here instead would
    // put a scheduler-dependent number into a file that must be byte-identical across two
    // identical runs (BTB-CAP-3), and would be misleading besides: it counts the handful
    // that landed in the shutdown window and not the tens of thousands published after we
    // stopped. The tail goes to the log, below. M5 fills this field with the internal
    // queue's real overflow, and the name and meaning carry over unchanged.
    drops.samplesNotRecorded = 0;
    drops.samplesOrphaned = snapshot.counters.samplesOrphaned;
    drops.samplesUnnamed = snapshot.counters.samplesUnnamed;
    drops.samplesUntimed = snapshot.counters.samplesUntimed;

    capture::TrailerBusMetrics busMetrics;
    busMetrics.schemaHashDrops = metrics.schemaHashDrops;
    busMetrics.messageIdDrops = metrics.messageIdDrops;
    busMetrics.decodeFailures = metrics.decodeFailures;
    busMetrics.missingSchemaPassthrough = metrics.missingSchemaPassthrough;
    busMetrics.legacyPayloadPassthrough = metrics.legacyPayloadPassthrough;
    // The delivery side, which nothing in this program read before 0.4.2. A message the bus
    // discards never reaches the decoder, so the five counters above stay at zero through a
    // loss - which is exactly what happened, and what made whole missing frames invisible.
    busMetrics.messagesDropped = busStats.messagesDropped;
    busMetrics.droppedByBackpressure = busStats.droppedByBackpressure;
    busMetrics.droppedByQueueOverflow = busStats.droppedByQueueOverflow;
    busMetrics.droppedByRateLimiting = busStats.droppedByRateLimiting;

    file << capture::writeTrailer(lastSimTimeS, endReason, counts, drops, busMetrics) << '\n';
    file.flush();
    if (!file) {
        N8RO_LOG_ERROR(std::string("capture file was opened but writing failed: ") + path,
                       kCategory);
        return false;
    }
    file.close();

    N8RO_LOG_INFO(std::string("capture written: ") + path + " (" +
                      std::to_string(counts.samples) + " sample records, segment " +
                      std::to_string(segment) + ", simTime " + std::to_string(firstSimTimeS) +
                      " to " + std::to_string(lastSimTimeS) + ", end_reason=" + endReason + ")",
                  kCategory);

    // The residue, reported here and deliberately not in the file. It cannot be zero - a bus
    // subscription cannot be stopped atomically - and it varies run to run, which is exactly
    // why a log line is its place (CLAUDE.md: wall-clock and anything like it belongs in log
    // lines and nowhere durable).
    N8RO_LOG_INFO(std::string("recording ended at the budget; ") +
                      std::to_string(buffer.notRecorded()) +
                      " further samples arrived between the budget being reached and the "
                      "subscription being cancelled. They are not in the capture and are not "
                      "counted as drops in it: the run was still publishing when we stopped "
                      "on purpose, which is what end_reason=size_limit records",
                  kCategory);

    // A schema field nothing ever published is a fact about the run and belongs in the
    // report, not only in the file's shape. The entity-state schema declares twelve fields
    // and activeAnimation was published zero times in 132 188 samples at M3 - which is the
    // case BTB-CAP-4's absent-not-defaulted rule exists for.
    const std::vector<std::string> absent = capture::neverPublishedFields(stateSchema, samples);
    if (absent.empty()) {
        N8RO_LOG_INFO(std::string("every field the entity-state schema declares was published at "
                                  "least once"),
                      kCategory);
    } else {
        std::string names;
        for (const std::string& field : absent) {
            if (!names.empty()) {
                names += ", ";
            }
            names += field;
        }
        N8RO_LOG_WARNING(std::string("declared but never published, so present in "
                                     "header.schemas and in no sample record: ") +
                             names + " (" + std::to_string(absent.size()) + " of " +
                             std::to_string(stateSchema.fields.size()) + " declared fields)",
                         kCategory);
    }
    return true;
}

int run(const Options& options) {
    // The registry is built from our own DbModel over the same model path and schema file
    // the engine was started with. SimulationEngineClient holds a registry of its own, but
    // exposes no accessor for it - only messageBus() - so the packed layer is ours to build.
    // This is the passive-observer recipe the shipped bus monitor documents.
    n8ro::schema::DbModel model(options.modelPath);
    if (!model.Open(options.schemaFile)) {
        N8RO_LOG_ERROR(std::string("DbModel::Open failed for schema file ") + options.schemaFile +
                           " under model path " + options.modelPath +
                           "; the packed schemas cannot be read and nothing could be decoded",
                       kCategory);
        return kExitModelOpenFailed;
    }

    n8ro::sim::MessageBusPackedSchemaRegistry registry;
    const Resolution resolution = resolveTopics(model, registry, options.entityStateMessage,
                                                options.modelPath, options.schemaFile);
    if (!resolution.ok) {
        // resolveTopics has already logged a named diagnostic for whichever condition fired.
        return resolution.exitCode;
    }

    n8ro::sim::SimulationEngineClientConfig clientConfig;
    clientConfig.simEngineConfigName = options.config;
    clientConfig.modelPath = options.modelPath;
    clientConfig.schemaFileName = options.schemaFile;

    N8RO_LOG_INFO(std::string("creating client: config=") + options.config + " modelPath=" +
                      options.modelPath + " schemaFile=" + options.schemaFile,
                  kCategory);

    std::optional<n8ro::sim::SimulationEngineClient> client =
        n8ro::sim::SimulationEngineClient::create(clientConfig);
    if (!client) {
        reportCreateFailure(options);
        return kExitCreateFailed;
    }

    n8ro::core::IMessageBus* bus = client->messageBus();
    if (bus == nullptr) {
        N8RO_LOG_ERROR(std::string("client was created but exposes no message bus; nothing can "
                                   "be subscribed"),
                       kCategory);
        return kExitNoMessageBus;
    }

    EntityPicture picture;
    n8ro::sim::MessageBusPacked packed(*bus, registry);

    const bool capturing = !options.captureOut.empty();
    SampleBuffer buffer(capturing ? options.captureMaxSamples : 0);
    if (capturing) {
        N8RO_LOG_INFO(std::string("capture enabled: ") + options.captureOut + " format " +
                          capture::kFormatVersion + ", budget " +
                          std::to_string(options.captureMaxSamples) +
                          " samples. Recording stops at the budget, then the file is written "
                          "and the bridge exits",
                      kCategory);
    } else {
        N8RO_LOG_INFO(std::string("no --capture-out given; reporting only, no capture file "
                                  "will be written"),
                      kCategory);
    }

    // The bus-side backpressure policy. M6 owns the decision (BTB-BP-3, OQ-4); M4's job is
    // to make it visible rather than to tune it, because the default is lossy and silence
    // about it is exactly the failure mode tenet 3 forbids. The value goes into the capture
    // header, so a capture always says which policy it was recorded under.
    const n8ro::core::SubscriptionOptions subscriptionOptions;
    N8RO_LOG_WARNING(std::string("subscribing with the bus default SubscriptionOptions: "
                                 "queueSize=") +
                         std::to_string(subscriptionOptions.queueSize) +
                         " backpressurePolicy=" +
                         backpressurePolicyName(subscriptionOptions.backpressurePolicy) +
                         " (lossy). This is deliberate before M6 - setting both boundaries "
                         "explicitly is BTB-BP-3/BTB-BP-4. At the reference scenario's 818 "
                         "packets/s a 100-message queue is ~120 ms of headroom",
                     kCategory);

    // BTB-EP-2: a decoded subscription. Each arrival delivers the raw Message, its
    // MessageSchema and the decoded StreamValueMap - no manual payload parsing exists
    // anywhere in this codebase.
    //
    // Both handlers run on the bus pump thread. Each copies what it needs and returns. No
    // IO, no formatting, no file, no float conversion: the sample is copied into the buffer
    // exactly as delivered and is not looked at again until phase 2.
    const std::uint64_t stateSubscription = packed.subscribeByTopic(
        resolution.entityState.topic,
        [&picture, &buffer, capturing](const n8ro::core::Message&, const n8ro::sim::MessageSchema&,
                                       const n8ro::sim::StreamValueMap& values) {
            const SampleOutcome outcome = picture.onSample(values);
            if (!capturing || !outcome.accepted) {
                return;
            }
            buffer.offer(CapturedSample{outcome.scenarioEntityName, outcome.generation,
                                        outcome.simulationTimeS, values});
        },
        subscriptionOptions);
    if (stateSubscription == 0) {
        N8RO_LOG_ERROR(std::string("subscribeByTopic returned no subscription for entity-state "
                                   "topic ") +
                           resolution.entityState.topic,
                       kCategory);
        return kExitSubscribeFailed;
    }

    const std::uint64_t eventSubscription = packed.subscribeByTopic(
        resolution.entityEvent.topic,
        [&picture](const n8ro::core::Message&, const n8ro::sim::MessageSchema&,
                   const n8ro::sim::StreamValueMap& values) { picture.onEntityEvent(values); },
        subscriptionOptions);
    if (eventSubscription == 0) {
        N8RO_LOG_ERROR(std::string("subscribeByTopic returned no subscription for entity-event "
                                   "topic ") +
                           resolution.entityEvent.topic,
                       kCategory);
        // Retained ids exist precisely so a partial bring-up unwinds cleanly (BTB-EP-2).
        static_cast<void>(packed.unsubscribe(stateSubscription));
        return kExitSubscribeFailed;
    }

    N8RO_LOG_INFO(std::string("subscribed decoded: entity-state topic ") +
                      resolution.entityState.topic + " (id " + std::to_string(stateSubscription) +
                      "), entity-event topic " + resolution.entityEvent.topic + " (id " +
                      std::to_string(eventSubscription) + ")",
                  kCategory);

    // Subscribe first, then pump - nothing published between the two is missed.
    client->startMessagePump();
    N8RO_LOG_INFO(std::string("message pump started; reporting engine state and the entity "
                              "picture once a second (Ctrl-C to stop)"),
                  kCategory);

    // The scenario name for the segment records. BTB-CX-4 requires it to be the name the
    // platform reports rather than one supplied on the command line; at M4 that is the
    // engine's own mirrored value, and at M5 it becomes the sim/scenario/event payload.
    std::string scenarioName;

    for (;;) {
        // Roster transitions first, so a removal is logged above the line whose count it
        // explains rather than below it.
        logRosterEvents(picture.drainEvents());

        const PictureSnapshot snap = picture.snapshot();
        const n8ro::sim::MessageBusPackedMetricsSnapshot metrics = packed.metricsSnapshot();

        // Two independent loss surfaces, and reporting only the first is what let whole
        // simulation frames go missing while this line said "drops=0" for four milestones.
        //
        //   decode  - MessageBusPacked: the message arrived and could not be turned into
        //             values. A schema mismatch shows up here (CLAUDE.md).
        //   deliver - IMessageBus: the message never arrived at all, because the bus
        //             discarded it under its backpressure policy. Nothing upstream of the
        //             decoder is visible to the decoder, so no amount of watching the first
        //             group can reveal the second.
        //
        // Both are printed every second whether zero or not: a number that only appears when
        // it is bad is a number nobody trusts.
        const n8ro::core::IMessageBus::Statistics busStats = bus->getStatistics();
        const std::uint64_t drops =
            metrics.schemaHashDrops + metrics.decodeFailures + metrics.missingSchemaPassthrough;
        const std::uint64_t busLoss = busStats.messagesDropped + busStats.droppedByBackpressure +
                                      busStats.droppedByQueueOverflow +
                                      busStats.droppedByRateLimiting;

        // Every value on the engine line is a local read on the client; nothing here
        // touches the bus.
        const std::string engineState = client->getEngineState();
        const std::optional<std::string> scenario = client->getLoadedScenarioName();

        if (scenarioName.empty() && scenario && !scenario->empty()) {
            scenarioName = *scenario;
        }

        std::printf(
            "engine=%-12s frame=%-10llu simTime=%10.3f scenario=%-24s live=%-4zu names=%-4zu "
            "samples=%llu drops=%llu(hash=%llu decode=%llu noschema=%llu)\n",
            engineState.empty() ? "(none yet)" : engineState.c_str(),
            static_cast<unsigned long long>(client->getFrameNumber()),
            client->getSimulationTimeS(),
            scenario && !scenario->empty() ? scenario->c_str() : "(none)", snap.liveCount,
            snap.roster.size(), static_cast<unsigned long long>(snap.counters.samplesAccepted),
            static_cast<unsigned long long>(drops),
            static_cast<unsigned long long>(metrics.schemaHashDrops),
            static_cast<unsigned long long>(metrics.decodeFailures),
            static_cast<unsigned long long>(metrics.missingSchemaPassthrough));
        std::printf("    removals=%-40s created=%llu deleted=%llu orphaned=%llu\n",
                    formatCountsByName(snap.removalsByReason).c_str(),
                    static_cast<unsigned long long>(snap.counters.entityCreated),
                    static_cast<unsigned long long>(snap.counters.entityDeleted),
                    static_cast<unsigned long long>(snap.counters.samplesOrphaned));
        std::printf("    busLoss=%llu(dropped=%llu backpressure=%llu queueOverflow=%llu "
                    "rateLimit=%llu)\n",
                    static_cast<unsigned long long>(busLoss),
                    static_cast<unsigned long long>(busStats.messagesDropped),
                    static_cast<unsigned long long>(busStats.droppedByBackpressure),
                    static_cast<unsigned long long>(busStats.droppedByQueueOverflow),
                    static_cast<unsigned long long>(busStats.droppedByRateLimiting));
        if (capturing) {
            std::printf("    capture=%zu/%zu records notRecorded=%llu\n", buffer.size(),
                        options.captureMaxSamples,
                        static_cast<unsigned long long>(buffer.notRecorded()));
        }

        // The remaining counters are surprises. They stay off the steady-state line so that
        // a non-zero value reads as the event it is.
        if (snap.counters.samplesUnnamed != 0 || snap.counters.samplesUntimed != 0 ||
            snap.counters.deleteOfUnknownEntity != 0 || snap.counters.eventsUnnamed != 0 ||
            snap.counters.eventsWithoutEntity != 0 || snap.counters.eventQueueDropped != 0) {
            std::printf("    unexpected: unnamedSamples=%llu untimedSamples=%llu "
                        "deleteOfUnknown=%llu unnamedEvents=%llu eventsWithoutEntity=%llu "
                        "eventLogDropped=%llu\n",
                        static_cast<unsigned long long>(snap.counters.samplesUnnamed),
                        static_cast<unsigned long long>(snap.counters.samplesUntimed),
                        static_cast<unsigned long long>(snap.counters.deleteOfUnknownEntity),
                        static_cast<unsigned long long>(snap.counters.eventsUnnamed),
                        static_cast<unsigned long long>(snap.counters.eventsWithoutEntity),
                        static_cast<unsigned long long>(snap.counters.eventQueueDropped));
        }
        if (!snap.unhandledEventNames.empty()) {
            std::printf("    otherEntityEvents=%s\n",
                        formatCountsByName(snap.unhandledEventNames).c_str());
        }
        std::fflush(stdout);

        if (capturing && buffer.atCapacity()) {
            // Recording stops here. Unsubscribe and stop the pump *before* touching the
            // buffer, so phase 2 reads a structure with no writer left to race with.
            static_cast<void>(packed.unsubscribe(stateSubscription));
            static_cast<void>(packed.unsubscribe(eventSubscription));
            client->stopMessagePump();
            N8RO_LOG_INFO(std::string("capture budget reached at ") +
                              std::to_string(buffer.size()) +
                              " samples; unsubscribed and stopped the pump, writing the file",
                          kCategory);

            // Taken once, after the pump has stopped, so the header and the trailer describe
            // the same instant and nothing can change under them.
            const PictureSnapshot finalSnapshot = picture.snapshot();
            const n8ro::sim::MessageBusPackedMetricsSnapshot finalMetrics = packed.metricsSnapshot();

            capture::HeaderInfo header;
            header.platform.engineConfig = options.config;
            header.platform.modelPath = options.modelPath;
            header.platform.schemaFile = options.schemaFile;
            header.platform.schemaVersion = model.getSchemaVersion();
            header.platform.runtimeVersion = std::string(n8ro::core::getN8roVersion());
            header.subscription.topic = resolution.entityState.topic;
            header.subscription.backpressurePolicy =
                backpressurePolicyName(subscriptionOptions.backpressurePolicy);
            header.subscription.queueSize =
                static_cast<std::uint64_t>(subscriptionOptions.queueSize);
            // Derived from what happened, not from what a status tick happened to see. A
            // bridge present at scenario load witnesses the entity_created burst first, so
            // its first accepted sample arrives with no orphans behind it; one that attached
            // after the burst sees nothing but orphans until the engine next creates
            // something. Same answer on every run, no clock involved (BTB-CAP-3).
            //
            // The fallback covers the case where nothing was ever accepted: a bridge that
            // attached mid-run to a scenario that then created no entity has orphans and no
            // samples, and is still mid-run.
            header.attachedMidRun =
                finalSnapshot.counters.samplesAccepted > 0
                    ? finalSnapshot.counters.orphansBeforeFirstAccepted > 0
                    : finalSnapshot.counters.samplesOrphaned > 0;
            // One entry, because one message type appears as a sample record's `message`.
            // The entity-event schema is not here: no record in the file is a verbatim dump
            // of one, so no reader ever needs it to interpret a line.
            header.schemas.push_back(resolution.entityState);

            // The budget is a configured bound on capture size, so `size_limit` is the
            // reason the format already has for it. It is the only end_reason an M4 run can
            // produce: `shutdown` needs signal handling (M7) and `host_lost` needs loss
            // detection (M5).
            const bool written =
                writeCaptureFile(options.captureOut, header, scenarioName, buffer,
                                 resolution.entityState, finalSnapshot, finalMetrics,
                                 bus->getStatistics(), "size_limit");
            return written ? kExitOk : kExitCaptureWriteFailed;
        }

        // While capturing, wait on the buffer rather than sleeping blind: the budget is what
        // ends the run, and waking a status tick after it was reached would let the run keep
        // publishing into a subscription we have already decided to cancel. The one-second
        // timeout keeps the report cadence identical either way. Without a capture there is
        // nothing to wait on - and the buffer's capacity is zero, which would read as
        // permanently full - so that path still sleeps.
        if (capturing) {
            static_cast<void>(buffer.waitUntilFull(std::chrono::seconds(1)));
        } else {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    // Console-only logger. The rotating-file initializer writes under <N8RO_RELEASE>\logs,
    // and C:\N8RO is read-only for this project (CLAUDE.md, Guardrail).
    n8ro::core::GlobalLogger::setLogger(
        n8ro::core::LogReportingFactory::createLogger<n8ro::core::ConsoleLoggerConfig>());

    // Hard rule 1: no exception escapes main. The try block starts after the logger exists
    // so that a failure still produces a named diagnostic rather than a silent abort.
    try {
        Options options;
        std::string error;
        if (!parseOptions(argc, argv, options, error)) {
            if (error == "help") {
                std::fputs(kUsage, stdout);
                n8ro::core::GlobalLogger::flush();
                return kExitOk;
            }
            N8RO_LOG_ERROR(std::string("bad invocation: ") + error, kCategory);
            std::fputs(kUsage, stderr);
            n8ro::core::GlobalLogger::flush();
            return kExitUsage;
        }

        const int code = run(options);
        n8ro::core::GlobalLogger::flush();
        return code;
    } catch (const std::exception& e) {
        N8RO_LOG_CRITICAL(std::string("unhandled std::exception escaped to main: ") + e.what(),
                          kCategory);
        n8ro::core::GlobalLogger::flush();
        return kExitUnexpected;
    } catch (...) {
        N8RO_LOG_CRITICAL(std::string("unhandled non-std exception escaped to main"), kCategory);
        n8ro::core::GlobalLogger::flush();
        return kExitUnexpected;
    }
}
