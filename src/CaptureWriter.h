// EXT-08 Bus Telemetry Bridge - M5: the writer thread and the capture session's state
// machine (BTB-CX-4, BTB-EP-3, BTB-BP-1, BTB-CAP-2).
//
// Everything expensive happens here and only here: float conversion, JSON encoding, and all
// file IO. The bus handlers copy into a RecordQueue and return (CLAUDE.md hard rule 2).
//
// This replaces M4's buffer-then-dump entirely. What survives unchanged is CaptureFormat's
// serialiser, which is pure functions over their arguments and never knew where its records
// came from.
//
// ---------------------------------------------------------------------------------------
// The segment state machine
//
// The PRD's state model reads
//
//     waiting -> attached -> segment_open <=> segment_closed -> closing -> closed
//
// and M5 implements all of it. The one thing the model did not anticipate is what M5
// measured on the live bus (docs/decisions-m5-m7.md, D-1): **the entity_created burst that
// materialises a scenario arrives before the scenario_loaded that announces it**, at
// bring-up and at every reload.
//
//     seq 6   scenario_unloaded  ""                <- bring-up noise, empty scenario name
//     seq 7..48   entity_created x 42              <- the burst, BEFORE the load
//     seq 49  scenario_loaded    "Atacama Air Defense"
//
// So a `scenario_loaded` is a completion announcement, not a start. Closing on the unload
// and opening on the load, with nothing in between, would leave 42 entity_add records
// outside any segment - which docs/capture-format-v1.md section 7 calls malformed.
//
// The staging area is the answer. Roster records that arrive while no segment is open wait
// there and flush into the segment that opens next, so each creation burst lands in the
// segment it belongs to. A `sample` never waits: if one arrives while no segment is open the
// segment opens immediately (the state model's attach-mid-run branch), which is also what
// bounds the staging area in practice - it can only grow with a creation burst, which is
// entity-count sized.
//
// ---------------------------------------------------------------------------------------
// Threading
//
// run() is the writer thread's whole body. open-on-demand, finish() and every getter that
// is not atomic are called from our own thread *after* the writer thread has been joined.
// The queue is the only thing the two threads share.

#pragma once

#include "CaptureFormat.h"
#include "CaptureRecord.h"
#include "RecordQueue.h"
#include "Referee.h"

#include <messaging/packed/MessageSchema.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace n8ro::bridge {

// Why a capture stopped. These map one-to-one onto the format's closed `end_reason` set.
enum class EndReason {
    Shutdown,     // the operator stopped the producer (M7 - signal handling)
    HostLost,     // the heartbeat went silent (BTB-CX-3)
    SizeLimit,    // the record budget was reached
};

[[nodiscard]] const char* endReasonName(EndReason reason);

struct WriterCounts {
    std::uint64_t segments = 0;
    std::uint64_t samples = 0;
    std::uint64_t entitiesAdded = 0;
    std::uint64_t entitiesRemoved = 0;
    std::uint64_t verdicts = 0;
    std::uint64_t stagedDropped = 0;     // staging area overflow - see kStagingCapacity
    std::uint64_t unloadNoiseIgnored = 0; // scenario_unloaded with an empty scenario name
};

class CaptureWriter {
public:
    // The staging area's bound. It only ever holds one creation burst - 42 records on the
    // reference scenario, 126 on the overload one - because a sample forces the segment open
    // and flushes it. Three orders of magnitude of headroom, and overflow is counted.
    static constexpr std::size_t kStagingCapacity = 8192;

    // `attachedMidRun` is evaluated once, when the file is opened, and answers the causal
    // question M4 settled: did samples arrive for entities whose creation we never saw?
    // Never a clock and never a status tick (BTB-CAP-3).
    CaptureWriter(std::string outDir, std::string runLabel, capture::HeaderInfo header,
                  n8ro::sim::MessageSchema stateSchema, std::size_t maxSamples,
                  std::function<bool()> attachedMidRun,
                  std::function<std::string()> lastKnownScenario);

    // Optional. When present, the referee is driven from the same record stream the writer is
    // already draining, in the same order, so a verdict lands in the capture at the position
    // and in the segment where it was decided. Set before run(); owned from here on.
    //
    // Verdicts go to two places: into the capture as `verdict` records, and into a separate
    // `verdicts-<scenario>-<run-label>.jsonl` beside it. The first is what the format
    // specifies; the second is what replay can also produce, which is how "live verdicts
    // equal replay verdicts" becomes a file comparison (BTB-REF-4).
    void setReferee(std::unique_ptr<Referee> referee);

    [[nodiscard]] const std::string& verdictPath() const { return verdictPath_; }

    // The writer thread's body. Drains until the queue is closed and empty, then returns.
    void run(RecordQueue& queue);

    // Called from our own thread after the writer thread is joined. Closes any open segment,
    // writes the trailer, flushes and closes the file. If nothing ever opened the file, it is
    // opened here so that a run which recorded nothing still leaves a valid, complete, empty
    // capture - header then trailer, which the format explicitly permits (section 7).
    [[nodiscard]] bool finish(EndReason reason, const capture::TrailerDrops& drops,
                              const capture::TrailerBusMetrics& busMetrics);

    // Set once the sample budget is reached, so the main loop can tear down promptly rather
    // than a status tick later. Read from our own thread.
    [[nodiscard]] bool budgetReached() const { return budgetReached_.load(); }

    // Live counters for the status line. Approximate by design - they are read without a
    // lock while the writer runs, and are exact once it is joined.
    [[nodiscard]] std::uint64_t samplesWritten() const { return samplesWritten_.load(); }
    [[nodiscard]] std::uint64_t recordsWritten() const { return recordsWritten_.load(); }
    [[nodiscard]] std::uint64_t samplesAfterBudget() const { return samplesAfterBudget_.load(); }

    // Valid after finish(). Empty until the file is opened.
    [[nodiscard]] const std::string& path() const { return path_; }
    [[nodiscard]] const WriterCounts& counts() const { return counts_; }
    [[nodiscard]] bool failed() const { return failed_; }
    [[nodiscard]] std::vector<std::string> neverPublishedFields() const;
    [[nodiscard]] double lastRecordSimTimeS() const { return lastRecordSimTimeS_; }

    // The simulation-time span of the samples in one segment, min to max.
    //
    // Reported per segment rather than as a single file-wide first-to-last pair. A complete
    // live run always ends with a teardown reload whose clock has been reset to 0 (spec 5.1),
    // so the last sample written is 0.0 on every such run and a "first written -> last
    // written" line reads 0.0 -> 0.0 however long the run was - hiding the entire run behind
    // its own teardown. min/max per segment says what actually happened and makes the reset
    // visible as its own segment.
    struct SegmentSpan {
        std::uint64_t ordinal = 0;
        std::uint64_t samples = 0;
        double minSimTimeS = 0.0;
        double maxSimTimeS = 0.0;
    };
    [[nodiscard]] const std::vector<SegmentSpan>& segmentSpans() const { return segmentSpans_; }

    // Turns a scenario name into the filename component: lowercase, runs of anything outside
    // [a-z0-9] collapsed to one hyphen, ends trimmed. Total, deterministic, and derived from
    // the scenario name alone (D-12). Exposed for the tests.
    [[nodiscard]] static std::string scenarioSlug(const std::string& scenario);

    // Picks `<run-label>` when the caller supplied none: the lowest zero-padded ordinal not
    // already present in `outDir` for this scenario slug. Never a timestamp (ADR-3).
    [[nodiscard]] static std::string nextRunLabel(const std::string& outDir,
                                                  const std::string& slug);

private:
    void apply(const CaptureRecord& record);
    bool ensureOpen(const std::string& scenarioForName);
    void openSegment(const std::string& scenario, double simTimeS);
    void closeSegment(double simTimeS, const char* reason);
    void flushStaging();
    void emit(const std::string& line);
    void drainVerdicts();

    std::string outDir_;
    std::string runLabel_;
    capture::HeaderInfo header_;
    n8ro::sim::MessageSchema stateSchema_;
    std::size_t maxSamples_ = 0;
    std::function<bool()> attachedMidRun_;
    std::function<std::string()> lastKnownScenario_;

    std::ofstream file_;
    std::string path_;
    bool opened_ = false;
    bool failed_ = false;

    // Segment state.
    bool segmentOpen_ = false;
    std::uint64_t segmentOrdinal_ = 0;
    bool anySegmentOpened_ = false;
    std::string currentScenario_;
    std::string lastScenarioSeen_;

    // Roster records waiting for a segment to belong to.
    std::deque<CaptureRecord> staging_;

    std::unique_ptr<Referee> referee_;
    std::ofstream verdictFile_;
    std::string verdictPath_;

    capture::FieldPresence presence_;
    WriterCounts counts_;
    std::vector<SegmentSpan> segmentSpans_;
    double lastRecordSimTimeS_ = 0.0;

    // The last record that carried data rather than a boundary. End-of-run verdicts are
    // stamped from these so that live and replay agree exactly: replay reads a file whose
    // boundary records it also sees, and anchoring on the last *data* record is the one
    // point both paths can reach identically (BTB-REF-4).
    double lastDataSimTimeS_ = 0.0;
    std::uint64_t lastDataSegment_ = 0;

    std::atomic<bool> budgetReached_{false};
    std::atomic<std::uint64_t> samplesWritten_{0};
    std::atomic<std::uint64_t> recordsWritten_{0};
    std::atomic<std::uint64_t> samplesAfterBudget_{0};
};

}  // namespace n8ro::bridge
