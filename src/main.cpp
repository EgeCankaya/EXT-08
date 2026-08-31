// EXT-08 Bus Telemetry Bridge - M7: shutdown, and the finished program.
//
// Registers the packed schemas, resolves four topics from the registry, subscribes decoded,
// maintains a roster and a latest-sample map, streams a `n8ro-capture/1` file through a
// writer thread behind a bounded queue, and evaluates declared conditions against the run.
//
// Two modes, mutually exclusive:
//
//   live      --config ...   attach to a running simulation and record it, judging as it goes
//   replay    --replay ...   re-judge a stored capture with no simulator, no bus, no client
//
// The referee is the same class in both, fed from a decoded StreamValueMap in one and from
// `sample.fields` out of a file in the other. That is what makes "live verdicts equal replay
// verdicts" true by construction rather than by testing (ADR-5), and it is the strongest
// available conformance test for the capture format: if the referee can re-derive its own
// verdicts from the file alone, the file demonstrably contains enough.
//
// Scope: BTB-SD-1 on top of M2-M6. The normative description of what it writes is
// docs/capture-format-v1.md; every decision taken while building it is in
// docs/decisions-m5-m7.md.
//
// A live run ends on host loss, on Ctrl-C, or on the record budget. The one requirement left
// unbuilt is BTB-CAP-6's byte-limited capture, which is P2 - see docs/decisions-m5-m7.md.
//
// The four topics, and why each is here:
//
//   sim/entity/state   the stream being recorded                        (BTB-EP-2)
//   sim/entity/event   the roster, and entity_add / entity_remove       (BTB-EP-3)
//   sim/scenario/event segment boundaries                               (BTB-CX-4)
//   sim/engine/state   the heartbeat whose silence is host loss         (BTB-CX-3)
//
// Not one of those strings is written anywhere in this program. Each is read off a schema
// the registry resolved from a message-instance name or an EventNames.h constant
// (CLAUDE.md hard rule 3, BTB-EP-1).

#include "CaptureFormat.h"
#include "CaptureRecord.h"
#include "CaptureWriter.h"
#include "Conditions.h"
#include "EntityPicture.h"
#include "ExitCodes.h"
#include "HandlerTiming.h"
#include "RecordQueue.h"
#include "Referee.h"
#include "Replay.h"
#include "Signals.h"
#include "TopicResolution.h"

#include <DbModel.h>
#include <core/logging/GlobalLogger.h>
#include <core/logging/LogReportingFactory.h>
#include <core/messaging/IMessageBus.h>
#include <core/version/BuildInfo.h>
#include <infrastructure/SimulationEngineClient.h>
#include <messaging/EventNames.h>
#include <messaging/packed/MessageBusPacked.h>
#include <messaging/packed/MessageBusPackedSchemaRegistry.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace n8ro::bridge;

constexpr const char* kCategory = "n8ro-bridge";

// Message-instance names, not topics: the topic string is always read off the schema each
// name resolves to, so a topic rename in the database needs no rebuild. Both are overridable
// because a message rename should be a flag change rather than a rebuild too.
constexpr const char* kDefaultEntityStateMessage = "simEntityStateUpdate";
constexpr const char* kDefaultEngineStateMessage = "simEngineState";

// --- The two backpressure boundaries (BTB-BP-3, BTB-BP-4, ADR-4) ------------------------
//
// BUS SIDE. SubscriptionOptions defaults to KEEP_LATEST with queueSize 100, and for a
// recorder that is precisely wrong: it discards the older of two messages, which is the one
// already part of the run's history. FIFO_DROP is the provisional answer (OQ-4); BLOCK is
// rejected outright because a recorder that stalls the bus changes the run it is recording,
// and docs/capture-format-v1.md section 14 states that to consumers in writing.
//
// 1024 rather than the default 100: at the reference scenario's 818 packets/s that is ~1.25 s
// of headroom instead of ~120 ms, and at the 126-entity overload scenario's 2 487/s it is
// ~410 ms instead of ~40 ms. Provisional, and M6 confirms it under load.
constexpr std::size_t kBusQueueSize = 1024;
constexpr n8ro::core::BackpressurePolicy kBusPolicy = n8ro::core::BackpressurePolicy::FIFO_DROP;

// INTERNAL SIDE. See RecordQueue.h for the shape and docs/decisions-m5-m7.md D-6 to D-9 for
// the reasoning. 8192 records is ~16 MB at M4's measured ~2 KB per held StreamValueMap -
// 10.0 s of headroom at 818/s against a p95 enqueue-to-durable target of 250 ms.
constexpr std::size_t kDefaultQueueSize = 8192;
constexpr std::size_t kStructuralReserve = 1024;

// --- Host-loss detection (BTB-CX-3) -----------------------------------------------------
//
// Derived, not guessed. sim/engine/state publishes at ~19.5/s through idle frames - before
// the scenario loads and after the engine stops - so its silence is evidence in a way that
// entity-state silence is not. Measured across two full bring-up/load/run/teardown cycles:
// nominal period 51 ms, and the largest inter-arrival gap anywhere was 548 ms, at scenario
// load on the reference scenario (408 ms on the 126-entity one - the stall does not scale
// with entity count).
//
// 3.0 s is 5.5x that largest observed gap and ~59 heartbeat periods. Deliberately far below
// SubscriptionOptions::activityThresholdS, whose 30 s default is the bus's own "this
// subscription looks idle" semantics and is two orders of magnitude too slow to be a
// host-loss signal for a campaign runner. docs/decisions-m5-m7.md, D-3.
constexpr double kHostLossWindowS = 3.0;

// How often the main loop wakes. The status line is printed once a second; the loop runs
// four times faster so that host loss is noticed within a quarter of a second of the window
// expiring rather than up to a second later.
constexpr auto kPollInterval = std::chrono::milliseconds(250);
constexpr int kPollsPerStatusLine = 4;
constexpr auto kWaitingLogInterval = std::chrono::seconds(5);

constexpr const char* kUsage =
    "usage: n8ro-bridge --config <SimEngineClient_*> --model-path <dir> --schema-file <name>\n"
    "                   --out-dir <dir> [--run-label <label>]\n"
    "                   [--entity-state-message <name>] [--engine-state-message <name>]\n"
    "                   [--queue-size <n>] [--overflow-policy <drop_newest|drop_oldest>]\n"
    "                   [--capture-max-bytes <n>] [--on-size-limit <stop|rotate>]\n"
    "                   [--capture-max-samples <n>] [--conditions <file>]\n"
    "       n8ro-bridge --replay <capture> --conditions <file> [--out-dir <dir>]\n"
    "\n"
    "  --config                 client-side sim engine config entry, e.g.\n"
    "                           SimEngineClient_SharedMemory. A SimEngineHost_* entry names\n"
    "                           the wrong side and will not connect.\n"
    "  --model-path             directory holding the schema and instance database, e.g.\n"
    "                           C:\\N8RO\\data\\db\n"
    "  --schema-file            schema name inside that database, e.g. N8roSimSchema\n"
    "  --out-dir                existing, writable directory for the capture. The file is\n"
    "                           named capture-<scenario>-<run-label>.n8rocap.jsonl.\n"
    "  --run-label              label for this run. Defaults to the next unused zero-padded\n"
    "                           ordinal in --out-dir. Never a timestamp: campaign tooling\n"
    "                           addresses runs by path, and a wall-clock name makes two\n"
    "                           identical runs unaddressable as a pair.\n"
    "  --entity-state-message   message instance name to resolve the entity-state topic\n"
    "                           from. Default simEntityStateUpdate.\n"
    "  --engine-state-message   message instance name to resolve the engine-state heartbeat\n"
    "                           from. Default simEngineState. Its silence is how host loss\n"
    "                           is detected, within a 3.0 s window.\n"
    "  --queue-size             handler-to-writer queue bound, in records. Default 8192.\n"
    "                           Overflow is counted into trailer.drops.samples_not_recorded.\n"
    "  --overflow-policy        drop_newest (default) or drop_oldest. block is not offered;\n"
    "                           ask for it to be told why.\n"
    "  --capture-max-bytes      maximum size of one capture file, in bytes (BTB-CAP-6).\n"
    "                           Default 0, meaning no bound. The limit and the action below\n"
    "                           are written into the capture's own header. A record is\n"
    "                           either written whole or not at all - no line is ever cut.\n"
    "  --on-size-limit          what to do on reaching --capture-max-bytes: stop (default)\n"
    "                           closes the capture with a well-formed trailer carrying\n"
    "                           end_reason=size_limit; rotate closes this part the same way\n"
    "                           and continues into a numbered .partNNN continuation file.\n"
    "                           Each part is a complete, independently valid capture.\n"
    "  --capture-max-samples    stop after this many sample records and close the capture\n"
    "                           with end_reason=size_limit. Default 0, meaning no bound -\n"
    "                           a live run ends on host loss. A record-count safety bound,\n"
    "                           counted across the whole run; it always stops, never rotates.\n"
    "  --conditions             JSON file of declared conditions. Verdicts are written into\n"
    "                           the capture and into verdicts-<scenario>-<run-label>.jsonl\n"
    "                           beside it. Without it the bridge records but judges nothing.\n"
    "  --replay                 offline mode: re-judge a stored capture with no simulator,\n"
    "                           no bus and no client. Requires --conditions. Mutually\n"
    "                           exclusive with --config.\n";

struct Options {
    std::string config;
    std::string modelPath;
    std::string schemaFile;
    std::string outDir;
    std::string runLabel;
    std::string entityStateMessage = kDefaultEntityStateMessage;
    std::string engineStateMessage = kDefaultEngineStateMessage;
    std::size_t queueSize = kDefaultQueueSize;
    OverflowPolicy overflowPolicy = OverflowPolicy::DropNewest;
    std::size_t captureMaxSamples = 0;   // 0 = unbounded
    std::uint64_t captureMaxBytes = 0;   // 0 = unbounded (BTB-CAP-6)
    SizeLimitAction onSizeLimit = SizeLimitAction::Stop;
    std::string conditionsPath;
    std::string replayPath;

    [[nodiscard]] bool isReplay() const { return !replayPath.empty(); }
};

[[nodiscard]] bool parseCount(const std::string& flag, const std::string& value, bool allowZero,
                              std::size_t& out, std::string& error) {
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0' || (!allowZero && parsed == 0)) {
        error = flag + " needs a " + (allowZero ? "non-negative" : "positive") +
                " integer, got " + value;
        return false;
    }
    out = static_cast<std::size_t>(parsed);
    return true;
}

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
        if (arg.rfind("--", 0) != 0) {
            error = "unrecognised argument " + arg;
            return false;
        }
        if (i + 1 >= argc) {
            error = arg + " requires a value";
            return false;
        }
        const std::string value = argv[++i];

        if (arg == "--config") {
            out.config = value;
        } else if (arg == "--model-path") {
            out.modelPath = value;
        } else if (arg == "--schema-file") {
            out.schemaFile = value;
        } else if (arg == "--out-dir") {
            out.outDir = value;
        } else if (arg == "--run-label") {
            out.runLabel = value;
        } else if (arg == "--entity-state-message") {
            out.entityStateMessage = value;
        } else if (arg == "--engine-state-message") {
            out.engineStateMessage = value;
        } else if (arg == "--queue-size") {
            if (!parseCount(arg, value, false, out.queueSize, error)) {
                return false;
            }
        } else if (arg == "--overflow-policy") {
            if (!parseOverflowPolicy(value, out.overflowPolicy, error)) {
                return false;
            }
        } else if (arg == "--capture-max-samples") {
            if (!parseCount(arg, value, true, out.captureMaxSamples, error)) {
                return false;
            }
        } else if (arg == "--capture-max-bytes") {
            std::size_t bytes = 0;
            if (!parseCount(arg, value, true, bytes, error)) {
                return false;
            }
            out.captureMaxBytes = static_cast<std::uint64_t>(bytes);
        } else if (arg == "--on-size-limit") {
            if (!parseSizeLimitAction(value, out.onSizeLimit, error)) {
                return false;
            }
        } else if (arg == "--conditions") {
            out.conditionsPath = value;
        } else if (arg == "--replay") {
            out.replayPath = value;
        } else {
            error = "unrecognised option " + arg;
            return false;
        }
    }

    // Live and replay are mutually exclusive, and the PRD says so explicitly: replay has no
    // bus, no client and no engine configuration, so accepting both would mean silently
    // ignoring one of them.
    if (out.isReplay()) {
        if (!out.config.empty()) {
            error = "--replay and --config are mutually exclusive. Replay mode has no bus, no "
                    "client and no engine configuration; it re-judges a stored capture";
            return false;
        }
        if (out.conditionsPath.empty()) {
            error = "--replay needs --conditions - there is nothing to judge without them";
            return false;
        }
        if (out.outDir.empty()) {
            // Default to the capture's own directory, which is where a campaign would want
            // the verdicts anyway.
            const std::size_t slash = out.replayPath.find_last_of("/\\");
            out.outDir = slash == std::string::npos ? std::string(".")
                                                    : out.replayPath.substr(0, slash);
        }
        return true;
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
    if (out.outDir.empty()) {
        error = "--out-dir is required";
        return false;
    }
    // A bound smaller than the space held back to close a file cannot be honoured: the header
    // alone would exceed it, and every part would be a header and a trailer. Named here
    // rather than discovered as a directory full of empty captures.
    if (out.captureMaxBytes != 0 &&
        out.captureMaxBytes < 2 * CaptureWriter::kCloseReserveBytes) {
        error = "--capture-max-bytes must be at least " +
                std::to_string(2 * CaptureWriter::kCloseReserveBytes) +
                " bytes; a capture's header is a few kilobytes on its own and " +
                std::to_string(CaptureWriter::kCloseReserveBytes) +
                " bytes are reserved to close the file cleanly";
        return false;
    }
    if (out.entityStateMessage.empty() || out.engineStateMessage.empty()) {
        error = "a message-name option cannot be empty";
        return false;
    }
    // The PRD's threat model: "refuse traversal components in the run label used for
    // filenames". A label is a filename component and nothing else.
    if (out.runLabel.find_first_of("/\\:") != std::string::npos || out.runLabel == "." ||
        out.runLabel == "..") {
        error = "--run-label is a filename component and may not contain a path separator, a "
                "drive letter, or a traversal component; got " + out.runLabel;
        return false;
    }
    return true;
}

// The PRD's fail-safe-on-output rule: validate the directory at startup rather than
// discovering it at first write, and never create it implicitly. A run that cannot store its
// evidence should not start capturing into a void.
[[nodiscard]] bool validateOutDir(const std::string& dir, std::string& canonical) {
    std::error_code ec;
    const std::filesystem::path path(dir);
    if (!std::filesystem::exists(path, ec) || ec) {
        N8RO_LOG_ERROR(std::string("--out-dir does not exist: ") + dir +
                           ". It must be an existing, writable directory; the bridge does not "
                           "create it, because a mistyped path would then look like success",
                       kCategory);
        return false;
    }
    if (!std::filesystem::is_directory(path, ec) || ec) {
        N8RO_LOG_ERROR(std::string("--out-dir is not a directory: ") + dir, kCategory);
        return false;
    }

    // Canonicalise, so a path reached through a symlink or with traversal components is
    // resolved and logged as what it actually is.
    const std::filesystem::path resolved = std::filesystem::canonical(path, ec);
    if (ec) {
        N8RO_LOG_ERROR(std::string("--out-dir could not be canonicalised: ") + dir + " (" +
                           ec.message() + ")",
                       kCategory);
        return false;
    }
    canonical = resolved.string();

    // Writability is only knowable by trying. A probe file is removed immediately; failing
    // here is far cheaper than failing after a 200-second run.
    const std::filesystem::path probe = resolved / ".n8ro-bridge-write-probe";
    {
        std::ofstream test(probe, std::ios::binary | std::ios::trunc);
        if (!test) {
            N8RO_LOG_ERROR(std::string("--out-dir is not writable: ") + canonical, kCategory);
            return false;
        }
    }
    std::filesystem::remove(probe, ec);
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

// One screen at exit (BTB-OBS-2). Everything a reader of the log needs to decide whether the
// capture is trustworthy, in the order they would ask.
void printRunSummary(const CaptureWriter& writer, const PictureSnapshot& snap,
                     const QueueCounters& queue, const RecordQueue& queueShape,
                     const HandlerTiming& stateTiming, const HandlerTiming& eventTiming,
                     const n8ro::sim::MessageBusPackedMetricsSnapshot& metrics,
                     const n8ro::core::IMessageBus::Statistics& busStats, EndReason endReason) {
    std::printf("\n=== run summary =========================================================\n");
    const std::vector<std::string>& parts = writer.parts();
    if (parts.size() <= 1) {
        std::printf("capture     %s\n",
                    writer.path().empty() ? "(none written)" : writer.path().c_str());
    } else {
        // A rotated run. The count comes first, because an operator told only about the last
        // part would think the rest had been lost. The list is elided in the middle rather
        // than printed whole: an overnight run can produce hundreds of parts, and a summary
        // that scrolls its own first line off the screen has stopped being a summary. The
        // names are derivable - they are the first name with .partNNN interposed - and the
        // set is walkable from `continued_in` in any case.
        constexpr std::size_t kPartsShownEachEnd = 3;
        std::printf("capture     %zu parts, each a complete capture:\n", parts.size());
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (parts.size() > 2 * kPartsShownEachEnd + 1 && i == kPartsShownEachEnd) {
                std::printf("            ... %zu more ...\n",
                            parts.size() - 2 * kPartsShownEachEnd);
                i = parts.size() - kPartsShownEachEnd - 1;
                continue;
            }
            std::printf("            part %03zu  %s\n", i, parts[i].c_str());
        }
    }
    std::printf("end_reason  %s\n", endReasonName(endReason));
    // Run-wide. The trailer's own `counts` is per file, which is what the format specifies
    // it to mean; an operator asking what a run recorded means the whole run.
    std::printf("records     %llu written (segments=%llu samples=%llu entity_add=%llu "
                "entity_remove=%llu verdicts=%llu)\n",
                static_cast<unsigned long long>(writer.recordsWritten()),
                static_cast<unsigned long long>(writer.runCounts().segments),
                static_cast<unsigned long long>(writer.runCounts().samples),
                static_cast<unsigned long long>(writer.runCounts().entitiesAdded),
                static_cast<unsigned long long>(writer.runCounts().entitiesRemoved),
                static_cast<unsigned long long>(writer.runCounts().verdicts));
    // Per segment, min to max. Not first-written to last-written: a complete live run ends
    // with a teardown reload whose clock has been reset (spec 5.1), so a file-wide
    // first-to-last pair reads 0.0 -> 0.0 on every such run and hides the whole run behind
    // its own teardown.
    const auto& spans = writer.segmentSpans();
    bool anySamples = false;
    bool anyZeroSpanSegment = false;
    double runMin = 0.0;
    double runMax = 0.0;
    for (const auto& span : spans) {
        if (span.samples == 0) {
            continue;
        }
        if (span.minSimTimeS == 0.0 && span.maxSimTimeS == 0.0) {
            anyZeroSpanSegment = true;
        }
        if (!anySamples) {
            runMin = span.minSimTimeS;
            runMax = span.maxSimTimeS;
            anySamples = true;
        } else {
            runMin = (std::min)(runMin, span.minSimTimeS);
            runMax = (std::max)(runMax, span.maxSimTimeS);
        }
    }
    if (!anySamples) {
        std::printf("sim_time    no samples recorded; last record %.6f s\n",
                    writer.lastRecordSimTimeS());
    } else {
        std::printf("sim_time    samples %.6f -> %.6f s across %zu segment%s; last record %.6f s\n",
                    runMin, runMax, spans.size(), spans.size() == 1 ? "" : "s",
                    writer.lastRecordSimTimeS());
        for (const auto& span : spans) {
            if (span.samples == 0) {
                std::printf("            segment %llu: no samples%s\n",
                            static_cast<unsigned long long>(span.ordinal),
                            parts.size() <= 1 ? "" : " (see part above)");
            } else if (parts.size() <= 1) {
                std::printf("            segment %llu: %llu samples, %.6f -> %.6f s\n",
                            static_cast<unsigned long long>(span.ordinal),
                            static_cast<unsigned long long>(span.samples), span.minSimTimeS,
                            span.maxSimTimeS);
            } else {
                // Ordinals restart at 0 in each part, so the part has to be named with them
                // or two different segments read as one.
                std::printf("            part %03llu segment %llu: %llu samples, "
                            "%.6f -> %.6f s\n",
                            static_cast<unsigned long long>(span.part),
                            static_cast<unsigned long long>(span.ordinal),
                            static_cast<unsigned long long>(span.samples), span.minSimTimeS,
                            span.maxSimTimeS);
            }
        }
    }
    // Only when the file actually contains the thing being explained. A run ended by Ctrl-C
    // has no teardown reload and no reset clock, and a note about one there describes a
    // record the reader is not looking at.
    if (anyZeroSpanSegment || writer.lastRecordSimTimeS() == 0.0) {
        std::printf("            (a segment spanning 0.0 -> 0.0, and a last record of 0.0, are the "
                    "teardown\n             boundary - the engine resets the clock before "
                    "publishing those. Spec 5.1)\n");
    }
    std::printf("entities    %zu names seen, %zu occupancies open at exit\n", snap.roster.size(),
                snap.liveCount);
    std::printf("removals    %s\n", formatCountsByName(snap.removalsByReason).c_str());
    if (!writer.verdictPath().empty()) {
        std::printf("verdicts    %llu written -> %s\n",
                    static_cast<unsigned long long>(writer.counts().verdicts),
                    writer.verdictPath().c_str());
    }

    std::printf("\n-- loss, in the order it can happen ---------------------------------------\n");
    std::printf("bus deliver  dropped=%llu backpressure=%llu queueOverflow=%llu rateLimit=%llu\n",
                static_cast<unsigned long long>(busStats.messagesDropped),
                static_cast<unsigned long long>(busStats.droppedByBackpressure),
                static_cast<unsigned long long>(busStats.droppedByQueueOverflow),
                static_cast<unsigned long long>(busStats.droppedByRateLimiting));
    std::printf("bus decode   schemaHash=%llu messageId=%llu decodeFail=%llu noSchema=%llu "
                "legacy=%llu\n",
                static_cast<unsigned long long>(metrics.schemaHashDrops),
                static_cast<unsigned long long>(metrics.messageIdDrops),
                static_cast<unsigned long long>(metrics.decodeFailures),
                static_cast<unsigned long long>(metrics.missingSchemaPassthrough),
                static_cast<unsigned long long>(metrics.legacyPayloadPassthrough));
    std::printf("picture      orphaned=%llu unnamed=%llu untimed=%llu deleteOfUnknown=%llu\n",
                static_cast<unsigned long long>(snap.counters.samplesOrphaned),
                static_cast<unsigned long long>(snap.counters.samplesUnnamed),
                static_cast<unsigned long long>(snap.counters.samplesUntimed),
                static_cast<unsigned long long>(snap.counters.deleteOfUnknownEntity));
    std::printf("writer queue samplesDropped=%llu eventsDropped=%llu  (capacity %zu+%zu, "
                "policy %s, highWater=%zu)\n",
                static_cast<unsigned long long>(queue.samplesDropped),
                static_cast<unsigned long long>(queue.structuralDropped),
                queueShape.sampleCapacity(),
                queueShape.hardCapacity() - queueShape.sampleCapacity(),
                overflowPolicyName(queueShape.policy()), queue.highWater);
    std::printf("staging      dropped=%llu   unload noise ignored=%llu\n",
                static_cast<unsigned long long>(writer.counts().stagedDropped),
                static_cast<unsigned long long>(writer.counts().unloadNoiseIgnored));

    std::printf("\n-- after the end, not loss (log only, never in the file) ------------------\n");
    std::printf("shutdown window  samples=%llu events=%llu   past a bound: records=%llu\n",
                static_cast<unsigned long long>(queue.samplesAfterClose),
                static_cast<unsigned long long>(queue.structuralAfterClose),
                static_cast<unsigned long long>(writer.recordsPastBound()));

    std::printf("\n-- handler cost (BTB-BP-1: p50<20us p95<100us p99<500us) ------------------\n");
    std::printf("entity-state %s\n", stateTiming.summary().c_str());
    std::printf("entity-event %s\n", eventTiming.summary().c_str());

    const std::vector<std::string> absent = writer.neverPublishedFields();
    if (!absent.empty()) {
        std::string names;
        for (const std::string& field : absent) {
            if (!names.empty()) {
                names += ", ";
            }
            names += field;
        }
        std::printf("\nschema       declared but NEVER published, so in header.schemas and in no "
                    "sample: %s\n",
                    names.c_str());
    }
    if (!snap.unhandledEventNames.empty()) {
        std::printf("other events %s\n", formatCountsByName(snap.unhandledEventNames).c_str());
    }
    std::printf("=========================================================================\n");
    std::fflush(stdout);
}

// Offline mode (BTB-REF-4). No bus, no client, no engine configuration - the referee reads a
// finished file and reaches the same verdicts a live run reached from the same records.
int runReplay(const Options& options) {
    std::vector<Condition> conditions;
    std::string error;
    if (!loadConditions(options.conditionsPath, conditions, error)) {
        N8RO_LOG_ERROR(std::string("condition file rejected: ") + error, kCategory);
        return kExitConditionsInvalid;
    }
    N8RO_LOG_INFO(std::string("loaded ") + std::to_string(conditions.size()) +
                      " conditions from " + options.conditionsPath,
                  kCategory);

    std::string outDir;
    if (!validateOutDir(options.outDir, outDir)) {
        return kExitOutDirInvalid;
    }

    // The verdict file is named from the capture rather than from a scenario, because replay
    // is addressed by the file it re-judges. `capture-x-000.n8rocap.jsonl` gives
    // `verdicts-x-000.replay.jsonl`, so a replay never overwrites the live run's verdicts and
    // the two can be diffed - which is exactly what BTB-REF-4's acceptance criterion asks for.
    const std::filesystem::path capture(options.replayPath);
    std::string stem = capture.filename().string();
    const std::string captureSuffix = ".n8rocap.jsonl";
    if (stem.size() > captureSuffix.size() &&
        stem.compare(stem.size() - captureSuffix.size(), captureSuffix.size(), captureSuffix) ==
            0) {
        stem.resize(stem.size() - captureSuffix.size());
    }
    if (stem.rfind("capture-", 0) == 0) {
        stem = stem.substr(std::string("capture-").size());
    }
    const std::string verdictPath =
        (std::filesystem::path(outDir) / ("verdicts-" + stem + ".replay.jsonl")).string();

    ReplayResult result;
    if (!replay(options.replayPath, verdictPath, conditions, result, error)) {
        N8RO_LOG_ERROR(std::string("replay failed: ") + error, kCategory);
        return kExitReplayFailed;
    }

    std::printf("\n=== replay summary ======================================================\n");
    std::printf("capture     %s\n", options.replayPath.c_str());
    std::printf("format      %s   end_reason %s\n", result.formatVersion.c_str(),
                result.endReason.empty() ? "(none)" : result.endReason.c_str());
    std::printf("read        %llu lines (segments=%llu samples=%llu entity_add=%llu "
                "entity_remove=%llu)\n",
                static_cast<unsigned long long>(result.linesRead),
                static_cast<unsigned long long>(result.segments),
                static_cast<unsigned long long>(result.samples),
                static_cast<unsigned long long>(result.entityAdds),
                static_cast<unsigned long long>(result.entityRemoves));
    if (result.verdictsInInput != 0) {
        std::printf("            the capture already carried %llu verdict records from the run "
                    "that produced it; ignored\n",
                    static_cast<unsigned long long>(result.verdictsInInput));
    }
    std::printf("conditions  %zu declared\n", conditions.size());
    std::printf("verdicts    %llu written (%llu met, %llu not met) -> %s\n",
                static_cast<unsigned long long>(result.verdictsEmitted),
                static_cast<unsigned long long>(result.met),
                static_cast<unsigned long long>(result.verdictsEmitted - result.met),
                verdictPath.c_str());
    std::printf("=========================================================================\n");
    std::fflush(stdout);
    return kExitOk;
}

int run(const Options& options) {
    std::string outDir;
    if (!validateOutDir(options.outDir, outDir)) {
        return kExitOutDirInvalid;
    }

    // BTB-REF-1: a malformed condition file is a named parse error and a non-zero exit
    // **before any subscription is made**. A run that quietly evaluates nothing and reports
    // nothing is indistinguishable from one where everything passed.
    std::vector<Condition> conditions;
    if (!options.conditionsPath.empty()) {
        std::string conditionError;
        if (!loadConditions(options.conditionsPath, conditions, conditionError)) {
            N8RO_LOG_ERROR(std::string("condition file rejected: ") + conditionError, kCategory);
            return kExitConditionsInvalid;
        }
        N8RO_LOG_INFO(std::string("loaded ") + std::to_string(conditions.size()) +
                          " conditions from " + options.conditionsPath +
                          "; verdicts go into the capture and into a verdicts-*.jsonl beside it",
                      kCategory);
    } else {
        N8RO_LOG_INFO(std::string("no --conditions given; recording only, no verdicts will be "
                                  "evaluated"),
                      kCategory);
    }

    // The registry is built from our own DbModel over the same model path and schema file
    // the engine was started with. SimulationEngineClient holds a registry of its own, but
    // exposes no accessor for it - only messageBus() - so the packed layer is ours to build.
    n8ro::schema::DbModel model(options.modelPath);
    if (!model.Open(options.schemaFile)) {
        N8RO_LOG_ERROR(std::string("DbModel::Open failed for schema file ") + options.schemaFile +
                           " under model path " + options.modelPath +
                           "; the packed schemas cannot be read and nothing could be decoded",
                       kCategory);
        return kExitModelOpenFailed;
    }

    n8ro::sim::MessageBusPackedSchemaRegistry registry;
    const Resolution resolution =
        resolveTopics(model, registry, options.entityStateMessage, options.engineStateMessage,
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
    RecordQueue queue(options.queueSize, kStructuralReserve, options.overflowPolicy);
    HandlerTiming stateTiming;
    HandlerTiming eventTiming;

    // The heartbeat counter. Incremented by the engine-state handler and read by the main
    // loop; its *rate of change* is the liveness signal, and nothing derived from it ever
    // reaches the capture.
    std::atomic<std::uint64_t> heartbeat{0};

    capture::HeaderInfo header;
    header.platform.engineConfig = options.config;
    header.platform.modelPath = options.modelPath;
    header.platform.schemaFile = options.schemaFile;
    header.platform.schemaVersion = model.getSchemaVersion();
    header.platform.runtimeVersion = std::string(n8ro::core::getN8roVersion());
    header.subscription.topic = resolution.entityState.topic;
    header.subscription.backpressurePolicy = backpressurePolicyName(kBusPolicy);
    header.subscription.queueSize = static_cast<std::uint64_t>(kBusQueueSize);
    // One entry, because one message type appears as a sample record's `message`. The other
    // three schemas are not here: no record in the file is a verbatim dump of one, so no
    // reader ever needs them to interpret a line.
    header.schemas.push_back(resolution.entityState);

    // An intermediate part's trailer is written by the writer thread, mid-run, when nothing
    // is there to hand it the platform's counters. This is where they come from instead: the
    // main loop already reads them once per status line, and stores the reading here.
    //
    // It is a cache rather than a direct read on purpose. RecordQueue::counters() is
    // mutex-guarded and would be safe to call from the writer thread, but nothing establishes
    // that for MessageBusPacked::metricsSnapshot() or IMessageBus::getStatistics(), and
    // reaching into the SDK off the main thread to fill in a counter would trade a real
    // invariant for a cosmetic one. The cost is that a rotated part's `bus_metrics` and
    // `drops` are as of the last status poll rather than the instant of the rotation - stated
    // in docs/capture-format-v1.md section 11 so a reader is not misled by it. The final
    // part's trailer is exact, as it has always been.
    std::mutex busSnapshotMutex;
    TrailerState busSnapshot;

    CaptureWriter writer(
        outDir, options.runLabel, header, resolution.entityState, options.captureMaxSamples,
        options.captureMaxBytes, options.onSizeLimit,
        // attached_mid_run, evaluated once when the file is opened. Derived from what the
        // message stream contained - did samples arrive for entities whose creation we never
        // saw - and never from a clock or a status tick (BTB-CAP-3).
        [&picture] {
            const PictureSnapshot snap = picture.snapshot();
            return snap.counters.samplesAccepted > 0
                       ? snap.counters.orphansBeforeFirstAccepted > 0
                       : snap.counters.samplesOrphaned > 0;
        },
        // Only used to name the file when the bridge attached mid-run and has therefore never
        // seen a scenario_loaded. A local read on the client; nothing here touches the bus.
        [&client] { return client->getLoadedScenarioName().value_or(std::string{}); },
        // Runs on the writer thread, at a rotation. Reads only the cache above.
        [&busSnapshotMutex, &busSnapshot] {
            const std::lock_guard<std::mutex> lock(busSnapshotMutex);
            return busSnapshot;
        });

    if (!conditions.empty()) {
        // Driven on the writer thread, from the same record stream in the same order, so a
        // verdict lands in the capture at the position where it was decided.
        writer.setReferee(std::make_unique<Referee>(conditions));
    }

    // --- subscriptions ------------------------------------------------------------------
    //
    // Both values explicit at every call site, with the default each overrides named
    // (BTB-BP-3). The bus default is KEEP_LATEST / 100.
    n8ro::core::SubscriptionOptions subscriptionOptions;
    subscriptionOptions.backpressurePolicy = kBusPolicy;    // overrides the default KEEP_LATEST
    subscriptionOptions.queueSize = kBusQueueSize;          // overrides the default 100

    N8RO_LOG_INFO(std::string("bus-side backpressure set explicitly: queueSize=") +
                      std::to_string(subscriptionOptions.queueSize) + " backpressurePolicy=" +
                      backpressurePolicyName(subscriptionOptions.backpressurePolicy) +
                      " (overriding the SubscriptionOptions defaults 100 / KEEP_LATEST). "
                      "KEEP_LATEST discards the older of two messages, which for a recorder is "
                      "the one already part of the run's history; BLOCK would stall the bus and "
                      "change the run being recorded (ADR-4). Provisional until M6 confirms "
                      "under overload (OQ-4)",
                  kCategory);
    N8RO_LOG_INFO(std::string("internal queue: ") + std::to_string(options.queueSize) +
                      " sample records + " + std::to_string(kStructuralReserve) +
                      " reserved for roster and segment records, policy " +
                      overflowPolicyName(options.overflowPolicy) +
                      ". Overflow is counted into trailer.drops (BTB-BP-4)",
                  kCategory);

    // BTB-EP-2: decoded subscriptions. Every handler below is a courier - it copies what it
    // needs, hands it to the queue, and returns. No IO, no formatting, no float conversion,
    // no file. All of that is the writer thread's (CLAUDE.md hard rule 2, BTB-BP-1).
    const std::uint64_t stateSubscription = packed.subscribeByTopic(
        resolution.entityState.topic,
        [&picture, &queue, &stateTiming](const n8ro::core::Message&,
                                         const n8ro::sim::MessageSchema&,
                                         const n8ro::sim::StreamValueMap& values) {
            const auto started = std::chrono::steady_clock::now();
            const SampleOutcome outcome = picture.onSample(values);
            if (outcome.accepted) {
                CaptureRecord record;
                record.kind = RecordKind::Sample;
                record.subject = outcome.scenarioEntityName;
                record.occupancy = outcome.generation;
                record.simTimeS = outcome.simulationTimeS;
                record.values = values;   // verbatim; the courier's whole job
                queue.offer(std::move(record));
            }
            stateTiming.note(std::chrono::steady_clock::now() - started);
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
        [&picture, &queue, &eventTiming](const n8ro::core::Message&,
                                         const n8ro::sim::MessageSchema&,
                                         const n8ro::sim::StreamValueMap& values) {
            const auto started = std::chrono::steady_clock::now();
            const EventOutcome outcome = picture.onEntityEvent(values);
            if (outcome.kind != EventOutcome::Kind::Ignored) {
                CaptureRecord record;
                record.kind = outcome.kind == EventOutcome::Kind::Added ? RecordKind::EntityAdd
                                                                        : RecordKind::EntityRemove;
                record.subject = outcome.scenarioEntityName;
                record.occupancy = outcome.generation;
                record.simTimeS = outcome.simulationTimeS;
                record.reason = outcome.reason;
                queue.offer(std::move(record));
            }
            eventTiming.note(std::chrono::steady_clock::now() - started);
        },
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

    // BTB-CX-4. The event names come from EventNames.h, which is compile-checked against the
    // engine's own publish sites; the topic came from the registry. Neither is a literal.
    const std::uint64_t scenarioSubscription = packed.subscribeByTopic(
        resolution.scenarioEvent.topic,
        [&queue](const n8ro::core::Message&, const n8ro::sim::MessageSchema&,
                 const n8ro::sim::StreamValueMap& values) {
            const std::optional<std::string> eventName = tryReadString(values, "eventName");
            if (!eventName) {
                return;
            }
            CaptureRecord record;
            if (*eventName == n8ro::sim::kEventScenarioLoaded) {
                record.kind = RecordKind::ScenarioLoaded;
            } else if (*eventName == n8ro::sim::kEventScenarioUnloaded) {
                record.kind = RecordKind::ScenarioUnloaded;
            } else {
                // Something else on the scenario-event topic. Not ours to interpret, and not
                // a boundary; the writer never sees it.
                return;
            }
            record.subject = tryReadString(values, "scenarioName").value_or(std::string{});
            record.simTimeS = tryReadDouble(values, "simulationTime").value_or(0.0);
            queue.offer(std::move(record));
        },
        subscriptionOptions);
    if (scenarioSubscription == 0) {
        N8RO_LOG_ERROR(std::string("subscribeByTopic returned no subscription for scenario-event "
                                   "topic ") +
                           resolution.scenarioEvent.topic,
                       kCategory);
        static_cast<void>(packed.unsubscribe(stateSubscription));
        static_cast<void>(packed.unsubscribe(eventSubscription));
        return kExitSubscribeFailed;
    }

    // BTB-CX-3. Arrival is the whole signal; nothing reads a value out of this message, and
    // nothing derived from it reaches the capture. sim/engine/state carries wallElapsedS,
    // which is exactly the field tenet 2 forbids anywhere durable - not copying the message
    // wholesale is what keeps that impossible rather than merely avoided.
    const std::uint64_t heartbeatSubscription = packed.subscribeByTopic(
        resolution.engineState.topic,
        [&heartbeat](const n8ro::core::Message&, const n8ro::sim::MessageSchema&,
                     const n8ro::sim::StreamValueMap&) {
            heartbeat.fetch_add(1, std::memory_order_relaxed);
        },
        subscriptionOptions);
    if (heartbeatSubscription == 0) {
        N8RO_LOG_ERROR(std::string("subscribeByTopic returned no subscription for engine-state "
                                   "topic ") +
                           resolution.engineState.topic,
                       kCategory);
        static_cast<void>(packed.unsubscribe(stateSubscription));
        static_cast<void>(packed.unsubscribe(eventSubscription));
        static_cast<void>(packed.unsubscribe(scenarioSubscription));
        return kExitSubscribeFailed;
    }

    N8RO_LOG_INFO(std::string("subscribed decoded: entity-state ") + resolution.entityState.topic +
                      ", entity-event " + resolution.entityEvent.topic + ", scenario-event " +
                      resolution.scenarioEvent.topic + ", engine-state " +
                      resolution.engineState.topic,
                  kCategory);

    // Subscribe first, then pump - nothing published between the two is missed.
    client->startMessagePump();

    // The writer thread. From here until it is joined, it is the only thing that touches the
    // file, and the queue is the only thing the two threads share.
    std::thread writerThread([&writer, &queue] { writer.run(queue); });

    if (!installInterruptHandlers()) {
        // Not fatal: the bridge still records correctly, it just has to be ended by the host
        // going away or by the record budget. Worth saying out loud rather than discovering.
        N8RO_LOG_WARNING(std::string("could not install interrupt handlers; Ctrl-C will not "
                                     "produce a clean shutdown and the capture would be "
                                     "truncated. The run will still end cleanly on host loss"),
                         kCategory);
    }

    N8RO_LOG_INFO(std::string("message pump and writer thread started. Waiting for the "
                              "simulation host; no start order is required (BTB-CX-2). Host loss "
                              "is declared after ") +
                      std::to_string(kHostLossWindowS) +
                      " s without an engine-state message (BTB-CX-3)",
                  kCategory);

    // --- the main loop ------------------------------------------------------------------
    EndReason endReason = EndReason::HostLost;
    std::uint64_t lastHeartbeat = 0;
    auto lastHeartbeatAt = std::chrono::steady_clock::now();
    auto lastWaitingLogAt = lastHeartbeatAt;
    bool everAttached = false;
    int pollsSinceStatus = kPollsPerStatusLine;

    for (;;) {
        // Roster transitions first, so a removal is logged above the line whose count it
        // explains rather than below it.
        logRosterEvents(picture.drainEvents());

        const auto now = std::chrono::steady_clock::now();
        const std::uint64_t beats = heartbeat.load(std::memory_order_relaxed);
        if (beats != lastHeartbeat) {
            lastHeartbeat = beats;
            lastHeartbeatAt = now;
            if (!everAttached) {
                everAttached = true;
                N8RO_LOG_INFO(std::string("attached: the simulation host is publishing "
                                          "engine state"),
                              kCategory);
            }
        }

        if (everAttached) {
            const double silentS = std::chrono::duration<double>(now - lastHeartbeatAt).count();
            if (silentS > kHostLossWindowS) {
                N8RO_LOG_WARNING(std::string("host lost: no engine-state message for ") +
                                     std::to_string(silentS) + " s (window " +
                                     std::to_string(kHostLossWindowS) +
                                     " s). Closing the capture with end_reason=host_lost",
                                 kCategory);
                endReason = EndReason::HostLost;
                break;
            }
        } else if (now - lastWaitingLogAt >= kWaitingLogInterval) {
            // BTB-CX-2's bounded, logged wait. Nothing to retry - the client is constructed
            // and subscribed, and the bus delivers as soon as a host publishes.
            lastWaitingLogAt = now;
            N8RO_LOG_INFO(std::string("waiting for a simulation host on ") +
                              resolution.engineState.topic +
                              "; the bridge is subscribed and will begin capturing the moment "
                              "one publishes",
                          kCategory);
        }

        // BTB-SD-1. The handler set a counter and returned; everything the interrupt means
        // happens here, on a thread that is allowed to allocate, lock and write.
        if (interruptCount() > 0) {
            N8RO_LOG_INFO(std::string("interrupt received; unsubscribing, stopping the pump, "
                                      "draining the queue and closing the capture with "
                                      "end_reason=shutdown. Every record enqueued before the "
                                      "signal will be in the file (BTB-SD-1). Interrupt again "
                                      "to force exit"),
                          kCategory);
            endReason = EndReason::Shutdown;
            break;
        }

        if (writer.budgetReached()) {
            N8RO_LOG_INFO(std::string("record budget reached at ") +
                              std::to_string(writer.samplesWritten()) +
                              " samples; closing the capture with end_reason=size_limit",
                          kCategory);
            endReason = EndReason::SizeLimit;
            break;
        }

        if (writer.failed()) {
            N8RO_LOG_ERROR(std::string("the writer reported a failure; stopping"), kCategory);
            endReason = EndReason::Shutdown;
            break;
        }

        if (++pollsSinceStatus >= kPollsPerStatusLine) {
            pollsSinceStatus = 0;
            const PictureSnapshot snap = picture.snapshot();
            const n8ro::sim::MessageBusPackedMetricsSnapshot metrics = packed.metricsSnapshot();
            const n8ro::core::IMessageBus::Statistics busStats = bus->getStatistics();
            const QueueCounters qc = queue.counters();

            // Feed the rotation-trailer cache from the reading just taken. Free - these four
            // objects were fetched for the status line regardless.
            {
                TrailerState fresh;
                fresh.drops.samplesNotRecorded = qc.samplesDropped;
                fresh.drops.eventsNotRecorded = qc.structuralDropped;
                fresh.drops.samplesOrphaned = snap.counters.samplesOrphaned;
                fresh.drops.samplesUnnamed = snap.counters.samplesUnnamed;
                fresh.drops.samplesUntimed = snap.counters.samplesUntimed;
                fresh.busMetrics.schemaHashDrops = metrics.schemaHashDrops;
                fresh.busMetrics.messageIdDrops = metrics.messageIdDrops;
                fresh.busMetrics.decodeFailures = metrics.decodeFailures;
                fresh.busMetrics.missingSchemaPassthrough = metrics.missingSchemaPassthrough;
                fresh.busMetrics.legacyPayloadPassthrough = metrics.legacyPayloadPassthrough;
                fresh.busMetrics.messagesDropped = busStats.messagesDropped;
                fresh.busMetrics.droppedByBackpressure = busStats.droppedByBackpressure;
                fresh.busMetrics.droppedByQueueOverflow = busStats.droppedByQueueOverflow;
                fresh.busMetrics.droppedByRateLimiting = busStats.droppedByRateLimiting;
                const std::lock_guard<std::mutex> lock(busSnapshotMutex);
                busSnapshot = fresh;
            }

            const std::string engineState = client->getEngineState();
            const std::optional<std::string> scenario = client->getLoadedScenarioName();

            std::printf(
                "engine=%-12s frame=%-10llu simTime=%10.3f scenario=%-24s live=%-4zu names=%-4zu "
                "samples=%llu orphaned=%llu\n",
                engineState.empty() ? "(none yet)" : engineState.c_str(),
                static_cast<unsigned long long>(client->getFrameNumber()),
                client->getSimulationTimeS(),
                scenario && !scenario->empty() ? scenario->c_str() : "(none)", snap.liveCount,
                snap.roster.size(), static_cast<unsigned long long>(snap.counters.samplesAccepted),
                static_cast<unsigned long long>(snap.counters.samplesOrphaned));
            std::printf("    capture=%llu records (seg=%llu samples=%llu add=%llu rm=%llu) "
                        "queue=%zu/%zu drops=%llu/%llu\n",
                        static_cast<unsigned long long>(writer.recordsWritten()),
                        static_cast<unsigned long long>(writer.counts().segments),
                        static_cast<unsigned long long>(writer.samplesWritten()),
                        static_cast<unsigned long long>(writer.counts().entitiesAdded),
                        static_cast<unsigned long long>(writer.counts().entitiesRemoved),
                        queue.depth(), queue.hardCapacity(),
                        static_cast<unsigned long long>(qc.samplesDropped),
                        static_cast<unsigned long long>(qc.structuralDropped));
            std::printf("    busLoss=%llu(bp=%llu qo=%llu rl=%llu) decode=%llu(hash=%llu "
                        "fail=%llu noschema=%llu)\n",
                        static_cast<unsigned long long>(busStats.messagesDropped),
                        static_cast<unsigned long long>(busStats.droppedByBackpressure),
                        static_cast<unsigned long long>(busStats.droppedByQueueOverflow),
                        static_cast<unsigned long long>(busStats.droppedByRateLimiting),
                        static_cast<unsigned long long>(metrics.schemaHashDrops +
                                                        metrics.decodeFailures +
                                                        metrics.missingSchemaPassthrough),
                        static_cast<unsigned long long>(metrics.schemaHashDrops),
                        static_cast<unsigned long long>(metrics.decodeFailures),
                        static_cast<unsigned long long>(metrics.missingSchemaPassthrough));
            std::fflush(stdout);
        }

        std::this_thread::sleep_for(kPollInterval);
    }

    // --- teardown -----------------------------------------------------------------------
    //
    // Unsubscribe, stop the pump, close the queue, join the writer, then write the trailer.
    // In that order: nothing may be enqueued after close(), and nothing may be written after
    // the writer thread has returned.
    // From here to the trailer is the drain. A second interrupt inside this window forces
    // exit with a logged warning rather than letting an operator watch a hang (BTB-SD-1); the
    // watchdog exists because a signal handler cannot write that warning itself.
    startDrainWatchdog(kExitDrainForced);

    static_cast<void>(packed.unsubscribe(stateSubscription));
    static_cast<void>(packed.unsubscribe(eventSubscription));
    static_cast<void>(packed.unsubscribe(scenarioSubscription));
    static_cast<void>(packed.unsubscribe(heartbeatSubscription));
    client->stopMessagePump();
    queue.close();
    writerThread.join();

    // Taken once, after the pump has stopped, so the trailer describes one instant and
    // nothing can change under it.
    const PictureSnapshot finalSnapshot = picture.snapshot();
    const n8ro::sim::MessageBusPackedMetricsSnapshot finalMetrics = packed.metricsSnapshot();
    const n8ro::core::IMessageBus::Statistics finalBusStats = bus->getStatistics();
    const QueueCounters finalQueue = queue.counters();

    capture::TrailerDrops drops;
    // The queue's genuine overflow, at last. M4 left this structurally 0 because the buffer
    // filling *was* the end of recording; from M5 it is what BTB-BP-4 asked for.
    drops.samplesNotRecorded = finalQueue.samplesDropped;
    drops.eventsNotRecorded = finalQueue.structuralDropped + writer.counts().stagedDropped;
    drops.samplesOrphaned = finalSnapshot.counters.samplesOrphaned;
    drops.samplesUnnamed = finalSnapshot.counters.samplesUnnamed;
    drops.samplesUntimed = finalSnapshot.counters.samplesUntimed;

    capture::TrailerBusMetrics busMetrics;
    busMetrics.schemaHashDrops = finalMetrics.schemaHashDrops;
    busMetrics.messageIdDrops = finalMetrics.messageIdDrops;
    busMetrics.decodeFailures = finalMetrics.decodeFailures;
    busMetrics.missingSchemaPassthrough = finalMetrics.missingSchemaPassthrough;
    busMetrics.legacyPayloadPassthrough = finalMetrics.legacyPayloadPassthrough;
    busMetrics.messagesDropped = finalBusStats.messagesDropped;
    busMetrics.droppedByBackpressure = finalBusStats.droppedByBackpressure;
    busMetrics.droppedByQueueOverflow = finalBusStats.droppedByQueueOverflow;
    busMetrics.droppedByRateLimiting = finalBusStats.droppedByRateLimiting;

    const bool written = writer.finish(endReason, drops, busMetrics);

    logRosterEvents(picture.drainEvents());
    printRunSummary(writer, finalSnapshot, finalQueue, queue, stateTiming, eventTiming,
                    finalMetrics, finalBusStats, endReason);

    if (!written) {
        return kExitCaptureWriteFailed;
    }
    // Host loss is an expected, handled state, not an error: the capture is complete and
    // closed, and BTB-CX-3 asks for a clean exit rather than a diagnostic one.
    return kExitOk;
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

        const int code = options.isReplay() ? runReplay(options) : run(options);
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
