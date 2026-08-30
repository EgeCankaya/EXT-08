// EXT-08 Bus Telemetry Bridge - M3: the entity picture.
//
// Registers the packed schemas, resolves the entity-state and entity-event topics from the
// registry, subscribes decoded to both, and maintains a roster and a latest-sample map on
// our own side. Extends M2's once-a-second line with the entity count and the bus decoder's
// own drop counters.
//
// Scope: BTB-EP-1 through BTB-EP-4, plus BTB-CX-1 carried over from M2. There is no capture
// file, no capture format, no segment record, no referee, no backpressure tuning and no
// signal handling - those are M4 through M7 (docs/prd.md).

#include "EntityPicture.h"
#include "ExitCodes.h"
#include "TopicResolution.h"

#include <DbModel.h>
#include <core/logging/GlobalLogger.h>
#include <core/logging/LogReportingFactory.h>
#include <core/messaging/IMessageBus.h>
#include <infrastructure/SimulationEngineClient.h>
#include <messaging/packed/MessageBusPacked.h>
#include <messaging/packed/MessageBusPackedSchemaRegistry.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <optional>
#include <string>
#include <thread>

namespace {

using namespace n8ro::bridge;

constexpr const char* kCategory = "n8ro-bridge";

// The default anchor for entity-state resolution. This is a *message instance name*, not a
// topic: the topic string is always read off the schema this name resolves to, so a topic
// rename in the database needs no rebuild. It is overridable because a message rename
// should be a flag change rather than a rebuild too.
constexpr const char* kDefaultEntityStateMessage = "simEntityStateUpdate";

constexpr const char* kUsage =
    "usage: n8ro-bridge --config <SimEngineClient_*> --model-path <dir> --schema-file <name>\n"
    "                   [--entity-state-message <name>]\n"
    "\n"
    "  --config                 client-side sim engine config entry, e.g.\n"
    "                           SimEngineClient_SharedMemory. A SimEngineHost_* entry names\n"
    "                           the wrong side and will not connect.\n"
    "  --model-path             directory holding the schema and instance database, e.g.\n"
    "                           C:\\N8RO\\data\\db\n"
    "  --schema-file            schema name inside that database, e.g. N8roSimSchema\n"
    "  --entity-state-message   message instance name to resolve the entity-state topic\n"
    "                           from. Default simEntityStateUpdate. The topic itself is\n"
    "                           always read from the registry, never from a literal.\n";

struct Options {
    std::string config;
    std::string modelPath;
    std::string schemaFile;
    std::string entityStateMessage = kDefaultEntityStateMessage;
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
        if (arg == "--config") {
            target = &out.config;
        } else if (arg == "--model-path") {
            target = &out.modelPath;
        } else if (arg == "--schema-file") {
            target = &out.schemaFile;
        } else if (arg == "--entity-state-message") {
            target = &out.entityStateMessage;
        } else {
            error = "unrecognised option " + arg;
            return false;
        }

        if (i + 1 >= argc) {
            error = arg + " requires a value";
            return false;
        }
        *target = argv[++i];
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

    // The bus-side backpressure policy. M6 owns the decision (BTB-BP-3, OQ-4); M3's job is
    // to make it visible rather than to tune it, because the default is lossy and silence
    // about it is exactly the failure mode tenet 3 forbids.
    const n8ro::core::SubscriptionOptions subscriptionOptions;
    N8RO_LOG_WARNING(std::string("subscribing with the bus default SubscriptionOptions: "
                                 "queueSize=") +
                         std::to_string(subscriptionOptions.queueSize) +
                         " backpressurePolicy=KEEP_LATEST (lossy). This is deliberate for M3 - "
                         "setting both boundaries explicitly is M6's BTB-BP-3/BTB-BP-4. At the "
                         "reference scenario's 818 packets/s a 100-message queue is ~120 ms of "
                         "headroom",
                     kCategory);

    // BTB-EP-2: a decoded subscription. Each arrival delivers the raw Message, its
    // MessageSchema and the decoded StreamValueMap - no manual payload parsing exists
    // anywhere in this codebase.
    //
    // Both handlers run on the bus pump thread. Each copies what it needs, takes the
    // picture's lock, updates, and returns. No IO, no formatting, no file.
    const std::uint64_t stateSubscription = packed.subscribeByTopic(
        resolution.entityState.topic,
        [&picture](const n8ro::core::Message&, const n8ro::sim::MessageSchema&,
                   const n8ro::sim::StreamValueMap& values) { picture.onSample(values); },
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

    for (;;) {
        // Roster transitions first, so a removal is logged above the line whose count it
        // explains rather than below it.
        logRosterEvents(picture.drainEvents());

        const PictureSnapshot snap = picture.snapshot();
        const n8ro::sim::MessageBusPackedMetricsSnapshot metrics = packed.metricsSnapshot();

        // The decoder's three loss counters (CLAUDE.md: a silent topic is a schema mismatch,
        // and these prove it). Reported every second whether zero or not - a number that
        // only appears when it is bad is a number nobody trusts.
        const std::uint64_t drops =
            metrics.schemaHashDrops + metrics.decodeFailures + metrics.missingSchemaPassthrough;

        // Every value on the engine line is a local read on the client; nothing here
        // touches the bus.
        const std::string engineState = client->getEngineState();
        const std::optional<std::string> scenario = client->getLoadedScenarioName();

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

        std::this_thread::sleep_for(std::chrono::seconds(1));
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
